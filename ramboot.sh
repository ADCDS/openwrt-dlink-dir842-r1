#!/bin/bash
# ramboot.sh — fully autonomous RAM-boot: self power-cycle via the smart plug, catch the
# loader, TFTP the current initramfs, jump. No human needed.
#
#   bash ramboot.sh [image.bin]     # defaults to the freshly built initramfs
#
# ★ Timing is the whole trick: the box HANGS after the #11 flash-boot crash (no watchdog
# reboot), so there is exactly ONE loader window per power-cycle and it opens within ~1 s
# of power-on. `tomada on` itself takes 1-2 s to return, so spamming only after it returns
# MISSES the window every time. Therefore: power OFF -> start a background ESC-spammer ->
# power ON while it is already spamming -> foreground watches the log for the catch.
set -u
PORT=/dev/ttyUSB0
LOG=/home/agiu/dir842-r1-bootlog.txt
FLAG=/tmp/ramboot-spam.flag
TOMADA="${TOMADA:-/home/agiu/.local/bin/tomada}"
IFACE="${IFACE:-eth1}"	# host NIC cabled to the DIR-842 LAN — override: IFACE=... ./ramboot.sh
BIN_DIR="${BIN_DIR:-$(dirname "$(readlink -f "$0")")/openwrt/bin/targets/rtl819x/rtl8197f}"
IMG="${1:-$(ls $BIN_DIR/*initramfs-kernel.bin | head -1)}"

sr() { stty -F "$PORT" 38400 cs8 -parenb -cstopb -crtscts -ixon clocal raw -echo 2>/dev/null; }
spam_stop() { rm -f "$FLAG"; sleep 0.3; }
trap 'spam_stop' EXIT
sr
touch "$LOG"
# ★ match the logger process only: a plain `cat $PORT` pattern also matches any
# shell whose command line merely mentions the device node, and the guard then
# skips starting the logger, leaving every later `wc -c < $LOG` to fail.
pgrep -f "^cat $PORT" >/dev/null || { setsid bash -c "exec cat $PORT >> $LOG" </dev/null >/dev/null 2>&1 & sleep 1; }

echo "[ramboot] IMG=$IMG"
for round in 1 2 3; do
  echo "=== round $round ==="
  "$TOMADA" off >/dev/null 2>&1; sleep 3

  # ESC-spammer runs FIRST so the loader window can't be missed.
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
  echo "  ★ caught; TFTP upload"

  sleep 2; sr; tftp_ok=0
  # ★ Re-assert the host address EVERY round. NetworkManager repeatedly strips the manual
  # IPv4 off this USB NIC (and a re-enumeration clears it too); when that happens the
  # catcher keeps catching the loader while curl silently fails, which looks like an
  # infinite "stuck at <RealTek>" loop. Unmanage + re-add before touching TFTP.
  sudo nmcli device set "$IFACE" managed no 2>/dev/null
  sudo ip link set "$IFACE" up 2>/dev/null
  ip -4 addr show "$IFACE" | grep -q 192.168.0.2 || sudo ip addr add 192.168.0.2/24 dev "$IFACE" 2>/dev/null
  sudo ip neigh flush dev "$IFACE" 2>/dev/null   # loader answers on the STOCK MAC
  ping -c1 -W2 192.168.0.1 >/dev/null 2>&1 || echo "  !! loader unreachable at 192.168.0.1"
  # ★ The loader's own IP lives in nvram and resets to the 192.168.1.6 factory default;
  # our bench link is 192.168.0.0/24, so pin it every time. (Also: the loader answers on
  # the STOCK MAC e0:1c:fc:51:c9:ef, NOT the Linux driver's 00:e0:4c:81:96:c2 — never
  # install a static ARP for 192.168.0.1 or the loader becomes unreachable.)
  printf '\r' > "$PORT"; sleep 0.5
  printf 'IPCONFIG 192.168.0.1\r' > "$PORT"; sleep 1.5
  for t in 1 2 3 4; do
    MARK=$(wc -c < "$LOG")
    printf '\r' > "$PORT"; sleep 0.5
    printf 'AUTOBURN 0\r' > "$PORT"; sleep 1
    printf 'LOADADDR 82000000\r' > "$PORT"; sleep 1
    printf 'TFTP +\r' > "$PORT"; sleep 3
    if curl -s --connect-timeout 8 --max-time 90 -T "$IMG" tftp://192.168.0.1/img; then tftp_ok=1; echo "    TFTP ok (try $t)"; break; fi
    echo "    curl failed (retry $t)"; sleep 2; sr
  done
  [ $tftp_ok -eq 1 ] || { echo "  TFTP failed 4x"; continue; }

  sleep 1; sr; printf 'J 82000000\r' > "$PORT"
  echo "  jumped; waiting for shell"
  DL2=$((SECONDS+90))
  while [ $SECONDS -lt $DL2 ]; do
    tail -c +"$MARK" "$LOG" 2>/dev/null | tr -d '\r' | grep -aqE "Please press Enter to activate" && {
      echo "★★ SHELL UP — RAM-booted"; exit 0; }
    sleep 3
  done
  echo "  did not reach shell"
done
echo "FAILED after 3 rounds"; exit 1
