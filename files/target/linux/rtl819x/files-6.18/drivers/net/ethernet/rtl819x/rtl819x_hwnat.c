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
#include <net/flow_offload.h>
#include <net/pkt_cls.h>
#include <net/neighbour.h>
#include <linux/etherdevice.h>
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
	/*
	 * The two tc-flower cookies this flow was offered under -- one per
	 * direction. nftables offers a bidirectional flow as two independent
	 * rules and expects BOTH to be accepted, so the pair is installed from
	 * whichever arrives first and the second is matched against it and
	 * acknowledged. Either cookie alone must be able to tear the pair down.
	 */
	unsigned long			cookie_out;	/* LAN->WAN rule */
	unsigned long			cookie_in;	/* WAN->LAN rule */
	u32				int_ip, rem_ip, ext_ip;
	u16				int_port, rem_port, gport;
	u16				idx_out;	/* outbound ASIC row */
	u16				idx_in;		/* inbound  ASIC row */
	u8				last_aging;	/* last agingTime seen by the worker */
	u8				is_tcp;
};
/* All of the following are protected by rtl865x_hal_lock. */
static LIST_HEAD(hwnat_flows);
static DECLARE_BITMAP(hwnat_used, RTL865X_NAPT_ROWS);	/* rows this module owns */
static bool hwnat_active;				/* datapath up (open) & armed */

static void hwnat_aging_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(hwnat_aging_work, hwnat_aging_work_fn);

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
/* ---- NAPT row key/action field sweep knobs -------------------------------
 * The fill-all discriminator proved the ASIC reaches a row and REJECTS its key, so the
 * fault is in these fields. Each is exposed so a host-side sweep can flip one at a time
 * and let the chip's own trap reason vote (reason 7 = still rejected, anything else =
 * the row was accepted). -1 means "use the built-in default".
 *
 * ★ napt_collision is the strongest suspect. This driver sets collision=collision2=1 on
 * every ACTIVE row, justified as "vendor: always set on a dedicated row" -- but the R6/B3
 * work records the opposite reading, that collision=collision2=1 is how a FREE row is
 * marked. If that reading is right, every row we write is flagged as free/collided and
 * the lookup walks straight past it, which is exactly "no matched NAPT entry". */
static int napt_collision;	/* bit0 = collision, bit1 = collision2; default 0 = PROVEN */
module_param(napt_collision, int, 0644);
MODULE_PARM_DESC(napt_collision, "NAPT row collision bits: -1=default(3), 0=neither, 1=collision, 2=collision2, 3=both");
static int napt_static = -1;
module_param(napt_static, int, 0644);
MODULE_PARM_DESC(napt_static, "NAPT row isStatic: -1=default(1), 0, 1");
static int napt_dedicate = -1;
module_param(napt_dedicate, int, 0644);
MODULE_PARM_DESC(napt_dedicate, "NAPT row dedicate: -1=default(0), 0, 1");
static int napt_tcpflag = -1;
module_param(napt_tcpflag, int, 0644);
MODULE_PARM_DESC(napt_tcpflag, "NAPT outbound row TCPFlag: -1=default(3), 0..7");

/* NAPT row key byte order: 1 = network (htonl/htons, previous), 0 = host order. */

/* ★ The G (global/masqueraded port) encoding in offset/selEIdx is a SEPARATE byte
 * order from the key above -- napt_key_htonl only ever swapped intIPAddr/intPort, so
 * this axis has never been varied. The vendor writes the raw HOST-order port:
 *     entry.offset  = insideGlobalPort >> RTL8651_TCPUDPTBL_BITS;   (asicL4.c:183)
 *     entry.selEIdx = insideGlobalPort &  (RTL8651_TCPUDPTBL_SIZE-1);  (asicL4.c:207)
 * this driver has always written htons(gport). 1 = htons (previous behaviour),
 * 0 = host order (what the vendor code literally does). */
