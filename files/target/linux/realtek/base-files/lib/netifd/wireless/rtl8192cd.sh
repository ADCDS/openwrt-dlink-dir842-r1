#!/bin/sh
. /lib/functions.sh
. /lib/netifd/netifd-wireless.sh

# netifd wireless handler for the Realtek vendor rtl8192cd driver (WEXT build,
# i.e. built WITHOUT -DRTK_NL80211).
#
# Why this exists: in its WEXT configuration rtl8192cd registers no cfg80211
# wiphy, so /lib/netifd/wireless/mac80211.sh cannot see or drive it, and
# hostapd has no WEXT AP backend (hostapd/Makefile references only
# driver_nl80211 / driver_hostap / driver_atheros / driver_bsd / driver_wired;
# driver_wext.c is wpa_supplicant-only). The WPA2-PSK 4-way handshake
# therefore runs IN-KERNEL (8192cd_psk.c, enabled by INCLUDE_WPA_PSK, which is
# #undef'd only under RTK_NL80211).
#
# Configuration mechanism: rather than issuing ~20 separate `iwpriv set_mib`
# round-trips, this writes the driver's own config file and lets the driver
# ingest it atomically. CONFIRMED in-tree:
#   8192cd_comapi.c:3431  #define CFG_FILE_PATH "/etc/Wireless/RTL8192CD.dat"
#   8192cd_comapi.c:3436  int CfgFileProc(struct net_device *dev)
#   8192cd_osdep.c:7703   CfgFileProc(dev) -- runs for the root dev on open
#   8192cd_ioctl.c:589    { SIOCCOMAPIFILE, ..., "cfgfile" } -- reload on demand
# Each line is "<ifname>_<mib>=<value>" and is applied as a set_mib. This is
# byte-for-byte the mechanism D-Link's stock firmware uses (its libdhal.so
# writes the same file, and /etc/Wireless is a symlink to /tmp so it lands in
# tmpfs rather than causing a flash write).
#
# The 5 GHz RTL8822BE stays on mac80211.sh + hostapd. netifd runs one handler
# per wifi-device, so the two are fully independent.
#
# MIB names verified against 8192cd_ioctl.c:
#   channel:1195  ssid:1282  regdomain:1296  authtype:1386  encmode:1387
#   psk_enable:1389  wpa_cipher:1391  wpa2_cipher:1393  passphrase:1395
#   gk_rekey:1396  opmode:1414 (WIFI_AP_STATE=0x10, 8192cd.h:757)
#   hiddenAP:1415  band:1481  use40M:1730
#   phyBandSelect, 2ndchoffset, dtimperiod, bcnint, stanum, dot11IEEE80211W

IFCONFIG=/sbin/ifconfig
CFGDIR=/etc/Wireless
CFGFILE="$CFGDIR/RTL8192CD.dat"

drv_rtl8192cd_init_device_config() {
	config_add_string path phy 'macaddr:macaddr'
	config_add_string country
	config_add_int band            # 1=11b 2=11g 11=b/g/n 4=11a 12=a/n 76=ac/a/n
	config_add_int beacon_int dtim_period
	config_add_boolean noscan
}

drv_rtl8192cd_init_iface_config() {
	config_add_string ifname
	config_add_boolean hidden isolate
	config_add_int maxassoc
}

drv_rtl8192cd_cleanup() {
	return 0
}

# UCI htmode -> use40M (0=20MHz, 1=40MHz, 2=auto 20/40)
rtl_htmode_to_use40m() {
	case "$1" in
		HT40*|VHT40*) echo 1 ;;
		HT20|VHT20|NONE|"") echo 0 ;;
		*) echo 2 ;;
	esac
}

# HT40+/HT40- -> 2ndchoffset (1=above, 2=below, 0=auto)
rtl_htmode_to_offset() {
	case "$1" in
		HT40+) echo 1 ;;
		HT40-) echo 2 ;;
		*) echo 0 ;;
	esac
}

# UCI encryption -> .dat security lines.
# encmode: 0=none 1=wep40 5=wep104 2=TKIP 4=CCMP 6=mixed (dot11PrivacyAlgrthm)
# psk_enable bitmask: 1=WPA 2=WPA2 3=WPA+WPA2. (8=WPA3 exists only in newer
#   vendor drops -- this driver vintage has no wpa3/ tree and no wpa3_cipher
#   MIB, so SAE is NOT available on this radio.)
# wpa_cipher/wpa2_cipher: 2=TKIP 8=CCMP 10=both
rtl_emit_encryption() {
	local p="$1" enc="$2" key="$3"

	case "$enc" in
		none)
			echo "${p}_authtype=0"
			echo "${p}_encmode=0"
			echo "${p}_psk_enable=0"
			;;
		psk2*|wpa2*)
			echo "${p}_authtype=0"
			echo "${p}_encmode=4"
			echo "${p}_psk_enable=2"
			echo "${p}_wpa2_cipher=8"
			echo "${p}_passphrase=\"$key\""
			;;
		psk-mixed*|wpa-mixed*)
			echo "${p}_authtype=0"
			echo "${p}_encmode=6"
			echo "${p}_psk_enable=3"
			echo "${p}_wpa_cipher=10"
			echo "${p}_wpa2_cipher=10"
			echo "${p}_passphrase=\"$key\""
			;;
		psk*|wpa*)
			echo "${p}_authtype=0"
			echo "${p}_encmode=2"
			echo "${p}_psk_enable=1"
			echo "${p}_wpa_cipher=2"
			echo "${p}_passphrase=\"$key\""
			;;
		*)
			return 1
			;;
	esac
	return 0
}

