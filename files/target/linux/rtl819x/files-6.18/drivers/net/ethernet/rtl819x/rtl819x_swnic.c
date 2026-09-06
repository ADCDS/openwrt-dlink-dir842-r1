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
#include <linux/crc32.h>	/* M7: software FCS verify (wedge detector) */
#include <linux/unaligned.h>
#include <asm/cpu-features.h>

#include "rtl819x_regs.h"
#include "rtl819x_swnic.h"

#ifndef SUCCESS
#define SUCCESS		0
#endif
#ifndef FAILED
#define FAILED		1
#endif

/*
 * M7 large-frame-corruption discriminator: `echo N > /sys/module/rtl819x/
 * parameters/rx_dump` dumps the next N received LARGE (>132 B) frames from
 * inside the RX path (tcpdump/AF_PACKET is blind on this driver). For each
 * frame it prints the descriptor view (ph_len vs mbuf m_len/m_next/m_data) and
 * hexdumps head+tail of the cluster through BOTH the cached kernel mapping and
 * the uncached KSEG1 alias of the same physical cluster, plus the first
 * cached-vs-uncached differing offset. Discriminates the three candidate
 * mechanisms of the wedge measured on the bench (large frames arrive with
 * correct ph_len but corrupt payload):
 *   - m_next != 0 or m_len < ph_len       -> HW split the frame across mbufs
 *     (gather engine latch; driver assumes single-mbuf like stock does)
 *   - cached == uncached, both stale      -> data never reached DRAM (NIC FIFO
 *     /bus-ordering latch; descriptor overtook the payload writes)
 *   - cached stale, uncached correct      -> CPU cache staleness (would indict
 *     the DMA API path; not expected - fresh boots are 100% clean)
 * Run with a known payload pattern (ping -p) so stale vs true data is obvious.
 */
int rtl819x_rx_dump;
module_param_named(rx_dump, rtl819x_rx_dump, int, 0644);
MODULE_PARM_DESC(rx_dump, "hexdump the next N large RX frames (cached vs uncached view)");

/* CPU-tag bring-up: print the source port of the next N received frames. */
static int pid_dump;
module_param(pid_dump, int, 0644);
MODULE_PARM_DESC(pid_dump, "log source port/vid/asic0 for the next N RX frames (CPU-tag bring-up instrument)");

/*
 * M7 FCS wedge signal: every large (>132 B) delivered frame gets its Ethernet
 * FCS software-verified (EXCLUDE_CRC is clear, so ph_len includes the 4 FCS
 * bytes and they are DMA'd into the cluster). The switch MAC already discards
 * genuinely bad-FCS frames at ingress, so a software mismatch here means the
 * DELIVERED bytes are not the received frame — exactly the wedge (validated
 * live: wedged = cached AND uncached views both stale, ~93% of large frames
 * corrupt, while small frames pass). The eth watchdog windows these counters
 * into the wedge detector (rtl819x-eth.c). CRC32 (Sarwate) over ~1 KB is a
 * few microseconds on this 1 GHz MIPS — only large CPU-terminating frames pay.
 * The frame is still delivered either way (the stack's checksums decide) —
 * these are counters, not a filter.
 */
u32 rtl819x_rx_fcs_ok;
u32 rtl819x_rx_fcs_fail;

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

static bool rx_cluster_shinfo_sane(struct sk_buff *skb, uint32 j);

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
			struct sk_buff *stale =
				(struct sk_buff *)(uintptr_t)rx_ri[j].skb;

			if (rx_ri[j].dma)
				dma_unmap_single(swnic_dmadev, rx_ri[j].dma,
						 size_of_cluster, DMA_FROM_DEVICE);
			rx_cluster_shinfo_sane(stale, j);
			dev_kfree_skb_any(stale);
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
/* True if the current Rx descriptor is CPU(RISC)-owned - i.e. a received frame
 * is waiting to be pulled. Lets the eth poll close the napi_complete re-arm
 * race: a frame landing just after receive() returned "empty" would otherwise
 * have its RX_DONE ack'd away and stall until the slow watchdog. Cheap, lockless
 * peek (single volatile read); a false negative just defers to the next IRQ. */
