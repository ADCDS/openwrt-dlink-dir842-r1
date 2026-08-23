# ★★★ R4/G3 — PATH B (WEXT). This build does NOT use the vendor cfg80211 layer.
#
# History: -DRTK_NL80211 (further down, now off) enables the CALL SITES of the
# vendor cfg80211/net80211 layer, while the OBJECTS carrying those definitions are
# added only under the MAKE variable CONFIG_OPENWRT_SDK. With the flag on and the
# make var off the module "linked" but failed modpost with ~12 undefined symbols
# (realtek_cfg80211_*, rtk_remove_dev, psk_indicate_evt, ...). Turning the make var
# ON did add 8192cd_net80211.o / 8192cd_psk_hapd.o / 8192cd_cfg80211.o — and then
# 8192cd_cfg80211.c hit ~40 hard errors from cfg80211 API drift between the vendor's
# 3.18 target and this tree's backports-5.8 headers (MEASURED, /kb-baseline.log):
#   redefinition of 'struct cfg80211_mgmt_tx_params'
#   cfg80211_inform_bss / cfg80211_disconnected / cfg80211_ibss_joined /
#     cfg80211_cac_event  — signatures all changed
#   STATION_INFO_* and SURVEY_INFO_CHANNEL_TIME* enums removed (→ NL80211_STA_INFO_*,
#     SURVEY_INFO_TIME*), struct survey_info::channel_time* renamed to ::time*
#   RATE_INFO_FLAGS_{40,80,160}_MHZ_WIDTH removed (→ rate_info::bw)
# That is a real forward-port of a 5,000-line file, not a flag fix.
#
# ★ PATH B instead: run the driver the way STOCK runs it — a pure WEXT netdev with
# the WPA2-PSK 4-way handshake INSIDE the driver (INCLUDE_WPA_PSK, 8192cd_psk.o +
# 1x_*.o). Confirmed by inspection of the stock rootfs: it ships NO hostapd at all.
# So -DRTK_NL80211 is off and CONFIG_OPENWRT_SDK stays unset; the three cfg80211
# objects are not built and nothing references them.
#   ⚠ Dropping RTK_NL80211 re-exposes a handful of use-sites in 8192cd_osdep.c whose
#     DEFINITIONS are gated on it but whose USES are not. Those are now individually
#     guarded with #ifdef RTK_NL80211 in that file — grep it for "R4/G3 Path B".
# CONFIG_OPENWRT_SDK=y

