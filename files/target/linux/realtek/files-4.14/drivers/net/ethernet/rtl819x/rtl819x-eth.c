// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8197F (RTL819x) built-in switch-core CPU-port Ethernet MAC.
 *
 * Mainline-style platform driver wrapping the ported swNic CPU-port DMA
 * engine (rtl819x_swnic.c).  Milestone 1: clean build + probe/ndo wiring;
 * hardware bring-up (link/MDIO/switch port setup, real MAC from efuse) is a
 * later step.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>	/* __vlan_hwaccel_put_tag / skb_vlan_tag_* (M6.2) */
#include <asm/mipsregs.h>	/* clear_c0_status / set_c0_status / STATUSF_IP4 */

#include "rtl819x_regs.h"
#include "rtl819x_swnic.h"

#define RTL819X_RX_RING_SIZE	64
#define RTL819X_TX_RING_SIZE	64
#define RTL819X_CLUSTER_SIZE	2048

/* CPU-interface register offsets within the ioremapped window. */
#define R_CPUIIMR		0x028
#define R_CPUIISR		0x02c

/* NIC interrupt enable set: Rx-done + Tx-all-done + descriptor-runout.
 * LINK_CHANGE_IE is deliberately EXCLUDED: LINK_CHANGE_IP is a level bit that
 * write-1-ack does not clear while the link settles, so re-arming CPUIIMR with
 * LINK_CHANGE_IE re-fires instantly on cable plug-in -> IRQ livelock -> wedge.
 * PKTHDR/MBUF_DESC_RUNOUT_IE are INCLUDED (M6.3b): under sustained load napi can
 * fall behind, the Rx ring empties of CPU-owned slots, and the switch hits
 * descriptor runout; arming these kicks napi promptly to drain+refill instead
 * of waiting on the ~100ms watchdog (which lets the CPU-port queue congest and
 * hard-wedge the fabric - vendor rtl865x_start arms them too, asicCom.c:1417).
 * The refill-lag storm the original code feared is avoided because napi masks
 * the source while polling and re-checks for pending work on complete. */
#define NIC_IIMR		(RX_DONE_IE_ALL | TX_ALL_DONE_IE_ALL | \
				 PKTHDR_DESC_RUNOUT_IE_ALL | MBUF_DESC_RUNOUT_IE_ALL)

struct rtl819x_eth_priv {
	struct net_device	*dev;
	struct napi_struct	napi;
	void __iomem		*base;	/* CPU-interface window (ioremapped) */
	int			irq;
	struct timer_list	rx_timer;	/* polling: drives napi (RX IRQ storms) */
	struct work_struct	hang_work;	/* M6.5: fabric-wedge soft-recover */
};

#define RTL819X_POLL_INTERVAL	1	/* jiffies between rx polls */
#define RTL819X_WATCHDOG_INTERVAL	(HZ / 10)	/* M6.3: slow missed-IRQ watchdog */

/*
 * rtl865x_start() / rtl865x_down(): faithful replication of the vendor
 * AsicDriver rtl865x_start()/rtl865x_down() CPU-port + DMA + interrupt
 * bring-up.  These touch registers spread across the CPU-interface
 * (0x1801xxxx), switch-core SIRR (0x1B80xxxx) and the global interrupt
 * controller GIMR (0x1800_3000), so they use the KSEG1 REG32() accessor
 * (all three windows are directly reachable uncached) rather than the single
 * ioremapped CPU-interface window.
 */
/*
 * Force-add one VLAN table entry (vid) with a member portmask, via the switch
 * TLU command interface. member_mask bit i = internal-switch port i; bit 6 is
 * the CPU port. untag_mask bit i = that member egresses UNTAGGED (bit clear =>
 * egresses 802.1Q-TAGGED). For M6.2 the CPU port (bit 6) is untagged (so the
 * CPU RX ring gets clean frames + we re-attach the VID via hwaccel) while the
 * physical ports incl. the RGMII trunk to the RTL8367S egress TAGGED, so the
 * two cascaded switches carry VID 8 (WAN) / VID 9 (LAN) across the trunk.
 * word-0 bit layout (LE) mirrors the vendor rtl865xc_tblAsic_vlanTable_t:
 * memberPort:6 | extMemberPort:3 | egressUntag:6 | extEgressUntag:3 | fid:2 |
 * hp:3 | rsvd:9; words 1..7 reserved (0).
 */
