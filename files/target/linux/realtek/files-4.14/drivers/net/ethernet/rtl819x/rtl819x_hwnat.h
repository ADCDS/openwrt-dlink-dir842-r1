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
extern bool rtl819x_hwnat_enabled;

/* The two ndo_flow_offload hooks, referenced by the hwnat net_device_ops in
 * rtl819x-eth.c. Signatures must match struct net_device_ops exactly. */
int rtl819x_hwnat_flow_offload_check(struct flow_offload_hw_path *path);
int rtl819x_hwnat_flow_offload(enum flow_offload_type type,
			       struct flow_offload *flow,
			       struct flow_offload_hw_path *src,
			       struct flow_offload_hw_path *dest);

/* Lifecycle, driven from rtl819x_eth_open()/rtl819x_eth_stop(). start() arms the
 * offload (aging worker) once the datapath is up; stop() tears down every installed
 * flow and quiesces the worker. Both are no-ops when hwnat is disabled. */
void rtl819x_hwnat_start(struct net_device *dev);
void rtl819x_hwnat_stop(void);

#endif /* _RTL819X_HWNAT_H */