# ★★ R4/G4 CRITICAL: the 8822BE support is REMOVED from this build.
# rtl8192cd_pci_tbl[] claims PCI 10ec:b822 under CONFIG_WLAN_HAL_8822BE, and
# MODULE_DEVICE_TABLE + pci_register_driver are live (CONFIG_NET_PCI is force-set
# by 8192cd_cfg.h under NOT_RTK_BSP). Leaving it on means this driver STEALS the
# RTL8822BE from mainline rtw88 and silently kills the working 5 GHz AP — the exact
# opposite of what R4 wants. This driver must own ONLY the on-SoC 2.4 GHz WMAC;
# rtw88 keeps 5 GHz. CONFIG_PCI_HCI stays ON (it is the descriptor/ring model the
# on-SoC WMAC also uses, NOT a "PCIe card present" switch).
# ── R4/G3 port flags ────────────────────────────────────────────────────────────
# Placement matters: these must come BEFORE the vendor's own EXTRA_CFLAGS blocks.
# Appending them at the end instead breaks the vendor's -I$(src)/WlanHAL/... paths
# (Hal88XXDesc.h stops resolving) and the build collapses to 2 objects.
#
# 1) vendor targets gcc 4.x/7.x; this tree is gcc 8.4. Scoped to this driver only.
ccflags-y += -Wno-error=incompatible-pointer-types
ccflags-y += -Wno-error=implicit-function-declaration
ccflags-y += -Wno-error=int-conversion
ccflags-y += -Wno-error=discarded-qualifiers
ccflags-y += -Wno-error=return-type
# 2) <bspchip.h> lives in the mach dir; the kernel only adds the CONFIGURED
#    platform's mach directory, and this target builds as "realtek". Needed for
#    BSP_WLAN_*, BSP_BOND_97F*, BSP_PCIE0_*, BSP_GPIO_*, BSP_MISC_PINSR, ...
#    ★ That header independently confirms R4/G1: BSP_WLAN_BASE_ADDR 0xB8640000,
#    BSP_WLAN_MAC_IRQ CPU_BASE+6, BSP_WLAN_MAC_IE BIT(29).
#
#    ⚠⚠ HISTORY — the BSP_* symbols were undeclared for a long time and several
#    confident explanations here were WRONG. What was finally MEASURED (gcc -E -dD
#    re-running the real kbuild command line for 8192cd_osdep.o, recovered from
#    .8192cd_tx.o.cmd) is this: the preprocessed TU contained ZERO bspchip.h line
#    markers and ZERO BSP_* defines. This -I was never the problem — the driver's
#    own `#include <bspchip.h>` lines are simply unreachable, because each one is
#    gated on a platform-identification macro the Realtek SDK supplies from the
#    *kernel's* .config and that this tree does not have:
#        8192cd_osdep.c:168  #if defined(CONFIG_RTL_819X) && defined(__LINUX_2_6__)
#        8192cd_hw.c:65      #if defined(USE_RTL8186_SDK) && !defined(__OSK__)
#    CONFIG_RTL_819X is self-defined only at 8192cd_cfg.h:168, under BOTH
#    `#if !defined(__LINUX_2_6__)` and `#if defined(CONFIG_RTL8196B|C|8198)` —
#    both false here, so it is never defined. USE_RTL8186_SDK likewise never is.
#    ⇒ FIX: 8192cd_cfg.h now includes <bspchip.h> directly at the bottom (see the
#    comment there). This -I is what makes that include resolve, so it IS
#    load-bearing now — but only because of that edit.
#    ⚠ Still do NOT use -include: that lands the header ahead of the kernel's own
#    headers. Including it from the bottom of 8192cd_cfg.h puts it *after* them.
ccflags-y += -I$(srctree)/arch/mips/include/asm/mach-rtl8197f
# ★ __LINUX_2_6__ — kept for UNIFORMITY, not because a BSP_* symbol depends on it.
# 8192cd_cfg.h:99 defines it under `#if LINUX_VERSION_CODE > 0x020600`, which needs
# <linux/version.h> already pulled in by the TU at that point; nothing at the top of
# 8192cd_cfg.h guarantees that, so without this -D the macro would be set in some
# TUs and not others. (It does fire on its own in practice — the build emits ~106
# `"__LINUX_2_6__" redefined ... previous definition <command-line>` warnings, which
# is exactly the evidence that this -D is redundant-but-consistent.) Asserting it
# keeps 8192cd_cfg.h:165 / :1820 evaluating the same way in every TU.
ccflags-y += -D__LINUX_2_6__
# ✗ REMOVED: -DUSE_RLX_BSP. It was documented as the thing that made the driver
#   "REACH" the <bspchip.h> include. It is provably INERT, in two independent ways:
#     a) 8192cd_cfg.h:1820 already `#define USE_RLX_BSP` itself (inside
#        `#ifdef __LINUX_2_6__`, which we force above), and
#     b) 8192cd_cfg.h:2007 then does `#undef USE_RLX_BSP` inside `#ifdef NOT_RTK_BSP`
#        — and NOT_RTK_BSP is set by this port (see 5 below).
#   The preprocessed TU literally contains that `#undef`. And every file that TESTS
#   USE_RLX_BSP — 8192cd_osdep.c (44 vs 138), 8192cd_host.c (28 vs 56),
#   8192cd_hw.c (33 vs 67), 8192d_hw.c (12 vs 21), HalDMOutSrc.c (25 vs 38),
#   Hal8192CDMOutSrc.c (31 vs 47); first-#include line vs first-test line — reaches
#   8192cd_cfg.h BEFORE its first test, so all of them see it undefined no matter
#   what the command line says.
# 3) the vendor selects its SoC with CONFIG_RTL_8197F; this tree names the same SoC
#    CONFIG_SOC_RTL8197F, so gates silently took wrong branches (8192cd_osdep.c:174).
#    Also the condition on the <bspchip.h> include added to 8192cd_cfg.h.
ccflags-y += -DCONFIG_RTL_8197F
# 5) ★ NOT_RTK_BSP must be declared EXPLICITLY once (3) above is set.
#    8192cd_cfg.h auto-defines it only under
#      #if defined(_LITTLE_ENDIAN_) && !defined(CONFIG_RTL_8197F)
#    so defining CONFIG_RTL_8197F (correct for this SoC) silently switched it OFF,
#    which re-enabled the vendor-BSP code paths that touch pskb->__unused — a field
#    the vendor SDK adds to the kernel's core struct sk_buff. Declaring NOT_RTK_BSP
#    is also simply TRUE here: this is mainline-style OpenWrt with its own arch code,
#    not the Realtek BSP. Keeps include/linux/skbuff.h unpatched.
#    (wifi.h's NOT_RTK_BSP branch now carries wlan_amsdullcsnaphdr_t, added by this
#    port, so taking that branch is safe.)
ccflags-y += -DNOT_RTK_BSP
# 6) ✗ RTK_NL80211 — OFF (Path B, see the header). The vendor cfg80211/nl80211 layer.
#    Its DEFINITIONS are gated on this macro (struct rtl8192cd_priv::rtk at
#    8192cd.h:5638, the CFG80211_* enum at :1057, is_WRT_scan_iface() and
#    event_indicate_cfg80211() in 8192cd_cfg80211.c) while a few of its USE sites in
#    8192cd_osdep.c were not — those are now guarded in-place (see "R4/G3 Path B"
#    markers in that file). With the macro off, the driver registers a plain WEXT
#    netdev and does WPA2-PSK in-driver, which is exactly how stock runs (stock's
#    rootfs has no hostapd). Coexistence with rtw88's cfg80211 5 GHz phy is not a
#    problem: they are separate netdevs, and WEXT ioctls are still compiled into this
#    kernel (CONFIG_WIRELESS_EXT / CONFIG_WEXT_PRIV).
# ccflags-y += -DRTK_NL80211
# 7) cfg80211 band enum rename. Kernel 4.7 renamed IEEE80211_BAND_* to NL80211_BAND_*
#    (and IEEE80211_NUM_BANDS to NUM_NL80211_BANDS); the vendor targets 3.18 and still
#    uses the old spellings. Map them rather than editing the vendor sources.
ccflags-y += -DIEEE80211_BAND_2GHZ=NL80211_BAND_2GHZ
ccflags-y += -DIEEE80211_BAND_5GHZ=NL80211_BAND_5GHZ
ccflags-y += -DIEEE80211_BAND_60GHZ=NL80211_BAND_60GHZ
ccflags-y += -DIEEE80211_NUM_BANDS=NUM_NL80211_BANDS
# 8) ✗ REMOVED: the "BSP constants supplied directly with -D" scheme this item used
#    to describe. There were never any such -D flags left in this file, and they are
#    no longer needed: <bspchip.h> is now genuinely included (from the bottom of
#    8192cd_cfg.h) so all ~30 BSP_* symbols come from the real header with the real
#    spellings — including BSP_WLAN_MAC_IRQ, which is `(BSP_IRQ_CPU_BASE + 6)` and
#    therefore could not be expressed as a plain -D anyway.
#    ★ The header's values match what R4/G1 decoded from the stock vendor kernel and
#    what the rtl8197f-wmac driver then read on silicon (BSP_WLAN_BASE_ADDR
#    0xB8640000, BSP_WLAN_MAC_IRQ CPU_BASE+6). Three independent sources agree.
# 9) Two more port fixes live in the sources rather than here:
#    a) net80211/ — 8192cd_ioctl.c, 8192cd_net80211.c and 8192cd_cfg80211.c include
#       <net80211/ieee80211*.h> under `#if defined(WIFI_HAPD) || defined(RTK_NL80211)`.
#       The Realtek SDK stages those headers into the kernel's include/ dir; this tree
#       now does the same, at target/linux/realtek/files-4.14/include/net80211/
#       (copied verbatim from sdk-rtl819x/target/linux/realtek/files/include/net80211).
#       They resolve via the kernel's own -I./include.
#    b) <asm/uaccess.h> -> <linux/uaccess.h> across the vendor sources. Several files
#       (8192cd_security.c:15 is the one that actually broke) include <asm/uaccess.h>
#       BEFORE <linux/module.h>. On 4.14/MIPS asm/uaccess.h's get_fs() expands to
#       current_thread_info()->addr_limit, which needs <linux/thread_info.h> first;
#       included bare it gives 'invalid type argument of ->' (have 'int') and then
#       'conflicting types for current_thread_info'. <linux/uaccess.h> is the correct
#       post-3.4 spelling and pulls the prerequisites in itself.
#       ⚠ NOTE for the record: this error class was previously blamed on
#       `-include bspchip.h`. It is not caused by that at all — it is the vendor's own
#       include order, and it appeared here with no -include anywhere in the build.
# 4) ⚠ CONFIG_OPENWRT_SDK: NOT enabled, deliberately. It DOES fix the
#    pskb->__unused errors (that field is a vendor addition to the kernel's core
#    struct sk_buff), but it simultaneously stops <bspchip.h> resolving in
#    8192cd_osdep.c and introduces an arch/mips uaccess.h type error — net worse
#    (53 objects -> 51 plus new error classes). The vendor config model is
#    order- and combination-sensitive; this needs untangling systematically rather
#    than by flag experiments. See docs/VENDOR-PARITY-INVENTORY.md.
#    ⚠ Also do NOT clear CONFIG_PCI_HCI: it selects the PCI-style descriptor/ring
#    model that the ON-SOC WMAC uses too; clearing it collapses the build to 5.
# ────────────────────────────────────────────────────────────────────────────────

