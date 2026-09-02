#!/usr/bin/env bash
# Build OpenWrt (main branch, Linux 6.18) for the D-Link DIR-842 rev R1 (RTL8197F).
#
# This repo is a build recipe: it clones openwrt/openwrt at a pinned commit into
# ./openwrt, overlays ./files/ (the rtl819x target + generic additions, path-preserving),
# seeds .config and builds. Images land in openwrt/bin/targets/rtl819x/rtl8197f/.
#
#   ./build.sh                       # release seed (seed.config)
#   SEED=seed-min.config ./build.sh  # bring-up seed: no wireless, no LuCI
#   PROFILE=~/dir842-profile ./build.sh   # ...plus a private pre-configured profile
#
# Iterating: edit inside ./openwrt (quilt, make kernel_menuconfig), then run
# tools/overlay-sync.sh to copy the target tree back into files/ before committing.
set -e
cd "$(dirname "$0")"
SELF_DIR="$(pwd)"

# Upstream pin. Re-pin deliberately; never let this float.
OPENWRT_COMMIT=928bb37350d20a2a7737e3a3b5329261572ca533

if [ ! -e openwrt ]; then
	git clone https://github.com/openwrt/openwrt openwrt
fi
cd openwrt
git checkout -q "$OPENWRT_COMMIT"

# Overlay the rtl819x target + generic additions (path-preserving).
cp -a ../files/. .

# ---- optional: private profile overlay (PROFILE=/path/to/profile) ----
if [ -n "${PROFILE:-}" ]; then
	[ -d "$PROFILE/files" ] || { echo "ERROR: PROFILE=$PROFILE has no files/ dir." >&2; exit 1; }
	echo ">>> PROFILE=$PROFILE: baking private profile config into the image"
	mkdir -p files
	cp -a "$PROFILE"/files/. files/
	find files -type d -exec chmod 755 {} +
	find files -type f -exec chmod 644 {} +
	grep -rlIZ '^#!' files 2>/dev/null | xargs -0 -r chmod 755
	[ -f files/etc/dropbear/authorized_keys ] && chmod 600 files/etc/dropbear/authorized_keys
fi

./scripts/feeds update -a
./scripts/feeds install luci luci-app-upnp iperf3 tcpdump-mini wireless-tools conntrack

cp "$SELF_DIR/${SEED:-seed.config}" .config
make defconfig
make -j"$(nproc)"

echo
echo "Build complete. Images are in:"
echo "  $(pwd)/bin/targets/rtl819x/rtl8197f/"
echo "  - *-dlink_dir-842-r1-initramfs-kernel.bin    (RAM boot via the loader's TFTP; never writes flash)"
echo "  - *-dlink_dir-842-r1-squashfs-factory.bin    (NOR flash via the loader's AUTOBURN 1 or D-Link's web UI)"
echo "  - *-dlink_dir-842-r1-squashfs-sysupgrade.bin (NOR flash via sysupgrade -n)"
echo
echo "⚠ Both squashfs images REPLACE stock firmware and are already loader-signed."
