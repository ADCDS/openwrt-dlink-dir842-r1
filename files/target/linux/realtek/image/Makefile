#
# Copyright (C) 2006-2010 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DEVICE_VARS += IMAGE_SIZE CVIMG_KERNEL_BURN_ADDR CVIMG_LOADER_START_ADDR CVIMG_ROOTFS_BURN_ADDR

define Build/loader-common
  rm -rf $@.src
  $(MAKE) -C lzma-loader \
     PKG_BUILD_DIR="$@.src" \
     TARGET_DIR="$(dir $@)" LOADER_NAME="$(notdir $@)" \
     BOARD="$(DEV_PROFILE)" \
     SUBTARGET="$(SUBTARGET)" \
     LZMA_TEXT_START=0x81000000 LOADADDR=0x80000000 \
     $(1) compile loader.bin
  mv "$@.bin" "$@"
  rm -rf $@.src
endef

define Build/loader-cmdline-compile
  $(call Build/loader-common,LOADER_DATA="$@" KERNEL_CMDLINE="$(CMDLINE)")
endef

define Build/cvimg
        $(STAGING_DIR_HOST)/bin/cvimg \
                -i $@ \
                -o $@.new \
                $(1) && mv $@.new $@ || rm -f $@
endef

define Build/cvimg-kernel
        $(call Build/cvimg, \
                -e $(CVIMG_LOADER_START_ADDR) \
                -b $(CVIMG_KERNEL_BURN_ADDR) \
                -c new \
                -t kernel \
                $(1))
endef

# DIR-842 R1: the D-Link v3.4.11B loader boots the 'cr6b' firmware signature
# (verified byte-identical to the stock mtd3 header), not the 'cs6c' that
# '-c new -t kernel' would emit.
define Build/cvimg-kernel-cr6b
        $(call Build/cvimg, \
                -e $(CVIMG_LOADER_START_ADDR) \
                -b $(CVIMG_KERNEL_BURN_ADDR) \
                -s cr6b \
                $(1))
endef

# DIR-842 R1: append the D-Link boot signature the v3.4.11B loader validates
# after finding the cr6b image (else "magic not found" / no boot). The check is:
#   trailer = MD5( key16 || cvimg_image )[16] , then 0x00C0FFEE[4]
# where key16 is embedded in the stock bootcode (mtd0 @0x291bc). Reverse-
# engineered from the bootcode; verified byte-identical to the stock trailer.
define Build/dlink-md5-sign
	{ printf '\x8c\xef\xeb\x7b\x3b\x27\x46\x29\xbe\x7d\xf7\xd4\x53\xa6\x4c\x29'; cat $@; } | \
		md5sum | cut -c1-32 | perl -ne 'chomp; print pack("H*", $$_)' > $@.dgst
	cat $@.dgst >> $@
	printf '\x00\xc0\xff\xee' >> $@
	rm -f $@.dgst
endef

define Build/cvimg-fw
        $(call Build/cvimg, \
                -e $(CVIMG_LOADER_START_ADDR) \
                -b $(CVIMG_KERNEL_BURN_ADDR) \
                -c new \
                -t fw \
                $(1))
endef

define Build/cvimg-custom
        $(call Build/cvimg, \
                -e $(CVIMG_LOADER_START_ADDR) \
                -b $(CVIMG_KERNEL_BURN_ADDR) \
                -s $(CVIMG_CUSTOM_SIGNATURE) \
                $(1))
endef

define Build/cvimg-rootfs
        $(call Build/cvimg, \
                -e $(CVIMG_ROOTFS_BURN_ADDR) \
                -b $(CVIMG_ROOTFS_BURN_ADDR) \
                -c new \
                -t rootfs \
                $(1))
endef

define Build/append-cvimg-rootfs
        cp $(IMAGE_ROOTFS) $(IMAGE_ROOTFS).new && \
        $(STAGING_DIR_HOST)/bin/padjffs2 $(IMAGE_ROOTFS).new \
                $(if $(BLOCKSIZE),$(BLOCKSIZE:%k=%),4 8 16 64 128 256) && \
        $(STAGING_DIR_HOST)/bin/cvimg \
                -i $(IMAGE_ROOTFS).new \
                -o $(IMAGE_ROOTFS).new \
                -e $(CVIMG_ROOTFS_BURN_ADDR) \
                -b $(CVIMG_ROOTFS_BURN_ADDR) \
                -c new \
                -t rootfs \
                $(1) && (dd if=$(IMAGE_ROOTFS).new >> $@ && \
                        rm -f $(IMAGE_ROOTFS).new) || rm -f $@ $(IMAGE_ROOTFS).new