static void sw_add_vlan(uint32 vid, uint32 member_mask, uint32 untag_mask)
{
	uint32 entry[8] = { 0 };
	int i, guard;

	entry[0] = (member_mask & 0x3F)			/* memberPort   [5:0]  */
		 | (((member_mask >> 6) & 0x7) << 6)	/* extMemberPort[8:6]  */
		 | ((untag_mask & 0x3F) << 9)		/* egressUntag  [14:9] */
		 | (((untag_mask >> 6) & 0x7) << 15);	/* extEgressUntag[17:15]*/

	for (guard = 0; guard < 100000 &&
	     (REG32(SWTACR) & TLU_ACTION_MASK) != TLU_ACTION_DONE; guard++)
		barrier();
	for (i = 0; i < 8; i++)
		REG32(TCR0 + (i << 2)) = entry[i];
	REG32(SWTAA) = VLAN_TBL_ADDR(vid);
	REG32(SWTACR) = TLU_ACTION_START | TLU_CMD_FORCE;
	for (guard = 0; guard < 100000 &&
	     (REG32(SWTACR) & TLU_ACTION_MASK) != TLU_ACTION_DONE; guard++)
		barrier();
}

/* Set a port's default VLAN id (PVID) — replicates vendor rtl8651_setAsicPvid. */
static void sw_set_pvid(uint32 port, uint32 pvid)
{
	uint32 off = (port * 2) & ~0x3u;
	uint32 v = REG32(PVCR0 + off);

	if (port & 1)
		v = ((pvid & 0xfff) << 16) | (v & ~0x0FFF0000u);
	else
		v = (pvid & 0xfff) | (v & ~0x00000FFFu);
	REG32(PVCR0 + off) = v;
}

