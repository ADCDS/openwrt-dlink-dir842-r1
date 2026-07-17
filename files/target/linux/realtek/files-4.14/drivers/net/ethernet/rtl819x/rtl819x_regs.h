/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Realtek RTL8197F (RTL819x) built-in switch-core CPU-port NIC
 * Trimmed register map: CPU interface + the handful of switch-core /
 * global-interrupt registers the CPU-port DMA engine needs.
 *
 * Register values were taken verbatim from the Realtek vendor SDK header
 * AsicDriver/rtl865xc_asicregs.h so the ported swNic DMA engine keeps
 * bit-for-bit register semantics.
 *
 * Access model
 * ------------
 * On these MIPS SoCs the register windows are reached uncached through
 * KSEG1 (0xB800_0000..).  The low 29 bits are the physical address, so the
 * fixed KSEG1 constants below (e.g. 0xB8010000) alias phys 0x18010000 that
 * the device tree advertises.  The ported engine pokes registers through the
 * REG32() lvalue macro exactly like the vendor driver did; the eth driver's
 * own (new) code uses ioremap()+readl/writel where practical.
 */

#ifndef _RTL819X_REGS_H
#define _RTL819X_REGS_H

#include <linux/types.h>

/* ---- uncached (KSEG1) register-window bases ------------------------------ */
#define RTL819X_SYSTEM_BASE		0xB8000000	/* phys 0x18000000 */
#define RTL819X_SWCORE_BASE		0xBB800000	/* phys 0x1B800000 */

#define CPU_IFACE_BASE			(RTL819X_SYSTEM_BASE + 0x10000)	/* 0xB8010000 */
#define GICR_BASE			(RTL819X_SYSTEM_BASE + 0x3000)	/* 0xB8003000 */
#define SWMISC_BASE			(RTL819X_SWCORE_BASE + 0x4200)

/* Physical mirror (what the device tree "reg" advertises) */
#define CPU_IFACE_PHYS			0x18010000
#define CPU_IFACE_SIZE			0x100

/* ---- CPU interface registers (offset from CPU_IFACE_BASE) ---------------- */
#define CPUICR				(0x000 + CPU_IFACE_BASE)	/* interface control */
#define CPURPDCR0			(0x004 + CPU_IFACE_BASE)	/* Rx pkthdr desc ctrl 0 */
#define CPURPDCR1			(0x008 + CPU_IFACE_BASE)
#define CPURPDCR2			(0x00c + CPU_IFACE_BASE)
#define CPURPDCR3			(0x010 + CPU_IFACE_BASE)
#define CPURPDCR4			(0x014 + CPU_IFACE_BASE)
#define CPURPDCR5			(0x018 + CPU_IFACE_BASE)
#define CPURPDCR(idx)			(CPURPDCR0 + ((idx) << 2))
#define CPURMDCR0			(0x01c + CPU_IFACE_BASE)	/* Rx mbuf desc ctrl */
#define CPUTPDCR0			(0x020 + CPU_IFACE_BASE)	/* Tx pkthdr desc ctrl 0 */
#define CPUTPDCR1			(0x024 + CPU_IFACE_BASE)	/* Tx pkthdr desc ctrl 1 */
#define CPUIIMR				(0x028 + CPU_IFACE_BASE)	/* interrupt mask */
#define CPUIISR				(0x02c + CPU_IFACE_BASE)	/* interrupt status */
#define DMA_CR0				(0x03c + CPU_IFACE_BASE)
#define DMA_CR1				(0x040 + CPU_IFACE_BASE)
#define DMA_CR2				(0x044 + CPU_IFACE_BASE)
/* NB: Tx rings 2/3 live at a non-contiguous offset (matches vendor SDK) */
#define CPUTPDCR2			(0x060 + CPU_IFACE_BASE)	/* Tx pkthdr desc ctrl 2 */
#define CPUTPDCR3			(0x064 + CPU_IFACE_BASE)	/* Tx pkthdr desc ctrl 3 */
#define DMA_CR3				(0x068 + CPU_IFACE_BASE)
#define TXRINGCR			(0x078 + CPU_IFACE_BASE)
#define DMA_CR4				(0x0a0 + CPU_IFACE_BASE)
#define CPUICR1				(0x0a4 + CPU_IFACE_BASE)

