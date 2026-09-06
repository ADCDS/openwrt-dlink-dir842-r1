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
 * One installed hardware flow. idx_out/idx_in are the two ASIC NAPT row indices
 * we own for this flow.
 *
 * ★ Corrected 2026-09-04 — this comment used to describe a `flow`/`orig_tuple`
 * pair and a "Phase-3 step 4" UAF-safe flow_offload_lookup()-based timeout
 * refresh, neither of which exists in this struct or file any more: they were
 * part of the pre-TC_SETUP_FT downstream xt_FLOWOFFLOAD handshake (see
 * `main:files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/
 * rtl819x_hwnat.c`'s hwnat_refresh_timeout()) and were removed by the
 * ndo_flow_offload -> TC_SETUP_FT migration without the comment being updated
 * — which itself hid a real bug (fixed the same day: see hwnat_flow_stats()
 * below). The refresh job mainline actually expects is FLOW_CLS_STATS, i.e.
 * hwnat_flow_stats(): the kernel's own nf_flow_table core polls it and pushes
 * the flow's software timeout out based on what it reports, roughly once a
 * flow's remaining time drops under ~90% of its offload timeout. There is no
 * driver-side push mechanism to reimplement; hwnat_aging_work_fn()'s "push its
 * software GC timeout out" comment below is the same kind of stale claim —
 * that function's poll is diagnostic only (the AGE log line, and reaping a
 * slot whose row the ASIC already auto-deleted), not a keepalive path.
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
	u8				last_aging;	/* last idx_out agingTime seen by the worker */
	u8				last_aging_in;	/* last idx_in  agingTime seen by the worker */
	u8				is_tcp;
};
/* All of the following are protected by rtl865x_hal_lock. */
static LIST_HEAD(hwnat_flows);
static DECLARE_BITMAP(hwnat_used, RTL865X_NAPT_ROWS);	/* rows this module owns */
static bool hwnat_active;				/* datapath up (open) & armed */
static bool hwnat_flow_hot;	/* >=1 installed flow's ASIC row was hit within the last
				 * aging poll -- see rtl819x_hwnat_has_hot_flow() below and
				 * its comment in rtl819x_hwnat.h. */
static bool hwnat_any_installed;	/* >=1 flow currently has ASIC rows installed. Set the
				 * instant a slot is added (true from packet 1), cleared only
				 * when the flow list empties -- see rtl819x_hwnat_any_flow_
				 * installed(). Unlike hwnat_flow_hot (which needs a 5s aging
				 * poll to observe a HIT) this closes the timing gap where a
				 * just-installed or stalled-before-hot flow is momentarily
				 * invisible to the LARGE-FRAME WEDGE detector's suppression. */

/* Recompute the lock-free hwnat_any_installed snapshot from the actual list.
 * Self-correcting (no refcount drift): call under rtl865x_hal_lock after ANY
 * mutation of hwnat_flows. */
static void hwnat_installed_update(void)
{
	WRITE_ONCE(hwnat_any_installed, !list_empty(&hwnat_flows));
}

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
	hwnat_installed_update();
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

/* ★ 2026-09-04: forward-only ASIC offload. The ASIC reverse (WAN->LAN) NAPT row
 * MATCHES (its aging pins hot) but was dual-capture-measured to DROP ~50% of return
 * frames on delivery to the LAN jack (peer sent 35, LAN client received 18), which
 * stalls the forward transfer (client stuck on a stale zero-window). With this set,
 * we install ONLY the outbound row and clear the inbound slot, so WAN->LAN un-NAT
 * falls to the software path (NEIGH) -- forward stays silicon-accelerated (bulk),
 * reverse is just low-volume ACKs on the CPU. Default 0 = install both rows. */
static int napt_no_reverse;
module_param(napt_no_reverse, int, 0644);
MODULE_PARM_DESC(napt_no_reverse, "1 = skip the ASIC reverse (idx_in) row; WAN->LAN un-NAT falls to software (default 0 = install both)");

