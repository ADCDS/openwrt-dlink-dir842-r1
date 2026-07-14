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
- ✅ **Wired ethernet** — a carved RTL8197F NIC driver (`rtl819x`) brings up
  `eth0` and pings/moves traffic. *Single flat LAN segment only* (see below).
- ✅ **5 GHz WiFi** — the on-board **RTL8822BE** (PCIe) via **rtw88**, as a
  working WPA2 AP. This is the headline result: RTL8197F + PCIe WiFi had never
  worked in OpenWrt before (see *Engineering notes*).
- ⚠️ **Not a gateway (yet).** Only one flat LAN port works; the external 5-port
  **RTL8367R** switch is unmanaged, there is **no WAN port and no NAT**. Multi-
  port WAN/LAN routing is net-new driver work — see [`docs/ASSESSMENT.md`](docs/ASSESSMENT.md).
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

## Credits & license

Built on [ggbruno/openwrt](https://github.com/ggbruno/openwrt) (RTL8197F target)
and hackpascal's `pci-realtek` driver. GPL-2.0, following OpenWrt.