rtl8192cd_emit_vif() {
	local name="$1"
	local mode ssid encryption key hidden maxassoc ifname

	# for_each_interface() has already json_select'ed this interface object.
	# On first setup there is no "data" section yet, so ifname comes from
	# "config" (it is a plain UCI option on the wifi-iface).
	json_select config
	json_get_vars mode ssid encryption key hidden maxassoc ifname
	json_select ..

	[ -n "$ifname" ] || ifname="$phy_ifname"

	[ "$mode" = "ap" ] || {
		wireless_setup_vif_failed UNSUPPORTED_MODE
		return 1
	}

	{
		echo "${ifname}_opmode=16"
		echo "${ifname}_ssid=\"$ssid\""
		[ -n "$hidden" ]   && echo "${ifname}_hiddenAP=$hidden"
		[ -n "$maxassoc" ] && echo "${ifname}_stanum=$maxassoc"
		rtl_emit_encryption "$ifname" "$encryption" "$key"
	} >> "$CFGFILE" || {
		wireless_setup_vif_failed UNSUPPORTED_ENCRYPTION
		return 1
	}

	_rtl_vifs="$_rtl_vifs $name:$ifname"
	return 0
}

drv_rtl8192cd_setup() {
	local phy_ifname _rtl_vifs=""
	local channel htmode country band phy macaddr
	local beacon_int dtim_period v

	json_select config
	json_get_vars channel htmode country band phy macaddr beacon_int dtim_period
	json_select ..

	# 'phy' here is the vendor netdev, not a cfg80211 phy. Default wlan1 so it
	# cannot race rtw88 for wlan0 -- which also matches stock's convention
	# (stock: wlan0 = 5 GHz RTL8822BE, wlan1 = 2.4 GHz on-SoC WMAC).
	phy_ifname="${phy:-wlan1}"

	[ -d "/sys/class/net/$phy_ifname" ] || {
		wireless_set_retry 0
		wireless_setup_failed NO_DEVICE
		return 1
	}

	# Stock symlinks /etc/Wireless -> /tmp so this never touches flash.
	[ -d "$CFGDIR" ] || mkdir -p "$CFGDIR"

	$IFCONFIG "$phy_ifname" down

	# Radio-level MIBs. phyBandSelect: 1 = 2.4 GHz, 2 = 5 GHz.
	{
		echo "${phy_ifname}_phyBandSelect=1"
		echo "${phy_ifname}_band=${band:-11}"
		[ -n "$country" ] && echo "${phy_ifname}_regdomain=$country"
		echo "${phy_ifname}_channel=${channel:-0}"
		echo "${phy_ifname}_use40M=$(rtl_htmode_to_use40m "$htmode")"
		echo "${phy_ifname}_2ndchoffset=$(rtl_htmode_to_offset "$htmode")"
		[ -n "$beacon_int" ]  && echo "${phy_ifname}_bcnint=$beacon_int"
		[ -n "$dtim_period" ] && echo "${phy_ifname}_dtimperiod=$dtim_period"
	} > "$CFGFILE"

	for_each_interface "ap" rtl8192cd_emit_vif

	[ -n "$macaddr" ] && $IFCONFIG "$phy_ifname" hw ether "$macaddr"

	# Bringing the root device up makes the driver read RTL8192CD.dat and apply
	# every line (8192cd_osdep.c:7703 -> CfgFileProc). Look for
	# "-------> Set MIB from /etc/Wireless/RTL8192CD.dat" in dmesg.
	$IFCONFIG "$phy_ifname" up || {
		wireless_setup_failed IFUP_FAILED
		return 1
	}

	for v in $_rtl_vifs; do
		wireless_add_vif "${v%%:*}" "${v##*:}"
	done

	wireless_set_up
}

drv_rtl8192cd_teardown() {
	local phy_ifname

	json_select config
	json_get_vars phy
	json_select ..

	phy_ifname="${phy:-wlan1}"
	[ -d "/sys/class/net/$phy_ifname" ] && $IFCONFIG "$phy_ifname" down
	rm -f "$CFGFILE"

	return 0
}

add_driver rtl8192cd
