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
#include <linux/delay.h>	/* msleep/udelay: M7 fabric-reset sequencing */
#include <asm/mipsregs.h>	/* clear_c0_status / set_c0_status / STATUSF_IP4 */
#include <net/dsa.h>		/* netdev_uses_dsa: this netdev is the DSA conduit */

#include "rtl819x_regs.h"
#include "rtl819x_swnic.h"
#include "rtl865x_asichal.h"	/* rtl865x_hal_lock: serializes all TLU/table access */
#include "rtl819x_hwnat.h"	/* M6.6 Phase 3: conntrack HW-NAT offload hooks */

/* M7.3 line-rate: 256 (was 64). The CPU punt path must absorb one full
 * lost-RX_DONE window — the RX IRQ is documented-lossy (CPUIISR-ack race, see
 * the M6.3b note in the poll) and the watchdog back-stop is ~10 ms, so at the
 * ~13 kpps software-forwarding rate a single lost interrupt queues ~130 frames
 * against the old 64-slot ring -> descriptor runout -> drops that no port MIB
 * counts (the fabric retries/discards internally) -> TCP retransmits with zero
 * visible loss. 256 slots = ~20 ms of headroom; cost ~512 KB of RX clusters +
 * ~20 KB of coherent descriptors on a 64 MB box. NAPI weight stays 64 (budget
 * per poll pass; the re-peek/IRQ loop drains the deeper ring fine). */
#define RTL819X_RX_RING_SIZE	256
#define RTL819X_TX_RING_SIZE	256
#define RTL819X_CLUSTER_SIZE	2048

/* CPU-interface register offsets within the ioremapped window. */
#define R_CPUIIMR		0x028
#define R_CPUIISR		0x02c

/* NIC interrupt enable set: Rx-done + Tx-all-done + descriptor-runout.
 * LINK_CHANGE_IE is deliberately EXCLUDED: LINK_CHANGE_IP is a level bit that
 * write-1-ack does not clear while the link settles, so re-arming CPUIIMR with
 * LINK_CHANGE_IE re-fires instantly on cable plug-in -> IRQ livelock -> wedge.
 * PKTHDR_DESC_RUNOUT_IE is INCLUDED (M6.3b): under sustained load napi can
 * fall behind, the Rx ring empties of CPU-owned slots, and the switch hits
 * descriptor runout; arming it kicks napi promptly to drain+refill instead
 * of waiting on the ~10ms watchdog (which lets the CPU-port queue congest and
 * hard-wedge the fabric - the shipped stock kernel arms it too).
 * MBUF_DESC_RUNOUT_IE was included by M6.3b as well but is now OFF by default,
 * because the shipped stock kernel leaves it masked - see the R2 block below.
 * The refill-lag storm the original code feared is avoided because napi masks
 * the source while polling and re-checks for pending work on complete.
 *
 * ---- R2: MBUF_DESC_RUNOUT_IE (bit16) is now OFF by default -------------------
 * The SHIPPED STOCK KERNEL writes CPUIIMR = 0x807E31FE (decoded from stock
 * vmlinux 0x80192bb0: `lui v1,0x807e; addiu v1,v1,12798; sw v1,0x28(v0)`), i.e.
 *   LINK_CHANGE | PKTHDR_DESC_RUNOUT_ALL | RX_DONE_ALL | TX_ALL_DONE_ALL
 * — PKTHDR runout IS armed, but bit16 MBUF_DESC_RUNOUT is NOT. Ours was
 * 0x007F31FE: bit16 set (and LINK_CHANGE deliberately clear, see above).
 *
 * The comment above cited the public SDK (asicCom.c:1417) for arming "them"
 * (both runout sources). ★ This is the SAME SDK-vs-shipped-stock discrepancy
 * class as the DMA_CR0 water marks in rtl865x_start(), where the SDK says
 * 0xA0A0 and the shipped stock kernel says 0xA0CE — and that one turned out to
 * be directly implicated in this very wedge. The shipped kernel is ground truth.
 *
 * Mechanism this is suspected to fix (R2 = stop the large-frame CPU-RX wedge at
 * source rather than self-healing it): the wedge is an RX-FIFO drain-lag race
 * under CPU saturation, where descriptor writeback overtakes the multi-burst
 * payload DMA. MBUF runout asserts exactly during that saturation window, and if
 * MBUF_DESC_RUNOUT_IP is a level bit that write-1-ack cannot clear until the
 * buffer pool actually refills, arming it produces precisely the re-fire ->
 * IRQ-livelock -> wedge pattern this driver already documents for LINK_CHANGE_IE
 * two paragraphs up. Dropping bit16 keeps M6.3b's prompt-napi kick (PKTHDR
 * runout, which stock DOES arm and which covers the descriptor ring) while no
 * longer arming the one source stock leaves masked.
 *
 * ⚠ HYPOTHESIS, NOT YET PROVEN ON HARDWARE. The R2 gate is 10 minutes of
 * bidirectional saturating traffic with ZERO fabric resets; that has not been
 * run. Exposed as a runtime knob so the bench can A/B it without a rebuild:
 *   echo 1 > /sys/module/rtl819x/parameters/mbuf_runout_ie   # pre-R2 behaviour
 *   ip link set eth0 down; ip link set eth0 up               # re-arm, then load
 * If the wedge rate is unchanged with 0, this candidate is FALSIFIED — say so in
 * docs/M7-LARGE-FRAME-RX-WEDGE.md rather than leaving it ambiguous.
 *
 * ★ DEFAULT IS 0 (masked, stock-aligned) — this is R2's deliverable, and it has
 * now been measured on hardware rather than assumed.
 *
 * GATE RUN (2026-07-30), with this knob at 0:
 *   888,569 x 1400-byte box-terminating frames, 9 minutes continuous saturation
 *   -> 0% packet loss
 *   -> 0 fabric resets          (the gate criterion)
 *   -> 0 FCS wedge detections   (the self-heal detector never armed)
 *   -> 56 B / 1400 B / NAT all 0% loss afterwards
 * Large box-terminating traffic is precisely the documented trigger for this
 * wedge (task #15: "breaks DHCP, SSH, any large box-terminating packet").
 *
 * ⚠ HONEST COVERAGE — what this run does and does not establish:
 *  - It DOES establish that masking bit16 is safe. It does not break RX. An
 *    earlier note here claimed, as measured fact, that masking it left the RX
 *    engine dead across cold boots; that was CONFOUNDED (the harness ran
 *    `ip neigh flush` immediately before each ping, recreating the task-#13
 *    cold-unicast condition, so it measured the flush) and is retracted.
 *  - It does NOT prove the candidate FIXED the wedge. The wedge only reproduces
 *    ~1 in 3-4 heavy attempts, so one clean run is consistent with "fixed" AND
 *    with "did not fire this time". No A/B baseline at 1 was run for comparison.
 *  - Coverage gap: the load was box-terminating ICMP, NOT bidirectional
 *    FORWARDED saturation. M6.3b's original reason for arming bit16 was napi
 *    falling behind under forwarded load and the Rx ring running out of
 *    CPU-owned slots — that exact scenario is still untested here. (iperf3's
 *    server would not stay up on the WAN peer, hence the flood.)
 * So: default 0 because it matches shipped-stock ground truth AND passed the
 * stated gate; revert to 1 if forwarded-saturation testing ever regresses. */
static int mbuf_runout_ie;		/* 0 = masked, stock-aligned (R2) */
module_param(mbuf_runout_ie, int, 0644);
MODULE_PARM_DESC(mbuf_runout_ie,
		 "arm MBUF_DESC_RUNOUT_IE / CPUIIMR bit16: 0=masked, matches shipped stock CPUIIMR 0x807E31FE (default, R2 — passed a 9 min large-frame gate with 0 resets), 1=armed, pre-R2 fallback");

#define NIC_IIMR		(RX_DONE_IE_ALL | TX_ALL_DONE_IE_ALL | \
				 PKTHDR_DESC_RUNOUT_IE_ALL | \
				 (mbuf_runout_ie ? MBUF_DESC_RUNOUT_IE_ALL : 0u))

struct rtl819x_eth_priv {
	struct net_device	*dev;
	struct napi_struct	napi;
	void __iomem		*base;	/* CPU-interface window (ioremapped) */
	int			irq;
	struct timer_list	rx_timer;	/* polling: drives napi (RX IRQ storms) */
	struct work_struct	hang_work;	/* M6.5: fabric-wedge soft-recover */
};

/* M6.6 Phase 3 fast-offload: the RX IRQ (CP0 IP4) is unreliable on this SoC — a
 * frame landing in the receive-empty/CPUIISR-ack race window loses its RX_DONE, so
 * at low-to-moderate rate RX ends up driven by THIS timer, not the IRQ (measured:
 * ~45ms/1-way ping latency == the old 100ms tick / 2). At 100ms that batches
 * moderate-rate forwarded traffic into 100ms bursts, which wrecks TCP timing and
 * collapses cwnd on the software (pre-offload) path — so a new flow's first-RTT
 * packets die before the conntrack ndo_flow_offload ADD can move it to hardware.
 * Poll every 1 jiffy (10ms @ HZ=100) instead: RX latency drops ~5x, moderate-rate
 * software forwarding survives, and a flow stays alive long enough to offload.
 * (Idempotent under load — napi_schedule is a no-op while napi is already active, so
 * the extra ticks cost nothing when a burst is actually being drained.) */
