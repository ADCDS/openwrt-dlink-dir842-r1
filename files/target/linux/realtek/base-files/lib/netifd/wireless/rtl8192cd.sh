#!/bin/sh
. /lib/functions.sh
. /lib/netifd/netifd-wireless.sh

# ★ MANDATORY, and its absence is silent. netifd-wireless.sh ships a NO-OP stub
#   `add_driver() { return; }` (netifd-wireless.sh:12). The real implementation is
#   installed only by init_wireless_driver() (netifd-wireless.sh:367), which each
#   handler must call with "$@". Without this line the `add_driver rtl8192cd` at the
#   bottom of this file expands to the stub, so netifd's driver probe
#   (`popen("./rtl8192cd.sh '' dump")`, handler.c:96) reads ZERO bytes, no JSON is
#   parsed, wireless_add_handler() is never reached, and the driver is never
#   registered. config_parse_wireless_device() (config.c:332) then fails its
#   avl_find_element() lookup for type='rtl8192cd' and `return`s WITHOUT LOGGING
#   ANYTHING -- radio1 simply never becomes a wireless device, so `wifi up`,
#   `wifi down`, and `ubus call network.wireless status` all ignore it entirely and
#   wlan0 is never brought up. mac80211.sh:6 has this same call.
init_wireless_driver "$@"

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
	# 802.11r. Same UCI option names OpenWrt's own mac80211/hostapd 802.11r
	# support uses, so a wifi-iface in a roaming domain reads identically
	# whether this radio or a mac80211 one is the other end. They map onto the
	# vendor MIB keys in rtl8192cd_emit_vif (ft_enable/ft_mdid/...), which the
	# driver exposes via its set_mib table (8192cd_ioctl.c:1851-1861).
	config_add_boolean ieee80211r ft_over_ds
	config_add_string mobility_domain nasid
	config_add_int reassociation_deadline
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
	local ieee80211r mobility_domain ft_over_ds reassociation_deadline nasid

	# for_each_interface() has already json_select'ed this interface object.
	# On first setup there is no "data" section yet, so ifname comes from
	# "config" (it is a plain UCI option on the wifi-iface).
	json_select config
	json_get_vars mode ssid encryption key hidden maxassoc ifname
	json_get_vars ieee80211r mobility_domain ft_over_ds reassociation_deadline nasid
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
		# ── 802.11r ──────────────────────────────────────────────────────────
		# EMITTED BEFORE rtl_emit_encryption ON PURPOSE. The driver builds its
		# advertised RSN IE when the encryption MIBs are applied; if ft_enable
		# arrives after that, the IE can already be finalised without the
		# FT-PSK AKM (00-0F-AC:4) and clients never attempt fast transition.
		# Read the result back with:  cat /proc/wlan0/mib_auth  -> "rsnie:".
		#
		# ft_mdid is BYTE_ARRAY_T: the MIB parser reads it as a HEX STRING,
		# 2 chars per byte (8192cd_ioctl.c:3641 get_array_val), so a
		# mobility_domain like 'b1a0' passes through verbatim and means the
		# same two bytes hostapd puts in the MDIE. Do not "helpfully"
		# 0x-prefix it.
		if [ "$ieee80211r" = "1" ]; then
			echo "${ifname}_ft_enable=1"
			[ -n "$mobility_domain" ] && echo "${ifname}_ft_mdid=$mobility_domain"
			echo "${ifname}_ft_over_ds=${ft_over_ds:-0}"
			[ -n "$reassociation_deadline" ] && \
				echo "${ifname}_ft_reasoc_timeout=$reassociation_deadline"
			# R0 key holder id. Unique per radio, like nas_identifier upstream.
			[ -n "$nasid" ] && echo "${ifname}_ft_r0kh_id=$nasid"
		fi
		rtl_emit_encryption "$ifname" "$encryption" "$key"
	} >> "$CFGFILE" || {
		wireless_setup_vif_failed UNSUPPORTED_ENCRYPTION
		return 1
	}

	_rtl_vifs="$_rtl_vifs $name:$ifname"
	return 0
}


# ── Resolve the vendor radio's netdev by IDENTITY, never by name ──────────────
# Which wlanN this driver gets depends on module load order relative to rtw88.
# MEASURED on this board: rtw88's 5 GHz AP takes **wlan1** and the vendor root
# device takes **wlan0** — the OPPOSITE of stock's convention. Hardcoding a name
# is therefore actively dangerous: guessing wrong makes netifd down and
# reconfigure rtw88's interface, silently killing the working 5 GHz AP.
#
# The vendor driver is uniquely identifiable by its procfs — only it creates
# /proc/<ifname>/mib_all (8192cd_proc.c: proc_mkdir(dev->name) then
# an unconditional "mib_all" entry). rtw88 creates no /proc/wlanN at all. An explicit uci 'phy' is honoured, but only if it really
# is a vendor radio; otherwise we search. If nothing matches we return the
# requested name unchanged so the caller reports NO_DEVICE rather than guessing.
rtl_find_phy() {
	local want="$1" i
	[ -n "$want" ] && [ -e "/proc/$want/mib_all" ] && {
		echo "$want"; return 0
	}
	for i in /sys/class/net/wlan*; do
		[ -e "$i" ] || continue
		i="${i##*/}"
		# skip VAPs: the driver proc_mkdir()s one dir per netdev, so virtual
		# interfaces (wlan0-va0, wlan0-vxd) match mib_all too. We want the root
		# radio. Glob order happens to put the root first, but don't rely on it.
		case "$i" in *-*) continue ;; esac
		[ -e "/proc/$i/mib_all" ] && { echo "$i"; return 0; }
	done
	echo "${want:-wlan0}"
}

drv_rtl8192cd_setup() {
	local phy_ifname _rtl_vifs=""
	local channel htmode country band phy macaddr
	local beacon_int dtim_period v

	json_select config
	json_get_vars channel htmode country band phy macaddr beacon_int dtim_period
	json_select ..

	# 'phy' here is the vendor netdev, not a cfg80211 phy. Resolved by identity
	# (see rtl_find_phy) because the name depends on load order and rtw88 takes
	# wlan1 on this board.
	phy_ifname="$(rtl_find_phy "$phy")"

	# Deliberately NOT just [ -d /sys/class/net/$phy_ifname ]: if the vendor
	# module is not loaded, rtl_find_phy falls back to the requested name, and a
	# bare netdev test would then happily pass on rtw88's interface and push
	# vendor MIBs into the 5 GHz AP. Require the vendor procfs so a missing
	# driver reports NO_DEVICE instead of clobbering the other radio.
	[ -e "/proc/$phy_ifname/mib_all" ] || {
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

	phy_ifname="$(rtl_find_phy "$phy")"
	# same identity check as setup -- never down an interface that is not ours
	[ -e "/proc/$phy_ifname/mib_all" ] && $IFCONFIG "$phy_ifname" down
	rm -f "$CFGFILE"

	return 0
}

add_driver rtl8192cd
