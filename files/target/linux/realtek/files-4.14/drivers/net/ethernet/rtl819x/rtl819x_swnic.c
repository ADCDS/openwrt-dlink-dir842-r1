// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8197F (RTL819x) CPU-port switch-NIC DMA engine.
 *
 * Ported from the Realtek vendor SDK rtl865xc_swNic.c (the REAL RTL8197F CPU
 * RX/TX engine).  Earlier revisions of this file mis-ported the single-level
 * rtl819x_swNic.c descriptor scheme (pkthdr.addr -> buffer); the RTL8197F
 * actually uses a TWO-level pkthdr-ring + mbuf-ring scheme, and leaving the
 * mbuf ring (CPURMDCR0) unset made the switch DMA received frames through a
 * garbage mbuf pointer and corrupt kernel memory the instant L2 forwarding was
 * enabled.  This version implements the correct scheme:
 *
 *   CPURPDCR0 -> rxPkthdrRing : u32[] of (phys(rtl_pktHdr) | own)
 *   CPURMDCR0 -> rxMbufRing   : u32[] of (phys(rtl_mBuf)   | own)
 *   CPUTPDCR0 -> txPkthdrRing : u32[] of (phys(rtl_pktHdr) | own)
 *   each rtl_pktHdr.ph_mbuf -> an rtl_mBuf ;  mbuf.m_data/m_extbuf -> a 2KB
 *   cluster (an sk_buff, streaming-DMA-mapped).
 *
 * The pkthdr pool, mbuf pool and the three descriptor rings are allocated with
 * dma_alloc_coherent (uncached on this non-coherent MIPS SoC); the driver
 * reaches those structs through their uncached virtual mapping while every
 * pointer field the ASIC follows holds the PHYSICAL/bus address.  Cluster data
 * uses the streaming DMA API (dma_map_single / dma_unmap_single).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/dma-mapping.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <asm/cpu-features.h>

#include "rtl819x_regs.h"
#include "rtl819x_swnic.h"

#ifndef SUCCESS
#define SUCCESS		0
#endif
#ifndef FAILED
#define FAILED		1
#endif

/* On UP this SoC serialises the engine with irq-off spinlocks. */
static DEFINE_SPINLOCK(swnic_tx_lock);
static DEFINE_SPINLOCK(swnic_rx_lock);
#define SMP_LOCK_ETH_XMIT(f)	spin_lock_irqsave(&swnic_tx_lock, (f))
#define SMP_UNLOCK_ETH_XMIT(f)	spin_unlock_irqrestore(&swnic_tx_lock, (f))
#define SMP_LOCK_ETH_RECV(f)	spin_lock_irqsave(&swnic_rx_lock, (f))
#define SMP_UNLOCK_ETH_RECV(f)	spin_unlock_irqrestore(&swnic_rx_lock, (f))

#define NEXT_IDX(N, RING_SIZE)	(((N) + 1 == (RING_SIZE)) ? 0 : (N) + 1)

/* net_device / DMA parent the engine allocates and maps against. */
static struct net_device *swnic_dev;
static struct device *swnic_dmadev;

uint32 size_of_cluster;

/* skb + streaming-DMA-handle bookkeeping, one entry per ring slot. */
struct ring_info {
	unsigned int	skb;	/* struct sk_buff * (as int) */
	dma_addr_t	dma;	/* streaming-DMA handle for skb->data */
};

/*
 * A coherent allocation: the driver-visible (uncached) virtual base, the
 * physical/bus base the ASIC uses, the original cookie for dma_free_coherent
 * and the byte size.
 */
struct coh {
	void	   *orig;	/* dma_alloc_coherent() return (for free) */
	dma_addr_t  phys;	/* bus/physical base */
	size_t	    sz;
};

/* Uncached access pointer (KSEG1) for a coherent block / element. */
#define COH_V(c)	((void *)(((uintptr_t)(c).orig) | UNCACHE_MASK))

static void *coh_alloc(struct coh *c, size_t sz)
{
	c->orig = dma_alloc_coherent(swnic_dmadev, sz, &c->phys, GFP_KERNEL);
	c->sz = sz;
	if (c->orig)
		memset(c->orig, 0, sz);
	return c->orig;
}

