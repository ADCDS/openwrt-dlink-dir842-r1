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
#include <linux/inetdevice.h>	/* M7.2: read the WAN netdev's live IPv4 */
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

/*
 * M7.3 step 4 handshake state (also under rtl865x_hal_lock): the module-owned flow table +
 * its flow_offload_lookup(), pushed in by xt_FLOWOFFLOAD at module init (we are =y and can't
 * link those =m symbols directly). Both NULL until the module registers; the aging worker
 * refreshes timeouts only while both are set.
 */
static struct nf_flowtable		*hwnat_flowtable;
static rtl819x_flow_lookup_fn		hwnat_flow_lookup;

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
/*
 * M7.2 Part B: the ASIC PPPoE hardware offload. Default ON — now FUNCTIONAL.
 * The EN_PPPOE encap path programs correctly (session table + type=1 PPPoEIndex
 * nexthop + dynamic extIP) AND now egresses. The original black-hole was the WAN
 * netif MTU set to 1492 (vs stock 1500), which made the ASIC trap the encap
 * frame as oversized — fixed in rtl865x_asichal.c gw_wan_netif_prog_locked().
 * Bench-validated: a LAN->PPPoE flow offloads and forwards end-to-end at
 * ~157 Mbit/s (peer eth0 rx-byte rate), was 0 (total black-hole). The A-2
 * trunk-pause fix (rtl819x-eth.c) is what lets it sustain — otherwise the
 * loader's trunk PAUSE throttled it. Set to 0 to force PPPoE onto software
 * forwarding.
 */
static bool pppoe_hw_offload = true;
module_param(pppoe_hw_offload, bool, 0644);
MODULE_PARM_DESC(pppoe_hw_offload,
	"ASIC PPPoE encap offload: 1=on (default, ~157 Mbit bench-validated), 0=software forwarding");