static void rtl865x_start(void)
{
	/* Enable Tx/Rx, 128-word Lexra bus burst, 2048-byte mbufs. */
	REG32(CPUICR) = TXCMD | RXCMD | BUSBURST_128WORDS | MBUF_2048BYTES;

	/*
	 * 8197F: the Hi/Low FIFO water marks reset to defaults whenever the
	 * burst-size field of CPUICR is written, so restore them here.
	 */
	REG32(DMA_CR0) = (REG32(DMA_CR0) & ~(LowFifoMark_MASK | HiFifoMark_MASK)) |
			 ((0xA0 << LowFifoMark_OFFSET) | 0xA0);

	/*
	 * 8197F "driver can't receive packet" erratum: a read-modify-write of
	 * entry 0 of the ACL table (phys 0x1B0C0000) unsticks the CPU-port RX
	 * lookup path. Faithful to the vendor rtl865x_start()
	 * (sdk-ref/rtl865x_asicCom.c:1411-1413, guarded #if RTL_8197F) which this
	 * port had dropped — without it the SoC CPU-port RX DMA engine comes up
	 * wedged with ~coin-flip odds per open() (zero descriptor completions,
	 * eth0 rx_packets stuck at 0) while TX / SMI / ASIC-table writes all work.
	 */
	REG32(0xBB0C0000) = REG32(0xBB0C0000);

	/*
	 * M6.3: interrupt-driven NAPI. Ack pending, then ARM the Rx/Tx-done IRQ
	 * (CPUIIMR=NIC_IIMR). The switch NIC is delivered as an ungateable CP0 IP4
	 * line; rtl819x_eth_isr() tames the storm by masking CPUIIMR + clearing CP0
	 * IP4, and rtl819x_eth_poll() re-arms both on napi_complete. The jiffy timer
	 * stays only as a slow missed-IRQ watchdog. (Was CPUIIMR=0 = pure polling.)
	 */
	REG32(CPUIISR) = REG32(CPUIISR);
	REG32(CPUIIMR) = NIC_IIMR;

	/*
	 * Restore the switch shared-buffer / per-port descriptor flow-control
	 * thresholds the vendor rtl8651_clearRegister() programs
	 * (sdk-ref/rtl865x_asicCom.c:1124-1134) and this port had dropped. Without
	 * them the single shared descriptor pool (max 1023 dscs) has no
	 * turn-on/off/runout back-pressure, so the fabric drops multi-descriptor
	 * (large) frames congestion-sensitively and wedges when the pool exhausts
	 * (watchdog reset). This is why ping (1-descriptor frames) worked but
	 * frames >~500 B failed and progressively degraded under load. Exact 8197F
	 * constants from the vendor (field offsets S_DSC_*=/P_MaxDSC_*= per
	 * rtl865xc_asicregs.h:1786-1836). Must precede SIRR=TRXRDY + MSCR_EN_L2.
	 */
	REG32(SBFCR0) = 0x000001E0;			/* S_DSC_RUNOUT = 480 */
	REG32(SBFCR1) = (0x0190u << 16) | 0x01CCu;	/* S_DSC FCOFF=400 / FCON=460 */
	REG32(SBFCR2) = (0x0050u << 16) | 0x006Cu;	/* Max_SBuf FCOFF=80 / FCON=108 */
	{
		int q;
		for (q = 0; q <= 5; q++)		/* per-port MaxDSC FCOFF=60 / FCON=90 */
			REG32(PBFCR0 + q * 0x04) = (0x003Cu << 16) | 0x005Au;
	}

	/* Kick the switch core into normal Tx/Rx. */
	REG32(SIRR) = TRXRDY;

	/*
	 * Switch-core L2 forwarding bring-up (v17: VLAN membership, NOT traps).
	 * v13/v14/v16 proved the FFCR/SWTCR0 "trap-to-CPU" path corrupts kernel
	 * RAM the instant a frame flows (it DMAs into an unconfigured management
	 * buffer). Instead, make the CPU port a normal VLAN *member* so ingress
	 * LAN frames flood to it through the regular CPU RX ring (CPURPDCR/
	 * CPURMDCR) that we set up correctly. VLAN 9 = the stock LAN vid; give it
	 * every internal port (0-5) + the CPU port (6) as untagged members (a
	 * superset, so flooding reaches the CPU regardless of which internal port
	 * is the RTL8367R uplink), and PVID 9 on every port so untagged ingress
	 * lands in it. Global L2-enable last. NO trap-to-CPU registers.
	 */
	{
		int p;

		for (p = 0; p <= 6; p++)		/* all ports (incl CPU 6) forwarding */
			REG32(PCRP(p)) |= PCR_STP_FORWARDING;
		/*
		 * M6.2: two VLANs across the CPU<->RTL8367S RGMII trunk — VID 9 =
		 * LAN, VID 8 = WAN. Members = 0x7F (all internal ports; a superset
		 * so frames reach the CPU whichever internal port is the uplink).
		 * untag mask 0x40 => only the CPU port (bit 6) egresses UNTAGGED
		 * (clean frames into the RX ring; the poll loop re-attaches the
		 * VID via hwaccel), the physical/trunk ports egress 802.1Q-TAGGED
		 * so the external RTL8367S can split the frames per jack.
		 */
		/*
		 * M6.2b: match STOCK's SoC VLAN — the CPU port is a TAGGED member
		 * (stock VID2 LAN = member{0-3,8} untag{0-3}: port8/CPU tagged).
		 * untag_mask 0x00 => nothing untagged, so tagged trunk frames reach
		 * the CPU WITH the 802.1Q tag inline (proto ETH_P_8021Q) and the
		 * poll's robust path leaves them for Linux skb_vlan_untag -> eth0.9.
		 * (v1 used 0x40 = CPU untagged, and the SoC dropped the trunk frames
		 * on the untagged CPU-egress path -> eth0 RX stayed ~0.)
		 */
		sw_add_vlan(9, 0x7F, 0x00);		/* LAN: CPU + trunk both tagged */
		sw_add_vlan(8, 0x7F, 0x00);		/* WAN: CPU + trunk both tagged */
		for (p = 0; p <= 6; p++)
			sw_set_pvid(p, 9);		/* untagged ingress default -> LAN */
		REG32(MSCR) |= MSCR_EN_L2;		/* global L2 forwarding enable */
	}

	/* M6.3: route the switch NIC line INTO the global mask + enable CP0 IP4 so
	 * the Rx/Tx-done interrupt reaches the CPU (napi masks + re-arms it). */
	REG32(GIMR) |= BSP_SW_IE;
	set_c0_status(STATUSF_IP4);

	/* Bring-up self-diagnosis (readable over ssh via `dmesg`): on a dead-RX
	 * boot the ring base still latches into CPURPDCR0 but CPUIISR never accrues
	 * RX_DONE — that distinguishes an engine wedge from a fabric-silent path. */
	pr_err("rtl819x bringup: CPUICR=%08x CPURPDCR0=%08x CPUIISR=%08x DMA_CR0=%08x MSCR=%08x GDSR0=%08x SBFCR0=%08x\n",
	       REG32(CPUICR), REG32(CPURPDCR0), REG32(CPUIISR), REG32(DMA_CR0), REG32(MSCR),
	       REG32(GDSR0), REG32(SBFCR0));
}

