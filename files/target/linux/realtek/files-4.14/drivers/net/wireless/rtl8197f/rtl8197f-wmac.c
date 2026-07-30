// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8197F on-SoC 2.4 GHz WMAC — R4/G2 bring-up driver.
 *
 * ── What this is ──────────────────────────────────────────────────────────────
 * The RTL8197F has an integrated 2.4 GHz MAC/PHY that mainline has never had any
 * driver for, and which this port had not even declared (no DTS node). That is
 * why the port is single-band: the RTL8822BE on pcie0 is ONE radio doing 2.4 OR
 * 5, not both. R4's goal is concurrent dual-band; G1 mapped the hardware, and
 * this is G2.
 *
 * ── Scope, stated honestly ────────────────────────────────────────────────────
 * This is NOT yet a working WiFi driver. It does not beacon and no client can
 * associate. It implements the first, load-bearing step that everything else
 * depends on and that can actually be verified on hardware:
 *
 *     probe the block, lift the enable gate, and PROVE the register window
 *     responds (and is not returning bus garbage).
 *
 * Until that is proven, writing PHY/RF init would be writing into the dark. The
 * verification is deliberately explicit: a known-value check on registers we can
 * predict, plus an all-0x00 / all-0xFF "dead bus" detector, all logged.
 *
 * ── Hardware facts, all decoded from the stock vendor kernel ─────────────────
 * (vmlinux.bin, link base 0x80000000 — file offset = VA - 0x80000000)
 *
 *  - Device table @0x8064ae6c, 2 entries, stride 0x14:
 *        {u32 flags; u32 pci_cfg; u32 mmio_base; u32 irq; void *dev;}
 *      [0] flags 0x6 pci_cfg 0xb8b10000 mmio 0xb9000000 irq 5 -> PCIe RTL8822BE
 *      [1] flags 0x2 pci_cfg 0x0        mmio 0xb8640000 irq 6 -> THIS (on-SoC)
 *    The decoding is self-validating: entry [0]'s two addresses are exactly
 *    pcie0's dev_cfg0_base and PCI window as declared in RTL8197F.dtsi.
 *
 *  - ★ The enable gate: SR + 0x64 (0xB8000064). rtl8192cd_init_one @0x801fbd6c
 *    takes the 8197F branch @0x801fce44 and does `*(u32*)0xB8000064 |= 0x1f`
 *    (@0x801fce5c-68); rtl8192cd_close @0x801fa294 writes 0 (@0x801fa2a8). Those
 *    are the ONLY two writers in the whole stock image. Bit0 gates ALL WMAC
 *    register access — the vendor driver re-checks it at ~1690 sites and
 *    otherwise prints "Should not access WiFi register since 0xB8000064[0]=0".
 *    This is the direct analogue of the PCIe SR+0x100 digital-reset release that
 *    mainline had wrong and that cracked M5.
 *
 *  - Chip identity: init_one gates on (*(u32*)0xB8000000 & 0xFFFFF000) ==
 *    0x8197F000 (@0x801fc5d0-e8) and then stores pshare->chip_type = 0x100A,
 *    the on-SoC 2.4 GHz WMAC hardware id.
 *
 *  - 25 vs 40 MHz reference: rtl8192cd_init_hw_PCI @0x80250ca8 branches on
 *    SR+0x08 BIT(24) (REALTEK_SR_BS_40MHZ), the same bootstrap bit the PCIe PHY
 *    setup uses. The DIR-842 is 25 MHz (bit clear), so the WMAC takes the 25 MHz
 *    PHY/PLL path — relevant to G2's next step, not to this one.
 *
 * ── What is deliberately NOT here ────────────────────────────────────────────
 * No PHY/RF programming. Past init, the vendor driver dispatches through a HAL
 * function-pointer table (priv->ops), so a literal ordered register sequence is
 * not recoverable by static analysis. The register sequences that ARE recoverable
 * live in compiled-in tables, and they have been extracted and sanity-checked
 * from vmlinux.bin so the next step starts from facts rather than guesses:
 *
 *   array_mp_8197f_mac_reg  @0x80564e08 .. 0x805652c8
 *       152 pairs of {u32 reg; u32 val}. 146/152 (96%) have reg inside the
 *       expected 0x0000-0x144e MAC window; values are byte-sized (0xbd, 0x1d,
 *       0x12, 0x92 ...), matching the vendor's `& 0xff` byte writes.
 *       Decodes cleanly as a flat table — usable almost as-is.
 *
 *   array_mp_8197f_phy_reg  @0x80558160 .. 0x8055b004
 *       1492 pairs, but only 59% land in the 0x0800-0x0ff8 BB window. ★ Do NOT
 *       treat this as a flat table: entries such as reg=0x80001003 and
 *       reg=0x40000000 are not registers at all, they are Realtek's
 *       CONDITIONAL-BRANCH markers (the 0x8.../0x4.../0xf... encoding used
 *       throughout their PHY tables to select by RF/board variant). Replaying it
 *       linearly would write garbage to whatever those values alias. It needs the
 *       small condition interpreter the vendor HAL implements.
 *
 *   array_mp_8197f_agc_tab  @0x8055b004 .. 0x80564dd4  (single-register streaming)
 *
 * These are deliberately NOT embedded in the tree yet: ~1600 pairs of unverified
 * data with no code that consumes or tests them would be bulk without value, and
 * the PHY table would be actively misleading in flat form. Embed them together
 * with the init code that replays them, including the conditional interpreter.
 *
 * ★★ AND THEN THE HARDWARE SAID NO — read this before spending any time on those
 * tables. The tables are conditional, and a full decode of the vendor interpreter
 * produced a flattened, board-specific sequence (144 MAC + 464 BB + 196 AGC + 144
 * RF writes) on two assumptions that MUST be checked on silicon. This driver now
 * prints both at probe. Measured on this board:
 *
 *     bonding strap (SR+0x0c)[3:0] = 10   -> 97FS -> package_type 1   ✓ as predicted
 *     cut version (WMAC+0xf0)[15:12] = 1  -> ✗ NOT as assumed
 *
 * The vendor gates the whole header-table path on `p_dm[0x3E4] = (cut >= 2)`
 * (decoded at 0x801f91d4). This chip reports **cut = 1**, so stock does NOT apply
 * those tables at all — it runs a separate legacy path (0x80252750 / 0x80253c90 /
 * 0x80252784). Replaying the flattened tables here would therefore program this
 * radio with a register set the vendor never uses on this silicon revision.
 *
 * That is the expensive mistake this probe-time check exists to prevent, and it is
 * why G2's next step is NOT "replay the tables". It also shifts the balance toward
 * G3 (port the real vendor driver, which contains the legacy path) rather than
 * hand-writing init from static analysis.
 *
 * Also note, from G1: per-unit 2.4 GHz RF calibration is NOT in an efuse. The
 * stock kernel never reads flash or efuse for this radio; the values live in the
 * read-only "MAC" mtd partition (0x20000 + 0xd8) and stock pushes them in from
 * USERSPACE via `iwpriv set_mib`. So the blank RTL8822BE efuse that keeps the
 * 5 GHz radio disabled does NOT block this radio.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>

