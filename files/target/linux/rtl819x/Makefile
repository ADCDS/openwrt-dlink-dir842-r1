# SPDX-License-Identifier: GPL-2.0-only
#
# Realtek RTL819x home-router SoCs (RTL8197F). Not to be confused with
# target/linux/realtek, which is the RTL838x/RTL930x managed-switch family.

include $(TOPDIR)/rules.mk

ARCH:=mipsel
BOARD:=rtl819x
BOARDNAME:=Realtek RTL819x (RTL8197F)
SUBTARGETS:=rtl8197f
FEATURES:=squashfs ramdisk gpio pci small_flash
DEVICE_TYPE:=router

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Realtek RTL8197F based routers
	(D-Link DIR-842 R1).
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += kmod-gpio-button-hotplug kmod-leds-gpio ethtool

# The vendor rtl8192cd 2.4 GHz driver tree is redistributed in files-6.18/ (see the
# README's licensing note). If a checkout ever lacks it, stub its Kconfig/Makefile so
# the unconditional `source` added by patches-6.18/041 does not break configuration.
define Kernel/Prepare
	$(call Kernel/Prepare/Default)
	if [ ! -e $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Kconfig ]; then \
		mkdir -p $(LINUX_DIR)/drivers/net/wireless/rtl8192cd; \
		printf '# stub: vendor rtl8192cd source not present\n' > $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Kconfig; \
		printf '# stub: vendor rtl8192cd source not present\n' > $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Makefile; \
	fi
endef

$(eval $(call BuildTarget))