static void coh_free(struct coh *c)
{
	if (c->orig)
		dma_free_coherent(swnic_dmadev, c->sz, c->orig, c->phys);
	c->orig = NULL;
	c->phys = 0;
	c->sz = 0;
}

/* ---- ring 0 state (the eth driver only uses ring 0) --------------------- */
static struct coh rxPh, rxMb, rxRing, rxMring;	/* pkthdr pool, mbuf pool, two rings */
static struct coh txPh, txMb, txRing;
static uint32 rxCnt, txCnt;
static uint32 rxCurr;			/* next rx pkthdr the CPU will inspect */
static uint32 txCurr, txDoneIdx;	/* tx produce / reclaim indices */
static struct ring_info *rx_ri;		/* [rxCnt] */
static struct ring_info *tx_ri;		/* [txCnt] */

/* Element accessors: struct via uncached virt, phys via bus base. */
static inline struct rtl_pktHdr *RXPH(uint32 i)
{ return (struct rtl_pktHdr *)((u8 *)COH_V(rxPh) + i * sizeof(struct rtl_pktHdr)); }
static inline struct rtl_mBuf *RXMB(uint32 i)
{ return (struct rtl_mBuf *)((u8 *)COH_V(rxMb) + i * sizeof(struct rtl_mBuf)); }
static inline struct rtl_pktHdr *TXPH(uint32 i)
{ return (struct rtl_pktHdr *)((u8 *)COH_V(txPh) + i * sizeof(struct rtl_pktHdr)); }
static inline struct rtl_mBuf *TXMB(uint32 i)
{ return (struct rtl_mBuf *)((u8 *)COH_V(txMb) + i * sizeof(struct rtl_mBuf)); }
static inline volatile uint32 *RXR(void) { return (volatile uint32 *)COH_V(rxRing); }
static inline volatile uint32 *RXMR(void) { return (volatile uint32 *)COH_V(rxMring); }
static inline volatile uint32 *TXR(void) { return (volatile uint32 *)COH_V(txRing); }

static inline uint32 RXPH_P(uint32 i) { return (uint32)rxPh.phys + i * sizeof(struct rtl_pktHdr); }
static inline uint32 RXMB_P(uint32 i) { return (uint32)rxMb.phys + i * sizeof(struct rtl_mBuf); }
static inline uint32 TXPH_P(uint32 i) { return (uint32)txPh.phys + i * sizeof(struct rtl_pktHdr); }
static inline uint32 TXMB_P(uint32 i) { return (uint32)txMb.phys + i * sizeof(struct rtl_mBuf); }

void New_swNic_setDev(struct net_device *dev)
{
	swnic_dev = dev;
	swnic_dmadev = dev ? dev->dev.parent : NULL;
}

/*
 * Allocate one Rx cluster as an sk_buff and DMA-map it FROM_DEVICE.  Returns
 * the bus address (what the ASIC DMAs into) in *pdma and the sk_buff in *pskb.
 */
static unsigned char *alloc_rx_buf(void **pskb, u32 size, dma_addr_t *pdma)
{
	struct sk_buff *skb;
	dma_addr_t dma;

	skb = netdev_alloc_skb(swnic_dev, size);
	if (!skb) {
		*pskb = NULL;
		return NULL;
	}

	dma = dma_map_single(swnic_dmadev, skb->data, size, DMA_FROM_DEVICE);
	if (dma_mapping_error(swnic_dmadev, dma)) {
		dev_kfree_skb_any(skb);
		*pskb = NULL;
		return NULL;
	}

	*pskb = skb;
	if (pdma)
		*pdma = dma;
	return (unsigned char *)(uintptr_t)dma;
}

static void free_all_coherent(void)
{
	coh_free(&rxPh);
	coh_free(&rxMb);
	coh_free(&rxRing);
	coh_free(&rxMring);
	coh_free(&txPh);
	coh_free(&txMb);
	coh_free(&txRing);
	kfree(rx_ri);	rx_ri = NULL;
	kfree(tx_ri);	tx_ri = NULL;
}

