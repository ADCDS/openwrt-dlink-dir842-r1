# Installing OpenWrt over the air — no serial cable

You can flash this port onto a stock DIR-842 rev R1 **entirely over the network**, through
D-Link's own firmware-update page — no UART adapter, no soldering, no exploit. You upload
the factory image exactly the way you would apply an official D-Link firmware update, and
the router reboots into OpenWrt. Going back to stock is just as reversible.

This is verified end-to-end on the hardware: stock firmware 3.0.3 → this port → and back,
repeatedly.

> **This is the easy path.** The serial / RAM-boot / loader methods in the main
> [README](../README.md#installing) still work and are the recovery route if anything
> goes wrong, but for a normal install you do not need them.

---

## Why this works

- **Stock's updater writes the uploaded file verbatim to flash.** The stock `fw_updater`
  binary is a thin `fwupdater <mtd> <file>` wrapper around a raw MTD write; the stock web
  layer performs **no firmware signature check**.
- **Our factory image is the exact on-flash format the router boots** — a RealTek `cr6b`
  kernel + squashfs rootfs, ending in the keyed-MD5 boot trailer the loader validates. The
  build bakes that trailer in (`dlink-md5-sign`), so **you do not sign anything** and you
  do not need `tools/sign-dlink.py`.
- Result: the stock web UI accepts our third-party (not D-Link-signed) OpenWrt image directly,
  writes it to the
  firmware region, and the loader boots it on the next reboot.

None of this touches your unit's MAC address or radio calibration — those live in separate
flash partitions the firmware update never writes (see
[RESTORE-STOCK.md](RESTORE-STOCK.md) for the partition map).

---

## Before you flash (do this once)

**Back up the whole 8 MB flash and keep it off the router.** It is the only restore
source guaranteed correct for *your* unit. Procedure:
[RESTORE-STOCK.md → "Before you ever flash"](RESTORE-STOCK.md#before-you-ever-flash-back-up-do-this-once).

> ⚠ **Honest caveat about "no serial".** *Installing* needs no serial — that is what this
> page demonstrates. But **taking the backup does** (it needs a root shell on stock, which
> only the serial console gives you), and so does **recovering a bad flash** (the RealTek
> loader). If you are not willing to open the case and attach a 3.3 V UART, then your only
> way back is official D-Link firmware for your exact revision, which may not be
> downloadable in your region. Decide that before you flash, not after.

Have the image ready: `openwrt-realtek-rtl8197f-GWR1200AC-V1-squashfs-factory.bin`.
Download it from the repo's **[latest release](https://github.com/ADCDS/openwrt-dlink-dir842-r1/releases/latest)**
(the `.bin` files are release assets — they are not committed into the git tree) and
verify it against the published `sha256sums.txt`:

```sh
sha256sum --ignore-missing -c sha256sums.txt
```

(`--ignore-missing` matters: `sha256sums.txt` lists all three images, so without it a
verification of the one file you downloaded exits non-zero and looks like a failure.)

> ℹ **Why is the file called `GWR1200AC-V1`?** This port reuses OpenWrt's existing
> Greatek GWR1200AC-V1 device profile (same RTL8197F platform) rather than adding a new
> one, so every image carries that name. **These are the DIR-842 R1 images** — there is no
> separate DIR-842-named file to look for, and you should not go looking for genuine
> GWR1200AC firmware.


---

## Install: stock → OpenWrt

1. **Connect a computer to the router** — a LAN port, or its Wi-Fi — and let it get an
   address **by DHCP**. Do **not** set a static IP on your computer: the stock web UI only
   serves clients the router has actually leased an address to; an unknown/static client is
   bounced to the "troubleshooting" page and never reaches the admin UI. (This is not a
   security control — just how the stock UI treats clients it doesn't recognise. A normal
   DHCP client is recognised automatically.)

2. **Open the router's admin page** — the factory default is `http://192.168.0.1` (use
   whatever LAN address your unit is set to) — and log in with your admin username and
   password. The factory default is `admin` / `admin`; if you changed it, use yours.

3. Go to **System → Firmware Update → Local Update**.

4. **Choose file** → select the `…-squashfs-factory.bin`. (It is already signed for the
   loader — no extra step.)

5. Click **UPDATE FIRMWARE**. The page shows *"Firmware update is in progress. Please do
   not power the device off."* — wait about **one to two minutes** and do not cut power.

6. The router writes the image and reboots **into OpenWrt**. It comes up at
   **`192.168.0.1`** — this port's default, which happens to be the same as the D-Link
   default, so your address doesn't even change. Reconnect (DHCP) and you're on OpenWrt.

**What you get:** wired Ethernet, the RTL8367S switch, WAN/LAN, **5 GHz Wi-Fi**, and
**gigabit hardware NAT offload** (armed automatically at boot). The 2.4 GHz radio is *not*
in the published build — see the README's *2.4 GHz* section for why.

7. **Plug your uplink into the Internet jack.** WAN ships as a **DHCP client**, so a cable
   modem or an upstream router just works. If your ISP needs **PPPoE**, set it in LuCI —
   *Network → Interfaces → WAN → Protocol → PPPoE* — or from the shell:

   ```sh
   uci set network.wan.proto='pppoe'
   uci set network.wan.username='<isp-user>'; uci set network.wan.password='<isp-pass>'
   uci commit network; /etc/init.d/network restart
   ```

   Hardware offload works on either: the ASIC's masquerade IP is reprogrammed from the
   live WAN address per flow (PPPoE sessions included).

> ⚠ **Secure it before it faces a real network.** A default image boots an **open 5 GHz
> AP** and **no root password**. Set a WPA2 passphrase on the radio and a root password
> (`passwd`) first.

> This port is **pre-production**: it routes at gigabit line rate on the bench, but it has
> not been through months of household duty. Keep your backup, and don't make it the only
> thing between your family and the internet on day one.

### If nothing forwards right after a boot

The ASIC's L2 tables come up empty and cold unicast is not delivered until traffic has
flowed, so the boot service primes them with a few pings. Its default targets are the
development bench's peers, which simply time out on your network — normally real traffic
warms the tables within seconds and you never notice. If a freshly booted box reads as
100 % packet loss anyway, reprogram and re-warm the datapath by hand:

```sh
/etc/init.d/dir842-asic restart
```

That is idempotent and is the supported recovery. (It disarms hardware offload, reprograms
the ASIC tables, re-warms them, and re-arms offload.)

**The same command fixes a stalled large transfer.** There is a known intermittent bug
([issue #1](https://github.com/ADCDS/openwrt-dlink-dir842-r1/issues/1)): sustained
line-rate bulk traffic can latch the switch fabric, so a big download stalls while pings
and small requests keep working normally. `/etc/init.d/dir842-asic restart` clears it and
restores full speed (measured: stalled → 896 Mbit). If it bothers you more than the speed
is worth, run on the software path instead with
`echo 0 > /sys/module/rtl819x/parameters/hwnat` (~180 Mbit, unaffected).

### If the router's IP collides with something on your network

If the DIR-842's LAN address is the same as another gateway your computer already talks to
(a very common case is both being `192.168.1.1`), give the router its own isolated segment
rather than fighting the address overlap:

- **Best:** a USB-Ethernet dongle with the DIR-842 cabled straight to it — the router never
  touches your main network, and unplugging it reverses everything.
- Or reach it over Wi-Fi from an interface held in a separate routing context (a network
  namespace / VRF).

Do **not** just add a second interface in the same subnet to your main routing table —
route/ARP selection will silently send the router's traffic out the wrong interface.

---

## Revert: OpenWrt → stock

Just as reversible, and also over the network — from an OpenWrt root shell (SSH):

⚠ **Check the image first.** It must be a bare firmware region, starting with the RealTek
`cr6b` magic:

```sh
head -c4 stock-firmware.bin        # must print: cr6b
```

If it prints anything else, the file has an outer wrapper (some official downloads do) —
**stop** and use the loader route in [RESTORE-STOCK.md](RESTORE-STOCK.md#route-b--restore-stock-from-the-bootloader-serial-cable-always-works),
which accepts D-Link's file as-is. Writing a wrapped file to the firmware partition leaves
the box unbootable until you attach a serial cable.

```sh
# stock-firmware.bin = your backup's firmware region, or official D-Link firmware
ssh root@192.168.0.1 'mtd -r write - firmware' < stock-firmware.bin
```

`mtd -r` reboots the moment the write completes. Do that rather than a second `ssh …
reboot`: once the write lands, the running system's filesystem no longer matches what is
on flash, and a fresh login may not succeed.

The box boots stock again, MAC and calibration intact. Full details — where to get a stock
image, how to carve the firmware region from a full backup, and the serial-console
fallback — are in **[RESTORE-STOCK.md](RESTORE-STOCK.md)**.

> Note: OpenWrt's own LuCI *"Flash new firmware image"* page expects an OpenWrt
> **sysupgrade** file and will refuse a D-Link stock image. Revert with the `mtd write`
> command above, or from the RealTek loader (RESTORE-STOCK.md, Route B).

---

## What was verified

On this exact hardware, with no serial cable and no exploit: logged into stock 3.0.3's web
UI, uploaded the `…-squashfs-factory.bin` — signed for the *loader*, but not by D-Link — through **Local Update**, watched the
serial console show stock's own updater write the image to the firmware region
(`/dev/mtd5 … 0% → 100% → finish`), the box reboot, the loader validate the trailer
(`Jump to image start=0x81000000`), and **OpenWrt (kernel 4.14.187) boot to userspace**.
(The image used in that run was a local dual-band build; the *published* images are
wired + 5 GHz, and install identically.) The reverse direction (`mtd write` back to stock, then forward again) was
exercised repeatedly in the same session.
