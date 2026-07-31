#!/bin/bash
# bench-wan-emu.sh — emulate the DIR-842's WAN peer on the HOST, over the gigabit
# LAN jack, so the bench needs no second machine and is not capped by a 100 Mb cable.
#
# Run AFTER ramboot.sh + bench-up.sh. Safe to re-run.
#
# How it works: the host's jack (8367S port 2) is added to the WAN VLAN as TAGGED,
# alongside the real WAN jack. The host then puts a vid-1 802.1Q subinterface in a
# network namespace holding 172.16.0.2/24 — the address tiny used to have. The
# namespace boundary forces the traffic onto the wire, so the box routes and NATs
# it exactly as it would for a real WAN host, but at 1000baseT.
#
# ★ vid 1 is reused DELIBERATELY. The SoC's internal switch has VLAN entries for
# vid 1 and vid 2 only (sw_add_vlan); TX on any other VID is silently dropped
# because ph_vlanId must name a VID whose SoC member mask covers the portlist
# (rtl819x-eth.c:1134). A vid 3 was tried first and produced exactly that -- ARP
# never resolved. Adding a third VLAN would need a driver change.
#
# ⚠ BENCH ONLY. Never ship this: it puts a LAN jack in the WAN VLAN.
# ⚠ Every byte crosses the host NIC twice (in on vid2, out on vid1), so the wire
#   carries ~2x the payload. Fine while the box saturates its CPU near 180 Mbit.
set -u
PORT=/dev/ttyUSB0
IF="${IF:-enx00e04c125990}"
NS=benchns
say() { printf '\r%s\r' "$*" > "$PORT"; sleep "${2:-3}"; }

echo "[1/3] box: add the host jack to the WAN vlan as tagged"
say 'swconfig dev switch0 vlan 1 set ports "4 2t 6t" && swconfig dev switch0 set apply' 6

echo "[2/3] host: vid-1 subinterface inside netns $NS"
sudo ip netns del $NS 2>/dev/null
sudo ip link del wanp 2>/dev/null
sudo ip link add link "$IF" name wanp type vlan id 1
sudo ip netns add $NS
sudo ip link set wanp netns $NS
sudo ip -n $NS addr add 172.16.0.2/24 dev wanp
sudo ip -n $NS link set lo up
sudo ip -n $NS link set wanp up
sudo ip -n $NS route add default via 172.16.0.1

echo "[3/3] warm + verify (cold ARP loses the first packets -- confound #5)"
for i in 1 2 3; do ping -c3 -W1 172.16.0.2 2>&1 | tail -1; done

cat <<'USAGE'

ready. Run a download (WAN->LAN, the CPU-forwarded direction):
    sudo ip netns exec benchns iperf3 -s -B 172.16.0.2 &
    iperf3 -c 172.16.0.2 -R -t 10 -f m
USAGE
