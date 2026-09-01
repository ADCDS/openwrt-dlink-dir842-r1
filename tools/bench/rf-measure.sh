#!/usr/bin/env bash
# 5 GHz metric via `tiny` (RPi4, FIXED position, same room as the DIR-842).
#
# ★ Reports our 5 GHz AP against a SAME-BAND, SAME-CHANNEL reference AP seen in
# the same scan. The ours-minus-reference delta cancels receiver drift and
# band-wide conditions, which raw RSSI does not -- raw readings drifted ~6 dB
# over one session for an unchanged config.
N=${1:-4}
G5=e0:1c:fc:51:c9:f0   # DIR-842 wlan1 (5 GHz, ch36)
R5=50:4f:3b:32:68:9f   # reference: neighbour AP also on 5180, fixed
G2=00:e0:4c:81:86:86   # DIR-842 wlan0 (2.4 GHz) -- healthy control
s5=0;c5=0; sr=0;cr=0; s2=0;c2=0
for i in $(seq 1 "$N"); do
  OUT=$(timeout 60 ssh -o ConnectTimeout=8 -o BatchMode=yes tiny \
    'sudo iw dev wlan0 scan 2>/dev/null | awk "/^BSS/{b=\$2;sub(/\(.*/,\"\",b)} /signal:/{s=\$2} /SSID:/{print b, s}"' 2>/dev/null)
  a=$(echo "$OUT" | grep -i "$G5" | awk '{print $2}' | head -1 | cut -d. -f1)
  r=$(echo "$OUT" | grep -i "$R5" | awk '{print $2}' | head -1 | cut -d. -f1)
  b=$(echo "$OUT" | grep -i "$G2" | awk '{print $2}' | head -1 | cut -d. -f1)
  [ -n "$a" ] && { s5=$((s5+a)); c5=$((c5+1)); }
  [ -n "$r" ] && { sr=$((sr+r)); cr=$((cr+1)); }
  [ -n "$b" ] && { s2=$((s2+b)); c2=$((c2+1)); }
  printf "  scan %d: ours5=%-5s ref5=%-5s ours2.4=%-5s\n" "$i" "${a:-?}" "${r:-?}" "${b:-?}"
  sleep 2
done
[ "$c5" -gt 0 ] || { echo "★ 5 GHz AP NOT SEEN"; exit 1; }
m5=$((s5/c5))
printf "★ ours5=%s dBm" "$m5"
[ "$cr" -gt 0 ] && printf "  ref5=%s dBm  ★ OURS-MINUS-REF=%s dB" "$((sr/cr))" "$((m5-sr/cr))"
[ "$c2" -gt 0 ] && printf "  ours2.4=%s dBm  band-delta=%s dB" "$((s2/c2))" "$((s2/c2-m5))"
echo
