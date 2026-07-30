#!/usr/bin/env bash
# Build mainline-style OpenWrt for the D-Link DIR-842 rev R1 (RealTek RTL8197F).
#
# The RTL819x/RTL8197F MIPS home-router platform is NOT in upstream OpenWrt
# (mainline's "realtek" target is the RTL838x/930x managed switches only). The
# only tree with RTL8197F target support is the ggbruno fork. This script
# overlays ./files/ (the DIR-842 device support + our fixes) onto a pinned
# ggbruno checkout and builds both a RAM-boot initramfs and a NOR squashfs image.
#
# NOTE: this used to be RAM-boot only. It is not anymore — M7.1 cracked the D-Link
# boot signature (a forgeable keyed-MD5) and R1 fixed the flash-boot crash, so this
# builds a squashfs image that boots from NOR and survives a power cycle (verified
# 10/10 consecutive unattended cold boots). ⚠ Flashing REPLACES stock firmware: back
# up all 8 MB of NOR first and keep it, because stock exists only in that backup.
# The initramfs image is still built and is still the safer way to iterate.
#
# Build environment: the ggbruno fork is from 2020 (kernel 4.14 / gcc 8.4). Use
# a Debian 11 (bullseye)-era build host or container; very new toolchains can
# fail to build the old host tools. See README.md for a container one-liner.
#
# Optional private profile (pre-configured gateway image):
#   PROFILE=~/dir842-profile ./build.sh
set -e
cd "$(dirname "$0")"
SELF_DIR="$(pwd)"

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

# Feeds: PINNED, and only the two we need are updated.
#
# The old advice here was "never update feeds" — true for the original initramfs image,
# which used in-tree packages only, but it also meant no web UI. The real constraint is
# that this fork's *default* feeds resolve to CURRENT (master) package versions, which do
# not build against a 19.07-era base: master's luci-base is ucode-based and dies on
# ucode-mod-html / liblucihttp-ucode, and master split miniupnpd into -iptables/-nftables
# so a plain `miniupnpd` dep no longer resolves.
#
# Pinning both feeds to the openwrt-19.07 branch fixes that (Lua-era LuCI, no ucode), so
# R5's LuCI + UPnP + QoS build cleanly. `feeds.conf` overrides feeds.conf.default wholly,
# and it is gitignored inside the OpenWrt tree, which is why it is shipped from here.
cp "$SELF_DIR/feeds.conf" feeds.conf
./scripts/feeds update luci packages
./scripts/feeds install luci-base luci-mod-admin-full luci-theme-bootstrap \
                        luci-app-firewall luci-app-upnp luci-app-opkg \
                        cgi-io miniupnpd qos-scripts

# Seed config: shipped from seed-m5.config so this script builds what the port
# actually is today (squashfs flash image + LuCI + PPPoE + offload diagnostics), not
# the historical minimal initramfs. Keep the seed as the single source of truth —
# an inline copy here drifted out of date once already.
# ★ Includes `# CONFIG_KERNEL_CRASHLOG is not set`, which is load-bearing for flash
# boot: OpenWrt's own crashlog handler faults on this SoC and recurses, masking the
# real oops. CONFIG_KERNEL_* is injected AFTER the target config merge, so the
# subtarget's "# CONFIG_CRASHLOG is not set" alone is silently overridden.
cp "$SELF_DIR/seed-m5.config" .config

make defconfig
make -j"$(nproc)"

echo
echo "Build complete. Images are in:"
echo "  $(pwd)/bin/targets/realtek/rtl8197f/"
echo "  - *-GWR1200AC-V1-initramfs-kernel.bin      (serial XMODEM RAM-boot; safest)"
echo "  - *-GWR1200AC-V1-squashfs-sysupgrade.bin   (NOR flash; REPLACES stock — back up NOR first)"
