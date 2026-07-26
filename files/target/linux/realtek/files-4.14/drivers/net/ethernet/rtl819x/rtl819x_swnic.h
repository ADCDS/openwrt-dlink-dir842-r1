/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek RTL8197F (RTL819x) CPU-port switch-NIC DMA engine.
 *
 * Ported from the Realtek vendor SDK (rtl819x_swNic.c/.h, Joey Lin,
 * Copyright (c) 2015 Realtek Semiconductor Corp).  The descriptor layout,
 * ownership handling and CDP (current-descriptor-pointer) arithmetic are the
 * real hardware DMA engine and are kept faithful; only the buffer allocator
 * was swapped to standard sk_buff + the streaming DMA API, and the vendor
 * fast-path / rome / l3 / l4 / netif couplings were dropped.
 */

#ifndef _RTL819X_SWNIC_H
#define _RTL819X_SWNIC_H

#include <linux/types.h>

/* The SoC is little-endian; select the LE descriptor bitfield layout. */
#ifndef _LITTLE_ENDIAN
#define _LITTLE_ENDIAN 1
#endif

/* Vendor fixed-width type aliases so the ported engine reads unchanged. */
typedef u8  uint8;
typedef u16 uint16;
typedef u32 uint32;
typedef s8  int8;
typedef s16 int16;
typedef s32 int32;

#define NEW_NIC_MAX_RX_DESC_RING	6
#define NEW_NIC_MAX_TX_DESC_RING	4
#define RTL865X_SWNIC_RXRING_HW_PKTDESC	NEW_NIC_MAX_RX_DESC_RING
#define RTL865X_SWNIC_TXRING_HW_PKTDESC	NEW_NIC_MAX_TX_DESC_RING

/* ---- descriptor shared bits (opts1 low nibble) -------------------------- */
#define DescOwn		(1 << 0)	/* owned by NIC */
#define RingEnd		(1 << 1)	/* end of descriptor ring */
#define LastFrag	(1 << 2)	/* final segment of a packet */
#define FirstFrag	(1 << 3)	/* first segment of a packet */

/* ---- Tx descriptor field offsets/masks (subset actually referenced) ----- */
#define TD_TYPE_OFFSET		29
#define TD_TYPE_MASK		(0x7)
#define TD_PHLEN_OFFSET		6
#define TD_PHLEN_MASK		(0x1FFFF)
#define TD_BRIDGE_OFFSET	5
#define TD_HWLKUP_OFFSET	4
#define TD_M_LEN_OFFSET		15
#define TD_M_LEN_MASK		(0x1FFFF)
#define TD_DP_OFFSET		24
#define TD_DP_MASK		(0x7F)
#define TD_EXTSPA_OFFSET	30
#define TD_L3CS_MASK		(1 << 20)
#define TD_L4CS_MASK		(1 << 19)
#define TD_IPV4_MASK		(1 << 17)
#define TD_IPV4_1ST_MASK	(1 << 16)
#define TD_IPV6_MASK		(1 << 18)
#define TD_LSO_MASK		(1u << 31)
#define TD_VLANTAGSET_MASK	(0x1FF)

/* ---- Rx descriptor field offsets/masks (subset actually referenced) ----- */
#define RD_M_EXTSIZE_OFFSET	16
#define RD_M_EXTSIZE_MASK	(0xFFFF)
#define RD_LEN_OFFSET		0
#define RD_LEN_MASK		(0x3FFF << 0)
#define RD_DVLANID_OFFSET	16
#define RD_DVLANID_MASK		(0xfff << 16)
#define RD_SPA_OFFSET		13
#define RD_SPA_MASK		(0x7 << 13)
#define RD_DPRI_OFFSET		19
#define RD_DPRI_MASK		(0x7 << 19)
#define RD_L3CSOK_MASK		(1u << 31)
#define RD_L4CSOK_MASK		(1u << 30)
#define RD_IPV6_MASK		(1 << 9)
#define RD_REASON_MASK		(0xFFFF << 0)

