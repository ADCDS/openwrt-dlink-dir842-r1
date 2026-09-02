/*
 *  Realtek RLX based SoC PCI host controller driver
 *
 *  Copyright (C) 2019 Gaspare Bruno <gaspare@anlix.io>
 *  Copyright (C) 2017 Weijie Gao <hackpascal@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#include <asm/mach-realtek/realtek_mem.h>

struct realtek_pci_controller {
	void __iomem *rc_cfg_base;
	void __iomem *rc_ext_base;
	void __iomem *dev_cfg0_base;
	void __iomem *dev_cfg1_base;

	int link_up;

	u32 bus_number;
	struct device_node *np;
	struct pci_controller pci_controller;
	struct resource io_res;
	struct resource mem_res;
	struct clk *clk;
};

static inline struct realtek_pci_controller *
pci_bus_to_realtek_pci_controller(struct pci_bus *bus)
{
	struct pci_controller *hose;

	hose = (struct pci_controller *) bus->sysdata;
	return container_of(hose, struct realtek_pci_controller, pci_controller);
}

static inline void realtek_pcie_mdio_write(struct realtek_pci_controller *rpc, u32 reg, u32 data) 
{
	u32 val;

	val = ((reg&0x1f)<<8) | ((data&0xffff)<<16) | BIT(0);
	__raw_writel(val, rpc->rc_ext_base);
	mdelay(2);
}

static inline
#if defined(CONFIG_CPU_RLX)
__attribute__ ((section(".iram")))
#endif
int realtek_pci_raw_read(void __iomem *mem, int where, int size, uint32_t *value){
	u32 data;
	int s;

	data = __raw_readl(mem + (where & ~3));
	switch (size) {
		case 1:
		case 2:
			s = ((where & 3) * 8);
			data >>= s;
			data &= (size==1?0xff:0xffff);
			break;
		case 4:
			break;
		default:
			return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	if (value)
		*value = data;

	return PCIBIOS_SUCCESSFUL;
}

static 
#if defined(CONFIG_CPU_RLX)
__attribute__ ((section(".iram")))
#endif
int realtek_pci_read(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, uint32_t *value)
{
	struct realtek_pci_controller *rpc;

	rpc = pci_bus_to_realtek_pci_controller(bus);
	if (!rpc->link_up)
		return PCIBIOS_DEVICE_NOT_FOUND;

	if(bus && rpc->bus_number == 0xff)
		rpc->bus_number = bus->number;

	if(bus->number == rpc->bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			if (value)
				realtek_pci_raw_read(rpc->rc_cfg_base, where, size, value);
		}
		else return PCIBIOS_DEVICE_NOT_FOUND;
	}
	else
	if(bus->number == rpc->bus_number+1) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0){
			if(value)
				realtek_pci_raw_read(rpc->dev_cfg0_base + (PCI_FUNC(devfn) << 12), where, size, value);
		}
		else return PCIBIOS_DEVICE_NOT_FOUND;
	}
	else {
		/* Devices connected through bridge (Max 4 devices in SDK)*/
		if (PCI_SLOT(devfn) < 4){
			// (0xc = PCIE0 IPCFG)
			__raw_writel(((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn), rpc->rc_ext_base+0x0c);
			if(value)
				realtek_pci_raw_read(rpc->dev_cfg1_base, where, size, value);
		}	
	}

	return PCIBIOS_SUCCESSFUL;
}

