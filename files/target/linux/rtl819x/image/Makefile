# SPDX-License-Identifier: GPL-2.0-only
#
# D-Link DIR-842 R1 (RTL8197F) images.
#
# Boot chain: the D-Link v3.4.11B loader finds a cvimg header with signature 'cr6b' at
# flash offset 0x40000, verifies the keyed-MD5 trailer, copies the payload to
# CVIMG_LOADER_START_ADDR (0x81000000) and jumps there. The payload is our lzma-loader
# (linked at LZMA_TEXT_START = 0x81000000) which decompresses vmlinux.bin+dtb to
# KERNEL_LOADADDR and jumps to it. The MIPS generic kernel links at 0x80100000.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

KERNEL_LOADADDR := 0x80100000
LZMA_TEXT_START := 0x81000000

DEVICE_VARS += CVIMG_KERNEL_BURN_ADDR CVIMG_LOADER_START_ADDR CMDLINE

define Build/loader-common
	rm -rf $@.src
	$(MAKE) -C lzma-loader \
		PKG_BUILD_DIR="$@.src" \
		TARGET_DIR="$(dir $@)" LOADER_NAME="$(notdir $@)" \
		BOARD="$(DEVICE_NAME)" \
		SUBTARGET="$(SUBTARGET)" \
		LZMA_TEXT_START=$(LZMA_TEXT_START) LOADADDR=$(KERNEL_LOADADDR) \
		$(1) compile loader.bin
	mv "$@.bin" "$@"
	rm -rf $@.src
endef

define Build/loader-cmdline-compile
	$(call Build/loader-common,LOADER_DATA="$@" KERNEL_CMDLINE="$(CMDLINE)")
endef

define Build/cvimg
	$(STAGING_DIR_HOST)/bin/cvimg -i $@ -o $@.new $(1) && mv $@.new $@ || rm -f $@
endef

# The D-Link loader boots the 'cr6b' firmware signature (verified byte-identical to the
# stock header), not the 'cs6c' that '-c new -t kernel' would emit.
define Build/cvimg-kernel-cr6b
	$(call Build/cvimg, \
		-e $(CVIMG_LOADER_START_ADDR) \
		-b $(CVIMG_KERNEL_BURN_ADDR) \
		-s cr6b \
		$(1))
endef

# A 640-byte fake squashfs superblock (magic 'hsqs', ident 'FAKE') between kernel and
# rootfs; mtdsplit_cvimg keys on it to find the real rootfs.
define Build/append-fake-rootfs
	$(call Build/cvimg, -a $(BLOCKSIZE) -f)
endef

define Build/cvimg-pad-rootfs
	$(call Build/cvimg, \
		-b $(CVIMG_KERNEL_BURN_ADDR) \
		-c new \
		-a $(BLOCKSIZE) \
		-t rootfs \
		-j)
endef

# Append the D-Link boot signature the loader validates after finding the cr6b image:
#   trailer = MD5( key16 || cvimg_image )[16] , then 0x00C0FFEE[4]
# key16 lives in the stock bootcode (decompressed mtd0 @0x291bc). Reverse-engineered
# and verified byte-identical to the stock trailer (docs/BENCH.md "The signing step").
define Build/dlink-md5-sign
	{ printf '\x8c\xef\xeb\x7b\x3b\x27\x46\x29\xbe\x7d\xf7\xd4\x53\xa6\x4c\x29'; cat $@; } | \
		md5sum | cut -c1-32 | perl -ne 'chomp; print pack("H*", $$_)' > $@.dgst
	cat $@.dgst >> $@
	printf '\x00\xc0\xff\xee' >> $@
	rm -f $@.dgst
endef

define Device/Default
	PROFILES := Default
	DEVICE_DTS_DIR := ../dts
	DEVICE_DTS = $$(SOC)_$(1)
	BLOCKSIZE := 64k
	CMDLINE := console=ttyS0,38400
	KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
	KERNEL_INITRAMFS := $$(KERNEL)
	IMAGES := sysupgrade.bin factory.bin
endef

define Device/dlink_dir-842-r1
	SOC := rtl8197f
	DEVICE_VENDOR := D-Link
	DEVICE_MODEL := DIR-842
	DEVICE_VARIANT := R1
	# stock flash map: firmware region at 0x40000, 7872k, booted via the cr6b signature
	IMAGE_SIZE := 7872k
	CVIMG_KERNEL_BURN_ADDR := 0x40000
	CVIMG_LOADER_START_ADDR := 0x81000000
	# v1.x images of this port identified as the Greatek GWR1200AC-V1 profile
	SUPPORTED_DEVICES += gwr1200ac-v1
	DEVICE_COMPAT_VERSION := 2.0
	DEVICE_COMPAT_MESSAGE := Config cannot be migrated from swconfig to DSA \
		(eth0.1/eth0.2 became wan/lan1..lan4); sysupgrade with -n
	# Radio packages are added back when PCIe and the 2.4 GHz WMAC are enabled;
	# rtw88 cannot even link without PCI in the kernel.
	DEVICE_PACKAGES := kmod-leds-gpio kmod-gpio-button-hotplug iperf3
	IMAGE/factory.bin := append-kernel | append-fake-rootfs | append-rootfs | \
		cvimg-kernel-cr6b | cvimg-pad-rootfs | dlink-md5-sign | check-size
	# sysupgrade MUST sign BEFORE pad-rootfs: pad-rootfs appends the jffs2 'deadc0de'
	# marker and pads to BLOCKSIZE, so a trailer appended after it lands in rootfs_data's
	# first erase block, which mount_root erases on first boot -> the loader then fails
	# "magic not found" on every later power-on (soft-brick on the SECOND boot, found by
	# the cold-boot gate 2026-08-16). Signed first, the trailer sits in the squashfs slack.
	IMAGE/sysupgrade.bin := append-kernel | append-fake-rootfs | append-rootfs | \
		cvimg-kernel-cr6b | dlink-md5-sign | pad-rootfs | check-size | append-metadata
endef
TARGET_DEVICES += dlink_dir-842-r1

$(eval $(call BuildImage))