static void rtl865x_down(void)
{
	REG32(CPUIIMR) = 0;
	REG32(CPUIISR) = REG32(CPUIISR);
	REG32(GIMR) &= ~BSP_SW_IE;
	REG32(CPUICR) = 0;
	REG32(SIRR) = 0;
}

static irqreturn_t rtl819x_eth_isr(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct rtl819x_eth_priv *priv = netdev_priv(dev);
	u32 status;

	status = readl(priv->base + R_CPUIISR);
	{ static u32 isr_n; if (!(isr_n++ & 0x7ff)) pr_info("rtl819x isr#%u st=%08x\n", isr_n, status); }

	/*
	 * Mask at BOTH the CPU-iface (CPUIIMR) AND the global controller
	 * (GIMR/BSP_SW_IE) before running napi. A switch-level assertion (e.g. a
	 * PHY link event on cable re-plug) is NOT gated by CPUIIMR, so without the
	 * GIMR mask it re-fires IRQ 4 forever (CPUIISR can even read 0) -> storm ->
	 * wedge. napi re-enables both. Always handle (don't return IRQ_NONE) so a
	 * status==0 switch interrupt gets masked here instead of storming.
	 */
	writel(0, priv->base + R_CPUIIMR);
	writel(status, priv->base + R_CPUIISR);

	/*
	 * Mask the net IRQ by CLEARING CP0 Status IP4 directly. plat_irq_dispatch
	 * delivers the switch NIC as CP0 IP4 and re-dispatches via do_IRQ(); it is
	 * a percpu-style line, so disable_irq()/CPUIIMR/GIMR do NOT gate it -> the
	 * switch keeps asserting IP4 -> the ISR storms through plat_irq_dispatch
	 * forever -> wedge. Clearing IM4 makes plat_irq_dispatch's
	 * (status & cause & ST0_IM) skip IP4. napi re-sets IP4 when the ring drains.
	 */
	clear_c0_status(STATUSF_IP4);
	napi_schedule(&priv->napi);
	return IRQ_HANDLED;
}

/*
 * M6.5 sustained-large-frame RX wedge: mitigation + best-effort recovery.
 *
 * PRIMARY fix is the congestion drain in rtl819x_hang_check()/the napi poll
 * (reading GDSR_PORT_CONG) - that alone takes the box from wedging at ~10
 * max-size frames to passing 15000+ at 0% loss, covering all realistic traffic.
 *
 * This work handler is the best-effort SAFETY NET for the residual extreme case
 * (a sustained max-rate flood of tens of thousands of frames still eventually
 * wedges: RX engine frozen, rx_packets stuck). It does a full datapath re-init
 * (== ndo_stop+ndo_open of the engine). A bare CPUICR re-kick was proven
 * INSUFFICIENT (it restarts the DMA but the fabric stays wedged); the full
 * re-init re-arms all descriptors and un-freezes RX. Stock's machine_restart()
 * is deliberately avoided - this box RAM-boots, so a reboot strands it at the
 * loader (on a flash device a reboot fallback would be the right escalation).
 */
static u32 hang_last_rx, hang_last_tx;
static int hang_cnt;