#define RTL819X_WATCHDOG_INTERVAL	DIV_ROUND_UP(HZ, 100)	/* ~10ms fast RX poll */

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
static void sw_add_vlan_fid(uint32 vid, uint32 member_mask, uint32 untag_mask, uint32 fid)
{
	uint32 entry[8] = { 0 };
	int i, guard;

	entry[0] = (member_mask & 0x3F)			/* memberPort   [5:0]  */
		 | (((member_mask >> 6) & 0x7) << 6)	/* extMemberPort[8:6]  */
		 | ((untag_mask & 0x3F) << 9)		/* egressUntag  [14:9] */
		 | (((untag_mask >> 6) & 0x7) << 15)	/* extEgressUntag[17:15]*/
		 | ((fid & 0x3) << 18);			/* fid          [19:18] */

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

/* ★ FID: the L2/FDB lookup for a routed egress is keyed by {MAC, FID}, and the FID comes
 * from the frame's VLAN entry. This driver writes peer L2 entries under fid 1 for the WAN
 * side and fid 0 for the LAN side (rtl865x_asichal.c:404,644,700) -- but sw_add_vlan()
 * never wrote the VLAN table's fid field at all, so BOTH VLANs claimed fid 0. The WAN
 * peer's entry therefore lived in FID 1 while a WAN-bound lookup searched FID 0 and
 * missed, leaving the ASIC with no egress to commit to. That is exactly the observed
 * signature: the L4 NAPT row matches and reloads its age, yet every packet is delivered
 * to the CPU. Fork A never noticed because it never resolved a per-port egress anyway.
 * (The vendor keeps the same split: eFID on the 8367S, FID on the SoC.) */
static __maybe_unused void sw_add_vlan(uint32 vid, uint32 member_mask, uint32 untag_mask)
{
	sw_add_vlan_fid(vid, member_mask, untag_mask, 0);
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

/*
 * ---------------------------------------------------------------------------
 * M7 fabric full reset (the large-frame RX-corruption wedge recovery)
 * ---------------------------------------------------------------------------
 * Measured wedge: after sustained large-frame CPU-RX load the switch delivers
 * multi-cell (>~128 B) frames with a CORRECT pkthdr/ph_len but CORRUPT cluster
 * payload (~93% fail ip_rcv checksum), while single-cell frames stay clean.
 * The latch survives eth0 down/up (rtl865x_down + New_swNic_init +
 * rtl865x_start) — it lives in switch-core NIC/fabric state below the CPU DMA
 * engine (cell-gather / RX-FIFO / internal free-list, invisible in GDSR/PCSR).
 * Stock-kernel coherency handling was verified equivalent to this port
 * (stock swNic_receive @0x8016eeb8: 32 B pkthdr+mbuf invalidate at consume,
 * full-cluster invalidate at re-arm == our uncached pools + dma_map_single),
 * so this is device-side latched state, not a dropped cache op.
 *
 * The only vendor reset that reaches that state is FullAndSemiReset()
 * (sdk-ref/rtl865x_asicCom.c:2122, stock @0x8019396c, called at every stock
 * ethernet init @0x8070be78): SIRR FULL_RST + a switch-core CLOCK GATE cycle.
 * That resets ALL switch tables & queues and returns config registers to
 * defaults, so the recovery snapshots a curated known-good register set at
 * first bring-up and restores it after the reset, then re-runs the normal
 * engine bring-up (New_swNic_init + rtl865x_start re-add rings/VLANs/PVIDs/
 * trunk/thresholds). ASIC L3/NAT scaffolding+flows are re-armed via
 * rtl819x_hwnat_stop/start plus a userspace `cat /proc/rtl865x_gw`.
 */

/* Curated config snapshot: every register the datapath depends on that the
 * LOADER or this driver set up and that SIRR_FULL_RST + the clock cycle would
 * revert to chip defaults. Ranges are contiguous register blocks (vendor map,
 * rtl865xc_asicregs.h). Deliberately EXCLUDED: SIRR/CPUICR/CPUIIMR/CPUIISR and
 * the ring-base registers (rebuilt by rtl865x_start/New_swNic_init), table RAM
 * (re-added by start/gw_prog), MIB/GDSR/PCSR (status).
 * Notable members:
 *  - CPUICR1 (0xB80100A4): pkthdr type + TX/RX gather config. Stock programs
 *    it right after FullAndSemiReset (=0x100|0x40: TX_PKTHDR_SHORTCUT_LSO +
 *    CF_TX_GATHER, @0x8070c014-38); this port INHERITS it from the loader, so
 *    it must be restored or RX/TX descriptor parsing changes shape.
 *  - DMA_CR0 whole (HsbAddrMark address-window bits[19:16], not just marks).
 *  - PCRP0-8 + PITCR + P0GMIICR + MACCR: the RGMII trunk force-link config the
 *    loader's init_8367r left behind (rtl865x_start only RMWs bits on top).
 *  - The egress-scheduler block (QIDDPCR/P0Q0RGCR/WFQ/ELB/ILB/PATP): restored
 *    to the LOADER-established baseline that provably carries line-rate TCP —
 *    NOT the vendor-SDK constants that the A-2 experiment proved starve TCP.
 */
static const struct { u32 base; u16 cnt; } fab_cfg_ranges[] = {
	{ 0xB8010030,  3 },	/* CPUQDM0-5 (3x32-bit view of six u16 regs) */
	{ 0xB801003C,  3 },	/* DMA_CR0-2 (incl HsbAddrMark + FIFO marks) */
	{ 0xB8010068,  1 },	/* DMA_CR3 */
	{ 0xB8010078,  1 },	/* TXRINGCR (tx ring enables) */
	{ 0xB80100A0,  2 },	/* DMA_CR4 (tail-aware), CPUICR1 (pkthdr type/gather) */
	{ 0xBB804000,  1 },	/* MACCR (LONG_TXE / CF_SYSCLK_SEL) */
	{ 0xBB804058,  1 },	/* MACCR1 (trunk bit0 — #14 cold path) */
	{ 0xBB804010, 13 },	/* PPMAR + PATP0-11 (port aging/trunk map) */
	{ 0xBB804100, 10 },	/* PITCR (port0 RGMII mode) + PCRP0-8 (force-link) */
	{ 0xBB80414C,  1 },	/* P0GMIICR (RGMII delays + Conf_done) */
	{ 0xBB805108,  1 },	/* EXTPCR0 (ext-port cfg — #14 cold path) */
	{ 0xBB804400, 12 },	/* TEACR,ATCR?,RMACR,ALECR,MSCR,SWTCR0/1,FFCR block */
	{ 0xBB804500,  9 },	/* SBFCR0-2 + PBFCR0-5 (also rewritten by start) */
	{ 0xBB804730, 19 },	/* LPTM8021Q,DSCPCR0-6,QIDDPCR,RMCR1P,DSCPRM0/1,RLRC */
	{ 0xBB804800, 69 },	/* P0Q0RGCR x42 + WFQRCR/WFQWCR x21 + ELB/ILB buckets */
	{ 0xBB804A00,  8 },	/* VCR0/1 + PVCR0-4 (PVIDs; start rewrites vid2) */
	{ 0xBB806300,  1 },	/* TMCR (test-mode off) */
};

#define FAB_CFG_WORDS 155	/* sum of cnt above (BUILD_BUG_ON-checked) */
static u32 fab_cfg_snap[FAB_CFG_WORDS];
static bool fab_cfg_snap_valid;

static void rtl819x_fabric_snapshot(void)
{
	int r, i, w = 0;

	for (r = 0; r < ARRAY_SIZE(fab_cfg_ranges); r++)
		for (i = 0; i < fab_cfg_ranges[r].cnt; i++) {
			if (WARN_ON_ONCE(w >= FAB_CFG_WORDS))
				return;
			fab_cfg_snap[w++] = REG32(fab_cfg_ranges[r].base + (i << 2));
		}
	fab_cfg_snap_valid = true;
}

static void rtl819x_fabric_restore(void)
{
	int r, i, w = 0;

	if (!fab_cfg_snap_valid) {
		pr_err("rtl819x: fabric restore skipped - no snapshot\n");
		return;
	}
	for (r = 0; r < ARRAY_SIZE(fab_cfg_ranges); r++)
		for (i = 0; i < fab_cfg_ranges[r].cnt; i++) {
			u32 addr = fab_cfg_ranges[r].base + (i << 2);
			u32 val;

			if (WARN_ON_ONCE(w >= FAB_CFG_WORDS))
				return;
			val = fab_cfg_snap[w++];

			/* MSCR: restore CONFIG bits but keep L2/L3/L4 forwarding
			 * OFF until the rings are re-armed — restoring EN_L2 here
			 * would let frames flood a CPU port whose ring registers
			 * are still reset (DMA to phys 0). rtl865x_start re-sets
			 * EN_L2 LAST as designed; gw_prog re-adds EN_L3|EN_L4. */
			if (addr == MSCR)
				val &= ~(MSCR_EN_L2 | (1 << 1) | (1 << 2));
			REG32(addr) = val;
		}
}

/* The vendor 8197F fabric reset + table-SRAM re-init.  Caller MUST have the
 * datapath fully quiesced (napi disabled, rx_timer stopped, TX disabled, HAL
 * mutex held, CPU engine down): between the clock gate and ungate NOTHING may
 * touch 0xBB80xxxx/0xB801xxxx or the Lexra bus access stalls. */
static void rtl819x_fabric_full_reset(void)
{
	int guard;

	/* FullAndSemiReset(), CONFIG_RTL_8197F branch (asicCom.c:2125-2133;
	 * stock 0x8019396c verified instruction-for-instruction). */
	REG32(SIRR) |= SIRR_FULL_RST;
	msleep(300);
	REG32(SYS_CLK_MAG) &= ~CM_ACTIVE_SWCORE;
	msleep(300);
	REG32(SYS_CLK_MAG) |= CM_ACTIVE_SWCORE;
	msleep(50);

	/* Stock rtl8651_initAsic table-SRAM init (0x801928c8): MEMCR=0 -> 0x24,
	 * poll (MEMCR & 0x2400) == 0x2400.  Without it the TLU table writes that
	 * follow land in uninitialised SRAM control state. */
	REG32(MEMCR) = 0;
	REG32(MEMCR) = MEMCR_INIT_CMD;
	for (guard = 0; guard < 200000 &&
	     (REG32(MEMCR) & MEMCR_INIT_DONE) != MEMCR_INIT_DONE; guard++)
		udelay(1);
	if ((REG32(MEMCR) & MEMCR_INIT_DONE) != MEMCR_INIT_DONE)
		pr_err("rtl819x: fabric reset: MEMCR init timeout (%08x)\n",
		       REG32(MEMCR));

	/* ★ L4/NAPT table SRAM clear — a SEPARATE bit the init command above does not
	 * cover (see MEMCR_L4_CLEAR in rtl819x_regs.h). Vendor rtl8651_clearAsicNaptTable()
	 * is gated on CONFIG_RTL_8197F, so this is required on this silicon. */
	REG32(MEMCR) &= ~MEMCR_L4_CLEAR;
	REG32(MEMCR) |= MEMCR_L4_CLEAR;
	for (guard = 0; guard < 100000 &&
	     (REG32(MEMCR) & MEMCR_L4_CLEAR_DONE) != MEMCR_L4_CLEAR_DONE; guard++)
		cpu_relax();
	if ((REG32(MEMCR) & MEMCR_L4_CLEAR_DONE) != MEMCR_L4_CLEAR_DONE)
		pr_err("rtl819x: L4 table clear timeout (MEMCR=%08x)\n", REG32(MEMCR));
	else
		pr_info("rtl819x: L4/NAPT table SRAM cleared (MEMCR=%08x)\n", REG32(MEMCR));

	rtl819x_fabric_restore();
	pr_err("rtl819x: fabric full reset done (SIRR FULL_RST + swcore clock cycle + MEMCR init + cfg restore)\n");
}

/* trunk_cold_force removed along with Fork A. It existed to force the trunk cold
 * replica on a loader-configured boot, because the replica used to be gated on
 * "loader has NOT already done it". CPU-tag mode needs the P0GMIICR tag bits that only
 * that block programs, so the gate became unconditionally true the moment cpu_tag
 * defaulted to 1 — i.e. the replica has already been running on every eth0 open, and
 * the knob has been dead since then. rtl865x_start() also re-runs it after
 * fabric_reset=3 (see the New_swNic_init + rtl865x_start pair), so there is nothing
 * left to re-arm by hand. */
/*
 * The 8367S half of the CPU-tag arrangement is programmed by the DSA driver
 * (drivers/net/dsa/realtek/rtl8365mb.c) now, not from here.
 */

/* ---- CPU-tag ("port0 router") mode: the vendor's 8197F + 8367R arrangement ----
 * This is now the ONLY model. It replicates what the vendor SDK does for exactly this
 * SoC+switch pair (CONFIG_RTL_CPU_TAG, implied by CONFIG_RTL_8367R_SUPPORT): the SoC
 * MAC inserts and strips a 4-byte Realtek 0x8899 tag on the trunk IN HARDWARE, carrying
 * the source and destination port. The jacks appear to the SoC as its own ports 0-4
 * with the CPU on port 8, exactly as the stock firmware's netif/L2 tables show.
 *
 * The 8367S end must agree (reg 0x121a = 0x2b1, programmed by the DSA driver) or
 * the link goes deaf — measured, 100% loss at every frame size. Both ends
 * together or neither; either alone breaks the datapath.
 *
 * ---- HISTORICAL: what "Fork A" was, and why it is gone -----------------------------
 * Fork A hid all five jacks behind the single RGMII trunk and picked the jack by VLAN
 * ID *after* the frame left the SoC. The SoC switch therefore only ever saw ONE port,
 * so a routed unicast had no distinct egress port to commit to and the ASIC trapped
 * every packet to the CPU — which is why hardware NAT never engaged there despite the
 * L4 NAPT rows matching. It was kept behind cpu_tag=0 as a fallback while CPU-tag mode
 * was proven. It is proven: 891/906 Mbit with 0.0% of payload bytes crossing the CPU.
 * Fork A was removed once that held across a cold NOR boot, so the datapath now has a
 * single shape rather than two that had to be reasoned about together. Recover it from
 * git history if a comparison is ever needed. */
/* Board jack layout (DIR-842, confirmed in GWR1200ACV1.dts:113-120 and the vendor
 * header): the RTL8367S carries the five GbE jacks as ports 0-4, with the WAN jack on
 * port 4 and LAN on 0-3. In CPU-tag mode these become the SoC's own port numbers, and
 * the CPU moves to port 8 (reachable only via the extMemberPort bit-slice). */
#define GW_JACK_WAN		4
#define GW_JACK_LAN_MASK	0x0F		/* jacks 0-3 */
#define GW_PORT_CPU_TAGGED	8		/* CPU port number in CPU-tag mode */

/* cpu_tag / Fork A removed — CPU-tag mode is unconditional (see the block comment
 * above). rtl865x_asichal.c no longer externs it. */

/* A-2 residual: DISABLE 802.3x PAUSE on the RGMII trunk (SoC-P0 <-> 8367S-EXT1),
 * applied in rtl865x_start(). MEASURED, and OPPOSITE the original hypothesis:
 * the loader FORCES pause on both trunk ends (0x1311=0x1076, PCRP0[17:16]=3),
 * and on this single trunk — which a routed LAN->WAN flow crosses TWICE (VID2
 * ingress + VID1 egress on the SAME port0) — that pause is a self-inflicted
 * feedback loop: WAN-egress back-pressure PAUSEs the LAN ingress on the same
 * wire, throttling a saturating single TCP flow. Bench A/B (fresh iperf3, plain
 * routed hal->tiny): pause ON = ~0.9 Mbit collapse; pause OFF = ~95 Mbit
 * sustained (vs ~27 Mbit with the loader's untouched pause). So default = 2
 * (force-clear both ends). Values: 2 = clear (default, the fix); 1 = force
 * enable (bench A/B — reproduces the collapse); 0 = leave the loader value
 * (~27 Mbit baseline). SWCORE regs have no devmem path, so this param is the
 * live lever — A/B/A on ONE RAM boot (no flaky flash reboots):
 *   echo 1 > /sys/module/rtl819x/parameters/trunk_pause  # pause on  (collapse)
 *   echo 2 > /sys/module/rtl819x/parameters/trunk_pause  # pause off (fix)
 *   ip link set eth0 down; ip link set eth0 up           # re-apply, then measure */
static int trunk_pause = 2;
module_param(trunk_pause, int, 0644);
MODULE_PARM_DESC(trunk_pause, "RGMII-trunk 802.3x pause: 2=force OFF (default, A-2 residual fix), 1=force on (bench A/B collapse), 0=leave loader value");

/*
 * The 8367S half of the trunk-pause fix comes from the DSA driver, which takes
 * it from the CPU port's fixed-link: omitting "pause" there leaves the switch's
 * force-mode register without the pause bits, which is what this side wants.
 */

static void rtl865x_start(void)
{
	/*
	 * ★ CF_NIC_LITTLE_ENDIAN (CPUICR1 bit 1) — byte order of the NIC master
	 * Lexra bus. This was NEVER programmed here; the driver silently
	 * inherited whatever the bootloader left, and the bootloader only sets
	 * it when it runs its own network init. A TFTP RAM boot fetches the
	 * image over the wire, so the bit is set (CPUICR1=0x82) and everything
	 * works; a NOR flash boot never touches the NIC, leaves it clear
	 * (CPUICR1=0x80), and the wired datapath is dead.
	 *
	 * With the bit clear the DMA engine writes every received frame into
	 * DRAM 32-bit-word byte-swapped. Measured on the bench: a ping from
	 * aa:bb:cc:00:00:02 to this box lands in the mbuf as
	 *     51 fc 1c e0  e0 00 ef c9  90 59 12 4c  02 00 00 81  00 45 00 08
	 * i.e. each word of "e0 1c fc 51 | c9 ef 00 e0 | 4c 12 59 90 |
	 * 81 00 00 02 | 08 00 45 00" reversed. eth_type_trans() then reads the
	 * destination MAC as 51:fc:1c:e0:e0:00 — octet 0 bit 0 set, so the
	 * frame is classified PACKET_MULTICAST — and the EtherType as 0x0200
	 * instead of 0x8100, so the 802.1Q untag path below is never taken.
	 * The frames are received and counted but reach neither the bridge nor
	 * the IP stack, so the box answers nothing. The fingerprint is
	 * eth0.2 rx_packets and rx_multicast rising 1:1 while br-lan rx stays
	 * flat. Vendor does this in its 8197F chip init,
	 * AsicDriver/rtl865x_asicL2.c:7647.
	 *
	 * Set bit 1 ONLY. The vendor's line also ORs CF_TXRX_DIV_LX (bit 0) and
	 * CF_TSO_ID_SEL (bit 4), but every boot that measured 890/900 Mbit of
	 * hardware offload ran with exactly 0x82 — so reproduce the known-good
	 * value and nothing more. Runs before the CPUICR write below so the bus
	 * is in the right byte order before Tx/Rx are enabled, and before the
	 * fabric snapshot at the end of this function so a fabric_reset
	 * restores the corrected value rather than the loader's.
	 */
	REG32(CPUICR1) |= (1u << 1);

	/* Enable Tx/Rx, 128-word Lexra bus burst, 2048-byte mbufs. */
	REG32(CPUICR) = TXCMD | RXCMD | BUSBURST_128WORDS | MBUF_2048BYTES;

	/*
	 * 8197F: the Hi/Low FIFO water marks reset to defaults (HiFifoMark 0x57)
	 * whenever the burst-size field of CPUICR is written, so restore them here.
	 *
	 * A-2 fabric wedge: write the SHIPPED stock kernel's 8197F value 0xA0CE
	 * (LowFifoMark=0xA0, HiFifoMark=0xCE — stock vmlinux 0x80192be4
	 * `ori v0,v0,0xa0ce` on the chip-ID==0x8197 branch this SoC takes), NOT
	 * the public SDK's 0xA0A0 (asicCom.c:1408) this port originally copied.
	 * Hi==Low==0xA0 is a degenerate no-hysteresis drain config; DMA_CR0
	 * governs when the CPU-port DMA drains its internal FIFO to DRAM relative
	 * to the descriptor writeback — the exact ordering the A-2 stale-DRAM
	 * race (descriptor overtakes payload, M7-LARGE-FRAME-RX-WEDGE.md) lives
	 * in.  rtl865x_start() runs at cold boot AND after every self-heal, so
	 * this covers both paths (the bringup pr_err below prints DMA_CR0 for
	 * bench verification: expect ....A0CE).
	 */
	REG32(DMA_CR0) = (REG32(DMA_CR0) & ~(LowFifoMark_MASK | HiFifoMark_MASK)) |
			 ((0xA0 << LowFifoMark_OFFSET) | 0xCE);

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
	 * M6.6 KNOWN-OPEN (A-2): sustained max-rate ROUTED bulk (~700 Mbit/s iperf3
	 * through the ASIC L3 engine) can latch the fabric into a state where
	 * multi-descriptor (large) frames die on a routed egress direction while
	 * small frames pass — the routed-path sibling of the M6.5 CPU-path congestion
	 * wedge (the GDSR_PORT_CONG drain can't reach it; recovery = eth0 down/up +
	 * re-trigger /proc/rtl865x_gw). The vendor rtl8651_clearRegister() egress
	 * packet-scheduler block (sdk-ref/rtl865x_asicCom.c:1112-1152: QIDDPCR,
	 * P0Q0RGCR rate-guarantee, WFQ, ELB/ILB leaky buckets) was tried here as the
	 * fix and REJECTED — transplanted onto this otherwise-default queue config it
	 * STARVES sustained TCP to 0 bit/s (two variants measured: with QIDDPCR the
	 * L4-classified queue has zero WFQ weight; even rate-regs-only, the leaky
	 * bucket/rate-guarantee values cap bulk while ICMP + the iperf3 control
	 * connection still pass). Revisit after Phase 2/3 (NAPT rows reshape the
	 * L4 datapath); do NOT re-add the block without solving the queue mapping.
	 */

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

		/* PCRP(n) is documented "port cfg 0..8". CPU-tag mode puts the CPU on
		 * port 8, so the loop must reach it or the CPU port never leaves
		 * blocking. (Fork A only ever went to 6.) */
		for (p = 0; p <= GW_PORT_CPU_TAGGED; p++)
			REG32(PCRP(p)) |= PCR_STP_FORWARDING;
		/*
		 * Two VLANs across the CPU <-> RTL8367S RGMII trunk: VID 2 = LAN,
		 * VID 1 = WAN, matching stock's netif table.
		 *
		 * The jacks are REAL SoC ports in CPU-tag mode, so the two VLANs get
		 * DISTINCT membership. (Fork A had to use a 0x7F superset in both,
		 * which is precisely why LAN and WAN were indistinguishable by
		 * membership and a routed unicast had no egress port to commit to.)
		 *
		 * Vendor reference (include/net/rtl/rtl865x_netif.h:626,760-765):
		 *   RTL_CPU_PORT      8
		 *   RTL_LANPORT_MASK  0x10f   jacks 0-3 + CPU port 8
		 *   RTL_WANPORT_MASK  0x10    jack 4
		 * sw_add_vlan()'s flat mask splits at bit 6, so bit 8 lands in
		 * extMemberPort bit 2 = port 8. Board layout: WAN = jack 4.
		 *
		 * ⚠ The vendor's WAN mask omits the CPU port while its LAN mask
		 * includes it. That asymmetry is real but unexplained, and dropping
		 * the CPU out of the WAN VLAN is exactly how WAN dies silently. We
		 * stay symmetric (CPU in both); the literal 0x10 is only worth trying
		 * as a deliberate A/B.
		 *
		 * untag stays 0x00: our 8367S sends 802.1Q-TAGGED up the trunk and the
		 * SoC reads the VID from the tag -- measured working. (The vendor
		 * instead sends untagged and derives the VID from the ingress port's
		 * PVID; both are coherent, ours is already proven.)
		 */
		/*
		 * ★ Untagged egress, the vendor's model. Under DSA the switch no
		 * longer adds an 802.1Q tag on the trunk -- it carries the Realtek CPU
		 * tag and nothing else -- so the VID has to come from the ingress
		 * port's PVID below rather than from a tag, and frames leaving toward
		 * the trunk must go out untagged.
		 */
		sw_add_vlan_fid(RTL865X_VID_LAN, 0x10F, 0x10F, 0); /* jacks 0-3 + CPU8, fid0 */
		sw_add_vlan_fid(RTL865X_VID_WAN, 0x110, 0x110, 1); /* jack 4    + CPU8, fid1 */
		/* Per-jack PVID now that ports are distinct: the WAN jack must default
		 * to the WAN VID, not LAN. Only matters for untagged ingress, but
		 * leaving every port on LAN would mis-zone the WAN jack the moment
		 * anything arrives untagged. */
		for (p = 0; p <= GW_PORT_CPU_TAGGED; p++)
			sw_set_pvid(p, (p == GW_JACK_WAN) ? RTL865X_VID_WAN
							  : RTL865X_VID_LAN);
		/* mode-2 fix: arm the CPU RX interrupt (GIMR switch-NIC line + CP0
		 * IP4) BEFORE enabling global L2 forwarding, so the CPU is already
		 * ready to drain the RX ring the instant frames flow. If L2-enable
		 * comes first (as it used to, a few lines below), an ingress burst
		 * (ARP/broadcast the moment the port forwards) floods the ring while
		 * no RX-done IRQ/napi is armed -> the ring overflows and the bring-up
		 * hangs right at 'br-lan forwarding' (the intermittent mode-2 freeze).
		 * This is the same "frames flow before the CPU path is ready" hazard
		 * the trap-to-CPU comment above warns about. */
		REG32(GIMR) |= BSP_SW_IE;
		set_c0_status(STATUSF_IP4);

		/* --- RGMII trunk (port0 <-> external RTL8367S) cold bring-up ---
		 * #14: FULL replica of the loader's init_97f_8367r (RE'd: init
		 * ~0x80197ed4, trunk setup 0x80194728, P0GMIICR write 0x801947a0;
		 * bit names sdk-ref/rtl865xc_asicregs.h). The loader only runs it
		 * on its TFTP path; a FLASHED boot skips it -> dead trunk. GATED
		 * on PITCR bit0 (port0 iface type): loader sets RGMII (1), chip
		 * default is UTP (0) — loader-configured boots skip this block
		 * UNTOUCHED (the #12 lesson: partial/duplicate re-writes over the
		 * loader's coherent bring-up desync the RGMII pair). Live loader-
		 * boot reference: P0GMIICR=0x00037d55, PITCR bit0=1. The replica now
		 * runs on EVERY open including loader boots, which is itself the
		 * standing proof that it is loader-exact: a correct replica must be a
		 * data-plane no-op over the loader's own bring-up. */
		pr_err("rtl819x trunk-pre : PITCR=%08x P0GMIICR=%08x MACCR=%08x PCRP0=%08x EXTPCR0=%08x MACCR1=%08x PAD=%08x\n",
		       REG32(RTL819X_SWCORE_BASE + 0x4100),
		       REG32(RTL819X_SWCORE_BASE + 0x414C),
		       REG32(RTL819X_SWCORE_BASE + 0x4000),
		       REG32(RTL819X_SWCORE_BASE + 0x4104),
		       REG32(RTL819X_SWCORE_BASE + 0x5108),
		       REG32(RTL819X_SWCORE_BASE + 0x4058),
		       REG32(0xB8000850));
		/* A-2 (M7.3 fabric-wedge prevention): MACCR back-pressure LONG_TXE
		 * (bit22) + CF_SYSCLK_SEL, set UNCONDITIONALLY — the fabric fix must
		 * apply on loader/RAM boots too, not only the gated cold path below.
		 * MACCR is a MAC-level reg (not an RGMII-pair timing reg), so writing
		 * it over the loader's config is safe, unlike the gated PHY regs. */
		REG32(RTL819X_SWCORE_BASE + 0x4000) |= (1u << 12) | SW_MACCR_LONG_TXE;
		/*
		 * ★ Both ends of the RGMII trunk must agree on the CPU tag. The switch
		 * end (reg 0x121a) is the DSA driver's job; this side programs the
		 * P0GMIICR tag bits below. If the two ever disagree the datapath drops
		 * everything -- measured: 100%% loss at every frame size.
		 */

		/* CPU-tag mode REQUIRES the P0GMIICR tag bits, which only this block
		 * programs, so it runs UNCONDITIONALLY -- including on a loader boot
		 * where PITCR bit0 is already set. This used to be gated on
		 * "!loader-configured || trunk_cold_force || cpu_tag"; with cpu_tag
		 * defaulting to 1 that gate was already always true, so making it
		 * unconditional is a no-op in behaviour and removes the last Fork A
		 * branch. A true replica must be a data-plane NO-OP over the loader's
		 * own bring-up -- an earlier PARTIAL replica provably killed the trunk
		 * in exactly this test, which is why every step below is loader-exact. */
		{
			u32 v;

			/* 1. board RGMII pad mux/drive (loader value) */
			REG32(0xB8000850) =
				(REG32(0xB8000850) & 0x019FFFFF) | 0xDA600000;
			/* 2. MACCR CF_SYSCLK_SEL (bits[13:12]=01) + MACCR1 bit0 */
			REG32(RTL819X_SWCORE_BASE + 0x4000) |= (1u << 12);
			REG32(RTL819X_SWCORE_BASE + 0x4058) |= (1u << 0);
			/* 3. EXTPCR0 bits[19:16] = 0x8 (stock init_8367r) */
			v = REG32(RTL819X_SWCORE_BASE + 0x5108);
			REG32(RTL819X_SWCORE_BASE + 0x5108) =
				(v & ~(0xFu << 16)) | (0x8u << 16);
			/* 4. P0GMIICR: GMAC=RGMII (bits[24:23]=0), loader fields
			 * bits[17:16]=3 + bits[15:8]=0x7d, delays TX(bit4)+RX=5.
			 * Conf_done (bit6) stays CLEAR here — latched LAST, after
			 * the settle, exactly like the loader.
			 *
			 * CPU-tag mode: the vendor's 8197F+8367R arrangement, copied
			 * from init_8197f_p0() (sdk rtl865x_asicL2.c:6408):
			 *   P0GMIICR |= (3 << CF_SEL_RGTXC_OFFSET);
			 *   P0GMIICR |= (CFG_CPUC_TAG | CFG_TX_CPUC_TAG);  bits 25,26
			 *   MACCR1   |= PORT0_ROUTER_MODE;                 bit0 (step 2)
			 * The SoC MAC then inserts/strips the 4-byte 0x8899 tag in
			 * HARDWARE, so the RX descriptor's spa carries the real jack
			 * and TX can name a destination port. Conf_done still latches
			 * last, below — that ordering is what the loader relies on. */
			v = REG32(RTL819X_SWCORE_BASE + 0x414C);
			v &= ~((3u << 25) | (3u << 23) | (3u << 18) |
			       (3u << 16) | (0xFFu << 8) | 0xFFu);
			v |= (3u << 16) | (0x7Du << 8) | (1u << 4) | (5u << 0);
			v |= (3u << 25) | (3u << 18);	/* CFG_CPUC_TAG|CFG_TX_CPUC_TAG, CF_SEL_RGTXC */
			REG32(RTL819X_SWCORE_BASE + 0x414C) = v;
			/* 5. PITCR port0 = RGMII (default UTP!) */
			REG32(RTL819X_SWCORE_BASE + 0x4100) |= (1u << 0);
			/* 6. PCRP0 trunk force-link: EnablePHYIf(0)|MacSwReset(3,
			 * 1=normal)|ForceDuplex(18)|ForceSpeed1000M(2<<19)|
			 * ForceLink(23)|EnForceMode(25) = 0x02940009, then the
			 * vendor EnForceMode double-toggle latch
			 * (TOGGLE_BIT_IN_REG_TWICE, sdk-ref asicCom.c). */
			/* ★ Multi-bit FIELDS must be CLEARED before being OR'd in, or
			 * whatever the loader left merges with what we want. This used to
			 * be a bare `| 0x02940009`, which is correct only if the loader
			 * happens to leave ForceSpeed[20:19] at 0 or 2. A NOR cold boot
			 * leaves it at 1 (100M), and 1|2 = 3 = the RESERVED speed code:
			 * the MAC then sits in force mode with an invalid speed and the
			 * port passes nothing. Measured: PCRP0=0x42fc0039 on a cold flash
			 * boot (100% loss, even to the router's own LAN IP) versus
			 * 0x16942039 on a RAM boot (working) — ForceSpeed 3 vs 2.
			 * IPMSTP_PortST[22:21] is masked for the same reason. */
			v = (REG32(RTL819X_SWCORE_BASE + 0x4104)
			     & ~((3u << 19) | (3u << 21))) | 0x02940009u;
			REG32(RTL819X_SWCORE_BASE + 0x4104) = v;
			REG32(RTL819X_SWCORE_BASE + 0x4104) = v ^ (1u << 25);
			REG32(RTL819X_SWCORE_BASE + 0x4104) = v;
			/* 7. settle, then latch Conf_done LAST (loader order/
			 * timing; cold/forced path only — loader boots never
			 * reach this). Final P0GMIICR must read 0x00037d55. */
			msleep(1000);
			REG32(RTL819X_SWCORE_BASE + 0x414C) |= (1u << 6);
			pr_err("rtl819x trunk COLD replica applied\n");
		}
		/* --- A-2 residual (a2-residual): 802.3x PAUSE on the RGMII trunk ---
		 * The routed LAN->WAN flow U-turns on port0 (ingress VID2 + egress
		 * VID1 on the SAME port), so a saturating TCP cwnd burst tail-drops
		 * ~0.25% at the port0 egress queue (Mathis => the ~27 Mbit collapse)
		 * while the SHARED pool never nears runout (GDSR0 MaxUsedDsc ~30 <<
		 * S_DSC_RUNOUT=480, and ICMP -f — 1 pkt in flight — loses 0%).
		 * Internal back-pressure (SBFCR/PBFCR above, byte-identical to the
		 * shipped stock kernel) cannot throttle the EXTERNAL sender; only
		 * link-level pause can push the burst back onto the 8367S and
		 * onward into hal's deep NIC queue. Two coordinated ends:
		 *   SoC:   PCRP0[17:16] PauseFlowControl = 3 (EtxErx: force TX+RX
		 *          pause on the forced-mode trunk). Stock's trunk setup
		 *          (0x80194728-64) masks/sets bits 18-25 and PRESERVES
		 *          [17:16]; the cold replica above only ORs 0x02940009 —
		 *          so this field was never forced by either. Ability-only
		 *          MAC bits, NOT RGMII-pair timing, so writing over the
		 *          loader's live trunk is safe (same argument as the
		 *          unconditional MACCR LONG_TXE write above); latched via
		 *          the vendor EnForceMode double-toggle exactly like the
		 *          cold replica (TOGGLE_BIT_IN_REG_TWICE).
		 *   8367S: EXT1 DI-force txpause|rxpause via the surgical SMI
		 *          helper (rtl8367b.c) — the driver never runs extif-init
		 *          for the 8367S, so on loader boots these bits are
		 *          inherited unknown (the flashed-boot cold path already
		 *          forces 0x1311=0x1076, pause included).
		 * Runs UNCONDITIONALLY of the PITCR cold-gate above: the bench
		 * RAM-boots via the loader path, which skips the gated block.
		 * Idempotent (skip + no re-toggle when already in the requested
		 * state), so self-heal re-runs of rtl865x_start() don't bounce the
		 * trunk. trunk_pause: 1=enable, 0=don't touch, 2=force-clear. */
		if (trunk_pause == 1 || trunk_pause == 2) {
			u32 v = REG32(RTL819X_SWCORE_BASE + 0x4104);
			u32 want = (trunk_pause == 1) ? (v | (3u << 16))
						      : (v & ~(3u << 16));

			if (want != v) {
				REG32(RTL819X_SWCORE_BASE + 0x4104) = want;
				REG32(RTL819X_SWCORE_BASE + 0x4104) =
					want ^ (1u << 25);	/* EnForceMode latch */
				REG32(RTL819X_SWCORE_BASE + 0x4104) = want;
				pr_err("rtl819x trunk-pause: PCRP0 %08x -> %08x\n",
				       v, want);
			}
		}
		pr_err("rtl819x trunk-post: PITCR=%08x P0GMIICR=%08x MACCR=%08x PCRP0=%08x\n",
		       REG32(RTL819X_SWCORE_BASE + 0x4100),
		       REG32(RTL819X_SWCORE_BASE + 0x414C),
		       REG32(RTL819X_SWCORE_BASE + 0x4000),
		       REG32(RTL819X_SWCORE_BASE + 0x4104));

		REG32(MSCR) |= MSCR_EN_L2;		/* global L2 forwarding enable (LAST) */
	}

	/* Bring-up self-diagnosis (readable over ssh via `dmesg`): on a dead-RX
	 * boot the ring base still latches into CPURPDCR0 but CPUIISR never accrues
	 * RX_DONE — that distinguishes an engine wedge from a fabric-silent path. */
	pr_err("rtl819x bringup: CPUICR=%08x CPURPDCR0=%08x CPUIISR=%08x DMA_CR0=%08x MSCR=%08x GDSR0=%08x SBFCR0=%08x CPUICR1=%08x\n",
	       REG32(CPUICR), REG32(CPURPDCR0), REG32(CPUIISR), REG32(DMA_CR0), REG32(MSCR),
	       REG32(GDSR0), REG32(SBFCR0), REG32(CPUICR1));

	/* M7: latch the known-good fabric config ONCE, at the end of the first
	 * successful bring-up (loader groundwork + this function's programming,
	 * before gw_prog's later L3/L4 additions). The fabric full reset restores
	 * from this snapshot. */
	if (!fab_cfg_snap_valid)
		rtl819x_fabric_snapshot();
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
static u32 hang_log_n;

/*
 * M7 recovery ladder trigger. Write to /sys/module/rtl819x/parameters/
 * fabric_reset to run a recovery level on the LIVE box (the ~10ms watchdog
 * tick picks it up, clears it and schedules hang_work):
 *   1 = CPU-engine re-init only (rtl865x_down + New_swNic_init + rtl865x_start
 *       — the old M6.5 recovery; proven NOT to clear the large-frame wedge)
 *   2 = level 1 + a CPUICR SOFTRST pulse (bit22 "re-initialize all
 *       descriptors" — the NIC-block descriptor/gather engine reset the old
 *       recovery never tried; cheap, no table loss)
 *   3 = level 2 + the FULL vendor fabric reset (SIRR FULL_RST + swcore clock
 *       cycle + MEMCR table-SRAM init + config restore) — resets all switch
 *       tables & queues; hwnat is re-armed AND the gw scaffolding (netif MACs
 *       / L2 / L3 / nexthops / ACL permits) is re-programmed IN-KERNEL via
 *       rtl865x_gw_rearm(), so level 3 is fully self-sufficient (validated:
 *       without the gw re-arm, MSCR=EN_L2-only and ALL CPU-bound frames drop
 *       until a manual `cat /proc/rtl865x_gw`).
 * fabric_autoreset: ladder level the wedge detector auto-runs (default 3 =
 * self-healing full reset; 0 = detector logs only).
 */
static int fabric_reset;
module_param(fabric_reset, int, 0644);
MODULE_PARM_DESC(fabric_reset, "trigger recovery now: 1=engine 2=+SOFTRST 3=+full fabric reset");
static int fabric_autoreset = 3;
module_param(fabric_autoreset, int, 0644);
MODULE_PARM_DESC(fabric_autoreset, "wedge detector action: 0=log only, 1/2/3=auto ladder level (default 3)");

/*
 * ★ Does level-3 recovery re-arm the ASIC gw program afterwards?
 *
 * Default 1 (router): FULL_RST wipes the TLU tables, and without gw_rearm the
 * ASIC refuses even small CPU-bound frames, so a router MUST reprogram them.
 *
 * ★ Set 0 on a BRIDGE. gw_rearm() -> gw_prog() reprograms the router
 * scaffolding and re-freezes L2 aging (TEACR bit0), which on a bridged AP is
 * the permanent-blackhole condition fixed in 5ff1a1f. Measured: after a
 * level-3 recovery on a bridge, ARP stayed perfect (20/20) while ICMP sat at
 * ~1000 ms with 20% loss and ssh timed out.
 *
 * A bridge never runs gw_prog at boot (dir842-asic skips it by role) and works
 * fine, so it does not need the re-arm after a reset either -- which is what
 * lets bridge role use the full reset it actually needs to clear an RX stall,
 * instead of being capped at the level-2 soft reset that does not clear it.
 */
static int fabric_gw_rearm = 1;
module_param(fabric_gw_rearm, int, 0644);
MODULE_PARM_DESC(fabric_gw_rearm, "level-3 recovery re-arms the ASIC gw program (default 1; set 0 in bridge role)");

static int fabric_reset_mode;	/* latched ladder level for the queued hang_work */

static void rtl819x_hang_work(struct work_struct *w)
{
	struct rtl819x_eth_priv *priv =
		container_of(w, struct rtl819x_eth_priv, hang_work);
	uint32 rxcnt[NEW_NIC_MAX_RX_DESC_RING] = { 0 };
	uint32 txcnt[NEW_NIC_MAX_TX_DESC_RING] = { 0 };
	int mode = xchg(&fabric_reset_mode, 0);

	/* Spurious re-queue, or the device went down (ndo_stop) while this was
	 * queued: rtl819x_eth_stop already ran napi_disable/rtl865x_down —
	 * re-running them here would deadlock (double napi_disable). */
	if (!mode || !netif_running(priv->dev))
		return;

	rxcnt[0] = RTL819X_RX_RING_SIZE;
	txcnt[0] = RTL819X_TX_RING_SIZE;

	pr_err("rtl819x: recovery level %d starting (rx_pkts=%lu)\n",
	       mode, priv->dev->stats.rx_packets);

	/* Level 3 nukes the NAPT table: quiesce/flush the hwnat module FIRST
	 * (takes rtl865x_hal_lock itself, so call before we take it). */
	if (mode >= 3)
		rtl819x_hwnat_stop();

	/*
	 * Quiesce EVERYTHING that can touch 0xBB80xxxx/0xB801xxxx: during the
	 * level-3 clock-gate window (600ms+) any switch-core register access
	 * stalls the Lexra bus. The HAL mutex fences gw_prog//proc scanners/
	 * hwnat; timer_delete_sync stops the GDSR_PORT_CONG drain; napi_disable
	 * stops the poll (its GDSR read + ring work); netif_tx_disable
	 * synchronously fences in-flight xmit (doorbell writes) and keeps the
	 * stack off; rtl865x_down masks CPUIIMR+GIMR so the (level-triggered)
	 * switch line can't storm while the core resets. The old
	 * netif_tx_lock_bh critical section is replaced because this path now
	 * sleeps (msleep in the reset) — a BH lock can't be held across it.
	 */
	mutex_lock(&rtl865x_hal_lock);
	timer_delete_sync(&priv->rx_timer);
	napi_disable(&priv->napi);
	netif_tx_disable(priv->dev);

	rtl865x_down();
	if (mode >= 2) {
		/* NIC descriptor-engine soft reset (vendor CPUICR bit22). Pulse
		 * with TX/RX already off; ring bases are re-latched by the
		 * New_swNic_init + rtl865x_start below. */
		REG32(CPUICR) = CPUICR_SOFTRST;
		udelay(100);
		REG32(CPUICR) = 0;
	}
	if (mode >= 3)
		rtl819x_fabric_full_reset();

	New_swNic_init(rxcnt, txcnt, RTL819X_CLUSTER_SIZE);
	rtl865x_start();

	netif_wake_queue(priv->dev);
	napi_enable(&priv->napi);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_WATCHDOG_INTERVAL);
	mutex_unlock(&rtl865x_hal_lock);

	if (mode >= 3) {
		rtl819x_hwnat_start(priv->dev);
		/*
		 * M7 self-heal: FULL_RST wiped the ASIC TLU tables (netif MACs,
		 * L2/L3 routes, nexthops, ACL permits) — without them the ASIC
		 * refuses even small CPU-bound frames (validated live:
		 * MSCR=EN_L2-only + 100% loss until a manual
		 * `cat /proc/rtl865x_gw`). Re-run the very same gw program
		 * in-kernel; it takes rtl865x_hal_lock itself, so it must run
		 * after the unlock above.
		 */
		if (fabric_gw_rearm) {
			rtl865x_gw_rearm();
			pr_err("rtl819x: ASIC gw scaffolding re-armed in-kernel (netif/L2/L3/NAT tables reprogrammed)\n");
		} else {
			/* Bridge role: see fabric_gw_rearm. Re-arming here would
			 * re-freeze L2 aging and blackhole a roaming client. */
			pr_err("rtl819x: gw re-arm SKIPPED (fabric_gw_rearm=0, bridge role)\n");
		}
	}
	pr_err("rtl819x: recovery level %d complete\n", mode);
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
	 * drain -> no effect) - done every ~10ms timer tick so congestion can never
	 * accumulate to the wedge threshold. Also read it in the napi poll so it
	 * drains at napi rate exactly when a large-frame burst is arriving.
	 */
	REG32(GDSR_PORT_CONG);

	/* M7 manual recovery trigger (bench: echo N > .../parameters/fabric_reset). */
	if (unlikely(fabric_reset)) {
		fabric_reset_mode = fabric_reset;
		fabric_reset = 0;
		schedule_work(&priv->hang_work);
	}

	/*
	 * M7 wedge detector v2 — sampled-FCS (replaces the USEDDSC-floor detector,
	 * which was USELESS: live calibration showed idle USEDDSC is 18 on a FRESH
	 * boot, identical to 18-19 wedged, so it fired constantly on a healthy box).
	 *
	 * Signal: New_swNic_receive software-verifies the Ethernet FCS of every
	 * large (>132 B) delivered frame. The switch MAC discards genuinely
	 * bad-FCS frames at ingress, so a software mismatch == the delivered bytes
	 * are not the received frame — the wedge's exact symptom (live-validated:
	 * wedged ≈93% of large frames stale in DRAM, fresh = 0 failures).
	 *
	 * Self-arming: the detector only acts after one window (~2.5s, A-2) has seen >=2
	 * GOOD large frames since boot (proves the FCS-in-cluster convention holds
	 * on this datapath; DHCP DISCOVERs / SSH KEX / any large ping arm it
	 * organically). If large frames systematically fail while none pass, it
	 * logs once and stays dormant instead of reset-looping a healthy box.
	 *
	 * Declare: a window with fail>=4 and a >=4:1 fail majority (the wedge
	 * measures ~93% fail, so 4:1 has margin while a healthy mixed window can
	 * never trip it). Action: fabric_autoreset ladder level (default 3 = full
	 * self-heal incl. in-kernel gw re-arm), rate-limited to one per 5s (A-2).
	 */
	{
		static u32 fcs_prev_ok, fcs_prev_fail;
		static int fcs_win;
		static bool fcs_armed, fcs_off_logged;
		static unsigned long fcs_last_auto;

		/* A-2: 256 ticks ~= 2.5s (was 1024 ~= 10s).  Detection latency is
		 * dead air — a wedged fabric corrupts ~93% of large frames until
		 * the self-heal runs, so shrink the window: when wedged, 4 fails
		 * accrue in well under 2.5s under any traffic, while a healthy
		 * window still cannot hit the >=4-fails AND 4:1-majority gate. */
		if (++fcs_win >= 256) {		/* ~2.5s @ ~10ms ticks */
			u32 dok = rtl819x_rx_fcs_ok - fcs_prev_ok;
			u32 dfail = rtl819x_rx_fcs_fail - fcs_prev_fail;

			fcs_prev_ok = rtl819x_rx_fcs_ok;
			fcs_prev_fail = rtl819x_rx_fcs_fail;
			fcs_win = 0;

			if (!fcs_armed) {
				if (dok >= 2) {
					fcs_armed = true;
					pr_info("rtl819x: FCS wedge detector armed (%u good large frames)\n",
						dok);
				} else if (dfail >= 8 && !dok && !fcs_off_logged) {
					fcs_off_logged = true;
					pr_err("rtl819x: FCS check unusable on this datapath (%u fails, 0 ok) - wedge detector stays OFF\n",
					       dfail);
				}
			} else if (dfail >= 4 && dfail >= 4 * dok) {
				pr_err("rtl819x: FABRIC WEDGE detected (large-frame FCS fail=%u ok=%u in 10s)%s\n",
				       dfail, dok,
				       fabric_autoreset ? " - auto recovery" : " - set fabric_reset=3 to recover");
				/* !fcs_last_auto == "never fired" (INITIAL_JIFFIES on
				 * MIPS would otherwise defer the FIRST auto-reset by
				 * minutes); |1 keeps the sentinel unambiguous. */
				/* A-2: holdoff 30s -> 5s.  Under sustained routed
				 * bulk the wedge recurs ~5s after each recovery; a
				 * 30s holdoff left the box sitting WEDGED (blackholing
				 * large frames) for up to ~25s per cycle = TCP-fatal.
				 * 5s still prevents back-to-back thrash (reset ~1s). */
				if (fabric_autoreset &&
				    (!fcs_last_auto ||
				     time_after(jiffies, fcs_last_auto + 5 * HZ))) {
					fcs_last_auto = jiffies | 1;
					fabric_reset_mode = fabric_autoreset;
					schedule_work(&priv->hang_work);
				}
			}
		}
	}

	/* ★ Second detector: the RX-STALL wedge, which the FCS detector above
	 * structurally CANNOT see.
	 *
	 * That detector declares on large-frame FCS *failures* (dfail >= 4 and a
	 * 4:1 majority). This wedge stops RX completely -- rx_done=0 on every
	 * poll, rx_packets frozen -- so dok and dfail are both zero and the
	 * dfail >= 4 gate can never trip, no matter how long the box is down.
	 * That is exactly why issue #2 sat at 40% loss for 10+ minutes with the
	 * detector armed and silent.
	 *
	 * Measured here with a 200 Mbit/s UDP flood at the box (TCP never does it
	 * -- it backs off; terminated TCP ran clean to the 152 Mbit/s CPU ceiling
	 * with USEDDSC never moving off 138):
	 *
	 *   PRESSURE poll#101487 rx_done=0 rx_pkts=504576 USEDDSC=470 runout=0
	 *   PRESSURE poll#101488 rx_done=0 rx_pkts=504576 USEDDSC=470 runout=0
	 *
	 * i.e. napi still polling ~61/s, the shared pool pinned near full,
	 * DSCRUNOUT never latched, and not one frame delivered -- for 386 s,
	 * until a power cycle. The box is unreachable the whole time.
	 *
	 * Signature: pool full AND zero frames delivered for two consecutive
	 * windows. ★ The pool occupancy is what separates this from an idle box
	 * -- idle measures USEDDSC ~138-146 on this board, a wedge 450+ -- so
	 * "no RX" alone is deliberately NOT enough to trip it.
	 */
	{
		static unsigned long stall_prev_rx;
		static int stall_win, stall_hits;
		static unsigned long stall_last_auto;

		if (++stall_win >= 256) {	/* ~2.5s @ ~10ms ticks, as above */
			u32 gd = REG32(GDSR0);
			unsigned int used = (gd & GDSR0_USEDDSC_MASK) >> 16;
			unsigned long rx = priv->dev->stats.rx_packets;

			stall_win = 0;

			if (used > 256 && rx == stall_prev_rx)
				stall_hits++;
			else
				stall_hits = 0;
			stall_prev_rx = rx;

			if (stall_hits >= 2) {	/* ~5s pool-full with zero delivery */
				stall_hits = 0;
				pr_err("rtl819x: RX-STALL WEDGE detected (USEDDSC=%u, rx_packets frozen at %lu)%s\n",
				       used, rx,
				       fabric_autoreset ? " - auto recovery"
						        : " - set fabric_reset=2 to recover");
				/* Same holdoff convention as the FCS path above:
				 * !stall_last_auto means "never fired" (INITIAL_JIFFIES
				 * on MIPS would otherwise defer the first one), |1 keeps
				 * the sentinel unambiguous. */
				if (fabric_autoreset &&
				    (!stall_last_auto ||
				     time_after(jiffies, stall_last_auto + 5 * HZ))) {
					stall_last_auto = jiffies | 1;
					fabric_reset_mode = fabric_autoreset;
					schedule_work(&priv->hang_work);
				}
			}
		}
	}

	if (++tick % 10)		/* the wedge detector below runs every 10th tick (~100ms) */
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
		/*
		 * M6.6: this "RXptr frozen + TXptr moving" gate false-positives in the
		 * gateway role — the box legitimately TXs (DHCP, ARP to unresolved peers,
		 * routed-path pings) with RX idle, indistinguishable from a real wedge, and
		 * used to fire a full rtl865x_start() re-init MID-TX that corrupted the
		 * egress datapath on every control-plane burst. The PRIMARY wedge fix is the
		 * GDSR_PORT_CONG drain above (runs unconditionally every tick); the re-init
		 * was only a best-effort net for a sustained max-rate flood we don't hit in
		 * the gateway path. Log (rate-limited); do NOT re-init on this signature.
		 */
		if (!(hang_log_n++ & 0xf))
			pr_warn("rtl819x: RXptr frozen %08x while TX active (rxpkts=%lu) - not re-initing (gateway TX-without-RX is normal)\n",
				rxd, priv->dev->stats.rx_packets);
	}
}

