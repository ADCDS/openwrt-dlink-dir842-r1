#!/bin/bash
# bootgate.sh — R1 gate: N consecutive unattended cold boots from NOR, each checked for
#   (a) no oops / unaligned / panic,  (b) reaching userspace ("Please press Enter").
# Fully autonomous via the smart plug.
#
#   N=10 bash bootgate.sh
set -u
LOG=/home/agiu/dir842-r1-bootlog.txt
TOMADA="${TOMADA:-/home/agiu/.local/bin/tomada}"
N="${N:-10}"; WAIT="${WAIT:-50}"
pass=0; fail=0
pgrep -f "cat /dev/ttyUSB0" >/dev/null || { setsid bash -c "cat /dev/ttyUSB0 >> $LOG" </dev/null >/dev/null 2>&1 & sleep 1; }

for i in $(seq 1 "$N"); do
  M=$(wc -c < "$LOG")
  "$TOMADA" off >/dev/null 2>&1; sleep 3; "$TOMADA" on >/dev/null 2>&1
  sleep "$WAIT"
  slice=$(tail -c +$((M+1)) "$LOG" | tr -d '\r')
  oops=$(printf '%s' "$slice" | grep -aciE 'Oops|Unable to handle kernel|unaligned access|Kernel panic|BUG:')
  up=$(printf '%s' "$slice" | grep -aci 'Please press Enter to activate')
  jffs=$(printf '%s' "$slice" | grep -aci 'switching to jffs2 overlay')
  if [ "$oops" -eq 0 ] && [ "$up" -ge 1 ]; then
    pass=$((pass+1)); echo "boot $i/$N: PASS (oops=0 userspace=yes jffs2=$jffs)"
  else
    fail=$((fail+1)); echo "boot $i/$N: FAIL (oops=$oops userspace=$up)"
    printf '%s' "$slice" | grep -aiE 'Oops|Unable to handle|panic' | head -3
  fi
done
echo "=== GATE RESULT: $pass/$N pass, $fail fail ==="
[ "$fail" -eq 0 ]