int32 New_swNic_rxPending(void)
{
	volatile uint32 *rr = RXR();

	return (rr[rxCurr] & DESC_OWNED_BIT) == DESC_RISC_OWNED;
}
/*
 * ★ RUNOUT MUST BE ACKED ON EVERY RX EXIT, NOT JUST THE SUCCESS PATH.
 *
 * Established by disassembling the SHIPPED D-Link firmware for this exact board
 * (DIR_842E_RT8197F 3.0.3, Linux 3.10.90, symbols recovered from its 18269-entry
 * kallsyms table). Stock's RX path writes `ISR = 0x007F0000` -- all seven runout
 * bits -- on EVERY exit, and its ISR masks the runout sources on assert
 * (CONFIG_FINETUNE_RUNOUT_IRQ, inlined at interrupt_isr+0x90) re-arming them only
 * once the free-skb pool recovers past a compile-time threshold of 128.
 *
 * The dangerous state stock is careful to avoid, in its own control flow: it is
 * legal to leave the tasklet with RUNOUT still masked and no RX pending, and in
 * that state nothing in the interrupt path will ever re-arm it. Stock's
 * one_sec_timer force-schedules the RX tasklet every second precisely so that
 * window can never last.
 *
 * This driver arms PKTHDR_DESC_RUNOUT_IE_ALL (see NIC_IIMR in rtl819x-eth.c) but
 * used to ack it on the RTL_NICRX_OK path ONLY. The ring-empty, allocation-
 * failure and loop-bound exits all returned with a latched, level-triggered
 * runout source unacknowledged. Our own equivalent of stock's one-second kick
 * (rtl819x_rx_timer -> napi_schedule, ~12 ms) runs far more often than stock's,
 * so the forced-kick half of the design was never the gap -- the ack was.
 *
 * Symptom this targets: after sustained forwarded load, frames larger than
 * ~128 B stop being delivered while small frames keep flowing, rx_packets keeps
 * advancing, the kernel stays alive, and no wedge detector fires
 * (docs/M7-LARGE-FRAME-RX-WEDGE.md, docs/RX-STALL-WEDGE.md §5).
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
			/* ★ see runout-ack note above New_swNic_receive() */
			REG32(CPUIISR) = (MBUF_DESC_RUNOUT_IP_ALL | PKTHDR_DESC_RUNOUT_IP_ALL);
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
				pr_err("swnic rx#%d idx=%u ph_len=%u port=%02x vid=%u reason=%04x mlen=%u\n",
				       rxt, idx, ph->ph_len, ph->ph_portlist,
				       ph->ph_vlanId & 0x0fff, ph->ph_reason, mb->m_len);
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
			/* out of buffers: leave the frame, don't advance. The
			 * descriptor stays CPU-owned and we retry next poll --
			 * but STILL ack runout, or the switch sits on a latched
			 * level source with no CPU-owned slots behind it. */
			/* ★ see runout-ack note above New_swNic_receive() */
			REG32(CPUIISR) = (MBUF_DESC_RUNOUT_IP_ALL | PKTHDR_DESC_RUNOUT_IP_ALL);
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

		/* CPU-tag bring-up instrument. (Under the old Fork A model the trunk was
		 * the only ingress the SoC ever saw, so ph_portlist was a constant and
		 * info->pid meaningless.) With the vendor CPU-tag mode on at
		 * BOTH ends, the SoC MAC strips the 4-byte 0x8899 tag in hardware and
		 * ph_portlist carries the REAL source jack — so this printing 2 for the
		 * host and 4 for the WAN peer is the gate for that phase.
		 * ph_asic0 is dumped too: it carries srcExtPortNum(0..1)/extPortList(8..11),
		 * i.e. how the CPU (port 8) shows up once it is an extension port. */
		if (unlikely(pid_dump > 0)) {
			pid_dump--;
			/* ph_asic0 bitfields, vendor common/mbuf.h:87-94 (LE):
			 *   [1:0] srcExtPortNum  [2] l2Trans  [3] isOriginal
			 *   [4]   hwFwd          [11:8] extPortList  [14:12] queueId
			 * hwFwd  = "copy from HSA bit 200" -- the ASIC's own HARDWARE FORWARD
			 *          flag: 1 means the switch already forwarded this frame.
			 * isOrig = "DP included cpu port or more than one ext port" -- i.e.
			 *          the destination portlist contained the CPU, so what we are
			 *          holding is a COPY, not the only delivery.
			 * Together they separate "the ASIC gave up and punted to us" (hwFwd=0)
			 * from "the ASIC forwarded it and also copied us" (hwFwd=1,isOrig=1) --
			 * the latter would mean the datapath is offloading while the CPU still
			 * burns a full packet's work on every frame, which looks identical to
			 * "no offload" in a bytes-through-CPU metric. */
			pr_err("rtl819x pid: port=%u vid=%u hwFwd=%u isOrig=%u l2Tr=%u extPL=%u srcExt=%u asic0=%04x reason=%04x len=%u\n",
			       info->pid, info->vid,
			       (ph->ph_asic0 >> 4) & 1,	/* hwFwd */
			       (ph->ph_asic0 >> 3) & 1,	/* isOriginal */
			       (ph->ph_asic0 >> 2) & 1,	/* l2Trans */
			       (ph->ph_asic0 >> 8) & 0xf,	/* extPortList */
			       ph->ph_asic0 & 3,		/* srcExtPortNum */
			       ph->ph_asic0, ph->ph_reason, len);
		}
		pr_err_once("rtl819x DP: first RX frame (len=%d)\n", info->len);

		/* M7 FCS wedge signal (doc above): verify the hardware-provided FCS
		 * against the delivered bytes the stack will actually consume.
		 * M7.3 line-rate: SAMPLE 1-in-8 large frames instead of every one.
		 * The full-rate check taxed the very CPU punt path it guards
		 * (~5-10 us Sarwate CRC per 1500 B frame ~= up to ~13% of the 1 GHz
		 * 24Kc at the ~13 kpps software-forwarding rate) — pure fast-path
		 * overhead on the pre-offload/trapped packets whose survival decides
		 * whether a flow lives long enough to reach hardware (rtl819x-eth.c
		 * watchdog note). Detector power is preserved: a wedged fabric
		 * corrupts ~93% of large frames, so a 1/8 sample still accrues the
		 * >=4-fails/4:1-majority gate within one 2.5 s window under any real
		 * traffic; arming (>=2 good sampled larges in one window) just needs
		 * ~16 large frames in 2.5 s — any SSH/bulk burst provides that. */
		if (len > 132) {
			static u32 fcs_sample;

			if (!(fcs_sample++ & 0x7)) {
				u8 *d = ((struct sk_buff *)r_skb)->data;
				u32 want = get_unaligned_le32(d + len - 4);
				u32 got = ~crc32_le(~0u, d, len - 4);

				if (got == want)
					rtl819x_rx_fcs_ok++;
				else
					rtl819x_rx_fcs_fail++;
			}
		}

		/* M7 wedge discriminator (see rx_dump doc at the top of this file). */
		if (unlikely(rtl819x_rx_dump > 0) && len > 132) {
			u8 *cv = ((struct sk_buff *)r_skb)->data;
			volatile u8 *uv = (volatile u8 *)
				(0xA0000000u | ((u32)rx_ri[idx].dma & 0x1FFFFFFFu));
			int i, diff = -1;

			rtl819x_rx_dump--;
			for (i = 0; i < (int)len; i++)
				if (cv[i] != uv[i]) { diff = i; break; }
			pr_err("rxdump idx=%u ph_len=%u ph_reason=%04x m_len=%u m_next=%08x m_data=%08x dma=%08x cache-vs-uncache-diff@%d\n",
			       idx, len, ph->ph_reason, mb->m_len, mb->m_next,
			       mb->m_data, (u32)rx_ri[idx].dma, diff);
			print_hex_dump(KERN_ERR, "rxd-head-c: ", DUMP_PREFIX_OFFSET,
				       16, 1, cv, 48, false);
			print_hex_dump(KERN_ERR, "rxd-head-u: ", DUMP_PREFIX_OFFSET,
				       16, 1, (void *)(uintptr_t)uv, 48, false);
			print_hex_dump(KERN_ERR, "rxd-tail-c: ", DUMP_PREFIX_OFFSET,
				       16, 1, cv + len - 16, 16, false);
		}

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

	/* ★ loop-bound exit (>256 iterations) -- ack here too. */
	REG32(CPUIISR) = (MBUF_DESC_RUNOUT_IP_ALL | PKTHDR_DESC_RUNOUT_IP_ALL);
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
	/* Bug #13: the 8197F mbuf own bit is 0x80, NOT the legacy 0x02 — without
	 * it the OUTGOING (direct-TX) descriptor path rejects the buffer and
	 * wedges ALL TX (the prior 0x8800 attempt wedged exactly here, m_flags
	 * was still 0x1E). TX-only: the RX pre-seed keeps legacy MBUF_USED. */
	mb->m_flags = MBUF_USED_8197F | MBUF_EXT | MBUF_PKTHDR | MBUF_EOR;
	mb->skb = (uint32)(uintptr_t)skb;

	/*
	 * Bug #13 clean-slate: zero every HW-interpreted pkthdr field before the
	 * writes below. Offsets +4..+23 = ph_asic0..ph_ptpbits; TX pkthdrs are
	 * ring-recycled, so stale bits (ph_reason, ph_asic1 linkID/tag fields,
	 * ph_flags2, ph_ipbits, ...) would otherwise ride into the OUTGOING
	 * descriptor. Preserves ph_mbuf (+0) and the sw scratch words (+24..).
	 */
	memset((u8 *)ph + 4, 0, 20);
	ph->ph_len = len;
	ph->ph_vlanId = nicTx->vid & 0x0fff;
	ph->ph_portlist = nicTx->portlist & 0x3f;
	/*
	 * Bug #13 (box-originated cold UNICAST never egressed the RGMII trunk):
	 * vendor direct-TX recipe — ph_flags = PKTHDR_USED|PKT_OUTGOING = 0x8800
	 * in the 8197F bit positions (PKTHDR_USED_8197F = 0x8000, NOT the legacy
	 * 0x0200 this port half-carried). 0x8800 makes the switch egress
	 * ph_portlist VERBATIM with no L2 lookup, so a unicast DA on a cold FDB
	 * can no longer be dropped inside the switch (broadcast/ARP already got
	 * out via VLAN-membership flooding; unicast did not). Requires the mbuf
	 * 0x80 own bit above (m_flags 0x9C) or the OUTGOING path rejects the
	 * buffer and wedges ALL TX. nicTx->flags is OR'd on top so a future
	 * HWLOOKUP request still passes through (it is 0 on the xmit path today).
	 */
	ph->ph_flags = PKTHDR_USED_8197F | PKT_OUTGOING | nicTx->flags;
	/*
	 * ph_srcExtPortNum = CPU (3) for EVERY CPU-injected frame, not just the
	 * HWLOOKUP path. With srcExtPort left at 0 the switch reads the source as
	 * physical port 0 and SOURCE-PORT-FILTERS it out of the egress portmask —
	 * and port 0 IS the single RGMII trunk to the RTL8367S. So a direct-portlist
	 * CPU frame egressed to ports 0-5 gets the trunk stripped, and
	 * box-initiated traffic (ARP, unicast on a cold FDB) never reaches the wire
	 * while host-initiated traffic (real ingress on port 0 -> flood to CPU) does.
	 * Marking the source as the CPU stops the trunk being filtered. (Same value
	 * the HWLOOKUP branch already used; see rtl865x_asichal.c PORT0_ROUTER_MODE
	 * note re: source-port filtering blocking egress back out the trunk.)
	 */
	ph->ph_asic0 = (3 << 0);	/* ph_srcExtPortNum = CPU */
	if (nicTx->flags & PKTHDR_HWLOOKUP) {
		/* CPU-injected frame, let the switch L2-bridge it (translation of
		 * the old tx_hwlkup|tx_bridge|tx_extspa=CPU descriptor bits). */
		ph->ph_flags |= PKTHDR_BRIDGING;
	}

	/*
	 * First few CPU-originated frames only: the descriptor as the switch core
	 * will see it. Cheap, bounded, and the one place that shows whether a
	 * frame that Linux counted as sent was actually handed over with a sane
	 * port list, VLAN id and flags.
	 */
	{
		static unsigned int txlog;

		if (txlog < 3) {
			txlog++;
			pr_info("rtl819x tx#%u len=%u portlist=%02x vid=%u flags=%04x asic0=%04x\n",
				txlog, ph->ph_len, ph->ph_portlist,
				ph->ph_vlanId & 0xfff, ph->ph_flags, ph->ph_asic0);
		}
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

/*
 * On-demand transmit-engine dump: `echo 1 > /sys/module/rtl819x/parameters/txdiag`.
 *
 * The question this exists to answer is the one the netdev counters cannot: the
 * driver counts a frame as sent the moment it hands the descriptor over, so
 * tx_packets rises even when the switch core never fetches a single one. If
 * swcore_owned is large and txDoneIdx is frozen behind txCurr, the descriptors
 * were queued and the engine never ran -- which looks from outside exactly like
 * a cable fault, and is what "the box receives but transmits nothing" turned
 * out to be.
 */
static int txdiag_set(const char *val, const struct kernel_param *kp)
{
	volatile uint32 *tr = TXR();
	unsigned int owned = 0, i;

	if (!tr || !txCnt) {
		pr_err("swnic txdiag: tx ring not allocated\n");
		return 0;
	}
	for (i = 0; i < txCnt; i++)
		if ((tr[i] & DESC_OWNED_BIT) == DESC_SWCORE_OWNED)
			owned++;

	pr_err("swnic txdiag: txCurr=%u txDoneIdx=%u txCnt=%u swcore_owned=%u\n",
	       txCurr, txDoneIdx, txCnt, owned);
	pr_err("swnic txdiag: CPUICR=%08x CPUIISR=%08x CPUTPDCR0=%08x tr[0..3]=%08x %08x %08x %08x\n",
	       REG32(CPUICR), REG32(CPUIISR), REG32(CPUTPDCR0),
	       tr[0], tr[1], tr[2], tr[3]);
	return 0;
}

static const struct kernel_param_ops txdiag_ops = {
	.set = txdiag_set,
};
module_param_cb(txdiag, &txdiag_ops, NULL, 0200);
MODULE_PARM_DESC(txdiag, "write anything to dump tx ring/engine state to the log");

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
/*
 * Outstanding Tx descriptors (handed to the switch core, not yet returned) and
 * the current reclaim index. The watchdog uses the pair to tell a busy engine
 * from a dead one: a working engine keeps moving txDoneIdx, a wedged one holds
 * it still while the ring fills behind it.
 */
uint32 New_swNic_txPending(uint32 *done_idx)
{
	unsigned long flags = 0;
	uint32 pending;

	SMP_LOCK_ETH_XMIT(flags);
	if (!txCnt)
		pending = 0;
	else if (txCurr >= txDoneIdx)
		pending = txCurr - txDoneIdx;
	else
		pending = txCnt - txDoneIdx + txCurr;
	if (done_idx)
		*done_idx = txDoneIdx;
	SMP_UNLOCK_ETH_XMIT(flags);

	return pending;
}

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

/*
 * A cluster still sitting in the Rx ring was never handed to the stack, so
 * its skb_shared_info MUST still be the pristine one __build_skb() wrote:
 * nr_frags == 0, frag_list == NULL. Forensics on a ramoops-only boot crash
 * (four captured occurrences, decoded byte-for-byte against this exact
 * build's own vmlinux) showed it can instead hold ASCII kernel-log text —
 * e.g. "P: f" from the "rtl819x DP: first RX frame" pr_err_once() above,
 * "nic " from the "swnic rx#%d ..." trace line — meaning something copies
 * log text over the tail of a live cluster's page-frag. Freeing such an skb
 * walks frags[] and put_page()s a text word as a struct page, crashing deep
 * in skb_release_data() on what looks like unrelated heap corruption. Until
 * the actual writer is found, refuse to free a clobbered shinfo as if it
 * were real — clear it and log the region so the writer can be identified
 * from the surrounding boot log next time this fires.
 */
static bool rx_cluster_shinfo_sane(struct sk_buff *skb, uint32 j)
{
	struct skb_shared_info *si = skb_shinfo(skb);

	if (likely(!si->nr_frags && !si->frag_list))
		return true;

	pr_err("swnic: rx_ri[%u] cluster shinfo clobbered: skb=%p head=%p end=%p nr_frags=%u frag_list=%p\n",
	       j, skb, skb->head, skb_end_pointer(skb), si->nr_frags, si->frag_list);
	print_hex_dump(KERN_ERR, "swnic clobber: ", DUMP_PREFIX_OFFSET,
		       16, 1, si, 128, true);
	si->nr_frags = 0;
	si->frag_list = NULL;
	return false;
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
				rx_cluster_shinfo_sane(skb, j);
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