#
# drivers/net/rtl8192cd
#
# Makefile for the Linux Wireless network device drivers.
#

# (G4) CONFIG_BAND_5G_ON_WLAN0=y
# ★★★ CONFIG_BAND_2G_ON_WLAN0 — REQUIRED, and it is not cosmetic. It is what puts the
# ON-SOC WMAC into the driver's device table at all. In 8192cd_osdep.c the only entry
# describing the embedded radio is
#     #if defined(CONFIG_WLAN_HAL_8197F) && defined(CONFIG_BAND_2G_ON_WLAN0)
#     { TYPE_EMBEDDED | ACCESS_SWAP_MEM, BSP_WLAN_CONF_ADDR, BSP_WLAN_BASE_ADDR,
#       BSP_WLAN_MAC_IRQ, NULL },
# (there is a twin under BAND_5G_ON_WLAN0, but that one is emitted LAST — it describes
# a board where the SoC radio is wlan1 behind a 5 GHz PCIe card in wlan0).
# Without either macro, wlan_device[] degenerates to the three all-zero TYPE_PCI_BIOS
# entries from the `#elif defined(NOT_RTK_BSP)` leg, and rtl8192cd_init_module() —
# which for a MODULE build (CONFIG_WIRELESS_LAN_MODULE, RTK_NL80211 off) uses the
# single-shot `if (wlan_index < ARRAY_SIZE(wlan_device))` with wlan_index==0 — would
# see TYPE_PCI_BIOS at index 0 and do nothing but pci_register_driver(). The embedded
# radio would never be touched. With this macro the SoC entry IS index 0.
# Second effect, also correct here: 8192cd_cfg.h sets WLANIDX=1, which flips the
# RX_BUF_LEN / RX_DESC_NUM / RX_MAX_SKB_NUM selectors so that wlandev_idx 0 (our
# radio) gets the 2.4 GHz sizes (MAX_RX_BUF_LEN_1 / NUM_RX_DESC_2G / MAX_SKB_NUM_1)
# instead of the 5 GHz ones. Without it a 2.4 GHz radio would be given 5 GHz rings.
CONFIG_BAND_2G_ON_WLAN0=y
CONFIG_FREE_WBB_PIN=y
CONFIG_MAC_PHY_RF_Parameter_V702B_Skyworth=y
CONFIG_PCI_HCI=y
# ★★★ R4/G3 CRYSTAL — MUST STAY OFF ON THIS BOARD. Despite the "PHY"/"40MHZ" name
# this is NOT a channel-bandwidth switch: for the 8197F it is the ONLY input that
# picks the WLAN crystal, at 8192cd_hw.c:14672-14679
#     if (GET_CHIP_VER(priv) == VERSION_8197F) {
#         #ifdef CONFIG_PHY_EAT_40MHZ
#             ClkSel = XTAL_CLK_SEL_40M;
#         #else
#             ClkSel = XTAL_CLK_SEL_25M;
#         #endif
#     }
# and ClkSel is handed straight to InitPONHandler() — the WLAN power-on/PLL setup.
# The vendor's strap-reading alternative right above it is dead here: it sits under
# `#if defined(CONFIG_AUTO_PCIE_PHY_SCAN)`, which 8192cd_cfg.h:3535 only defines for
# CONFIG_RTL_819X && __OSK__ && CONFIG_RTL_8196E — none of which this port defines.
# So with the flag on, the Kconfig value silently WINS over the hardware.
#
# MEASURED, three independent sources say this board's WLAN crystal is 25 MHz:
#   1. stock firmware on this exact unit    -> "[13.710000] clock 25MHz"
#      (recon/bootlog-38400.txt:288, right after "[97F] Bonding Type 97FS, PKG1")
#   2. our own mainline platform stub reads the bootstrap bit SR+0x08 BIT(24)
#      (REALTEK_SR_BS_40MHZ) and prints "xtal 25MHz" -> the bit is CLEAR here
#      (rtl8197f-wmac.c:254)
#   3. with the flag ON we printed "clock 40MHz" and the radio was silent
# Symptom when wrong: everything ABOVE the synthesiser looks perfect — firmware
# loads, IQK completes, BDOK fires every TBTT (beacon_ok climbs at exactly the
# beacon rate) — but the LO is off by 40/25 = 1.6x, so nothing lands in the 2.4 GHz
# band and RX is stone deaf. Easy to misread as "the AP was never started".
#
# The flag's other uses are all in 8192cd_host.c's PCIe ROOT-COMPLEX MDIO PHY
# setup, which this port does not use (mainline realtek-pci owns the RC) and which
# is additionally gated on CONFIG_RTL_8198C / 819XD / 8196E / 88E — none defined.
# CONFIG_PHY_EAT_40MHZ=y
# (G4) CONFIG_RTL_5G_SLOT_0=y
CONFIG_RTL_CLIENT_MODE_SUPPORT=y
# ★ R4/G3: the config-file MIB ingest. Without it CfgFileProc() and its call site
# (8192cd_osdep.c:7691, on root-device open) are BOTH preprocessed away, so
# /etc/Wireless/RTL8192CD.dat is silently ignored — which would make
# base-files/lib/netifd/wireless/rtl8192cd.sh a no-op. It only selects
# 8192cd_comapi.o, which CONFIG_RTL_COMAPI_WLTOOLS already links in, so this adds
# no new objects. It also enables the "cfgfile" private ioctl (reload on demand).
CONFIG_RTL_COMAPI_CFGFILE=y
CONFIG_RTL_COMAPI_WLTOOLS=y
CONFIG_RTL_DFS_SUPPORT=y
CONFIG_RTL_DISABLE_WLAN_MIPS16=y
CONFIG_RTL_ODM_WLAN_DRIVER=y
CONFIG_RTL_REPEATER_MODE_SUPPORT=y
CONFIG_RTL_TDLS_SUPPORT=y
CONFIG_RTL_VAP_SUPPORT=y
CONFIG_RTL_WPS2_SUPPORT=y
# (G4) CONFIG_SLOT_0_8822BE=y
# (G4) CONFIG_SLOT_0_RFE_TYPE_10=y
# (G4) CONFIG_SLOT_0_TX_BEAMFORMING=y
CONFIG_SOC_RFE_TYPE_0=y
CONFIG_SOC_WIFI=y
CONFIG_TXPWR_LMT=y
# (G4) CONFIG_USE_PCIE_SLOT_0=y
CONFIG_WIRELESS_LAN_MODULE=y
CONFIG_WLAN_HAL=y
CONFIG_WLAN_HAL_8197F=y
# (G4) CONFIG_WLAN_HAL_8822BE=y
CONFIG_WLAN_HAL_88XX=y
# ★ (G4) CONFIG_WLAN_MACHAL_API=y — OFF, and it MUST follow CONFIG_WLAN_HAL_8822BE.
# "halmac" is the RTL8822B MAC abstraction library. In the vendor Makefile its
# objects (WlanHAL/HalMac88XX/**, incl. halmac_8822b/*) are added inside
# `ifeq ($(CONFIG_WLAN_HAL_8822BE),y)`, so switching 8822BE off drops the whole
# library — while this flag left the CALL alive:
#   8192cd_osdep.c:11248  Wlan_HAL_Link() -> halmac_init_adapter(...)
# guarded only by `if (GET_CHIP_VER(priv)==VERSION_8822B)`, i.e. dead at RUNTIME on
# the 8197F but still a link-time reference => modpost "halmac_init_adapter undefined".
# Verified safe to drop: grep finds ZERO halmac references under
# WlanHAL/RTL88XX/RTL8197F/, so the on-SoC radio's HAL never uses it.
# CONFIG_WLAN_MACHAL_API=y
CONFIG_RTL_819X_SWCORE=y
CONFIG_RTL_NO_BR_SHORTCUT=y
# 802.11r needs this as a MAKE VARIABLE as well as a -D (see the ccflags block
# below). This is the half that adds sha256.o to rtl8192cd-objs (Makefile:653),
# and FT's whole key hierarchy is sha256: PMK-R0/R1 come out of sha256_prf() and
# the *_Name values out of sha256_vector(). With only the -D, the FT code
# compiles and then fails to link. Verified the shipped module has zero sha256
# symbols today, and that none of the other three gates that would pull it in
# (11W / 11R / WAPI) is set.
CONFIG_RTL_11R_SUPPORT=y

