#!/bin/bash
# bench-up.sh — bring the DIR-842 bench to a known-good state after a RAM-boot.
# Run AFTER ramboot.sh. Everything here is required every boot; skipping any step
# reproduces "100% packet loss" that looks like a driver bug but is just cold state.
#
#   bash bench-up.sh
#
# Topology (2026-07-21 rewire):
#   host USB-eth $IF 192.168.0.2/24 -> DIR-842 LAN jack (8367S port 2, 1000M)
#   tiny br0 172.16.0.2/24 (eth0 is a br0 SLAVE)  -> DIR-842 WAN jack (8367S port 4, 100M)
#   box: br-lan 192.168.0.1 (eth0.2) / eth0.1 172.16.0.1 ; loader IP 192.168.0.1 (IPCONFIG)
set -u
PORT=/dev/ttyUSB0
IF="${IF:-eth1}"	# host NIC cabled to the DIR-842 LAN — override: IF=... bash bench-up.sh
say() { printf '\r%s\r' "$*" > "$PORT"; sleep "${2:-2}"; }

# 1. Host side. NetworkManager strips manual addresses off this NIC -> unmanage it.
#    NEVER pin a static ARP for 192.168.0.1: the LOADER answers on the stock MAC
#    (e0:1c:fc:51:c9:ef) while Linux uses 00:e0:4c:81:96:c2 — a permanent entry for
#    either one breaks the other. Let ARP resolve dynamically.
sudo nmcli device set "$IF" managed no 2>/dev/null
sudo ip link set "$IF" up
sudo ip addr add 192.168.0.2/24 dev "$IF" 2>/dev/null
sudo ip route replace 172.16.0.0/24 via 192.168.0.1 dev "$IF"
sudo ip neigh flush dev "$IF" 2>/dev/null

# 2. tiny's bench WAN address lives on br0 (NOT eth0) and is lost on reboot.
ssh -o ConnectTimeout=8 tiny 'ip addr show br0 | grep -q "172.16.0.2" || sudo ip addr add 172.16.0.2/24 dev br0' 2>/dev/null

# 3. Box: program the ASIC gateway (routes/mode/extIP/ACL). This WIPES the L2 tables,
#    so it must come BEFORE the warm-up, never after.
say 'cat /proc/rtl865x_gw | grep RESULT' 6

# 4. Warm the ASIC L2/ARP both ways. Cold entries = 100% loss until traffic flows;
#    a single ping burst is not always enough, hence the loop.
say 'for i in 1 2 3; do ping -c3 -W1 192.168.0.2 >/dev/null 2>&1; ping -c3 -W1 172.16.0.2 >/dev/null 2>&1; done; echo WARMED' 40

# 5. Offload on.
say 'echo 1 > /sys/module/rtl819x/parameters/hwnat; echo HW=$(cat /sys/module/rtl819x/parameters/hwnat)' 3

# 6. ★ RE-ASSERT the host route + tiny's address IMMEDIATELY BEFORE measuring. Both drift
#    away on their own (NetworkManager re-adds a default via the house gateway, so
#    172.16.0.0/24 silently starts routing out enp3s0; tiny's br0 loses 172.16.0.2 on any
#    reconfigure). When that happens EVERY path reads "100% packet loss" and it looks like
#    a driver regression — it cost a wrong conclusion about SWTCR0 WANRouteMode once.
#    Verify these two lines before believing ANY negative bench result.
sudo ip route replace 172.16.0.0/24 via 192.168.0.1 dev "$IF"
ssh -o ConnectTimeout=8 tiny 'ip addr show br0 | grep -q "172.16.0.2" || sudo ip addr add 172.16.0.2/24 dev br0' 2>/dev/null

echo "=== verify (route + tiny addr re-asserted above) ==="
ip route get 172.16.0.2 | head -1
ping -c3 -W2 192.168.0.1 2>&1 | grep -E 'packet loss'
ping -c3 -W2 172.16.0.2 2>&1 | grep -E 'packet loss'
ping -c3 -W2 -s1400 192.168.0.1 2>&1 | grep -E 'packet loss'   # large-frame wedge check