static inline 
#if defined(CONFIG_CPU_RLX)
__attribute__ ((section(".iram")))
#endif
int realtek_pci_raw_write(void __iomem *mem, int where, int size, uint32_t value){
	u32 data;
	int s,v;

	switch (size) {
		case 1:
		case 2:
			data = __raw_readl(mem + (where & ~3));
			s = ((where & 3) * 8);
			v = ~(size==1?0xff:0xffff) << s;
			data = (data & v) | (value << s);
			break;
		case 4:
			data = value;
			break;
		default:
			return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	__raw_writel(data, mem + (where & ~3));

	return PCIBIOS_SUCCESSFUL;
}

static 
#if defined(CONFIG_CPU_RLX)
__attribute__ ((section(".iram")))
#endif
int realtek_pci_write(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, uint32_t value)
{
	struct realtek_pci_controller *rpc;

	rpc = pci_bus_to_realtek_pci_controller(bus);
	if (!rpc->link_up)
		return PCIBIOS_DEVICE_NOT_FOUND;

	if(bus && rpc->bus_number == 0xff)
		rpc->bus_number = bus->number;

	if(bus->number == rpc->bus_number) {
		/* PCIE host controller */
		if (PCI_SLOT(devfn) == 0) {
			realtek_pci_raw_write(rpc->rc_cfg_base, where, size, value);
		}
		else return PCIBIOS_DEVICE_NOT_FOUND;
	}
	else
	if(bus->number == rpc->bus_number+1) {
		/* PCIE devices directly connected */
		if (PCI_SLOT(devfn) == 0){
			realtek_pci_raw_write(rpc->dev_cfg0_base + (PCI_FUNC(devfn) << 12), where, size, value);
		}
		else return PCIBIOS_DEVICE_NOT_FOUND;
	}
	else {
		/* Devices connected through bridge (Max 4 devices in SDK)*/
		if (PCI_SLOT(devfn) < 4){
			// (0xc = PCIE0 IPCFG)
			__raw_writel(((bus->number) << 8) | (PCI_SLOT(devfn) << 3) | PCI_FUNC(devfn), rpc->rc_ext_base+0x0c);
			realtek_pci_raw_write(rpc->dev_cfg1_base, where, size, value);
		}	
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops realtek_pci_ops = {
	.read	= realtek_pci_read,
	.write	= realtek_pci_write,
};


static int realtek_pcie_check_link(struct realtek_pci_controller *rpc)
{
	int i = 20;
	u32 val = 0;

	do
	{
		val = __raw_readl(rpc->rc_cfg_base + 0x728);
		if((val & 0x1f) == 0x11)
			return 1;

		mdelay(100);
	} while (i--);

	return 0;
}

static inline void realtek_pcie_device_reset(struct realtek_pci_controller *rpc)
{
	u32 val;

	/*
	 * Vendor "device reset": pulse SR PCIE_PHY0 (0xb8000050) bit1 - assert,
	 * hold ~300 ms, deassert. The old CLKMANAGE bit26 toggle here was a
	 * no-op for the PHY and never brought the RTL8822BE endpoint out of
	 * reset (LTSSM stuck at 0x02). This is the vendor stock-kernel behaviour.
	 */
	val = sr_r32(REALTEK_SR_PCIE_PHY0);
	sr_w32(val & ~BIT(1), REALTEK_SR_PCIE_PHY0);
	mdelay(300);
	val = sr_r32(REALTEK_SR_PCIE_PHY0);
	sr_w32(val | BIT(1), REALTEK_SR_PCIE_PHY0);
}

static inline void realtek_pcie_mdio_reset(struct realtek_pci_controller *rpc)
{
	/*
	 * Release the PCIe PHY digital reset. THE key fix: the stock D-Link
	 * kernel writes 8/9/0xb to SR+0x100 (0xb8000100) - the only code in the
	 * whole vendor kernel that touches that register - NOT PCIE_PHY0 (0x50)
	 * as the old mainline code did. Without it the endpoint PHY stays held
	 * in reset and the link never leaves LTSSM polling.
	 */
	sr_w32(0x8, REALTEK_SR_MDIORST);
	sr_w32(0x9, REALTEK_SR_MDIORST);
	sr_w32(0xb, REALTEK_SR_MDIORST);
}

static inline void realtek_pcie_phy_reset(struct realtek_pci_controller *rpc)
{
	//(0x8 = PCIE0 PWRCR)
	__raw_writel(0x01, rpc->rc_ext_base+0x8);	//bit7:PHY reset=0   bit0: Enable LTSSM=1
	__raw_writel(0x81, rpc->rc_ext_base+0x8);	//bit7:PHY reset=1   bit0: Enable LTSSM=1
}

/*
 * PCIe RC bring-up mirroring the stock D-Link kernel's pcie_init (reverse-
 * engineered from the vendor vmlinux). This trains the RTL8822BE link on the
 * DIR-842 (RTL8197F) where the previous hackpascal/SDK-derived sequence did
 * not - verified on hardware via a live insmod harness before baking in here.
 * Crystal (40 vs 25 MHz) is read from the bootstrap register, not the DTS clk.
 */
static void realtek_pcie_reset(struct realtek_pci_controller *rpc)
{
	u32 val;
	int is40 = !!(sr_r32(REALTEK_SR_REG_BOOTSTRAP) & REALTEK_SR_BS_40MHZ);

	/* clock enable: vendor sets only bits 12,13,18 then 14 (NOT 19,20,26) */
	val = sr_r32(REALTEK_SR_CLKMANAGE);
	val |= BIT(12)|BIT(13)|BIT(18);
	sr_w32(val, REALTEK_SR_CLKMANAGE);
	val = sr_r32(REALTEK_SR_CLKMANAGE);
	val |= BIT(14);
	sr_w32(val, REALTEK_SR_CLKMANAGE);
	mdelay(10);

	/* PHY digital-reset release (SR+0x100) - the mainline-missing step */
	realtek_pcie_mdio_reset(rpc);
	mdelay(10);

	realtek_pcie_phy_reset(rpc);	/* PWRCR pulse #1 */
	mdelay(10);

	/* crystal-tuning MDIO (vendor values; short table, not the old 20-write one) */
	if (is40) {
		realtek_pcie_mdio_write(rpc, 0x0f, 0x12f6);
		realtek_pcie_mdio_write(rpc, 0x00, 0x0071);
		realtek_pcie_mdio_write(rpc, 0x06, 0x1ac1);
	} else {
		realtek_pcie_mdio_write(rpc, 0x00, 0x0071);
		realtek_pcie_mdio_write(rpc, 0x06, 0x18c1);
	}
	mdelay(10);

	realtek_pcie_phy_reset(rpc);	/* PWRCR pulse #2 */

	realtek_pcie_device_reset(rpc);	/* SR PCIE_PHY0 bit1 clear/300ms/set */
	mdelay(10);
}

static void load_ranges(struct pci_controller *hose, struct device_node *node)
{
	struct of_pci_range range;
	struct of_pci_range_parser parser;

	hose->of_node = node;

	if (of_pci_range_parser_init(&parser, node))
		return;

	for_each_of_pci_range(&parser, &range) {
		switch (range.flags & IORESOURCE_TYPE_BITS) {
		case IORESOURCE_IO:
			hose->io_map_base =
				(unsigned long)ioremap(range.cpu_addr, range.size);
			hose->io_resource->flags = range.flags;
			hose->io_resource->name = node->full_name;
			hose->io_resource->start = range.cpu_addr;
			hose->io_resource->end = range.cpu_addr + range.size - 1;
			break;
		case IORESOURCE_MEM:
			hose->mem_resource->flags = range.flags;
			hose->mem_resource->name = node->full_name;
			hose->mem_resource->start = range.cpu_addr;
			hose->mem_resource->end = range.cpu_addr + range.size - 1;
			break;
		}
	}
}

static int realtek_pci_probe(struct platform_device *pdev)
{
	struct realtek_pci_controller *rpc;
	struct resource *res;
	int id;
	u32 val;
	u16 cmd;
	u8 v8;

	id = pdev->id;
	if (id == -1)
		id = 0;

	rpc = devm_kzalloc(&pdev->dev, sizeof(struct realtek_pci_controller),
			    GFP_KERNEL);
	if (!rpc)
		return -ENOMEM;
	rpc->bus_number=0xff;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_cfg_base");
	rpc->rc_cfg_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->rc_cfg_base))
		return PTR_ERR(rpc->rc_cfg_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rc_ext_base");
	rpc->rc_ext_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->rc_ext_base))
		return PTR_ERR(rpc->rc_ext_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dev_cfg0_base");
	rpc->dev_cfg0_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->dev_cfg0_base))
		return PTR_ERR(rpc->dev_cfg0_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dev_cfg1_base");
	rpc->dev_cfg1_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rpc->dev_cfg1_base))
		return PTR_ERR(rpc->dev_cfg1_base);

	rpc->clk = devm_clk_get(&pdev->dev, NULL);
	if(!rpc->clk)
		return PTR_ERR(rpc->clk);

	iomem_resource.start = 0;
	iomem_resource.end = ~0;
	ioport_resource.start = 0;
	ioport_resource.end = ~0;

	rpc->np = pdev->dev.of_node;
	rpc->pci_controller.pci_ops = &realtek_pci_ops;
	rpc->pci_controller.io_resource = &rpc->io_res;
	rpc->pci_controller.mem_resource = &rpc->mem_res;
	load_ranges(&rpc->pci_controller, pdev->dev.of_node);

	realtek_pcie_reset(rpc);

	rpc->link_up = realtek_pcie_check_link(rpc);
	/*
	 * Diagnostic: dump the raw LTSSM/link register (rc_cfg + 0x728; low 5
	 * bits == 0x11 means L0 = trained) and the ref clock the PHY was tuned
	 * for. If the link is still down after the 25 MHz clock fix, this line
	 * shows WHICH state it's stuck in (0x0d detect / 0x1x config...) and
	 * confirms clk=25000000, without needing devmem in userspace.
	 */
	dev_info(&pdev->dev, "PCIe RC%d link check: LTSSM(0x728)=0x%08x clk=%luHz link_up=%d\n",
		 id, __raw_readl(rpc->rc_cfg_base + 0x728),
		 clk_get_rate(rpc->clk), rpc->link_up);
	if (!rpc->link_up) {
		/*
		 * Link never reached L0 (LTSSM != 0x11). Do NOT fall through to
		 * the dev_cfg0_base config read below: a config-space access to a
		 * downstream device that isn't there gets no PCIe completion and
		 * HARD-HANGS the CPU bus (no exception, no timeout, no recovery -
		 * observed on DIR-842 as a dead lock right after this warn). Bail
		 * cleanly instead so the kernel keeps booting AND the other root
		 * complex (pcie1@18b20000) still gets a chance to probe - the
		 * RTL8822BE may actually sit on that one.
		 */
		dev_warn(&pdev->dev, "PCIe link is down - skipping bus (no device)\n");
		return 0;
	}

	cmd = __raw_readw(rpc->rc_cfg_base + PCI_COMMAND);
	cmd = PCI_COMMAND_MASTER | PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
	__raw_writew(cmd, rpc->rc_cfg_base + PCI_COMMAND);

	// Set MAX_PAYLOAD_SIZE to 128B,default
	v8 = __raw_readb(rpc->dev_cfg0_base + 0x78);
	v8 &= ~(PCI_EXP_DEVCTL_PAYLOAD);
	__raw_writeb(v8, rpc->rc_cfg_base + 0x78);

	mdelay(100);

	register_pci_controller(&rpc->pci_controller);

	return 0;
}

int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct realtek_pci_controller *rpc;
	u16 cmd;
	int irq = 5;

	rpc = pci_bus_to_realtek_pci_controller(dev->bus);
	if(!rpc)
		return 0;

	//TODO: Implement second pcie (get irq from dt)
	// Bus:1 Slot:0 Pin:1 -> first wireless card

	/* setup the slot */
	cmd = PCI_COMMAND_MASTER | PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
	pci_write_config_word(dev, PCI_COMMAND, cmd);
	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);

	return irq;
}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}

static const struct of_device_id realtek_pci_ids[] = {
	{ .compatible = "realtek,rtl8196b-pci" },
	{},
};

static struct platform_driver realtek_pci_driver = {
	.probe = realtek_pci_probe,
	.driver = {
		.name = "realtek-pci",
		.of_match_table = of_match_ptr(realtek_pci_ids),
	},
};

static int __init realtek_pci_init(void)
{
	return platform_driver_register(&realtek_pci_driver);
}

postcore_initcall(realtek_pci_init);