ccflags-y += -DCONFIG_RTL_COMAPI_CFGFILE
ccflags-y += -DCONFIG_RTL_COMAPI_WLTOOLS
# ── 802.11r (fast transition) on the on-SoC 2.4 GHz WMAC ─────────────────────
# EXPERIMENT 2026-08-23. The vendor FT implementation is fully present in this
# tree (r0/r1 key holders, MDIE/FTIE, FT auth algorithm, and PMK-R0 derived
# LOCALLY from the PSK in 8192cd_psk.c:5386 — the equivalent of hostapd's
# ft_psk_generate_local, so NO userspace FT daemon is required). It was simply
# never compiled: 8192cd_cfg.h:3090 gates `#define CONFIG_IEEE80211R` behind
# CONFIG_RTL_11R_SUPPORT, and this Makefile enabled 14 other CONFIG_RTL_*
# features but not that one.
#
# MEASURED without it, on the live box: the module contains no FT strings at
# all and the MIB keys do not exist, so the .dat lines are rejected outright —
#   CFGFILE set_mib "ft_enable=1" failed
#   CFGFILE set_mib "ft_mdid=b1a0" failed
# and the advertised RSN IE stays AKM-count-1 (PSK only, 00-0F-AC:02), which is
# why clients never attempt fast transition on 2.4 GHz.
#
# This also pulls in SUPPORT_FAST_CONFIG=2 (R1KH_PSUH_CALC_MIX) from the same
# #ifdef: local R1 calculation PLUS an optional key push to a userspace daemon.
# The push is guarded by `wlanft_pid > 0`, which stays unset with no daemon
# registered, so mode 2 degrades to pure local calculation. If FT ever
# misbehaves, force SUPPORT_FAST_CONFIG=1 (R1KH_CALC_ONLY) before blaming keys.
ccflags-y += -DCONFIG_RTL_11R_SUPPORT
# (G4) ccflags-y += -DCONFIG_BAND_5G_ON_WLAN0
ccflags-y += -DCONFIG_BAND_2G_ON_WLAN0
ccflags-y += -DCONFIG_FREE_WBB_PIN
ccflags-y += -DCONFIG_MAC_PHY_RF_Parameter_V702B_Skyworth
ccflags-y += -DCONFIG_PCI_HCI
# ✗ OFF — see the CONFIG_PHY_EAT_40MHZ block above. This board's WLAN crystal is
#   25 MHz; asserting this made the 8197F PLL come up for a 40 MHz crystal.
# ccflags-y += -DCONFIG_PHY_EAT_40MHZ
# (G4) ccflags-y += -DCONFIG_RTL_5G_SLOT_0
ccflags-y += -DCONFIG_RTL_CLIENT_MODE_SUPPORT
ccflags-y += -DCONFIG_RTL_DFS_SUPPORT
ccflags-y += -DCONFIG_RTL_DISABLE_WLAN_MIPS16
ccflags-y += -DCONFIG_RTL_ODM_WLAN_DRIVER
ccflags-y += -DCONFIG_RTL_REPEATER_MODE_SUPPORT
ccflags-y += -DCONFIG_RTL_TDLS_SUPPORT
ccflags-y += -DCONFIG_RTL_VAP_SUPPORT
ccflags-y += -DCONFIG_RTL_WPS2_SUPPORT
# (G4) ccflags-y += -DCONFIG_SLOT_0_8822BE
# (G4) ccflags-y += -DCONFIG_SLOT_0_RFE_TYPE_10
# (G4) ccflags-y += -DCONFIG_SLOT_0_TX_BEAMFORMING
ccflags-y += -DCONFIG_SOC_RFE_TYPE_0
ccflags-y += -DCONFIG_SOC_WIFI
ccflags-y += -DCONFIG_TXPWR_LMT
# (G4) ccflags-y += -DCONFIG_USE_PCIE_SLOT_0
ccflags-y += -DCONFIG_WIRELESS_LAN_MODULE
ccflags-y += -DCONFIG_WLAN_HAL
ccflags-y += -DCONFIG_WLAN_HAL_8197F
# (G4) ccflags-y += -DCONFIG_WLAN_HAL_8822BE
ccflags-y += -DCONFIG_WLAN_HAL_88XX
# (G4) ccflags-y += -DCONFIG_WLAN_MACHAL_API   <- see the make-var block above
ccflags-y += -DCONFIG_RTL_819X_SWCORE
ccflags-y += -DCONFIG_RTL_NO_BR_SHORTCUT

$(info CFLAGS BL $(CFLAGS))

ifeq ($(CPTCFG_RTL8192CD),m)
CONFIG_RTL8192CD:=m
endif

