// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8197F ASIC L3/NAPT table engine — minimal HAL (M6.6 foundation).
 *
 * Reverse-engineered from the stock 3.10.90 kernel and cross-validated against
 * the Realtek vendor SDK (AsicDriver/rtl865x_asicBasic.c). See the full spec in
 * m6.6-hwnat/ASIC-ENGINE.md. This file implements ONLY the table-access engine,
 * the L3-engine enable, and a boot-safe on-demand self-test (/proc/rtl865x_asic)
 * that writes a NAPT entry and reads it back — proving the RE'd engine works on
 * real silicon before any forwarding logic is built on top of it.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/mutex.h>

#include "rtl819x_regs.h"
#include "rtl865x_asichal.h"

/* Serialises all ASIC table mutation across gw_prog, the /proc scanners, and the
 * conntrack hwnat module (see the header for the full rationale). */
DEFINE_MUTEX(rtl865x_hal_lock);

/* Registers not already in rtl819x_regs.h (all verbatim from vendor asicregs.h,
 * confirmed against stock disasm). */
#define SWTASR			(TACI_BASE + 0x004)	/* table access status */
#define TABSTS_MASK		0x1
#define TABSTS_SUCCESS		0x0
#define CMD_ADD			(1 << 1)		/* SWTACR add (hashed) */
#define EN_STOP_TLU		(1 << 18)		/* SWTCR0: freeze lookup */
#define STOP_TLU_READY		(1 << 19)		/* SWTCR0: freeze acked */
#define ASIC_L3_ENGINE_CFG	(RTL819X_SWCORE_BASE + 0x4234)
#define L3_ENGINE_READY		(1 << 11)		/* poll bit after enable */

#define ASIC_ADDR(type, idx)	(RTL819X_SWTBL_BASE + ((type) << 16) + ((idx) << 5))
#define ASIC_POLL_MAX		200000

/*
 * Words actually written/read per table type (a 32-byte entry has 8 words but
 * only the first N carry meaning). Verbatim from vendor _rtl8651_asicTableSize[]
 * (RTL8197F branch) — byte-for-byte identical to the stock table @ 0x8050e590.
 */
static const u8 asic_tbl_words[] = {
	2,  /*  0 L2_SWITCH   */	1,  /*  1 ARP        */	3,  /*  2 L3_ROUTING */
	3,  /*  3 MULTICAST   */	5,  /*  4 NETIF      */	3,  /*  5 EXT_INT_IP */
	3,  /*  6 VLAN        */	3,  /*  7 VLAN1      */	4,  /*  8 SERVER_PORT*/
	3,  /*  9 L4_TCP_UDP  */	3,  /* 10 L4_ICMP    */	1,  /* 11 PPPOE      */
	11, /* 12 ACL_RULE    */	1,  /* 13 NEXT_HOP   */	3,  /* 14 RATE_LIMIT */
	1,  /* 15 ALG         */	9,  /* 16 DS_LITE    */	6,  /* 17 6RD        */
	6,  /* 18 L3_V6_ROUTE */	1,  /* 19 NEXT_HOP_V6*/	3,  /* 20 ARP_V6     */
	9,  /* 21 MULTICAST_V6*/
};

static int asic_wait_action_done(void)
{
	u32 g = 0;

	while ((REG32(SWTACR) & TLU_ACTION_MASK) != TLU_ACTION_DONE)
		if (++g > ASIC_POLL_MAX)
			return -ETIMEDOUT;
	return 0;
}

int rtl865x_asic_l3_engine_enable(void)
{
	u32 g = 0;

	REG32(ASIC_L3_ENGINE_CFG) = 0;		/* reset */
	REG32(ASIC_L3_ENGINE_CFG) = 8;		/* enable */
	while (!(REG32(ASIC_L3_ENGINE_CFG) & L3_ENGINE_READY))
		if (++g > ASIC_POLL_MAX)
			return -ETIMEDOUT;
	return 0;
}

int rtl865x_asic_write_entry(u32 type, u32 idx, const void *entry, bool force)
{
	const u32 *w = entry;
	int i, n, rc = 0;
	u32 g = 0;

	if (type >= ARRAY_SIZE(asic_tbl_words) || !entry)
		return -EINVAL;
	n = asic_tbl_words[type];

	/* Freeze the table-lookup unit around the write (vendor RTL819X_TLU_CHECK). */
	REG32(SWTCR0) |= EN_STOP_TLU;
	while (!(REG32(SWTCR0) & STOP_TLU_READY))
		if (++g > ASIC_POLL_MAX) { rc = -ETIMEDOUT; goto out; }

	rc = asic_wait_action_done();
	if (rc)
		goto out;

	for (i = 0; i < n; i++)
		REG32(TCR0 + (i << 2)) = w[i];
	REG32(SWTAA) = ASIC_ADDR(type, idx);
	REG32(SWTACR) = TLU_ACTION_START | (force ? TLU_CMD_FORCE : CMD_ADD);

	rc = asic_wait_action_done();
	if (rc)
		goto out;

	if (!force && (REG32(SWTASR) & TABSTS_MASK) != TABSTS_SUCCESS)
		rc = -EIO;
out:
	REG32(SWTCR0) &= ~EN_STOP_TLU;
	return rc;
}

int rtl865x_asic_read_entry(u32 type, u32 idx, void *entry)
{
	volatile u32 *base;
	u32 *w = entry;
	int i, n, rc;

	if (type >= ARRAY_SIZE(asic_tbl_words) || !entry)
		return -EINVAL;
	n = asic_tbl_words[type];

	rc = asic_wait_action_done();
	if (rc)
		return rc;

	base = (volatile u32 *)(uintptr_t)ASIC_ADDR(type, idx);
	for (i = 0; i < n; i++)
		w[i] = base[i];

	/* Vendor: dummy-read an unused ACL entry to refresh the ASIC read latch. */
	(void)REG32(ASIC_ADDR(ASIC_TYPE_ACL_RULE, 1024));
	return 0;
}