/* ---- bitfield descriptor views ------------------------------------------ */
struct rx_desc {
#ifdef _LITTLE_ENDIAN
	union {
		struct {
			uint32 own:1;		/* 0 */
			uint32 eor:1;		/* 1 */
			uint32 ls:1;		/* 2 */
			uint32 fs:1;		/* 3 */
			uint32 rcdf:1;		/* 4 */
			uint32 fae:1;		/* 5 */
			uint32 rsvd:10;		/* 6..15 */
			uint32 m_extsize:16;	/* 16..31 */
		} bit;
		uint32 dw;
	} opts1;
	uint32 mdata;
	union {
		struct {
			uint32 len:14;		/* 0..13 */
			uint32 rsvd:3;		/* 14..16 */
			uint32 qid:3;		/* 17..19 */
			uint32 dp_ext:4;	/* 20..23 */
			uint32 extspa:2;	/* 24..25 */
			uint32 rsvd2:6;		/* 26..31 */
		} bit;
		uint32 dw;
	} opts2;
	union {
		struct {
			uint32 reason:16;	/* 0..15 */
			uint32 linkid:7;	/* 16..22 */
			uint32 ppp_idx:3;	/* 23..25 */
			uint32 po:1;		/* 26 */
			uint32 lo:1;		/* 27 */
			uint32 vo:1;		/* 28 */
			uint32 type:3;		/* 29..31 */
		} bit;
		uint32 dw;
	} opts3;
	union {
		struct {
			uint32 tos:8;		/* 0..7 */
			uint32 ipv4:1;		/* 8 */
			uint32 ipv6:1;		/* 9 */
			uint32 ipv4_1st:1;	/* 10 */
			uint32 frag:1;		/* 11 */
			uint32 last_f:1;	/* 12 */
			uint32 spa:3;		/* 13..15 */
			uint32 dvlanid:12;	/* 16..27 */
			uint32 l2act:1;		/* 28 */
			uint32 ext_vlano:3;	/* 29..31 */
		} bit;
		uint32 dw;
	} opts4;
	union {
		struct {
			uint32 svlanid:12;	/* 0..11 */
			uint32 ext_ttl:3;	/* 12..14 */
			uint32 rsvd:1;		/* 15 */
			uint32 spri:3;		/* 16..18 */
			uint32 dpri:3;		/* 19..21 */
			uint32 porg:1;		/* 22 */
			uint32 lorg:1;		/* 23 */
			uint32 vorg:1;		/* 24 */
			uint32 ip_mdf:3;	/* 25..27 */
			uint32 org:1;		/* 28 */
			uint32 fwd:1;		/* 29 */
			uint32 l4csok:1;	/* 30 */
			uint32 l3csok:1;	/* 31 */
		} bit;
		uint32 dw;
	} opts5;
#else
#error "big-endian descriptor layout not ported"
#endif

#define rx_own		opts1.bit.own
#define rx_eor		opts1.bit.eor
#define rx_ls		opts1.bit.ls
#define rx_fs		opts1.bit.fs
#define rx_m_extsize	opts1.bit.m_extsize
#define rx_len		opts2.bit.len
#define rx_spa		opts4.bit.spa
#define rx_dvlanid	opts4.bit.dvlanid
#define rx_ipv6		opts4.bit.ipv6
#define rx_reason	opts3.bit.reason
#define rx_dpri		opts5.bit.dpri
#define rx_l4csok	opts5.bit.l4csok
#define rx_l3csok	opts5.bit.l3csok
};

struct tx_desc {
#ifdef _LITTLE_ENDIAN
	union {
		struct {
			uint32 own:1;		/* 0 */
			uint32 eor:1;		/* 1 */
			uint32 ls:1;		/* 2 */
			uint32 fs:1;		/* 3 */
			uint32 hwlkup:1;	/* 4 */
			uint32 bridge:1;	/* 5 */
			uint32 ph_len:17;	/* 6..22 */
			uint32 pppidx:3;	/* 23..25 */
			uint32 pi:1;		/* 26 */
			uint32 li:1;		/* 27 */
			uint32 vi:1;		/* 28 */
			uint32 type:3;		/* 29..31 */
		} bit;
		uint32 dw;
	} opts1;
	uint32 mdata;
	union {
		struct {
			uint32 vlantag:9;	/* 0..8 */
			uint32 pqid:3;		/* 9..11 */
			uint32 qid:3;		/* 12..14 */
			uint32 mlen:17;		/* 15..31 */
		} bit;
		uint32 dw;
	} opts2;
	union {
		struct {
			uint32 dvlanid:12;	/* 0..11 */
			uint32 dp_ext:3;	/* 12..14 */
			uint32 rsvd:1;		/* 15 */
			uint32 ipv4_1st:1;	/* 16 */
			uint32 ipv4:1;		/* 17 */
			uint32 ipv6:1;		/* 18 */
			uint32 l4cs:1;		/* 19 */
			uint32 l3cs:1;		/* 20 */
			uint32 po:1;		/* 21 */
			uint32 dpri:3;		/* 22..24 */
			uint32 ptp_ver:2;	/* 25..26 */
			uint32 ptp_type:4;	/* 27..30 */
			uint32 ptp_pkt:1;	/* 31 */
		} bit;
		uint32 dw;
	} opts3;
	union {
		struct {
			uint32 ipv6_hdrlen:16;	/* 0..15 */
			uint32 linkid:7;	/* 16..22 */
			uint32 rsvd:1;		/* 23 */
			uint32 dp:7;		/* 24..30 */
			uint32 lso:1;		/* 31 */
		} bit;
		uint32 dw;
	} opts4;
	union {
		struct {
			uint32 tcp_hdrlen:4;	/* 0..3 */
			uint32 ipv4_hdrlen:4;	/* 4..7 */
			uint32 flags:8;		/* 8..15 */
			uint32 mss:14;		/* 16..29 */
			uint32 extspa:2;	/* 30..31 */
		} bit;
		uint32 dw;
	} opts5;
#else
#error "big-endian descriptor layout not ported"
#endif

#define tx_own		opts1.bit.own
#define tx_eor		opts1.bit.eor
#define tx_ls		opts1.bit.ls
#define tx_fs		opts1.bit.fs
#define tx_hwlkup	opts1.bit.hwlkup
#define tx_bridge	opts1.bit.bridge
#define tx_ph_len	opts1.bit.ph_len
#define tx_type		opts1.bit.type
#define tx_vlantag	opts2.bit.vlantag
#define tx_mlen		opts2.bit.mlen
#define tx_dvlanid	opts3.bit.dvlanid
#define tx_ipv4_1st	opts3.bit.ipv4_1st
#define tx_ipv4		opts3.bit.ipv4
#define tx_ipv6		opts3.bit.ipv6
#define tx_l4cs		opts3.bit.l4cs
#define tx_l3cs		opts3.bit.l3cs
#define tx_dpri		opts3.bit.dpri
#define tx_dp		opts4.bit.dp
#define tx_lso		opts4.bit.lso
#define tx_extspa	opts5.bit.extspa
#define tx_mss		opts5.bit.mss
};