ifeq ($(CONFIG_RTL_92C_SUPPORT),y)
RTL_WLAN_DATA_DIR := data
endif
  
ifeq ($(CONFIG_RTL_92D_SUPPORT),y)
RTL_WLAN_DATA_DIR_D := data_92d
endif

ifeq ($(CONFIG_RTL_88E_SUPPORT),y)
  RTL_WLAN_DATA_DIR_E := data_88e
endif

ifeq ($(CONFIG_RTL_8812_SUPPORT),y)
  RTL_WLAN_DATA_DIR_8812 := data_8812
endif

ifeq ($(CONFIG_RTL_8723B_SUPPORT),y)
  RTL_WLAN_DATA_DIR_8723 := data_8723
endif

ifeq ($(CONFIG_WLAN_HAL),y)
	ifeq ($(CONFIG_WLAN_HAL_8814AE),y)
		RTL_WLAN_DATA_DIR_8814 := WlanHAL/Data/8814A
	endif	
	ifeq ($(CONFIG_WLAN_HAL_8192EE),y)
		RTL_WLAN_DATA_DIR_92E := WlanHAL/Data/8192E
	endif
	ifeq ($(CONFIG_WLAN_HAL_8881A),y)
		ifeq ($(CONFIG_MAC_PHY_RF_Parameter_V700),y)
			RTL_WLAN_DATA_DIR_8881A := WlanHAL/Data/8881A/V700
		endif
		ifeq ($(CONFIG_MAC_PHY_RF_Parameter_V702B),y)
			RTL_WLAN_DATA_DIR_8881A := WlanHAL/Data/8881A/V702B
		endif
		ifeq ($(CONFIG_MAC_PHY_RF_Parameter_V702B_Skyworth),y)
			RTL_WLAN_DATA_DIR_8881A := WlanHAL/Data/8881A/V702B_Skyworth
		endif
#		ifeq ($(CONFIG_MAC_PHY_RF_Parameter_V702B_MP),y)
#			RTL_WLAN_DATA_DIR_8881A := WlanHAL/Data/8881A/V702B_MP
#		endif
		RTL_WLAN_DATA_DIR_8812 := WlanHAL/Data/8881A/V702B_MP
	endif
	ifeq ($(CONFIG_WLAN_HAL_8197F),y)
		RTL_WLAN_DATA_DIR_97F := WlanHAL/Data/8197F
	endif	
	ifeq ($(CONFIG_WLAN_HAL_8822BE),y)
		RTL_WLAN_DATA_DIR_8822B := WlanHAL/Data/8822B
	endif		
endif

ifeq ($(CONFIG_RTL_MESH_SUPPORT),y)
        EXTRA_CFLAGS += -DCONFIG_RTK_MESH #-DMESH_USE_METRICOP
#        ifeq ($(CONFIG_11S_TEST_MODE),y)
#        EXTRA_CFLAGS += -D_11s_TEST_MODE_
#        endif
        obj-mesh = mesh_ext/mesh_proc.o\
        mesh_ext/mesh_route.o\
        mesh_ext/mesh_rx.o\
        mesh_ext/mesh_sme.o\
        mesh_ext/mesh_security.o\
        mesh_ext/mesh_tx.o\
        mesh_ext/mesh_util.o\
        mesh_ext/mesh_11kv.o\
        mesh_ext/hash_table.o
endif

