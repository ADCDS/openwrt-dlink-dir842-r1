#!/usr/bin/env bash
# Copy the rtl819x target tree (and the generic additions we own) from the working
# OpenWrt checkout back into files/, so edits made in-tree (quilt refresh, kernel_menuconfig)
# become part of the recipe. Run from the repo root, then review `git status`.
set -e
cd "$(dirname "$0")/.."
[ -d openwrt/target/linux/rtl819x ] || { echo "no openwrt/target/linux/rtl819x" >&2; exit 1; }
rsync -a --delete --exclude 'ref/' openwrt/target/linux/rtl819x/ files/target/linux/rtl819x/
for f in target/linux/generic/files/drivers/mtd/mtdsplit/Kconfig \
         target/linux/generic/files/drivers/mtd/mtdsplit/Makefile \
         target/linux/generic/files/drivers/mtd/mtdsplit/mtdsplit_cvimg.c \
         package/kernel/mac80211/patches/rtl \
         tools/firmware-utils/patches; do
	[ -e "openwrt/$f" ] && rsync -a "openwrt/$f" "files/$(dirname "$f")/"
done
git status --short files | head -40