/* NOTE (M6.6 Phase-3 review): the original /proc/rtl865x_asic write+readback
 * self-test that lived here is REMOVED. It predated the live gateway (it proved
 * the RE'd table engine on silicon before any forwarding was built on top) and
 * had become dangerous: it reset the L3 engine mid-traffic, force-wrote NAPT row
 * 1000 (inside the live hash space — could clobber a conntrack-owned flow row)
 * with a valid=1 test entry it never cleared, and took no lock against gw_prog /
 * the hwnat module. Table-engine health is now covered by the gw_prog readback
 * (/proc/rtl865x_gw) and the read-only /proc/rtl865x_{napt,fabric} scanners. */

/* ---- gateway config: program a stock-equivalent HW routing/NAT datapath ----
 * Mirrors the stock ASIC gateway (see m6.6-hwnat/STOCK-TABLES.md) but with the
 * ingress ACL DISABLED (MSCR EN_IN_ACL off => permit-all), so the whole ACL
 * table is skipped. On-demand via /proc/rtl865x_gw (read = program + dump).
 * LAN=VID2 192.168.0.0/24; WAN=VID1 172.16.0.0/24; box WAN IP 172.16.0.1.
 */
#define MSCR_EN_L3		(1 << 1)
#define MSCR_EN_L4		(1 << 2)
#define MSCR_EN_IN_ACL		(1 << 4)
#define GW_ALECR		(RTL819X_SWCORE_BASE + 0x440C)	/* == TTLCR */
#define GW_ALECR_EN_TTL1	(1 << 16)	/* TTL-decrement on routing (required) */
#define GW_TEACR		(RTL819X_SWCORE_BASE + 0x4400)
#define GW_TEATCR		(RTL819X_SWCORE_BASE + 0x4404)	/* per-proto NAPT aging reload timeouts */
#define GW_CSCR			(RTL819X_SWCORE_BASE + 0x4048)	/* checksum control */
#define GW_CSCR_L3L4CHK		((1 << 4) | (1 << 5))	/* EnL3ChkCal|EnL4ChkCal: recompute */
/* THE cascade: SoC switch-core port 0 = the RGMII trunk to the RTL8367S. Enabling the
 * Realtek CPU/source-port tag decode on it makes the 5 jacks appear as SoC ports 0-4
 * (RE'd from stock: init_8367r trunk setup 0x80194728, P0GMIICR write 0x801947a0). */
#define GW_P0GMIICR		(RTL819X_SWCORE_BASE + 0x414C)	/* trunk (port0) GMII control */
#define GW_CFG_CPUC_TAG		(0x06000000)	/* CFG_CPUC_TAG(1<<25) | CFG_TX_CPUC_TAG(1<<26) */
#define GW_PCRP0		(RTL819X_SWCORE_BASE + 0x4104)	/* trunk port force-link */
/* Full SoC-side trunk/CPU-tag bring-up (stock init_8367r ~0x80197ed4, trunk setup
 * 0x80194728). The RGMII is already strap-up so normal frames forward, but the
 * CPU-tag DECODE needs the ext-port + GMII-mode config below (esp. EXTPCR0 and
 * P0GMIICR CF_SEL_RGTXC bits[19:18]=3, which read 0 without this). */
#define GW_PITCR		(RTL819X_SWCORE_BASE + 0x4100)	/* port iface trunk ctrl */
#define GW_MACCR		(RTL819X_SWCORE_BASE + 0x4000)
#define GW_MACCR1		(RTL819X_SWCORE_BASE + 0x4058)
#define GW_EXTPCR0		(RTL819X_SWCORE_BASE + 0x5108)	/* extension-port ctrl 0 */
#define GW_RGMII_PAD		(0xB8000850)			/* board RGMII pad mux */
#define PVCR(port)		(RTL819X_SWCORE_BASE + 0x4A08 + ((port) >> 1) * 4)

#define GW_VID_LAN		RTL865X_VID_LAN		/* 2 (shared w/ rtl819x_hwnat.c) */
#define GW_VID_WAN		RTL865X_VID_WAN		/* 1 */
/* box LAN MAC = OpenWrt eth0 (00:e0:4c:81:96:c2); WAN MAC = +1 */
static const u8 GW_MAC_LAN[6] = { 0x00,0xe0,0x4c,0x81,0x96,0xc2 };
static const u8 GW_MAC_WAN[6] = { 0x00,0xe0,0x4c,0x81,0x96,0xc3 };

static void gw_set_pvid(u32 port, u32 pvid)
{
	u32 off = (port & 1) ? 16 : 0;
	u32 v = REG32(PVCR(port));

	v &= ~(0xFFF << off);
	v |= (pvid & 0xFFF) << off;
	REG32(PVCR(port)) = v;
}

/* prefix length -> asicMask value the ASIC wants: 31 - (32-prefixlen) trailing
 * zeros of the mask; vendor computes 31 - lowbit(mask). For /24 => mask
 * 255.255.255.0, lowest set bit = bit8 => asicMask = 31 - 8 = 23. */
static u32 gw_asicmask(u32 prefix_len)
{
	return 31 - (32 - prefix_len);
}

/* L2 switch-table row is forced by the vendor hash of {MAC, FID}. */
static u32 gw_l2_row(const u8 *m, u32 fid)
{
	static const u8 fh[4] = { 0x00, 0x0f, 0xf0, 0xff };

	return (m[0] ^ m[1] ^ m[2] ^ m[3] ^ m[4] ^ m[5] ^ fh[fid]) & 0xFF;
}

/* Program a static nexthop L2 entry {mac -> egress port, fid, static, NH}. */
static u32 gw_write_l2(const u8 *m, u32 member_portmask, u32 fid)
{
	u32 row = gw_l2_row(m, fid);
	u32 e[8] = { 0 };

	e[0] = (m[1] << 24) | (m[2] << 16) | (m[3] << 8) | m[4];
	/* mac[0] | memberPort<<8 | STA(1<<18) | age3(3<<19) | NH(1<<22) | fid<<23 */
	e[1] = m[0] | (member_portmask << 8) | (1u << 18) | (3u << 19) | (1u << 22) | (fid << 23);
	rtl865x_asic_write_entry(ASIC_TYPE_L2_SWITCH, (row << 2) | 0, e, true);
	return (row << 2) | 0;			/* the (row<<2)|col nextHop pointer */
}

