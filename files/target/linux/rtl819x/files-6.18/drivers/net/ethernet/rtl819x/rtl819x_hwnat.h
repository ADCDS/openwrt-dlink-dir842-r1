/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RTL8197F conntrack-driven hardware NAT offload (M6.6 Phase 3).
 *
 * Bridges the kernel's downstream flow-offload interface (Felix Fietkau's 2018
 * ndo_flow_offload / xt_FLOWOFFLOAD --hw design) to the RTL8197F ASIC NAPT table:
 * when Linux software-NATs the first packets of a LAN->WAN flow and offers it for
 * hardware offload, we install the matching pair of ASIC NAPT rows (via the
 * rtl865x_napt_* HAL helpers) so the ASIC carries the rest of the flow in silicon
 * with the CPU fully bypassed. A miss on a process=5 route traps to the CPU
 * (SWTCR0 NAPTR_NOT_FOUND_DROP=0), so new/declined/ICMP flows ride the existing
 * software fastpath unharmed — this is a pure accelerator, never a hard dependency.
 *
 * The two ndo hooks are always installed on eth0's netdev_ops. The ADD path gates on
 * the `hwnat` module param: while it is off every offered flow is declined to the
 * software fastpath, so no rows are ever installed and forwarding behaves like a
 * driver without the hooks. The check hook is deliberately NOT gated — the framework
 * re-runs it when preparing a DEL, so gating it would drop DELs and strand live ASIC
 * rows (see the check-hook comment in rtl819x_hwnat.c). Toggling 1->0 at runtime
 * therefore drains cleanly: existing offloaded flows are DEL'd by the flowtable GC
 * within ~30 s and fall back to software. Runtime-writable, no reboot needed.
 */
#ifndef _RTL819X_HWNAT_H
#define _RTL819X_HWNAT_H

#include <linux/netdevice.h>

/* Runtime gate: module param rtl819x.hwnat, perm 0644, default 0. Read (READ_ONCE)
 * by the offload hooks on every flow; writable via
 * /sys/module/rtl819x/parameters/hwnat. */
#ifdef CONFIG_RTL819X_HWNAT
extern bool rtl819x_hwnat_enabled;
#else
#define rtl819x_hwnat_enabled false
#endif

/*
 * Flow offload entry point.
 *
 * The 4.14 port hooked ndo_flow_offload, a downstream interface that no longer
 * exists. Its replacement is ndo_setup_tc(TC_SETUP_FT), which hands the driver a
 * flow block; nf_flow_table then offers each NAT flow through that block as a
 * tc-flower rule. It is also what DSA user ports forward to their conduit, so
 * one callback on eth0 serves lan1..lan4 and wan together.
 */
#ifdef CONFIG_RTL819X_HWNAT
int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data);
#else
static inline int rtl819x_hwnat_setup_tc(struct net_device *dev,
					 enum tc_setup_type type,
					 void *type_data)
{
	return -EOPNOTSUPP;
}
#endif

/* Lifecycle, driven from rtl819x_eth_open()/rtl819x_eth_stop(). start() arms the
 * offload (aging worker) once the datapath is up; stop() tears down every installed
 * flow and quiesces the worker. Both are no-ops when hwnat is disabled. */
#ifdef CONFIG_RTL819X_HWNAT
void rtl819x_hwnat_start(struct net_device *dev);
#else
static inline void rtl819x_hwnat_start(struct net_device *dev) { }
#endif
#ifdef CONFIG_RTL819X_HWNAT
void rtl819x_hwnat_stop(void);
#else
static inline void rtl819x_hwnat_stop(void) { }
#endif

/*
 * ★ 2026-09-04: added for the LARGE-FRAME WEDGE detector (rtl819x-eth.c). That
 * detector's whole premise is "large frames vanished from the CPU RX path while
 * small frames keep arriving = wedged". Once hardware NAT offload actually
 * accelerates a flow, that is exactly what HEALTHY looks like too: the ASIC
 * stops handing large bulk frames to the CPU at all, on purpose. Bench-confirmed
 * false positive: a flow whose NAPT row's agingTime sat pinned at the reload
 * ceiling (i.e. genuinely hardware-hit) for 5+ continuous seconds still tripped
 * the detector, which then fired a level-3 recovery and wiped the very table
 * that was working. This lets the wedge check consult the SAME hotness state
 * hwnat_aging_work_fn() already computes every 5s (the AGE poll) before
 * declaring a wedge, so real hardware-forwarded traffic is not mistaken for a
 * dead datapath. True positive detection (nothing hot, large frames vanished
 * anyway) is unaffected.
 */
#ifdef CONFIG_RTL819X_HWNAT
bool rtl819x_hwnat_has_hot_flow(void);
/* True whenever >=1 flow currently has ASIC NAPT rows installed (set from packet 1
 * of install, unlike has_hot_flow() which needs a 5s aging poll to observe a HIT).
 * The LARGE-FRAME WEDGE detector uses it to avoid destroying a genuinely-offloaded
 * flow that has stalled before its row was ever observed hot. */
bool rtl819x_hwnat_any_flow_installed(void);
#else
static inline bool rtl819x_hwnat_has_hot_flow(void) { return false; }
static inline bool rtl819x_hwnat_any_flow_installed(void) { return false; }
#endif

#endif /* _RTL819X_HWNAT_H */
