#!/bin/bash
# flash-nor.sh — autonomously burn a cr6b factory image to NOR via the loader's AUTOBURN
# path, then (optionally) cold-boot from flash and capture the boot. No human needed:
# the smart plug does the power-cycling.
#
#   bash flash-nor.sh [factory.bin]        # burn only
#   BOOT=1 bash flash-nor.sh [factory.bin] # burn, then cold-boot from flash + capture
#
# Modeled on the proven ramboot.sh. Differences: AUTOBURN 1 (write flash) instead of
# AUTOBURN 0, and no `J` — after the burn we power-cycle and let the loader boot NOR.
#
# ★ Three gotchas this script handles, all learned the hard way:
#   1. The loader's own IP lives in nvram and resets to the 192.168.1.6 factory default;
#      our bench link is 192.168.0.0/24, so pin it with IPCONFIG every time.
#   2. NetworkManager strips the manual IPv4 off the host USB NIC, so curl silently
#      fails while the catcher keeps re-arming — looks like an endless <RealTek> loop.
#   3. The loader answers on the STOCK MAC (e0:1c:fc:51:c9:ef), Linux on
#      00:e0:4c:81:96:c2 — never pin a static ARP for 192.168.0.1.
# AUTOBURN writes the ENTIRE uploaded file, so only feed it a real factory image
# (kernel cr6b + rootfs + the D-Link MD5 trailer). It never touches mtd0 (boot).
set -u
PORT=/dev/ttyUSB0
LOG=/home/agiu/dir842-r1-bootlog.txt
FLAG=/tmp/flashnor-spam.flag
TOMADA="${TOMADA:-/home/agiu/.local/bin/tomada}"
IFACE="${IFACE:-eth1}"	# host NIC cabled to the DIR-842 LAN — override: IFACE=... ./flash-nor.sh
BIN_DIR="${BIN_DIR:-$(dirname "$(readlink -f "$0")")/openwrt/bin/targets/rtl819x/rtl8197f}"
IMG="${1:-$(ls $BIN_DIR/*squashfs-factory.bin | head -1)}"

sr() { stty -F "$PORT" 38400 cs8 -parenb -cstopb -crtscts -ixon clocal raw -echo 2>/dev/null; }
spam_stop() { rm -f "$FLAG"; sleep 0.3; }
trap 'spam_stop' EXIT
[ -f "$IMG" ] || { echo "no image: $IMG"; exit 2; }
sr
touch "$LOG"
# ★ Anchor the pattern to the logger process itself. A bare `cat $PORT` pattern
# also matches any shell whose command line merely MENTIONS the device node --
# including the one invoking this script -- so the guard skips starting the
# logger, the log stays empty, and every later `wc -c < $LOG` / catch test fails.
# That reads as "no loader window" on every round while nothing is actually
# wrong with the board. ramboot.sh was fixed for this; this script was not.
pgrep -f "^cat $PORT" >/dev/null || { setsid bash -c "exec cat $PORT >> $LOG" </dev/null >/dev/null 2>&1 & sleep 1; }
echo "[flash-nor] IMG=$IMG ($(stat -c%s "$IMG") bytes)"

host_net() {
  sudo nmcli device set "$IFACE" managed no 2>/dev/null
  sudo ip link set "$IFACE" up 2>/dev/null
  ip -4 addr show "$IFACE" | grep -q 192.168.0.2 || sudo ip addr add 192.168.0.2/24 dev "$IFACE" 2>/dev/null
  sudo ip neigh flush dev "$IFACE" 2>/dev/null
}

burned=0
for round in 1 2 3; do
  echo "=== round $round: power-cycle + catch loader ==="
  "$TOMADA" off >/dev/null 2>&1; sleep 3
  touch "$FLAG"
  ( while [ -f "$FLAG" ]; do printf '\033' > "$PORT" 2>/dev/null; sleep 0.04; done ) &
  START=$(wc -c < "$LOG")
  "$TOMADA" on >/dev/null 2>&1
  echo "  powered on $(date +%H:%M:%S), spamming ESC"
  caught=0; DL=$((SECONDS+60))
  while [ $SECONDS -lt $DL ]; do
    tail -c +"$START" "$LOG" 2>/dev/null | grep -aq "Escape booting by user" && { caught=1; break; }
    sleep 0.2
  done
  spam_stop
  [ $caught -eq 1 ] || { echo "  no loader window"; continue; }
  echo "  ★ caught loader"

  sleep 2; sr; host_net
  printf '\r' > "$PORT"; sleep 0.5
  printf 'IPCONFIG 192.168.0.1\r' > "$PORT"; sleep 1.5
  ping -c1 -W2 192.168.0.1 >/dev/null 2>&1 || echo "  !! loader unreachable at 192.168.0.1"

  MARK=$(wc -c < "$LOG")
  printf 'AUTOBURN 1\r' > "$PORT"; sleep 1
  printf 'TFTP +\r' > "$PORT"; sleep 2
  echo "  [tftp PUT -> AUTOBURN burns @0x40000] DO NOT POWER OFF"
  curl -s --connect-timeout 10 --max-time 200 -T "$IMG" tftp://192.168.0.1/img; echo "  curl rc=$?"
  tail -c +"$MARK" "$LOG" 2>/dev/null | grep -aqiE 'File: img|Upload File' || { echo "  !! loader never got the image"; continue; }

  echo "  [waiting for 'Flash Write Successed']"
  DL2=$((SECONDS+180))
  while [ $SECONDS -lt $DL2 ]; do
    tail -c +"$MARK" "$LOG" 2>/dev/null | grep -aqi 'Flash Write Success' && { burned=1; break; }
    sleep 2
  done
  [ $burned -eq 1 ] && { echo "★★ FLASH WRITE OK"; break; }
  echo "  burn not confirmed"
done
[ $burned -eq 1 ] || { echo "FAILED to burn after 3 rounds"; exit 1; }

# Optional: cold-boot from NOR with NO ESC spam and capture what the kernel prints.
if [ "${BOOT:-0}" = "1" ]; then
  echo "=== cold-booting from NOR (no catcher) — capturing boot ==="
  BM=$(wc -c < "$LOG")
  "$TOMADA" off >/dev/null 2>&1; sleep 3; "$TOMADA" on >/dev/null 2>&1
  sleep 45
  echo "--- first fault / boot outcome ---"
  tail -c +"$BM" "$LOG" | tr -d '\r' | grep -aiE 'Oops|Unable to handle|unaligned|Call Trace|epc |Kernel panic|Please press Enter|VFS: Mounted|jffs2' | head -25
fi