int32 New_swNic_init(uint32 userNeedRxPkthdrRingCnt[NEW_NIC_MAX_RX_DESC_RING],
		     uint32 userNeedTxPkthdrRingCnt[NEW_NIC_MAX_TX_DESC_RING],
		     uint32 clusterSize)
{
	volatile uint32 *rr, *mr, *tr;
	uint32 j;

	BUILD_BUG_ON(sizeof(struct rtl_pktHdr) != 32);
	BUILD_BUG_ON(sizeof(struct rtl_mBuf) != 32);

	/* No lock: New_swNic_init() runs from ndo_open before napi/timer start,
	 * so there is no concurrent receive; and the allocations below may sleep
	 * (dma_alloc_coherent/GFP_KERNEL), which must not happen under the
	 * irq-off Rx spinlock. */
	size_of_cluster = clusterSize;
	rxCnt = userNeedRxPkthdrRingCnt[0];
	txCnt = userNeedTxPkthdrRingCnt[0];

	/* Allocate the pools + rings once (a re-open after New_swNic_freeRings()
	 * finds them NULL and re-allocates). */
	if (!rxPh.orig) {
		if (!coh_alloc(&rxPh, rxCnt * sizeof(struct rtl_pktHdr)) ||
		    !coh_alloc(&rxMb, rxCnt * sizeof(struct rtl_mBuf))    ||
		    !coh_alloc(&rxRing, rxCnt * sizeof(uint32))           ||
		    !coh_alloc(&rxMring, rxCnt * sizeof(uint32))          ||
		    !coh_alloc(&txPh, txCnt * sizeof(struct rtl_pktHdr))  ||
		    !coh_alloc(&txMb, txCnt * sizeof(struct rtl_mBuf))    ||
		    !coh_alloc(&txRing, txCnt * sizeof(uint32)))
			goto err_out;

		rx_ri = kcalloc(rxCnt, sizeof(*rx_ri), GFP_KERNEL);
		tx_ri = kcalloc(txCnt, sizeof(*tx_ri), GFP_KERNEL);
		if (!rx_ri || !tx_ri)
			goto err_out;
	}

	rr = RXR();
	mr = RXMR();
	tr = TXR();

	/* ---- TX ring: pkthdr<->mbuf linked, CPU-owned (free) ---------------- */
	for (j = 0; j < txCnt; j++) {
		struct rtl_pktHdr *ph = TXPH(j);
		struct rtl_mBuf *mb = TXMB(j);

		ph->ph_mbuf = TXMB_P(j);
		ph->ph_len = 0;
		ph->ph_flags = PKTHDR_USED | PKT_OUTGOING;
		ph->ph_portlist = 0;
		mb->m_next = 0;
		mb->m_pkthdr = TXPH_P(j);
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_data = 0;
		mb->m_extbuf = 0;
		mb->m_extsize = 0;
		mb->skb = 0;

		tr[j] = TXPH_P(j) | DESC_RISC_OWNED;	/* CPU owns until it Tx's */
		tx_ri[j].skb = 0;
		tx_ri[j].dma = 0;
	}
	if (txCnt)
		tr[txCnt - 1] |= DESC_WRAP;
	txCurr = 0;
	txDoneIdx = 0;

	/* ---- RX ring: pkthdr<->mbuf<->cluster, switch-core owned ------------ */
	for (j = 0; j < rxCnt; j++) {
		struct rtl_pktHdr *ph = RXPH(j);
		struct rtl_mBuf *mb = RXMB(j);
		void *skb = NULL;
		dma_addr_t dma = 0;
		unsigned char *buf;

		/* free any stale cluster from a prior init without free */
		if (rx_ri[j].skb) {
			if (rx_ri[j].dma)
				dma_unmap_single(swnic_dmadev, rx_ri[j].dma,
						 size_of_cluster, DMA_FROM_DEVICE);
			dev_kfree_skb_any((struct sk_buff *)(uintptr_t)rx_ri[j].skb);
			rx_ri[j].skb = 0;
			rx_ri[j].dma = 0;
		}

		buf = alloc_rx_buf(&skb, clusterSize, &dma);
		if (!buf)
			goto err_out;

		ph->ph_mbuf = RXMB_P(j);
		ph->ph_len = 0;
		ph->ph_flags = PKTHDR_USED | PKT_INCOMING;
		ph->ph_portlist = 0;
		mb->m_next = 0;
		mb->m_pkthdr = RXPH_P(j);
		mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
		mb->m_len = 0;
		mb->m_data = (uint32)dma;
		mb->m_extbuf = (uint32)dma;
		mb->m_extsize = clusterSize;
		mb->skb = (uint32)(uintptr_t)skb;

		rx_ri[j].skb = (uintptr_t)skb;
		rx_ri[j].dma = dma;

		rr[j] = RXPH_P(j) | DESC_SWCORE_OWNED;	/* HW owns, waiting for pkt */
		mr[j] = RXMB_P(j) | DESC_SWCORE_OWNED;
	}
	if (rxCnt) {
		rr[rxCnt - 1] |= DESC_WRAP;
		mr[rxCnt - 1] |= DESC_WRAP;
	}
	rxCurr = 0;

	wmb();

	/* ---- program the CPU-interface ring registers (physical bases) ------ */
	REG32(CPUTPDCR0) = (uint32)txRing.phys;
	REG32(CPURPDCR0) = (uint32)rxRing.phys;
	/* Unused rx rings 1..5: point at ring 0 (valid memory) so a stray
	 * priority selection can never hit a null/stale pointer. The switch
	 * defaults to ring 0, so these are not normally advanced. */
	REG32(CPURPDCR1) = (uint32)rxRing.phys;
	REG32(CPURPDCR2) = (uint32)rxRing.phys;
	REG32(CPURPDCR3) = (uint32)rxRing.phys;
	REG32(CPURPDCR4) = (uint32)rxRing.phys;
	REG32(CPURPDCR5) = (uint32)rxRing.phys;
	REG32(CPURMDCR0) = (uint32)rxMring.phys;

	return SUCCESS;

err_out:
	New_swNic_freeRings();
	return -EINVAL;
}

