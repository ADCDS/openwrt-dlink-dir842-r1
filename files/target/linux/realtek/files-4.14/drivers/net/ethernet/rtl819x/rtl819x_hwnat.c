// SPDX-License-Identifier: GPL-2.0
/*
 * RTL8197F conntrack-driven hardware NAT offload (M6.6 Phase 3).
 *
 * See rtl819x_hwnat.h for the high-level design. In brief: the kernel software-NATs
 * the first packets of a LAN->WAN masquerade flow (the ASIC traps NAPT misses to the
 * CPU) and then offers the established flow for hardware offload through the
 * downstream ndo_flow_offload interface. We translate that offer into a pair of ASIC
 * NAPT rows so the rest of the flow is forwarded and source-NAT'd entirely in
 * silicon, CPU bypassed. Declined flows (anything not IPv4 TCP/UDP LAN->WAN
 * masquerade-to-our-one-WAN-IP, or a hash collision) simply stay on the software
 * fastpath — hardware offload here is a pure accelerator, never load-bearing.
 *
 * Concurrency: ADD and DEL arrive serialized in process context on eth0's ops (the
 * nf_flow_table_hw workqueue holds nf_flow_offload_hw_mutex across each call) and are
 * sleepable. ndo_flow_offload_check runs under rcu_read_lock_bh (atomic). All ASIC
 * table mutation and the shadow-bitmap/flow-list state are serialized by the shared
 * rtl865x_hal_lock (also held by gw_prog and the /proc scanners in rtl865x_asichal.c).
 *
 * Flow lifetime (the sharp edge): nf_flow_table_core.c's flow_offload_del() queues our
 * async DEL and then IMMEDIATELY kfree_rcu()s the flow. So by the time our DEL runs the
 * flow may already be freed. We therefore treat the flow pointer as an OPAQUE IDENTITY
 * token (compared by value in DEL, never dereferenced) once ADD returns. The aging
 * worker never dereferences it either.
 *
 * KNOWN LIMITATIONS (documented per the Phase-3 independent review):
 *  1. Inherited framework ADD-vs-free race: the queued ADD work dereferences `flow`,
 *     but a flow can expire (flow_offload_add() starts it with an already-expired
 *     timeout; only forwarded packets refresh it) and be freed by the 1 Hz GC before
 *     the work runs — and since FLOW_OFFLOAD_HW isn't set yet, no DEL is ever queued.
 *     The window is narrow (needs the workqueue to lag a full GC period) and the
 *     defect is in nf_flow_table_hw.c's design (it equally derefs the flow after the
 *     driver returns); it is not fixable driver-side without copying the tuples into
 *     the queued work at prepare time. Consequence ranges from reading freed memory
 *     to installing rows for a dead flow (which the aging reaper later collects).
 *  2. The inbound ASIC row is FULL-CONE: the hardware entry stores only
 *     {proto, extIP, globalPort G -> intIP, intPort} — no remote endpoint (the remote
 *     is encoded only in the OUTBOUND row's hash index). Linux masquerade preserves
 *     source ports and guarantees only 5-tuple uniqueness, so two LAN hosts using the
 *     same source port to different remotes share one G: the first flow offloads, the
 *     second correctly declines (bitmap) and stays in software — but its INBOUND
 *     packets (same extIP:G, different remote) match the first flow's row and are
 *     rewritten to the wrong host. Same mechanism can hijack replies to the router's
 *     own connections if a client's preserved port equals a router ephemeral port.
 *     Low frequency (ephemeral-port collision), but silent; the stock firmware
 *     avoided it by owning the whole port namespace (it allocated every G). A real
 *     fix needs port-range partitioning between kernel NAT and the offload.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/workqueue.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/netfilter.h>		/* nf_hookfn / nf_hook_state (needed by the header below) */
#include <net/netfilter/nf_flow_table.h>
#include <net/netfilter/nf_conntrack.h>	/* struct nf_conn (silences fwd-decl warnings) */

#include "rtl865x_asichal.h"
#include "rtl819x_hwnat.h"

