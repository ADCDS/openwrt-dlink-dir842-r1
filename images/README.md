# Prebuilt images (`port/main-6.18`)

Built 2026-09-03 from `port/main-6.18` (kernel 6.18.44, OpenWrt main), natively — no
container, no pinned toolchain. `seed.config` (the release package set): both radios'
worth of packages minus 2.4 GHz (not ported, see
[`../docs/PORT-MAIN-6.18-STATUS.md`](../docs/PORT-MAIN-6.18-STATUS.md) §6), firewall4,
PPPoE, and LuCI.

**This is a rebase-in-progress, not a finished release.** Wired ethernet and the 5 GHz radio
work; hardware NAT offload is wired up but does not accelerate traffic yet; there is no
2.4 GHz radio at all. Full status: the root [`../README.md`](../README.md) and
[`../docs/PORT-MAIN-6.18-STATUS.md`](../docs/PORT-MAIN-6.18-STATUS.md). If you want the
finished, dual-band, hardware-NAT-at-gigabit product, use the `main` branch's images
instead.

Verified before this build was staged: `bootgate.sh` 10/10 clean cold boots from this exact
image, `sysupgrade -n` clean, LuCI reachable and serving over HTTP.

> **The `.bin` files are not committed to git** (they are build artifacts). Build them
> yourself (`git checkout port/main-6.18 && ./build.sh`) or get them from a release attached
> to this branch if one exists, then verify:
>
> ```sh
> sha256sum --ignore-missing -c sha256sums.txt
> ```
>
> The checksums here are the authoritative record of what this build contains.

| file | use |
|---|---|
| `*-initramfs-kernel.bin` | RAM boot via the loader (TFTP/XMODEM) — **never writes flash**; a power-cycle discards it. Try this first. Built from `seed-min.config` (no LuCI, no wireless) if you need it to fit comfortably in this board's 64 MB RAM — see the status doc §5. |
| `*-squashfs-factory.bin` | **The one most people want.** Upload through stock's web UI ([OTA-INSTALL](../docs/OTA-INSTALL.md), no serial), or NOR-flash via the loader's `AUTOBURN`. Already carries the D-Link keyed-MD5 trailer. |
| `*-squashfs-sysupgrade.bin` | NOR flash via `sysupgrade` from a running OpenWrt. |

⚠ Both squashfs images **replace stock firmware**. Back up all 8 MB of NOR first —
after flashing, stock exists only in your backup. And read the root README's security
box: default images ship with the 5 GHz radio **disabled and unconfigured** (no SSID, no
key baked in) and **no root password set**.