/* Full-flags L2 entry writer (member/extmember/toCPU/fid/auth) — for the base
 * initAsicL2 entries (broadcast + CPU-MAC trap) that stock installs and the
 * minimal gw_write_l2 above omits. w1 = mac0 | member<<8 | extmember<<14 |
 * toCPU<<17 | STA(1<<18) | age3(3<<19) | NH(1<<22) | fid<<23 | auth<<25. */
static u32 gw_write_l2_full(const u8 *m, u32 member, u32 extmember,
			    u32 tocpu, u32 fid, u32 auth)
{
	u32 row = gw_l2_row(m, fid);
	u32 e[8] = { 0 };

	e[0] = (m[1] << 24) | (m[2] << 16) | (m[3] << 8) | m[4];
	e[1] = m[0] | (member << 8) | (extmember << 14) | (tocpu << 17)
	     | (1u << 18) | (3u << 19) | (1u << 22) | (fid << 23) | (auth << 25);
	rtl865x_asic_write_entry(ASIC_TYPE_L2_SWITCH, (row << 2) | 0, e, true);
	return (row << 2) | 0;
}

/* test peers: hal on LAN switch-port 0 (VID2/fid0); tiny (RPi) on WAN port 4 (VID1/fid1) */
static const u8 GW_MAC_HALLAN[6]  = { 0x54,0xbf,0x64,0x18,0xb8,0xde };	/* hal enp3s0 */
static const u8 GW_MAC_TINYWAN[6] = { 0xe4,0x5f,0x01,0x04,0x98,0xaf };	/* tiny eth0 */

/* ---- M6.6 Phase 2: static NAPT (hardware NAT src-rewrite) ---- */
#define ASIC_TYPE_EXT_INT_IP	5	/* external/WAN-IP table (16 entries, 3 words) — absent from hal.h's enum */
#define SWTCR1			(ALE_BASE + 0x1C)	/* EnL4WayH(bit9)/L4EnHash1(bit13) — not in rtl819x_regs.h */

/* External-IP (masquerade) table entry — vendor rtl8651_tblAsic_extIpTable_t
 * (sdk-ref/rtl865x_asicL3.h:65-87, LITTLE_ENDIAN). isOne2One=0 + internalIP=0
 * = many-to-one masquerade to externalIP. */
struct asic_extintip {
	u32 internalIP;					/* word0 */
	u32 externalIP;					/* word1 */
	u32 valid:1, isOne2One:1, isLocalPublic:1,	/* word2 */
	    nextHop:5, reserv0:24;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* The NAPT outbound-index hash — vendor rtl8651_naptTcpUdpTableIndex HASH1
 * (sdk-ref/rtl865x_asicL4.c:160-163), ported VERBATIM: a 10-way XOR fold of
 * {dip,dport,isTCP,sip,sport} to a 10-bit index. With SWTCR1 EnL4WayH=0 the
 * NAPT table is 1024 flat 1-way rows and this value IS the exact write index.
 * (The reverse direction needs no hash: the inbound row lives at
 * globalPort & 0x3ff, which the globalPort itself encodes via offset<<10.) */
u32 gw_napt_hash1(u32 isTCP, u32 sip, u32 sport, u32 dip, u32 dport)
{
	return ((sport & 0x3ff)
		^ (((sport & 0xfc00) >> 10) | ((sip & 0xf) << 6))
		^ ((sip >> 4) & 0x3ff)
		^ ((sip >> 14) & 0x3ff)
		^ (((sip & 0xff000000) >> 24) | ((isTCP & 1) << 8) | ((dport & 1) << 9))
		^ ((dport >> 1) & 0x3ff)
		^ (((dport >> 11) & 0x1f) | ((dip & 0x1f) << 5))
		^ ((dip >> 5) & 0x3ff)
		^ ((dip >> 15) & 0x3ff)
		^ ((dip >> 25) & 0x7f)) & 0x3ff;
}

/*
 * ---- Phase 3: lock-free NAPT-row helpers (see rtl865x_asichal.h) ----
 * Caller holds rtl865x_hal_lock. Thin wrappers over the type-9 table engine, with
 * a row-index bounds check so a bad conntrack-derived index can never scribble
 * outside the 1024-row table.
 */
int rtl865x_napt_write(u32 idx, const struct asic_napt_tcpudp *e)
{
	if (idx >= RTL865X_NAPT_ROWS || !e)
		return -EINVAL;
	return rtl865x_asic_write_entry(ASIC_TYPE_L4_TCP_UDP, idx, e, true);
}

int rtl865x_napt_clear(u32 idx)
{
	struct asic_napt_tcpudp z;

	if (idx >= RTL865X_NAPT_ROWS)
		return -EINVAL;
	memset(&z, 0, sizeof(z));	/* valid=0 => the ASIC treats the row as free */
	return rtl865x_asic_write_entry(ASIC_TYPE_L4_TCP_UDP, idx, &z, true);
}

int rtl865x_napt_read(u32 idx, struct asic_napt_tcpudp *out)
{
	int rc;

	if (idx >= RTL865X_NAPT_ROWS || !out)
		return -EINVAL;
	/* Vendor double-read discipline: the 6-bit agingTime field is latched and only
	 * reads back live on the SECOND access (each rtl865x_asic_read_entry ends with a
	 * dummy ACL read that refreshes the latch). The aging worker keys activity off
	 * this field, so the first read alone would report a stale count. */
	rc = rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, idx, out);
	if (rc)
		return rc;
	return rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, idx, out);
}