bool rtl819x_hwnat_enabled;
module_param_named(hwnat, rtl819x_hwnat_enabled, bool, 0644);
MODULE_PARM_DESC(hwnat,
	"RTL8197F ASIC hardware NAT offload: 0=off (software fastpath only, default), 1=on. "
	"Runtime-writable via /sys/module/rtl819x/parameters/hwnat: the offload hooks are "
	"always installed on eth0 but decline every flow to the software path while off, so "
	"toggling it needs no reboot and off is behaviourally identical to no hooks at all.");

/* Poll cadence for the aging worker: must be well under the 30 s software GC timeout
 * (NF_FLOW_TIMEOUT) so an actively HW-forwarded flow can be kept alive between polls. */
#define HWNAT_AGING_INTERVAL	(5 * HZ)

/*
 * One installed hardware flow. `flow` is stored ONLY to match the eventual DEL by
 * pointer value — it is never dereferenced after ADD returns (the flow may be freed
 * behind our back, see the file header). `orig_tuple` is a by-value copy of the
 * ORIGINAL-direction conntrack tuple taken while the flow is provably alive (during
 * ADD); Phase-3 step 4 uses it for a UAF-safe flow_offload_lookup()-based timeout
 * refresh. idx_out/idx_in are the two ASIC NAPT row indices we own for this flow.
 */
struct hwnat_slot {
	struct list_head		list;
	struct flow_offload		*flow;		/* identity token only */
	struct flow_offload_tuple	orig_tuple;	/* by-value copy (for step-4 refresh) */
	u16				idx_out;	/* outbound row = gw_napt_hash1(...) */
	u16				idx_in;		/* inbound  row = globalPort & 0x3ff */
	u8				last_aging;	/* last agingTime seen by the worker */
	u8				is_tcp;
};

/* All of the following are protected by rtl865x_hal_lock. */
static LIST_HEAD(hwnat_flows);
static DECLARE_BITMAP(hwnat_used, RTL865X_NAPT_ROWS);	/* rows this module owns */
static bool hwnat_active;				/* datapath up (open) & armed */

static void hwnat_aging_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(hwnat_aging_work, hwnat_aging_work_fn);

/* ------------------------------------------------------------------ check hook */

/*
 * Runs under rcu_read_lock_bh (atomic): structural validation ONLY, no sleeping and
 * no ASIC table access. By the time we see the path the VLAN layer has flattened it
 * (path->dev == eth0, FLOW_OFFLOAD_PATH_VLAN set, vlan_id filled). Accept only a real
 * ethernet path carrying one of our two datapath VIDs; everything else declines to
 * the software fastpath. Called once for the LAN (src) path and once for the WAN
 * (dest) path — both must pass for the flow to be offered to ndo_flow_offload().
 *
 * ★ Deliberately NOT gated on rtl819x_hwnat_enabled: the framework re-runs this
 * check when preparing a DEL (nf_flow_table_hw.c flow_offload_hw_prepare), so a
 * decline here would silently DROP the DEL — toggling hwnat off with flows installed
 * would then leak their slots and leave live ASIC rows NAT-ing forever. The enable
 * gate lives in the ADD path only; with it off, every offer is declined there and
 * already-installed flows drain via their (still-delivered) DELs within one GC
 * timeout (~30 s).
 */
int rtl819x_hwnat_flow_offload_check(struct flow_offload_hw_path *path)
{
	if ((path->flags & (FLOW_OFFLOAD_PATH_ETHERNET | FLOW_OFFLOAD_PATH_VLAN)) !=
	    (FLOW_OFFLOAD_PATH_ETHERNET | FLOW_OFFLOAD_PATH_VLAN))
		return -EOPNOTSUPP;

	if (path->vlan_id != RTL865X_VID_LAN && path->vlan_id != RTL865X_VID_WAN)
		return -EOPNOTSUPP;

	return 0;
}

/* ------------------------------------------------------------------ ADD / DEL */

/*
 * Build both NAPT rows for one LAN->WAN masquerade flow and write them. Caller holds
 * rtl865x_hal_lock. Returns 0 on success, <0 to decline (flow stays in software).
 */
