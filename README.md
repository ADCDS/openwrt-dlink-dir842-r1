# openwrt-dlink-dir842-r1

Mainline-style OpenWrt for the **D-Link DIR-842 rev R1** (RealTek **RTL8197F** SoC).
This repo is a **build recipe**: `build.sh` overlays `./files/` onto a pinned
[ggbruno/openwrt](https://github.com/ggbruno/openwrt) checkout and produces both a
RAM-boot image and a NOR flash image.

Headline: **hardware NAT offload at gigabit line rate on mainline OpenWrt** —
**891 Mbit up / 896 Mbit down with 0.0 % of payload bytes crossing the CPU** and the
CPU ~99.7 % idle. As far as we know this is the first working mainline OpenWrt
`rtl865x` ASIC L3/L4 offload. Stock D-Link on the same bench measures 913/923.

> ## ⚠️ Read this before flashing
>
> **This is for the DIR-842 rev R1 (RTL8197F) only.** Do not try it on other DIR-842
> revisions or other devices.
>
> **Flashing REPLACES the stock firmware, and there is no vendor recovery path here.**
> Earlier revisions of this README said the port was RAM-boot-only and that "a
> power-cycle always returns you to stock". **That is no longer true.** The NOR image
> boots from flash and survives a power cycle, which means:
>
> - **Back up all 8 MB of NOR first, and keep that backup.** After flashing, stock
>   exists *only* in your backup. There is no D-Link recovery image for this.
> - Recovery from a bad flash is over the **serial console** (RealTek loader → TFTP /
>   XMODEM). If you are not set up to open the case and attach a 3.3 V UART, stop here.
>
> **The safe way to try this is the initramfs image**, which is XMODEM'd into RAM and
> **never writes flash** — a power-cycle discards it entirely. Do that first.
>
> Default images have **no root password** and a **placeholder WiFi PSK**
> (`ChangeMeNow123`). Set both before putting this on a real network.

## Status

**Works**

- **Boots** mainline OpenWrt (kernel 4.14) from NOR, unattended, surviving power cycles.
- **Wired ethernet** — a carved RTL8197F NIC driver (`rtl819x`) drives the CPU-port DMA.
- **Managed switch, WAN/LAN split** — the external 5-port **RTL8367S** in the vendor's
  **CPU-tag / port0-router** mode, so the five jacks appear to the SoC as its own ports
  0–4 with the CPU on port 8 (`eth0.1` = WAN, `eth0.2` = LAN).
- **Router with NAT**, fw3, software flowtable fastpath, PPPoE.
- **Hardware NAT offload** — the RTL8197F switch core's L3/L4 engine routes *and*
  source-NATs entirely in silicon. Linux conntrack installs per-flow NAPT rows into the
  ASIC via `ndo_flow_offload`. Measured on a two-port gigabit bench:

  | | throughput | payload bytes through CPU | CPU busy |
  |---|---|---|---|
  | offload off | 184 up / 187 down Mbit | ~100.6 % | ~52 % |
  | **offload on** | **891 up / 896 down Mbit** | **0.0 %** | **0.3 %** |

  Runtime toggle: `echo 1 > /sys/module/rtl819x/parameters/hwnat`.
- **5 GHz WiFi** — on-board **RTL8822BE** (PCIe) via **rtw88**, WPA2 AP. RTL8197F +
  PCIe WiFi had never worked in OpenWrt before (see *Engineering notes*).
- **2.4 GHz WiFi** — the on-SoC WMAC via the vendor `rtl8192cd` driver (WEXT, in-kernel
  WPA2-PSK). **Requires the vendor SDK at build time** — see *Building*.
- **Both radios concurrently**, both bridged into `br-lan`.

**Known limitations**

- **Never run as a real household gateway.** Everything above is measured on an isolated
  bench (one host on a LAN jack, one Pi on the WAN jack). Treat it as pre-production.
- **Blank WiFi efuse** — this board keeps no RTL8822BE calibration on-chip, so TX power
  is uncalibrated (works, but not "loud"); handled in software (default RFE + pinned MAC).
- **Download throughput is variable** (~810–906 Mbit) with 1–2 k TCP retransmits per
  10 s run. The router is not the bottleneck (CPU 0.3 %, zero interface errors), but the
  loss source is not yet identified. Take a range, not a single run.
- The ASIC's inbound NAPT row is **full-cone**: a masquerade source-port collision
  between an offloaded and a software flow can misdeliver packets. Offload is
  default-off and a NAPT miss always traps to the CPU, so software is the safe fallback.
- `rtl819x: recovery level 3` fires ~2× per boot. Pre-existing, benign, unexplained.

## Building

```bash
git clone https://github.com/ADCDS/openwrt-dlink-dir842-r1
cd openwrt-dlink-dir842-r1
./build.sh
```

Output in `openwrt/bin/targets/realtek/rtl8197f/`:

| image | what it does |
|---|---|
| `*-initramfs-kernel.bin` | XMODEM into RAM over serial. **Never writes flash.** Start here. |
| `*-squashfs-factory.bin` | NOR flash via the loader's `AUTOBURN`. **Replaces stock.** |
| `*-squashfs-sysupgrade.bin` | NOR flash via `sysupgrade`. **Replaces stock.** |

**2.4 GHz needs the vendor SDK.** Two pieces of that path are Realtek SDK sources that
this repo deliberately does **not** redistribute — ~37 `include/net/rtl/*` headers and
the ~120-file `rtl8192cd` driver, all carrying *"Copyright Realtek Semiconductor
Corporation. All rights reserved."* with no license grant, and none of them present in
the pinned ggbruno base. What *is* shipped here is our own work against them:
`g4-rtl-headers-4.14-port.patch` and `g3-rtl8192cd-4.14-port.patch`, plus the
BSD-licensed `net80211` headers.

```bash
VENDOR_SDK=/path/to/rtl819x-sdk ./build.sh
```

Without `VENDOR_SDK` the build still succeeds — you get wired, the switch, NAT,
hardware offload and 5 GHz. You lose only the 2.4 GHz radio.

**Build environment:** the ggbruno fork is from 2020 (kernel 4.14 / gcc 8.4). Build on a
**Debian 11 (bullseye)-era** host or container; very new toolchains fail the old host
tools.

```bash
docker run --rm -v "$PWD":/build -w /build debian:bullseye \
  bash -c 'apt-get update && apt-get install -y build-essential git file \
    libncurses-dev zlib1g-dev gawk gettext unzip python3 rsync wget && ./build.sh'
```

## Installing

**Serial console is 38400 8N1** — the D-Link documentation's 115200 is wrong and gives
you garbage.

### RAM-boot (safe, no flash writes)

1. Power on and spam `ESC` to catch the RealTek loader → `<RealTek>` prompt.
2. `XMOD 81000000`, then send the initramfs image over XMODEM (~18 min at 38400).
3. `J 81000000` to jump. A power-cycle discards it and returns you to whatever is in flash.

### NOR flash (replaces stock — back up first)

The loader verifies a D-Link trailer before it will boot an image from flash. It is a
**forgeable keyed-MD5**, so a self-built image can be made bootable:

```
[cvimg image][ MD5(key || cvimg_image) : 16 ][ 00 C0 FF EE : 4 ]
```

with the key embedded in the bootcode (`mtd0`) at offset `0x291bc`.
`tools/sign-dlink.py` implements it:

```bash
tools/sign-dlink.py openwrt-...-squashfs-factory.bin signed.bin
```

Then, at the loader prompt: set the loader's IP, `AUTOBURN 1`, TFTP the signed image,
and wait for `Flash Write Successed` before removing power.

This is published because it is what makes the port usable at all — it is a boot-time
integrity check on a device you own, not a content-protection or anti-tamper measure,
and without it the flash path cannot be reproduced by anyone else.

## Engineering notes

- **PCIe / RTL8822BE bring-up.** The stock loader trains the PCIe link but OpenWrt's
  `pci-realtek` did not — a wall the RTL8197F community had abandoned (e.g. the Netis
  N2 port). Reversing the stock kernel showed mainline pointed two writes at the wrong
  registers: the **PHY digital-reset release must go to `0xb8000100`** (not
  `0xb8000050`), and the **device reset is `0xb8000050` bit 1: clear → 300 ms → set**
  (not a no-op CLKMANAGE toggle). Link now trains cold, `10ec:b822` enumerates, rtw88
  binds. Crystal is **25 MHz** (from the SoC bootstrap register), not 40.
- **Hardware NAT offload.** `rtl865x_asichal.c` is a clean re-implementation of the
  ASIC table engine (netif / route / nexthop / ARP / L2 / NAPT / extIP), reverse
  engineered from the stock 3.10 kernel and cross-checked against the vendor SDK;
  `rtl819x_hwnat.c` wires it to conntrack. Forwarding chain is
  `route(process=5) → nexthop → ARP → L2`.
  - ★ **The RTL8197F keys and hashes NAPT on NUMERIC (host-order) values, not on the
    on-wire network order.** The vendor's `htonl()` at the ASIC boundary is `ntohl()`
    in disguise, because its fields are raw `__be32` straight from conntrack. Key,
    index, G encoding and the inbound verification hash must *all* be numeric
    **together** — any one left swapped hides the win completely.
  - ★ **CPU-tag mode is what makes offload possible.** With all five jacks hidden behind
    the single RGMII trunk, a routed unicast has no distinct egress port to commit to,
    so the ASIC traps every packet to the CPU no matter how well the NAPT rows match.
- **`CPUICR1` bit 1 `CF_NIC_LITTLE_ENDIAN` must be programmed explicitly.** The
  bootloader only sets it when it runs its own network init, so a TFTP RAM boot works
  (`0x82`) while a NOR flash boot does not (`0x80`) — and with it clear the NIC DMAs
  every frame into DRAM 32-bit-word byte-swapped, so `eth_type_trans()` reads a
  "multicast" destination MAC and frames are counted but never reach the IP stack.
  Fingerprint: `eth0.2` `rx_packets` and `rx_multicast` rising 1:1 while `br-lan` stays
  flat.
- **The two radios collide over interface naming.** `mac80211` derives its name from the
  phy index (`phy0` → `wlan0`), but the vendor `rtl8192cd` driver creates a real netdev
  called `wlan0` at module load. `10_wireless-5g-ifname-dir842` pins the mac80211 side
  to `wlan1`. Do not rely on module load order to keep them apart.

More detail in [`docs/`](docs/).

## Credits & license

Built on [ggbruno/openwrt](https://github.com/ggbruno/openwrt) (RTL8197F target) and
hackpascal's `pci-realtek` driver.

**GPL-2.0** ([`LICENSE`](LICENSE)), following OpenWrt. The `net80211` headers under
`files/` are BSD-licensed and carry their original notices. Realtek SDK sources are
**not** redistributed here — see *Building*.
