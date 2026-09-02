#!/usr/bin/env bash
# Busy% of the DIR-842's CPU over a window, from /proc/stat deltas.
#
# ★ Use this, NOT `top -bn1`. On this single-core MIPS box a single top sample
# read 75% sys during a transfer that /proc/stat deltas showed as 17% busy --
# see RETRACTIONS-AND-METHOD.md confound #29. BusyBox top's first pass is a real
# delta but over only ~100 ms, and on one MIPS core top's own /proc walk plus the
# ssh session dominate such a short window: it measures the observer. Use a
# window of several seconds, as this script does.
#
#   ./cpu-delta.sh [seconds]     (default 6)
BOX=${BOX:-"ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
-o ConnectTimeout=5 -o LogLevel=ERROR -o IdentitiesOnly=yes \
-i $HOME/.ssh/id_router_rsa root@192.168.100.3"}
read -r _ a b c idle _ < <(timeout 20 $BOX 'grep "^cpu " /proc/stat' 2>/dev/null)
t1=$((a+b+c+idle)); i1=$idle
sleep "${1:-6}"
read -r _ a b c idle _ < <(timeout 20 $BOX 'grep "^cpu " /proc/stat' 2>/dev/null)
t2=$((a+b+c+idle)); i2=$idle
dt=$((t2-t1)); di=$((i2-i1))
if [ "$dt" -gt 0 ]; then
  echo "busy: $(( (dt-di)*100/dt ))%  (idle $(( di*100/dt ))%) over ${dt} ticks"
else
  echo "no tick delta -- box unreachable?" >&2; exit 1
fi
