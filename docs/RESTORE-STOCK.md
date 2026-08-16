# Restoring the stock D-Link firmware (and reinstalling OpenWrt)

This port is **fully reversible**. Installing OpenWrt overwrites only the *firmware*
region of the NOR flash; the bootloader and — crucially — your router's **per-unit MAC
address and RF calibration** are in separate partitions that no install step touches. You
can go back to a pristine D-Link firmware at any time, and back to OpenWrt again, as often
as you like.

Everything below is verified on the hardware, both directions.

---

## What installing OpenWrt actually changes

The DIR-842 R1's 8 MB SPI-NOR is partitioned like this (names as OpenWrt sees them):

| mtd | offset | size | what | touched by install? |
|---|---|---|---|---|
| `boot` | `0x000000` | 128 KB | RealTek bootloader | **no** |
| `MAC` | `0x020000` | 64 KB | **your unit's MAC + RF calibration** | **no** |
| `config` | `0x030000` | 64 KB | stock config area | **no** |
| `firmware` | `0x040000` | 7.75 MB | kernel + rootfs (this is what boots) | **yes — replaced** |

So a "restore to stock" only has to rewrite the **`firmware`** partition. Your unit keeps
its own MAC and calibration throughout — which is also why you must **never** flash someone
else's full-flash dump onto your unit: it would graft their MAC and radio calibration onto
your hardware.

The firmware region is a RealTek `cr6b`-headered image (kernel `cr6b` + squashfs rootfs)
with a D-Link keyed-MD5 trailer. Both stock and this port use that exact format, which is
why the same flash tools restore either one.

---

## Before you ever flash: back up (do this once)

If you have not flashed yet, dump the whole chip from the running stock firmware and keep
it somewhere off the router. This backup is the *most reliable* restore source — it is
byte-for-byte your unit, trailer and all.

From a root shell on stock (or OpenWrt), stream the flash off over the network — the chip
is 8 MB, larger than free RAM, so don't stage it on the device:

```sh
# on the router:
cat /dev/mtd6 | nc <your-pc-ip> 5555      # mtd6 = whole 8 MB ("ALL")
# on your PC:
nc -l -p 5555 > dir842-stock-full-8mb.bin
```

Verify it is 8388608 bytes. Keep it. (If you only ever want to restore the *firmware*
region, `cat /dev/mtd3` — the 7.75 MB `firmware` partition — is enough, since MAC/config
survive anyway.)

---

## Route A — restore stock from a running OpenWrt (no serial cable)

This is the easiest path and is **verified**: streaming a stock `firmware`-region image
into `mtd write` puts the box back on stock D-Link 3.10.90 on the next boot, MAC and
calibration intact.

`mtd write` is exactly what stock's own updater does — the stock `fw_updater` binary is a
thin `fwupdater <mtd> <fw_file>` → `d_mtd_write_ex` wrapper — so this is the sanctioned
operation, not a hack.

You need a stock **firmware-region** image (`stock-firmware.bin` below). Get it from either:
- **your own backup** — the `firmware` partition is `dir842-stock-full-8mb.bin` bytes
  `0x40000..0x7F0000` (7.75 MB). Carve it: `dd if=dir842-stock-full-8mb.bin of=stock-firmware.bin bs=64k skip=4 count=123`
- **D-Link's official firmware** for the DIR-842 rev Rx — see "Getting official firmware"
  below.

Then, because the file (~6–8 MB) is bigger than the router's free RAM, stream it straight
into `mtd write` over SSH instead of copying it to the device first:

```sh
# from your PC, router at 192.168.0.1:
ssh root@192.168.0.1 'mtd write - firmware' < stock-firmware.bin
ssh root@192.168.0.1 reboot
```

The box comes up on stock. (Stock's LAN default is `192.168.0.1` with its own DHCP
server; give your PC a `192.168.0.x` address to reach the stock web UI.)

> Sanity-check the image first: `head -c4 stock-firmware.bin` should read `cr6b`. If it
> does not, the file has an extra outer wrapper (some web downloads do) — use Route B,
> whose loader accepts D-Link's file as-is.

---

## Route B — restore stock from the bootloader (serial cable, always works)

This is the bulletproof path and the one to use if the box won't boot, or if your stock
image isn't a bare `cr6b` region. It uses the RealTek loader's TFTP/AUTOBURN, the same
mechanism D-Link's own recovery uses.

1. Serial console at **38400 8N1** (not 115200 — the D-Link docs are wrong).
2. Power on and spam **Esc** to catch the loader → `<RealTek>` prompt.
3. Point the loader at your PC and AUTOBURN the stock image to the firmware region:
   ```
   IPCONFIG 192.168.0.1          # loader's own IP on the bench
   AUTOBURN 1
   TFTP 192.168.0.2 stock-firmware.bin    # your PC = TFTP client pushing the file
   ```
   (The repo's `flash-nor.sh` automates exactly this cycle — power-cycle, catch, upload,
   burn — for a factory image; point it at your stock image instead.)
4. Power-cycle. Stock boots.

The loader writes whatever you give it to `0x40000`, so D-Link's official firmware file
works here directly, wrapper or not.

---

## Reinstalling OpenWrt (the other half of "at will")

Symmetric to the above — pick whichever matches where you are:

- **From stock, no serial (easiest):** upload this port's `…-squashfs-factory.bin` through
  the stock web UI's **System → Firmware Update → Local Update** page. Verified
  end-to-end on the hardware: stock accepts the unsigned image, writes it to the firmware
  region, and OpenWrt boots. Step-by-step: **[OTA-INSTALL.md](OTA-INSTALL.md)**.
- **From stock, serial:** loader AUTOBURN the `…-squashfs-factory.bin` (Route B, verified).
- **From a running OpenWrt:** `sysupgrade -n …-squashfs-sysupgrade.bin` (verified, survives
  cold power-cycles).

See the main [README](../README.md) "Installing" section for the full first-time flow.

---

## Getting official D-Link firmware

This repository does **not** redistribute D-Link's stock firmware — it is D-Link/Realtek
proprietary, and any single unit's full-flash dump also contains that unit's MAC and
calibration, which must not be shared. Download the official firmware for your exact
hardware revision from D-Link's support site:

- D-Link support → search **DIR-842** → your **hardware revision** (printed on the label,
  e.g. `Rev. B1`) → *Firmware*.

Match the hardware revision exactly. Then use Route A (if it's a bare `cr6b` region) or
Route B (always).

---

## Safety notes (all learned on this hardware)

- **Keep a full 8 MB backup off the router.** It is the one restore source that is
  unquestionably correct for *your* unit.
- **Never flash another unit's full-flash dump** — you'd overwrite nothing important (MAC
  is a separate partition) *only if* you restrict yourself to the `firmware` region; a
  whole-chip write would clobber your MAC/calibration with theirs.
- The `boot`, `MAC`, and `config` partitions are never touched by any procedure here, so a
  bad firmware write is always recoverable from the loader (Route B).
- Serial is **38400 8N1**.