static int hwnat_program_rows(u32 is_tcp, u32 int_ip, u16 int_port, u16 gport,
			      u16 idx_out, u16 idx_in)
{
	/* The remote endpoint is not stored in the row — it is encoded in idx_out via
	 * gw_napt_hash1(); the row only needs the internal tuple + the ext mapping. */
	struct asic_napt_tcpudp e, v;
	int rc;

	memset(&e, 0, sizeof(e));
	e.intIPAddr  = int_ip;
	e.intPort    = int_port;
	e.isTCP      = is_tcp;
	e.valid      = 1;
	e.collision  = 1;			/* vendor: always set on a dedicated row */
	e.collision2 = 1;
	e.isStatic   = 0;			/* dynamic: let the ASIC age it (Phase 3) */
	e.dedicate   = 1;
	e.agingTime  = RTL865X_NAPT_AGING_RELOAD;	/* start at the ceiling (fresh) */
	e.selIPIdx   = 0;			/* -> extIP[0] = the WAN masquerade IP */
	e.selEIdx    = gport & 0x3ff;
	e.offset     = gport >> 10;		/* G reconstructs as (offset<<10)|selEIdx */
	e.TCPFlag    = 0x1;			/* outbound direction */

	rc = rtl865x_napt_write(idx_out, &e);
	if (rc)
		return rc;

	e.TCPFlag = 0x0;			/* inbound twin: only the direction differs */
	rc = rtl865x_napt_write(idx_in, &e);
	if (rc) {
		rtl865x_napt_clear(idx_out);
		return rc;
	}

	/* Read the outbound row back and sanity-check it landed. */
	if (rtl865x_napt_read(idx_out, &v) || !v.valid || v.intIPAddr != int_ip) {
		rtl865x_napt_clear(idx_out);
		rtl865x_napt_clear(idx_in);
		return -EIO;
	}
	return 0;
}

static int hwnat_add_flow(struct flow_offload *flow,
			  struct flow_offload_hw_path *src,
			  struct flow_offload_hw_path *dest)
{
	const struct flow_offload_tuple *o = &flow->tuplehash[FLOW_OFFLOAD_DIR_ORIGINAL].tuple;
	const struct flow_offload_tuple *r = &flow->tuplehash[FLOW_OFFLOAD_DIR_REPLY].tuple;
	u32 int_ip, rem_ip, ext_ip, is_tcp;
	u16 int_port, rem_port, gport;
	u16 idx_out, idx_in;
	struct hwnat_slot *slot;
	int rc;

	if (!READ_ONCE(rtl819x_hwnat_enabled))	/* runtime gate (also guards a stale ADD) */
		return -EOPNOTSUPP;

	/* Structural: LAN(vid2) src -> WAN(vid1) dest only. */
	if (src->vlan_id != RTL865X_VID_LAN || dest->vlan_id != RTL865X_VID_WAN)
		return -EOPNOTSUPP;

	/* Source-NAT masquerade only (no destination NAT / port forwards), and not a
	 * flow that is already being torn down. */
	if (!(flow->flags & FLOW_OFFLOAD_SNAT) || (flow->flags & FLOW_OFFLOAD_DNAT))
		return -EOPNOTSUPP;
	if (flow->flags & (FLOW_OFFLOAD_DYING | FLOW_OFFLOAD_TEARDOWN))
		return -EOPNOTSUPP;

	/* IPv4 TCP/UDP only (ICMP et al. keep trapping to the CPU via NAPTF2CPU). */
	if (o->l3proto != AF_INET)
		return -EOPNOTSUPP;
	if (o->l4proto == IPPROTO_TCP)
		is_tcp = 1;
	else if (o->l4proto == IPPROTO_UDP)
		is_tcp = 0;
	else
		return -EOPNOTSUPP;

	/* Tuple extraction (all conntrack fields are network-order; the ASIC tables and
	 * gw_napt_hash1() use host order). The REPLY tuple's destination is the actual
	 * masquerade mapping Linux picked (external IP + global port G) — never assume
	 * keep-port. */
	int_ip   = ntohl(o->src_v4.s_addr);
	int_port = ntohs(o->src_port);
	rem_ip   = ntohl(o->dst_v4.s_addr);
	rem_port = ntohs(o->dst_port);
	ext_ip   = ntohl(r->dst_v4.s_addr);
	gport    = ntohs(r->dst_port);

	/* We masquerade to exactly one WAN IP (extIP[0]); decline anything else. */
	if (ext_ip != RTL865X_WAN_EXTIP)
		return -EOPNOTSUPP;

	idx_out = gw_napt_hash1(is_tcp, int_ip, int_port, rem_ip, rem_port);
	idx_in  = gport & 0x3ff;

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	mutex_lock(&rtl865x_hal_lock);

