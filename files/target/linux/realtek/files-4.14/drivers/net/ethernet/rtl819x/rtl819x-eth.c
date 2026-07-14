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
#include <asm/mipsregs.h>	/* clear_c0_status / set_c0_status / STATUSF_IP4 */

#include "rtl819x_regs.h"
#include "rtl819x_swnic.h"

#define RTL819X_RX_RING_SIZE	64
#define RTL819X_TX_RING_SIZE	64
#define RTL819X_CLUSTER_SIZE	2048

/* CPU-interface register offsets within the ioremapped window. */
#define R_CPUIIMR		0x028
#define R_CPUIISR		0x02c

/* NIC interrupt enable set: Rx-done + Tx-all-done only.
 * LINK_CHANGE_IE is deliberately EXCLUDED: LINK_CHANGE_IP is a level bit that
 * write-1-ack does not clear while the link settles, so re-arming CPUIIMR with
 * LINK_CHANGE_IE re-fires instantly on cable plug-in -> IRQ livelock -> wedge.
 * PKTHDR_DESC_RUNOUT_IE_ALL is excluded too (avoids a storm if Rx refill lags). */
#define NIC_IIMR		(RX_DONE_IE_ALL | TX_ALL_DONE_IE_ALL)

struct rtl819x_eth_priv {
	struct net_device	*dev;
	struct napi_struct	napi;
	void __iomem		*base;	/* CPU-interface window (ioremapped) */
	int			irq;
	struct timer_list	rx_timer;	/* polling: drives napi (RX IRQ storms) */
};

#define RTL819X_POLL_INTERVAL	1	/* jiffies between rx polls */

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
 * the CPU port. All members egress UNTAGGED so the CPU port receives clean
 * (un-tagged) frames. word-0 bit layout (LE) mirrors the vendor
 * rtl865xc_tblAsic_vlanTable_t: memberPort:6 | extMemberPort:3 | egressUntag:6
 * | extEgressUntag:3 | fid:2 | hp:3 | rsvd:9; words 1..7 reserved (0).
 */
static void sw_add_vlan(uint32 vid, uint32 member_mask)
{
	uint32 entry[8] = { 0 };
	int i, guard;

	entry[0] = (member_mask & 0x3F)			/* memberPort   [5:0]  */
		 | (((member_mask >> 6) & 0x7) << 6)	/* extMemberPort[8:6]  */
		 | ((member_mask & 0x3F) << 9)		/* egressUntag  [14:9] */
		 | (((member_mask >> 6) & 0x7) << 15);	/* extEgressUntag[17:15]*/

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
	 * Ack any pending. POLLING MODE: leave CPUIIMR masked (0) and do NOT
	 * route the switch NIC line to the CPU (no GIMR). The switch NIC IRQ is
	 * delivered as CP0 IP4 via plat_irq_dispatch and cannot be reliably gated
	 * on this SoC (GIMR / disable_irq / CP0-IM4 / CPUIIMR masking all storm),
	 * so RX/TX is driven by a jiffy timer + napi instead of interrupts.
	 */
	REG32(CPUIISR) = REG32(CPUIISR);
	REG32(CPUIIMR) = 0;

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
		sw_add_vlan(9, 0x7F);			/* vid 9: members ports 0-6 (bit6=CPU) */
		for (p = 0; p <= 6; p++)
			sw_set_pvid(p, 9);
		REG32(MSCR) |= MSCR_EN_L2;		/* global L2 forwarding enable */
	}

	/* POLLING MODE: force the switch NIC line OUT of the global mask so it
	 * can never reach the CPU, regardless of what the bootloader left set. */
	REG32(GIMR) &= ~BSP_SW_IE;
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

/* Polling: re-schedule napi every jiffy (the RX interrupt is left disabled). */
static void rtl819x_rx_timer(struct timer_list *t)
{
	struct rtl819x_eth_priv *priv = from_timer(priv, t, rx_timer);

	napi_schedule(&priv->napi);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_POLL_INTERVAL);
}

static int rtl819x_eth_poll(struct napi_struct *napi, int budget)
{
	struct rtl819x_eth_priv *priv =
		container_of(napi, struct rtl819x_eth_priv, napi);
	struct net_device *dev = priv->dev;
	int rx_done = 0;

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
		napi_gro_receive(napi, skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += info.len;
		rx_done++;
	}

	/* POLLING MODE: complete; the jiffy timer re-schedules us (no IRQ re-enable). */
	if (rx_done < budget)
		napi_complete_done(napi, rx_done);

	/* Liveness heartbeat (~every 10s @ HZ=100): if this stops, the box wedged. */
	{
		static unsigned long pc;
		if (!(++pc & 0x3ff))
			pr_err("rtl819x DP: poll#%lu alive rx_done=%d\n", pc, rx_done);
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
	 */
	nicTx.portlist = 0x3F;
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

	/* POLLING MODE: drive napi from a jiffy timer (RX IRQ is left disabled). */
	timer_setup(&priv->rx_timer, rtl819x_rx_timer, 0);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_POLL_INTERVAL);

	netif_start_queue(dev);

	netdev_info(dev, "interface up (polling, irq %d unused)\n", priv->irq);
	return 0;
}

static int rtl819x_eth_stop(struct net_device *dev)
{
	struct rtl819x_eth_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	del_timer_sync(&priv->rx_timer);
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