/*
 * Pull one received frame off Rx ring 0.  A descriptor whose ring-slot own bit
 * is CPU-owned (cleared by the switch) holds a frame: follow the pkthdr, hand
 * up its cluster sk_buff, refill the slot with a fresh mapped cluster and
 * re-arm both the pkthdr and mbuf descriptors for the switch.
 */
int32 New_swNic_receive(rtl_nicRx_info *info, int retryCount)
{
	unsigned long flags = 0;
	volatile uint32 *rr, *mr;
	int loops = 0;

	SMP_LOCK_ETH_RECV(flags);
	rr = RXR();
	mr = RXMR();

	for (;;) {
		struct rtl_pktHdr *ph;
		struct rtl_mBuf *mb;
		struct sk_buff *r_skb;
		void *nskb = NULL;
		dma_addr_t ndma = 0;
		unsigned char *nbuf;
		uint32 idx, slot, len;

		if (++loops > 256)	/* bound: never spin holding the Rx lock */
			break;

		idx = rxCurr;
		slot = rr[idx];
		if ((slot & DESC_OWNED_BIT) == DESC_SWCORE_OWNED) {
			/* switch core still owns it -> no frame */
			SMP_UNLOCK_ETH_RECV(flags);
			return RTL_NICRX_NULL;
		}

		ph = RXPH(idx);
		mb = RXMB(idx);
		len = ph->ph_len;	/* includes 4-byte FCS */

		{
			static int rxt;
			if (rxt < 40) {
				rxt++;
				pr_err("swnic rx#%d idx=%u slot=%08x ph_len=%u port=%02x reason=%04x mlen=%u mdata=%08x\n",
				       rxt, idx, slot, ph->ph_len, ph->ph_portlist,
				       ph->ph_reason, mb->m_len, mb->m_data);
			}
		}

		/* Belt-and-suspenders: never skb_put an implausible length. */
		if (len < 15 || len > size_of_cluster) {
			mb->m_len = 0;
			wmb();
			mr[idx] |= DESC_SWCORE_OWNED;
			rr[idx] |= DESC_SWCORE_OWNED;
			rxCurr = NEXT_IDX(idx, rxCnt);
			continue;
		}

		r_skb = (struct sk_buff *)(uintptr_t)rx_ri[idx].skb;
		if (!r_skb) {		/* should not happen */
			mb->m_len = 0;
			wmb();
			mr[idx] |= DESC_SWCORE_OWNED;
			rr[idx] |= DESC_SWCORE_OWNED;
			rxCurr = NEXT_IDX(idx, rxCnt);
			continue;
		}

		/* Allocate the replacement cluster before releasing this one. */
		nbuf = alloc_rx_buf(&nskb, size_of_cluster, &ndma);
		if (!nbuf) {
			/* out of buffers: leave the frame, don't advance */
			SMP_UNLOCK_ETH_RECV(flags);
			return RTL_NICRX_NULL;
		}

		/* Hand the received cluster up. */
		dma_unmap_single(swnic_dmadev, rx_ri[idx].dma,
				 size_of_cluster, DMA_FROM_DEVICE);
		info->input = (void *)r_skb;
		info->len = len - 4;
		info->pid = ph->ph_portlist & 0x7;
		info->vid = ph->ph_vlanId & 0x0fff;
		pr_err_once("rtl819x DP: first RX frame (len=%d)\n", info->len);

		/* Install the fresh cluster into the mbuf + bookkeeping. */
		rx_ri[idx].skb = (uintptr_t)nskb;
		rx_ri[idx].dma = ndma;
		mb->m_data = (uint32)ndma;
		mb->m_extbuf = (uint32)ndma;
		mb->m_len = 0;
		mb->m_extsize = size_of_cluster;
		mb->skb = (uint32)(uintptr_t)nskb;

		/* Re-arm mbuf first, then pkthdr, then advance. */
		wmb();
		mr[idx] |= DESC_SWCORE_OWNED;
		rr[idx] |= DESC_SWCORE_OWNED;
		rxCurr = NEXT_IDX(idx, rxCnt);

		REG32(CPUIISR) = (MBUF_DESC_RUNOUT_IP_ALL | PKTHDR_DESC_RUNOUT_IP_ALL);
		SMP_UNLOCK_ETH_RECV(flags);
		return RTL_NICRX_OK;
	}

	SMP_UNLOCK_ETH_RECV(flags);
	return RTL_NICRX_NULL;
}