/* ★ Byte order of the HASH INPUTS (distinct from the key and from G).
 * gw_napt_hash1() is a verified-faithful port of the vendor's modelLayer4Hash1()
 * (naptModel.c) -- the bit mapping was diffed field by field and matches. What was
 * never checked is what we feed it. The vendor's hsb fields come off the packet in
 * network order, so its htonl()/htons() on a little-endian build yield the NUMERIC
 * value the silicon hashes. This driver has already normalised to host order
 * upstream (int_ip = ntohl(...); int_port = ntohs(...)), so applying htonl()/htons()
 * again is a DOUBLE conversion that swaps it right back -- landing every row at a
 * byte-swapped index the hardware never probes. Self-consistently, since our write
 * and our readback both use it. 1 = htonl/htons (previous behaviour), 0 = feed the
 * host-order values straight through (what the vendor effectively hashes). */


/* Diagnostic: fill every NAPT index with the outbound row (see hwnat_program_rows). */
static int napt_fill_all;
module_param(napt_fill_all, int, 0644);
MODULE_PARM_DESC(napt_fill_all, "DIAG: write the outbound NAPT row at all 1024 indices to test whether the index derivation is the fault (0=off)");

static int hwnat_program_rows(u32 is_tcp, u32 int_ip, u16 int_port, u16 gport,
			      u32 rem_ip, u16 rem_port, u16 idx_out, u16 idx_in)
{
	/* The outbound row encodes the internal tuple + ext mapping; the inbound row is
	 * NOT a copy of it (see below) — it re-encodes G differently and stores a
	 * remote-endpoint VERIFICATION hash the ASIC checks on every return packet. */
	struct asic_napt_tcpudp e, v;
	int rc;

	/* ★ KEY BYTE ORDER. The napt_fill_all discriminator proved the INDEX is not the
	 * fault: with all 1024 rows holding this row the ASIC still reported reason 7
	 * ("no matched NAPT entry"), i.e. it reaches a row and REJECTS the key. Byte order
	 * is the prime suspect, and it is precisely where the SDKs diverge -- 3.4.11B+ adds
	 * htonl()/htons() in l4Driver/rtl865x_nat.c on the hash index and the outbound row
	 * match, which our 3.4.9.x reference lacks (it computes unswapped at :325-326 while
	 * swapping at :714-716). 1 = network order (previous behaviour), 0 = host order. */
	memset(&e, 0, sizeof(e));
	e.intIPAddr  = int_ip;
	e.intPort    = int_port;
	e.isTCP      = is_tcp;
	e.valid      = 1;
	e.collision  =  napt_collision       & 1;
	e.collision2 = (napt_collision >> 1) & 1;
	e.isStatic   = (napt_static    < 0) ? 1 : !!napt_static;	/* vendor: driver-added rows are STATIC (nat.c:1134) */
	e.dedicate   = (napt_dedicate  < 0) ? 0 : !!napt_dedicate;	/* vendor main path sets isDedicated=0 (nat.c:1133) */
	e.agingTime  = RTL865X_NAPT_AGING_RELOAD;	/* start at the ceiling (fresh) */
	e.selIPIdx   = 0;			/* -> extIP[0] = the WAN masquerade IP */
	e.selEIdx    = gport & 0x3ff;
	e.offset     = gport >> 10;
	e.TCPFlag    = (napt_tcpflag < 0) ? 0x3 : (napt_tcpflag & 0x7);	/* vendor outbound: 0x2|0x1, nat.c:1142 */

	/* ★ DIAGNOSTIC: napt_fill_all writes the OUTBOUND row at EVERY index, which
	 * separates the two possible causes of the ASIC's "no matched NAPT entry" trap
	 * (decoded reason 7) in a single shot:
	 *   - if the trap DISAPPEARS, every index now holds a valid row, so the row
	 *     CONTENT/key is acceptable to the silicon and our INDEX derivation is wrong;
	 *   - if the trap PERSISTS, the hardware is reaching a row and rejecting it, so the
	 *     KEY FIELDS are wrong and no amount of index fixing will help.
	 * Debug only -- it destroys the inbound row and the whole table. */
	if (napt_fill_all) {
		u16 i;

		for (i = 0; i < 1024; i++)
			rtl865x_napt_write(i, &e);
		pr_err("rtl819x hwnat: DIAG filled all 1024 NAPT rows with the outbound row "
		       "(intIP=%pI4h intPort=%u G=%u) -- index derivation now cannot matter\n",
		       &int_ip, int_port, gport);
		return 0;
	}

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
	e.offset   = gport & 0x3f;
	/* ★ NUMERIC, like every other hash input. This was the last hardcoded htonl/htons
	 * pair in the driver — it had no knob, so it stayed byte-swapped through the whole
	 * byte-order investigation and kept the WAN->LAN direction at ~100% through-CPU
	 * even after the outbound row started hardware-forwarding at 0.0%. The ASIC
	 * recomputes this verification hash from the inbound packet's numeric {remIP,
	 * remPort} and rejects the row on mismatch, so a swapped `very` fails every
	 * inbound lookup. Numeric, exactly like the two row indices. */
	e.selEIdx  = gw_napt_hash1(is_tcp, rem_ip, rem_port, 0, 0) & 0x3ff;
	e.selIPIdx = (gport & 0x3ff) >> 6;
	e.TCPFlag  = 0x2;			/* vendor inbound: 0x2 (unidirectional) | 0x0 (inbound), nat.c:1142 */
	rc = rtl865x_napt_write(idx_in, &e);
	if (rc) {
		rtl865x_napt_clear(idx_out);
		return rc;
	}

	/* Read the outbound row back and sanity-check it landed.
	 *
	 * ★ The comparison MUST use the same byte order the row was written with at the
	 * top of this function. It used to hardcode htonl(int_ip): with napt_key_htonl=0
	 * (host order — the proven setting) every install wrote a host-order key, failed
	 * this check, tore both rows down and returned -EIO. The offload then silently
	 * never engaged, and because the failure happens BEFORE the "+tcp" log line the
	 * only visible symptom was a permanent trap reason 7 ("no matched NAPT entry")
	 * with no row ever appearing — indistinguishable from the offer never arriving. */
	if (rtl865x_napt_read(idx_out, &v) || !v.valid ||
	    v.intIPAddr != int_ip) {
		rtl865x_napt_clear(idx_out);
		rtl865x_napt_clear(idx_in);
		return -EIO;
	}
	return 0;
}

