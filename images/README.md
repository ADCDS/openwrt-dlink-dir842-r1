# Prebuilt images

Built 2026-09-05 from the **v1.4.1** tree (`852bfb1`) with the build container's toolchain
(Debian 11, `docs/BENCH.md` §7) and **no private profile in the tree** — the profile
directory was removed from `openwrt/files/` before the image step, and the resulting rootfs
was audited: no `authorized_keys`, no `ap-profile/`, no profile seeds, no baked `wireless`
config, no PSK strings and no private profile codenames anywhere under `/etc`; and every
v1.4/v1.4.1 fix present. Nothing outside this repository went into the images —
**dual-band** (2.4 GHz `DIR842-2G` via the vendor `rtl8192cd` driver + 5 GHz
`DIR842-OpenWrt` via rtw88, ⚠ both open by default) + wired + hardware-NAT + 802.11r images.

★ **v1.4.1 fixes an OOM-killer bug found running both radios with LuCI up
simultaneously.** OpenWrt's stock `vm.min_free_kbytes=8192` default for this board's RAM
tier holds back ~14% of its ~58 MB usable RAM as an untouchable floor. With both radios,
hostapd, wpa_supplicant and uhttpd/LuCI all running, free memory sits at/under that floor
and the kernel OOM-killer starts reaping live daemons — observed on hardware killing
`netifd` and `hostapd` outright, and even basic shell commands failing to fork. Fixed by
shipping `vm.min_free_kbytes=2048` instead (still a real reserve for atomic/GFP_ATOMIC
allocations in the NIC/WiFi RX paths, just not an oversized one). No other changes from
v1.4.

★ **v1.4 makes the 5 GHz AP actually usable, and stops sysupgrade from wiping your
config.** On this hardware the 5 GHz radio at 80/40 MHz associates clients but never
completes DHCP — the link is too marginal (a stock-firmware A/B on the same unit shows the
transmitter ~17 dB short) — so **the image now defaults to HT20**, which carries data on
every boot, and a migration seed moves an existing VHT80/VHT40 config there. The first-boot
seed also **no longer re-runs on every sysupgrade**: v1.3 and earlier re-seeded LAN
192.168.0.1, a DHCP WAN and an *open* `DIR842-OpenWrt` SSID over a preserved config on every
flash. Both WiFi panel LEDs now light (2.4 GHz via the vendor `led_type`, 5 GHz via a small
rtw88 patch driving the card's GPIO8), and the RX-stall auto-recovery from v1.3 is verified
against the original bug report. See [`../docs/WIFI-DUAL-BAND.md`](../docs/WIFI-DUAL-BAND.md)
and [`../docs/LEDS.md`](../docs/LEDS.md).

> **The `.bin` files are not committed to git** (they are build artifacts). Download them
> from the repo's **[latest release](https://github.com/ADCDS/openwrt-dlink-dir842-r1/releases/latest)**,
> drop them in this directory, then verify:
>
> ```sh
> sha256sum --ignore-missing -c sha256sums.txt
> ```
>
> The checksums here are the authoritative record of what that release contains.

ℹ The images are named **`GWR1200AC-V1`** because this port reuses OpenWrt's Greatek
GWR1200AC-V1 device profile (same RTL8197F platform). These *are* the DIR-842 R1 images.

| file | use |
|---|---|
| `*-initramfs-kernel.bin` | RAM boot via the loader (TFTP/XMODEM) — **never writes flash**; a power-cycle discards it. Try this first. |
| `*-squashfs-factory.bin` | **The one most people want.** Upload through stock's web UI ([OTA-INSTALL](../docs/OTA-INSTALL.md), no serial), or NOR-flash via the loader's `AUTOBURN`. Already carries the D-Link keyed-MD5 trailer. |
| `*-squashfs-sysupgrade.bin` | NOR flash via `sysupgrade` from a running OpenWrt. |

⚠ Both squashfs images **replace stock firmware**. Back up all 8 MB of NOR first —
after flashing, stock exists only in your backup. And read the README's security box:
default images boot with an **open 5 GHz AP** (`DIR842-OpenWrt`), no root password, and a
`BR` regulatory domain.