static int32 _New_swNic_send(void *skb, void *output, uint32 len,
			     rtl_nicTx_info *nicTx)
{
	volatile uint32 *tr = TXR();
	struct rtl_pktHdr *ph;
	struct rtl_mBuf *mb;
	uint32 idx = txCurr, next;

	next = NEXT_IDX(idx, txCnt);
	if (next == txDoneIdx)
		return FAILED;			/* ring full */
	if ((tr[idx] & DESC_OWNED_BIT) != DESC_RISC_OWNED)
		return FAILED;			/* slot not reclaimed yet */

	ph = TXPH(idx);
	mb = TXMB(idx);

	/* Pad small packets; hardware appends the CRC. */
	if (len < 60)
		len = 64;
	else
		len += 4;

	mb->m_data = (uint32)(uintptr_t)output;
	mb->m_extbuf = (uint32)(uintptr_t)output;
	mb->m_len = len;
	mb->m_extsize = len;
	mb->m_next = 0;
	mb->m_flags = MBUF_USED | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
	mb->skb = (uint32)(uintptr_t)skb;

	ph->ph_len = len;
	ph->ph_vlanId = nicTx->vid & 0x0fff;
	ph->ph_portlist = nicTx->portlist & 0x1f;
	ph->ph_flags = nicTx->flags;
	ph->ph_asic0 = 0;
	ph->ph_asic1 = 0;
	ph->ph_ptpbits = 0;
	if (nicTx->flags & PKTHDR_HWLOOKUP) {
		/* CPU-injected frame, let the switch L2-bridge it (translation of
		 * the old tx_hwlkup|tx_bridge|tx_extspa=CPU descriptor bits). */
		ph->ph_flags |= PKTHDR_BRIDGING;
		ph->ph_asic0 = (3 << 0);	/* ph_srcExtPortNum = CPU */
	}

	tx_ri[idx].skb = (uintptr_t)skb;
	tx_ri[idx].dma = (dma_addr_t)(uintptr_t)output;

	/* Publish descriptor contents, then hand the slot to the switch core. */
	wmb();
	tr[idx] |= DESC_SWCORE_OWNED;
	txCurr = next;

	/* Ring the Tx doorbell (8197F: clear bit29 first, per vendor). */
	REG32(CPUICR) = REG32(CPUICR) & ~(1u << 29);
	REG32(CPUICR) |= TXFD;
	return SUCCESS;
}

