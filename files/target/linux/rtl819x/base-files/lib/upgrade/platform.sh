#
# Copyright (C) 2011 OpenWrt.org
#

. /lib/functions.sh
. /lib/functions/system.sh

PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1

platform_check_image() {
	local board=$(board_name)

	case "$board" in
	dlink,dir-842-r1|gwr1200ac-v1)
		# The on-flash firmware is a Realtek "cr6b" image: a 16-byte header
		# the v3.4.11B loader validates before booting. Reject anything
		# without that signature so a wrong-target image cannot be written.
		# The header sits at offset 0 even when sysupgrade metadata is
		# appended, because that rides at the tail.
		#
		# ★ Both names are matched on purpose. This target calls the board
		# dlink,dir-842-r1; v1.x images identified as gwr1200ac-v1 and
		# SUPPORTED_DEVICES still carries it, so an upgrade started from one
		# of those must be checked too. Matching only the old name -- which
		# is what this file did -- silently skipped the check on every
		# image this target builds.
		local magic
		magic=$(get_magic_long "$1")
		if [ "$magic" != "63723662" ]; then
			echo "Invalid image: expected cr6b firmware header, got 0x$magic"
			return 1
		fi
		return 0
		;;
	esac

	return 0
}

platform_do_upgrade() {
	# Writes the image to the "firmware" mtd (PART_NAME). The per-device MAC
	# and RF calibration live in separate read-only partitions (boot/MAC) and
	# are never touched; config is preserved by the sysupgrade framework.
	default_do_upgrade "$1"
}
