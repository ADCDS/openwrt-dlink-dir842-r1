# DIR-842 R1 on OpenWrt — assessment

Answers to the five questions about running this port as a (secondary) home
gateway. Measured on the working RAM-boot build (kernel 4.14, RTL8197F).

## TL;DR

The port is a real achievement (ethernet + the first-ever RTL8197F PCIe WiFi in
OpenWrt), but as a **gateway it is not ready**, and current **throughput is very
low** (~4 Mbit/s wired) because the drivers are still Milestone-1 quality. It's
a great WiFi-AP / experimentation platform; it is **not** something to route your
production internet through today.

## 1. Can it be my gateway?

**Not today — this is the headline gap.** It would take net-new driver work
(call it M6) on the scale of the ethernet/WiFi milestones.

- Only **one** ethernet netdev exists (`eth0`), as a single **flat, flooded
  VLAN-9 segment**. There is **no WAN port**, **no NAT** (`iptables -t nat`
  empty, `nf_conntrack_count 0`), and the firewall is off in the bench default.
- The external **RTL8367R 5-port gigabit switch is completely unmanaged** — no
  driver, no MDIO/DSA node. All 5 jacks are one L2 segment on whatever u-boot
  left the switch in.
- The eth driver is explicitly "Milestone-1": **polling RX** (1-jiffy timer, not
  IRQ), **TX floods every frame to all ports**, no phylink/ethtool.

**To become a gateway you need:** (a) real switch management — an RTL8367R
driver (DSA or swconfig) or a proper internal-switch VLAN carve — to split a WAN
port from the LAN and register it as a second netdev; (b) NAT/firewall on top.
This is a from-scratch driver effort (the RTL8367R has no existing OpenWrt
driver to crib), not a config change.

## 2. Is WiFi healthy — are the antennas "loud"?

**The antennas and radio are fine; the calibration is the limiter.**

- `phy1` = RTL8822BE (rtw88), **2×2** (TX/RX antenna masks `0x3`/`0x3`, both
  chains active). 2.4 GHz HT40 (≤300 Mbps PHY); **5 GHz VHT80, 24 channels**
  (≤780 Mbps PHY) — 5 GHz is the chip's **primary** band.
- Live link (2.4 GHz HT20 AP, dongle client): **signal −48 dBm**, tx MCS4
  39 Mbps / rx MCS7 65 Mbps. The radio negotiates fine.
- **Root limiter = blank efuse.** This board keeps **no RTL8822BE calibration
  on-chip** (efuse reads all `0xff`): no per-board TX-power/PA/crystal table, and
  regdomain defaults to conservative `country 00`. So absolute TX power is
  uncalibrated — it works but isn't "loud", especially on 2.4 GHz (the chip's
  weak secondary band, and the band the board wires for its *other* radio).
- **Recommendations (baked into the default):** run the AP on **5 GHz** (primary
  band, 2×2 VHT80) and set a **real regdomain** (the default uci-default uses
  `country BR`, which unlocks the UNII-1 channels for beaconing). Loudness stays
  capped until a board TX-power table is supplied, but 5 GHz + a real country is
  materially better than the 2.4 GHz/world default we first tested.

## 3. Is NSS / hardware offload working?

**N/A — there is no NSS here.** NSS is Qualcomm (IPQ, e.g. the AX3000T). The
RealTek equivalent would be the switch-ASIC L3/L4 hardware NAT ("rome"), and this
port uses **none of it — pure software forwarding.** OpenWrt's **software
flowtable** (`nf_flow_table`, `xt_FLOWOFFLOAD`) is built and loaded and would
help once a real NAT/WAN exists, but every routed byte still crosses the single
MIPS 24Kc core. There is no hardware-offload backend to enable.

## 4. LAN / WiFi bandwidth — healthy?

**Hardware is capable; the current drivers are the ceiling.**

- **Measured (busybox wget over TCP, box as receiver):** ~**4 Mbit/s** on the
  wired LAN and ~4 Mbit/s over 2.4 GHz WiFi, with the **CPU idle** (`loadavg`
  ~0.08 — *not* CPU-bound).
- **Wired ~4 Mbit/s is the real story:** the LAN ports are **gigabit hardware**
  (RTL8367R GbE + RGMII uplink), but the Milestone-1 **polling** eth driver caps
  throughput at single-digit Mbit/s (fixed small poll-batch × 100 Hz). This is a
  driver limitation, not the silicon — and it's the #1 thing to fix for any
  gateway/router use.
- **WiFi:** the *link* negotiates 39–65 Mbps (2.4 GHz) and the radio is healthy;
  the ~4 Mbit/s end-to-end figure is constrained by the **test path** (busybox
  wget's small buffers + an old 1×1 USB test dongle), not the RTL8822BE. A proper
  5 GHz client + `iperf3` would show much more (up to the VHT80 link). `iperf3`
  isn't on the initramfs image, so a precise ceiling wasn't measured.
- **Blocked:** LAN↔WAN NAT throughput can't be measured until a WAN port exists
  (see §1).

## 5. Can we move to a newer kernel?

**Yes in principle, but it's a port project, not a version bump.**

- We're on **kernel 4.14** (out-of-tree ggbruno fork). **Upstream OpenWrt does
  not support RTL819x/RTL8197F at all** — mainline's `realtek` target is the
  RTL838x/930x managed switches (DSA, kernel 6.x). The whole RTL8197F target is
  out-of-tree.
- The CPU is **full MIPS 24Kc** (not the Lexra reduced-ISA of older RTL8196), so
  a modern kernel *can* target it — the arch is not a blocker.
- **The work:** forward-port our device-specific code — the carved eth driver
  (`rtl819x-eth.c` + swnic), `pci-realtek.c` (incl. the reverse-engineered reset
  sequence), the DTS, and `patches-4.14/` — across large API churn (NAPI/timer,
  `of_get_mac_address`, PCI host-bridge framework). rtw88 is already backports-5.8
  and would come along for free (5.15+ has it in-tree). Best done **together with
  the M6 switch/WAN work** rather than first.

---

**Bottom line:** excellent as a proof-of-port and a 5 GHz WiFi AP; **not a
production gateway** without (M6) real switch/WAN/NAT drivers and a matured,
interrupt-driven eth driver. The stock D-Link firmware remains the gateway for
now (and is fully backed up — see the `dir842-firmware` repo).