/* ---- CPUICR bits --------------------------------------------------------- */
#define TXCMD				(1 << 31)	/* enable Tx */
#define RXCMD				(1 << 30)	/* enable Rx */
#define BUSBURST_32WORDS		0
#define BUSBURST_64WORDS		(1 << 28)
#define BUSBURST_128WORDS		(2 << 28)
#define BUSBURST_256WORDS		(3 << 28)
#define MBUF_128BYTES			0
#define MBUF_256BYTES			(1 << 24)
#define MBUF_512BYTES			(2 << 24)
#define MBUF_1024BYTES			(3 << 24)
#define MBUF_2048BYTES			(4 << 24)
#define TXFD				(1 << 23)	/* notify Tx descriptor fetch */

/* ---- CPUIIMR / CPUIISR bits ---------------------------------------------- */
#define LINK_CHANGE_IE			(1 << 31)
#define PKTHDR_DESC_RUNOUT_IE_ALL	(0x3f << 17)
#define MBUF_DESC_RUNOUT_IE_ALL		(1 << 16)
#define RX_DONE_IE_ALL			(0x3f << 3)
#define TX_ALL_DONE_IE0			(1 << 1)
#define TX_ALL_DONE_IE1			(1 << 2)
#define TX_ALL_DONE_IE2			(1 << 12)
#define TX_ALL_DONE_IE3			(1 << 13)
#define TX_ALL_DONE_IE_ALL		(TX_ALL_DONE_IE0 | TX_ALL_DONE_IE1 | \
					 TX_ALL_DONE_IE2 | TX_ALL_DONE_IE3)

#define LINK_CHANGE_IP			(1 << 31)
#define MBUF_DESC_RUNOUT_IP_ALL		(1 << 16)
#define PKTHDR_DESC_RUNOUT_IP_ALL	(0x3f << 17)
#define PKTHDR_DESC_RUNOUT_IP(idx)	(1 << (17 + (idx)))
#define RX_DONE_IP_ALL			(0x3f << 3)
#define TX_ALL_DONE_IP0			(1 << 1)
#define TX_ALL_DONE_IP1			(1 << 2)
#define TX_ALL_DONE_IP2			(1 << 12)
#define TX_ALL_DONE_IP3			(1 << 13)
#define TX_ALL_DONE_IP_ALL		(TX_ALL_DONE_IP0 | TX_ALL_DONE_IP1 | \
					 TX_ALL_DONE_IP2 | TX_ALL_DONE_IP3)

/* ---- DMA_CR0 / DMA_CR4 / CPUICR1 ---------------------------------------- */
#define LowFifoMark_OFFSET		8
#define LowFifoMark_MASK		(0xff << 8)
#define HiFifoMark_MASK			(0xff << 0)
#define TX_RING0_TAIL_AWARE		(1 << 0)
#define CF_PKT_HDR_TYPE_OFFSET		8
#define CF_PKT_HDR_TYPE_MASK		(3 << 8)

/* ---- global interrupt controller (GICR) --------------------------------- */
#define GIMR				(0x000 + GICR_BASE)	/* global interrupt mask */
#define GISR				(0x004 + GICR_BASE)	/* global interrupt status */
#define BSP_SW_IE			(1 << 15)		/* switch-core NIC line */

/* ---- switch-core misc: System Initial and Reset Register ---------------- */
#define SSIR				(0x04 + SWMISC_BASE)
#define SIRR				(SSIR)			/* alias */
#define TRXRDY				(1 << 0)		/* start normal Tx and Rx */

/* ---- switch-core L2 forwarding (offsets from SWCORE_BASE 0xBB800000) -----
 * Without these the switch never forwards ingress LAN-port frames to the CPU
 * port, so RX is dead (a minimal CPU-DMA start omits them). Registers/values
 * per the vendor AsicDriver reg map (rtl865xc_asicregs.h). */