endef

define Build/append-fake-rootfs
        $(call Build/cvimg, \
                -a $(BLOCKSIZE) \
                -f)
endef

define Build/cvimg-pad-rootfs
        $(call Build/cvimg, \
                -b $(CVIMG_KERNEL_BURN_ADDR) \
                -c new \
                -a $(BLOCKSIZE) \
                -t rootfs \
                -j)
endef

define Build/tbs_dlink_big
  $(STAGING_DIR_HOST)/bin/tbs_dlink -b $(1) $@
endef

define Device/Default
  BLOCKSIZE := 4k
  PROFILES := Default $$(DEV_PROFILE)
  CVIMG_ROOTFS_BURN_ADDR := 0
endef

define Device/RealtekDTS
  PROFILES = Default $$(DTS)
  DEVICE_DTS_DIR := ../dts
  KERNEL := kernel-bin | append-dtb | lzma
endef

ifeq ($(SUBTARGET),rtl8197d)
define Device/DIR815D1
  $(Device/RealtekDTS)
  DEVICE_DTS := DIR815D1
  DEV_PROFILE := DIR815D1
  IMAGE_SIZE := 7936k
  DEVICE_TITLE := D-Link DIR-815 D1
  SUPPORTED_DEVICES := dir-815-d1
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin := append-kernel | pad-to 1900544 | append-rootfs | pad-rootfs | \
	tbs_dlink_big -k 0x197400 -r 0x4DFF70 -g BR -m DIR825G1 -p RTL8197DL_AC1200
  IMAGE/sysupgrade.bin := append-kernel | pad-to 1900544 | append-rootfs | \
  pad-rootfs | append-metadata
endef
TARGET_DEVICES += DIR815D1

define Device/DIR822C1
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_DTS := DIR822C1
  DEVICE_TITLE := D-Link DIR-822 C1
  SUPPORTED_DEVICES := dir-822-c1
  IMAGE_SIZE := 7936k
  DEV_PROFILE := DIR822C1
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef
TARGET_DEVICES += DIR822C1

endif

ifeq ($(SUBTARGET),rtl8197f)
define Device/RE708
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_DTS := RE708
  DEVICE_TITLE := Multilaser RE708 V1
  SUPPORTED_DEVICES := re708v1 re708-v1
  IMAGE_SIZE := 7936k
  DEV_PROFILE := RE708
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef
TARGET_DEVICES += RE708

define Device/GWR1200AC
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_TITLE := Greatek GWR1200AC V1
  SUPPORTED_DEVICES := gwr1200ac-v1
  IMAGE_SIZE := 7936k
  DEV_PROFILE := GWR1200AC
  CMDLINE := console=ttyS0,38400 ipv6.disable=1
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x80A00000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef

define Device/GWR1200AC-V1
  $(Device/GWR1200AC)
  DEVICE_DTS := GWR1200ACV1
  DEVICE_TITLE := Greatek GWR1200AC V1
  SUPPORTED_DEVICES := gwr1200ac-v1
  # This profile is the D-Link DIR-842 R1 (RTL8197F) masquerading as
  # GWR1200AC-V1. Its flash geometry is the DIR-842 stock map: kernel+rootfs
  # firmware region at 0x40000 (7872k), booted via the cr6b signature.
  IMAGE_SIZE := 7872k
  CVIMG_KERNEL_BURN_ADDR := 0x40000
  # entry = the lzma-loader link addr (LZMA_TEXT_START, image/Makefile:19), NOT
  # the Greatek 0x80a00000; the loader loads to start_addr and jumps there.
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel-cr6b | cvimg-pad-rootfs | dlink-md5-sign
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel-cr6b | pad-rootfs | dlink-md5-sign
  # R3: declare the board's package set per-device instead of relying on a
  # hand-maintained seed config — a clean `make menuconfig` for this profile now
  # yields a working router (WiFi, PPPoE, LEDs, switch, diagnostics) instead of an
  # image with no wireless at all.
  DEVICE_PACKAGES := kmod-rtw88 rtl8822be-firmware wpad-basic iw \
	kmod-leds-gpio kmod-gpio-button-hotplug swconfig \
	ppp ppp-mod-pppoe kmod-nf-flow kmod-ipt-offload iperf3
  # R3: hwnat deliberately NOT on the cmdline — measured, it breaks the datapath when
  # armed before the ASIC tables are warm (100% loss; recovers immediately on
  # `echo 0 > .../parameters/hwnat` + fabric_reset). Arm offload after bring-up instead.
  # R5: ipv6.disable=1 dropped — the v6 packages (odhcpd/odhcp6c/ip6tables) were
  # already shipped but dead weight while the kernel had v6 compiled out at boot.
  CMDLINE := console=ttyS0,38400