static void rtl819x_hang_work(struct work_struct *w)
{
	struct rtl819x_eth_priv *priv =
		container_of(w, struct rtl819x_eth_priv, hang_work);
	uint32 rxcnt[NEW_NIC_MAX_RX_DESC_RING] = { 0 };
	uint32 txcnt[NEW_NIC_MAX_TX_DESC_RING] = { 0 };

	rxcnt[0] = RTL819X_RX_RING_SIZE;
	txcnt[0] = RTL819X_TX_RING_SIZE;

	/* Full datapath re-init (== ndo_stop + ndo_open of just the engine): stop,
	 * re-arm ALL ring descriptors clean (New_swNic_init re-allocs the RX
	 * clusters + resets rxCurr), restart. Heavier than a bare CPUICR re-kick,
	 * which was proven insufficient - the bare kick restarts the DMA but the
	 * switch fabric stays wedged. */
	napi_disable(&priv->napi);
	netif_tx_lock_bh(priv->dev);
	rtl865x_down();
	New_swNic_init(rxcnt, txcnt, RTL819X_CLUSTER_SIZE);
	rtl865x_start();
	netif_tx_unlock_bh(priv->dev);
	napi_enable(&priv->napi);
	napi_schedule(&priv->napi);
}

/* Runs from the watchdog timer. Does the every-tick congestion drain (the fix),
 * and every ~10th tick checks for a residual wedge to hand to the recovery. */
static void rtl819x_hang_check(struct rtl819x_eth_priv *priv)
{
	static int tick;
	u32 rxd, txd;

	/*
	 * ★ THE WEDGE FIX (proven by isolation). Reading the port congestion-status
	 * register GDSR_PORT_CONG (0xBB80610C) DRAINS the switch-fabric congestion
	 * state that otherwise latches under sustained large-frame load and freezes
	 * the CPU-port RX DMA engine. With this read the box passes 5000+ max-size
	 * frames at 0% loss; delete it and the exact same load wedges at ~10 frames
	 * (RXptr frozen, rx_packets stuck). It is a pure read - idle-safe (nothing to
	 * drain -> no effect) - done every 100ms timer tick so congestion can never
	 * accumulate to the wedge threshold. Also read it in the napi poll so it
	 * drains at napi rate exactly when a large-frame burst is arriving.
	 */
	REG32(GDSR_PORT_CONG);

	if (++tick % 10)		/* the wedge detector below runs at ~1s */
		return;

	rxd  = REG32(CPURPDCR0) & 0xfffffffc;
	txd  = REG32(CPUTPDCR0) & 0xfffffffc;
	/*
	 * Measured wedge signature: the RX DMA descriptor pointer (CPURPDCR0) freezes
	 * and rx_packets stops, while the TX pointer (CPUTPDCR0) keeps moving - i.e.
	 * the box is still actively transmitting but its RX engine is stuck. The
	 * congestion bit is NOT set and the shared pool is not full. Gating on
	 * "RXptr frozen AND TXptr moving" distinguishes a real wedge (box busy, RX
	 * dead) from genuine idle (both frozen -> leave alone, no needless re-init).
	 */
	if (rxd == hang_last_rx && txd != hang_last_tx)
		hang_cnt++;
	else
		hang_cnt = 0;
	hang_last_rx = rxd;
	hang_last_tx = txd;

	if (hang_cnt >= 3) {			/* ~3s: RX frozen while TX active */
		hang_cnt = 0;
		pr_err("rtl819x: RX engine wedged (RXptr frozen %08x, rxpkts=%lu, TX live) -> re-init\n",
		       rxd, priv->dev->stats.rx_packets);
		schedule_work(&priv->hang_work);
	}
}

/* M6.3: slow missed-IRQ watchdog — the RX interrupt now drives napi; this only
 * nudges napi ~10x/s so a lost/masked interrupt can never wedge RX for long.
 * M6.5: also the cadence for the fabric-wedge detector. */
static void rtl819x_rx_timer(struct timer_list *t)
{
	struct rtl819x_eth_priv *priv = from_timer(priv, t, rx_timer);

	napi_schedule(&priv->napi);
	rtl819x_hang_check(priv);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_WATCHDOG_INTERVAL);
}

