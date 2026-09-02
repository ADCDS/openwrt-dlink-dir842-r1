/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RTL819x (RTL8197F) system controller register window.
 *
 * The block sits at physical 0x18000000 and is reachable through KSEG1 without
 * a mapping, which matters because several users run before ioremap() is
 * usable (the board file's early init) or from vendor code that dereferences
 * KSEG1 addresses directly.
 */
#ifndef __ASM_MACH_RTL819X_SYSC_H
#define __ASM_MACH_RTL819X_SYSC_H

#include <linux/bits.h>
#include <linux/io.h>
#include <asm/addrspace.h>

#define REALTEK_SYSC_BASE		0x18000000

/* System registers, offsets from REALTEK_SYSC_BASE */
#define REALTEK_SR_REG_ID		0x00
#define REALTEK_SR_REG_BOOTSTRAP	0x08
#define REALTEK_SR_CLKMANAGE		0x10
#define REALTEK_SR_PCIE_PHY0		0x50
#define REALTEK_SR_PCIE_PHY1		0x54
/*
 * On-SoC 2.4 GHz WMAC enable gate. Bit 0 gates ALL WMAC register access; the
 * vendor driver ORs 0x1f in probe and writes 0 on close. Without it every WMAC
 * read returns garbage. (Decoded from the stock kernel: the only two writers.)
 */
#define REALTEK_SR_WLAN_EN		0x64
#define REALTEK_SR_WLAN_EN_ALL		0x1f
/* PCIe PHY digital-reset release -- NOT PCIE_PHY0; see pci-realtek.c */
#define REALTEK_SR_MDIORST		0x100

/* Crystal is 40 MHz when set, 25 MHz when clear (DIR-842 R1: clear) */
#define REALTEK_SR_BS_40MHZ		BIT(24)

/* Switch/NIC core clock enable. The D-Link loader only sets this while it runs
 * its own network init, so a flash boot (no TFTP) can leave the whole switch
 * register block un-clocked. Forced on by the board file.
 */
#define REALTEK_SR_CLKMANAGE_SWCORE	BIT(11)

/* Writing 0 here makes the watchdog reset the SoC immediately */
#define REALTEK_SR_WDTCNR		0x311c

static inline void __iomem *rtl819x_sysc(void)
{
	return (void __iomem *)KSEG1ADDR(REALTEK_SYSC_BASE);
}

static inline u32 sr_r32(unsigned int reg)
{
	return __raw_readl(rtl819x_sysc() + reg);
}

static inline void sr_w32(u32 val, unsigned int reg)
{
	__raw_writel(val, rtl819x_sysc() + reg);
}

#endif /* __ASM_MACH_RTL819X_SYSC_H */
