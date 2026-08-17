// SPDX-License-Identifier: GPL-2.0
/*
 * 8192cd_owrt_bsp.c — R4/G3 Path B: the two Realtek-BSP entry points this vendor
 * driver expects the *platform* to export, supplied here because this is mainline
 * OpenWrt and not the Realtek rtl819x BSP.
 *
 * Both were modpost undefineds:
 *     ERROR: "rtl819x_bond_option"     [rtl8192cd.ko] undefined!
 *     ERROR: "PCIE_reset_procedure_97F" [rtl8192cd.ko] undefined!
 *
 * In the vendor SDK they live in arch/mips/rtl8197f/{gpio.c,pci.c} and are
 * EXPORT_SYMBOL'd. This tree's arch code has neither, so they are provided here,
 * scoped to this module.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <bspchip.h>

/*
 * rtl819x_bond_option() — read the SoC bonding strap and classify the package.
 *
 * Transcribed from the vendor BSP (sdk-rtl819x arch/mips/rtl8197f/gpio.c:356),
 * byte for byte including the "no match -> 0" behaviour.
 *
 * This is LOAD-BEARING for RF: 8192cd_hw.c feeds the result to
 * ODM_CmnInfoInit(ODM_CMNINFO_PACKAGE_TYPE, ...) — 97FN->2, 97FS->1, 97FB->0,
 * anything else->1 — and package type selects which PHY/RF register set phydm
 * programs. Getting it wrong picks a different-but-well-formed register set,
 * which is the hardest kind of failure to spot afterwards.
 *
 * ★ Independently corroborated on THIS silicon: the mainline rtl8197f-wmac probe
 * driver reads SR+0x0c[3:0] and printed 10 (0xa) => 97FS => package_type 1, which
 * matches the RTL8197F*S* marking on the DIR-842 R1 board.
 */
unsigned int rtl819x_bond_option(void)
{
	unsigned int type, ret = 0;

	type = __raw_readl((void __iomem *)BSP_BOND_OPTION) & 0xf;

	switch (type) {
	case 0x0:			/* 97FB */
		ret = BSP_BOND_97FB;
		break;
	case 0x4:			/* 97FN */
	case 0x5:
	case 0x6:
		ret = BSP_BOND_97FN;
		break;
	case 0xa:			/* 97FS  <- the DIR-842 R1 reads 0xa here */
	case 0xb:
	case 0xc:
		ret = BSP_BOND_97FS;
		break;
	}

	return ret;
}

/*
 * PCIE_reset_procedure_97F() — DELIBERATELY NOT the vendor implementation.
 *
 * ★★ The vendor version (arch/mips/rtl8197f/pci.c:206) does a full PCIe bring-up:
 * it re-clocks the PCIe IP, does an MDIO reset, resets the PCIe PHY twice, drives
 * PERST# low for 300 ms, and then polls for link-up. On THIS box the PCIe bus
 * carries the RTL8822BE that mainline rtw88 already owns and that serves the
 * working 5 GHz AP. Running that sequence from this driver would drop the link and
 * take rtw88's device out from under it — the single most destructive thing this
 * port could do.
 *
 * So this is a READ-ONLY probe: it reports the current link state and never writes
 * anything. Returning 1 means "link is up", which is exactly what the one live
 * caller wants to hear.
 *
 * The only live call site is 8192cd_hw.c (~14728), inside
 *   #if defined(CONFIG_WLAN_HAL_8197F) ... if (efuse_virtual_data.special) ...
 *      if ((REG32(0xb8b00728) & 0x1f) != 0x11)      // link NOT up
 *              if (PCIE_reset_procedure_97F(0,1) != 1) return -1;
 * i.e. it is only reached when the link is already down. With rtw88 up the link IS
 * 0x11, so that branch is not taken and this function is not called at all. The
 * other textual call site (8192cd_osdep.c ~12533) is preprocessed out here: it
 * needs !defined(CONFIG_NET_PCI), and 8192cd_cfg.h force-defines CONFIG_NET_PCI
 * under NOT_RTK_BSP. Verified with nm — only 8192cd_hw.o references the symbol.
 *
 * If a future milestone genuinely needs the PCIe controller reset, port the vendor
 * routine THEN, and only with rtw88 unbound first.
 */
int PCIE_reset_procedure_97F(unsigned int PCIeIdx, unsigned int mdioReset)
{
	unsigned int dbg;

	dbg = __raw_readl((void __iomem *)(BSP_PCIE_RC_CFG + 0x728));

	pr_warn("rtl8192cd: PCIE_reset_procedure_97F(%u,%u) called — refusing to reset "
		"PCIe (mainline rtw88 owns the RTL8822BE on this bus). "
		"Link state 0x%08x -> reporting %s\n",
		PCIeIdx, mdioReset, dbg,
		((dbg & 0x1f) == 0x11) ? "UP" : "DOWN");

	return ((dbg & 0x1f) == 0x11) ? 1 : 0;
}