/* M6.3: slow missed-IRQ watchdog — the RX interrupt now drives napi; this only
 * nudges napi ~10x/s so a lost/masked interrupt can never wedge RX for long.
 * M6.5: also the cadence for the fabric-wedge detector. */
static void rtl819x_rx_timer(struct timer_list *t)
{
	struct rtl819x_eth_priv *priv = timer_container_of(priv, t, rx_timer);

	napi_schedule(&priv->napi);
	rtl819x_hang_check(priv);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_WATCHDOG_INTERVAL);
}

/*
 * DSA conduit tag shim.
 *
 * The switch and this MAC exchange Realtek's 4-byte CPU tag, which the MAC
 * inserts and strips in hardware: the frames this driver sees carry no tag at
 * all, and the source jack arrives out of band in the receive descriptor.
 * Hardware NAT depends on that arrangement, because the ASIC has to classify
 * by ingress jack, so it stays exactly as it is.
 *
 * The DSA core and the rtl8_4 tagger, however, speak the 8-byte on-wire form.
 * Rather than fork the tagger, translate at this boundary: synthesise the
 * 8-byte tag on receive from the descriptor's port field, and consume it on
 * transmit into the descriptor's port mask. Everything above this line is then
 * stock DSA.
 */
#define RTL819X_DSA_TAG_LEN		8
/* protocol id the rtl8_4 tagger expects for this switch family */
#define RTL819X_DSA_PROTO_RTL8365MB	0x04
/* highest switch port that can appear in the descriptor's source field */
#define RTL819X_DSA_MAX_PORT		6

