# Prebuilt images

Built 2026-08-16 from this repo at commit `d26ad62` by the exact clean-room path the
README documents: fresh clone → `./build.sh` inside the Debian 11 (bullseye) container
from `docs/BENCH.md` §7. Nothing outside this repository went into them, which also
means **no 2.4 GHz radio** — these are wired + 5 GHz + hardware-NAT images (see the
README's "2.4 GHz" section for why that driver cannot be shipped).

Verify with `sha256sum -c sha256sums.txt`.

| file | use |
|---|---|
| `*-initramfs-kernel.bin` | RAM boot via the loader (TFTP/XMODEM) — **never writes flash**; a power-cycle discards it. Try this first. |
| `*-squashfs-factory.bin` | NOR flash via the loader's `AUTOBURN` (serial). Carries the D-Link keyed-MD5 trailer. |
| `*-squashfs-sysupgrade.bin` | NOR flash via `sysupgrade` from a running OpenWrt. |

⚠ Both squashfs images **replace stock firmware**. Back up all 8 MB of NOR first —
after flashing, stock exists only in your backup. And read the README's security box:
default images boot with an **open 5 GHz AP**, a placeholder 2.4 GHz PSK and no root
password.