/* ★ 2026-09-05: the NAPT row's priority field (asic_napt_tcpudp.priValid/.priority,
 * rtl865x_asichal.h:75) was never set anywhere in this driver -- every ASIC-accelerated
 * row installs with priValid=0 (priority explicitly marked not-in-use). Found live on
 * hardware: a bulk LAN->WAN transfer under hw=1 sends an initial burst far faster than
 * the software-forwarding path ever produces (hardware forwarding's near-zero added
 * latency lets the sender's TCP window ramp up explosively during slow-start -- captured
 * at the peer: 515 packets in ~40ms), and a SPECIFIC segment within that burst is
 * silently dropped and every retransmission of that exact same segment ALSO fails,
 * repeatedly, permanently stalling the connection even though hundreds of KB on either
 * side of it were delivered fine. That is the signature of an unprioritized flow losing
 * arbitration at a shared, momentarily-congested egress queue (this SoC's port0 RGMII
 * trunk carries every LAN<->WAN routed flow, documented elsewhere in this driver as the
 * congestion point for exactly this kind of burst) -- not random loss, since the SAME
 * priority class would lose the SAME arbitration on every retry. Default 0 keeps the
 * previous (unset) behavior for a quick A/B revert; positive values are written into the
 * row's `priority` field (masked to the real 3-bit hardware range) with `priValid=1`. */
static int napt_priority = 7;
module_param(napt_priority, int, 0644);
MODULE_PARM_DESC(napt_priority, "NAPT row priValid/priority (0-7, hardware ceiling): 0 or negative = leave priValid unset (previous/default driver behavior); >0 = mark the row priValid=1 with this priority (default 7, highest)");