static int gw_prog(struct seq_file *m, void *v)
{
	struct asic_vlan vlan;
	struct asic_netif nif;
	struct asic_l3route_arp rt;
	struct asic_l3route_l2 hr;
	struct asic_l3route_nxthop nr;
	struct asic_nexthop nh;
	u32 mac_hi, mac_lo, rb[8];
	u32 mscr0, mscr1, hal_nh, tiny_nh;
	int rc, ok = 1;

	/* Hold the HAL lock across the whole reprogram so it can't interleave with a
	 * concurrent hwnat ADD/DEL or the aging worker mutating the L4 table. */
	mutex_lock(&rtl865x_hal_lock);

	seq_puts(m, "[rtl865x gateway config — program + readback]\n");

	/* 1. init: L3 engine + operation layer 4 (L2|L3|L4), ACL OFF */
	rc = rtl865x_asic_l3_engine_enable();
	mscr0 = REG32(MSCR);
	/* M6.6: EN_IN_ACL OFF — with it ON, hal->tiny frames were dropped in the SoC
	 * BEFORE routing (e0rx flat even with CPU in the nexthop egress = not routed,
	 * not trapped = an ingress-ACL drop; the all-zero ACL entries evidently aren't
	 * permit-all). Off => no ingress ACL check => permit-all default.
	 *
	 * ★ EN_L4 OFF for the routing milestone: with L4 on and NO extIP/NAPT session
	 * rows programmed, the L4 engine's inbound-session matching EATS unsolicited
	 * WAN->LAN traffic once the first outbound flow arms it (measured: virgin boot
	 * = reverse requests pass; after the first forward burst, tiny-initiated
	 * requests die silently while tiny's REPLIES to hal-initiated pings — same
	 * addressing, same size, only the ICMP type differs — keep passing; eth0 RX
	 * stays flat = silicon drop, not CPU trap; SWTCR0's reset NAPTR_NOT_FOUND_DROP
	 * is preserved by the write below, so misses DROP). The degraded-state frame
	 * CORRUPTION (payload shifted over the L3 header + repeated buffer cells +
	 * embedded metadata words) is consistent with a garbage L4 session REWRITE.
	 * BUT EN_L4 cannot simply stay off: with L2|L3 only, TCP bulk COLLAPSES (iperf3
	 * 0 bytes, SYN retransmits; ICMP fine) — the vendor pipeline routes TCP/UDP
	 * ONLY through the L4 stage. The 372 Mbit/s "no rows programmed" run earlier
	 * was the reset-default NAPT AUTO-LEARN silently creating hardware sessions
	 * (which also explains the eating and the garbage-rewrite corruption).
	 *
	 * ⇒ the deliberate config: EN_L4 ON with SWTCR0 forced explicitly below —
	 * AutoLearn OFF (no silent hardware sessions), NAPTR_NOT_FOUND_DROP=0 (a
	 * TCP/UDP miss TRAPS TO CPU for software forwarding instead of being eaten),
	 * NAPTF2CPU=1 (non-TCP/UDP L4 to CPU). ICMP keeps HW-routing via L3; TCP/UDP
	 * run software until Phase 2 writes real extIP+NAPT rows (then per-flow HW). */
	REG32(MSCR) = REG32(MSCR) | MSCR_EN_L2 | MSCR_EN_L3 | MSCR_EN_L4;
	mscr1 = REG32(MSCR);
	REG32(GW_ALECR) |= GW_ALECR_EN_TTL1;			/* TTL-decrement on route */
	REG32(GW_CSCR) |= GW_CSCR_L3L4CHK;			/* ★ recompute L3/L4 cksum after TTL-- / NAT rewrite (else the peer drops it) */
	/* SWTCR0: VLAN netdec + mcast-internal as before, PLUS explicit NAPT policy
	 * (the old read-modify-write PRESERVED the reset defaults of bits 0-2 — i.e.
	 * auto-learn/not-found-drop stayed at silicon defaults, causing the silent
	 * hardware sessions + inbound eating documented above):
	 *   bit0 NAPTR_NOT_FOUND_DROP = 0  (L4 miss -> trap to CPU, never drop)
	 *   bit1 EnNAPTAutoLearn      = 0  (no hardware session auto-learn)
	 *   bit2 EnNAPTAutoDelete     = 1  (Phase 3: HW clears valid at age-0; the aging
	 *                                   worker reads valid==0 as "flow idle/dead")
	 *   bit14 NAPTF2CPU           = 1  (non-TCP/UDP L4 protos -> CPU) */
	REG32(SWTCR0) = ((REG32(SWTCR0) & ~(3u << 16) & ~0x7u) | (0x3Fu << 5)) | (1u << 14) | (1u << 2);
	REG32(GW_MACCR1) |= (1u << 0);	/* M6.6 PORT0_ROUTER_MODE: put the trunk (SoC port0) in router mode — test whether it permits the L3-routed U-turn (egress back out the single trunk) that source-port filtering otherwise blocks, since hal(LAN)+tiny(WAN) are both behind port0 */
	/* Phase 3 aging: TEACR bit0 L2/ARP aging OFF (keeps the static ARP[64] nexthop
	 * chain from aging→invalid mid-flow, which re-triggered the bad-pointer hang);
	 * bit1 L4 aging ON so the per-session `agingTime` field ticks — that field (there
	 * is NO per-entry hit bit) is the only way the aging worker infers activity: HW
	 * reloads it from TEATCR on each matching packet and decrements it per tick, and
	 * clears `valid` at 0 (EnNAPTAutoDelete). TEATCR = per-proto reload, 6-bit
	 * differentiated timer 0x11 ≈ 100 s decay (ICMP[29:24] UDP[23:18] TCP-long[17:12]
	 * TCP-med[11:6] TCP-short[5:0]). */
	REG32(GW_TEACR) = 0x1;
	REG32(GW_TEATCR) = (RTL865X_NAPT_AGING_RELOAD << 24) | (RTL865X_NAPT_AGING_RELOAD << 18)
			 | (RTL865X_NAPT_AGING_RELOAD << 12) | (RTL865X_NAPT_AGING_RELOAD << 6)
			 | RTL865X_NAPT_AGING_RELOAD;
	/* ★ INGRESS ACL permit-all: each netif references an ingress ACL range that
	 * MUST hold a valid rule or the ASIC BLOCKS routing (stock ground truth,
	 * STOCK-TABLES.md). MSCR EN_IN_ACL is set above; an all-zero ACL entry =
	 * RULE_ETHERNET / ACTION_PERMIT (permit-all). Fill 0..255 so every netif's
	 * ingress range resolves to permit. Egress ACL stays OFF (documented hw bug). */
	{
		u32 acl_permit[11] = { 0 };
		int a;
		for (a = 0; a < 256; a++)
			rtl865x_asic_write_entry(ASIC_TYPE_ACL_RULE, a, acl_permit, true);
	}
	/*
	 * M6.6 Fork A: NO source-port CPU-tag (it breaks the box's CPU RX). The SoC
	 * classifies netifs purely by 802.1Q VID — rtl865x_start() already programs
	 * SoC VLANs VID2(LAN)/VID1(WAN) members 0x7F (all ports incl CPU port6) +
	 * PVID2, and the external RTL8367S tags each jack's frames with its VID and
	 * sends them TAGGED up the trunk. So we do NOT touch P0GMIICR/PCRP0 or the
	 * 8367S far-end, and we do NOT rewrite the SoC VLAN/PVID here (rtl865x_start
	 * owns them coherently). (void) the CPU-tag/RGMII regs kept for Fork B.
	 */
	(void)GW_PCRP0; (void)GW_P0GMIICR; (void)GW_CFG_CPUC_TAG;
	(void)GW_PITCR; (void)GW_MACCR; (void)GW_MACCR1; (void)GW_EXTPCR0; (void)GW_RGMII_PAD;
	(void)vlan;
	seq_printf(m, "L3 rc=%d  MSCR %08x -> %08x  ALECR=%08x SWTCR0=%08x  (Fork A: VID-based, NO CPU-tag)\n",
		   rc, mscr0, mscr1, REG32(GW_ALECR), REG32(SWTCR0));

	/* 4. netif[0]=LAN, netif[1]=WAN (enHWRoute, 1 MAC => macMask 7, mtu 1500) */
	memset(&nif, 0, sizeof(nif));
	mac_hi = (GW_MAC_LAN[0] << 21) | (GW_MAC_LAN[1] << 13) | (GW_MAC_LAN[2] << 5) | (GW_MAC_LAN[3] >> 3);
	mac_lo = ((GW_MAC_LAN[3] & 0x7) << 16) | (GW_MAC_LAN[4] << 8) | GW_MAC_LAN[5];
	nif.valid = 1; nif.vid = GW_VID_LAN; nif.mac18_0 = mac_lo; nif.mac47_19 = mac_hi;
	nif.enHWRoute = 1; nif.macMaskL = 1; nif.macMaskH = 3; nif.mtu = 1500;
	nif.inACLStartL = 0; nif.inACLStartH = 0; nif.inACLEnd = 3;	/* ingress ACL [0..3] permit-all */
	nif.outACLStart = 253; nif.outACLEnd = 253;
	rtl865x_asic_write_entry(ASIC_TYPE_NETIF, 0, &nif, true);
	memset(&nif, 0, sizeof(nif));
	mac_hi = (GW_MAC_WAN[0] << 21) | (GW_MAC_WAN[1] << 13) | (GW_MAC_WAN[2] << 5) | (GW_MAC_WAN[3] >> 3);
	mac_lo = ((GW_MAC_WAN[3] & 0x7) << 16) | (GW_MAC_WAN[4] << 8) | GW_MAC_WAN[5];
	nif.valid = 1; nif.vid = GW_VID_WAN; nif.mac18_0 = mac_lo; nif.mac47_19 = mac_hi;
	nif.enHWRoute = 1; nif.macMaskL = 1; nif.macMaskH = 3; nif.mtu = 1500;
	nif.inACLStartL = 0; nif.inACLStartH = 0; nif.inACLEnd = 3;	/* ingress ACL [0..3] permit-all */
	nif.outACLStart = 253; nif.outACLEnd = 253;
	rtl865x_asic_write_entry(ASIC_TYPE_NETIF, 1, &nif, true);

	/* 5. direct routes: LAN 192.168.0.0/24 -> netif0; WAN 172.16.0.0/24 -> netif1
	 *    process=ARP(2); ARP range 0..248 like stock. */
	/* 4a. base initAsicL2 entries (stock installs these via rtl865x_initAsicL2;
	 * my PoC omitted them): broadcast (flood jacks + CPU) + the CPU-MAC trap
	 * 00:00:0a:00:00:0f (route/ARP misses trap to CPU via this) for BOTH fids.
	 * member 0x1f = jacks 0-4, extmember 0x4 = CPU port8, toCPU=1, auth=1.
	 * (Exact values read live from stock /proc/rtl865x/l2.) */
	{
		static const u8 BC[6]  = { 0xff,0xff,0xff,0xff,0xff,0xff };
		static const u8 CPM[6] = { 0x00,0x00,0x0a,0x00,0x00,0x0f };
		/* Fork A: no CPU-tag, so jacks are NOT SoC ports 0-4 — everything external
		 * is behind the trunk. Flood to physical ports 0-5 (0x3f; the trunk is one
		 * of them, the VID picks the jack) + CPU port6 (extMember bit0=0x1, NOT the
		 * CPU-tag model's port8). */
		gw_write_l2_full(BC,  0x3f, 0x1, 1, 0, 1);	/* bcast   FID0 -> ports0-5 + CPU6 */
		gw_write_l2_full(CPM, 0x00, 0x1, 1, 0, 1);	/* CPU-MAC FID0 (trap to CPU6) */
		gw_write_l2_full(CPM, 0x00, 0x1, 1, 1, 1);	/* CPU-MAC FID1 (trap to CPU6) */
		gw_write_l2_full(BC,  0x3f, 0x1, 1, 1, 1);	/* bcast   FID1 -> ports0-5 + CPU6 */
	}
	/* 4b. L2 nexthop entries for the two peers (row forced by MAC/FID hash). Fork
	 * A: egress the TRUNK (flood ports 0-5; the routed frame carries the egress
	 * netif's VID so the 8367S sends it to the right jack). CPU excluded (no loop). */
	hal_nh  = gw_write_l2(GW_MAC_HALLAN,  0x3f, 0);	/* hal  -> trunk (LAN vid2), fid0 */
	tiny_nh = gw_write_l2(GW_MAC_TINYWAN, 0x3f, 1);	/* tiny -> trunk (WAN vid1), fid1 — trunk-only egress (HW offload; CPU not in the path) */

	/* 5. Routes. /32 host routes FIRST (process=L2, nextHop = peer L2 idx directly —
	 * bypasses the ARP-window hashing) so hal<->tiny resolves deterministically; the
	 * /24 direct routes (process=ARP) follow as a fallback. Most-specific = lowest idx. */
	/* ★ Phase 2: the tiny (WAN) /32 must be a process=5 NAPT-NextHop route, NOT a
	 * process=1 Direct route — only NxtHop routes link to the extIP table (via the
	 * nexthop's IPIndex) and so engage the L4 NAPT src-rewrite stage; a Direct route
	 * L3-forwards but bypasses NAPT entirely (proven: process=1 = src never
	 * translated + hardware auto-learn saw nothing). Stock uses process=NxtHop for
	 * all WAN-bound traffic. process=5 needs a nexthop GROUP of >=2 (nhNum field: 0=2
	 * nexthops), nhStart stored >>1 (0 => start at nexthop[0]); program two identical
	 * nexthops -> tiny. Each nexthop's IPIndex=0 => extIP[0] = the masquerade src IP. */
	/* The hardware chain is route(5) → nexthop → ARP → L2 (TWO indirections):
	 * nexthop.nextHop is dereferenced as an ARP-table index, NOT an L2 index
	 * (STOCK-TABLES.md:50/54/89; setter L3.c:109). Pointing it straight at the L2
	 * entry made the ASIC walk a garbage ARP slot → fabric hang/watchdog. So add an
	 * intermediate ARP entry {valid, nextHop=tiny_nh} and point the nexthop at IT. */
	{
		struct asic_arp a;

		memset(&a, 0, sizeof(a));
		a.valid = 1;
		a.nextHop = tiny_nh;		/* ARP → the L2 entry (tiny's MAC, trunk egress) */
		a.aging = 0x1f;			/* max; TEACR bit0 below freezes aging anyway */
		rtl865x_asic_write_entry(ASIC_TYPE_ARP, 64, &a, true);
	}
	memset(&nh, 0, sizeof(nh));
	nh.nextHop = 64;			/* ★ ARP index (was tiny_nh = an L2 index = the hang) */
	nh.dstVid = GW_VID_WAN;			/* egress VID 1 (WAN); NxtHopEntry has no netif field */
	nh.IPIndex = 0;				/* -> extIP[0] : the source-rewrite IP linkage */
	nh.type = 0;				/* ethernet (not PPPoE) */
	rtl865x_asic_write_entry(ASIC_TYPE_NEXT_HOP, 0, &nh, true);
	rtl865x_asic_write_entry(ASIC_TYPE_NEXT_HOP, 1, &nh, true);

	/* Phase 3: WIDEN to the whole WAN /24 (was tiny /32) so ANY WAN-bound flow
	 * engages the process=5 NAPT stage — per-flow NAT rows are now added dynamically
	 * by the conntrack hook, not hard-coded for one dest. The nexthop group (→ARP[64]
	 * →tiny_nh) still egresses via tiny as the single "WAN gateway" of this rig. */
	memset(&nr, 0, sizeof(nr));
	nr.ipAddr = 0xAC100000;			/* 172.16.0.0/24 = WAN subnet */
	nr.ipMask = gw_asicmask(24); nr.valid = 1;
	nr.process = 5;				/* NAPT NextHop -> engages outbound NAT */
	nr.internal = 0;			/* external dest => LAN->WAN direction detected */
	nr.nhStart = 0; nr.nhNum = 0;		/* nexthop[0..1] (nhNum=0 encodes 2), nhStart<<1=0 */
	nr.nhNxt = 0; nr.nhAlgo = 2; nr.IPDomain = 0;	/* nhAlgo=2 matches stock (STOCK-TABLES.md:42); nhAlgo=0 hung the fabric */
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 0, &nr, true);

	memset(&hr, 0, sizeof(hr));
	hr.ipAddr = 0xC0A80002;			/* 192.168.0.2/32 = hal (LAN) */
	hr.ipMask = gw_asicmask(32); hr.valid = 1; hr.process = 1; hr.netif = 0;
	hr.internal = 1;			/* ★ source-side = INTERNAL (else this more-specific
						 * /32 overrides the LAN /24 and mis-zones hal as external) */
	hr.nextHop = hal_nh;
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 1, &hr, true);
	/* /24 direct routes (process=ARP), most-specific-after the /32s. */
	memset(&rt, 0, sizeof(rt));
	rt.ipAddr = 0xC0A80000;			/* 192.168.0.0/24 -> LAN */
	rt.ipMask = gw_asicmask(24); rt.valid = 1; rt.process = 2; rt.netif = 0;
	rt.internal = 1; rt.ARPStart = 0; rt.ARPEnd = 31;
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 2, &rt, true);
	/* Phase 3: the WAN 172.16.0.0/24 is now the process=5 NAPT route at idx0 (above);
	 * the old process=2 direct WAN /24 that used to live at idx3 is REMOVED so it can't
	 * shadow NAPT. Force-invalidate idx3 in case a warm reload left the stale entry. */
	{
		u32 dead[3] = { 0 };
		rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 3, dead, true);
	}
	/* catch-all default route [7] -> CPU (stock has [7] 0/0 process=NxtHop; here
	 * a plain process=CPU(4) trap so any unresolved frame reaches Linux instead
	 * of being silently dropped — also the ingress-works probe). w0=ip0;
	 * w1 = ipMask0 | valid(1<<5) | process=4(4<<6).
	 * (Review note: by the gw_asicmask() convention ipMask=0 encodes /1, not /0 —
	 * the exact /0 encoding is unconfirmed from the SDK snippets. Bench-proven to
	 * work as the CPU trap; if a dst with top bit differing from 0.0.0.0 ever
	 * bypasses it, this encoding is the suspect.) */
	{
		u32 ca[3] = { 0, (1u << 5) | (4u << 6), 0 };
		rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 7, ca, true);
	}
	/* NOTE: the /32 host routes above use process=1(L2). With the RX cascade now
	 * working, frames reach L3 but process=1 TRAPS to CPU (e0rx grows) instead of
	 * HW-egressing to port4. Tried process=ARP(2) + static ARP entries here
	 * (ARP word = valid|col<<1|(L2row&0xff)<<3|netif<<11) — that made frames DROP
	 * (bad ARP range/hash or entry semantics). Needs the exact setAsicArp hash /
	 * a live stock ARP-entry register-diff to finish the HW routed-egress. */

	/* 6. Phase 3 — per-GATEWAY NAPT scaffolding (the per-FLOW rows are now written
	 * dynamically by rtl819x_hwnat.c off conntrack ndo_flow_offload). All that stays
	 * static here is the masquerade external-IP table + the table-shape register:
	 *   - extIP[0] = 172.16.0.1 (the single WAN IP every flow masquerades to; the
	 *     hwnat ADD path guards that conntrack's chosen extIP matches this, else it
	 *     declines to software). internalIP=0 + isOne2One=0 => many-to-one NAPT.
	 *   - SWTCR1 = 0 (EnL4WayH=0, L4EnHash1=0): flat 1024x1-way L4 table so the 10-bit
	 *     gw_napt_hash1() value IS the physical row index — the invariant both the
	 *     hwnat writer and the aging reader depend on. CSCR (above) recomputes the
	 *     L3+L4 checksums after each src-rewrite.
	 * Phase-2's hand-programmed static hal->tiny flow is GONE: with EN_L4 armed and a
	 * NAPT miss trapping to CPU (SWTCR0 NAPTR_NOT_FOUND_DROP=0), the first packets of
	 * every flow reach Linux, get software-NAT'd + masqueraded to 172.16.0.1, and the
	 * conntrack hook then installs the matching HW rows. */
	{
		struct asic_extintip ext;

		REG32(SWTCR1) = 0;		/* EnL4WayH=0, L4EnHash1=0: deterministic flat table */

		memset(&ext, 0, sizeof(ext));
		ext.externalIP = RTL865X_WAN_EXTIP;	/* 172.16.0.1 = the WAN/masquerade IP */
		ext.valid = 1;			/* internalIP=0 + isOne2One=0 => many-to-one */
		ext.nextHop = 0;		/* -> nexthop[0] (tiny) for the reverse/inbound path */
		rtl865x_asic_write_entry(ASIC_TYPE_EXT_INT_IP, 0, &ext, true);
	}

	/* ---- readback verification ---- */
	seq_puts(m, "readback:\n");
	rtl865x_asic_read_entry(ASIC_TYPE_VLAN, GW_VID_LAN, rb);
	seq_printf(m, "  VLAN2  w0=%08x (mbr/untag/fid)\n", rb[0]);
	rtl865x_asic_read_entry(ASIC_TYPE_VLAN, GW_VID_WAN, rb);
	seq_printf(m, "  VLAN1  w0=%08x\n", rb[0]);
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 0, rb);
	seq_printf(m, "  NETIF0 w0=%08x w1=%08x w3=%08x (LAN VID2)\n", rb[0], rb[1], rb[3]);
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 1, rb);
	seq_printf(m, "  NETIF1 w0=%08x w1=%08x w3=%08x (WAN VID1)\n", rb[0], rb[1], rb[3]);
	rtl865x_asic_read_entry(ASIC_TYPE_L3_ROUTING, 0, rb);
	seq_printf(m, "  ROUTE0 w0=%08x w1=%08x (172.16.0.0/24 process=5 NAPT)\n", rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L3_ROUTING, 1, rb);
	seq_printf(m, "  ROUTE1 w0=%08x w1=%08x (192.168.0.2/32 hal)\n", rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L2_SWITCH, tiny_nh, rb);
	seq_printf(m, "  L2[tiny nh=%u] w0=%08x w1=%08x\n", tiny_nh, rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L2_SWITCH, hal_nh, rb);
	seq_printf(m, "  L2[hal  nh=%u] w0=%08x w1=%08x\n", hal_nh, rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_EXT_INT_IP, 0, rb);
	seq_printf(m, "  EXTIP0 w0=%08x w1=%08x w2=%08x (want w1=ac100001 w2 bit0)\n", rb[0], rb[1], rb[2]);
	/* Phase 3: no static NAPT rows to read back — per-flow rows are dynamic (see
	 * /proc/rtl865x_napt for a live scan of whatever the conntrack hook has installed). */

	/* validate the two netifs read back valid with the right VID */
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 0, rb);
	if (!(rb[0] & 1) || ((rb[0] >> 1) & 0xFFF) != GW_VID_LAN) ok = 0;
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 1, rb);
	if (!(rb[0] & 1) || ((rb[0] >> 1) & 0xFFF) != GW_VID_WAN) ok = 0;
	seq_printf(m, "RESULT %s (gateway datapath %s in ASIC; MSCR=%08x)\n",
		   ok ? "PASS" : "FAIL", ok ? "LIVE" : "not-live", REG32(MSCR));
	pr_info("rtl865x gw: config programmed, netif readback %s\n", ok ? "PASS" : "FAIL");
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

