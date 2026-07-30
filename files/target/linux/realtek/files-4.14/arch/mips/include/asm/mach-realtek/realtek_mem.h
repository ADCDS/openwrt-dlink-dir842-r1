
#ifndef __REALTEK_MEMMAP__
#define __REALTEK_MEMMAP__

extern __iomem void *_sys_membase;

// System registers
#define REALTEK_SR_REG_ID			0x00
#define REALTEK_SR_REG_BOOTSTRAP	0x08
#define REALTEK_SR_CLKMANAGE		0x10
#define REALTEK_SR_PCIE_PHY0		0x50
#define REALTEK_SR_PCIE_PHY1		0x54
#define REALTEK_SR_MDIORST			0x100 // PCIe PHY digital-reset release (8/9/0xb)
/*
 * R4/G1: on-SoC 2.4 GHz WMAC enable gate. This is the WMAC analogue of
 * REALTEK_SR_MDIORST above — the register mainline never knew about, without
 * which the block simply does not respond.
 *
 * Decoded from the stock vendor kernel (vmlinux.bin, link base 0x80000000):
 * rtl8192cd_init_one @0x801fbd6c takes the 8197F branch at 0x801fce44 and does
 *     *(volatile u32 *)0xB8000064 |= 0x1f;      (0x801fce5c-68)
 * and rtl8192cd_close @0x801fa294 reverses it with
 *     *(u32 *)0xB8000064 = 0;                   (0x801fa2a8)
 * Those are the ONLY two writers of this register in the whole stock image.
 *
 * Bit 0 gates ALL WMAC register access: the driver re-checks it at ~1690 sites
 * and otherwise prints "Should not access WiFi register since 0xB8000064[0]=0".
 * So this must be set before the first read of the WMAC reg window or every
 * access returns garbage / hangs. Stock writes bits 0-4 as one `ori 0x1f`; the
 * individual meaning of bits 1-4 is NOT recoverable from the binary, so mirror
 * the vendor and write all five.
 */
#define REALTEK_SR_WLAN_EN			0x64
#define REALTEK_SR_WLAN_EN_ALL			0x1f
#define REALTEK_SR_BS_40MHZ			BIT(24) // Crystal clock at 40Mhz

#define sr_w32(val, reg) __raw_writel(val, _sys_membase + reg)
#define sr_r32(reg)      __raw_readl(_sys_membase + reg)

#endif