/* Flat 6-doubleword descriptor views (used for raw copies / base setup). */
typedef struct dma_tx_desc {
	uint32 opts1;
	uint32 addr;
	uint32 opts2;
	uint32 opts3;
	uint32 opts4;
	uint32 opts5;
} DMA_TX_DESC;

typedef struct dma_rx_desc {
	uint32 opts1;
	uint32 addr;
	uint32 opts2;
	uint32 opts3;
	uint32 opts4;
	uint32 opts5;
} DMA_RX_DESC;

/* ---- RTL8197F two-level pkthdr+mbuf RX/TX DMA scheme --------------------- *
 * The RTL8197F CPU port does NOT use the flat single-level descriptor above;
 * that was a mis-port and left the mbuf ring (CPURMDCR0) unset, so enabling
 * switch forwarding made the ASIC DMA received frames through a garbage mbuf
 * pointer and corrupt kernel memory. The real engine (vendor rtl865xc_swNic.c)
 * uses:  CPURPDCR0 -> a ring of (phys pkthdr ptr | own);  CPURMDCR0 -> a ring
 * of (phys mbuf ptr | own).  Each rtl_pktHdr.ph_mbuf -> an rtl_mBuf whose
 * m_data/m_extbuf -> the 2 KB cluster the frame is DMA'd into.  Both structs
 * are exactly 32 bytes; the field OFFSETS below match vendor common/mbuf.h for
 * the little-endian RTL8197F build (only the fields HW or this driver touch are
 * named; the rest is reserved padding preserving the 32-byte size/offsets).
 * All pointer fields the ASIC follows (ph_mbuf, m_pkthdr, m_data, m_extbuf) are
 * PHYSICAL/bus addresses; the driver reaches the structs via their uncached
 * (dma_alloc_coherent) virtual mappings. */
struct rtl_pktHdr {
	uint32 ph_mbuf;		/* +0  phys ptr to the paired rtl_mBuf */
	uint16 ph_asic0;	/* +4  srcExtPortNum(0..1)/extPortList(8..11)/queueId */
	uint16 ph_len;		/* +6  total packet length incl FCS (HW writes on RX) */
	uint16 ph_reason;	/* +8  RX: reason code */
	uint16 ph_asic1;	/* +10 linkID/tag/type bitfields */
	uint8  ph_portlist;	/* +12 RX: source port (&0x7) / TX: dest portmask */
	uint8  ph_orgtos;	/* +13 */
	uint16 ph_flags;	/* +14 PKTHDR_* status bits */
	uint16 ph_flags2;	/* +16 */
	uint16 ph_vlanId;	/* +18 low 12 bits = VLAN id */
	uint16 ph_ipv6hdrlen;	/* +20 */
	uint8  ph_ipbits;	/* +22 */
	uint8  ph_ptpbits;	/* +23 */
	uint32 ph_pending0;	/* +24 sw scratch / cache-line pad */
	uint32 ph_pending1;	/* +28 */
};