int32 New_swNic_send(void *skb, void *output, uint32 len, rtl_nicTx_info *nicTx)
{
	int ret;
	unsigned long flags = 0;

	SMP_LOCK_ETH_XMIT(flags);
	ret = _New_swNic_send(skb, output, len, nicTx);
	SMP_UNLOCK_ETH_XMIT(flags);
	return ret;
}

/*
 * Reclaim completed Tx descriptors: a slot the switch core has transmitted is
 * handed back CPU-owned (own bit cleared).  Walk from txDoneIdx toward txCurr,
 * freeing the stashed sk_buffs, stopping at the first still-pending slot.
 */
int32 New_swNic_txDone(int idx)
{
	unsigned long flags = 0;
	volatile uint32 *tr;
	int loops = 0;

	SMP_LOCK_ETH_XMIT(flags);
	tr = TXR();

	while (txDoneIdx != txCurr) {
		struct sk_buff *skb;

		if (++loops > (int)txCnt) {
			pr_err_ratelimited("swnic txDone: loop bound (cnt=%u) hit\n",
					   txCnt);
			break;
		}

		/* still switch-owned -> not yet transmitted, stop here */
		if ((tr[txDoneIdx] & DESC_OWNED_BIT) != DESC_RISC_OWNED)
			break;

		skb = (struct sk_buff *)(uintptr_t)tx_ri[txDoneIdx].skb;
		if (skb) {
			if (tx_ri[txDoneIdx].dma)
				dma_unmap_single(swnic_dmadev, tx_ri[txDoneIdx].dma,
						 skb->len, DMA_TO_DEVICE);
			SMP_UNLOCK_ETH_XMIT(flags);
			dev_kfree_skb_any(skb);
			SMP_LOCK_ETH_XMIT(flags);
			tx_ri[txDoneIdx].skb = 0;
			tx_ri[txDoneIdx].dma = 0;
			TXMB(txDoneIdx)->skb = 0;
		}

		txDoneIdx = NEXT_IDX(txDoneIdx, txCnt);
	}

	SMP_UNLOCK_ETH_XMIT(flags);
	return 0;
}

/* Free the Rx cluster sk_buffs (leave the coherent pools/rings intact). */
void New_swNic_freeRxBuf(void)
{
	uint32 j;

	txCurr = txDoneIdx = 0;

	if (rx_ri) {
		for (j = 0; j < rxCnt; j++) {
			struct sk_buff *skb =
				(struct sk_buff *)(uintptr_t)rx_ri[j].skb;
			if (skb) {
				if (rx_ri[j].dma)
					dma_unmap_single(swnic_dmadev, rx_ri[j].dma,
							 size_of_cluster,
							 DMA_FROM_DEVICE);
				dev_kfree_skb_any(skb);
				rx_ri[j].skb = 0;
				rx_ri[j].dma = 0;
			}
		}
	}
	rxCurr = 0;
}

/* Tear the rings down entirely (driver remove / open-failure unwind). */
void New_swNic_freeRings(void)
{
	uint32 j;

	New_swNic_freeRxBuf();

	if (tx_ri) {
		for (j = 0; j < txCnt; j++) {
			struct sk_buff *skb =
				(struct sk_buff *)(uintptr_t)tx_ri[j].skb;
			if (skb) {
				if (tx_ri[j].dma)
					dma_unmap_single(swnic_dmadev, tx_ri[j].dma,
							 skb->len, DMA_TO_DEVICE);
				dev_kfree_skb_any(skb);
				tx_ri[j].skb = 0;
				tx_ri[j].dma = 0;
			}
		}
	}

	free_all_coherent();
}
