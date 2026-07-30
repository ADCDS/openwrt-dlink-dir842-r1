/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek RTL8197F ASIC L3/NAPT hardware-NAT table engine — minimal HAL.
 *
 * The RTL8197F "rome" fast-path does WAN/LAN routing + NAPT in silicon via a
 * set of indirect ASIC tables (ARP / L3-route / nexthop / NAPT). This HAL is a
 * clean re-implementation of the table-access engine and the NAPT entry format,
 * reverse-engineered from the stock 3.10.90 kernel (see m6.6-hwnat/ASIC-ENGINE.md)
 * and cross-validated field-for-field against the Realtek vendor SDK
 * (AsicDriver/rtl865x_asic{Basic,L3,L4}.{c,h}). It is the M6.6 foundation: a
 * real (and gigabit) gateway on this SoC requires HW NAT because a software
 * WAN/LAN split is impossible here (single RGMII cascade, see HANDOFF §5).
 *
 * Entry structs are the vendor little-endian branch VERBATIM — the LE compiler
 * reproduces the exact hardware bit layout the stock disassembly showed.
 */
#ifndef _RTL865X_ASICHAL_H
#define _RTL865X_ASICHAL_H

#include <linux/types.h>
#include <linux/mutex.h>

/* M6.6 gateway datapath constants shared between the HAL (rtl865x_asichal.c, which
 * programs the static per-gateway scaffolding) and the conntrack hardware-NAT glue
 * (rtl819x_hwnat.c, which writes per-flow NAPT rows). Single source of truth so the
 * two translation units can never disagree on the VLAN/masquerade identity. */
#define RTL865X_VID_WAN		1		/* 802.1Q VID the ASIC treats as WAN */
#define RTL865X_VID_LAN		2		/* 802.1Q VID the ASIC treats as LAN */
#define RTL865X_WAN_EXTIP	0xAC100001	/* 172.16.0.1 = BOOT-DEFAULT masquerade IP (extIP[0]);
						 * M7.2: the LIVE value is rtl865x_wan_extip — dynamic,
						 * = the WAN (ppp0) local IPv4, learned per-flow */
#define RTL865X_NAPT_ROWS	1024		/* flat 1-way L4 table depth (SWTCR1 EnL4WayH=0) */
#define RTL865X_PPPOE_TBL_SIZE	8		/* type-11 PPPoE session table depth (vendor RTL8651_PPPOETBL_SIZE) */
/* 6-bit "differentiated timer" reload value written to every TEATCR proto field and
 * to a freshly-added row's agingTime. 0x11 ≈ 102 s of idle life before the ASIC
 * auto-clears the row's valid bit (per vendor _rtl8651_NaptAgingToSec). The hwnat
 * aging worker also treats it as the ceiling: agingTime at/above it == recently
 * reloaded by traffic == the flow is active. Single source shared by the HAL (which
 * programs TEATCR) and the worker (which reads it back). */
#define RTL865X_NAPT_AGING_RELOAD	0x11

/* ASIC table type ids (enum TYPE_* from vendor asicBasic.h). Cross-validated
 * against the stock per-type word-count table @ VA 0x8050e590. */
#define ASIC_TYPE_L2_SWITCH	0
#define ASIC_TYPE_ARP		1
#define ASIC_TYPE_L3_ROUTING	2
#define ASIC_TYPE_MULTICAST	3
#define ASIC_TYPE_NETIF		4
#define ASIC_TYPE_VLAN		6
#define ASIC_TYPE_L4_TCP_UDP	9
#define ASIC_TYPE_PPPOE		11
#define ASIC_TYPE_ACL_RULE	12
#define ASIC_TYPE_NEXT_HOP	13

/*
 * NAPT TCP/UDP hardware entry (type 9, 1024 entries, 3 meaningful words of a
 * 32-byte stride). Little-endian branch from vendor rtl865x_asicL4.h, RTL8197F.
 * Bit layout confirmed against stock disasm of rtl8651_setAsicNaptTcpUdpTable
 * (word1 starts 0x4002 => collision(b1)=collision2(b14)=1; word2 has the
 * 8197F-only NHIDX/NHIDXValid at bits 24..29).
 */