struct rtl_mBuf {
	uint32 m_next;		/* +0  phys ptr to next mbuf (0: single cluster) */
	uint32 m_pkthdr;	/* +4  phys ptr back to the pkthdr */
	uint8  m_reserved2;	/* +8 */
	uint8  m_flags;		/* +9  MBUF_* */
	uint16 m_len;		/* +10 data bytes in cluster (HW writes on RX) */
	uint32 m_data;		/* +12 phys ptr: data location in cluster */
	uint32 m_extbuf;	/* +16 phys ptr: start of the cluster */
	uint8  m_reserved[2];	/* +20 */
	uint16 m_extsize;	/* +22 cluster size in bytes */
	uint32 skb;		/* +24 driver: the sk_buff (virtual ptr) */
	uint32 m_pending0;	/* +28 cache-line pad */
};

/* mbuf/pkthdr software flags (vendor common/mbuf.h) */
#define MBUF_USED		0x02
#define MBUF_EOR		0x04
#define MBUF_PKTHDR		0x08
#define MBUF_EXT		0x10
#define PKTHDR_USED		0x0200
#define PKT_INCOMING		0x1000
#define PKT_OUTGOING		0x0800
#define PKTHDR_BRIDGING		0x0040	/* HW L2-bridge assist (with PKTHDR_HWLOOKUP) */

/*
 * Bug #13 — RTL8197F "used/own" software-flag positions. The MBUF_USED /
 * PKTHDR_USED values above are the legacy RTL865x bit positions of a
 * half-ported flag table; on the 8197F the used bits moved to 0x80 / 0x8000
 * (vendor common/mbuf.h, 8197F branch). The proven-working RX pre-seed
 * (New_swNic_init / receive refill) is built on the legacy values and the
 * fragile large-frame RX path is tuned around it, so those macros MUST NOT
 * be changed. These _8197F constants are for the TX send path ONLY:
 * ph_flags = PKTHDR_USED_8197F | PKT_OUTGOING (= 0x8800) is the vendor
 * direct-TX recipe that makes the switch egress ph_portlist VERBATIM (no L2
 * lookup) — required for box-originated cold-FDB UNICAST to reach the RGMII
 * trunk — and it only works when m_flags also carries the 0x80 own bit
 * (m_flags = 0x9C); with the legacy 0x02 instead, the OUTGOING descriptor
 * path rejects the buffer and wedges ALL TX.
 */
#define MBUF_USED_8197F		0x80
#define PKTHDR_USED_8197F	0x8000

/* ---- trimmed NIC Rx/Tx info structs ------------------------------------- *
 * Only the fields the ported engine actually touches are kept.
 */
typedef struct {
	uint16 vid;
	uint16 pid;
	uint16 len;
	uint16 priority:3;
	uint16 rxPri:3;
	void  *input;		/* the received sk_buff */
} rtl_nicRx_info;

typedef struct {
	uint16 vid;
	uint16 portlist;
	uint16 flags;
	uint16 txIdx:2;
	uint16 priority:3;
	uint16 tagport;
} rtl_nicTx_info;

/* Return codes for the receive path (vendor values). */
#define RTL_NICRX_OK		0
#define RTL_NICRX_NULL		-1

/* ---- exported engine API ------------------------------------------------ */
extern uint32 size_of_cluster;

/*
 * Set the net_device the engine allocates Rx sk_buffs against and DMA-maps
 * through.  Must be called before New_swNic_init().
 */
void New_swNic_setDev(struct net_device *dev);

int32 New_swNic_init(uint32 userNeedRxPkthdrRingCnt[NEW_NIC_MAX_RX_DESC_RING],
		     uint32 userNeedTxPkthdrRingCnt[NEW_NIC_MAX_TX_DESC_RING],
		     uint32 clusterSize);
int32 New_swNic_send(void *skb, void *output, uint32 len, rtl_nicTx_info *nicTx);
int32 New_swNic_receive(rtl_nicRx_info *info, int retryCount);
int32 New_swNic_rxPending(void);
int32 New_swNic_txDone(int idx);
void New_swNic_freeRxBuf(void);
void New_swNic_freeRings(void);

/* M7 FCS wedge signal: cumulative software-FCS verdicts for large (>132 B)
 * delivered RX frames (see rtl819x_swnic.c). Windowed by the eth watchdog's
 * wedge detector. Same-CPU softirq increment/read — plain u32 is fine. */
extern u32 rtl819x_rx_fcs_ok;
extern u32 rtl819x_rx_fcs_fail;

#endif /* _RTL819X_SWNIC_H */
