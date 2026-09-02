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
 * Flow offload entry point. The 4.14 port hooked ndo_flow_offload, a downstream
 * interface that no longer exists; the replacement is ndo_setup_tc(TC_SETUP_FT)
 * feeding a flow_block callback, which is also what DSA user ports forward to
 * their conduit. Until that lands rtl819x_hwnat.c is out of the build and these
 * two calls are no-ops, so the datapath runs entirely in software.
 */

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
 * M7.3 step 4 — software-flow timeout refresh, built-in <-> module handshake.
 *
 * The driver is CONFIG_NET_RTL819X=y but nf_flow_table / xt_FLOWOFFLOAD are =m, so it can
 * neither link the flow table's EXPORT_SYMBOL_GPL'd flow_offload_lookup() nor reach
 * xt_FLOWOFFLOAD's file-static struct nf_flowtable. The linkage is INVERTED: the driver
 * EXPORTs the two functions below and xt_FLOWOFFLOAD's module init/exit hands over
 * {&nf_flowtable, flow_offload_lookup} (see hack-4.14/650-netfilter-add-xt_OFFLOAD-target.patch).
 * Guarded by rtl865x_hal_lock; the aging worker uses them (under RCU) to keep an actively
 * ASIC-forwarded flow's software timeout fresh so it isn't GC'd + re-learned every ~30 s.
 * Structs only forward-declared here (real defs come from <net/netfilter/nf_flow_table.h>,
 * which rtl819x_hwnat.c includes BEFORE this header, so the fwd-decls are harmless no-ops). */
struct nf_flowtable;
struct flow_offload_tuple;
struct flow_offload_tuple_rhash;
typedef struct flow_offload_tuple_rhash *(*rtl819x_flow_lookup_fn)(
		struct nf_flowtable *ft, struct flow_offload_tuple *tuple);
void rtl819x_hwnat_flowtable_register(struct nf_flowtable *ft,
				      rtl819x_flow_lookup_fn lookup);
void rtl819x_hwnat_flowtable_unregister(void);

#endif /* _RTL819X_HWNAT_H */