/*
 * M7 level-3 recovery: run the exact same gw scaffolding program WITHOUT the
 * /proc read. SIRR FULL_RST wipes the ASIC TLU tables (netif MACs, L2/L3
 * routes, nexthops, ACL permits) — rtl865x_start() only re-adds VLANs, so
 * after a fabric reset the box drops even small CPU-bound frames until this
 * runs (validated live: MSCR=00000001 + 100% loss until a manual
 * `cat /proc/rtl865x_gw`). Trick: seq_printf/seq_puts on a ZEROED seq_file
 * (count==size==0, buf==NULL) take the overflow path and write nothing (4.14
 * seq_vprintf: `if (m->count < m->size)` is false), so gw_prog's dump lines
 * vanish safely and only its register/table programming happens.
 * Takes rtl865x_hal_lock internally (via gw_prog) — caller must NOT hold it.
 */
int rtl865x_gw_rearm(void)
{
	struct seq_file m;

	memset(&m, 0, sizeof(m));
	return gw_prog(&m, NULL);
}

static int gw_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, gw_prog, NULL);
}

static const struct file_operations gw_proc_fops = {
	.owner   = THIS_MODULE,
	.open    = gw_proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* M6.6 DIAGNOSTIC: read-only scan of all 1024 L4/NAPT rows — dumps every valid
 * entry (Phase 3: including the live 6-bit agingTime, so the conntrack-installed
 * rows and their decay can be watched from userspace). Does NOT reprogram. */
static int napt_scan_show(struct seq_file *m, void *v)
{
	int i, n = 0;
	u32 rb[8];

	mutex_lock(&rtl865x_hal_lock);
	for (i = 0; i < 1024; i++) {
		rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, i, rb);
		if (rb[1] & 1) {	/* word1 bit0 = valid */
			u32 gport = ((rb[1] >> 8) & 0x3f) << 10 | ((rb[1] >> 21) & 0x3ff);

			seq_printf(m, "  [%4d] w0=%08x(intIP) w1=%08x w2=%08x(port/flag) selIP=%u selE=%u off=%u G=0x%x age=%u TCPflag=%u\n",
				   i, rb[0], rb[1], rb[2],
				   (rb[1] >> 17) & 0xf, (rb[1] >> 21) & 0x3ff,
				   (rb[1] >> 8) & 0x3f, gport,
				   (rb[1] >> 2) & 0x3f, (rb[2] >> 16) & 0x7);
			n++;
		}
	}
	mutex_unlock(&rtl865x_hal_lock);
	seq_printf(m, "total valid NAPT rows: %d (dynamic, conntrack-driven)\n", n);
	return 0;
}

