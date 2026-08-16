#
# Copyright (C) 2008-2011 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk

ARCH:=mips
BOARD:=realtek
BOARDNAME:=Realtek
SUBTARGETS:=rtl8197d rtl8196e rtl8197f
FEATURES:=squashfs gpio pci
MAINTAINER:=Anlix <contato@anlix.io>

KERNEL_PATCHVER:=4.14

define Target/Description
	Build firmware images for Realtek based boards.
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += \
        kmod-gpio-button-hotplug swconfig

# The vendor rtl8192cd 2.4 GHz driver tree is NOT redistributable and exists only in
# private checkouts (gitignored even there — see the mirror repo's README, "2.4 GHz").
# Patch 0007 unconditionally `source`s its Kconfig, so a tree WITHOUT the driver dies
# at silentoldconfig ("can't open file drivers/net/wireless/rtl8192cd/Kconfig" —
# caught by the 2026-08-16 clean-room build). Stub the directory when it is absent:
# an empty Kconfig satisfies the `source`, silentoldconfig then DROPS the now-unknown
# CONFIG_RTL8192CD=m from config-4.14, obj-$(CONFIG_RTL8192CD) stays empty, and the
# build proceeds wired + 5 GHz only. A checkout that HAS the real driver is untouched
# (files-4.14 is copied before this check runs, so the stub is never created there).
define Kernel/Prepare
	$(call Kernel/Prepare/Default)
	if [ ! -e $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Kconfig ]; then \
		mkdir -p $(LINUX_DIR)/drivers/net/wireless/rtl8192cd; \
		printf '# stub: vendor rtl8192cd driver not present in this checkout\n' \
			> $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Kconfig; \
		printf '# stub: vendor rtl8192cd driver not present in this checkout\n' \
			> $(LINUX_DIR)/drivers/net/wireless/rtl8192cd/Makefile; \
	fi
endef

$(eval $(call BuildTarget))
