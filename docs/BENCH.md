# The bench: hardware, wiring, serial, power, build container, and the automation loop

Everything needed to physically reproduce or resume work on this port: the rig, the console,
the unattended boot/flash loop, the build environment, and the operational gotchas that cost
the most time. The bench is not incidental to the result — an unattended power-cycle → catch →
TFTP → boot loop is what made a multi-month register-level reverse-engineering project
tractable on a box with no `devmem`, no `kexec` and exactly one loader window per power-on.

Committed bench scripts live at the **repo root** (`../ramboot.sh`, `../flash-nor.sh`,
`../bootgate.sh`, `../bench-up.sh`, `../bench-wan-emu.sh`) and are byte-identical to the
originals on the bench host (verified by `md5sum`). A few helpers named below —
`hwnat-ab.sh`, `hwnat-measure.sh`, `container-fast.sh`, `catch-robust.sh`, `kb.sh`, `kbm.sh` —
are **not** in this repo; where they matter their contents are reproduced or described here.

---

## 1. The rig at a glance

```
  build host: /dev/ttyUSB0 (USB-UART, 38400 8N1) --------> DIR-842 UART header
  build host: USB3 GbE NIC (RTL8153B, r8152) ------------> a DIR-842 LAN jack   = 8367S port 2
  WAN peer  : Raspberry Pi 4, eth0 enslaved to br0 ------> DIR-842 Internet jack = 8367S port 4
  smart plug ------------------------------------------> DIR-842 mains

  Inside the box, two cascaded switches:
    CPU eth0 -- SoC internal switch (rtl865x) -- SoC port0 RGMII trunk <=> 8367S EXT1 = port 6
                                                                          |- ports 0-4 = the 5 jacks
```

Three cables and a switchable outlet. That is the whole rig. The two things that make it
*unattended* are the smart plug (§6) and the byte-offset serial log slicing (§5) — with those,
`ramboot.sh` goes from a cold box to a root shell with no human in the loop.

---

## 2. Hosts and roles

| host | hardware | role |
|---|---|---|
| build host (`hal`) | Debian 13 trixie, kernel 6.12.x amd64, Intel i7-8750H (12 threads), 16 GB RAM | **everything**: build host, serial console, LAN peer, TFTP client, iperf3 client, power controller |
| WAN peer (`tiny`) | Raspberry Pi 4 | WAN peer + `iperf3 -s`. Nothing else. |
| — | Tuya smart plug (Ekaza EKGC-T206M) | mains power control for the DIR-842 |

**Passwordless sudo is required on both.** Every bench script calls `sudo ip` / `sudo nmcli`
locally, and the Pi steps run as `ssh tiny 'sudo ip addr add ...'` (`bench-up.sh:28,48`).

★ **The build host is DUAL-HOMED and that matters more than anything else in this document.**
Its onboard NIC (`enp3s0`, `r8169`) is on the house LAN; the USB NIC (`enx00e04c125990`,
`r8152`) is the bench. NetworkManager keeps re-adding a default route via the house gateway,
which silently steals `172.16.0.0/24`. This is the single most-documented failure mode of the
whole bench — see the comment block at `bench-up.sh:41-46`, which records that it once cost a
wrong published conclusion about `SWTCR0.WANRouteMode`.

The Pi's **wired** port is dedicated to the bench. It stays reachable over WiFi plus a
Tailscale overlay, which is why the control channel survives every box reboot, VLAN mistake and
NAT failure — you can always still `ssh tiny` even when the bench wire is completely dead. Its
bench IP lives on bridge `br0`, **never on `eth0`** (`eth0` is a `br0` slave), and is lost on
reboot; both `bench-up.sh` and the measurement harness re-add it defensively.

---

## 3. Wiring and the switch port map

The five GbE jacks belong to the **external RTL8367S**, not to the SoC. The SoC reaches them
over a single RGMII trunk. Hardware-confirmed map:

| 8367S port | role | evidence |
|---|---|---|
| 0–3 | the four LAN jacks | `files/target/linux/realtek/dts/GWR1200ACV1.dts:115` |
| **2** | the jack the build host is cabled to | `bench-up.sh:9` |
| **4** | the WAN / Internet jack — the Pi | `GWR1200ACV1.dts:115` |
| **6** | **EXT1** = the RGMII trunk to the SoC; `cpu_port = <6>` (`GWR1200ACV1.dts:121`). It is **EXT1, not EXT0** — see the comment at `:111-119` | `GWR1200ACV1.dts:111-121` |
| 5, 7 | unused; only ever appear as `link:down` in ~31 MB of captured console | bootlog |

SMI management of the 8367S is **bit-banged GPIO**: MDC = `gpio0 18`, MDIO = `gpio0 19`
(`GWR1200ACV1.dts:112` and the `gpio-sck` / `gpio-sda` properties at `:123-124`).

Link state, from `swconfig dev switch0 show` in the captured console:

```
	link: port:2 link:up speed:1000baseT full-duplex txflow rxflow auto
	link: port:4 link:up speed:1000baseT full-duplex txflow rxflow auto
	      port:6 link:up speed:1000baseT full-duplex txflow rxflow
```

⚠ **`bench-up.sh:10` still says `8367S port 4, 100M` — that is STALE.** The 100 Mb WAN cable was
replaced mid-project. The console log proves the chronology directly: the last
`port:4 link:up speed:100baseT` sighting is at byte offset **24,664,958**, the last
`port:4 link:up speed:1000baseT` at **30,847,046**, in a 31,040,846-byte log. **Several early
throughput numbers predate that swap** and are capped by the cable, not by the box.

### VLAN plumbing over the single trunk

```
swconfig vlan 1 = WAN = VID1 = ports '4 6t'
swconfig vlan 2 = LAN = VID2 = ports '0 1 2 3 6t'
```