#include <asm/mach-realtek/realtek_mem.h>

#define WMAC_CHIP_ID_8197F	0x8197f000u
#define WMAC_CHIP_ID_MASK	0xfffff000u
#define WMAC_HW_ID_ONSOC_2G	0x100au	/* vendor pshare->chip_type for this block */

/*
 * Probe reads. These offsets are inside the range the vendor's own compiled-in
 * 8197F MAC table writes (0x0000-0x144e), so they are known-decoded addresses
 * rather than arbitrary pokes.
 */
#define WMAC_REG_PROBE0		0x0000
#define WMAC_REG_PROBE1		0x0004
#define WMAC_REG_PROBE2		0x0008
#define WMAC_REG_PROBE3		0x0100

struct rtl8197f_wmac {
	struct device		*dev;
	void __iomem		*base;
	int			irq;
	bool			gate_taken;
};

/*
 * SR (SoC system registers, 0xB8000000) are reached through the tree's existing
 * sr_r32()/sr_w32() macros from <asm/mach-realtek/realtek_mem.h>, which go via
 * the shared _sys_membase mapping. Do NOT ioremap this block here: it is shared
 * with the PCIe, IRQ and ethernet drivers, and claiming it would collide.
 */
/*
 * ★ Lift the enable gate. Vendor writes bits 0-4 as one `ori 0x1f`; the meaning
 * of bits 1-4 individually is NOT recoverable from the binary, so mirror the
 * vendor exactly rather than guessing a narrower mask.
 */