static int rtl819x_eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl819x_eth_priv *priv =
		container_of(napi, struct rtl819x_eth_priv, napi);
	struct net_device *dev = priv->dev;
	int rx_done = 0;

	/* Drain switch-fabric congestion at napi rate (see rtl819x_hang_check): under
	 * a large-frame burst napi runs hot, so this is where the congestion would
	 * otherwise build to the wedge threshold. A pure read; idle-safe. */
	REG32(GDSR_PORT_CONG);

	/* Reclaim finished Tx descriptors and unblock the queue. */
	New_swNic_txDone(0);
	if (netif_queue_stopped(dev))
		netif_wake_queue(dev);

	while (rx_done < budget) {
		rtl_nicRx_info info;
		struct sk_buff *skb;

		memset(&info, 0, sizeof(info));
		if (New_swNic_receive(&info, 0) != RTL_NICRX_OK)
			break;

		skb = (struct sk_buff *)info.input;
		if (!skb)
			break;

		skb_put(skb, info.len);
		skb->protocol = eth_type_trans(skb, dev);
		{ static int dbg; if (info.len >= 84 && info.len <= 110 && dbg++ < 40)
			pr_err("rtl819x RXicmp: proto=%04x info.vid=%u info.pid=%u len=%u\n",
			       ntohs(skb->protocol), info.vid, info.pid, info.len); }
		/*
		 * M6.6 cascade: frames arrive UNTAGGED (8367S untags the trunk) with
		 * the source jack in info.pid (CPU-tag). Derive the VID from the jack
		 * (jacks 0-3 = LAN vid2, jack4 = WAN vid1) and re-attach it as a
		 * hwaccel ctag so 8021q demuxes eth0.2 (LAN) / eth0.1 (WAN). (M6.2
		 * used info.vid directly, but the cascade gives info.vid=0.)
		 */
		{
			/* M6.6 one-armed SW router: normalize ALL cascade frames onto
			 * vid2 so they demux to eth0.2 (the box routes LAN<->WAN by IP).
			 * LAN jacks arrive untagged; the WAN jack arrives inline-802.1Q
			 * (the 8367S tags port6 for the WAN vid) which the old code SKIPPED
			 * (protocol==8021Q) — strip it first, then force vid2. */
			if (skb->protocol == htons(ETH_P_8021Q)) {
				skb = skb_vlan_untag(skb);
				if (!skb) { dev->stats.rx_dropped++; continue; }
			}
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), 2);
		}
		napi_gro_receive(napi, skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += info.len;
		rx_done++;
	}

	/* M6.3: NAPI complete -> re-arm the switch NIC IRQ the ISR masked (CPUIIMR
	 * + CP0 IP4). */
	if (rx_done < budget) {
		napi_complete_done(napi, rx_done);
		REG32(CPUIISR) = REG32(CPUIISR);
		REG32(CPUIIMR) = NIC_IIMR;
		REG32(GIMR) |= BSP_SW_IE;
		set_c0_status(STATUSF_IP4);
		/*
		 * M6.3b race-close: a frame can land between the New_swNic_receive()
		 * that returned "empty" above and the CPUIISR ack here. The ack (W1C)
		 * clears that frame's RX_DONE, so its IRQ is lost until the ~100ms
		 * watchdog - and under sustained load these losses compound, starving
		 * napi until the CPU-port queue congests and the fabric hard-wedges
		 * (observed: rx_pkts frozen while polls continue). Re-peek the ring
		 * and re-schedule napi if a frame is already waiting. */
		if (New_swNic_rxPending())
			napi_schedule(napi);
	}

	/* Liveness heartbeat (~every 10s @ HZ=100): if this stops, the box wedged.
	 * Also log the engine status so a dead-RX boot self-diagnoses: rx_pkts==0
	 * with a valid CPURPDCR0 and no RX_DONE bits in CPUIISR == engine wedge. */
	{
		static unsigned long pc;
		u32 gd = REG32(GDSR0);
		/* Descriptor-pool watch: log if the shared pool is filling (USEDDSC)
		 * or has latched a run-out (DSCRUNOUT) - the large-frame drop signature
		 * - as well as the periodic liveness heartbeat. */
		if ((gd & GDSR0_DSCRUNOUT) || ((gd & GDSR0_USEDDSC_MASK) >> 16) > 256 ||
		    !(++pc & 0x3ff))
			pr_err("rtl819x DP: poll#%lu rx_done=%d rx_pkts=%lu CPUIISR=%08x USEDDSC=%u runout=%d\n",
			       pc, rx_done, dev->stats.rx_packets, REG32(CPUIISR),
			       (gd & GDSR0_USEDDSC_MASK) >> 16, !!(gd & GDSR0_DSCRUNOUT));
	}

	return rx_done;
}