static int napt_scan_open(struct inode *inode, struct file *file)
{
	return single_open(file, napt_scan_show, NULL);
}

static const struct file_operations napt_scan_fops = {
	.owner = THIS_MODULE, .open = napt_scan_open,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
};

/* M6.6 A-2 DIAGNOSTIC: dump the switch-fabric descriptor-pool + per-port
 * output-queue state (DESCDIAG block). Under sustained routed bulk the shared
 * 1023-descriptor pool can exhaust (GDSR0 DSCRUNOUT / USEDDSC near the SBFCR
 * runout threshold) and/or a single egress port's output queue (Pn OQ) backs up
 * and latches — this exposes exactly which. `MaxUsedDsc` is a high-water history
 * so it survives a self-recovered latch; read it right after a bulk run to see the
 * peak. NOTE: reading GDSR0/PCSR is (partly) clear-on-read, and the datapath itself
 * already reads GDSR0 in the napi poll and drains PCSR1 every watchdog tick — so
 * transient event FLAGS here are usually already consumed; trust the high-water
 * MaxUsedDsc and the occupancy counters, not the flags. Read-only, no lock
 * (pure register reads, no table access). */
static int fabric_show(struct seq_file *m, void *v)
{
	u32 g0, g1, c0, c1;
	int p;

	/* M7: must hold the HAL lock — the fabric-reset recovery (rtl819x-eth.c
	 * hang_work) gates the switch-core CLOCK for ~600ms under this lock, and
	 * a concurrent read of any 0xBB80xxxx register in that window stalls the
	 * Lexra bus. (Table access is still not needed here; the lock is purely
	 * the clock-gate fence.) */
	mutex_lock(&rtl865x_hal_lock);
	g0 = REG32(GDSR0), g1 = REG32(GDSR1);
	c0 = REG32(PCSR0), c1 = REG32(PCSR1);

	seq_printf(m, "GDSR0=%08x  USEDDSC(now)=%u  MaxUsedDsc(hi)=%u  %s%s%s\n",
		   g0,
		   (g0 & GDSR0_USEDDSC_MASK) >> 16,
		   g0 & GDSR0_MAXUSEDDSC_MASK,
		   (g0 & GDSR0_DSCRUNOUT)   ? "DSCRUNOUT " : "",
		   (g0 & GDSR0_TOTALDSC_FC) ? "TotalDscFC " : "",
		   (g0 & GDSR0_SHAREDBUF_FCON) ? "SharedBufFCON " : "");
	seq_printf(m, "GDSR1=%08x\n", g1);
	/* PCSR0: P0-3 OQ congestion (7 bits/port, bit=queue). PCSR1: P4-6 OQ + IQ. */
	seq_printf(m, "PCSR0=%08x (P0oq=%02x P1oq=%02x P2oq=%02x P3oq=%02x)\n",
		   c0, c0 & 0x7f, (c0 >> 8) & 0x7f, (c0 >> 16) & 0x7f, (c0 >> 24) & 0x7f);
	seq_printf(m, "PCSR1=%08x (P4oq=%02x P5oq=%02x P6oq=%02x IQcong=%02x)\n",
		   c1, c1 & 0x7f, (c1 >> 8) & 0x7f, (c1 >> 16) & 0x7f, (c1 >> 24) & 0x7f);
	/* Per-port descriptor counts: DCR0=OQ0/OQ1, DCR1=OQ2/3, DCR2=OQ4/5, DCR3[9:0]=IQ.
	 * port0 = RGMII trunk (routed egress for BOTH LAN+WAN here); port6 = CPU. */
	for (p = 0; p <= 6; p++) {
		u32 d0 = REG32(Pn_DCR0(p) + 0x00);
		u32 d3 = REG32(Pn_DCR0(p) + 0x0c);
		seq_printf(m, "  P%d: OQ0=%u OQ1=%u  IQ=%u%s\n",
			   p, d0 & 0x3ff, (d0 >> 16) & 0x3ff, d3 & 0x3ff,
			   p == 0 ? "   <- RGMII trunk" : (p == 6 ? "   <- CPU" : ""));
	}
	seq_printf(m, "SBFCR0=%08x SBFCR1=%08x SBFCR2=%08x (S_DSC runout/FCOFF/FCON thresholds)\n",
		   REG32(SBFCR0), REG32(SBFCR1), REG32(SBFCR2));
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

static int fabric_open(struct inode *inode, struct file *file)
{
	return single_open(file, fabric_show, NULL);
}

static const struct file_operations fabric_fops = {
	.owner = THIS_MODULE, .open = fabric_open,
	.read = seq_read, .llseek = seq_lseek, .release = single_release,
};

static int __init rtl865x_asichal_init(void)
{
	proc_create("rtl865x_gw", 0444, NULL, &gw_proc_fops);
	proc_create("rtl865x_napt", 0444, NULL, &napt_scan_fops);
	proc_create("rtl865x_fabric", 0444, NULL, &fabric_fops);
	pr_info("rtl865x asic-hal: /proc/rtl865x_{gw,napt,fabric} ready (M6.6)\n");
	return 0;
}
late_initcall(rtl865x_asichal_init);