/*
 * Egress port mask for frames the CPU originates. The tagger names a single
 * port, but it is not yet established that this MAC honours a narrow mask in
 * CPU-tag mode -- the older driver always flooded and noted that narrowing had
 * no effect. 1 = flood every jack (proven), 0 = use the port the tagger asked
 * for. Settable at runtime so both can be measured on one boot.
 */
static int dsa_tx_flood = 1;
module_param(dsa_tx_flood, int, 0644);
MODULE_PARM_DESC(dsa_tx_flood, "CPU-originated frames flood all jacks (1) or go only to the port DSA named (0)");

static bool rtl819x_dsa_tag_rx(struct sk_buff *skb, unsigned int port)
{
	__be16 *tag;

	if (unlikely(port > RTL819X_DSA_MAX_PORT))
		return false;

	if (unlikely(skb_headroom(skb) < RTL819X_DSA_TAG_LEN) &&
	    pskb_expand_head(skb, RTL819X_DSA_TAG_LEN, 0, GFP_ATOMIC))
		return false;

	/* [DA][SA][payload] -> [DA][SA][tag][payload] */
	skb_push(skb, RTL819X_DSA_TAG_LEN);
	memmove(skb->data, skb->data + RTL819X_DSA_TAG_LEN, 2 * ETH_ALEN);

	tag = (__be16 *)(skb->data + 2 * ETH_ALEN);
	tag[0] = htons(ETH_P_REALTEK);
	/* protocol, and REASON 0 so the tagger marks the frame as forwarded */
	tag[1] = htons(RTL819X_DSA_PROTO_RTL8365MB << 8);
	tag[2] = 0;
	tag[3] = htons(port & 0xf);

	return true;
}

