// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTL819x (RTL8197F) board support for the MIPS generic platform.
 *
 * The SoC needs three things done before the kernel proper can run, none of
 * which any upstream driver covers:
 *
 *  - early console output, because UART0 is a Synopsys DW-APB block whose
 *    RBR/THR sit at register index 9 (byte offset 0x24) rather than 0;
 *  - the switch/NIC core clock forced on, because the D-Link bootloader only
 *    enables it while running its own network stack, so a boot from flash can
 *    otherwise leave the entire switch register block un-clocked;
 *  - the CPU's SI_TimerInt routed back in through the SoC interrupt
 *    controller's second bank, which is where this SoC wires it.
 *
 * Ported from the 4.14 fork's arch/mips/realtek/{prom,setup,irq}.c.
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/libfdt.h>
#include <linux/pm.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/types.h>

#include <asm/addrspace.h>
#include <asm/fw/fw.h>
#include <asm/machine.h>
#include <asm/reboot.h>
#include <asm/setup.h>

#include <mach-rtl819x/rtl819x-sysc.h>

/*
 * UART0 for early printk. Deliberately open-coded rather than using
 * setup_8250_early_printk_port(): that helper assumes THR at offset 0, and
 * getting a character out has to work before 8250_dw and its quirk are in play.
 */
#define RTL819X_UART0_BASE	0xb8147000
#define RTL819X_UART0_FCR	((void __iomem *)(RTL819X_UART0_BASE + 0x008))
#define RTL819X_UART0_LSR	((void __iomem *)(RTL819X_UART0_BASE + 0x014))
#define RTL819X_UART0_THR	((void __iomem *)(RTL819X_UART0_BASE + 0x024))

#define UART_FCR_TXRST		0x04
#define UART_LSR_THRE_BIT	0x20
#define UART_TX_BUSY_LIMIT	30000

/*
 * SoC interrupt controller, second bank. The first bank is driven by
 * drivers/irqchip/irq-realtek-rtl.c from DT; bank 2 carries exactly one line
 * we care about (the CPU timer) and no driver touches it.
 */
#define RTL819X_INTC_BASE	0xb8003000
#define RTL819X_INTC_GIMR2	((void __iomem *)(RTL819X_INTC_BASE + 0x20))
#define RTL819X_INTC_IRR5	((void __iomem *)(RTL819X_INTC_BASE + 0x2c))

/* bank-2 input 15 is the 24K core's SI_TimerInt */
#define RTL819X_INTC2_TIMER	15
/* IRR5 covers bank-2 inputs 8..15, 4 bits each; input 15 lands in bits 31..28 */
#define RTL819X_IRR5_TIMER_IP7	(7u << 28)
/* MIPS IP7, where the 24K reports its own compare interrupt */

void prom_putchar(char c)
{
	unsigned int busy = 0;

	while ((__raw_readb(RTL819X_UART0_LSR) & UART_LSR_THRE_BIT) == 0) {
		if (++busy >= UART_TX_BUSY_LIMIT) {
			/* wedged: reset the TX FIFO and drop the character */
			__raw_writeb(UART_FCR_TXRST | 0xc0, RTL819X_UART0_FCR);
			return;
		}
	}

	__raw_writeb(c, RTL819X_UART0_THR);
}

static void rtl819x_machine_restart(char *command)
{
	local_irq_disable();
	sr_w32(0, REALTEK_SR_WDTCNR);
	while (1)
		cpu_relax();
}

static void rtl819x_machine_halt(void)
{
	local_irq_disable();
	while (1)
		cpu_relax();
}

static void __init rtl819x_setup_timer_irq(void)
{
	/*
	 * The core's timer interrupt is fed through the SoC interrupt
	 * controller's second bank and routed back out to MIPS IP7, which is
	 * where cp0_compare_irq expects it. Nothing else in bank 2 is used, so
	 * program it wholesale.
	 */
	__raw_writel(RTL819X_IRR5_TIMER_IP7, RTL819X_INTC_IRR5);
	__raw_writel(BIT(RTL819X_INTC2_TIMER), RTL819X_INTC_GIMR2);
}

static void __init rtl819x_soc_init(void)
{
	u32 clk;

	pr_info("RTL819x: ID %08x BOOTSTRAP %08x CLKMANAGE %08x\n",
		sr_r32(REALTEK_SR_REG_ID), sr_r32(REALTEK_SR_REG_BOOTSTRAP),
		sr_r32(REALTEK_SR_CLKMANAGE));

	clk = sr_r32(REALTEK_SR_CLKMANAGE) | REALTEK_SR_CLKMANAGE_SWCORE;
	sr_w32(clk, REALTEK_SR_CLKMANAGE);
	pr_info("RTL819x: switch core clock forced on -> %08x\n",
		sr_r32(REALTEK_SR_CLKMANAGE));

	rtl819x_setup_timer_irq();
}

static __init const void *rtl819x_fixup_fdt(const void *fdt,
					    const void *match_data)
{
	if (fdt_check_header(fdt))
		panic("Corrupt DT");

	/*
	 * The bootloader hands over registers that are not a UHI boot protocol
	 * argument set; make sure fw_init_cmdline() cannot walk a wild pointer.
	 */
	if (fw_arg2 >= CKSEG2)
		fw_arg2 = 0;

	fw_init_cmdline();

	_machine_restart = rtl819x_machine_restart;
	_machine_halt = rtl819x_machine_halt;
	pm_power_off = rtl819x_machine_halt;

	rtl819x_soc_init();

	return fdt;
}

static const struct of_device_id rtl819x_of_match[] __initconst = {
	{ .compatible = "realtek,rtl8197f-soc" },
	{}
};

MIPS_MACHINE(rtl819x) = {
	.matches = rtl819x_of_match,
	.fixup_fdt = rtl819x_fixup_fdt,
};