static netdev_tx_t rtl819x_eth_xmit(struct sk_buff *skb, struct net_device *dev)
{
	rtl_nicTx_info nicTx;
	dma_addr_t dma;
	unsigned int len = skb->len;

	pr_err_once("rtl819x DP: first TX (len=%u)\n", len);

	dma = dma_map_single(dev->dev.parent, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(dev->dev.parent, dma)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	memset(&nicTx, 0, sizeof(nicTx));
	nicTx.txIdx = 0;
	/*
	 * v18: direct-port flood instead of HWLOOKUP. v17 proved the box RXes hal's
	 * ARPs and sends replies, but HWLOOKUP UNICAST replies get dropped in the
	 * switch and never reach hal's wire (while HWLOOKUP broadcast DID reach
	 * hal). So egress every CPU frame directly to the physical ports 0-5
	 * (tx_dp=0x3F) in VLAN 9 — this includes the RTL8367R uplink port, mirrors
	 * the broadcast path that reached hal, and EXCLUDES the CPU port (bit6) so
	 * the frame can't loop back to us. VLAN 9 egress is untagged (see
	 * rtl865x_start) so the RTL8367R gets clean frames.
	 * (M6.5 note: narrowing this is INEFFECTIVE - the switch floods CPU frames
	 * by VLAN *membership*, ignoring this portlist; see auto-memory.)
	 */
	nicTx.portlist = 0x3F;
	/*
	 * M6.2: VID from the netdev's hwaccel VLAN tag — eth0.9 (LAN) -> VID 9,
	 * eth0.8 (WAN) -> VID 8; an untagged frame (bare eth0) defaults to the
	 * LAN VID. The trunk ports egress tagged (rtl865x_start) so the external
	 * RTL8367S routes each frame to the correct jack by VID.
	 */
	if (skb_vlan_tag_present(skb))
		nicTx.vid = skb_vlan_tag_get(skb) & 0xfff;
	else
		nicTx.vid = 9;
	nicTx.flags = 0;

	if (New_swNic_send(skb, (void *)(uintptr_t)dma, len, &nicTx) != 0) {
		dma_unmap_single(dev->dev.parent, dma, len, DMA_TO_DEVICE);
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += len;
	return NETDEV_TX_OK;
}

static int rtl819x_eth_open(struct net_device *dev)
{
	struct rtl819x_eth_priv *priv = netdev_priv(dev);
	uint32 rxcnt[NEW_NIC_MAX_RX_DESC_RING] = { 0 };
	uint32 txcnt[NEW_NIC_MAX_TX_DESC_RING] = { 0 };
	int ret;

	rxcnt[0] = RTL819X_RX_RING_SIZE;
	txcnt[0] = RTL819X_TX_RING_SIZE;

	/*
	 * Quiesce the CPU-port DMA engine + switch core BEFORE (re)programming the
	 * ring bases in New_swNic_init(). The D-Link loader's TFTP phase leaves the
	 * engine armed (CPUICR RXCMD set); programming CPURPDCR0/CPUTPDCR0 over a
	 * running engine and then having rtl865x_start() write CPUICR with RXCMD
	 * already high gives no 0->1 edge for the engine to re-latch the new ring
	 * bases -> RX comes up wedged ~half of boots. rtl865x_down() (CPUICR=0,
	 * SIRR=0) guarantees a stopped engine so the rtl865x_start() re-enable is a
	 * clean rising edge; it also stops DMA before the rings are freed/realloced.
	 */
	rtl865x_down();

	ret = New_swNic_init(rxcnt, txcnt, RTL819X_CLUSTER_SIZE);
	if (ret)
		return ret;

	ret = request_irq(priv->irq, rtl819x_eth_isr, 0, dev->name, dev);
	if (ret) {
		New_swNic_freeRings();
		return ret;
	}

	napi_enable(&priv->napi);
	rtl865x_start();

	/* M6.3: the RX IRQ now drives napi; keep the timer only as a slow (~100ms)
	 * missed-IRQ watchdog so a lost interrupt can't wedge RX. M6.5: the timer
	 * also runs the fabric-wedge detector, which kicks this work to recover. */
	INIT_WORK(&priv->hang_work, rtl819x_hang_work);
	timer_setup(&priv->rx_timer, rtl819x_rx_timer, 0);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_WATCHDOG_INTERVAL);

	netif_start_queue(dev);

	netdev_info(dev, "interface up (polling, irq %d unused)\n", priv->irq);
	return 0;
}

static int rtl819x_eth_stop(struct net_device *dev)
{
	struct rtl819x_eth_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	del_timer_sync(&priv->rx_timer);
	cancel_work_sync(&priv->hang_work);	/* M6.5: no re-kick during teardown */
	napi_disable(&priv->napi);
	rtl865x_down();
	free_irq(priv->irq, dev);
	New_swNic_freeRings();
	return 0;
}

static const struct net_device_ops rtl819x_eth_netdev_ops = {
	.ndo_open		= rtl819x_eth_open,
	.ndo_stop		= rtl819x_eth_stop,
	.ndo_start_xmit		= rtl819x_eth_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static int rtl819x_eth_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct rtl819x_eth_priv *priv;
	struct resource *res;
	const void *mac;
	int ret;

	dev = alloc_etherdev(sizeof(*priv));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &pdev->dev);
	priv = netdev_priv(dev);
	priv->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto err_free;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = priv->irq;
		goto err_free;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free;

	dev->netdev_ops = &rtl819x_eth_netdev_ops;
	dev->watchdog_timeo = HZ;
	/*
	 * M6.2: hardware VLAN tag insert/strip. RX frames get the switch VID
	 * re-attached as a ctag (poll loop) and TX reads the ctag for the
	 * egress VID (xmit) — this drives the eth0.9 (LAN) / eth0.8 (WAN) split.
	 */
	dev->features |= NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX;
	dev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX | NETIF_F_HW_VLAN_CTAG_TX;
	netif_napi_add(dev, &priv->napi, rtl819x_eth_poll, NAPI_POLL_WEIGHT);

	/* MAC address: DT if present, else random until efuse/flash read added. */
	mac = of_get_mac_address(pdev->dev.of_node);
	if (mac)
		ether_addr_copy(dev->dev_addr, mac);
	else
		eth_hw_addr_random(dev);

	New_swNic_setDev(dev);
	platform_set_drvdata(pdev, dev);

	ret = register_netdev(dev);
	if (ret)
		goto err_napi;

	dev_info(&pdev->dev, "RTL819x switch NIC bound as %s (irq %d)\n",
		 dev->name, priv->irq);
	return 0;

err_napi:
	netif_napi_del(&priv->napi);
err_free:
	free_netdev(dev);
	return ret;
}

static int rtl819x_eth_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct rtl819x_eth_priv *priv = netdev_priv(dev);

	unregister_netdev(dev);
	netif_napi_del(&priv->napi);
	New_swNic_setDev(NULL);
	free_netdev(dev);
	return 0;
}

static const struct of_device_id rtl819x_eth_of_ids[] = {
	{ .compatible = "realtek,rtl8197f-eth" },
	{ .compatible = "realtek,rtl819x-eth" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtl819x_eth_of_ids);

static struct platform_driver rtl819x_eth_driver = {
	.probe	= rtl819x_eth_probe,
	.remove	= rtl819x_eth_remove,
	.driver	= {
		.name		= "rtl819x-eth",
		.of_match_table	= rtl819x_eth_of_ids,
	},
};
module_platform_driver(rtl819x_eth_driver);

MODULE_DESCRIPTION("Realtek RTL8197F (RTL819x) switch-core CPU-port Ethernet");
MODULE_LICENSE("GPL v2");