#define PCRAM_BASE			(RTL819X_SWCORE_BASE + 0x4100)
#define PCRP(n)				(PCRAM_BASE + 0x04 + (n) * 0x04) /* port cfg 0..8 */
#define ALE_BASE			(RTL819X_SWCORE_BASE + 0x4400)
#define MSCR				(ALE_BASE + 0x10)	/* module switch control */
#define SWTCR0				(ALE_BASE + 0x18)	/* switch table control 0 */
#define FFCR				(ALE_BASE + 0x28)	/* frame forwarding control */

#define SW_CPU_PORT			6			/* CPU = L2 port 6 (bit 6) */
#define MSCR_EN_L2			(1 << 0)		/* global: enable L2 fwd */
#define PCR_EN_PHY_IF			(1 << 0)		/* enable the port MAC/PHY iface */
#define PCR_MAC_SW_RESET		(1 << 3)		/* 1 = out of reset (normal) */
#define PCR_STP_FORWARDING		(3 << 4)		/* STP port state = forwarding */
#define FFCR_UNKMC_2CPU			(1 << 0)		/* trap unknown multicast to CPU */

/* ---- switch shared-buffer / descriptor flow-control thresholds -----------
 * The vendor rtl8651_clearRegister() (sdk-ref/rtl865x_asicCom.c:1124-1134)
 * programs these; this port had dropped them, leaving the switch's single
 * shared descriptor pool (max 1023 dscs) with NO back-pressure thresholds. A
 * frame spans multiple buffer-page descriptors, so a large frame needs many
 * dscs; with no turn-on/off/runout thresholds the fabric mismanages the pool
 * and drops large frames (congestion-sensitively) and wedges when the pool
 * exhausts (-> watchdog reset). Small (1-descriptor) frames survive, which is
 * why ping worked but frames >~500 B failed and degraded under load. */
#define SBFCTR				(RTL819X_SWCORE_BASE + 0x4500)
#define SBFCR0				(SBFCTR + 0x00)		/* S_DSC_RUNOUT threshold */
#define SBFCR1				(SBFCTR + 0x04)		/* S_DSC FCOFF<<16 | FCON */
#define SBFCR2				(SBFCTR + 0x08)		/* Max_SBuf FCOFF<<16 | FCON */
#define PBFCR0				(SBFCTR + 0x0C)		/* per-port MaxDSC FCOFF<<16|FCON */
/* PBFCR1..5 = PBFCR0 + n*0x04 */

/* ---- descriptor-pool diagnostic (DESCDIAG, SWCORE+0x6100) ---------------- */
#define DESCDIAG_BASE			(RTL819X_SWCORE_BASE + 0x6100)
#define GDSR0				(DESCDIAG_BASE + 0x000)	/* Global Descriptor Status 0 */
#define GDSR0_USEDDSC_MASK		(0x3ff << 16)		/* total used descriptors NOW */
#define GDSR0_DSCRUNOUT			(1 << 27)		/* descriptor run-out latched */
#define GDSR0_TOTALDSC_FC		(1 << 26)		/* total-descriptor flow-control event */
#define GDSR0_SHAREDBUF_FCON		(1 << 14)		/* shared-buffer FCON threshold hit */
#define GDSR0_MAXUSEDDSC_MASK		(0x3fff << 0)		/* max-used-dsc history (high-water) */
#define GDSR1				(DESCDIAG_BASE + 0x004)	/* Global Descriptor Status 1 */
#define PCSR0				(DESCDIAG_BASE + 0x008)	/* Port Congestion Status 0 (P0-3 OQ) */
#define PCSR1				(DESCDIAG_BASE + 0x00C)	/* Port Congestion Status 1 (P4-6 OQ + IQ) */
#define Pn_DCR0(p)			(DESCDIAG_BASE + 0x010 + (p) * 0x10)	/* per-port dsc-count regs */

