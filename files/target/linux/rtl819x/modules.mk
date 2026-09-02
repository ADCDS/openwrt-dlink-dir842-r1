# SPDX-License-Identifier: GPL-2.0-only
#
# Vendor rtl8192cd driver for the RTL8197F on-SoC 2.4 GHz WMAC (WEXT, in-kernel PSK).
# Lands in the last port milestone; until then CONFIG_RTL8192CD stays unset and this
# package is simply not selected. Loaded late (60) so rtw88 has claimed PCI 10ec:b822.

define KernelPackage/rtl8192cd
  SUBMENU:=Wireless Drivers
  TITLE:=Realtek RTL8197F on-SoC 2.4 GHz WMAC (vendor driver)
  DEPENDS:=@TARGET_rtl819x
  KCONFIG:=CONFIG_RTL8192CD \
	CONFIG_WIRELESS_EXT=y \
	CONFIG_WEXT_PRIV=y
  FILES:=$(LINUX_DIR)/drivers/net/wireless/rtl8192cd/rtl8192cd.ko
  AUTOLOAD:=$(call AutoLoad,60,rtl8192cd)
endef

define KernelPackage/rtl8192cd/description
 Vendor Realtek rtl8192cd driver scoped to the RTL8197F's integrated 2.4 GHz
 MAC/PHY (the 5 GHz RTL8822BE is driven by mainline rtw88). WEXT-based; the
 netifd handler lives in lib/netifd/wireless/rtl8192cd.sh.
endef

$(eval $(call KernelPackage,rtl8192cd))