/* Bench instrument: log why an offer was declined. Every decline path below is a
 * silent -EOPNOTSUPP, which makes "no row ever installs" indistinguishable from
 * "the offer never arrived" -- exactly the ambiguity that cost a full debug cycle.
 * Ratelimited; set hwnat_debug=0 to silence. */
static int hwnat_debug = 1;
module_param(hwnat_debug, int, 0644);
MODULE_PARM_DESC(hwnat_debug, "log why hwnat declines a flow-offload offer (1=on)");

#define HWNAT_DECLINE(fmt, ...)						\
	do {								\
		if (hwnat_debug)					\
			pr_info_ratelimited("rtl819x hwnat: decline " fmt "\n", ##__VA_ARGS__); \
		return -EOPNOTSUPP;					\
	} while (0)

/*
 * A flow as this driver needs it, gathered from ONE tc-flower rule.
 *
 * The kernel hands NAT flows to hardware as tc-flower rules on an ingress
 * block: match keys describe the packet as it arrives, and the action list
 * describes the rewrite. Everything the ASIC row needs is in there --
 * the masquerade mapping is the MANGLE actions, not a second tuple -- except
 * the LAN client's MAC, which is looked up from the neighbour table because
 * only the opposite direction's rule carries it.
 */
struct hwnat_rule {
	u32			int_ip, rem_ip, ext_ip;
	u16			int_port, rem_port, gport;
	u8			is_tcp;
	bool			wan_pppoe;
	u16			pppoe_sid;
	u8			wan_peer_mac[ETH_ALEN];
	u8			lan_peer_mac[ETH_ALEN];
	struct net_device	*in_dev;	/* LAN ingress (DSA user port) */
	struct net_device	*out_dev;	/* WAN egress */
};

static int hwnat_add_flow(const struct hwnat_rule *r, unsigned long cookie)
{
	u32 int_ip = r->int_ip, rem_ip = r->rem_ip, ext_ip = r->ext_ip;
	u16 int_port = r->int_port, rem_port = r->rem_port, gport = r->gport;
	u32 is_tcp = r->is_tcp, wan_ip;
	bool wan_pppoe = r->wan_pppoe;
	u16 idx_out, idx_in;
	struct hwnat_slot *slot;
	int rc;

	if (!READ_ONCE(rtl819x_hwnat_enabled))	/* runtime gate (also guards a stale ADD) */
		return -EOPNOTSUPP;

	/*
	 * The masquerade IP is DYNAMIC -- it must equal the WAN interface's
	 * CURRENT primary IPv4 (ppp0's local address on a PPPoE WAN, assigned at
	 * IPCP-up and different every reconnect; a static address on an ethernet
	 * WAN). Masquerade always sources from the egress interface's address, so
	 * a mismatch means the address moved under the flow: decline, the flow is
	 * dying with the old address anyway.
	 */
	wan_ip = hwnat_wan_ipv4(dev_net(r->out_dev), r->out_dev->ifindex);
	if (!wan_ip || ext_ip != wan_ip)
		HWNAT_DECLINE("add: extIP %pI4h != live WAN IP %pI4h (oif=%s)",
			      &ext_ip, &wan_ip, r->out_dev->name);

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
	idx_out = gw_napt_hash1(is_tcp, int_ip, int_port, rem_ip, rem_port);
	/* Return-path row: the ASIC hashes the INBOUND packet {src=rem, dst=ext:G} to
	 * locate the reverse row, so it must live at hash(remIP,remPort,extIP,G) -- NOT at
	 * G&0x3ff (that full-cone shortcut is not where the silicon looks). Vendor
	 * l4Driver/rtl865x_nat.c:715: in = naptTcpUdpTableIndex(htonl(remIp),htons(remPort),
	 * htonl(extIp),extPort), extPort=htons(G); network order as everywhere. Without it,
	 * WAN->LAN replies miss + trap to CPU (NAPTR_NOT_FOUND_DROP=0): the flow still works
	 * but the return half is never HW-accelerated (and the old G&0x3ff row aliases an
	 * unrelated inbound slot). */
	idx_in  = gw_napt_hash1(is_tcp, rem_ip, rem_port, ext_ip, gport);

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
	/* And the WAN NETIF MAC itself: eth0.1 is destroyed/recreated on ifdown/ifup,
	 * and a gw_prog run in the down-window programs netif[1] from a stale shadow.
	 * The nexthop resync below never catches that (it keys on the PEER MAC), so
	 * compare the live netdev MAC here and reprogram + flush on divergence. */
	if (rtl865x_wan_netif_mac_sync() > 0)
		hwnat_flush_locked();
	rc = rtl865x_wan_set_nexthop(r->wan_peer_mac, wan_pppoe,
				     wan_pppoe ? r->pppoe_sid : 0);
	if (rc < 0)
		goto out_unlock;
	if (rc > 0)
		hwnat_flush_locked();

	/* Same for the LAN side: the client's MAC comes from the SRC path's
	 * eth_dest (the address the reply direction egresses to) and its IP is the
	 * flow's internal address. Without this the ASIC kept a build-time constant
	 * -- one bench machine's MAC -- so the reply direction had no valid L2 and
	 * the flow degraded to CPU forwarding on any real network. */
	rc = rtl865x_lan_set_nexthop(r->lan_peer_mac, int_ip);
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
		if (hwnat_debug)
			pr_info_ratelimited("rtl819x hwnat: decline add: rows busy out@%u(%d) in@%u(%d)%s\n",
				idx_out, !!test_bit(idx_out, hwnat_used),
				idx_in, !!test_bit(idx_in, hwnat_used),
				idx_out == idx_in ? " SAME-INDEX" : "");
		goto out_unlock;
	}

	rc = hwnat_program_rows(is_tcp, int_ip, int_port, gport, rem_ip, rem_port, idx_out, idx_in);
	if (rc)
		goto out_unlock;

	__set_bit(idx_out, hwnat_used);
	__set_bit(idx_in, hwnat_used);

	slot->cookie_out = cookie;
	slot->cookie_in  = 0;			/* filled in when the reply rule arrives */
	slot->int_ip     = int_ip;
	slot->int_port   = int_port;
	slot->rem_ip     = rem_ip;
	slot->rem_port   = rem_port;
	slot->ext_ip     = ext_ip;
	slot->gport      = gport;
	slot->idx_out    = idx_out;
	slot->idx_in     = idx_in;
	slot->last_aging = RTL865X_NAPT_AGING_RELOAD;
	slot->is_tcp     = is_tcp;
	list_add_tail(&slot->list, &hwnat_flows);

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


static int hwnat_del_cookie(unsigned long cookie)
{
	struct hwnat_slot *slot, *tmp;
	int found = 0;

	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry_safe(slot, tmp, &hwnat_flows, list) {
		if (slot->cookie_out != cookie && slot->cookie_in != cookie)
			continue;
		rtl865x_napt_clear(slot->idx_out);
		rtl865x_napt_clear(slot->idx_in);
		__clear_bit(slot->idx_out, hwnat_used);
		__clear_bit(slot->idx_in, hwnat_used);
		list_del(&slot->list);
		kfree(slot);
		found++;
	}
	mutex_unlock(&rtl865x_hal_lock);

	return found ? 0 : -ENOENT;
}

/* ------------------------------------------------------------- tc-flower front end */

static void hwnat_mangle_eth(const struct flow_action_entry *act, struct ethhdr *eth)
{
	void *dest = (void *)eth + act->mangle.offset;
	const void *src = &act->mangle.val;

	if (act->mangle.offset > 8)
		return;
	if (act->mangle.mask == 0xffff) {
		src += 2;
		dest += 2;
	}
	memcpy(dest, src, act->mangle.mask ? 2 : 4);
}

/*
 * Resolve the LAN client's MAC.
 *
 * Only the reply-direction rule carries it (as its ethernet MANGLE), and the
 * two directions are offered independently, so rather than make row
 * installation wait on rule ordering, ask the neighbour table: the client is by
 * definition actively sending on this flow, so its entry is present and fresh.
 */
static int hwnat_lan_mac(struct net_device *dev, u32 int_ip_host, u8 *mac)
{
	__be32 ip = htonl(int_ip_host);
	struct net_device *l3dev;
	struct neighbour *n;
	int rc = -ENOENT;

	/*
	 * The rule names the DSA user port the packet arrived on, but ARP is
	 * resolved on the L3 device -- br-lan -- and that is where the entry
	 * lives. Looking it up on lan2 itself finds nothing, every flow declines,
	 * and the offload silently never engages.
	 */
	rcu_read_lock();
	l3dev = netdev_master_upper_dev_get_rcu(dev);
	if (l3dev)
		dev_hold(l3dev);
	rcu_read_unlock();
	if (!l3dev) {
		l3dev = dev;
		dev_hold(l3dev);
	}

	n = neigh_lookup(&arp_tbl, &ip, l3dev);
	dev_put(l3dev);
	if (!n)
		return -ENOENT;

	read_lock_bh(&n->lock);
	if (n->nud_state & NUD_VALID) {
		ether_addr_copy(mac, n->ha);
		rc = 0;
	}
	read_unlock_bh(&n->lock);
	neigh_release(n);

	return rc;
}

static int hwnat_flow_replace(struct flow_cls_offload *cls)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_control control;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_meta meta;
	struct flow_action_entry *act;
	struct hwnat_slot *slot;
	struct hwnat_rule r = {};
	struct ethhdr eth = {};
	struct net_device *idev;
	u32 msrc_ip, mdst_ip;
	u16 msrc_port, mdst_port;
	__be32 nat_src = 0, nat_dst = 0;
	__be16 nat_sport = 0, nat_dport = 0;
	bool have_redirect = false;
	int i, rc;

	if (!READ_ONCE(rtl819x_hwnat_enabled))
		return -EOPNOTSUPP;

	/* Ingress device: the DSA user port the client is on. */
	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		HWNAT_DECLINE("replace: no META key");
	flow_rule_match_meta(rule, &meta);
	if (!meta.key->ingress_ifindex)
		HWNAT_DECLINE("replace: no ingress ifindex");

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		flow_rule_match_control(rule, &control);
		if (control.key->flags & FLOW_DIS_IS_FRAGMENT)
			HWNAT_DECLINE("replace: fragmented flow");
	}

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC))
		HWNAT_DECLINE("replace: no BASIC key");
	flow_rule_match_basic(rule, &basic);
	if (basic.key->n_proto != htons(ETH_P_IP))
		HWNAT_DECLINE("replace: n_proto %04x not IPv4", ntohs(basic.key->n_proto));
	if (basic.key->ip_proto == IPPROTO_TCP)
		r.is_tcp = 1;
	else if (basic.key->ip_proto == IPPROTO_UDP)
		r.is_tcp = 0;
	else
		HWNAT_DECLINE("replace: ip_proto %u not TCP/UDP", basic.key->ip_proto);

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS))
		HWNAT_DECLINE("replace: missing address/port keys");
	flow_rule_match_ipv4_addrs(rule, &addrs);
	flow_rule_match_ports(rule, &ports);

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				hwnat_mangle_eth(act, &eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				if (act->mangle.offset == offsetof(struct iphdr, saddr))
					memcpy(&nat_src, &act->mangle.val, 4);
				else if (act->mangle.offset == offsetof(struct iphdr, daddr))
					memcpy(&nat_dst, &act->mangle.val, 4);
				else
					HWNAT_DECLINE("replace: ip mangle off %u",
						      act->mangle.offset);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP: {
				u32 val = ntohl(act->mangle.val);

				if (act->mangle.offset == 0) {
					if (act->mangle.mask == ~htonl(0xffff))
						nat_dport = cpu_to_be16(val);
					else
						nat_sport = cpu_to_be16(val >> 16);
				} else if (act->mangle.offset == 2) {
					nat_dport = cpu_to_be16(val);
				} else {
					HWNAT_DECLINE("replace: l4 mangle off %u",
						      act->mangle.offset);
				}
				break;
			}
			default:
				HWNAT_DECLINE("replace: mangle htype %u", act->mangle.htype);
			}
			break;
		case FLOW_ACTION_REDIRECT:
			r.out_dev = act->dev;
			have_redirect = true;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			r.wan_pppoe = true;
			r.pppoe_sid = act->pppoe.sid;
			break;
		default:
			/* VLAN push/pop and anything else: leave it in software. */
			HWNAT_DECLINE("replace: action id %u unsupported", act->id);
		}
	}

	if (!have_redirect || !r.out_dev)
		HWNAT_DECLINE("replace: no redirect action");

	msrc_ip   = ntohl(addrs.key->src);
	mdst_ip   = ntohl(addrs.key->dst);
	msrc_port = ntohs(ports.key->src);
	mdst_port = ntohs(ports.key->dst);

	/*
	 * Is this the reply half of a flow whose rows are already installed?
	 *
	 * A bidirectional flow is offered as two independent rules and BOTH must
	 * be accepted or the kernel keeps the whole flow in software. The reply
	 * rule un-NATs, so it rewrites the DESTINATION -- which the shape check
	 * below rejects. Match it against the installed pair FIRST: its match
	 * tuple is {remote -> external:G}, the exact mirror of what we recorded.
	 * (nf_flow_table offers the original direction first, so by the time the
	 * reply arrives the pair exists.)
	 */
	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry(slot, &hwnat_flows, list) {
		if (slot->is_tcp != r.is_tcp)
			continue;
		if (slot->rem_ip != msrc_ip || slot->rem_port != msrc_port ||
		    slot->ext_ip != mdst_ip || slot->gport != mdst_port)
			continue;
		slot->cookie_in = cls->cookie;
		mutex_unlock(&rtl865x_hal_lock);
		return 0;
	}
	mutex_unlock(&rtl865x_hal_lock);

	/*
	 * Masquerade shape only: the source address and port are rewritten and the
	 * destination is not. A DNAT/port-forward rewrites the destination, and the
	 * ASIC row pair this driver programs cannot express that, so it stays in
	 * software.
	 */
	if (nat_dst || nat_dport)
		HWNAT_DECLINE("replace: destination NAT not supported");
	if (!nat_src || !nat_sport)
		HWNAT_DECLINE("replace: not a source-NAT rule");

	r.int_ip   = msrc_ip;
	r.rem_ip   = mdst_ip;
	r.int_port = msrc_port;
	r.rem_port = mdst_port;
	r.ext_ip   = ntohl(nat_src);
	r.gport    = ntohs(nat_sport);
	ether_addr_copy(r.wan_peer_mac, eth.h_dest);

	if (is_zero_ether_addr(r.wan_peer_mac))
		HWNAT_DECLINE("replace: no WAN next-hop MAC in the rule");

	idev = dev_get_by_index(dev_net(r.out_dev), meta.key->ingress_ifindex);
	if (!idev)
		HWNAT_DECLINE("replace: ingress ifindex %u is gone",
			      meta.key->ingress_ifindex);
	r.in_dev = idev;

	rc = hwnat_lan_mac(idev, r.int_ip, r.lan_peer_mac);
	if (rc) {
		dev_put(idev);
		HWNAT_DECLINE("replace: no neighbour entry for %pI4h on %s",
			      &r.int_ip, idev->name);
	}

	rc = hwnat_add_flow(&r, cls->cookie);
	dev_put(idev);

	return rc;
}

