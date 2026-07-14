#!/usr/bin/env bash
# Build mainline-style OpenWrt for the D-Link DIR-842 rev R1 (RealTek RTL8197F).
#
# The RTL819x/RTL8197F MIPS home-router platform is NOT in upstream OpenWrt
# (mainline's "realtek" target is the RTL838x/930x managed switches only). The
# only tree with RTL8197F target support is the ggbruno fork. This script
# overlays ./files/ (the DIR-842 device support + our fixes) onto a pinned
# ggbruno checkout and builds a RAM-boot initramfs image.
#
# IMPORTANT: this port is RAM-boot only (serial XMODEM into memory, then jump);
# it does NOT write flash. Stock D-Link firmware stays intact — see README.md.
#
# Build environment: the ggbruno fork is from 2020 (kernel 4.14 / gcc 8.4). Use
# a Debian 11 (bullseye)-era build host or container; very new toolchains can
# fail to build the old host tools. See README.md for a container one-liner.
#
# Optional private profile (pre-configured gateway image):
#   PROFILE=~/dir842-profile ./build.sh
set -e
cd "$(dirname "$0")"

[ -e openwrt ] && { echo "ERROR: ./openwrt already exists — remove it first." >&2; exit 1; }

# Base tree: ggbruno/openwrt @ 8a0ccb9 (branch Realtek) — the only OpenWrt tree
# with RTL819x/RTL8197F target support.
git clone https://github.com/ggbruno/openwrt
cd openwrt
git checkout 8a0ccb93f3431bcf8f5c5d03d4acc2c8e442de67

# Overlay DIR-842 device support + fixes (path-preserving)
cp -a ../files/. .

# ---- optional: private profile overlay (PROFILE=/path/to/profile) ----
# Bakes a private, secret-bearing profile into the image as custom rootfs files
# (OpenWrt copies ./files/ into the rootfs). The profile's
# files/etc/config/{network,wireless,firewall,dhcp} hold the real gateway
# topology + WiFi PSK, so a clean image boots configured. It lives OUTSIDE this
# repo; only its path is passed in:  PROFILE=~/dir842-profile ./build.sh
# NOTE: full gateway routing needs the deferred RTL8367R switch + WAN driver
# work (docs/ASSESSMENT.md); today only a flat LAN + WiFi AP function.
if [ -n "$PROFILE" ]; then
	[ -d "$PROFILE/files" ] || { echo "ERROR: PROFILE=$PROFILE has no files/ dir." >&2; exit 1; }
	echo ">>> PROFILE=$PROFILE: baking private profile config into the image"
	mkdir -p files
	cp -a "$PROFILE"/files/. files/
	# git records only the exec bit; normalize modes so a group-writable
	# /etc/dropbear doesn't make dropbear reject the dir and lock SSH out.
	find files -type d -exec chmod 755 {} +
	find files -type f -exec chmod 644 {} +
	grep -rlIZ '^#!' files 2>/dev/null | xargs -0 -r chmod 755
	[ -f files/etc/dropbear/authorized_keys ] && chmod 600 files/etc/dropbear/authorized_keys
fi

# NOTE: feeds are intentionally NOT updated. This old fork's default feeds
# resolve to incompatible CURRENT package versions, and the minimal initramfs
# image needs only in-tree packages (rtw88/mac80211, wpad, iw). Running
# ./scripts/feeds update -a here would break the build.

# Seed config: realtek/rtl8197f, GWR1200AC-V1 profile (the RTL8197F reference
# board the DIR-842 rides), initramfs (RAM-boot) + rtw88 WiFi packages.
cat > .config <<'EOF'
CONFIG_TARGET_realtek=y
CONFIG_TARGET_realtek_rtl8197f=y
CONFIG_TARGET_realtek_rtl8197f_DEVICE_GWR1200AC-V1=y
CONFIG_TARGET_ROOTFS_INITRAMFS=y
# CONFIG_TARGET_ROOTFS_SQUASHFS is not set
CONFIG_PACKAGE_kmod-mac80211=y
CONFIG_PACKAGE_kmod-cfg80211=y
CONFIG_PACKAGE_kmod-rtw88=y
CONFIG_PACKAGE_rtl8822be-firmware=y
CONFIG_PACKAGE_wpad-basic=y
# CONFIG_PACKAGE_wpad-mini is not set
CONFIG_PACKAGE_iw=y
CONFIG_DRIVER_11AC_SUPPORT=y
CONFIG_DRIVER_11N_SUPPORT=y
CONFIG_DRIVER_11W_SUPPORT=y
EOF

make defconfig
make -j"$(nproc)"

echo
echo "Build complete. RAM-boot image is in:"
echo "  $(pwd)/bin/targets/realtek/rtl8197f/"
echo "  - *-GWR1200AC-V1-initramfs-kernel.bin   (serial XMODEM RAM-boot; see README)"