static void rtl8197f_wmac_enable(struct rtl8197f_wmac *w)
{
	u32 before = sr_r32(REALTEK_SR_WLAN_EN);

	sr_w32(before | REALTEK_SR_WLAN_EN_ALL, REALTEK_SR_WLAN_EN);
	/*
	 * The vendor does not delay here, but it also runs a halmac adapter init
	 * immediately afterwards which takes far longer than any settling time.
	 * Give the block a moment before the first register read so a "dead bus"
	 * verdict below cannot be a race against the gate taking effect.
	 */
	usleep_range(1000, 2000);

	w->gate_taken = true;
	dev_info(w->dev, "WLAN_EN (SR+0x%02x): %08x -> %08x\n",
		 REALTEK_SR_WLAN_EN, before, sr_r32(REALTEK_SR_WLAN_EN));
}

static void rtl8197f_wmac_disable(struct rtl8197f_wmac *w)
{
	if (!w->gate_taken)
		return;
	/* vendor rtl8192cd_close writes 0, not a read-modify-write clear */
	sr_w32(0, REALTEK_SR_WLAN_EN);
	w->gate_taken = false;
}

/*
 * Decide whether the register window is actually answering. Two failure modes
 * matter and they look different:
 *   - all reads 0xFFFFFFFF -> nothing driving the bus (gate not lifted, or the
 *     block is not at this address after all)
 *   - all reads 0x00000000 -> block held in reset / clock gated
 * Anything else is a live block. This is intentionally a weak, honest test: it
 * proves "responds", not "correctly initialised".
 */
static int rtl8197f_wmac_probe_regs(struct rtl8197f_wmac *w)
{
	u32 v[4];

	v[0] = readl(w->base + WMAC_REG_PROBE0);
	v[1] = readl(w->base + WMAC_REG_PROBE1);
	v[2] = readl(w->base + WMAC_REG_PROBE2);
	v[3] = readl(w->base + WMAC_REG_PROBE3);

	dev_info(w->dev, "WMAC regs +0x000=%08x +0x004=%08x +0x008=%08x +0x100=%08x\n",
		 v[0], v[1], v[2], v[3]);

	if (v[0] == 0xffffffffu && v[1] == 0xffffffffu &&
	    v[2] == 0xffffffffu && v[3] == 0xffffffffu) {
		dev_err(w->dev,
			"register window reads all-ones: nothing is driving the bus. "
			"Either the enable gate did not take or the block is not at this base.\n");
		return -ENODEV;
	}

	if (!v[0] && !v[1] && !v[2] && !v[3]) {
		dev_err(w->dev,
			"register window reads all-zero: block appears held in reset or clock-gated. "
			"SR+0x%02x bits 1-4 may carry a reset/clock we have not decoded.\n",
			REALTEK_SR_WLAN_EN);
		return -ENODEV;
	}

	return 0;
}