endef
TARGET_DEVICES += GWR1200AC-V1

define Device/GWR1200AC-V2
  $(Device/GWR1200AC)
  DEVICE_DTS := GWR1200ACV2
  DEVICE_TITLE := Greatek GWR1200AC V2
  SUPPORTED_DEVICES := gwr1200ac-v2
endef
TARGET_DEVICES += GWR1200AC-V2

define Device/ACTIONRG1200
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_DTS := ACTIONRG1200
  DEVICE_TITLE := Intelbras ACTIONRG1200 V1
  SUPPORTED_DEVICES := actionrg1200v1
  IMAGE_SIZE := 7872k
  DEV_PROFILE := ACTIONRG1200
  CMDLINE := console=ttyS0,115200
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x80A00000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | pad-offset 64k 16 | append-rootfs | pad-rootfs -x 16 | cvimg-kernel -T
  IMAGE/sysupgrade.bin = append-kernel | pad-offset 64k 16 | append-rootfs | pad-rootfs -x 16 | cvimg-kernel -S
endef
TARGET_DEVICES += ACTIONRG1200

define Device/ACTIONRF1200
  $(Device/RealtekDTS)
  BLOCKSIZE := 64k
  DEVICE_DTS := ACTIONRF1200
  DEVICE_TITLE := Intelbras ACTIONRF1200 V1
  SUPPORTED_DEVICES := actionrf1200v1
  IMAGE_SIZE := 7872k
  DEV_PROFILE := ACTIONRF1200
  CMDLINE := console=ttyS0,115200
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x80A00000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | pad-offset 64k 16 | append-rootfs | pad-rootfs -x 16 | cvimg-kernel -T
  IMAGE/sysupgrade.bin = append-kernel | pad-offset 64k 16 | append-rootfs | pad-rootfs -x 16 | cvimg-kernel -S
endef
TARGET_DEVICES += ACTIONRF1200
endif

ifeq ($(SUBTARGET),rtl8196e)
define Device/GWR300N
  $(Device/RealtekDTS)
  DEVICE_DTS := GWR300
  DEVICE_TITLE := Greatek GWR 300N
  SUPPORTED_DEVICES := gwr300v1 gwr300-v1
  IMAGE_SIZE := 3904k
  DEV_PROFILE := GWR300N
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x18000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef
TARGET_DEVICES += GWR300N

define Device/RE172
  $(Device/RealtekDTS)
  DEVICE_DTS := RE172
  DEVICE_TITLE := Multilaser RE172 V1
  SUPPORTED_DEVICES := re172v1 re172-v1
  IMAGE_SIZE := 4032k
  DEV_PROFILE := RE172
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x10000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef
TARGET_DEVICES += RE172

define Device/RE707
  $(Device/RealtekDTS)
  DEVICE_DTS := RE707
  DEVICE_TITLE := Multilaser RE707 V1
  SUPPORTED_DEVICES := re707v1 re707-v1
  IMAGE_SIZE := 3904k
  DEV_PROFILE := RE707
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile
  CVIMG_KERNEL_BURN_ADDR := 0x20000
  CVIMG_LOADER_START_ADDR := 0x81000000
  IMAGES := factory.bin sysupgrade.bin
  IMAGE/factory.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | cvimg-pad-rootfs
  IMAGE/sysupgrade.bin = append-kernel | append-fake-rootfs | append-rootfs | cvimg-kernel | pad-rootfs
endef
TARGET_DEVICES += RE707

endif

define Image/Build/Initramfs
        $(call Image/Build/Profile/$(PROFILE),initramfs)
endef

$(eval $(call BuildImage))