static int hwnat_program_rows(u32 is_tcp, u32 int_ip, u16 int_port, u16 gport,
			      u32 rem_ip, u16 rem_port, u16 idx_out, u16 idx_in)
{
	/* The outbound row encodes the internal tuple + ext mapping; the inbound row is
	 * NOT a copy of it (see below) — it re-encodes G differently and stores a
	 * remote-endpoint VERIFICATION hash the ASIC checks on every return packet. */
	struct asic_napt_tcpudp e, v;
	int rc;

	/* ★ KEY BYTE ORDER — settled: HOST order (numeric), napt_key_htonl default 0.
	 * ⚠ An earlier note here read the napt_fill_all reason=7 as "proof the key byte-order
	 * is wrong." That is now known to be a CONFOUNDED reading (see the napt_fill_all
	 * comment below): under the ASIC's actual 4-way+enhanced-hash SWTCR1, filling all
	 * indices violates hash-consistency, so a persistent reason=7 does not prove the key
	 * is rejected. Independent, stronger evidence says the key/hash are CORRECT: they are
	 * byte-identical to the working 4.14 driver and the ASIC was observed matching a
	 * driver-installed row (reverse hwFwd=1, docs §4). Do not re-swap the byte order;
	 * reason=7 is an aging/teardown + DSA-context problem, not a key-encoding bug. */
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
	if (napt_priority > 0) {
		e.priValid = 1;
		e.priority = napt_priority & 0x7;
	}

	/* ★ DIAGNOSTIC: napt_fill_all writes the OUTBOUND row at EVERY index, meant to
	 * separate index-fault from key-fault for the "no matched NAPT entry" trap (reason 7):
	 *   - trap DISAPPEARS => key/content is fine, our INDEX derivation was wrong;
	 *   - trap PERSISTS   => hardware reaches a row and rejects the KEY.
	 * ⚠ 2026-09-04 CONFOUND — this discriminator is NOT valid under the SWTCR1 this ASIC
	 * actually runs (0x2200 = EnL4WayH=1 4-way + L4EnHash1=1 enhanced-hash). Under 4-way +
	 * enhanced-hash the silicon very likely VERIFIES hash-consistency (a matched row must
	 * sit at ITS OWN hash index). Filling all 1024 indices deliberately violates that at
	 * 1023 of them, so a persistent reason=7 could mean "row is not at its own hash index"
	 * rather than "key fields wrong" — the two are no longer separated. The clean form of
	 * this test needs a flat 1-way table (EnL4WayH=0), which clearing wedged the L4
	 * datapath. Independent evidence says the key is NOT the fault anyway: the hash is
	 * byte-identical to the working 4.14 driver and the ASIC was observed matching a
	 * driver-installed row (reverse hwFwd=1). Treat reason=7 as an aging/teardown +
	 * DSA-datapath-context problem, not an index/key encoding bug. See docs §4.
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

	if (napt_no_reverse) {
		/* Forward-only ASIC: clear the inbound slot so the ASIC has NO reverse
		 * match there (a stale/prefill row could otherwise false-hit), and let
		 * WAN->LAN un-NAT fall to the software path. See the param comment above. */
		rtl865x_napt_clear(idx_in);
		if (rtl865x_napt_read(idx_out, &v) || !v.valid || v.intIPAddr != int_ip) {
			rtl865x_napt_clear(idx_out);
			return -EIO;
		}
		return 0;
	}

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
	/* priValid/priority carries over from the memset+outbound-row setup above (this
	 * struct is reused, not re-zeroed) whenever napt_priority>0 -- the reverse/ACK row
	 * needs to keep pace with a now-larger forward burst too, not just the outbound row. */

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

	/* ★ The ASIC keys and hashes on NUMERIC (host-order) values — proven from the
	 * stock binary disassembly (docs/HWNAT-OFFLOAD.md §4: `801ae9d4 ror ...` == an
	 * ntohl the vendor applies so its OWN host-order naptEntry matches the on-wire
	 * frame; net effect = numeric/host order end to end). int_ip/int_port arrived here
	 * already host-order (ntohl/ntohs at the FLOW_CLS_REPLACE parse, see the
	 * napt_key_htonl note above), the stored row key is the same host-order value
	 * (hwnat_program_rows: e.intIPAddr = int_ip), and the hash index below is computed
	 * from the same. The A/B `napt_key_htonl` param defaults OFF (host order) because
	 * that is what was measured to work.
	 * ⚠ An earlier version of THIS comment said "feed NETWORK order" — that was a
	 * REFUTED hypothesis (docs/HWNAT-OFFLOAD.md §11.1, RETRACTIONS #21). The index is
	 * NOT the cause of reason=7: the ASIC has been observed matching a row installed at
	 * exactly this index on real silicon (hwFwd=1 reason=0000). reason=7 is an
	 * aging/teardown artifact, not a placement bug. Do not re-swap the byte order. */
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
	/* And the WAN NETIF MAC itself: rtl865x_wan_netif_mac_sync() heals netif[1]
	 * against a stale shadow of the "wan" netdev's own MAC (see that function's
	 * header comment in rtl865x_asichal.c for the corrected, DSA-accurate
	 * rationale — an earlier version of this comment named the 4.14/swconfig
	 * "eth0.1" VLAN subinterface, which does not exist on this port). The
	 * nexthop resync below never catches this on its own (it keys on the PEER
	 * MAC, not our own), so compare the live netdev MAC here and reprogram +
	 * flush on divergence. */
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
	slot->last_aging_in = RTL865X_NAPT_AGING_RELOAD;
	slot->is_tcp     = is_tcp;
	list_add_tail(&slot->list, &hwnat_flows);
	hwnat_installed_update();	/* true from packet 1 -> wedge detector sees the flow */

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
		/* M5 diag (2026-09-04): was silent. Logged so a bench pass can tell a
		 * genuine ifdown/flush-driven DEL apart from the kernel core reaping a
		 * flow FLOW_CLS_STATS just under-reported -- see the long comment in
		 * hwnat_flow_stats() above. A DEL landing here seconds after this same
		 * idx_out/idx_in pair showed real hwFwd=1 traffic is the signature of
		 * the bug that comment describes; one that never did is ordinary GC. */
		pr_info_ratelimited("rtl819x hwnat: -%s idx_out=%u idx_in=%u cookie=%lu\n",
				     slot->is_tcp ? "tcp" : "udp", slot->idx_out,
				     slot->idx_in, cookie);
		rtl865x_napt_clear(slot->idx_out);
		rtl865x_napt_clear(slot->idx_in);
		__clear_bit(slot->idx_out, hwnat_used);
		__clear_bit(slot->idx_in, hwnat_used);
		list_del(&slot->list);
		kfree(slot);
		found++;
	}
	hwnat_installed_update();
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
	struct asic_napt_tcpudp e, ein;
	struct hwnat_slot *slot;
	u64 lastused = 0;

	/*
	 * The ASIC keeps no per-flow byte or packet counters, only a 6-bit aging
	 * counter that reloads on a hit. Report activity through lastused and
	 * leave the counters at zero -- which is why conntrack accounting freezes
	 * on an offloaded flow, exactly as it did on the 4.14 port.
	 *
	 * ★ M5 fix (2026-09-04, bench-confirmed): this used to check idx_out ONLY.
	 * This is the sole path that keeps mainline's nf_flow_table core from
	 * expiring an offloaded flow -- hwnat_aging_work_fn()'s own 5s poll reads
	 * BOTH rows and logs them ("hwnat AGE out[]=.. in[]=.." below) but has no
	 * mechanism of its own to push the kernel's flow->timeout out; the comment
	 * there claiming it does predates the mainline TC_SETUP_FT rework and is
	 * stale (the old xt_FLOWOFFLOAD-era hwnat_refresh_timeout() it describes
	 * was removed with the ndo_flow_offload -> TC_SETUP_FT migration and never
	 * replaced). So FLOW_CLS_STATS reporting idx_out-only silently starved
	 * every flow whose *inbound* row was the one taking hits: nf_flow_offload_
	 * gc_step() sees stale lastused, expires the flow, and our own
	 * hwnat_del_cookie() wipes both rows out from under still-live hardware
	 * forwarding. Measured on the bench: sustained LAN->WAN bulk data hashes
	 * to idx_out and never once showed hwFwd=1, while idx_in (small return
	 * traffic) DID take real hardware hits (hwFwd=1, isOrig=0 -- a genuine
	 * ASIC-forwarded copy) for a ~4s window, then the whole flow reverted to
	 * 100% software-path misses and reinstalled at the SAME indices 99s
	 * later -- exactly the signature of the kernel core reaping a flow this
	 * function had wrongly reported as idle. Check either row; a hit on
	 * either direction is real evidence the flow is alive in silicon.
	 */
	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry(slot, &hwnat_flows, list) {
		bool out_hot, in_hot;

		if (slot->cookie_out != cls->cookie && slot->cookie_in != cls->cookie)
			continue;
		out_hot = !rtl865x_napt_read(slot->idx_out, &e) && e.valid &&
			  (e.agingTime >= RTL865X_NAPT_AGING_RELOAD ||
			   e.agingTime > slot->last_aging);
		in_hot = !rtl865x_napt_read(slot->idx_in, &ein) && ein.valid &&
			 ein.agingTime >= RTL865X_NAPT_AGING_RELOAD;
		if (out_hot || in_hot)
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
 * Runs every HWNAT_AGING_INTERVAL in process context. This is DIAGNOSTIC ONLY —
 * read each row's live agingTime (which ticks because TEACR L4-aging is on), log
 * both rows' ages, and reap rows the ASIC auto-deleted at age-0 (see the comment
 * at that branch below). It does NOT keep a flow's software timeout alive; the
 * `active` counter it computes feeds nothing but a pr_debug summary.
 *
 * ★ Corrected 2026-09-04: this comment used to describe a "REFRESH (step 4)"
 * job — hwnat_refresh_timeout() pushing the software flow's GC timeout out via
 * a UAF-safe orig_tuple/flow_offload_lookup(), gated on an xt_FLOWOFFLOAD
 * handshake. None of that exists in this file (it belonged to the pre-
 * TC_SETUP_FT downstream ndo_flow_offload mechanism 4.14 still uses — see
 * `main:.../rtl819x_hwnat.c`'s hwnat_refresh_timeout()) and the comment
 * describing it as still-functional-but-a-no-op was itself wrong: it hid the
 * fact that NOTHING refreshes the kernel's flow timeout except
 * hwnat_flow_stats() (FLOW_CLS_STATS), whose idx_out-only check was starving
 * every flow whose hardware hits landed on idx_in — see the fix and full
 * account in hwnat_flow_stats() above. Fixed the same day.
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
			bool in_hot = false, out_hot = false;

			if (!rtl865x_napt_read(slot->idx_in, &ein) && ein.valid) {
				age_in = ein.agingTime;
				/* ★ 2026-09-04: the SAME idx_out-vs-idx_in blind spot that was
				 * fixed in hwnat_flow_stats() (FLOW_CLS_STATS) also lived HERE, in
				 * the poll that computes hwnat_flow_hot -- the ONLY signal the
				 * LARGE-FRAME WEDGE detector uses to suppress its false-positive
				 * fabric+NAPT-table reset. Bench-measured: a sustained LAN->WAN bulk
				 * flow's hardware hits land on idx_in (small return traffic), while
				 * idx_out shows cold. Counting idx_out ONLY reported hwnat_flow_hot
				 * = false for exactly the flow being genuinely hardware-forwarded,
				 * so the wedge detector fired and level-3-recovery WIPED the live
				 * NAPT table mid-flow. Count EITHER row hot -- same contract as
				 * hwnat_flow_stats()'s out_hot||in_hot. */
				if (age_in >= RTL865X_NAPT_AGING_RELOAD ||
				    age_in > slot->last_aging_in)
					in_hot = true;
				slot->last_aging_in = age_in;
			}
			pr_err("hwnat AGE out[%u]=%u in[%u]=%u\n",
			       slot->idx_out, e.agingTime, slot->idx_in, age_in);

			out_hot = (e.agingTime >= RTL865X_NAPT_AGING_RELOAD ||
				   e.agingTime > slot->last_aging);
			/* Either row hot => the flow is live in silicon. hwnat_flow_hot
			 * only cares that active > 0, so counting a both-rows-hot slot once
			 * is all that matters (it is the LARGE-FRAME WEDGE detector's sole
			 * suppression signal, see rtl819x_hwnat_has_hot_flow() / .h). This
			 * poll does not and cannot push any kernel-side GC timeout; that is
			 * hwnat_flow_stats()'s job. */
			if (out_hot || in_hot)
				active++;
		}
		slot->last_aging = e.agingTime;
	}
	hwnat_installed_update();	/* the reap loop above may have emptied the list */
	mutex_unlock(&rtl865x_hal_lock);

	/* Snapshot for rtl819x_hwnat_has_hot_flow() -- see rtl819x_hwnat.h. Plain
	 * WRITE_ONCE, no lock: an off-by-one-poll-interval staleness on this value
	 * is harmless (the wedge check only needs "was something hot recently"),
	 * matching the READ_ONCE(rtl819x_hwnat_enabled) style already used here. */
	WRITE_ONCE(hwnat_flow_hot, active > 0);

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

	/* Every row is gone (hwnat_flush_locked() above); nothing can be hot now. Set
	 * this AFTER the cancel_sync so a poll that was already in flight can't race
	 * back in and re-set it from its own (now-stale) snapshot. */
	WRITE_ONCE(hwnat_flow_hot, false);
}

bool rtl819x_hwnat_has_hot_flow(void)
{
	return READ_ONCE(hwnat_flow_hot);
}

bool rtl819x_hwnat_any_flow_installed(void)
{
	return READ_ONCE(hwnat_any_installed);
}
