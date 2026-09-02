#!/usr/bin/env bash
# Busy% of the DIR-842's CPU over a window, from /proc/stat deltas.
#
# ★ Use this, NOT `top -bn1`. BusyBox top's first pass is a real delta but over
# only ~100 ms, and on one MIPS core top's own /proc walk plus the ssh session
# dominate such a short window: it measures the observer (RETRACTIONS #29).
#
# Both samples are taken in ONE ssh session so (a) a failed first login cannot
# turn the result into a since-boot lifetime average, and (b) the second login's
# key exchange on a 1 GHz MIPS box stays out of the measured window. All eight
# /proc/stat fields are summed; softirq -- where NAPI, bridge and mac80211 work
# lands on this router -- counts as busy. (Both were review findings, 2026-09-02.)
#
#   ./cpu-delta.sh [seconds]     (default 6)
BOX=${BOX:-"ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
-o ConnectTimeout=5 -o BatchMode=yes -o LogLevel=ERROR -o IdentitiesOnly=yes \
-i $HOME/.ssh/id_router_rsa root@192.168.100.3"}
W=${1:-6}
OUT=$($BOX "awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=\$i;print t,\$5,\$6,\$8}' /proc/stat; sleep $W; awk '/^cpu /{t=0;for(i=2;i<=NF;i++)t+=\$i;print t,\$5,\$6,\$8}' /proc/stat") || { echo "box unreachable" >&2; exit 1; }
echo "$OUT" | awk 'NR==1{t1=$1;i1=$2;w1=$3;s1=$4} NR==2{dt=$1-t1; if(dt<=0){print "bad window" > "/dev/stderr"; exit 1}
  di=$2-i1; dw=$3-w1; ds=$4-s1; printf "busy: %d%%  (idle %d%%, iowait %d%%, softirq %d%%) over %d ticks\n",(dt-di-dw)*100/dt,di*100/dt,dw*100/dt,ds*100/dt,dt}
END{if(NR<2){print "no samples" > "/dev/stderr"; exit 1}}'