	if (!hwnat_active) {			/* datapath went down under us */
		rc = -EOPNOTSUPP;
		goto out_unlock;
	}

	/* Both rows must be free in our shadow bitmap. A collision (or the pathological
	 * case where the two indices coincide) declines cleanly to software — the
	 * intended graceful fallback, not an error to chase. */
	if (idx_out == idx_in ||
	    test_bit(idx_out, hwnat_used) || test_bit(idx_in, hwnat_used)) {
		rc = -ENOSPC;
		goto out_unlock;
	}

	rc = hwnat_program_rows(is_tcp, int_ip, int_port, gport, idx_out, idx_in);
	if (rc)
		goto out_unlock;

	__set_bit(idx_out, hwnat_used);
	__set_bit(idx_in, hwnat_used);

	slot->flow       = flow;		/* identity token (never dereferenced later) */
	slot->orig_tuple = *o;			/* by-value copy while flow is alive */
	slot->idx_out    = idx_out;
	slot->idx_in     = idx_in;
	slot->last_aging = RTL865X_NAPT_AGING_RELOAD;
	slot->is_tcp     = is_tcp;
	list_add_tail(&slot->list, &hwnat_flows);

	/* Give the software flow a fresh 30 s window now (safe: the flow is alive during
	 * ADD). Step 4's aging worker will keep refreshing it for as long as the ASIC
	 * shows the row active, so a fully HW-forwarded flow won't be GC'd at 30 s. */
	flow->timeout = (u32)jiffies + NF_FLOW_TIMEOUT;

	mutex_unlock(&rtl865x_hal_lock);

	mod_delayed_work(system_wq, &hwnat_aging_work, HWNAT_AGING_INTERVAL);

	/* Ratelimited: on a busy LAN a per-flow line would flood the log, and printk
	 * over the 38400-baud console is itself a throttle. /proc/rtl865x_napt gives
	 * the full live picture on demand. */
	pr_info_ratelimited("rtl819x hwnat: +%s %pI4h:%u -> %pI4h:%u  G=%u  rows out@%u in@%u\n",
			    is_tcp ? "tcp" : "udp", &int_ip, int_port, &rem_ip, rem_port,
			    gport, idx_out, idx_in);
	return 0;

out_unlock:
	mutex_unlock(&rtl865x_hal_lock);
	kfree(slot);
	return rc;
}