static int hwnat_flow_stats(struct flow_cls_offload *cls)
{
	struct asic_napt_tcpudp e;
	struct hwnat_slot *slot;
	u64 lastused = 0;

	/*
	 * The ASIC keeps no per-flow byte or packet counters, only a 6-bit aging
	 * counter that reloads on a hit. Report activity through lastused and
	 * leave the counters at zero -- which is why conntrack accounting freezes
	 * on an offloaded flow, exactly as it did on the 4.14 port.
	 */
	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry(slot, &hwnat_flows, list) {
		if (slot->cookie_out != cls->cookie && slot->cookie_in != cls->cookie)
			continue;
		if (!rtl865x_napt_read(slot->idx_out, &e) && e.valid &&
		    (e.agingTime >= RTL865X_NAPT_AGING_RELOAD ||
		     e.agingTime > slot->last_aging))
			lastused = jiffies;
		break;
	}
	mutex_unlock(&rtl865x_hal_lock);

	flow_stats_update(&cls->stats, 0, 0, 0, lastused, FLOW_ACTION_HW_STATS_IMMEDIATE);

	return 0;
}

static int hwnat_flow_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return hwnat_flow_replace(cls);
	case FLOW_CLS_DESTROY:
		return hwnat_del_cookie(cls->cookie);
	case FLOW_CLS_STATS:
		return hwnat_flow_stats(cls);
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(hwnat_block_cb_list);

/*
 * fw4 emits `flowtable { devices = { lan1..lan4, wan }; flags offload; }`, and
 * DSA forwards the resulting block bind from each user port to this conduit, so
 * all five binds land on one netdev and share a single callback.
 */
static int hwnat_setup_tc_block(struct net_device *dev, struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &hwnat_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, hwnat_flow_block_cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(hwnat_flow_block_cb, dev, dev, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);

		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &hwnat_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, hwnat_flow_block_cb, dev);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data)
{
	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	return hwnat_setup_tc_block(dev, type_data);
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