(`files/target/linux/realtek/base-files/etc/uci-defaults/99-dir842-m5:49-58`.)
Linux splits the single CPU netdev into `eth0.1` (WAN) and `eth0.2` (LAN).

**There is no `eth1`, and it must never be declared.** `board.d/02_network:10-17` records why:
a phantom `eth1` made `config_generate` emit a `wan_eth1_dev` device section and hang the
per-unit WAN MAC off a device that never appears, so `eth0.1` silently inherited the LAN MAC —
which breaks the LAN = WAN+1 invariant and blackholes CPU↔WAN traffic while HW-routed frames
keep working.

★ **GOTCHA THAT COST A LONG DEBUG: the `rtl8367b` swconfig driver IGNORES the uci `vid=`
attribute and uses the VLAN *index* as the VID.** So `vlan '1'` ⇒ 8367S VID1 regardless of what
`vid=` says. That silently produced 8367S vid1 = LAN / vid2 = WAN — the exact opposite of the
SoC — and looked like a hardware forwarding failure for an entire session. The numbering must
therefore be **WAN = vlan1 / LAN = vlan2** to match the SoC ASIC. Recorded in full at
`99-dir842-m5:16-22`; found with the forced-on `rtl8367b` debugfs `vlan_mc` dump (§11.6).

> **Scope note.** The shipped datapath has since moved to unconditional **CPU-tag mode** — the
> SoC MAC inserts/strips a 4-byte Realtek 0x8899 tag on the trunk in hardware and the jacks
> become the SoC's own ports 0–4 with the CPU on port 8
> (`rtl819x-eth.c:391-418`; "Fork A", the VLAN-only model, was removed in `86880757ed`). The
> per-jack **VLAN membership above is still programmed and still how Linux demuxes**: the
> driver derives the VID from the CPU-tag source jack and re-attaches it as a hwaccel ctag so
> 8021q splits `eth0.2` / `eth0.1` (`rtl819x-eth.c:1165-1180`). The bench port map is unchanged
> by that transition.

---

## 4. Addressing and MACs

| thing | address | evidence |
|---|---|---|
| bench LAN subnet | `192.168.0.0/24` | |
| box LAN (`br-lan` / `eth0.2`) | `192.168.0.1` | `99-dir842-m5:61` |
| build host bench NIC | `192.168.0.2/24` static | `bench-up.sh:23` |
| bench WAN subnet | `172.16.0.0/24` | |
| box WAN (`eth0.1`) | `172.16.0.1` static — **load-bearing, it is ASIC `extIP[0]`** | `99-dir842-m5:67` |
| Pi WAN peer | `172.16.0.2/24` on `br0` (earlier in the project: `172.16.0.9`) | `bench-up.sh:28` |
| host route to the WAN side | `172.16.0.0/24 via 192.168.0.1 dev <bench NIC>` | `bench-up.sh:24,47` |

★ **The bootloader's own IP resets every single time.** The nvram default is the factory
`192.168.1.6`; the bench link is `192.168.0.0/24`. It must be re-pinned each round with the
loader's `IPCONFIG` command (`ramboot.sh:58-63`, `flash-nor.sh:64`). When the loader is *not*
caught it autoboots by trying to TFTP `fw.bin` from `10.0.0.2` and harmlessly fails —
`TFTP form Server: 10.0.0.2; Filename 'fw.bin'.` in the console is the fingerprint of a missed
catch, not of a fault.

### MACs

| device | MAC |
|---|---|
| D-Link per-unit **base** MAC (from `mtd1+0x00`) | `e0:1c:fc:51:c9:ee` |
| loader's own MAC (= LAN MAC = base+1) | `e0:1c:fc:51:c9:ef` |
| Linux LAN `eth0.2` / `br-lan` | `e0:1c:fc:51:c9:ef` |
| Linux WAN `eth0.1` | `e0:1c:fc:51:c9:ee` |
| old Realtek shared defaults (pre-R3) | `00:e0:4c:81:96:c2` LAN / `:c3` WAN |
| Pi | `e4:5f:01:04:98:af` |
| build-host bench NIC | `00:e0:4c:12:59:90` |

★ **A FAMOUS WARNING IN THE SCRIPTS IS NOW OBSOLETE — corrected here.**
`bench-up.sh:18-20` and `flash-nor.sh:17-18` both warn:

> *"never pin a static ARP for 192.168.0.1 — the loader answers on `e0:1c:fc:51:c9:ef` while
> Linux uses `00:e0:4c:81:96:c2`"*