static int hwnat_del_flow(struct flow_offload *flow)
{
	struct hwnat_slot *slot, *tmp;

	/* NOTE: `flow` may already be freed; use it ONLY as a comparison key.
	 * No `break` on match: if a slot's DEL was ever lost (see the check-hook
	 * comment) its stale pointer can alias a LATER flow allocated at the same
	 * address — clearing every match takes out the stale slot together with the
	 * real one instead of leaking the live flow's rows. */
	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry_safe(slot, tmp, &hwnat_flows, list) {
		if (slot->flow != flow)
			continue;
		rtl865x_napt_clear(slot->idx_out);
		rtl865x_napt_clear(slot->idx_in);
		__clear_bit(slot->idx_out, hwnat_used);
		__clear_bit(slot->idx_in, hwnat_used);
		list_del(&slot->list);
		kfree(slot);
	}
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

int rtl819x_hwnat_flow_offload(enum flow_offload_type type,
			       struct flow_offload *flow,
			       struct flow_offload_hw_path *src,
			       struct flow_offload_hw_path *dest)
{
	switch (type) {
	case FLOW_OFFLOAD_ADD:
		return hwnat_add_flow(flow, src, dest);
	case FLOW_OFFLOAD_DEL:
		return hwnat_del_flow(flow);
	default:
		return -EOPNOTSUPP;
	}
}

/* ------------------------------------------------------------------ aging worker */

/*
 * Runs every HWNAT_AGING_INTERVAL in process context. v1 (Phase-3 steps 1-3):
 * OBSERVE only — read each row's live agingTime (which ticks because TEACR L4-aging
 * is on) and count how many are at the reload ceiling (== recently carried traffic).
 * It deliberately does NOT dereference slot->flow.
 *
 * Phase-3 step 4 will add the timeout refresh here, done UAF-safely: look the flow up
 * by slot->orig_tuple under rcu_read_lock via the (exported) flow_offload_lookup on
 * xt_FLOWOFFLOAD's table — a lookup only returns a still-linked, non-dying flow, and
 * RCU keeps it alive across the flow->timeout store. That is what lets an actively
 * HW-forwarded flow avoid re-offloading every 30 s. Until then, a long-lived flow
 * simply re-learns each GC timeout (a brief software-forwarding blip), which is
 * correct, just not optimal.
 */
static void hwnat_aging_work_fn(struct work_struct *w)
{
	struct hwnat_slot *slot, *tmp;
	struct asic_napt_tcpudp e;
	unsigned int total = 0, active = 0, reaped = 0;

	mutex_lock(&rtl865x_hal_lock);
	if (!hwnat_active) {
		mutex_unlock(&rtl865x_hal_lock);
		return;
	}
	list_for_each_entry_safe(slot, tmp, &hwnat_flows, list) {
		total++;
		if (rtl865x_napt_read(slot->idx_out, &e))
			continue;
		if (!e.valid) {
			/* The ASIC auto-deleted the row at age-0 (EnNAPTAutoDelete) —
			 * i.e. the flow has been idle past the ~102 s TEATCR reload.
			 * Normally the software GC DELs a flow at 30 s idle and we clear
			 * the rows then, so reaching hardware age-out means this slot's
			 * DEL was LOST (bridge-FDB/neigh miss at DEL-prepare, GFP_ATOMIC
			 * failure, device churn — the framework silently drops the DEL).
			 * Reap it here so the two row indices return to the pool instead
			 * of leaking until ifdown; a late real DEL for this flow then
			 * simply finds no slot, which is harmless. */
			rtl865x_napt_clear(slot->idx_in);	/* twin may still be valid */
			__clear_bit(slot->idx_out, hwnat_used);
			__clear_bit(slot->idx_in, hwnat_used);
			list_del(&slot->list);
			kfree(slot);
			reaped++;
			continue;
		}
		if (e.agingTime >= RTL865X_NAPT_AGING_RELOAD || e.agingTime > slot->last_aging)
			active++;
		slot->last_aging = e.agingTime;
	}
	mutex_unlock(&rtl865x_hal_lock);

	if (total)
		pr_debug("rtl819x hwnat: aging poll — %u flows, %u active, %u reaped\n",
			 total, active, reaped);

	/* Re-arm while the datapath is up. Racing with stop() is fine:
	 * cancel_delayed_work_sync() there waits this out, and hwnat_active is re-checked
	 * at the top of every run. */
	if (READ_ONCE(hwnat_active))
		mod_delayed_work(system_wq, &hwnat_aging_work, HWNAT_AGING_INTERVAL);
}

/* ------------------------------------------------------------------ lifecycle */

void rtl819x_hwnat_start(struct net_device *dev)
{
	/* Always arm on interface-up regardless of the current hwnat setting, so the
	 * param can be toggled on at runtime with no reboot. The ADD/check hooks gate on
	 * the param, so while it is off nothing is installed and the worker just polls an
	 * empty list. */
	mutex_lock(&rtl865x_hal_lock);
	hwnat_active = true;
	mutex_unlock(&rtl865x_hal_lock);

	mod_delayed_work(system_wq, &hwnat_aging_work, HWNAT_AGING_INTERVAL);
	netdev_info(dev, "hwnat: offload ready (rtl819x.hwnat=%d)\n", rtl819x_hwnat_enabled);
}

void rtl819x_hwnat_stop(void)
{
	struct hwnat_slot *slot, *tmp;

	/* Quiesce ADD/worker via hwnat_active, then tear down every installed row. */
	mutex_lock(&rtl865x_hal_lock);
	hwnat_active = false;
	list_for_each_entry_safe(slot, tmp, &hwnat_flows, list) {
		rtl865x_napt_clear(slot->idx_out);
		rtl865x_napt_clear(slot->idx_in);
		__clear_bit(slot->idx_out, hwnat_used);
		__clear_bit(slot->idx_in, hwnat_used);
		list_del(&slot->list);
		kfree(slot);
	}
	mutex_unlock(&rtl865x_hal_lock);

	/* Outside the lock (the worker takes it): wait for any in-flight poll to finish. */
	cancel_delayed_work_sync(&hwnat_aging_work);
}