struct asic_napt_tcpudp {
	u32 intIPAddr;						/* word0 */
	/* word1 */
	u32 valid:1, collision:1, agingTime:6, offset:6,
	    collision2:1, dedicate:1, isStatic:1, selIPIdx:4,
	    selEIdx:10, reserv0:1;
	/* word2 (RTL8197F) */
	u32 intPort:16, TCPFlag:3, isTCP:1, priValid:1, priority:3,
	    NHIDXValid:1, NHIDX:5, reserv2:2;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3 route entry (type 2, 8 entries, 3 words) — "L2/direct" (process 1) variant.
 * LE branch from vendor rtl865x_asicL3.h. */
struct asic_l3route_l2 {
	u32 ipAddr;						/* word0 */
	/* word1 (process==1, direct/L2 nexthop) */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    netif:3, nextHop:10, reserv0:8;
	u32 reservw2;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* Nexthop entry (type 13, 32 entries, 1 word). LE branch, vendor asicL3.h. */
struct asic_nexthop {
	u32 type:1, IPIndex:4, dstVid:3, PPPoEIndex:3, nextHop:10, reserv0:11;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* PPPoE session entry (type 11, 8 entries, 1 word) — vendor
 * rtl8651_tblAsic_pppoeTable_t LE branch (sdk-ref/rtl865x_asicL3.h:89-115):
 * word0 = sessionID[15:0] | ageTime[18:16]. A nexthop with type=1 selects a row
 * here via PPPoEIndex; with ALECR EN_PPPOE set the ASIC then auto-encapsulates
 * routed WAN egress as {ETH_P_PPP_SES, this sessionID, PPP proto 0x0021} and
 * auto-decapsulates matching inbound session frames before the L3/L4 lookup
 * (non-matching / non-IPv4 PPP frames — LCP etc. — trap to the CPU intact). */
struct asic_pppoe {
	u32 sessionID:16, ageTime:3, reserv0:13;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* ARP entry (type 1, 512 entries, 1 word). LE branch, vendor asicL3.h.
 * (The vendor LE header has a bug: reserv0:21 -> should be :16; stock disasm
 * confirms nextHop@1-10, aging@11-15, so reserv0 = the top 16 bits.) */
struct asic_arp {
	u32 valid:1, nextHop:10, aging:5, reserv0:16;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3 route entry (type 2), ARP-process variant (process=2) — for a directly
 * connected subnet; the ASIC hashes the host IP into ARP range [ARPStart..ARPEnd].
 * Stock's LAN route uses this. LE branch, RTL8197F. */
struct asic_l3route_arp {
	u32 ipAddr;						/* word0 */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    netif:3, ARPStart:6, ARPEnd:6, ARPIpIdx:3, DSLEG:1, DSL_IDX1_0:2;
	u32 reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3 route entry (type 2), NxtHop-process variant (process=5) — for the default
 * route; uses nexthop entries [nhStart..nhStart+nhNum-1]. LE branch, RTL8197F. */
struct asic_l3route_nxthop {
	u32 ipAddr;						/* word0 */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    nhNum:3, nhStart:4, nhNxt:5, nhAlgo:2, IPDomain:3, reserv0:1,
	    DSLEG:1, DSL_IDX1_0:2;
	u32 reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* VLAN entry (type 6, 3 words). LE branch, RTL8197F. memberPort = ports 0..5,
 * extMemberPort = ports 6..8 (CPU port 8 => extMemberPort bit2). */
struct asic_vlan {
	u32 memberPort:6, extMemberPort:3, egressUntag:6, extEgressUntag:3,
	    fid:2, hp:3, reserved2:9;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* Netif (ASIC L3 interface) entry (type 4, 5 words). LE branch, RTL8197F.
 * The 48-bit MAC is split: mac18_0 = MAC[18:0], mac47_19 = MAC[47:19].
 * The 8-bit ingress-ACL start is split: (inACLStartH<<1)|inACLStartL.
 * macMask (# of low MAC bits ignored, 0 = exact) is split (macMaskH<<1)|macMaskL. */
struct asic_netif {
	u32 valid:1, vid:12, mac18_0:19;			/* word0 */
	u32 mac47_19:29, enHWRoute:1, enHWRouteV6:1, inACLStartL:1;	/* word1 */
	u32 inACLStartH:7, inACLEnd:8, outACLStart:8, outACLEnd:8, macMaskL:1;	/* word2 */
	u32 macMaskH:2, mtu:15, mtuV6:15;			/* word3 */
	u32 reservw4, reservw5, reservw6, reservw7;
};

/*
 * Low-level table engine. @force selects the vendor CMD_FORCE (write at exact
 * index) vs CMD_ADD (hardware-hashed insert, reports status). Both freeze the
 * TLU (SWTCR0 EN_STOP_TLU) around the access. Return 0 on success, <0 on error.
 */
int rtl865x_asic_write_entry(u32 type, u32 idx, const void *entry, bool force);
int rtl865x_asic_read_entry(u32 type, u32 idx, void *entry);

/* Enable the ASIC L3 engine (reset+enable, poll ready). Does NOT enable NAPT
 * forwarding or touch L2 — safe on a running L2 bridge. Return 0/‑errno. */
int rtl865x_asic_l3_engine_enable(void);

/*
 * ---- M6.6 Phase 3: per-flow NAPT-row API for the conntrack hwnat module ----
 *
 * Serialises ALL ASIC table mutation. The low-level engine already freezes the
 * TLU per access, but a NAT session is TWO rows (outbound + inbound) that must be
 * installed/torn-down atomically w.r.t. the gw_prog reprogrammer and the aging
 * worker. Every caller that touches the tables (gw_prog, the /proc scanners, and
 * the hwnat ADD/DEL/worker) takes this. All callers are process-context/sleepable;
 * the ndo_flow_offload core already serialises ADD vs DEL, but NOT against gw_prog
 * or the worker — this mutex closes that gap. The helpers below are deliberately
 * lock-FREE: the caller holds rtl865x_hal_lock across a whole multi-row transaction.
 */
extern struct mutex rtl865x_hal_lock;

/* NAPT outbound-row index = HASH1(proto,intIP,intPort,remIP,remPort) (vendor verbatim).
 * With SWTCR1 EnL4WayH=0 the returned 10-bit value IS the physical row index. */
u32 gw_napt_hash1(u32 isTCP, u32 sip, u32 sport, u32 dip, u32 dport);

/* Thin, lock-free wrappers over the table engine for the L4/NAPT table (type 9).
 * Caller MUST hold rtl865x_hal_lock. idx is bounds-checked to [0, RTL865X_NAPT_ROWS).
 * _read does the vendor double-read so the 6-bit agingTime field reads back live. */
int rtl865x_napt_write(u32 idx, const struct asic_napt_tcpudp *e);
int rtl865x_napt_read(u32 idx, struct asic_napt_tcpudp *out);
int rtl865x_napt_clear(u32 idx);
void rtl865x_napt_prefill(void);

/* ---- M7.2: dynamic WAN identity (PPPoE session + dynamic masquerade IP) ----
 * The WAN side is no longer compile-time: on a PPPoE WAN the peer MAC (the AC),
 * the session id and the masquerade IP (ppp0's local IPv4) are only known at
 * PPP-up and change on every reconnect. rtl819x_hwnat.c learns them from the
 * flow-offload dest path + the WAN netdev and pushes them down here. The two
 * setters are shadow-compared and idempotent: return 0 = ASIC already holds the
 * requested state (steady-state fast path, no table writes), 1 = tables were
 * actually rewritten (the caller must flush its per-flow NAPT rows — they were
 * programmed under the OLD identity), <0 = error. Caller MUST hold
 * rtl865x_hal_lock. gw_prog()/rtl865x_gw_rearm() replay the live shadows, so a
 * /proc reprogram or fabric-reset rearm reproduces the CURRENT WAN identity. */
extern u32 rtl865x_wan_extip;	/* live extIP[0] (host order); boot = RTL865X_WAN_EXTIP */
int rtl865x_set_wan_extip(u32 ip);
int rtl865x_pppoe_set(u32 idx, u16 sid);	/* raw PPPoE session-table row write */
int rtl865x_wan_set_nexthop(const u8 *gw_mac, bool is_pppoe, u16 pppoe_sid);

/* M7: re-run the full gw scaffolding program (== `cat /proc/rtl865x_gw` minus
 * the dump) from kernel context after a fabric full reset. Takes
 * rtl865x_hal_lock internally — caller must NOT hold it. */
int rtl865x_gw_rearm(void);

#endif /* _RTL865X_ASICHAL_H */
