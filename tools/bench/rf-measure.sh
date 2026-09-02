#!/usr/bin/env bash
# 5 GHz metric via `tiny` (RPi4, FIXED position, same room as the DIR-842).
#
# ★ Reports our 5 GHz AP against a SAME-BAND, SAME-CHANNEL reference AP seen in
# the same scan, averaged over N GOOD samples. The ours-minus-reference delta
# cancels receiver drift; raw RSSI drifted ~6 dB in one session for an
# unchanged config (RETRACTIONS #28).
#
# Review fixes 2026-09-02: `scan flush` (cfg80211 re-reports cached BSSes for
# 30 s at their OLD RSSI, so a dropout could never register and stale readings
# were averaged in); ssh/scan failures are counted as failures, not as "AP not
# seen"; N counts good samples, not attempts; a missing reference is an error;
# per-BSS signal is reset so a BSS without a signal line cannot inherit its
# neighbour's; means are floats (integer truncation biased results by ~1-2 dB).
#
# ★ Site-specific: the MACs, the `tiny` ssh host and the box IP are this bench's.
N=${1:-4}; MAXTRY=$((N*3))
G5=e0:1c:fc:51:c9:f0   # DIR-842 wlan1 (5 GHz, ch36)
R5=50:4f:3b:32:68:9f   # reference: another BRAVO AP in the house, co-channel on 5180, fixed
G2=00:e0:4c:81:86:86   # DIR-842 wlan0 (2.4 GHz) -- healthy control
a5=(); ar=(); a2=(); try=0; fails=0
while [ ${#a5[@]} -lt "$N" ] && [ $try -lt $MAXTRY ]; do
  try=$((try+1))
  OUT=$(timeout 60 ssh -o ConnectTimeout=8 -o BatchMode=yes tiny \
    'sudo /usr/sbin/iw dev wlan0 scan flush 2>&1 | awk "/^BSS/{b=\$2;sub(/\(.*/,\"\",b);s=\"\"} /signal:/{s=\$2} /SSID:/{print b, s}"'); rc=$?
  if [ $rc -ne 0 ] || [ -z "$OUT" ]; then fails=$((fails+1)); echo "  try $try: scan FAILED (rc=$rc)"; sleep 3; continue; fi
  a=$(echo "$OUT" | awk -v m="$G5" 'tolower($1)==m{print $2; exit}')
  r=$(echo "$OUT" | awk -v m="$R5" 'tolower($1)==m{print $2; exit}')
  b=$(echo "$OUT" | awk -v m="$G2" 'tolower($1)==m{print $2; exit}')
  printf "  try %d: ours5=%-7s ref5=%-7s ours2.4=%-7s\n" "$try" "${a:-?}" "${r:-?}" "${b:-?}"
  if [ -z "$a" ] || [ -z "$r" ]; then echo "  (incomplete sample: $([ -z "$a" ] && echo 'ours5 missing ')$([ -z "$r" ] && echo 'ref5 missing')) -- not counted"; sleep 2; continue; fi
  a5+=("$a"); ar+=("$r"); [ -n "$b" ] && a2+=("$b"); sleep 2
done
if [ ${#a5[@]} -lt "$N" ]; then echo "★ ERROR: only ${#a5[@]}/$N good samples after $try tries ($fails scan failures) -- receiver or link problem, NOT a measurement of the AP" >&2; exit 1; fi
mean(){ printf '%s\n' "$@" | awk '{s+=$1;n++} END{printf "%.1f", s/n}'; }
m5=$(mean "${a5[@]}"); mr=$(mean "${ar[@]}")
printf "★ ours5=%s dBm  ref5=%s dBm  ★ OURS-MINUS-REF=%.1f dB  (n=%d good of %d tries)" "$m5" "$mr" "$(echo "$m5 $mr" | awk '{print $1-$2}')" "${#a5[@]}" "$try"
[ ${#a2[@]} -gt 0 ] && printf "  ours2.4=%s dBm  band-delta=%.1f dB" "$(mean "${a2[@]}")" "$(echo "$(mean "${a2[@]}") $m5" | awk '{print $1-$2}')"
echo
