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
| `firmware` | `0x040000` | 7872 KB (`0x7b0000`) | kernel + rootfs (this is what boots) | **yes — replaced** |
| *(unmapped)* | `0x7f0000` | 64 KB | not in any partition | no |

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

> ⚠ **This step needs a root shell on the stock firmware, and that means the serial
> console.** Stock ships no SSH or telnet you can log into, so there is no
> network-only way to take this backup. Attach a 3.3 V UART to the board header
> (**38400 8N1**) — stock drops straight to a root prompt with no login. See
> [`BENCH.md`](BENCH.md) §5 for the header pinout and adapter notes.
>
> This is the honest caveat behind the "no serial needed" install: *flashing* needs no
> serial, but *making your own backup* — and *recovering a bad flash* — does. If you are
> not willing to open the case, your only fallback is official D-Link firmware for this
> exact revision, which may not be downloadable in your region (see below).

From that root shell, stream the flash off over the network — the chip is 8 MB, larger
than free RAM, so don't stage it on the device:

```sh
# on the router (stock):
cat /dev/mtd6 | nc <your-pc-ip> 5555      # on STOCK, mtd6 = the whole 8 MB ("ALL")
# on your PC:
nc -l -p 5555 > dir842-stock-full-8mb.bin
```

Verify it is 8388608 bytes. Keep it.

★ **Check `cat /proc/mtd` and select by name/size — never assume the numbers.** The mtd
numbering differs between firmwares: on **stock** the whole chip is `mtd6` ("ALL") and
the bootable region is `mtd5`; on **OpenWrt** there is no whole-chip device at all
(`mtd0`–`mtd3` = boot/MAC/config/firmware, and `mtd4`–`mtd6` are the kernel/rootfs/
rootfs_data split out of firmware). If you are taking a backup from OpenWrt instead, the
equivalent of the firmware region is `cat /dev/mtd3`.

---

## Route A — restore stock from a running OpenWrt (no serial cable)

This is the easiest path and is **verified**: streaming a stock `firmware`-region image
into `mtd write` puts the box back on stock D-Link firmware (version 3.0.3, kernel
3.10.90) on the next boot, MAC and calibration intact.

`mtd write` is exactly what stock's own updater does — the stock `fw_updater` binary is a
thin `fwupdater <mtd> <fw_file>` → `d_mtd_write_ex` wrapper — so this is the sanctioned
operation, not a hack.

You need a stock **firmware-region** image (`stock-firmware.bin` below). Get it from either:
- **your own backup** — the `firmware` partition is `dir842-stock-full-8mb.bin` bytes
  `0x40000..0x7F0000` (7872 KB). Carve it: `dd if=dir842-stock-full-8mb.bin of=stock-firmware.bin bs=64k skip=4 count=123`
- **D-Link's official firmware** for the DIR-842 rev Rx — see "Getting official firmware"
  below.

Then, because the file (~6–8 MB) is bigger than the router's free RAM, stream it straight
into `mtd write` over SSH instead of copying it to the device first:

```sh
# from your PC, router at 192.168.0.1:
head -c4 stock-firmware.bin        # sanity: must print cr6b (see the note below)
ssh root@192.168.0.1 'mtd -r write - firmware' < stock-firmware.bin
```

`mtd -r` reboots as soon as the write finishes — safer than a separate `ssh … reboot`,
which has to log in again against a filesystem that no longer matches flash.

The box comes up on stock — but ⚠ **not necessarily at `192.168.0.1`**. The stock
`config` partition (`mtd2`) is never touched by any install or restore here, so stock
boots with whatever settings it last saved — LAN IP, admin password, WiFi PSK and all.
On the development unit that meant `192.168.1.1` with the previous owner's
ISP-provisioned config, not D-Link defaults. If you don't find the box, check both
addresses (or watch the serial console / your DHCP lease); a factory reset
(hold the reset button) returns it to D-Link's defaults at `192.168.0.1`.
Conversely, if you *want* the old settings gone, that reset is on you — reflashing
alone does not clear them.

> Sanity-check the image first: `head -c4 stock-firmware.bin` should read `cr6b`. If it
> does not, the file has an extra outer wrapper (some web downloads do) — use Route B,
> whose loader accepts D-Link's file as-is.

---

## Route B — restore stock from the bootloader (serial cable, always works)

This is the bulletproof path and the one to use if the box won't boot, or if your stock
image isn't a bare `cr6b` region. It uses the RealTek loader's TFTP/AUTOBURN, the same
mechanism D-Link's own recovery uses.

1. Serial console at **38400 8N1** (not 115200 — the D-Link docs are wrong).
2. Put your PC on a **static `192.168.0.2/24`**, cabled to a LAN jack.
3. Power on and spam **Esc** to catch the loader → `<RealTek>` prompt. ★ There is exactly
   one ~1-second window per power-cycle, opening immediately at power-on — start spamming
   before you apply power.
4. At the `<RealTek>` prompt:
   ```
   IPCONFIG 192.168.0.1     # sets the LOADER's own IP (not your PC's)
   AUTOBURN 1               # 1 = write what arrives to flash (0 = RAM only)
   TFTP +                   # start the loader's TFTP SERVER and wait
   ```
5. ★ **The loader is the TFTP *server*; your PC is the client that PUTs the file.** This
   is backwards from every other TFTP recovery you have used. From your PC:
   ```sh
   curl -T stock-firmware.bin tftp://192.168.0.1/img
   # or:  tftp 192.168.0.1 -m binary -c put stock-firmware.bin img
   ```
6. Wait for **`Flash Write Successed`** on the console before removing power.
7. Power-cycle. Stock boots.

> The repo's `flash-nor.sh` scripts this exact cycle, but it is **written for the
> author's bench** (hardcoded log path, a specific USB NIC, and a smart-plug CLI for the
> power-cycling). Read it as a reference for the command sequence rather than expecting
> it to run as-is.

The loader writes whatever you give it to `0x40000`, so D-Link's official firmware file
works here directly, wrapper or not.

---

## Reinstalling OpenWrt (the other half of "at will")

Symmetric to the above — pick whichever matches where you are:

- **From stock, no serial (easiest):** upload this port's `…-squashfs-factory.bin` through
  the stock web UI's **System → Firmware Update → Local Update** page. Verified
  end-to-end on the hardware: stock accepts the image (loader-signed, but not D-Link-signed),
  writes it to the firmware
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

- D-Link support → search **DIR-842** → your **hardware revision** (printed on the label:
  this port needs **`H/W Ver.: R1`**) → *Firmware*.

⚠ **If your label does not say R1, stop.** The other DIR-842 revisions (B/C/…) are
entirely different SoCs — their firmware will not work here, and this port will not work
there. Availability also varies by region (the R1 stock build identifies as a D-Link
Russia image), so treat "I can just download stock again" as unverified until you have
actually found the file for your revision — which is why your own backup matters.

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
