# Prebuilt images

Built 2026-08-31 from the **v1.2** tree by the exact clean-room path the README
documents: fresh clone → `./build.sh` inside the Debian 11 (bullseye) container
from `docs/BENCH.md` §7. Nothing outside this repository went into them — **dual-band**
(2.4 GHz `DIR842-2G` via the vendor `rtl8192cd` driver + 5 GHz `DIR842-OpenWrt` via
rtw88, ⚠ both open by default) + wired + hardware-NAT + 802.11r images.

★ **v1.2 removes the persistent crash log** that v1.1 advertised. An attached
pstore/ramoops wedges CPU-originated TX on this board: the box boots, serves WiFi and
receives normally while silently dropping everything it originates on the wire. If you are
running v1.1, upgrade. See [`../docs/COLD-BOOT-TX-WEDGE.md`](../docs/COLD-BOOT-TX-WEDGE.md) §9.

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