That was true **until commit `484e4db4bd`** ("R3: per-unit MAC now deterministic — one source
of truth (board.d)"), which made `board.d` read the 17-byte ASCII per-unit MAC at `mtd1`
offset 0 (`02_network:59-64`). **Linux's LAN MAC now equals the loader MAC**, so the conflict
cannot occur. The comments are stale — see §13.

The invariant that **does** still matter is **LAN = WAN + 1** (`99-dir842-m5:70-78`): `gw_prog`
programs the ASIC's WAN netif from `eth0.1`'s *live* MAC and rewrites HW-routed egress source
addresses to it, so a software/hardware MAC disagreement blackholes every CPU↔WAN packet while
HW-routed traffic keeps working — a genuinely confusing failure mode.

`mtd1` ("MAC") holds **two overlaid schemas**:

| offset | schema | note |
|---|---|---|
| `0x00` | D-Link per-unit base MAC, 17-byte **ASCII** string | = the WAN MAC; LAN = base+1; WiFi BSSID = base+2 (`99-dir842-m5:106-112`) |
| `0x13` | Realtek default **binary** `macAddr[]` block | **identical on every unit** — `board.d` used to read this, so every board came up with the same MAC |

---

## 5. Serial console

Device: `/dev/ttyUSB0`.

★ **BAUD IS 38400 8N1, no flow control.** The D-Link / vendor documentation's 115200 is WRONG
and yields pure garbage (a captured `bootlog-115200-garbage.txt` exists on the bench). The exact
line settings used everywhere:

```bash
stty -F "$PORT" 38400 cs8 -parenb -cstopb -crtscts -ixon clocal raw -echo
```

(`ramboot.sh:20`, `flash-nor.sh:29`.)

Box side: a 16550A at MMIO `0x18147000`. The DTS declares SoC interrupt `<9>`
(`RTL8197F.dtsi:67-77`); Linux enumerates it as virq 17 —
`ttyS0 at MMIO 0x18147000 (irq = 17, base_baud = 6250000) is a 16550A`. Kernel arg is
`console=ttyS0,38400` (`GWR1200ACV1.dts:30`). **It drops to a BusyBox ash root shell with no
login** — which has consequences, see §11.4.

**Adapter.** The project's older notes say Silicon Labs CP2102 / `cp210x`. The adapter
physically in use at the end of the project is a **CH340** (USB ID `1a86:7523`, driver
`ch341-uart`, USB full-speed) — verified live on the build host. Note the discrepancy honestly:
**the `cp210x` unbind/bind recovery recipe in the older notes does not apply to a CH340.** The
adapter was swapped at least once mid-project (see §11.5), so treat any adapter-specific advice
in older material as provisional.

### Read path — there is no `screen`

A detached background `cat` appends the raw port to one ever-growing log:

```bash
setsid bash -c "cat /dev/ttyUSB0 >> ~/dir842-r1-bootlog.txt" </dev/null &
```

(`ramboot.sh:24`, `flash-nor.sh:34`, `bootgate.sh:12` — each guards with
`pgrep -f "cat $PORT"` so it is safe to re-run.)

Scripts then read the box by **byte-offset slicing**:

```bash
START=$(wc -c < "$LOG")
# ... do the thing ...
tail -c +"$START" "$LOG" | grep -a "Escape booting by user"
```

This is why one append-only log works for an entire multi-month project instead of a terminal
emulator: every step records its own offset and only ever looks at its own slice. The log
reached **31,040,846 bytes / ~494 k lines**.

Write path is always a direct write to the device node:

```bash
printf 'cmd\r' > /dev/ttyUSB0
```

Never `screen -X stuff` — see §11.3.

---

## 6. Power control — the thing that makes it unattended

A **Tuya smart plug driven LOCALLY over the LAN** (protocol 3.5 via `tinytuya`) — no cloud, no
DNS. Concrete hardware used here: an **Ekaza EKGC-T206M** (chip Lightning LN882HKI, energy
meter BL0937). It sits on an isolated IoT VLAN with **no WAN route**; credentials (`devId` +
`localKey`, `chmod 600`) live outside this repo and the `localKey` rotates on every re-pairing.

The bench only ever touches it through a one-command wrapper:

```
tomada on | off | toggle | status | watch [seconds] | json
```

`status` also reports V / mA / W. It is referenced as `TOMADA="${TOMADA:-...}"` in
`ramboot.sh:16`, `flash-nor.sh:25` and `bootgate.sh:9`.

**Any remotely switchable outlet works — only the interface matters.** Point `TOMADA` at
anything that accepts `on` and `off` and the whole automation loop works unchanged. Without it,
**a human must power-cycle for every single iteration**, and `bootgate.sh`'s ten-cold-boot
reliability gate is simply not runnable.

---

## 7. The build environment

A Docker image built `FROM debian:bullseye` (Debian 11).

**Why Debian 11 is required:** the ggbruno fork this port builds on is a ~2019/2020 tree
(kernel **4.14.187**, target gcc **8.4.0**). Bullseye still ships `python2` /
`python-is-python2` and the older host tools the old buildroot expects; modern toolchains fail
to build those host tools. (`README.md:105-107` and `build.sh:16-18` say the same thing in
one line each.)

```dockerfile
FROM debian:bullseye
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
      gettext git libncurses5-dev libssl-dev rsync unzip zlib1g-dev file \
      wget curl subversion swig time xsltproc libelf-dev bc python3 python2 \
      python-is-python2 ca-certificates ccache quilt && \
    rm -rf /var/lib/apt/lists/*
RUN useradd -u 1000 -m -s /bin/bash builder     # OpenWrt refuses to build as root;
USER builder                                    # uid 1000 owns the bind-mounted tree
ENV HOME=/home/builder FORCE_UNSAFE_CONFIGURE=1
WORKDIR /build/openwrt
```

Toolchain coordinates: target `mipsel_24kc_musl`, gcc 8.4.0, kernel 4.14.187. Staging dir is
`staging_dir/toolchain-mipsel_24kc_gcc-8.4.0_musl`. Images land in
`openwrt/bin/targets/realtek/rtl8197f/`:

```
openwrt-realtek-rtl8197f-GWR1200AC-V1-initramfs-kernel.bin
openwrt-realtek-rtl8197f-GWR1200AC-V1-squashfs-factory.bin
openwrt-realtek-rtl8197f-GWR1200AC-V1-squashfs-sysupgrade.bin
```

### Incremental kernel builds — ~3 min instead of ~15

The recipe (from `container-fast.sh`, **not in this repo**, but worth publishing):

1. Copy the edited file out of the overlay (`target/linux/generic/files` or
   `target/linux/realtek/files-4.14`) into
   `build_dir/target-mipsel_24kc_musl/linux-realtek_rtl8197f/linux-4.14.187/` and `touch` it.
2. `rm -f $KDIR/.image` — forces the kernel-image sub-step to re-run.
3. `make target/linux/compile V=s` — **deliberately NO `make target/linux/clean`**, which would
   trigger a full rebuild.
4. `make -j$(nproc)` to re-wrap the image.

For a **DTS change**, also delete `build_dir/.../image-GWR1200ACV1.dtb*` first.

### Driver-only, even faster

`rsync` the driver sources into `build_dir` and run kbuild directly, bypassing the OpenWrt make
wrapper:

```bash
PATH=<staging_dir>/toolchain-mipsel_24kc_gcc-8.4.0_musl/bin:$PATH \
STAGING_DIR=<staging_dir> \
make ARCH=mips CROSS_COMPILE=mipsel-openwrt-linux-musl- KBUILD_HAVE_NLS=no -j12
```

Running the full `vmlinux modules` link is what surfaces undefined symbols — a module-only
build will not.

★ **VERIFY A CHANGE ACTUALLY LANDED:**

```bash
strings build_dir/.../linux-4.14.187/vmlinux | grep <token>
```

Grepping the final `.bin` returns nothing because it is **LZMA-compressed**. More than one
"the fix didn't work" was actually "the fix wasn't in the image".

---

## 8. The automation loop: `ramboot.sh`, `flash-nor.sh`, `bootgate.sh`, `bench-up.sh`

### `../ramboot.sh` — unattended RAM boot, no flash writes

★ **THE TIMING TRICK IS THE WHOLE SCRIPT** (`ramboot.sh:7-11`). The box *hangs* after a bad
flash boot (no watchdog reboot), so there is **exactly one loader window per power-cycle** and
it opens **within ~1 s of power-on**. `tomada on` itself takes 1–2 s to return, so spamming ESC
only after it returns misses the window every single time. The order must be:

```
power OFF  ->  start the ESC spammer in the background  ->  power ON while it is already
spamming  ->  foreground watches the log for the catch
```

Per round (up to 3 rounds):

1. `stty` the port; ensure the `cat >> bootlog` logger exists.
2. `tomada off; sleep 3`.
3. Background spammer — **a direct write to the device node at 25 Hz**:
   ```bash
   ( while [ -f "$FLAG" ]; do printf '\033' > "$PORT" 2>/dev/null; sleep 0.04; done ) &
   ```
4. `START=$(wc -c < "$LOG")`; `tomada on`.
5. Poll up to 60 s for the literal string `Escape booting by user`; stop the spam.
6. **RE-ASSERT HOST NETWORKING EVERY ROUND** (`ramboot.sh:49-55`). NetworkManager strips the
   IPv4 off the USB NIC and then `curl` fails silently while the catcher keeps re-catching —
   which looks like an infinite `<RealTek>` loop and is not.
7. `ip neigh flush` + `ping 192.168.0.1`.
8. `IPCONFIG 192.168.0.1` to re-pin the loader's nvram IP.
9. Up to 4 TFTP tries:
   ```
   AUTOBURN 0          # 0 = load-to-RAM + jump. NEVER writes flash.
   LOADADDR 81000000
   TFTP +
   curl -s --connect-timeout 8 --max-time 90 -T "$IMG" tftp://192.168.0.1/img
   ```
   ★ **The LOADER runs the TFTP server**; the host is the client that `PUT`s the image. This is
   backwards from every other TFTP recovery you have used.
10. `J 81000000`, then poll 90 s for `Please press Enter to activate`.

**Serial-only fallback:** `XMOD 81000000`, then send the image over XMODEM — **~18 min at
38400**. ⚠ `XMOD` takes a **bare hex address**; `0x...` is rejected.

A harder-edged variant, `catch-robust.sh` (**not in this repo**), does **12 rounds of 180 s of
continuous ESC** with **no power control at all**, riding the box's own watchdog reboots. Use it
when the plug is unavailable or the box is watchdog-cycling on its own.

**Loader command set worth knowing:** ESC-spam during power-on ⇒ the `<RealTek>` prompt;
`AUTOBURN 0|1`, `LOADADDR`, `J`, `TFTP`, `XMOD`, `FLR` / `FLW` / `ERASESECTOR`, `IPCONFIG`,
`BOOT`.

### `../flash-nor.sh` — burn to NOR

Identical catch machinery, then:

- `AUTOBURN 1` (write flash) with **no `J`**.
- `TFTP +`; `curl -T $IMG tftp://192.168.0.1/img` (200 s budget).
- **Sanity-check that the loader actually received it** before waiting on anything:
  `grep -aqiE 'File: img|Upload File'` (`flash-nor.sh:72`).
- Wait up to 180 s for `Flash Write Success` — the box prints the literal
  `Flash Write Successed`. **DO NOT POWER OFF during this.**
- Then `BOOT=1` power-cycles with **no ESC spam** and greps the capture for
  `Oops|Unable to handle|unaligned|Call Trace|epc |Kernel panic|Please press Enter|VFS: Mounted|jffs2`.

⚠ **`AUTOBURN` writes the ENTIRE uploaded file at `0x40000`** — only ever feed it a real factory
image (`flash-nor.sh:19-20`). It never touches `mtd0` (the boot partition), which is what makes
the loader an unbrickable escape hatch.

### `../bootgate.sh` — the reliability gate

```bash
N=10 bash bootgate.sh
```

N unattended cold power-cycles, `WAIT=50` s each, scoring per boot:

- (a) **no** `Oops|Unable to handle kernel|unaligned access|Kernel panic|BUG:`
- (b) reached `Please press Enter to activate`

It also counts `switching to jffs2 overlay` as informational. This is the difference between
"it booted once" and "it boots".

### `../bench-up.sh` — every-boot bring-up, run AFTER `ramboot.sh`

★ **THE ORDER IS LOAD-BEARING.** Skipping or reordering any step reproduces "100% packet loss"
that looks like a driver bug and is just cold state (`bench-up.sh:2-4`):

1. Unmanage the NIC in NetworkManager, add `192.168.0.2/24`, add the `172.16.0.0/24` route,
   flush ARP.
2. Re-add the Pi's `172.16.0.2` on `br0`.
3. `cat /proc/rtl865x_gw` — programs ASIC routes / mode / extIP / ACL, and **WIPES THE L2
   TABLES**, so it must **precede** the warm-up.
4. Ping-loop both peers 3× to warm the ASIC L2/ARP tables — **cold entries = 100% loss**.
5. `echo 1 > /sys/module/rtl819x/parameters/hwnat`.
6. **Re-assert the route and the Pi's address immediately before measuring** (`bench-up.sh:47-48`).
7. Verify — including a `ping -s1400` large-frame check, the RX-wedge screen (`bench-up.sh:54`).

### The signing step

`../tools/sign-dlink.py` (published; identical to the bench original). The v3.4.11B loader
verifies a **forgeable keyed MD5** before booting an image from flash:

```
[cvimg image][ MD5(key || cvimg_image) : 16 ][ 00 C0 FF EE : 4 ]
```

```bash
tools/sign-dlink.py in.bin out.bin [entry]
```

It asserts the `cr6b` magic and can patch the big-endian entry address.

**Three facts that exist nowhere else in this repo:**

1. **The entry point must be `0x81000000`** — the lzma-loader `LZMA_TEXT_START` — **not**
   Greatek's `0x80a00000`.
2. **The loader accepts ONLY the `cr6b` signature.** `cs6c` and `H601` give `no sys signature`,
   and `cvimg -c new -t kernel` emits `cs6c`.
3. **The verification that anchors the whole crack**, re-run against the stock 8 MB dump while
   writing this document:

   ```
   size 8388608 (0x800000)
   cvimg magic at 0x40000:            b'cr6b'
   MD5(key || stock[0x40000:0x67d4d6]) = 758d2b85976798e0925eb15be9f9fc3a
   stored digest at 0x67d4d6..0x67d4e5 = 758d2b85976798e0925eb15be9f9fc3a   <- byte-exact
   magic  at 0x67d4e6..0x67d4e9        = 00c0ffee
   ```

★ **A correction to `tools/sign-dlink.py:4` and `README.md:135`.** Both say the key is
*"embedded in the bootcode (`mtd0`) at `0x291bc`"*. That offset is **not** into the raw NOR
image — absolute `0x291bc` in the 8 MB dump reads `0xFF` (erased), and the key appears nowhere
in the raw dump in any encoding. `0x291bc` is an offset into the **decompressed** bootcode:
`mtd0` is a tiny raw first stage plus a **gzip stream at `mtd0+0x7d70`**, which inflates to
**191,632 bytes** (links at `0x80000000`), and at `0x291bc` of *that* sits the key as a
NUL-terminated 32-char ASCII string:

```python
dec = zlib.decompressobj(16+zlib.MAX_WBITS).decompress(flash[0x7d70:0x20000])
dec[0x291bc:0x291bc+34]   # b'8cefeb7b3b274629be7df7d453a64c29\x00\x00'
```

Anyone trying to re-derive the key from a flash dump alone will fail until they know that.

---

## 9. The WAN-emulation fallback (no second machine)

When the WAN cable died, the bench was rebuilt to need **no second machine** —
`../bench-wan-emu.sh`:

```bash
swconfig dev switch0 vlan 1 set ports "4 2t 6t" && swconfig dev switch0 set apply
```

That puts the host's LAN jack (**port 2**) into the **WAN VLAN as tagged**, alongside the real
WAN jack. The host then runs an 802.1Q **vid-1** subinterface `wanp` inside network namespace
`benchns`, holding `172.16.0.2/24` + `default via 172.16.0.1`. **The namespace boundary forces
the traffic onto the wire**, so the box routes and NATs it exactly as for a real WAN host — at
1000baseT.

★ **vid 1 is REUSED DELIBERATELY, and this is a DRIVER constraint, not a bench detail.** The SoC
internal switch only has VLAN entries for **vid 1 and vid 2** (`sw_add_vlan`), and TX on any
other VID is **silently dropped**, because `ph_vlanId` must name a VID whose SoC member mask
covers the portlist. A vid 3 was tried first and ARP simply never resolved.
`bench-wan-emu.sh:14-17` cites `rtl819x-eth.c:1134`; that line number is from the tree state
when the script was written — in the current tree the constraint is documented at
**`rtl819x-eth.c:1269-1276`** with the code at `:1277-1280` (`nicTx.vid` from the hwaccel tag,
defaulting to 2).

⚠ **Bench only. Never ship it.** It puts a LAN jack in the WAN VLAN, and **every byte crosses
the host NIC twice** (in on vid2, out on vid1), so the wire carries ~2× the payload.

---

## 10. Measurement harness

The working harness is `hwnat-ab.sh` — **not in this repo**. It A/Bs `hwnat=0` vs `hwnat=1` in
both directions. **Every assert in it exists because a false conclusion was published and
retracted** (`hwnat-ab.sh:4-21`):

1. **Frame-size preflight** at 64 B *and* 1400 B, 0% loss required. A box in the large-frame RX
   wedge (`docs/M7-LARGE-FRAME-RX-WEDGE.md`) still answers default-size pings, so wedged runs
   were being scored as results.
2. **Flow-source assert.** `iperf3 -s` handles **one** test at a time, and a timeout-killed
   client wedges it into accepting nothing — silently turning trials into zero-byte runs that
   were read as negatives. Restart it per measurement **and** confirm `ss -lnt` shows
   `172.16.0.2:5201`.
3. **Bytes-through-CPU vs payload** — never throughput, never packet counts. GRO makes CPU-side
   packet counts meaningless and throughput alone cannot distinguish "offloaded" from "the CPU
   is keeping up". Metric is `delta(/proc/net/dev rx) / payload`: `eth0.2` for upload, `eth0.1`
   for download.
4. **Re-assert the host route and the peer address immediately before every single
   measurement** (both drift; §11.11).

**Rule: NEVER MEASURE OUTSIDE THE HARNESS.** A hand-run `iperf3` that bypassed the very assert
added in the same session produced a false *"TCP dies only when rows install"* result —
retracted in `97ba3369e4`.

⚠ **`hwnat-measure.sh` (also not in the repo) is STALE.** It still sweeps
`/sys/module/rtl819x/parameters/napt_idx_htonl` (`hwnat-measure.sh:70-76`), a knob **deleted in
`3ea4906a25`** ("delete the ASIC byte-order knobs — the values are numeric, unconditionally").
It will fail if run.

### Final harness result

Over the real gigabit WAN jack, Pi as `iperf3` server:

| run | throughput | payload bytes through CPU | CPU busy |
|---|---|---|---|
| `hwnat=0` UP | 184 Mbit | 101.1 % | 51.9 % |
| `hwnat=0` DOWN | 183 Mbit | 100.3 % | 50.0 % |
| **`hwnat=1` UP** | **889 / 892 Mbit** | **0.0 %** | 0.3 / 0.4 % |
| **`hwnat=1` DOWN** | **896 / 895 Mbit** | **0.0 %** | 0.3 / 0.7 % |

---

## 11. ★ Operational gotchas that cost the most time

This is the highest-value section of the document. Each item is a real day lost.

### 11.1 A printk burst truncates the command you are typing and wedges the shell at `>`

The console log interleaves `ttyS ttyS0: 1 input overrun(s)` (105 occurrences) with a ~3/s
`INFO: flash mode error, cmd 0x04` flood (63 occurrences). A dropped character mid-quote leaves
the shell at a `>` continuation prompt. **Three separate sessions were lost to a wedged `>`.**

`Ctrl-C` often does **not** clear it (it echoes `^C` without aborting). The reliable recovery is
to send the **matching quote first**, then `\003`; for a heredoc-like continuation, `Ctrl-D`
(`\004`).

Mitigation, encoded in `hwnat-ab.sh:19-21,29-30`: run `dmesg -n 1` first, keep commands short,
and send them in **6-character chunks with 0.10 s gaps**:

```bash
slow() { local s="$1" i; for ((i=0;i<${#s};i+=6)); do printf '%s' "${s:i:6}" > "$PORT"; sleep 0.10; done
         printf '\r' > "$PORT"; sleep "${2:-3}"; }
```

**A maintainer who does not know this will read a wedged shell as a crashed box and
power-cycle, losing the state being measured.**

*(Update: the specific `flash mode error, cmd 0x04` flood was root-caused to a mislabelled
`pr_info` in the SPI driver and demoted; the string no longer exists in the shipped
`files/target/linux/realtek/files-4.14/drivers/spi/spi-sheipa.c`. The mechanism — any printk
burst eating console input — is unchanged.)*

### 11.2 `dmesg -n 1` is not always sufficient

The driver carries prints in the RX hot path that a runtime loglevel knob does not fully
suppress. In the current tree they are bounded but still unconditional in form:
`rtl819x_swnic.c:409-416` (a static counter, first 40 frames) and `rtl819x-eth.c:1223-1227`
(every 1024th poll, plus every descriptor run-out). Silencing them for a quiet measurement
requires a **rebuild**, not a runtime knob.

*(The older note that "RAM images boot with `ignore_loglevel`" could **not** be verified against
the current tree: the only `bootargs` is `console=ttyS0,38400` at `GWR1200ACV1.dts:30`, and
`ignore_loglevel` appears nowhere in the ~31 MB console capture. Treat it as a transient
debugging edit, not a property of the shipped image.)*

### 11.3 ★ SILENT SERIAL MEANS THE BOX CRASHED, NOT THE ADAPTER

An entire session was burned reseating and swapping the USB-UART. Recovery is a **power-cycle
plus direct-write ESC spam**:

```bash
while ...; do printf '\033' > /dev/ttyUSB0; sleep 0.03; done
```

**NEVER use `screen -X stuff` for the catch.** It spawns one process per keystroke (~30 ms
each) — far too slow for the escape window — and `screen` takes exclusive control of the port.
Never let `screen` own the port during a timing-critical catch.

### 11.4 The console is a no-login root shell, so UART line noise executes commands

After three reseats a floating line / bad ground produced a `0xFF` flood, and a box that had
been up 118 minutes and was SSH-clean went offline mid-session — almost certainly garbage that
ran `ifconfig eth0 down` or `reboot`. **Do not keep a reader open, or write to the port, while
it is noisy.** For a long soak, consider requiring a console login.

### 11.5 The USB-serial adapter degrades with use and recovers with rest

After ~3–5 rapid reload cycles the loader-catch fails and register/counter reads come back
**empty**. It recovers after a pause, after a driver unbind/bind, or after a physical reseat.
Multiple "RX dead" boots in a determinism soak were **later shown to be serial-read failures**,
never real RX-dead. **It silently manufactures false negatives in any serial-driven soak** —
prefer serial-independent checks such as a ping from the control host.

### 11.6 There is no `devmem`, no `/dev/mem`, and no `CONFIG_KEXEC`

`# CONFIG_DEVMEM is not set` (`target/linux/generic/config-4.14:1088`) and
`# CONFIG_KEXEC is not set` (`:2253`). **Every register hypothesis therefore costs a build +
flash/reload cycle.** Budget experiments accordingly, or **add a poke module-param first**.

The one live register window that *does* exist is the **forced-on debugfs for the external
switch** — the `#ifdef CONFIG_RTL8366_SMI_DEBUG_FS` was changed to `#if 1`
(`rtl8366_smi.c:25,701`, `rtl8366_smi.h:61,122`), giving:

```
/sys/kernel/debug/rtl8367b/{reg,val,vlan_mc,pvid,mibs}
```

It cracked both the swapped-VID bug (§3) and the trunk-forwarding bug
(`docs/M7-TRUNK-FORWARDING-FIX.md`). A **switch-core register-window dump was later added on
the SoC side too** (`40f5dfb069`), because stock exposes `/proc/rtl865x/memory` and this port
had no equivalent, making a register diff impossible. It is off by default:

```bash
echo 0xBB804400 > /sys/module/rtl819x/parameters/regdump_base   # rtl865x_asichal.c:144-150
echo 64         > /sys/module/rtl819x/parameters/regdump_n
cat /proc/rtl865x_gw
```

### 11.7 ★ Reading a proc file destroys ASIC state

`cat /proc/rtl865x_gw` (`gw_prog`) **wipes the L2 tables**. The L2 warm-up must come **after**
it, never before (`bench-up.sh:30-36`, `dir842-asic:16-21,47-49`).

**Mirror hazard:** a level-3 `fabric_reset` (SIRR `FULL_RST`) clears the TLU tables, so it must
be **paired with a following `cat /proc/rtl865x_gw`** — otherwise even small frames stay 100%
dead with `MSCR=1`. Two-step recovery:

```sh
echo 3 > /sys/module/rtl819x/parameters/fabric_reset
cat /proc/rtl865x_gw
```

### 11.8 Never `swconfig ... set apply 1` interactively at runtime

Its soft reset re-wedges the 8367S and kills RX. **Bake all switch/VLAN config into the boot
path.** The apparent counter-example — one `apply` *after* netifd, which clears a warm-reload
trunk wedge — is exactly why this ended up as an ordered boot service
(`/etc/init.d/dir842-asic`, `START=97`) rather than an interactive step; see the ordering
rationale at `dir842-asic:10-21`.

### 11.9 A hard fabric wedge survives a warm RAM reload

Only a **physical power-cycle** clears it. A binary validated at 15,000 frames later read 100%
loss across four warm reboots. **Corollary: never trust large-frame results taken after a
session of wedge testing** — validate on a freshly power-cycled box.

### 11.10 A soft `reboot` strands the loader's RGMII trunk

TFTP then gives `curl rc=28`. The loader's own watchdog self-reboots ~49 s later with a clean
trunk init — **catch that window** instead of fighting the first one.

### 11.11 Bench guards drift silently, and every drift reads as 100% loss

Three independent drifts, all of which look identical from the box:

| drift | consequence |
|---|---|
| NetworkManager strips the USB NIC's IPv4 and re-adds a default via the house gateway | `172.16.0.0/24` silently routes out the house NIC |
| the Pi's bench address lives on `br0` and is lost on reboot | WAN peer unreachable |
| the loader's nvram IP resets to `192.168.1.6` | TFTP silently fails while the catcher keeps catching |

**This drift already caused one wrong published conclusion** (about `SWTCR0.WANRouteMode`) —
`bench-up.sh:41-46`. Verify the route and the peer address before believing **any** negative
bench result.

### 11.12 Background builds and serial catchers get killed after ~2–3 minutes

…in the agent harness used for this project. Run the catcher in the **foreground**
(`timeout 560 bash catch-robust.sh`) and relaunch builds.

### 11.13 ★ The vendor SDK tree contains non-UTF8 files and plain `grep -r` SILENTLY SKIPS THEM

**Always use `grep -ra` / `grep -a` under the SDK.** This caused missed findings **twice** —
most expensively, the CPU-tag reference implementation (`rtl865x_asicL2.c` and
`rtl8367r/rtk_api.c`) sat there unfound for the whole project. **This matters to any reader of
this repo**, because the 2.4 GHz build path points you at that SDK (`VENDOR_SDK=`, `build.sh:59-73`).

### 11.14 `uci-defaults` scripts are SOURCED, not exec'd

`( . "./$file" ) && rm -f "$file"` — so the exec bit is irrelevant, and `exit 1` is a
legitimate "keep me, retry next boot" idiom.

### 11.15 `uci delete wireless` with no section is a silent no-op

`uci_delete()` asserts `ptr->s` (`list.c:590`) so it fails, and `uci -q batch` swallows the
error and continues. `99-dir842-m5:158` intends to wipe the whole package; **`radio1` survives
BY ACCIDENT.** If uci ever changes that behaviour, `radio1` disappears.

### 11.16 `wifi up radio0` at runtime hung the box hard

No ping, no serial, silent — while the same config comes up fine at boot. **Prefer a
power-cycle** over a runtime `wifi up`.

### 11.17 `rtl819x: recovery level 3` fires ~2× per boot

335 occurrences across the capture. Pre-existing, benign, unexplained — **do not chase it as a
regression** (also flagged in `README.md:72`).

### 11.18 `print_hex_dump(..., rowsize, groupsize=1, ...)` prints raw memory order

Confirming `groupsize` is what proved the RX buffer really *was* byte-swapped, rather than the
*printer* swapping it. **Check `groupsize` before trusting any hex dump used as endianness
evidence.**

### 11.19 Aliasing trap: a `ping -i` harmonic of the driver's watchdog poll fakes a flat RTT

The driver polls on a 100 ms watchdog; a `ping -i 0.2` produced a **phantom flat 40 ms** RTT.
Always cross-check with a non-harmonic interval.

### 11.20 Kill stray background pings before measuring

A leftover host→box ping faked a box→host "5/5" and "20/20 warm". Get **wire ground truth**
with `tcpdump` on the host.

### 11.21 RAM-boot and NOR-boot results are NOT interchangeable for anything memory-sensitive

A RAM boot's initramfs rootfs is **unreclaimable**:

| boot medium | MemFree | MemAvailable |
|---|---|---|
| fresh RAM boot | 12,868 kB | **984 kB** |
| after flashing to NOR | — | **7,576 kB** |

After ~20 minutes of hwnat testing on a RAM boot, **`MemAvailable` reaches 0 and the box cannot
fork at all** — which is why diagnostics kept dying mid-command. It also produced a fake WiFi
regression: `mac80211.sh: eval: can't fork: Out of memory` → `Could not find PHY for device
'radio0'`, root-caused in `261c833bd4` as **memory pressure, not a PHY/naming conflict**. The
vendor `rtl8192cd` 2.4 GHz path survived because its WPA2-PSK is in-kernel and it barely forks;
`mac80211` + hostapd is the first casualty.

### 11.22 `/proc/buddyinfo`, `/proc/pagetypeinfo` and `/proc/zoneinfo` are ABSENT by config

Because of `CONFIG_PROC_STRIPPED=y` (`target/linux/generic/config-4.14:3645`, applied by
`target/linux/generic/hack-4.14/902-debloat_proc.patch` in the ggbruno base tree — neither file
is part of this repo's `files/` overlay), **not** because of a kernel limitation.
`/proc/vmstat` survives the strip. Override in `target/linux/realtek/rtl8197f/config-4.14` if
needed. **Three separate investigations stalled on "buddyinfo isn't compiled in" as if it were
immovable.**

---

## 12. Bench artefacts baked into the shipped firmware

**Call this out before anyone flashes.** The bench peer addresses are **hardcoded in the
image**:

| file | what it hardcodes |
|---|---|
| `files/target/linux/realtek/base-files/etc/rc.local:64-68` | boot-time ASIC L2 warm-up pings `192.168.0.2` (`:65`) and `172.16.0.2` (`:66`) |
| `files/target/linux/realtek/base-files/etc/init.d/dir842-asic:33` | `WARM_HOSTS="${WARM_HOSTS:-192.168.0.2 172.16.0.2}"` — *"the defaults are the bench rig's peers and simply time out harmlessly elsewhere"* |
| `files/.../uci-defaults/99-dir842-m5:61,67` | LAN `192.168.0.1`, WAN **static** `172.16.0.1` |

The WAN static is not cosmetic: `172.16.0.1` **is** the ASIC's `extIP[0]`, the address the
hardware NAPT rows rewrite to.

### The boot ritual encoded in `dir842-asic` (`START=97`, `USE_PROCD=0`)

```
swconfig apply  ->  echo 3 > fabric_reset  ->  cat /proc/rtl865x_gw (gw_prog)  ->  L2 warm
```

with a `sleep 5` first so netifd has finished creating `eth0.1` / `eth0.2` / `br-lan` and
applied the per-unit MACs (`dir842-asic:62-73`).

**`hwnat` is DELIBERATELY LEFT OFF at boot** and armed by hand. The script's own comment
(`dir842-asic:23-27`) records why: enabling it before the tables are warm **killed the datapath
outright** — 100% loss, recovering on `echo 0` plus a `fabric_reset`.

```sh
echo 1 > /sys/module/rtl819x/parameters/hwnat   # arm offload, once the box is warm
```

---

## 13. Known-stale comments in the committed scripts

Flagged rather than edited, so the scripts stay byte-identical to the originals that produced
the published numbers.

| location | says | actually |
|---|---|---|
| `bench-up.sh:10` | `8367S port 4, 100M` | **1000baseT full-duplex**, measured; the cable was swapped mid-project (§3) |
| `bench-up.sh:18-20` | loader answers on `e0:1c:fc:51:c9:ef`, Linux on `00:e0:4c:81:96:c2` — never pin a static ARP | **No longer true since `484e4db4bd`.** Both are `e0:1c:fc:51:c9:ef` (`02_network:59-64`). The conflict cannot occur. |
| `flash-nor.sh:17-18` | same MAC-conflict warning | same correction |
| `bench-wan-emu.sh:16` | cites `rtl819x-eth.c:1134` for the `ph_vlanId` constraint | current tree: **`rtl819x-eth.c:1269-1276`** (comment) / `:1277-1280` (code) |
| `tools/sign-dlink.py:4`, `README.md:135` | key *"embedded in the bootcode (`mtd0`) at `0x291bc`"* | `0x291bc` is an offset into the **decompressed** bootcode (gzip stream at `mtd0+0x7d70`, 191,632 B inflated), **not** into the raw NOR image, where that offset reads `0xFF` (§8) |
| `MANIFEST.txt` | — | **has no mention of the bench at all.** `grep -niE 'bench\|ramboot\|flash-nor\|serial\|38400'` returns nothing, even though `bench-up.sh`, `bench-wan-emu.sh`, `bootgate.sh`, `flash-nor.sh` and `ramboot.sh` are all committed in the repo root and absent from its "REPO ROOT" listing. |

---

## 14. What was never written down

Stated as gaps, deliberately not guessed:

1. **Which silkscreen jack label (LAN1..LAN4) corresponds to 8367S port 2.** Every source in
   this project uses the switch port number only. **Not determined — do not guess.** If you
   rebuild this bench, find your host jack empirically:
   `swconfig dev switch0 show | grep -A1 'Port [0-4]'` with one cable plugged in at a time.
2. **The DIR-842 UART header's physical location, pin order and voltage level.** Not
   determined. (Baud, framing and the fact that it is a no-login root shell *are* — §5.)
3. **The chip/driver of the USB WiFi dongle used for the M5 AP proof.** Not determined.
4. **The source of the download-path TCP retransmits** (1200–2500 per run at ~900 Mbit). The
   router is measurably not the bottleneck — CPU 0.3 %, zero interface errors — but the loss
   source was never identified (`README.md:66-68`). Take a range, not a single run.

---

## Appendix — minimal resume-work sequence

```bash
# 0. one-time: bench NIC out of NetworkManager's hands, image built
docker run --rm -v "$PWD":/build -w /build/openwrt <bullseye-image> make -j12

# 1. cold box -> root shell in RAM, no flash writes, no human
bash ramboot.sh

# 2. bring the bench to a known-good state (order is load-bearing)
bash bench-up.sh

# 3. measure ONLY through the harness
bash hwnat-ab.sh          # not in this repo; see §10

# --- flashing instead of RAM-booting ---
tools/sign-dlink.py openwrt-...-squashfs-factory.bin signed.bin
BOOT=1 bash flash-nor.sh signed.bin
N=10 bash bootgate.sh     # prove it boots, not that it booted once
```
