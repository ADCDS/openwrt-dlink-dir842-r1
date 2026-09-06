# SPDX-License-Identifier: GPL-2.0-only
#
# Vendor rtl8192cd driver for the RTL8197F on-SoC 2.4 GHz WMAC (WEXT, in-kernel PSK).
# Lands in the last port milestone; until then CONFIG_RTL8192CD stays unset and this
# package is simply not selected.
#
# ★ Loaded from /etc/modules.d only (AutoLoad's 2-arg form) -- deliberately NOT
# /etc/modules-boot.d. An earlier revision of this file passed AutoLoad a third
# argument (`,1`) to also symlink the module into modules-boot.d, on the theory
# that netifd's synchronous startup probe of every /lib/netifd/wireless/*.sh
# handler raced this module's load: the handler's rtl_find_phy() reads
# /proc/wlan0/mib_all, which does not exist until the module is up, and the load
# was measured at ~59-63s against network bring-up starting ~44s.
#
# That theory was WRONG, and the evidence for it was confounded. The observed
# symptom (netifd pinned at 77-92% CPU, not even br-lan ever coming up, cascading
# into OOM kills as procd respawned daemons into the starved window) was really
# the EOF bug in netifd's own handler_load() -- see
# files/package/network/config/netifd/files/lib/netifd/utils.uc and
# docs/PORT-MAIN-6.18-STATUS.md §6. The "removing the handler file fixes it" test
# looked decisive but changed two variables at once, because
# KernelPackage/rtl8192cd/install below ships the handler *inside this same
# package*: dropping the handler also dropped the module.
#
# Verified live on hardware 2026-09-03, after the utils.uc fix, with the handler
# present and this module absent: netifd settles at 0% CPU, all five
# network.interface objects register, and br-lan comes up with its address. The
# handler is not a boot hazard; the load-order race it was meant to fix is not
# real; and modules-boot.d loads during preinit, before the overlay is mounted,
# which is a genuinely worse place to bring up a DMA-allocating wireless driver
# on a 64 MB board. Hence: plain modules.d, late load, as upstream intends.
#
# Name collision (both radios can register as wlan0) is a separate concern and is
# handled independently of load order -- rtl_find_phy() matches by
# /proc/<name>/mib_all rather than a fixed name, and
# base-files/etc/uci-defaults/10-dir842-5g-ifname pins the mac80211 iface to
# wlan1.

# Captured at PARSE time, not referenced from inside a recipe: this file is
# `-include`d directly by package/kernel/linux/Makefile (see its
# SUBTARGET_MODULES), so by the time the /install recipe below actually runs,
# $(CURDIR) is package/kernel/linux, not this directory -- a bare relative
# path in the recipe would resolve against the wrong place. $(lastword
# $(MAKEFILE_LIST)) is only correct for the file currently being parsed, so it
# has to be latched into a plain variable here, at parse time, rather than
# read again later.
RTL819X_MODULES_MK_DIR:=$(dir $(lastword $(MAKEFILE_LIST)))

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

# The netifd handler ships ONLY as part of THIS package, never through
# base-files/. An earlier version placed it in base-files/ (installed
# unconditionally on every image, whether or not this kmod was even built)
# and that sent netifd into a 100%-CPU spin -- describing a wifi-device whose
# netdev never appears applies NO configuration at all, not even loopback --
# the moment any other real wireless subsystem (rtw88/mac80211) was also
# present. See docs/PORT-MAIN-6.18-STATUS.md §3 for the measured incident.
# include/kernel.mk's generated Package/kmod-rtl8192cd/install target calls
# this automatically, right after it copies the .ko -- and ONLY when
# CONFIG_PACKAGE_kmod-rtl8192cd is actually set (kernel.mk:259) -- so the
# handler's presence is now structurally tied to the kmod's presence.
define KernelPackage/rtl8192cd/install
	$(INSTALL_DIR) $(1)/lib/netifd/wireless
	$(INSTALL_BIN) $(RTL819X_MODULES_MK_DIR)rtl8192cd-files/lib/netifd/wireless/rtl8192cd.sh \
		$(1)/lib/netifd/wireless/
endef

$(eval $(call KernelPackage,rtl8192cd))