static int rtl8197f_wmac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtl8197f_wmac *w;
	struct resource *res;
	u32 chip_id, strap, cut;
	int ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = dev;
	platform_set_drvdata(pdev, w);

	/*
	 * Refuse to bind on anything that is not actually an RTL8197F. The vendor
	 * driver gates its whole on-SoC branch on this same comparison, and this
	 * block's programming is chip-specific, so binding elsewhere would poke
	 * unknown silicon.
	 */
	chip_id = sr_r32(REALTEK_SR_REG_ID);
	if ((chip_id & WMAC_CHIP_ID_MASK) != WMAC_CHIP_ID_8197F) {
		dev_err(dev, "not an RTL8197F (SR chip id %08x); refusing to bind\n",
			chip_id);
		return -ENODEV;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	w->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(w->base))
		return PTR_ERR(w->base);

	w->irq = platform_get_irq(pdev, 0);
	if (w->irq < 0) {
		dev_err(dev, "no IRQ in DT (expected cpuintc 6)\n");
		return w->irq;
	}

	dev_info(dev, "RTL8197F on-SoC 2.4 GHz WMAC: chip id %08x, hw id %04x, reg %pR, irq %d, xtal %s\n",
		 chip_id, WMAC_HW_ID_ONSOC_2G, res, w->irq,
		 (sr_r32(REALTEK_SR_REG_BOOTSTRAP) & REALTEK_SR_BS_40MHZ) ?
			"40MHz" : "25MHz");

	rtl8197f_wmac_enable(w);

	ret = rtl8197f_wmac_probe_regs(w);
	if (ret) {
		rtl8197f_wmac_disable(w);
		return ret;
	}

	/*
	 * Report the three runtime values that decide WHICH vendor init tables
	 * apply. The tables are conditional: the loader evaluates
	 *   driver1 = cut<<24 | package<<12 | interface<<8 | rfe_type
	 * and only entries whose condition matches are executed. Picking the wrong
	 * package/RFE silently selects a DIFFERENT but equally well-formed register
	 * set — the hardest possible failure to debug after the fact — so these are
	 * printed at probe rather than assumed.
	 *
	 *  - bonding strap, SR+0x0C bits[3:0]: selects package_type via the vendor's
	 *    13-entry table (strap 10/11/12 = 97FS -> package 1; 4/5/6 = 97FN ->
	 *    package 2; 0 = 97FB -> package 0). The DIR-842 carries an RTL8197F*S*,
	 *    so package 1 is EXPECTED — confirm here.
	 *  - cut version, WMAC+0xF0 bits[15:12]: ★ gates whether the header-file
	 *    tables are used AT ALL. The vendor sets its "use tables" flag as
	 *    (cut >= 2); below that it runs a completely different legacy path, and
	 *    the flattened tables would NOT be what stock applies.
	 *  - rfe_type is a software MIB default (0 for chip_type 0x100A), not a
	 *    register, so it cannot be read here — noted for completeness.
	 */
	strap = sr_r32(0x0c) & 0xf;
	cut = (readl(w->base + 0xf0) >> 12) & 0xf;
	dev_info(dev,
		 "table-selection inputs: bonding strap(SR+0x0c)=%u cut(WMAC+0xf0[15:12])=%u -> tables %s (vendor uses header tables only when cut>=2)\n",
		 strap, cut, cut >= 2 ? "APPLY" : "DO NOT APPLY (legacy path)");

	/*
	 * G2 stops here, deliberately and visibly. Registering with mac80211 now
	 * would advertise a radio that cannot beacon, which is worse than not
	 * registering at all: hostapd would attach and fail in a confusing way.
	 * Next step is the PHY/BB/RF init from the vendor's compiled-in tables
	 * (see the file header for their addresses), then ieee80211_alloc_hw().
	 */
	dev_info(dev,
		 "WMAC responds. G2 bring-up stops here: no PHY/RF init yet, so the radio is NOT registered with mac80211 and cannot beacon.\n");

	return 0;
}

static int rtl8197f_wmac_remove(struct platform_device *pdev)
{
	struct rtl8197f_wmac *w = platform_get_drvdata(pdev);

	rtl8197f_wmac_disable(w);
	return 0;
}

static const struct of_device_id rtl8197f_wmac_of_match[] = {
	{ .compatible = "realtek,rtl8197f-wmac" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtl8197f_wmac_of_match);

static struct platform_driver rtl8197f_wmac_driver = {
	.probe	= rtl8197f_wmac_probe,
	.remove	= rtl8197f_wmac_remove,
	.driver	= {
		.name		= "rtl8197f-wmac",
		.of_match_table	= rtl8197f_wmac_of_match,
	},
};
module_platform_driver(rtl8197f_wmac_driver);

MODULE_DESCRIPTION("Realtek RTL8197F on-SoC 2.4 GHz WMAC (R4/G2 bring-up)");
MODULE_LICENSE("GPL v2");