/*
 * Strip the tag the tagger wrote and return the egress port mask it asked for,
 * or a negative value if this frame is not tagged (something sent straight
 * through the conduit rather than a DSA user port).
 */
static int rtl819x_dsa_tag_tx(struct sk_buff *skb)
{
	__be16 *tag;

	if (unlikely(skb->len < ETH_HLEN + RTL819X_DSA_TAG_LEN))
		return -EINVAL;

	tag = (__be16 *)(skb->data + 2 * ETH_ALEN);
	if (unlikely(tag[0] != htons(ETH_P_REALTEK)))
		return -EINVAL;
	if (unlikely((ntohs(tag[1]) >> 8) != RTL819X_DSA_PROTO_RTL8365MB))
		return -EINVAL;

	if (unlikely(skb_cow_head(skb, 0)))
		return -ENOMEM;

	/* re-read: skb_cow_head() may have moved the data */
	tag = (__be16 *)(skb->data + 2 * ETH_ALEN);
	{
		int mask = ntohs(tag[3]) & 0x7ff;

		memmove(skb->data + RTL819X_DSA_TAG_LEN, skb->data, 2 * ETH_ALEN);
		skb_pull(skb, RTL819X_DSA_TAG_LEN);

		return mask;
	}
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

		if (netdev_uses_dsa(dev)) {
			/*
			 * Under DSA the jack identity comes from the receive
			 * descriptor and is handed to the tagger as an 8-byte
			 * header; no VLAN demux happens here at all.
			 */
			if (unlikely(!rtl819x_dsa_tag_rx(skb, info.pid))) {
				dev->stats.rx_dropped++;
				dev_kfree_skb_any(skb);
				continue;
			}

			skb->protocol = eth_type_trans(skb, dev);
			napi_gro_receive(napi, skb);

			dev->stats.rx_packets++;
			dev->stats.rx_bytes += info.len;
			rx_done++;
			continue;
		}

		skb->protocol = eth_type_trans(skb, dev);
		/*
		 * M6.6 cascade: frames arrive UNTAGGED (8367S untags the trunk) with
		 * the source jack in info.pid (CPU-tag). Derive the VID from the jack
		 * (jacks 0-3 = LAN vid2, jack4 = WAN vid1) and re-attach it as a
		 * hwaccel ctag so 8021q demuxes eth0.2 (LAN) / eth0.1 (WAN). (M6.2
		 * used info.vid directly, but the cascade gives info.vid=0.)
		 */
		{
			/* M6.6 Fork A two-VLAN demux: frames arrive from the trunk inline-
			 * 802.1Q tagged with their REAL vid (rtl865x_start makes CPU egress
			 * tagged). Move the tag into the hwaccel slot so 8021q demuxes by the
			 * real vid -> eth0.2 (LAN vid2) / eth0.1 (WAN vid1). (The old one-armed
			 * code force-normalized everything to vid2, which mis-delivered WAN
			 * frames to eth0.2 and broke two-VLAN software routing.) Untagged
			 * frames default to the LAN vid. */
			if (skb->protocol == htons(ETH_P_8021Q)) {
				skb = skb_vlan_untag(skb);	/* -> hwaccel tag = real vid */
				if (!skb) { dev->stats.rx_dropped++; continue; }
			} else {
				__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), 2);
			}
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
		 * clears that frame's RX_DONE, so its IRQ is lost until the ~10ms
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
		unsigned int used = (gd & GDSR0_USEDDSC_MASK) >> 16;
		bool runout = !!(gd & GDSR0_DSCRUNOUT);
		bool pressure = runout || used > 256;
		bool beat;

		/* ★ Increment UNCONDITIONALLY. This used to sit in the third operand
		 * of a ||, so short-circuit evaluation skipped it the moment
		 * `pressure` was true -- i.e. poll# froze exactly when the box was in
		 * trouble. A real wedge then logged the same "poll#20497" 2419 times,
		 * which reads as "the napi loop has hung" when in fact only the
		 * counter had stopped. It cost real diagnostic time; don't put side
		 * effects back into this condition. */
		beat = !(++pc & 0x3ff);

		/* Descriptor-pool watch: log if the shared pool is filling (USEDDSC)
		 * or has latched a run-out (DSCRUNOUT) - the large-frame drop
		 * signature - as well as the periodic liveness heartbeat. */
		if (pressure)
			/* ★ Rate-limited, and it must stay that way. `pressure` is true
			 * on EVERY poll while the pool is full, and printk to a serial
			 * console is synchronous. Unthrottled this measured 37.7 lines/s
			 * = ~3772 B/s against a 3840 B/s (38400 8N1) console: 98% of the
			 * line, with every napi poll blocking on the UART. The diagnostic
			 * became a denial-of-service precisely when the box was already
			 * under pressure, turning a degraded box into an unreachable one.
			 * See the USEDDSC=451 episode in docs/ and issue #2. */
			pr_err_ratelimited("rtl819x DP: PRESSURE poll#%lu rx_done=%d rx_pkts=%lu CPUIISR=%08x USEDDSC=%u runout=%d\n",
					   pc, rx_done, dev->stats.rx_packets,
					   REG32(CPUIISR), used, runout);
		else if (beat)
			pr_err("rtl819x DP: poll#%lu rx_done=%d rx_pkts=%lu CPUIISR=%08x USEDDSC=%u runout=%d\n",
			       pc, rx_done, dev->stats.rx_packets, REG32(CPUIISR),
			       used, runout);
	}

	return rx_done;
}