int rtl819x_hwnat_flow_offload_check(struct flow_offload_hw_path *path)
{
	/* Two accepted path shapes once the upper layers have flattened onto eth0
	 * (M7.2):
	 *   ETHERNET|VLAN       — plain ethernet on one of our two datapath VIDs
	 *                         (the LAN side, or the bench ethernet-WAN).
	 *   PPPOE|VLAN (no ETH) — PPPoE WAN: ppp0 is not ARPHRD_ETHER so the core
	 *                         never set ETHERNET (nf_flow_table_hw.c
	 *                         flow_offload_check_ethernet); instead
	 *                         pppoe_flow_offload_check filled eth_dest = the AC
	 *                         MAC + pppoe_sid and chained through eth0.1 (which
	 *                         added VLAN). Only the WAN VID may carry PPPoE, and
	 *                         a connected session always has a nonzero sid. */
	if (path->flags == (FLOW_OFFLOAD_PATH_ETHERNET | FLOW_OFFLOAD_PATH_VLAN)) {
		if (path->vlan_id != RTL865X_VID_LAN && path->vlan_id != RTL865X_VID_WAN)
			return -EOPNOTSUPP;
		return 0;
	}

	if (path->flags == (FLOW_OFFLOAD_PATH_PPPOE | FLOW_OFFLOAD_PATH_VLAN)) {
		/* Default OFF (see pppoe_hw_offload above): the ASIC encap black-holes
		 * offloaded PPPoE, so decline here and let PPPoE ride software forwarding. */
		if (!pppoe_hw_offload)
			return -EOPNOTSUPP;
		if (path->vlan_id != RTL865X_VID_WAN || path->pppoe_sid == 0)
			return -EOPNOTSUPP;
		return 0;
	}

	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------ ADD / DEL */

/*
 * Current primary IPv4 of the netdev with ifindex @ifidx in @net, host order
 * (0 = device gone / no address). For a PPPoE WAN this is ppp0's local IP —
 * assigned at IPCP-up, different on every reconnect, so it must be read LIVE
 * and never compiled in. Same RCU discipline as inet_select_addr(): safe from
 * the sleepable ADD path with only rcu_read_lock held.
 */
static u32 hwnat_wan_ipv4(struct net *net, int ifidx)
{
	struct net_device *dev;
	struct in_device *in_dev;
	u32 ip = 0;

	rcu_read_lock();
	dev = dev_get_by_index_rcu(net, ifidx);
	if (dev) {
		in_dev = __in_dev_get_rcu(dev);
		if (in_dev && in_dev->ifa_list)
			ip = ntohl(in_dev->ifa_list->ifa_local);
	}
	rcu_read_unlock();
	return ip;
}

/*
 * Tear down every installed flow: rows cleared, indices returned, slots freed.
 * Used when the WAN identity changes under us (extIP / PPPoE session / peer
 * MAC) — rows programmed under the old identity would translate to a stale IP
 * or encapsulate into a dead session — and by rtl819x_hwnat_stop(). The flows
 * themselves keep software-forwarding and simply re-offload on a later ADD
 * offer. Caller holds rtl865x_hal_lock.
 */
static void hwnat_flush_locked(void)
{
	struct hwnat_slot *slot, *tmp;

	list_for_each_entry_safe(slot, tmp, &hwnat_flows, list) {
		rtl865x_napt_clear(slot->idx_out);
		rtl865x_napt_clear(slot->idx_in);
		__clear_bit(slot->idx_out, hwnat_used);
		__clear_bit(slot->idx_in, hwnat_used);
		list_del(&slot->list);
		kfree(slot);
	}
}

/*
 * Build both NAPT rows for one LAN->WAN masquerade flow and write them. Caller holds
 * rtl865x_hal_lock. Returns 0 on success, <0 to decline (flow stays in software).
 */
static int hwnat_program_rows(u32 is_tcp, u32 int_ip, u16 int_port, u16 gport,
			      u32 rem_ip, u16 rem_port, u16 idx_out, u16 idx_in)
{
	/* The outbound row encodes the internal tuple + ext mapping; the inbound row is
	 * NOT a copy of it (see below) — it re-encodes G differently and stores a
	 * remote-endpoint VERIFICATION hash the ASIC checks on every return packet. */
	struct asic_napt_tcpudp e, v;
	int rc;

	memset(&e, 0, sizeof(e));
	e.intIPAddr  = htonl(int_ip);		/* NETWORK order: the ASIC verifies the stored */
	e.intPort    = htons(int_port);		/* key against the on-wire src (vendor nat.c:1129-1130) */
	e.isTCP      = is_tcp;
	e.valid      = 1;
	e.collision  = 1;			/* vendor: always set on a dedicated row */
	e.collision2 = 1;
	e.isStatic   = 1;			/* vendor: driver-added NAPT rows are STATIC (nat.c:1134); a dynamic (isStatic=0) row with auto-learn OFF is matched but NOT forwarded */
	e.dedicate   = 0;			/* vendor main path sets isDedicated=0 (nat.c:1133); dedicate=1 was mis-copied from the LIBERAL setter (asicL4.c:191) */
	e.agingTime  = RTL865X_NAPT_AGING_RELOAD;	/* start at the ceiling (fresh) */
	e.selIPIdx   = 0;			/* -> extIP[0] = the WAN masquerade IP */
	e.selEIdx    = htons(gport) & 0x3ff;	/* G in NETWORK order -- the ASIC writes it back */
	e.offset     = htons(gport) >> 10;	/* as the on-wire src port (vendor nat.c:711-712) */
	e.TCPFlag    = 0x3;			/* vendor outbound: 0x2 (unidirectional) | 0x1 (outbound), nat.c:1142 */

	rc = rtl865x_napt_write(idx_out, &e);
	if (rc)
		return rc;

	/* Inbound/return row is NOT a TCPFlag-tweaked copy of the outbound row. The ASIC
	 * treats it as a VERIFICATION row (enhanced-hash1): its offset/selEIdx/selIPIdx
	 * encode different fields than the outbound row (vendor nat.c:1137-1140). It stores
	 * the low 10 bits of G split across offset[5:0] + selIPIdx[9:6], and in selEIdx a
	 * verification hash `very` of the remote endpoint = HASH1(remIP,remPort,0,0)
	 * (nat.c:714; HASH_FOR_VERI forces HASH1, so gw_napt_hash1 with dip/dport=0 is it).
	 * On an inbound hit the silicon recomputes `very` from the packet's src
	 * {remIP,remPort} and REJECTS the row on mismatch — so a copied outbound selEIdx
	 * (=G) fails verification: the return packet is never un-NAT'd (dst stays the extIP),
	 * mis-routes, and egresses with a garbage DMAC that floods (proven on the WAN peer:
	 * forward frames carried the peer MAC + correct src, return frames carried
	 * 00:00:00:00:00:10 and an un-rewritten dst). intIPAddr/intPort (the un-NAT target)
	 * are identical in both rows and stay as set above. */
	e.offset   = htons(gport) & 0x3f;
	e.selEIdx  = gw_napt_hash1(is_tcp, htonl(rem_ip), htons(rem_port), 0, 0) & 0x3ff;
	e.selIPIdx = (htons(gport) & 0x3ff) >> 6;
	e.TCPFlag  = 0x2;			/* vendor inbound: 0x2 (unidirectional) | 0x0 (inbound), nat.c:1142 */
	rc = rtl865x_napt_write(idx_in, &e);
	if (rc) {
		rtl865x_napt_clear(idx_out);
		return rc;
	}

	/* Read the outbound row back and sanity-check it landed. */
	if (rtl865x_napt_read(idx_out, &v) || !v.valid || v.intIPAddr != htonl(int_ip)) {
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
	u32 int_ip, rem_ip, ext_ip, is_tcp, wan_ip;
	u16 int_port, rem_port, gport;
	bool wan_pppoe;
	u16 idx_out, idx_in;
	struct hwnat_slot *slot;
	int rc;

	if (!READ_ONCE(rtl819x_hwnat_enabled))	/* runtime gate (also guards a stale ADD) */
		return -EOPNOTSUPP;

	/* Structural: LAN(vid2) src -> WAN(vid1) dest only; PPPoE (if present) can
	 * only be the WAN side. */
	if (src->vlan_id != RTL865X_VID_LAN || dest->vlan_id != RTL865X_VID_WAN)
		return -EOPNOTSUPP;
	if (src->flags & FLOW_OFFLOAD_PATH_PPPOE)
		return -EOPNOTSUPP;
	wan_pppoe = !!(dest->flags & FLOW_OFFLOAD_PATH_PPPOE);

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

	/* M7.2: the masquerade IP is DYNAMIC — it must equal the WAN interface's
	 * CURRENT primary IPv4 (ppp0's local address on a PPPoE WAN, assigned at
	 * IPCP-up and different every reconnect; the bench rig's static 172.16.0.1
	 * on the ethernet WAN). The REPLY tuple's iifidx IS the WAN-side ingress
	 * device (nf_flow_table_core fill_dir: iifidx = other-direction egress dev),
	 * so read the address off it live. Masquerade always sources from the
	 * egress interface's address, so a mismatch means the address moved under
	 * the flow — decline; the flow is dying with the old address anyway. */
	wan_ip = hwnat_wan_ipv4(dev_net(dest->dev), r->iifidx);
	if (!wan_ip || ext_ip != wan_ip)
		return -EOPNOTSUPP;

	/* The ASIC hashes/keys the ON-WIRE (network-order) header fields. The vendor
	 * driver installs the outbound row at hash(htonl(intIP),htons(intPort),
	 * htonl(remIP),htons(remPort)) to match it (sdk-ref l4Driver/rtl865x_nat.c:716;
	 * the vendor's naptEntry IPs/ports are HOST order -- see the 0xc0a8030b test
	 * literals in rtl865x_proc_debug.c:1725 and the byte<<24|byte<<16|... compose in
	 * rtl865x_multipleWan.c:1018 -- and it converts with htonl/htons at the ASIC
	 * boundary). This is a LITTLE-ENDIAN build (target rtl8197f ARCH:=mipsel,
	 * CONFIG_CPU_LITTLE_ENDIAN=y), so htonl(host)!=host: feeding host order writes the
	 * row at a byte-swapped index the ASIC never looks up -> every outbound packet
	 * misses, traps to the CPU, and the orphan row just ages out (the exact symptom).
	 * Feed NETWORK order for the index AND for the stored key + G-encoding (below). */
	idx_out = gw_napt_hash1(is_tcp, htonl(int_ip), htons(int_port),
				htonl(rem_ip), htons(rem_port));
	/* Return-path row: the ASIC hashes the INBOUND packet {src=rem, dst=ext:G} to
	 * locate the reverse row, so it must live at hash(remIP,remPort,extIP,G) -- NOT at
	 * G&0x3ff (that full-cone shortcut is not where the silicon looks). Vendor
	 * l4Driver/rtl865x_nat.c:715: in = naptTcpUdpTableIndex(htonl(remIp),htons(remPort),
	 * htonl(extIp),extPort), extPort=htons(G); network order as everywhere. Without it,
	 * WAN->LAN replies miss + trap to CPU (NAPTR_NOT_FOUND_DROP=0): the flow still works
	 * but the return half is never HW-accelerated (and the old G&0x3ff row aliases an
	 * unrelated inbound slot). */
	idx_in  = gw_napt_hash1(is_tcp, htonl(rem_ip), htons(rem_port),
				htonl(ext_ip), htons(gport));

	slot = kzalloc(sizeof(*slot), GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	mutex_lock(&rtl865x_hal_lock);

	if (!hwnat_active) {			/* datapath went down under us */
		rc = -EOPNOTSUPP;
		goto out_unlock;
	}

	/* M7.2: sync the live WAN identity into the ASIC before touching rows.
	 * extIP[0] <- the masquerade IP (verified == the WAN local IP above);
	 * nexthop chain <- the WAN peer MAC from the dest path (PPPoE: the AC MAC
	 * pppoe_flow_offload_check captured; ethernet: the neigh MAC), plus type=1
	 * + the PPPoE session row when the WAN is PPPoE. Both helpers are
	 * shadow-compared no-ops in steady state; 1 means the identity CHANGED
	 * (PPP reconnect / new session / new IP), so every row programmed under the
	 * old identity is flushed — those flows fall back to software and
	 * re-offload on their next offer. */
	rc = rtl865x_set_wan_extip(ext_ip);
	if (rc < 0)
		goto out_unlock;
	if (rc > 0)
		hwnat_flush_locked();
	rc = rtl865x_wan_set_nexthop(dest->eth_dest, wan_pppoe,
				     wan_pppoe ? dest->pppoe_sid : 0);
	if (rc < 0)
		goto out_unlock;
	if (rc > 0)
		hwnat_flush_locked();

	/* Both rows must be free in our shadow bitmap. A collision (or the pathological
	 * case where the two indices coincide) declines cleanly to software — the
	 * intended graceful fallback, not an error to chase. */
	if (idx_out == idx_in ||
	    test_bit(idx_out, hwnat_used) || test_bit(idx_in, hwnat_used)) {
		rc = -ENOSPC;
		goto out_unlock;
	}

	rc = hwnat_program_rows(is_tcp, int_ip, int_port, gport, rem_ip, rem_port, idx_out, idx_in);
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

/* ------------------------------------------------------------------ step-4 handshake */

/*
 * Registration handshake with xt_FLOWOFFLOAD (=m). We are =y and cannot link that module's
 * EXPORT'd flow_offload_lookup() nor its file-static nf_flowtable, so the module hands us
 * {&nf_flowtable, flow_offload_lookup} at its init and retracts them at its exit (see
 * hack-4.14/650-netfilter-add-xt_OFFLOAD-target.patch and the prototypes in rtl819x_hwnat.h).
 * Serialized against the aging worker by rtl865x_hal_lock, so once unregister() returns no
 * poll can dereference the module state it is about to free.
 */
void rtl819x_hwnat_flowtable_register(struct nf_flowtable *ft, rtl819x_flow_lookup_fn lookup)
{
	mutex_lock(&rtl865x_hal_lock);
	hwnat_flowtable   = ft;
	hwnat_flow_lookup = lookup;
	mutex_unlock(&rtl865x_hal_lock);
	pr_info("rtl819x hwnat: step-4 timeout-refresh armed (xt_FLOWOFFLOAD flowtable registered)\n");
}
EXPORT_SYMBOL_GPL(rtl819x_hwnat_flowtable_register);

void rtl819x_hwnat_flowtable_unregister(void)
{
	mutex_lock(&rtl865x_hal_lock);
	hwnat_flowtable   = NULL;
	hwnat_flow_lookup = NULL;
	mutex_unlock(&rtl865x_hal_lock);
}
EXPORT_SYMBOL_GPL(rtl819x_hwnat_flowtable_unregister);

/*
 * Step 4: refresh the software flow's GC timeout so a flow the ASIC is still actively
 * forwarding is not torn down + re-learned every NF_FLOW_TIMEOUT (~30 s). UAF-safe: never
 * touch slot->flow (it may be freed); look the flow up by our by-value ORIGINAL-tuple copy
 * on the module's table under rcu_read_lock. flow_offload_lookup() returns only a still-
 * linked, non-DYING/TEARDOWN flow, and because flow_offload_free() defers with kfree_rcu(),
 * the RCU read-side section pins the flow across the timeout store. Caller holds
 * rtl865x_hal_lock (so both handshake pointers are stable here). No-op until the module
 * has registered.
 */
static void hwnat_refresh_timeout(struct hwnat_slot *slot)
{
	struct flow_offload_tuple_rhash *th;
	struct flow_offload *flow;

	if (!hwnat_flow_lookup || !hwnat_flowtable)
		return;

	rcu_read_lock();
	th = hwnat_flow_lookup(hwnat_flowtable, &slot->orig_tuple);
	if (th) {
		flow = container_of(th, struct flow_offload, tuplehash[th->tuple.dir]);
		flow->timeout = (u32)jiffies + NF_FLOW_TIMEOUT;
	}
	rcu_read_unlock();
}

/* ------------------------------------------------------------------ aging worker */

/*
 * Runs every HWNAT_AGING_INTERVAL in process context. Two jobs:
 *  - OBSERVE (Phase-3 steps 1-3): read each row's live agingTime (which ticks because
 *    TEACR L4-aging is on), reap rows the ASIC auto-deleted at age-0, count how many are
 *    still hot. Never dereferences slot->flow.
 *  - REFRESH (step 4): for every still-hot row, hwnat_refresh_timeout() pushes the software
 *    flow's GC timeout out (UAF-safe, by orig_tuple under RCU). This is what lets an actively
 *    HW-forwarded flow avoid being GC'd + software-re-learned every ~30 s. The refresh is a
 *    no-op until xt_FLOWOFFLOAD registers its table (see the handshake section above);
 *    without it a long-lived flow just re-learns each GC timeout — correct, only less optimal.
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
		{	/* M7 DEBUG instrument: expose BOTH row ages each poll to the console log
			 * (captured continuously by the bootlog, so it survives serial input-
			 * overrun). Decisive question: on WAN->LAN return traffic does the ASIC
			 * HIT the inbound row (age pins near the RELOAD ceiling) or NEVER look it
			 * up (age monotonically decays)? The latter = reverse-NAPT isn't even
			 * attempted (L3 routes the reply before the L4 reverse stage). */
			struct asic_napt_tcpudp ein;
			u8 age_in = 0xff;
			if (!rtl865x_napt_read(slot->idx_in, &ein) && ein.valid)
				age_in = ein.agingTime;
			pr_err("hwnat AGE out[%u]=%u in[%u]=%u\n",
			       slot->idx_out, e.agingTime, slot->idx_in, age_in);
		}
		if (e.agingTime >= RTL865X_NAPT_AGING_RELOAD || e.agingTime > slot->last_aging) {
			/* Row still hot (agingTime at the reload ceiling, or it ticked back UP
			 * since the last poll = the ASIC reloaded it on a recent hit) => the flow
			 * is live in silicon. Push its software GC timeout out so the flowtable
			 * GC doesn't tear it down at 30 s idle and force a software re-learn. */
			active++;
			hwnat_refresh_timeout(slot);
		}
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
	/* Quiesce ADD/worker via hwnat_active, then tear down every installed row. */
	mutex_lock(&rtl865x_hal_lock);
	hwnat_active = false;
	hwnat_flush_locked();
	mutex_unlock(&rtl865x_hal_lock);

	/* Outside the lock (the worker takes it): wait for any in-flight poll to finish. */
	cancel_delayed_work_sync(&hwnat_aging_work);
}
