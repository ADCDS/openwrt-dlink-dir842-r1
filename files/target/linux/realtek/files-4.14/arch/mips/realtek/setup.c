#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/bootmem.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>

#include <asm/reboot.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/setup.h>
#include <asm/bootinfo.h>
#include <asm/addrspace.h>
#include <asm/idle.h>

#include <linux/clk-provider.h>
#include <linux/clocksource.h>

#include <asm/mach-realtek/realtek_mem.h>

#define REALTEK_WATCHDOG_TIMER_REG	0x311C

const char *get_system_type(void)
{
#ifdef CONFIG_SOC_RTL8196E
	return "Realtek RTL8196E";
#endif

#ifdef CONFIG_SOC_RTL8197D
	return "Realtek RTL8197D";
#endif

#ifdef CONFIG_SOC_RTL8197F
	return "Realtek RTL8197F";
#endif
}

static inline void wait_instruction(void)
{
#ifndef CONFIG_SOC_RTL8197F
	__asm__(
	"       .set    push            \n"
	"       sleep                   \n"
	"       .set    pop             \n");
#endif
}

void realtek_machine_restart(char *command)
{
	/* Disable all interrupts */
	local_irq_disable();

	/* Use watchdog to reset the system */
	sr_w32(0x00, REALTEK_WATCHDOG_TIMER_REG);

	for (;;)
		wait_instruction();
}

void realtek_wait(void)
{
	if (!need_resched())
		wait_instruction();
	local_irq_enable();
}

void realtek_halt(void)
{
	while (1)
		wait_instruction();
}

void __init plat_mem_setup(void)
{
	void *dtb = NULL;

	_machine_restart = realtek_machine_restart;
	_machine_halt = realtek_halt;

#ifndef CONFIG_SOC_RTL8197F
	// 8197F uses the r4k wait
	cpu_wait = realtek_wait;
#endif

	// Initialize DTB
	if (fw_passed_dtb)
		dtb = (void *)fw_passed_dtb;
	else if (__dtb_start != __dtb_end)
		dtb = (void *)__dtb_start;

	__dt_setup_arch(dtb);
}

__iomem void *_sys_membase;

void __init device_tree_init(void)
{
	struct device_node *np;
	struct resource res;

	unflatten_and_copy_device_tree();

	np = of_find_compatible_node(NULL, NULL, "realtek,rtl819x-sysc");
	if (!np)
		panic("Failed to find realtek,rtl819x-sysc node");

	if (of_address_to_resource(np, 0, &res))
		panic("Failed to get resource for realtek,rtl819x-sysc");

	_sys_membase = ioremap_nocache(res.start, resource_size(&res));
	if(!_sys_membase)
		panic("Failed to map memory for rtl819x-sysc");

	pr_info("BOOTSTRAP = %x %x %x %x\n", sr_r32(0x00), sr_r32(0x04), sr_r32(0x08), sr_r32(0x10));

	/*
	 * Force the switch/NIC-core clock ON (CLKMANAGE bit 11). The D-Link
	 * v3.4.11B loader only enables it during its network/TFTP phase, so a
	 * FLASHED boot (no TFTP) intermittently leaves the entire switch register
	 * block at 0xBB800000 un-clocked: it reads all-zero, so rtl819x-eth's
	 * ndo_open config writes are no-ops and the box hangs the instant the
	 * bridge forwards traffic into the unconfigured RX ring (the "mode-2"
	 * freeze right at 'br-lan: entered forwarding state'). Empirically proven
	 * on this board: CLKMANAGE 0x...800 => switch clocked (MSCR reads 1, boots
	 * cleanly); 0x...000 => un-clocked (MSCR reads 0, hangs). ~80% of flashed
	 * boots landed in the un-clocked state. Idempotent on TFTP/RAM boots where
	 * the loader already set the bit.
	 */
	sr_w32(sr_r32(REALTEK_SR_CLKMANAGE) | (1u << 11), REALTEK_SR_CLKMANAGE);
	pr_info("CLKMANAGE forced switch-clock on -> %x\n", sr_r32(REALTEK_SR_CLKMANAGE));

#ifdef CONFIG_SOC_RTL8197D
	/* Voodoo from SDK */
	if((sr_r32(0x00)&0xf)<3)
	{
		sr_w32((sr_r32(0x88) & ( ~(3<<5)&~(0xF<<0))), 0x88);
		sr_w32((sr_r32(0x88)|(1<<4)), 0x88);
		sr_w32(sr_r32(0x88) & (~(3<<7)), 0x88);
	}
#endif
}

void __init plat_time_init(void)
{
	of_clk_init(NULL);
	timer_probe();
}