SRCS_TXT = $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR)/*.txt))))
SRCS_BIN = $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR)/*.bin))))
SRCS_BIN_U = $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_D)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_D)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_D)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_E)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_E)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_E)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8812)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8812)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8812)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8723)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8723)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8723)/*.BIN))))

SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_92E)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_92E)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_92E)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8822B)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8822B)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8822B)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_97F)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_97F)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_97F)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8881A)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8881A)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8881A)/*.BIN))))
SRCS_TXT += $(addprefix $(src)/data_,$(notdir $(patsubst %.txt,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8814)/*.txt))))
SRCS_BIN += $(addprefix $(src)/data_,$(notdir $(patsubst %.bin,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8814)/*.bin))))
SRCS_BIN_U += $(addprefix $(src)/data_,$(notdir $(patsubst %.BIN,%.c,$(wildcard $(src)/$(RTL_WLAN_DATA_DIR_8814)/*.BIN))))

TXPWR_HEADER = 
ifeq ($(CONFIG_RTL_92C_SUPPORT),y)
TXPWR_LMT_TXT_92C = $(addprefix $(src)/data/,$(notdir $(wildcard $(src)/$(RTL_WLAN_DATA_DIR)/TXPWR*.txt)))
TXPWR_LMT_FNAME_92C = $(basename $(notdir $(wildcard $(src)/$(RTL_WLAN_DATA_DIR)/TXPWR*.txt)))
TXPWR_HEADER += $(obj)/TXPWR_92C.h
endif

ifeq ($(CONFIG_RTL_92D_SUPPORT),y)
TXPWR_LMT_TXT_92D = $(addprefix $(src)/data_92d/,$(notdir $(wildcard $(src)/$(RTL_WLAN_DATA_DIR_D)/TXPWR*.txt)))
TXPWR_LMT_FNAME_92D = $(basename $(notdir $(wildcard $(src)/$(RTL_WLAN_DATA_DIR_D)/TXPWR*.txt)))
TXPWR_HEADER += $(obj)/TXPWR_92D.h
endif

ifeq ($(CONFIG_RTL_DOT11K_SUPPORT),y)
	rtl8192cd-objs += \
	dot11k/8192cd_11k.o \
	dot11k/8192cd_11k_neighbor.o\
	dot11k/8192cd_11k_link.o\
	dot11k/8192cd_11k_beacon.o
endif

ifeq ($(CONFIG_RTL_11V_SUPPORT),y)
	rtl8192cd-objs += 8192cd_11v.o
endif

ifeq ($(CONFIG_RTL_STA_CONTROL_SUPPORT),y)
	rtl8192cd-objs += 8192cd_sta_control.o
endif

ifeq ($(CONFIG_RTK_SMART_ROAMING),y)
	rtl8192cd-objs += 8192cd_smart_roaming.o
endif

#obj-$(CONFIG_RTL_WAPI_SUPPORT) += wapi_wai.o wapiCrypto.o wapiRandom.o
obj-$(CONFIG_RTL8192CD) += rtl8192cd.o

ifeq ($(CONFIG_RTL_88E_SUPPORT),y)
	rtl8192cd-objs += HalPwrSeqCmd.o\
	Hal8188EPwrSeq.o\
	8188e_hw.o 
	ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
		rtl8192cd-objs += \
        phydm/rtl8188e/halhwimg8188e_bb.o\
        phydm/rtl8188e/halhwimg8188e_mac.o\
        phydm/rtl8188e/halhwimg8188e_rf.o\
        phydm/rtl8188e/phydm_regconfig8188e.o\
        phydm/rtl8188e/hal8188erateadaptive.o\
        phydm/rtl8188e/phydm_rtl8188e.o\
        phydm/rtl8188e/halphyrf_8188e_ap.o			
	else
		rtl8192cd-objs += Hal8192CDMOutSrc.o \
		rtl8192cd-objs += RateAdaptive.o 
	endif
endif

ifeq ($(CONFIG_RTL_8723B_SUPPORT),y)				
	rtl8192cd-objs += HalPwrSeqCmd.o\
	Hal8723BPwrSeq.o\
	8812_hw.o\
	phydm/rtl8723b/phydm_rtl8723b.o\
	phydm/rtl8723b/halhwimg8723b_bb.o\
	phydm/rtl8723b/halhwimg8723b_fw.o\
	phydm/rtl8723b/halhwimg8723b_mac.o\
	phydm/rtl8723b/halhwimg8723b_mp.o\
	phydm/rtl8723b/halhwimg8723b_rf.o\
	phydm/rtl8723b/phydm_regconfig8723b.o\
	phydm/rtl8723b/phydm_rtl8723b.o\
	phydm/rtl8723b/halphyrf_8723b_ap.o
endif

ifeq ($(CONFIG_RTL_8812_SUPPORT),y)
       rtl8192cd-objs += HalPwrSeqCmd.o\
        Hal8812PwrSeq.o\
        8812_hw.o
	ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
	     rtl8192cd-objs += ./phydm/rtl8812a/halphyrf_8812a_ap.o
#	     rtl8192cd-objs += ./phydm/rtl8812a/halphyrf_8812a_ap.o\
#		phydm/rtl8812a/halhwimg8812a_bb.o\
#		phydm/rtl8812a/halhwimg8812a_mac.o\
#		phydm/rtl8812a/halhwimg8812a_rf.o\
#		phydm/rtl8812a/odm_RegConfig8812A.o
	endif
endif
ifeq ($(CONFIG_RTL_92C_SUPPORT),y) 
	rtl8192cd-objs += Hal8192CDMOutSrc.o
endif
ifeq ($(CONFIG_RTL_92D_SUPPORT),y)
        rtl8192cd-objs += 8192d_hw.o Hal8192CDMOutSrc.o
endif

EXTRA_CFLAGS += -DDM_ODM_SUPPORT_TYPE=0x01
ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
	rtl8192cd-objs += \
	phydm/phydm.o \
	phydm/phydm_dig.o\
	phydm/phydm_edcaturbocheck.o\
	phydm/phydm_antdiv.o\
	phydm/phydm_soml.o\
	phydm/phydm_dynamicbbpowersaving.o\
	phydm/phydm_pathdiv.o\
	phydm/phydm_rainfo.o\
	phydm/phydm_dynamictxpower.o\
	phydm/phydm_powertracking_ap.o\
	phydm/phydm_adaptivity.o\
	phydm/phydm_debug.o\
	phydm/phydm_interface.o\
	phydm/phydm_hwconfig.o\
	phydm/phydm_dfs.o\
	phydm/halphyrf_ap.o\
	phydm/phydm_cfotracking.o\
	phydm/phydm_acs.o\
	phydm/phydm_adc_sampling.o\
	phydm/phydm_ccx.o\
	phydm/txbf/phydm_hal_txbf_api.o\
	phydm/phydm_noisemonitor.o\
	EdcaTurboCheck.o
	EXTRA_CFLAGS += -I$(src) -I$(src)/phydm
endif

ifeq ($(CONFIG_WLAN_HAL_8881A),y)
	rtl8192cd-objs += phydm/rtl8821a/phydm_iqk_8821a_ap.o
endif

ifeq ($(CONFIG_WLAN_HAL_8192EE),y)
	rtl8192cd-objs += \
	phydm/rtl8192e/halphyrf_8192e_ap.o\
	phydm/rtl8192e/phydm_rtl8192e.o
endif
ifeq ($(CONFIG_WLAN_HAL_8814AE),y)
	rtl8192cd-objs += phydm/rtl8814a/halphyrf_8814a_ap.o
	rtl8192cd-objs += phydm/rtl8814a/phydm_iqk_8814a.o
	ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
		rtl8192cd-objs += \
        phydm/rtl8814a/halhwimg8814a_bb.o\
        phydm/rtl8814a/halhwimg8814a_mac.o\
        phydm/rtl8814a/halhwimg8814a_rf.o\
        phydm/rtl8814a/phydm_regconfig8814a.o\
        phydm/rtl8814a/phydm_rtl8814a.o			
	endif
endif
ifeq ($(CONFIG_WLAN_HAL_8822BE),y)
	rtl8192cd-objs += phydm/rtl8822b/halphyrf_8822b.o
	rtl8192cd-objs += phydm/rtl8822b/phydm_iqk_8822b.o
	ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
		rtl8192cd-objs += \
        phydm/rtl8822b/halhwimg8822b_bb.o\
        phydm/rtl8822b/halhwimg8822b_mac.o\
        phydm/rtl8822b/halhwimg8822b_rf.o\
		phydm/rtl8822b/halhwimg8822b_fw.o\
        phydm/rtl8822b/phydm_regconfig8822b.o\
        phydm/rtl8822b/phydm_hal_api8822b.o\
		phydm/rtl8822b/phydm_rtl8822b.o
	endif
endif

ifeq ($(CONFIG_WLAN_HAL_8197F),y)
		rtl8192cd-objs += phydm/rtl8197f/halphyrf_8197f.o
		rtl8192cd-objs += phydm/rtl8197f/phydm_iqk_8197f.o
		rtl8192cd-objs += efuse_97f/efuse.o
	ifeq ($(CONFIG_RTL_ODM_WLAN_DRIVER),y)
		rtl8192cd-objs += \
		phydm/rtl8197f/halhwimg8197f_bb.o\
        phydm/rtl8197f/halhwimg8197f_mac.o\
        phydm/rtl8197f/halhwimg8197f_rf.o\
		phydm/rtl8197f/phydm_hal_api8197f.o\
		phydm/rtl8197f/phydm_regconfig8197f.o\
		phydm/rtl8197f/phydm_rtl8197f.o
	endif
endif

ifeq ($(CONFIG_BT_COEXIST),y)
	EXTRA_CFLAGS += -I$(src)/btcoexist
	rtl8192cd-objs += \
	btcoexist/rtl_btc.o
	ifeq ($(CONFIG_BT_COEXIST_SOCKET),y)
		rtl8192cd-objs += \
		btcoexist/halbtc_socket.o
	endif
endif

# R4/G3 Path B: replacements for the two Realtek-BSP exports this tree's arch code
# does not have (rtl819x_bond_option, PCIE_reset_procedure_97F). See the file header.
rtl8192cd-objs += 8192cd_owrt_bsp.o

rtl8192cd-objs +=\
	8192cd_tx.o\
	8192cd_rx.o\
	8192cd_osdep.o\
	8192cd_sme.o\
	8192cd_util.o\
	8192cd_hw.o\
	8192cd_security.o\
	8192cd_tkip.o\
	8192cd_aes.o\
	8192cd_proc.o\
	8192cd_br_ext.o\
	8192cd_eeprom.o\
	8192cd_mp.o\
	8192cd_psk.o\
	8192cd_ioctl.o\
	1x_kmsm_aes.o\
	1x_kmsm_hmac.o\
	1x_md5c.o\
	1x_rc4.o\
	8192cd_mib.o\
	8192cd_dmem.o\
	8192cd_host.o\
	8192cd_led.o\
	8192cd_dfs.o\
	8192cd_dfs_det.o\
	8812_vht_gen.o\
	romeperf.o\
	HalDMOutSrc.o\
	Beamforming.o\
	8192cd_11h.o\
	8192cd_debug.o\
	$(obj-mesh)
	
ifeq ($(CONFIG_RTL_A4_STA_SUPPORT),y)
obj-y += 8192cd_a4_sta.o
endif	

ifeq ($(CONFIG_RTL_11W_SUPPORT),y)
	rtl8192cd-objs += sha256.o
else ifeq ($(CONFIG_RTL_11R_SUPPORT),y)
	rtl8192cd-objs += sha256.o
else ifeq ($(CONFIG_RTL_WAPI_SUPPORT),y)
	rtl8192cd-objs += sha256.o
endif

ifeq ($(CONFIG_RTL_SIMPLE_CONFIG),y)
obj-y += 8192cd_profile.o
endif	
ifeq ($(CONFIG_RTL_COMAPI_CFGFILE),y)
obj-y += 8192cd_comapi.o
endif
ifeq ($(CONFIG_RTL_P2P_SUPPORT),y)
ifeq ($(CONFIG_OPENWRT_SDK),y)
rtl8192cd-objs += 8192cd_p2p.o
else
obj-y += 8192cd_p2p.o
endif
endif
ifeq ($(CONFIG_RTL_COMAPI_WLTOOLS),y)
rtl8192cd-objs += 8192cd_comapi.o
endif
ifeq ($(CONFIG_PACP_SUPPORT),y)
obj-y += 8192cd_comapi.o
endif	

#For Hostapd
ifeq ($(CONFIG_RTL_HOSTAPD_SUPPORT),y)
obj-y += 8192cd_comapi.o
endif
ifeq ($(CONFIG_RTL8672),y)
obj-y += rtl8672_port.o
endif

ifeq ($(CONFIG_OPENWRT_SDK),y)
rtl8192cd-objs += 8192cd_net80211.o
rtl8192cd-objs += 8192cd_psk_hapd.o
rtl8192cd-objs += 8192cd_cfg80211.o
ifeq ($(CONFIG_RTL_P2P_SUPPORT),y)
rtl8192cd-objs += rtw_android.o
endif
ifeq ($(CONFIG_PACP_SUPPORT),y)
rtl8192cd-objs += 8192cd_comapi.o
endif
endif

ifeq ($(CONFIG_WLAN_HAL),y)
	EXTRA_CFLAGS += -I$(src) -I$(src)/WlanHAL/ -I$(src)/WlanHAL/Include -I$(src)/WlanHAL/HalHeader 
	rtl8192cd-objs += WlanHAL/HalCommon.o \
										WlanHAL/HalCfg.o \
										WlanHAL/HalDbgCmd.o	\
										WlanHAL/HalMacFunc.o \
										WlanHAL/HalMacAPI.o 
										
endif

ifeq ($(CONFIG_WLAN_HAL_88XX),y)
	EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX -I$(src)/WlanHAL/HalMac88XX 
	rtl8192cd-objs += WlanHAL/RTL88XX/Hal88XXFirmware.o \
										WlanHAL/RTL88XX/Hal88XXGen.o \
										WlanHAL/RTL88XX/Hal88XXHWImg.o \
										WlanHAL/RTL88XX/Hal88XXIsr.o	\
										WlanHAL/RTL88XX/Hal88XXPwrSeqCmd.o \
										WlanHAL/RTL88XX/Hal88XXRxDesc.o \
										WlanHAL/RTL88XX/Hal88XXTxDesc.o \
										WlanHAL/RTL88XX/Hal88XXVerify.o \
										WlanHAL/RTL88XX/Hal88XXPhyCfg.o \
										WlanHAL/RTL88XX/Hal88XXDM.o																
endif

ifeq ($(CONFIG_WLAN_HAL_8881A),y)
	EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX/RTL8881A
	rtl8192cd-objs += WlanHAL/RTL88XX/RTL8881A/Hal8881AFirmware.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881AGen.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881AHWImg.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881AIsr.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881APwrSeqCmd.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881ARxDesc.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881ATxDesc.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881AVerify.o \
										WlanHAL/RTL88XX/RTL8881A/Hal8881APhyCfg.o \
										WlanHAL/RTL88XX/RTL8881A/RTL8881AE/Hal8881AEGen.o \
										8812_hw.o									
endif

ifeq ($(CONFIG_WLAN_HAL_8192EE),y)
		EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX/RTL8192E/RTL8192EE -I$(src)/WlanHAL/RTL88XX/RTL8192E
		rtl8192cd-objs += WlanHAL/RTL88XX/RTL8192E/RTL8192EE/Hal8192EEGen.o \
				  WlanHAL/RTL88XX/RTL8192E/Hal8192EGen.o \
				  WlanHAL/RTL88XX/RTL8192E/Hal8192EPhyCfg.o \
				  WlanHAL/RTL88XX/RTL8192E/Hal8192EPwrSeqCmd.o	
endif

ifeq ($(CONFIG_WLAN_HAL_8197F),y)
	EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX/RTL8197F -I$(src)/efuse_97f
	rtl8192cd-objs += WlanHAL/RTL88XX/RTL8197F/Hal8197FFirmware.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FGen.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FHWImg.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FIsr.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FPwrSeqCmd.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FRxDesc.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FTxDesc.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FVerify.o \
										WlanHAL/RTL88XX/RTL8197F/Hal8197FPhyCfg.o \
										WlanHAL/RTL88XX/RTL8197F/RTL8197FE/Hal8197FEGen.o 					
endif

ifeq ($(CONFIG_WLAN_HAL_8814AE),y)
		EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX/RTL8814A/RTL8814AE -I$(src)/WlanHAL/RTL88XX/RTL8814A
		rtl8192cd-objs += WlanHAL/RTL88XX/RTL8814A/RTL8814AE/Hal8814AEGen.o \
				  WlanHAL/RTL88XX/RTL8814A/Hal8814AGen.o \
				  WlanHAL/RTL88XX/RTL8814A/Hal8814APhyCfg.o \
				  WlanHAL/RTL88XX/RTL8814A/Hal8814APwrSeqCmd.o	\
					8812_hw.o					  
endif

ifeq ($(CONFIG_WLAN_HAL_8822BE),y)
	EXTRA_CFLAGS += -I$(src)/WlanHAL/RTL88XX/RTL8822B
	rtl8192cd-objs += WlanHAL/RTL88XX/RTL8822B/Hal8822BFirmware.o \
										WlanHAL/RTL88XX/RTL8822B/RTL8822BE/Hal8822BEGen.o \
										WlanHAL/RTL88XX/RTL8822B/Hal8822BGen.o \
										WlanHAL/RTL88XX/RTL8822B/Hal8822BPwrSeqCmd.o \
										WlanHAL/RTL88XX/RTL8822B/Hal8822BPhyCfg.o \
										8812_hw.o
ifeq ($(CONFIG_WLAN_MACHAL_API),y)
	EXTRA_CFLAGS += -I$(src)/WlanHAL/HalMac88XX 
	rtl8192cd-objs += WlanHAL/HalMac88XX/halmac_api.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_api_88xx.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_api_88xx_pcie.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_api_88xx_sdio.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_api_88xx_usb.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_func_88xx.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_func_88xx.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_8822b_pwr_seq.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_api_8822b.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_api_8822b_pcie.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_api_8822b_sdio.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_api_8822b_usb.o \
										WlanHAL/HalMac88XX/halmac_88xx/halmac_8822b/halmac_func_8822b.o
endif
										
endif

ifeq ($(CONFIG_RTL8672),y)
	ifeq ($(CONFIG_OPENWRT_SDK),y)
		ccflags-y += -DCONFIG_RTL8196B -DCONFIG_RTL8196C -DCONFIG_RTL8196B_GW -DCONFIG_RTL8196C_TESTCHIP_PATCH
	else
		EXTRA_CFLAGS += -DCONFIG_RTL8196B -DCONFIG_RTL8196C -DCONFIG_RTL8196B_GW -DCONFIG_RTL8196C_TESTCHIP_PATCH
		ifeq ($(CONFIG_DEFAULTS_KERNEL_2_6),y)
			EXTRA_CFLAGS += -DCONFIG_COMPAT_NET_DEV_OPS -D_MP_TELNET_SUPPORT_
		endif
	endif
endif

#CONFIG_SINUX_SUPPORT=1
ifeq ($(CONFIG_SINUX_SUPPORT),1)
EXTRA_CFLAGS  += -DOPENSSL_FIPS -D__linux__ -DRSDK_BUILT -DOPENSSL_NO_SPEED -DOPENSSL_THREADS -D_REENTRANT \
                        -DDSO_DLFCN -DHAVE_DLFCN_H -DOPENSSL_NO_KRB5 -DB_ENDIAN -DTERMIO \
			-fomit-frame-pointer 
# -save-temps                      
                        
obj-y  += wps/ssl/mem.o wps/ssl/mem_clr.o wps/ssl/bn_add.o wps/ssl/bn_lib.o \
			wps/ssl/bn_asm.o wps/ssl/bn_const.o wps/ssl/bn_ctx.o \
                wps/ssl/bn_div.o wps/ssl/bn_exp.o wps/ssl/bn_gcd.o \
				wps/ssl/bn_mod.o wps/ssl/bn_mont.o \
                wps/ssl/bn_mul.o wps/ssl/bn_prime.o wps/ssl/bn_rand.o \
				wps/ssl/bn_recp.o wps/ssl/bn_shift.o wps/ssl/bn_sqr.o \
                wps/ssl/bn_word.o wps/ssl/dh_check.o wps/ssl/dh_gen.o \
				wps/ssl/dh_key.o wps/ssl/dh_lib.o wps/ssl/digest.o \
                wps/ssl/m_sha1.o wps/ssl/hmac.o wps/ssl/md_rand.o \
				wps/ssl/rand_lib.o wps/ssl/sha1dgst.o wps/ssl/sha256.o \
                wps/ssl/aes_cbc.o wps/ssl/fips_aes_core.o
				
#EXTRA_CFLAGS += -I../../../lib
obj-y += wps/8192cd_wscd.o
obj-y += wps/sercomm_intf.o
endif

EXTRA_CFLAGS += -I$(DIR_BOARD) -D'SVN_REV="$(shell svnversion -n $(src))"'
#EXTRA_CFLAGS += -I$(DIR_BOARD) -Werror
#EXTRA_CFLAGS += -I$(DIR_LINUX)/drivers/net/rtl819x/

#
#Build C code for TXT or BIN file
#
$(obj)/8192cd_hw.o :  $(SRCS_TXT) $(SRCS_BIN) $(SRCS_BIN_U) $(TXPWR_HEADER)

$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR)/%.txt FORCE
	rm -f $(obj)/TXPWR_92C.h
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_D)/%.txt FORCE
	rm -f $(obj)/TXPWR_92D.h
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_D)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_D)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@				
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_E)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_E)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_E)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@	
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8812)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8812)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8812)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@	
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8723)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8723)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8723)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@		

$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_92E)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_92E)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_92E)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@

ifneq ($(TARGET_PROFILE),)
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_97F)/%_$(TARGET_PROFILE).override FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
endif
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_97F)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_97F)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_97F)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@

ifneq ($(TARGET_PROFILE),)
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8822B)/%_$(TARGET_PROFILE).override FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
endif
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8822B)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8822B)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8822B)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
	
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8881A)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8881A)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8881A)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@

$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8814)/%.txt FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8814)/%.bin FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
$(obj)/data_%.c: $(src)/$(RTL_WLAN_DATA_DIR_8814)/%.BIN FORCE
	$(obj)/bin2c.pl $(notdir $(basename $@)) < $< > $@
	
$(obj)/TXPWR_92C.h: $(TXPWR_LMT_TXT_92C) FORCE 		 
	@echo $(TXPWR_LMT_FNAME_92C) > $(obj)/tmp_TXPWR
	$(obj)/tplmt2h.pl < $(obj)/tmp_TXPWR > $@
	rm -f $(obj)/tmp_TXPWR
$(obj)/TXPWR_92D.h: $(TXPWR_LMT_TXT_92D) FORCE 		 
	@echo $(TXPWR_LMT_FNAME_92D) > $(obj)/tmp_TXPWR
	$(obj)/tplmt2h.pl < $(obj)/tmp_TXPWR > $@
	rm -f $(obj)/tmp_TXPWR	