/* Per-port/queue congestion status - vendor rtl819x_poll_sw reads 0xBB80610C
 * (== PCSR1) bit16 = CPU port (6) queue0 congested; combined with frozen Rx/Tx
 * descriptor pointers it is the fabric-wedge signature (asicBasic.c:502). Reading
 * it also DRAINS the latched port-congestion state (the M6.5 wedge mitigation). */
#define GDSR_PORT_CONG			PCSR1
#define PORT6_Q0_CONG			(1 << 16)

/* ---- switch MAC config (reserve: carrier-based back-pressure tolerance) --- */
#define SW_MACCR			(RTL819X_SWCORE_BASE + 0x4000)
#define SW_MACCR_LONG_TXE		(1 << 22)		/* vendor MACCR=LONG_TXE */
#define FFCR_UNKUC_2CPU			(1 << 1)		/* trap unknown unicast to CPU */
#define SWTCR0_UNKVID_2CPU		(1 << 15)		/* trap VLAN-lookup-miss to CPU */

/* ---- switch TABLE RAM + TLU table-command interface ---------------------
 * The proper way to make the CPU port a VLAN member (so ingress frames flood
 * to it through the normal CPU RX ring, not the trap engine). Table entries
 * are force-added via the TLU: fill TCR0.., point SWTAA at the table-RAM slot,
 * write SWTACR=START|FORCE, spin until ACTION_DONE. Values verbatim from the
 * vendor AsicDriver (rtl865xc_asicregs.h / rtl865x_asicBasic.c). */
#define RTL819X_SWTBL_BASE		0xBB000000	/* switch table RAM (phys 0x1B000000) */
#define SW_TYPE_VLAN_TABLE		6		/* enum TYPE_VLAN_TABLE */
#define VLAN_TBL_ADDR(vid)		(RTL819X_SWTBL_BASE + (SW_TYPE_VLAN_TABLE << 16) + (vid) * 0x20)

#define TACI_BASE			(RTL819X_SWCORE_BASE + 0x4D00)
#define SWTACR				(TACI_BASE + 0x000)	/* table access control */
#define SWTAA				(TACI_BASE + 0x008)	/* table access address */
#define TCR0				(TACI_BASE + 0x020)	/* table content word 0.. */
#define TLU_ACTION_MASK			0x1
#define TLU_ACTION_DONE			0x0
#define TLU_ACTION_START		0x1
#define TLU_CMD_FORCE			(1 << 3)

/* Per-port default VLAN id (PVID) registers: 2 ports/reg, even port in
 * bits[11:0], odd port in bits[27:16] (see vendor rtl8651_setAsicPvid). */
#define PVCR0				(RTL819X_SWCORE_BASE + 0x4A08)

/* ---- descriptor ownership / wrap bits (shared Tx/Rx) -------------------- */
#define DESC_OWNED_BIT			(1 << 0)
#define DESC_RISC_OWNED			(0 << 0)	/* owned by CPU */
#define DESC_SWCORE_OWNED		(1 << 0)	/* owned by switch core */
#define DESC_WRAP			(1 << 1)

/* ---- KSEG0<->KSEG1 uncached alias bit ----------------------------------- */
#define UNCACHE_MASK			0x20000000

/* ---- packet-header protocol type field (Tx opts1 type / Rx opts3 type) -- */
#define PKTHDR_ETHERNET			0
#define PKTHDR_PPTP			1
#define PKTHDR_IP			2
#define PKTHDR_ICMP			3
#define PKTHDR_IGMP			4
#define PKTHDR_TCP			5
#define PKTHDR_UDP			6
#define PKTHDR_IPV6			7

/* ph_flags / extport helpers used by the Tx path */
#define PKTHDR_HWLOOKUP			0x0020
#define PKTHDR_EXTPORT_LIST_CPU		3

/* ---- register accessor (KSEG1, faithful to vendor REG32) ---------------- */
#define REG32(reg)			(*(volatile u32 *)(uintptr_t)(reg))

#endif /* _RTL819X_REGS_H */
