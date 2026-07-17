# openwrt-dlink-dir842-r1

Mainline-style OpenWrt for the **D-Link DIR-842 rev R1** (RealTek **RTL8197F**
SoC). This repo is a **build recipe**: `build.sh` overlays `./files/` (the
device support + the fixes below) onto a pinned [ggbruno/openwrt](https://github.com/ggbruno/openwrt)
checkout and produces a **RAM-boot** image.

> ⚠️ **Scope / safety.** This is for the **DIR-842 rev R1** (RTL8197F) only.
> The port is **RAM-boot only** — it is XMODEM'd into memory over the serial
> console and jumped to; **it never writes flash.** The stock D-Link firmware
> stays fully intact and a **power-cycle always returns you to stock**, so the
> brick risk is low. There is **no NAND-flashing / sysupgrade path yet** (see
> *Status*). Do not try to flash this on other DIR-842 revisions or other
> devices.

## Status — what works

- ✅ **Boots** mainline OpenWrt (kernel 4.14) to a root shell over serial + SSH.
- ✅ **Wired ethernet** — a carved RTL8197F NIC driver (`rtl819x`) brings up the
  CPU-port DMA engine and moves traffic.
- ✅ **Managed switch + WAN/LAN split** — the external 5-port **RTL8367S** is
  driven via `swconfig`, and an 802.1Q VID cascade over the single RGMII trunk
  splits it into a **WAN** jack and a **LAN** bridge (`eth0.1` / `eth0.2`).
- ✅ **Router with NAT** — masquerading gateway (fw3), with the software
  flowtable fastpath.
- ✅ **Hardware NAT offload (the M6.6 headline).** The RTL8197F's internal switch
  L3/L4 engine routes **and source-NATs** flows entirely in silicon: Linux
  conntrack installs per-flow NAPT rows into the ASIC via the kernel's
  `ndo_flow_offload` hook, so established flows are forwarded with the **CPU
  bypassed** — a flow's conntrack counters *freeze* mid-transfer while the CPU
  sits ~99 % idle. As far as we can tell this is the **first working mainline
  OpenWrt rtl865x ASIC L3/L4 offload** — the vendor's own "gigabit fast-path"
  mechanism, reverse-engineered onto a stock kernel. Default-off, runtime
  toggle: `echo 1 > /sys/module/rtl819x/parameters/hwnat`.
- ✅ **5 GHz WiFi** — the on-board **RTL8822BE** (PCIe) via **rtw88**, as a
  working WPA2 AP. RTL8197F + PCIe WiFi had never worked in OpenWrt before
  (see *Engineering notes*).
- ⚠️ **Sustained max-rate ceiling (A-2, open).** Line-rate bulk can latch the
  switch fabric; the earlier "descriptor-pool exhaustion" theory is **disproven**
  (a `/proc/rtl865x_fabric` diagnostic shows the pool never fills). Root-causing
  it needs a stable wired test peer — tracked as a GitHub issue.
- ⚠️ **RAM-boot only, no NAND/sysupgrade** yet (see *Scope / safety*).
- ⚠️ **Blank WiFi efuse** — this board keeps no RTL8822BE calibration on-chip,
  so TX power is uncalibrated (works, but not "loud"); handled in software
  (default RFE + random/pinned MAC).

## Build

```bash
git clone https://github.com/ADCDS/openwrt-dlink-dir842-r1
cd openwrt-dlink-dir842-r1
./build.sh
```
Output: `openwrt/bin/targets/realtek/rtl8197f/*-GWR1200AC-V1-initramfs-kernel.bin`.

**Build environment:** the ggbruno fork is from 2020 (kernel 4.14 / gcc 8.4);
build on a **Debian 11 (bullseye)-era** host or container — very new toolchains
can fail the old host tools. Container one-liner:
```bash
docker run --rm -v "$PWD":/build -w /build debian:bullseye \
  bash -c 'apt-get update && apt-get install -y build-essential git file \
    libncurses-dev zlib1g-dev gawk gettext unzip python3 rsync wget && ./build.sh'
```

## Install (RAM-boot over serial)

1. Serial console **38400 8N1** (USB-TTL on the board's UART header — note the
   D-Link doc's 115200 is wrong).
2. Power on and **spam `ESC`** to catch the RealTek loader → `<RealTek>` prompt.
3. `XMOD 81000000` then send the image with XMODEM (`sx -k … < /dev/ttyUSB0 > /dev/ttyUSB0`),
   ~18 min at 38400.
4. `J 81000000` to jump. It boots to OpenWrt; a power-cycle reverts to stock.

Full recovery notes: [`docs/`](docs/). A **private profile** (real gateway
config + WiFi secrets) can be layered at build time:
`PROFILE=~/dir842-profile ./build.sh`.

## Engineering notes

- **PCIe / RTL8822BE bring-up (the hard part).** The stock RealTek loader/driver
  trains the PCIe link but OpenWrt's `pci-realtek` driver did not — a wall the
  RTL8197F community had abandoned (e.g. the Netis N2 port). Reversing the stock
  D-Link kernel showed the mainline reset pointed two writes at the wrong
  registers: the **PHY digital-reset release (`8/9/0xb`) must go to `0xb8000100`**
  (not `0xb8000050`), and the **device reset is `0xb8000050` bit 1: clear → 300 ms
  → set** (not a no-op CLKMANAGE toggle). Fixed in `files/target/linux/realtek/
  files-4.14/arch/mips/pci/pci-realtek.c` → link trains at cold boot, `10ec:b822`
  enumerates, rtw88 binds.
- **Crystal is 25 MHz** (read from the SoC bootstrap register), not 40 — the DTS
  `pcie_clk` sets it so the PHY MDIO tuning matches.
- **Blank efuse** → two small mac80211 patches (`files/package/kernel/mac80211/
  patches/realtek/03,04`): default the RFE type to 2, and assign a random MAC
  when the efuse MAC is invalid.
- **Hardware NAT offload.** The RTL8197F switch core has an L3/L4 engine (netif +
  route + nexthop + ARP + L2 + NAPT + extIP tables) that stock uses for its
  gigabit fast-path but which has no mainline driver. `rtl865x_asichal.c` is a
  clean re-implementation of the table-access engine (reverse-engineered from the
  stock 3.10 kernel and cross-checked against the vendor SDK) and `rtl819x_hwnat.c`
  wires it to Linux conntrack through the downstream `ndo_flow_offload` interface:
  each offered LAN→WAN masquerade flow gets a pair of NAPT rows (outbound at the
  vendor hash index, inbound at `globalPort & 0x3ff`), aged by the ASIC and reaped
  by a worker. The forwarding chain is `route(process=5) → nexthop → ARP → L2`; a
  NAPT miss traps to the CPU so the software path is always the safe fallback.
  A key gotcha baked into the config: a VLAN sub-interface inherits the parent's
  MAC, but the ASIC's WAN netif uses `LAN_MAC+1`, and that mismatch silently
  blackholes every CPU-path WAN packet — so both LAN and WAN MACs are pinned to
  match the ASIC netifs. Default-off; an independent code review hardened the
  DEL/teardown path, table-access locking, and MAC pinning before release.
- **Known limitations** (documented in `rtl819x_hwnat.c`): the inbound ASIC row
  is full-cone (a masquerade source-port collision between an offloaded and a
  software flow can misdeliver packets), and the framework's async ADD can race a
  flow free. Both are narrow; offload is default-off and conservative.

## Credits & license

Built on [ggbruno/openwrt](https://github.com/ggbruno/openwrt) (RTL8197F target)
and hackpascal's `pci-realtek` driver. GPL-2.0, following OpenWrt.