static netdev_tx_t rtl819x_eth_xmit(struct sk_buff *skb, struct net_device *dev)
{
	rtl_nicTx_info nicTx;
	dma_addr_t dma;
	unsigned int len;
	int dsa_ports = -1;

	if (netdev_uses_dsa(dev)) {
		dsa_ports = rtl819x_dsa_tag_tx(skb);
		if (unlikely(dsa_ports < 0)) {
			dev_kfree_skb_any(skb);
			dev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
	}

	len = skb->len;

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
	/*
	 * Under DSA the tagger already named the egress jack; otherwise fall back
	 * to flooding the physical ports (bit 6, the CPU port, stays clear so a
	 * frame cannot loop back).
	 */
	nicTx.portlist = (dsa_ports >= 0 && !dsa_tx_flood) ? (dsa_ports & 0x3F)
							  : 0x3F;
	/*
	 * VID from the netdev's hwaccel VLAN tag — eth0.2 (LAN) -> VID 2,
	 * eth0.1 (WAN) -> VID 1 (the M6.6 Fork A VLAN plan); an untagged frame
	 * (bare eth0) defaults to the LAN VID. The trunk ports egress tagged
	 * (rtl865x_start) so the external RTL8367S routes each frame to the
	 * correct jack by VID.
	 * Bug #13 requirement: ph_vlanId must name a VID whose SoC VLAN member
	 * mask covers the 0x3F portlist — only VID 2 (LAN) and VID 1 (WAN) are
	 * installed (sw_add_vlan, members 0x7F, trunk egress TAGGED), and the
	 * external RTL8367S only carries those two across the trunk. The old
	 * fallback, the stale M6.2 vid 9, has had NO VLAN table entry since
	 * Fork A, so untagged bare-eth0 TX rode a VID with a 0/garbage member
	 * mask and an unknown tag downstream.
	 */
	if (dsa_ports >= 0)
		/* jack 4 is the WAN zone, jacks 0-3 the LAN zone */
		nicTx.vid = (dsa_ports & BIT(4)) ? RTL865X_VID_WAN : RTL865X_VID_LAN;
	else if (skb_vlan_tag_present(skb))
		nicTx.vid = skb_vlan_tag_get(skb) & 0xfff;
	else
		nicTx.vid = RTL865X_VID_LAN;
	nicTx.flags = 0;	/* direct-TX: swnic adds PKTHDR_USED_8197F|PKT_OUTGOING */

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
	/* rtl865x_start() drives the TLU command interface (VLAN/PVID writes) and RMWs
	 * MSCR — hold the HAL lock so it can't interleave with a concurrent gw_prog
	 * (rc.local backgrounds `cat /proc/rtl865x_gw` ~6 s into boot, racing netifd's
	 * ifup on slow boots) or the /proc scanners. An interleave could commit a
	 * mixed table entry or lose gw_prog's MSCR EN_L3|EN_L4 bits (silent NAT death). */
	mutex_lock(&rtl865x_hal_lock);
	rtl865x_start();
	mutex_unlock(&rtl865x_hal_lock);

	/* M6.3: the RX IRQ now drives napi; keep the timer only as a slow (~100ms)
	 * missed-IRQ watchdog so a lost interrupt can't wedge RX. M6.5: the timer
	 * also runs the fabric-wedge detector, which kicks this work to recover. */
	INIT_WORK(&priv->hang_work, rtl819x_hang_work);
	timer_setup(&priv->rx_timer, rtl819x_rx_timer, 0);
	mod_timer(&priv->rx_timer, jiffies + RTL819X_WATCHDOG_INTERVAL);

	netif_start_queue(dev);

	/* M6.6 Phase 3: arm conntrack HW-NAT offload now the datapath is up (no-op
	 * unless rtl819x.hwnat=1). The static ASIC gateway scaffolding is programmed
	 * separately by rc.local's `cat /proc/rtl865x_gw`. */
	rtl819x_hwnat_start(dev);

	netdev_info(dev, "interface up (polling, irq %d unused)\n", priv->irq);
	return 0;
}

static int rtl819x_eth_stop(struct net_device *dev)
{
	struct rtl819x_eth_priv *priv = netdev_priv(dev);

	netif_stop_queue(dev);
	/* M6.6 Phase 3: tear down all HW-NAT rows + quiesce the aging worker while the
	 * switch core is still up (before rtl865x_down()); no-op unless hwnat=1. */
	rtl819x_hwnat_stop();
	/* M7: cancel the recovery work BEFORE the timer — hang_work re-arms the
	 * timer when it finishes, so the old order (timer first) could leave a
	 * live timer behind. A work queued after this point no-ops on
	 * !netif_running(). */
	cancel_work_sync(&priv->hang_work);
	timer_delete_sync(&priv->rx_timer);
	napi_disable(&priv->napi);
	rtl865x_down();
	free_irq(priv->irq, dev);
	New_swNic_freeRings();
	return 0;
}

/* M6.6 Phase 3: the two conntrack HW-NAT offload hooks are always present. They gate
 * internally on the runtime-writable rtl819x.hwnat param and decline every flow to
 * the software path while it is off, so hwnat=0 behaves identically to a driver
 * without the hooks — but the param can be flipped on at runtime with no reboot. */
static const struct net_device_ops rtl819x_eth_netdev_ops = {
	.ndo_open		= rtl819x_eth_open,
	.ndo_stop		= rtl819x_eth_stop,
	.ndo_start_xmit		= rtl819x_eth_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	/*
	 * Hardware NAT attaches through ndo_setup_tc(TC_SETUP_FT) now; the
	 * downstream ndo_flow_offload interface this port used to hook no longer
	 * exists. See rtl819x_hwnat.c.
	 */
};

static int rtl819x_eth_probe(struct platform_device *pdev)
{
	struct net_device *dev;
	struct rtl819x_eth_priv *priv;
	struct resource *res;
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
	/*
	 * Room for the DSA tag on top of a normal frame. Without it the core
	 * cannot raise the conduit to 1508, and every user port is then capped
	 * below 1500 and refuses the standard MTU.
	 */
	dev->max_mtu = ETH_DATA_LEN + RTL819X_DSA_TAG_LEN;
	netif_napi_add(dev, &priv->napi, rtl819x_eth_poll);

	/* MAC address: DT if present, else random (userspace sets the real one). */
	if (of_get_ethdev_address(pdev->dev.of_node, dev))
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

static void rtl819x_eth_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct rtl819x_eth_priv *priv = netdev_priv(dev);

	unregister_netdev(dev);
	netif_napi_del(&priv->napi);
	New_swNic_setDev(NULL);
	free_netdev(dev);
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
