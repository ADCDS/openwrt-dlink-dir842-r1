# SPDX-License-Identifier: GPL-2.0
#
# R4/G3: package the vendor rtl8192cd driver for the RTL8197F on-SoC 2.4 GHz WMAC.
#
# Built from target/linux/realtek/files-4.14/drivers/net/wireless/rtl8192cd/ (vendor
# source, 8devices v3.4.11e, ported to 4.14 — see docs/VENDOR-PARITY-INVENTORY.md and
# g3-rtl8192cd-4.14-port.patch in the mirror repo).
#
# ★ This driver owns ONLY the on-SoC 2.4 GHz radio. Its 8822BE support is compiled out
# so it cannot claim PCI 10ec:b822 — mainline rtw88 keeps the 5 GHz card. Both radios
# live simultaneously (hardware precondition verified on silicon).
#
# Auto-loaded at boot (AUTOLOAD below). It is loaded LATE (priority 60) so mainline
# rtw88 has already claimed PCI 10ec:b822 for the 5 GHz card; this driver only drives
# the on-SoC 2.4 GHz WMAC, so the two never contend.

define KernelPackage/rtl8192cd
  SUBMENU:=Wireless Drivers
  TITLE:=Realtek RTL8197F on-SoC 2.4 GHz WMAC (vendor driver)
  DEPENDS:=@TARGET_realtek
  KCONFIG:=CONFIG_RTL8192CD
  FILES:=$(LINUX_DIR)/drivers/net/wireless/rtl8192cd/rtl8192cd.ko
  AUTOLOAD:=$(call AutoLoad,60,rtl8192cd)
endef

define KernelPackage/rtl8192cd/description
 Vendor Realtek rtl8192cd driver, scoped to the RTL8197F's integrated 2.4 GHz
 MAC/PHY. Mainline has no driver for this radio, which is why the port was
 otherwise single-band. ~1.9 MB stripped.

 Proven on hardware: brings up wlan0 (+ 4 VAPs and the vxd interface) and the
 2.4 GHz AP associates. Coexists with rtw88 on 5 GHz.
endef

$(eval $(call KernelPackage,rtl8192cd))
