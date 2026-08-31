# WiFi: cracking RTL8197F + PCIe, and running two incompatible radios at once

How the 5 GHz RTL8822BE was made to enumerate on a SoC where PCIe had never worked in
OpenWrt, how the on-SoC 2.4 GHz radio was brought up with the vendor `rtl8192cd` driver,
and how two radios with no shared abstraction — one mac80211, one WEXT — are kept from
colliding over an interface name. Every failure in here was **silent**: the logs said
success and nothing was on air.

The 2.4 GHz porting *journal* (how the vendor driver was audited, measured and ported)
lives in [`VENDOR-PARITY-INVENTORY.md`](VENDOR-PARITY-INVENTORY.md) §R4. This file is the
finished-state account and does not repeat it.

---

## 1. What ships

| | 5 GHz | 2.4 GHz |
|---|---|---|
| chip | RTL8822BE, on-board, PCIe `01:00.0` (`10ec:b822`) | RTL8197F on-SoC WMAC, `0x18640000`, IRQ 6 |
| driver | mainline **rtw88** (`rtw_8822be`) | **vendor `rtl8192cd`** (Realtek SDK source) |
| stack | mac80211 / cfg80211 + hostapd | WEXT + 114 private ioctls, **no cfg80211** |
| WPA2 | hostapd | **in-kernel** 4-way handshake (`8192cd_psk.c`) — no hostapd |
| config path | `/lib/netifd/wireless/mac80211.sh` | `/lib/netifd/wireless/rtl8192cd.sh` → `/etc/Wireless/RTL8192CD.dat` |
| netdev | **`wlan1`** (pinned) | **`wlan0`** (driver-created at module load) |
| uci | `radio0`, `type='mac80211'` | `radio1`, `type='rtl8192cd'` |

Both run concurrently and both are bridged into `br-lan`.

**★ The naming is the mirror image of stock, and this is a live source of confusion.**
Stock drives *both* radios with `rtl8192cd` and never loads rtw88/mac80211 at all
(`modules.builtin` has exactly one wireless entry); its Kconfig default is
`BAND_5G_ON_WLAN0`, so stock is **wlan0 = 5 GHz, wlan1 = 2.4 GHz**. This port is
**wlan0 = 2.4 GHz (vendor), wlan1 = 5 GHz (rtw88)** — the opposite. Any stock log,
`iwpriv` recipe or MIB name you copy across bands will land on the wrong radio.

**This repo ships the 2.4 GHz build path.** The vendor driver + `include/net/rtl`
headers are in `files/`, `CONFIG_RTL8192CD=m` is in the subtarget kernel config, and
the seed selects `kmod-rtl8192cd`, so `build.sh` produces dual-band images.
(Historical note: until 2026-08-16 the driver was withheld and the port shipped only as
`g3-rtl8192cd-4.14-port.patch` + `g4-rtl-headers-4.14-port.patch`; an earlier
`VENDOR_SDK=` raw-SDK import was verified broken by dry-run 2026-08-02 and withdrawn —
§9 item 5 records that evidence. The patches remain in the repo root as the record of
the port; the shipped tree already contains everything they describe.)

`rtl8192cd` is **real source, no driver blobs** — 279 `.c` + 363 `.h`, 539 038 LoC of
`.c`, `MODULE_LICENSE("GPL")`; the only binary is the WMAC chip firmware
(`WlanHAL/Data/8197F/rtl8197Ffw.bin`, embedded by the Makefile's bin2c step). The
earlier claim here that *every* file lacked a licence grant was **wrong** — a
file-by-file audit (2026-08-16) found explicit GPLv2 grant headers on the core driver
and `phydm` (336 of ~650 C/H files, including `8192cd.h`); the redistribution
rationale is in the root README's *Building* section. See
[`VENDOR-PARITY-INVENTORY.md`](VENDOR-PARITY-INVENTORY.md) §1.

**Shipped defaults, as of the code in this tree:**

| | SSID | encryption |
|---|---|---|
| 5 GHz `radio0` | `DIR842-OpenWrt` | **`none`**, key deleted — deliberately no baked credential (`files/target/linux/realtek/base-files/etc/uci-defaults/99-dir842-m5 (5 GHz wifi-iface block)`) |
| 2.4 GHz `radio1` | `DIR842-2G` | **`none`** — same stance, since 2026-08-16 (`files/…/uci-defaults/09_wireless-dualband-dir842 (radio1 block)`; it previously seeded `psk2` with a placeholder key, which was inert while no driver shipped but would have been a published credential in a dual-band image) |

Both seeds ship open on purpose: a PSK baked into a published image is a published
credential. Set keys on both before this touches a real network.

---

## 2. ★ M5: the PCIe bring-up the community had abandoned

**Result:** cold-boot `link_up=1`, `10ec:b822` enumerates, rtw88 binds, an AP comes up and
a client associates with WPA2 and pings 8/8, 0 % loss. RTL8197F + PCIe WiFi had never
worked in OpenWrt before this — it is the wall the RTL8197F community had given up on
(e.g. the Netis N2 port).

Real boot lines, taken from the driver's own diagnostic (`pci-realtek.c:403-405`):

```
PCIe RC0 link check: LTSSM(0x728)=0x0322c611 clk=25000000Hz link_up=1
PCIe RC0 link check: LTSSM(0x728)=0x0360f711 clk=25000000Hz link_up=1
```

`link_up` is `(rc_cfg + 0x728) & 0x1f == 0x11` (L0), polled up to 21× at 100 ms
(`pci-realtek.c:209-224`). Note the AP verified at M5 was on **2.4 GHz** — the 8822BE is
one radio doing 2.4 **or** 5, and 5 GHz did not become usable until the crystal-cap fix
in §3.

### ★ Mainline pointed two writes at the wrong registers

The sequence was reverse-engineered from the stock kernel's `pcie_init`; the registers
below are the only ones the whole vendor image touches for this block.

| step | mainline (wrong) | stock / the fix |
|---|---|---|
| PHY digital-reset release | `sr_w32(BIT(3) / BIT(3)\|BIT(0) / …, REALTEK_SR_PCIE_PHY0)` = **SR+0x50** | write **`0x8, 0x9, 0xb` to `REALTEK_SR_MDIORST` = SR+0x100 (`0xb8000100`)** |
| device reset | `CLKMANAGE` bit 26 toggle — a **no-op for the PHY**, LTSSM stuck at `0x02` | **SR+0x50 (`PCIE_PHY0`) bit 1: clear → hold 300 ms → set** |
| clock enable | `CLKMANAGE \|= 12,13,18,19,20,26`, then 14, `mdelay(100)` | vendor sets **only 12,13,18**, then 14, `mdelay(10)` |
| MDIO tuning | 20-write table; refclk from the DTS `clk_get_rate()` | short table — **2 writes at 25 MHz**, 3 at 40 MHz; refclk from the SoC **bootstrap bit 24** |
| PHY reset pulses | one | **two** `PWRCR` pulses, around the MDIO tuning |

Code: `files/target/linux/realtek/files-4.14/arch/mips/pci/pci-realtek.c` —
`realtek_pcie_mdio_reset()` :242-255, `realtek_pcie_device_reset()` :226-241,
`realtek_pcie_reset()` :270-307. Register names in
`files/target/linux/realtek/files-4.14/arch/mips/include/asm/mach-realtek/realtek_mem.h:10-13`.

**Crystal is 25 MHz, not 40.** `REALTEK_SR_BS_40MHZ = BIT(24)` of the bootstrap register
is **clear** on this board — boot log `BOOTSTRAP = 8197f001 0 40258ec0 80000800`, so
`sr_r32(0x08) = 0x40258ec0`, `& 0x01000000 == 0`
(`files/target/linux/realtek/dts/RTL8197F.dtsi:96-108`). The first M5 boot had this at
40 MHz, programmed the PHY with 40 MHz coefficients for a 25 MHz reference, and the link
never trained. ⚠ That DTS comment still *says* the driver picks the sequence off
`clk_get_rate()`; **it does not** — `realtek_pcie_reset()` reads the bootstrap bit
directly (`pci-realtek.c:273`) and the DTS clock survives only as the value printed in the
diagnostic line. Prefer the code.

### ★ Second, independent fix in the same file: a hard CPU-bus hang

On `!link_up` the old code fell through to a config-space read at `dev_cfg0_base + 0x78`.
With no link there is **no PCIe completion and the CPU bus hard-hangs** — no exception, no
watchdog, physical power-cycle only. It now warns and `return`s 0 cleanly, so the kernel
finishes booting *and* the second root complex (`pcie1@18b20000`) still gets a chance to
probe (`pci-realtek.c:406-419`). This is worth keeping even now that the link trains: it
converts "board is dead, reflash it" into a log line.

### ★ Cross-reference worth making

The 2.4 GHz WMAC turned out to have the **direct analogue** of the `SR+0x100` mistake:
`REALTEK_SR_WLAN_EN = 0xB8000064`, whose **bit 0 gates all WMAC register access** and is
re-checked at ~1690 sites in the vendor driver. Same class of bug — a system register
mainline never wrote, with the block simply not responding until it is set. See §4.

**Commit hashes.** The M5 milestone commit `98eedd4` **no longer resolves**: that history
was rebased (see [`README.md`](README.md) → *Hash aliasing*). The `pci-realtek.c` work is
in the squashed `d150b24606` → now **`f096f5d463`**, which changes that file by 132 lines
(72 insertions / 60 deletions per `git show --stat`; a "+132/−88" figure circulates in
older notes and does not match the tree).

---

## 3. Blank efuse: RFE type, MAC, and the crystal cap

This board's RTL8822BE efuse is **all `0xFF`**. Three separate consequences, fixed in
three places.

**RFE type.** rtw88 only supports RFE `{2,3,5}`; a blank efuse yields `0xff` and chip init
refuses. Forced to **RFE = 2** (standard 2T2R front end) in
`files/package/kernel/mac80211/patches/realtek/03-rtw8822b-blank-efuse-rfe.patch`.

**MAC.** `04-rtw88-random-mac-blank-efuse.patch` gives an invalid efuse MAC a
`eth_random_addr()` so the interface can come up at all — but that is a **different BSSID
every boot**. `99-dir842-m5:100-113` then pins a stable, per-unit one: the D-Link per-unit
base MAC is an ASCII string at **mtd1 + 0x00**, and the WiFi BSSID is **base + 2** (WAN =
base + 0, LAN = base + 1), falling back to `02:11:22:33:44:56` if the field is
unprogrammed (commit `b68c51ced1`). Reading mtd1 here is safe because it sets *only* the
BSSID — `board.d/02_network` remains the single writer of the ethernet MACs, and two
writers is exactly what previously made those non-deterministic.

### ★ The blank efuse is a **5 GHz-only** problem

Per-unit **2.4 GHz** RF calibration is not in an efuse at all. The stock kernel never
reads efuse or flash for that radio: the values live in the read-only **mtd1 "MAC"**
partition (`0x20000 + 0xd8`: `pwrlevelCCK_A/B`, `pwrlevelHT40_1S_A/B`, xcap, thermal) and
stock pushes them in **from userspace** via `iwpriv set_mib`
(`lib/librlx_wifi_mibs.so`). See `files/target/linux/realtek/dts/RTL8197F.dtsi:216-224`.

### ★ The crystal cap — and the theory it replaced

**✗ RETRACTED:** that 5 GHz was blocked by *missing RF power tables*. The `write RF mode
table fail` WARN comes from `rtw8822b_set_channel_bb()`, which writes RF path A and then
polls **RF register 0x33** for `0x00001` a hundred times and never gets it. That is **RF
register access failing**, not a missing power table.

**Real cause:** rtw88's blank-efuse fallback sets `crystal_cap = 0` — the *minimum* load
capacitance, not this unit's calibrated value — which puts the RF synthesiser
off-frequency, so the RF registers stop answering.

**Fix (`f5a3ac7993`):** feed the per-unit crystal cap in from flash. The patch adds a
sweepable module parameter (`03-…-rfe.patch`, `xtal_cap_override`, `0..63`, `-1` = rtw88
default) and `99-dir842-m5:130-155` reads **mtd1 + 0x13e** (= 39 on this unit), writes
`/etc/modprobe.d/10-rtw88-xtal.conf`, and re-probes `rtw88_8822be` so it applies on the
same boot. Cold-boot verified: no WARN, `wlan1` reaches bridge forwarding, `iw dev wlan1
info` = AP on **channel 36, 80 MHz, 17 dBm**.

⚠ **39 is empirical.** `mtd1+0x13e..0x141` reads `39, 49, 35, 24` — two plausible 6-bit
xcap values and two inside the vendor's 7..50 thermal range — and the **field order could
not be resolved from the binary**. 0x13e is the byte that made the radio initialise
cleanly, nothing more. A different unit that fails to bring the radio up should sweep the
other candidates via `/sys/module/rtw88_core/parameters/xtal_cap_override` before assuming
a hardware fault.

⚠ `hexdump` is in this image; **`od` is not.** The flash read uses `hexdump -e '"%d"'`.
Getting that backwards yields an empty string, which falls silently into the fallback
branch and looks exactly like a flash-read failure (`99-dir842-m5:136-139`).

### TX power

Two *different* uncalibrated-TX situations, routinely conflated:

| radio | why | status |
|---|---|---|
| 5 GHz RTL8822BE | blank efuse holds no per-unit TX calibration | works, not "loud" — **believed still true, not freshly re-verified** |
| 2.4 GHz WMAC | the mtd1 per-unit tables are never pushed; `pwrlevelCCK_A/B` and `pwrlevelHT40_1S_A/B` read zeros | running on driver defaults; stock's userspace `iwpriv set_mib` equivalent is **not implemented** |

---

## 4. 2.4 GHz: why the vendor driver, and the three silent failures

### Hand-writing an init was attempted, and abandoned

**G1 (`1bb3d673d1`)** — the on-SoC WMAC had **no DTS node at all**. Decoded from the stock
`vmlinux`: device table at `0x8064ae6c`, stride `0x14`, entry [1] = phys `0x18640000`,
IRQ 6. Self-validating, because entry [0]'s two addresses are exactly `pcie0`'s
`dev_cfg0_base` and PCI window as already declared in the DTS.

**★ The find:** `REALTEK_SR_WLAN_EN = 0xB8000064` bit 0 gates *all* WMAC register access.
`rtl8192cd_init_one` @`0x801fbd6c` does `*(u32*)0xB8000064 |= 0x1f`; `rtl8192cd_close`
writes 0 — the **only two writers in the whole stock image**. It **read `0x00000000`
before our write**, confirming the bootloader does not set it. Decode in
`realtek_mem.h:13-34`.

**G2 (`9931511050`)** — with the gate lifted the block probes and answers: chip id
`8197f001`, hw id `100a`, IRQ 6, xtal 25 MHz. The stub deliberately stops there and
includes an all-`0x00`/all-`0xFF` dead-bus detector rather than writing PHY/RF init into
the dark (`files/target/linux/realtek/files-4.14/drivers/net/wireless/rtl8197f/rtl8197f-wmac.c:186-213`).

**The table-replay route was ruled out on silicon** (see
[`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) #8): this silicon reports **cut =
1**, and the vendor gates the header-table path on **cut ≥ 2** — so stock never applies
those tables here. Replaying them would have been replaying something the hardware never
sees. `array_mp_8197f_phy_reg` is additionally *not* a flat table: entries like
`0x80001003` / `0x40000000` are Realtek conditional-branch markers, and a linear replay
writes garbage to whatever they alias.

### G3: porting the real driver was cheaper than it looked

The kernel-API surface is **5 files out of 664** — the driver carries its own OS
abstraction layer. `asm/rtl865x/*` turned out to be a **non-problem**: all five references
are dead for this config. An earlier estimate of "8 files need vendor headers" overstated
it. The 8devices `v3.4.11e/openwrt-18.06-rtkmipsel-3.18` build config is *literally* the
dual-band target — `CONFIG_SOC_WIFI=y` + `CONFIG_SOC_RFE_TYPE_0=y` alongside
`CONFIG_SLOT_0_8822BE=y`.

### The three silent failures

All three share a signature: **everything reports success and nothing happens.**

**(1) ★ `CONFIG_BAND_2G_ON_WLAN0` — the radio does not exist without it.** It is what puts
the on-SoC WMAC into `wlan_device[]` at all: the only entry describing the embedded radio
is behind `#if defined(CONFIG_WLAN_HAL_8197F) && defined(CONFIG_BAND_2G_ON_WLAN0)`.
Without either band macro, `wlan_device[]` degenerates to the three **all-zero
`TYPE_PCI_BIOS`** entries from the `#elif defined(NOT_RTK_BSP)` leg, and
`rtl8192cd_init_module()` — single-shot at `wlan_index == 0` for a module build — sees
`TYPE_PCI_BIOS`, calls `pci_register_driver()`, and never touches the embedded radio.
Second effect, also needed: `8192cd_cfg.h` sets `WLANIDX=1`, which flips the
`RX_BUF_LEN`/`RX_DESC_NUM`/`RX_MAX_SKB_NUM` selectors so our radio gets 2.4 GHz ring sizes
instead of 5 GHz ones (mirror `4d0d7e5`).

**(2) ★ The wrong crystal — the bug that kept the radio silent.** `8192cd_hw.c:14672-14679`
picks the 8197F WLAN crystal **solely** from `CONFIG_PHY_EAT_40MHZ`; the vendor's
strap-reading alternative immediately above it is dead code here (gated on
`CONFIG_AUTO_PCIE_PHY_SCAN`, defined only for `CONFIG_RTL_8196E`/`__OSK__`). With the flag
on, `XTAL_CLK_SEL_40M` goes into `InitPONHandler()` and the WLAN PLL comes up for a 40 MHz
part on a **25 MHz** board. **LO off by 40/25 = 1.6×.** MAC, BB and firmware all report
success, IQK completes, `beacon_ok` climbs at exactly the TBTT rate — and nothing is on
air, RX equally deaf (`rx_packets: 0`).

Easy to misread as "the AP was never started". It was started. Three independent sources
say 25 MHz: stock's own boot log on this unit (`clock 25MHz`); our mainline
`rtl8197f-wmac` stub reading bootstrap `SR+0x08 BIT(24)` and printing `xtal 25MHz`
(`rtl8197f-wmac.c:254`) — written long before this driver existed; and the radio going
from silent to loud when the flag was cleared. (Stock's `clock 40MHz` line belongs to the
8822B — a different chip.)

**(3) ★ The config path was compiled out.** `CONFIG_RTL_COMAPI_CFGFILE` had **never** been
set, so `CfgFileProc()` *and* its call site at `8192cd_osdep.c:7691` were both
preprocessed away — the netifd handler already in the tree would have been a silent no-op.
`CfgFileRead()` additionally called `fp->f_op->read` directly, which is **NULL on tmpfs**
(and `/etc/Wireless` → `/tmp`); replaced with 4.14's `kernel_read()`.

### ★★★ R4 complete — verified on air, externally

Confirmed by a scan from a **separate host adapter**, not the box's self-report
(mirror `e208a00`):

```
BSS e0:1c:fc:51:c9:f0 (on wlp4s0)   freq 5180.0   SSID: DIR842-OpenWrt
BSS 00:e0:4c:81:86:86 (on wlp4s0)   freq 2412.0   SSID: DIR842-2G
                                    RSN: CCMP / PSK, beacon interval 100 TUs
```

Client association on 2.4 GHz: WPA2-PSK, HT MCS4 43.3 Mbit/s, **DHCP lease obtained from
the box over the air**, ping 8/8 0 % loss, `total_psk_fail: 0`. The 4-way handshake runs
**inside the driver** — no hostapd, exactly as stock does it. PCI `01:00.0` stays bound to
`rtw_8822be` throughout.

---

## 5. ★ The netifd handler that never registered its driver

**A silent-by-design failure mode that no log will ever tell you about.** Commit
`42ecc2308d`, verified in `e3c70a9ad5`.

`/lib/netifd/wireless/rtl8192cd.sh` never called `init_wireless_driver "$@"` — and
`git log --all -p` shows the call **never existed in any revision**. The chain:

- `netifd-wireless.sh:12-14` ships `add_driver()` as a **no-op stub** (`add_driver() {
  return; }`). Verified in the built source tree.
- The real implementation is installed **only inside** `init_wireless_driver()`
  (`netifd-wireless.sh:367`), which `mac80211.sh:6` calls and this handler did not.
- So `add_driver rtl8192cd` at the bottom of the handler emitted **0 bytes**.
- netifd probes handlers with `popen("<handler> '' dump")` (`handler.c:96`) and needs JSON
  back, so `wireless_add_handler()` (`wireless.c:685`) was never reached.
- `config.c:332-334` then drops the radio **with no log line at all**:

```c
drv = avl_find_element(&wireless_drivers, driver_name, drv, node);
if (!drv)
    return;
```

`radio1` was therefore **absent** from `ubus call network.wireless status`, not "failed".
`wifi up`, `wifi down` and the status call all simply ignored it.

**★ The clinching evidence was a file that isn't there.** `/proc/wlan0/mib_all` existed
(so the driver was loaded and healthy) but **`/etc/Wireless/RTL8192CD.dat` was missing** —
and `drv_rtl8192cd_setup` writes that file unconditionally once the `mib_all` gate passes.
Proof the setup handler never ran, as opposed to running and failing. A manual `ip link
set wlan0 up` meanwhile produced the full vendor init (`Set MIB from
/etc/Wireless/RTL8192CD.dat`, `[97F] Bonding Type 97FS`, `load efuse ok`) — nothing had
ever called `dev_open()`.

**Second fix in the same commit: uci `band` 7 → 11.** `band` is a **bitmask**
(`8192cd.h:875`: 11B=1, 11G=2, 11A=4, 11_24N=8). `7 = b|g|A` asks a 2.4 GHz radio for the
**5 GHz-only** mode and drops 11n entirely. `WIRELESS_MODE_24G = 1|2|8 = 11`
(`09_wireless-dualband-dir842:58-64`).

**Verification (`e3c70a9ad5`) — all four predicted observables flipped:**

| observable | before | after |
|---|---|---|
| handler `'' dump` | **0 bytes** | **541 bytes** |
| `/etc/Wireless/RTL8192CD.dat` | absent | present, `wlan0_ssid="DIR842-2G"` |
| `wlan0` operstate | `down` | `unknown` |
| `ubus call network.wireless status` | `radio1` absent | `radio1` present |

**★ Correction recorded in the commit:** OOM had been blamed for this failure. The OOM was
real (§7) but was **not** the cause here — setup provably never ran, memory or not. Two
distinct bugs with overlapping symptoms.

The live handler is
`files/target/linux/realtek/base-files/lib/netifd/wireless/rtl8192cd.sh`; the mandatory
call is at **:17**, `add_driver rtl8192cd` at **:275**, and lines 5-16 carry the whole
explanation inline so it cannot be deleted by someone tidying up.

⚠ **Published-artifact drift is a recurring failure mode here.** An audit found the repo's
`files/` overlay stale against the live build tree — including a handler that lacked
`init_wireless_driver`, lacked `rtl_find_phy`, and a seed that still pinned
`phy='wlan1'`/`ifname='wlan1'`, i.e. **the exact naming hazard a later commit had fixed**.
Resynced at `ddb04e2` (46 files byte-compared). Re-verified while writing this doc: the
five WiFi-relevant overlay files are byte-identical to
`/home/agiu/dir842-build/openwrt`. **Run a mechanical `files/` ↔ build-tree diff before
any publish** — see §9 for one that is still outstanding.

---

## 6. ★ The interface-naming collision, and how the recommended fix caused it

`mac80211.sh` derives the interface name from the **phy index** (`phy0` → `wlan0`). The
vendor `rtl8192cd` driver **creates a real netdev named `wlan0` at module load**. netifd
then hands hostapd the *vendor* netdev.

Symptom set:

```
netifd:  radio0: Device setup failed: INTERFACE_CREATION_FAILED
hostapd: Configuration file: /var/run/hostapd-phy0.conf (phy wlan0) --> new PHY
hostapd: nl80211: Could not configure driver mode
hostapd: nl80211 driver initialization failed.
```

…while `/sys/class/ieee80211` shows a perfectly healthy `phy0` with no interface on it,
and **`iw phy phy0 interface add wlan1 type __ap` succeeds immediately by hand**. The
radio was never the problem. Only the name was.

### ★ The trigger was the previously-recommended fix

1. **`35ec543891`** measured — on two boots, *before any insmod* — that rtw88's 5 GHz took
   `wlan1` and the vendor root device took `wlan0`, and resolved the hazard **by identity,
   not by name**: `rtl_find_phy()` looks for `/proc/<ifname>/mib_all`, which only the
   vendor driver creates (rtw88 creates no `/proc/wlanN` at all), skipping VAP directories
   (`…/rtl8192cd.sh:182-197`). It was unit-tested against five synthetic sysfs/procfs
   layouts. Its accompanying note recommended **pinning load order, e.g. `AUTOLOAD` the
   module**.
2. **`9edc6bc5a8`** did exactly that — `AUTOLOAD:=$(call AutoLoad,60,rtl8192cd)`
   (`files/target/linux/realtek/modules.mk:23`). rtl8192cd now probed **first** and claimed
   `wlan0` before rtw88, rtw88's wiphy came up as `phy0`, and **the 5 GHz AP died
   silently.**

The identity-based resolution was correct and still is; the *ordering* advice attached to
it is what fired.

### The fix

**`b830957df9`** pins the mac80211 side's `ifname` to `wlan1`, **guarded on
`type='mac80211'`** — which of `radio0`/`radio1` is mac80211 depends on detection order,
and pinning `wlan1` onto the WEXT radio would be strictly worse (it would push mac80211
naming into a driver that cannot use it). Live file:
`files/target/linux/realtek/base-files/etc/uci-defaults/10_wireless-5g-ifname-dir842`.

Two details in that script that are easy to lose:

- It runs `wifi config` first if no wifi-iface exists yet, because `09_` seeds `radio1`
  *before* mac80211 detection has created `radio0`.
- If it still finds no mac80211 radio it **exits non-zero on purpose**: `/etc/init.d/boot`
  only deletes a uci-defaults script that **succeeds** (`( . "./$file" ) && rm -f "$file"`),
  so failing keeps the file and retries next boot instead of silently deleting itself
  having done nothing.

Verified on a factory flash + cold NOR boot, then again on a settled jffs2 boot:
`br-lan = eth0.2 + wlan0 + wlan1`; `wlan1` = ESSID `DIR842-OpenWrt`, Master, channel 36
(5.180 GHz); `wlan0` vendor radio alive (`/proc/wlan0/mib_all`, 268 MIBs); wired 0 % loss
at 64 B and 1400 B, `CPUICR1=0x82`.

**The commit's own conclusion: "never rely on load order to keep the two radios apart."**
The now-wrong MEASURED comment in `09_wireless-dualband-dir842` was **flagged stale in
place, not deleted** (`:19-36`), because its lesson is precisely what the bug re-proved.

---

## 7. Memory: why 5 GHz failed on a RAM boot and works on NOR

On a RAM boot the 5 GHz AP regressed with, verbatim (`261c833bd4`):

```
radio0 (1956): ./mac80211.sh: eval: line 30:  can't fork: Out of memory
radio0 (1956): Could not find PHY for device 'radio0'
radio0 (1956): ./mac80211.sh: eval: line 125: can't fork: Out of memory
radio0 (1991): ./mac80211.sh: eval: line 850: can't fork: Out of memory
```

**★ "Could not find PHY" is a consequence, not a cause.** mac80211's PHY detection *forks
helpers*; the forks fail; so it cannot find a PHY it has already enumerated. Note the same
message at lines 30 / 125 / 850 — three different fork sites, one condition. A manual
`ip link set wlan1 up` got `rtw_8822be: start vif … on port 0` from the driver; the card
was healthy the whole time.

**Why only 5 GHz loses:**

| radio | bring-up cost | outcome under pressure |
|---|---|---|
| `radio1` / rtl8192cd | WPA2-PSK **in-kernel**, no hostapd, minimal forking | survives |
| `radio0` / mac80211 | forks repeatedly for PHY detection, then forks hostapd | **first casualty** |

Fixing the 2.4 GHz netifd registration (§5) did not cause this; it simply added a second
radio's worth of demand to a box with no headroom.

**Measured, fresh RAM boot, before any test load:** `MemFree 12868 kB`,
**`MemAvailable 984 kB`**. After ~20 minutes of hwnat testing `MemAvailable` reaches 0 and
the box cannot fork at all — which is why diagnostics kept dying mid-command.

### ★ The prediction, and its confirmation

`261c833bd4` predicted: *"the NOR flash we already owe is the fix"* — a RAM boot's rootfs
is a RAM-resident initramfs and is **unreclaimable**; squashfs on flash gives several MB
back. Confirmed by `24d998f9a3` on a real flash + cold power-cycle:

| | RAM boot | NOR boot |
|---|---|---|
| `MemAvailable` | 984 kB | **7576 kB** |
| `br-lan` members | `eth0.2 wlan0` | **`eth0.2 wlan0 wlan1`** |
| `ubus network.wireless` | one radio | **`radio0` and `radio1`** |

### ★ The `fork()`-ENOMEM question is closed — and the long-carried note was a misreading

The note *"fork() returns ENOMEM with ~10 MB free (fragmentation?)"* is wrong on both
counts. Measured by spawning processes to failure:

| n | MemFree | MemAvailable | |
|---|---|---|---|
| 40 | 16412 k | 6172 k | |
| 80 | 13632 k | 3404 k | |
| 120 | 10904 k | **676 k** | **← fork refused here** |

**`MemFree` was the wrong gauge.** `MemAvailable` was genuinely exhausted at ~0.7 MB. The
~10 MB gap is the zone watermark reserve plus non-reclaimable page cache, which the
allocator will never hand to a userspace fork. ~46–69 kB per process on this box.

**★ Fragmentation is RULED OUT, not merely unproven.** The order-1 (8 kB,
`THREAD_SIZE_ORDER(1)` on 4 kB-page MIPS32) thread-stack theory was plausible — but that
path calls `warn_alloc()` and dumps per-order free lists, and **nothing appeared in dmesg
at all.** No page-allocation failure was ever logged.

**★ Reusable technique:** use `warn_alloc()`'s dump as **free buddyinfo** whenever a real
order-N failure is suspected. `/proc/buddyinfo`, `pagetypeinfo` and `zoneinfo` are absent
on this image because of **`CONFIG_PROC_STRIPPED=y`**
(`target/linux/generic/config-4.14:3645`), not a kernel limitation; `/proc/vmstat`
survives the strip.

---

## 8. Diagnosing each radio (they share no tooling)

**★ `iwinfo` cannot report the 2.4 GHz radio.** It is WEXT with no cfg80211 — its
`SIOCGIWNAME` handler is NULL. That is expected for this driver, **not** a fault, and it
is the single most common way to conclude the radio is dead when it is fine.

**★ Corollary: the LuCI wireless panel for `DIR842-2G` shows dashes and zeros —
`Channel: 6 (0.000 GHz)`, `Signal: 0 dBm / Quality: 0%`, `BSSID: -`,
`Associations: -` — and that is NORMAL.** LuCI renders that page from iwinfo, so every
*runtime* field is empty; the fields that do show (SSID, channel, Master) are echoed
from `/etc/config/wireless`, not read from the radio. A blank panel says nothing about
whether the AP is up. Ground truth is the table below — clients in
`/proc/wlan0/sta_info`, beacons in `/proc/wlan0/stats`. (The 5 GHz panel shows real
numbers because rtw88 is mac80211/nl80211.)

**★ `iwpriv`/`iwconfig` are NOT in the shipped images** (wireless-tools is not
packaged), so stock's `iwpriv` diagnostic recipes do not run there. Nothing needs them:
netifd configures the driver via `/etc/Wireless/RTL8192CD.dat`, and status lives under
`/proc/wlan0/`. `opkg install wireless-tools` (19.07 feed) if you want them.

| | 5 GHz (rtw88 / mac80211) | 2.4 GHz (vendor rtl8192cd) |
|---|---|---|
| is it there | `iw dev`, `/sys/class/ieee80211/phy*` | **`/proc/wlan0/mib_all`** (268 MIBs) |
| link / clients | `iw dev wlan1 station dump`, `iwinfo` | **`/proc/wlan0/sta_info`** |
| is it beaconing | `iw dev wlan1 info` | `/proc/wlan0/stats` — `beacon_ok` climbing at the TBTT rate (10/s at 100 TU) |
| AP daemon | `hostapd_cli`, `logread \| grep hostapd` | none — the 4-way handshake is in-kernel |
| config knobs | uci → hostapd conf | uci → `/etc/Wireless/RTL8192CD.dat` (`iwpriv`/`iwconfig` work too but are **not in the images** — `opkg install wireless-tools`) |
| did netifd run setup | `/var/run/hostapd-phy*.conf` exists | **`/etc/Wireless/RTL8192CD.dat` exists** |
| enumerated at all | `lspci` → `10ec:b822`; boot line `PCIe RC0 link check: … link_up=1` | boot line `RTL8197F on-SoC 2.4 GHz WMAC: chip id 8197f001, hw id 100a` |

Notes that save time:

- `wlan0` operstate is permanently **`unknown`**, not `up` — normal for this WEXT driver.
- `up_flag` in the vendor procfs is a **red herring**: it is `CLIENT_MODE`-only, set when a
  *station* associates, never in AP mode.
- `/etc/Wireless` is a symlink to `/tmp` (`09_wireless-dualband-dir842:41`), matching
  stock, so writing the `.dat` never causes a flash write.
- Both radios present in `ubus call network.wireless status` is the quickest single check
  that netifd has actually adopted both.

---

## 9. 802.11r (fast transition) on both radios

Added in **v1.1**. The 5 GHz side is unremarkable — hostapd, `ieee80211r`, done. The
2.4 GHz side is the story: **the vendor driver has always implemented 802.11r, and this
port had simply never compiled it.**

### ★ The code was there; the flag was not

`8192cd_cfg.h` gates the whole feature:

```c
#ifdef CONFIG_RTL_11R_SUPPORT
#define CONFIG_IEEE80211R
#define SUPPORT_FAST_CONFIG 2
#endif
```

The driver `Makefile` enabled **12** other `CONFIG_RTL_*` features and not that one, so
`CONFIG_IEEE80211R` never got defined. What that looked like on a running box: the
module contained no FT symbols at all, so the MIB keys did not exist and the `.dat`
lines were rejected outright —

```
CFGFILE set_mib "ft_enable=1" failed
CFGFILE set_mib "ft_mdid=b1a0" failed
```

— and the advertised RSN IE carried a single AKM (`00-0F-AC:02`, plain PSK), which is
why clients never attempted fast transition on 2.4 GHz.

### ★ It is TWO changes, not one — the ccflag alone does not link

```make
CONFIG_RTL_11R_SUPPORT=y                  # make var: adds sha256.o (Makefile:661-662)
ccflags-y += -DCONFIG_RTL_11R_SUPPORT     # ccflag:   compiles the FT code
```

The make variable is the half that pulls in `sha256.o`, and FT's entire key hierarchy is
sha256: `sha256_prf()` derives PMK-R0/PMK-R1, `sha256_vector()` derives the `*_Name`
values. With only the `-D` the FT code compiles and then fails to link. Confirmed the
shipped v1.0 module has **zero** sha256 symbols and that none of the other three gates
that would pull it in (`CONFIG_RTL_11W_SUPPORT`, `CONFIG_RTL_11R_SUPPORT`,
`CONFIG_RTL_WAPI_SUPPORT`) is set.

### ★ No userspace FT daemon is needed

The driver has hooks for one — `wlanft_pid`, and the `SIOCSIWRTLSETFTPID` /
`SIOCGIFTGETEVENT` / `SIOCSIFTSETKEY` private ioctls — and it does not exist anywhere.
Not in this port, and not in D-Link's stock firmware either: stock's `/bin/auth` is a
Realtek 802.1x daemon v1.8f with not one FT string in it.

It is not needed for FT-PSK. `8192cd_psk.c` derives PMK-R0 **locally from the PSK**:

```
PMK-R0 = sha256_prf(PMK, "FT-R0", SSIDlen ‖ SSID ‖ MDID ‖ R0KHlen ‖ R0KH-ID ‖ S0KH-ID)
```

which is exactly what hostapd's `ft_psk_generate_local=1` does. The daemon would only be
required for FT-over-DS and for pushing R1 keys between APs; with `ft_over_ds 0` neither
applies. (`SUPPORT_FAST_CONFIG 2` also enables an R1 key *push*, but the push is guarded
by `wlanft_pid > 0`, which stays unset, so it degrades to pure local calculation.)

### Configuring it — vendor MIB, not hostapd options

hostapd never sees this radio, so `ieee80211r`/`mobility_domain` mean nothing to it. The
netifd handler translates the familiar UCI options into the vendor's MIB keys:

| UCI option | vendor MIB key |
|---|---|
| `ieee80211r 1` | `ft_enable=1` |
| `mobility_domain <4 hex>` | `ft_mdid=` (`BYTE_ARRAY_T`: a hex string, 2 chars/byte) |
| `ft_over_ds 0` | `ft_over_ds=0` |
| `reassociation_deadline 1000` | `ft_reasoc_timeout=1000` |
| `nasid <string>` | `ft_r0kh_id=` (the R0 key holder id) |

⚠ **The handler emits these BEFORE the encryption block, deliberately.** The driver
builds its advertised RSN IE when the cipher MIBs are applied; if `ft_enable` arrives
afterwards the IE has already been finalised without the FT-PSK AKM and clients will
never try fast transition.

FT is **opt-in per BSS** — the shipped config is open, and FT requires WPA2, so nothing
is enabled by default. Add the options above to a WPA2 `wifi-iface` to turn it on. Every
AP in a mobility domain needs the **same** `mobility_domain` and a **unique** `nasid`.

### Verifying

```sh
logread | grep -c 'set_mib .* failed'    # must be 0
grep rsnie /proc/wlan0/mib_auth
#  without FT: 3014 ... 0100 000fac02 0000            <- 1 AKM  (PSK)
#  with FT:    3018 ... 0200 000fac02 000fac04 0000   <- 2 AKMs (PSK + FT-PSK)
cat /proc/wlan0/ft_info                  # R0KHs / R1KHs populate as clients associate
```

### ★ Measured on air, both directions

Two independent clients. A phone's scan reports the BSS as
`[WPA2-PSK+FT/PSK-CCMP]`. A USB adapter associates with `key_mgmt=FT-PSK`, and the AP's
`ft_info` fills in with R0KH **and** R1KH entries carrying the configured `r0kh_id` and
derived PMK-R0/PMK-R1. Forced roams with `wpa_cli roam`, between this radio and a
separate hostapd AP on another channel:

```
rtl8192cd -> hostapd   ch6  -> ch11   FT: Completed successfully
hostapd -> rtl8192cd   ch11 -> ch6    FT: Completed successfully
```

Note the second direction in particular: the client presents the **other AP's** R0KH-ID,
and each side derives the keys for a key holder that is not itself. Realtek↔hostapd
interop works.

### Known limits

- **802.11k and 802.11v are still off** on this radio. Same story as 11r — the code is
  in the tree behind `DOT11K` / `CONFIG_RTL_11V_SUPPORT` and the Makefile does not set
  them. Not enabled here because they are untested; the flags are the obvious next step.
- **This radio is 802.11n (HT20).** Modern clients weight estimated throughput once RSSI
  saturates, so an 802.11ax AP will usually win the selection even when it is much
  weaker — measured: a phone preferred an ax AP at **-56 dBm** over this one at
  **-29 dBm**. FT here is correct, but do not expect clients to use it often when an ax
  AP is audible.

---

## 10. Cold-boot wireless datapath dead: `asic-wifi-settle`

Added in **v1.1**, alongside 802.11r. A different bug, found while testing it.

### The symptom

On a cold boot in **router role**, the S97 `dir842-asic` bring-up pass runs *after* the
wlan netifs are already up and bridged (boot log: `wlan1 AP-ENABLED 15:43:18` ->
`"ASIC up" 15:43:26`), and it still leaves the **wireless** datapath dead: the 2.4 GHz
BSS beacons at full signal, `wlan0` sits in `br-lan` "forwarding", and clients cannot
associate or get DHCP at all. Running `/etc/init.d/dir842-asic restart` by hand a few
minutes later fixes it every time — association, DHCP and forwarding all come good
instantly.

The wired path is never affected, which is exactly what makes this dangerous: the box
looks perfectly healthy from SSH while every wireless client is silently locked out.
Root cause: `gw_prog` wipes the ASIC L2 tables, and the boot pass evidently programs
them before the wireless side has genuinely finished settling.

### The fix, and its cost

`asic-wifi-settle` waits (up to 120s) for both radios to be up and bridged, sleeps a
further 20s to let hostapd finish its own churn, then re-runs `dir842-asic restart` —
a sequence that's already idempotent by design. **Cost: WiFi clients cannot pass
traffic for roughly the first minute after a cold boot in router role.**

### ★ Router role only

This shim exists specifically because `gw_prog` (router-role-only since the role-aware
ASIC bring-up fix landed in the same v1.1 — the full story, including the two real
house-wide outages it fixes, is `docs/SWITCH-AND-DATAPATH.md` §10) wipes the L2
tables the wireless side needs. In **bridge/dumb-AP role, nothing wipes those tables in
the first place, so this service returns immediately and does nothing** — measured: a
cold boot in bridge role with the service disabled still associated and passed traffic
at 0% loss, `settle_ran=0`. If you're running this box as a dumb AP, the ~1 minute
cold-boot cost above does not apply to you.

### Known limits

- **Only measured against a dual-radio, both-bridged router-role configuration.** A box
  running router role with only one radio enabled (or one that never comes up) will wait
  the full 120s before giving up and re-programming anyway — untested, since nobody has
  run this port with a radio deliberately disabled. The log line distinguishes the two
  outcomes (`"wireless settled"` vs. `"gave up waiting... re-programmed anyway"`), so a
  140s-total wait with the latter message is the signal that this path was hit.

---

## 11. Open items

1. **5 GHz TX power is uncalibrated** (blank efuse). Believed still true; **not
   re-verified** recently.
2. **The mtd1 field order at `+0x13e`** (crystal cap and its neighbours `39, 49, 35, 24`)
   was never resolved from the binary. `xtal_cap_override = 39` is **empirical**.
3. **No per-unit 2.4 GHz calibration push.** Stock's userspace `iwpriv set_mib` sequence
   from `librlx_wifi_mibs.so` (`pwrlevelCCK_A/B` @ `0x0d8`/`0x0e6`, `pwrlevelHT40_1S_A/B` @
   `0x0f4`/`0x102`) has no equivalent here. The obvious next win for 2.4 GHz range.
4. **`regdomain` is numeric.** The handler emits `${phy}_regdomain=$country`
   (`rtl8192cd.sh:233`) and that MIB takes a number, so a real code like `BR` would be
   rejected. It works today only because the seed sets `country='1'`
   (`09_wireless-dualband-dir842 (radio1 country)`). The mac80211 side, independently, uses `country='BR'`
   (`99-dir842-m5 (radio0 country)`) — correct there, and *not* interchangeable.
5. ⚠→✅ **The `VENDOR_SDK=` build path was WITHDRAWN 2026-08-02 — verified broken by
   dry-run, then removed rather than shipped broken.** `build.sh` no longer accepts
   `VENDOR_SDK=`; the two port patches stay in the repo root as the record of the
   work. The dry-run evidence is kept here so a future reintroduction does not start
   from zero:
   - **Wrong patch base, fatal on its own.** `g3-rtl8192cd-4.14-port.patch` is rooted
     at `package/kernel/rtl8192cd/…` — the 8devices layout it was developed on, which
     its own diff headers record — and that path exists nowhere in the pinned base,
     so `patch -p1` matches nothing. (`g4-rtl-headers-4.14-port.patch` *is* rooted at
     the destination layout and lines up.)
   - **Re-rooted onto the SDK layout it still fails: 15 hunks across nine files.**
     `Makefile` 4/4, `Kconfig` 1/1, `8192cd_cfg.h` 2/2, `8192cd_dfs.c`,
     `8192cd_util.c` — all "(different line endings)", the SDK files are CRLF — plus
     real content drift in `8192cd_osdep.c` 2/4, `8192cd_rx.c`, `8192cd_tx.c`,
     `wifi.h` 2/4. The Makefile hunks are the load-bearing ones: they carry
     `CONFIG_BAND_2G_ON_WLAN0=y` and `CONFIG_RTL_COMAPI_CFGFILE=y`.
   - **The SDK layout lacks sources the ported Makefile requires**: `8192cd_11v.*`
     and `8192cd_debug.c` (both referenced by the working Makefile), plus
     `phydm_soml.*`, `8192cd_smart_roaming.c`, `btcoexist/`, and three
     `WlanHAL/Data/8197F/` PHY/TXPWR tables (live driver dir: 128 top-level entries;
     SDK: 119). The patch itself creates only `8192cd_owrt_bsp.c` and
     `Makefile.vendor` — the other missing files have no source to come from.
   - **`g3-rtl8192cd-portflags.mk` was deleted with the feature.** Its flags
     (`CONFIG_BAND_5G_ON_WLAN0=y`, `CONFIG_PHY_EAT_40MHZ=y`, no
     `CONFIG_RTL_COMAPI_CFGFILE`) were the G3-era set that §4's three silent failures
     later corrected; nothing ever included it, and the working flag set is already
     recorded in the `+` side of the g3 patch's `Makefile` hunks. Ground truth
     remains the live build tree's `…/rtl8192cd/Makefile:189,224,233`.

   Net: reintroducing 2.4 GHz reproduction needs the **matching 8devices-vintage
   tree** the patch was developed against — or regenerating the patch against the
   SDK layout with line endings normalised and the nine missing sources supplied —
   not a path rewrite.
6. ✅ **RESOLVED 2026-08-02 — the `delete wireless` ordering hazard does not exist;
   measured on-box.** Whole-config `uci delete wireless` (no section name) is an
   **invalid statement**: it returns `uci: Invalid argument` (exit 1) and deletes
   nothing — inside the `uci -q batch` at `99-dir842-m5 (radio0 block)` it is a silent no-op.
   Proven two ways: against a scratch config (`uci -c /tmp/ucitest` — all four wifi
   sections survived it), and on the running box, where `99-`'s overlay whiteout
   shows it ran yet `radio1` still carries `09_`'s exact seed. The earlier analysis
   ("would wipe the radio1 stanza on a virgin overlay") assumed the statement worked;
   it never has — [`BENCH.md`](BENCH.md) §11.15 had in fact already read the mechanism
   from uci source (`uci_delete()` asserts `ptr->s`, `list.c:590`), and this
   measurement confirms it on the box. ⚠ The corollary, now commented on the batch itself: do **not**
   "repair" that line into a working per-section wipe — deleting `radio1` after `09_`
   has self-deleted is exactly the 5 GHz-only trap the original analysis feared. If a
   reset of the 5 GHz config is ever wanted there, target it:
   `delete wireless.radio0` / `delete wireless.default_radio0`. (Residual and
   low-stakes: whether `radio0` exists yet when `10_` first runs on a truly virgin
   overlay — `10_`'s `exit 1` retry-next-boot guard covers both outcomes.)

---

## ★ 5 GHz is ~13-15 dB quieter than it should be — measured, and five explanations refuted

**Symptom:** a client in the SAME ROOM as the box measures the 5 GHz AP at **-67 to -81 dBm**
and speedtests at ~130 Mbit/s. The link negotiates `VHT-MCS 7 80MHz NSS 1` (292.5 Mbit/s
PHY), so ~130 Mbit/s of TCP is *normal efficiency for that PHY rate* — the PHY rate is the
problem, and the PHY rate is a consequence of signal.

### The measurement that localises it

Both of our own radios, scanned by the same phone at the same instant:

| our radio | freq | client RSSI |
|---|---|---|
| 2.4 GHz (vendor `rtl8192cd`, `wlan0`) | 2437 | **-60 dBm** |
| 5 GHz (rtw88 8822BE, `wlan1`) | 5180 | **-81 dBm** |

21 dB apart. 5 GHz costs ~6-8 dB of extra path loss at these frequencies, so roughly
**13-15 dB is unexplained** and it is specific to the 8822BE path. ★ The 2.4 GHz radio is
the control: board, placement and antennas are fine.

For reference the house's main router reads **-40 dBm** on its 5 GHz from the same spot.

### The driver is asking for full power

Not a power-index bug: a blank efuse leaves `txpwr_idx_table[]` all `0xff`, which clamps
*high* against `max_power_index = 0x3f`, so the requested power ends up at the regulatory
ceiling. `iw` confirms `txpower 17.00 dBm` on ch36. The power is requested and not radiated.

### ★ Refuted — do not re-try these

| hypothesis | test | result |
|---|---|---|
| Mistuned crystal (`xtal_cap_override`, patch 03's own theory) | swept stock-partition candidates 39/49/35/24, then 0-63 | **REFUTED** — the RF WARN is intermittent ~50% and value-independent; same value gives pass/fail/fail. Confound #22 |
| Wrong RF front-end type | `rfe_option` 2 (eFEM) / 3 / 5 (iFEM) swept at runtime | **REFUTED** — 2 (the shipped guess) is the BEST: -74 vs -84 (3) and -81 (5) |
| Regulatory ceiling (BR allows 17 dBm on ch36 but 30 dBm on ch149) | moved to ch149; `txpower` did rise 17 -> 30 dBm | **REFUTED, and it BACKFIRED** — client RSSI went -67 -> **-87**. ★ On this unit the LOW band is the better band despite the lower legal cap. Reverted |
| CPU / bridge bottleneck | `/proc/stat` during a client transfer | **REFUTED** — box ~85% idle. (Separately, terminated TCP tops out ~152 Mbit/s, so the CPU is a ceiling but not this one) |
| Hardware NAT offload not working | `hwnat=N` | **NOT APPLICABLE** — the box is a pure bridge (no `network.wan`); offload accelerates routed/NAT traffic and there is none. Enabling it changes nothing |
| `NSS 1` on a 2x2 chip is an anomaly | signal check | **NO** — at -67 dBm, MCS 7 / NSS 1 is correct rate adaptation, not a fault |

### Where that leaves it

The 8822BE requests full regulatory power and radiates ~13-15 dB below its own 2.4 GHz
sibling, and no software knob available recovers it. This is consistent with what
`README.md` already warns: the blank efuse means **no per-unit RF calibration**. Tonight
only quantified the cost.

★ **Bench note:** do NOT characterise this with repeated `rmmod`/`modprobe` of
`rtw88_8822be`. After a few cycles the interface loses the profile MAC (blank efuse ->
random MAC per probe), `txpower` drifts 17 -> 20 dBm, and the AP stops being scannable —
the readings become meaningless. Reboot between measurements instead.

### Deeper pass with `RTW88_DEBUGFS` — what the chip is actually doing

Enabling `CPTCFG_RTW88_DEBUG`/`RTW88_DEBUGFS` (in `openwrt/package/kernel/mac80211/realtek.mk`;
diagnostic only, **not shipped**) exposes `tx_pwr_tbl`, `rf_read`/`rf_write`,
`read_reg`/`write_reg` and `phy_info` under
`/sys/kernel/debug/ieee80211/phy1/rtw88/`. Four things came out of it, and together they
rule out everything rtw88 controls.

**1. The driver already asks for maximum power.** `tx_pwr_tbl` shows every rate on every
path at `pwr = 63 (0x3f)` — `max_power_index`. A blank efuse leaves the base at 254, which
clamps *high*, not low:

```
path rate       pwr       base      (byr  lmt ) rem
   A  OFDM_6M    63(0x3f)  254   -2 (  14   -2)    0
   A  MCS9       63(0x3f)  253  -10 (  12  -10)    0
```

★ So this is **not** a power-table or calibration-index problem. The chip is told to
transmit at full gain and still radiates ~13-15 dB low. The loss is downstream of the
power computation.

**2. The RF front-end IS correctly configured.** Despite the `write RF mode table fail`
WARN (which early-returns past `set_channel_rfe`), the eFEM registers read back exactly
what rtw88 intends for 5 GHz:

| reg | live | rtw88's 5G eFEM target |
|---|---|---|
| `0xcb0` RFESEL0 | `0x77`**`177517`** | `0x177517` ✓ |
| `0xcb4` RFESEL8 | `0x0000`**`75`**`77` | byte1 `0x75` ✓ |
| `0xcb8` RFECTL | `0x00000000` | BIT(5) clear ✓ |
| `0xca0` TRSW | `0x0000a501` | `0xa501` = 2TX/2RX ✓ |

★ "The external PA is stuck in bypass because the WARN skipped its setup" is therefore
**refuted**.

**3. Both RF paths are alive and identically tuned.** `rf_read` on paths 0/1: `rf[0x18]`
(channel) identical, `rf[0x55]`/`rf[0x56]`/`rf[0x8f]` identical. No dead chain.

**4. ★ rtw88 implements only a fraction of this chip's front-end variants.** The stock
kernel carries Realtek's full ODM driver, with `phy_reg_pg` and `txpwr_lmt` tables for RFE
types **2, 3, 4, 5, 12, 15, 16, 17, 18**, and describes them:

```
RFE type 2: APA1+ALNA1+GPA1+GLNA1      RFE type 4: APA1+ALNA3+GPA1+GLNA1
RFE type 3: APA2+ALNA2+GPA2+GLNA2      RFE type 5: APA2+ALNA4+GPA2+GLNA2
RFE type 0: ALNA5+GLNA3   (LNA only, no PA)
```

rtw88 has only `bb_pg` types 2/3/5 and `txpwr_lmt` types 0/2/5. With the efuse blank the
type cannot be read, and our fallback guesses 2. **If this board is any of 4/12/15/16/17/18,
rtw88 cannot represent it and no available option is correct.** That is the leading
remaining explanation and it is a driver-coverage limitation, not a misconfiguration.

### ★ A measurement warning that invalidates part of the earlier sweep

Client RSSI of our AP swung between **-64 and -87 dBm across this session for nominally
identical configurations** — the phone moves, and Android's scan cache ages. The
`rfe_option` 2/3/5 comparison recorded above (-74 / -84 / -81) was **single-sample inside
that noise band and must not be treated as a ranking.** Re-run it, if at all, with the
position-independent metric: measure our AP and a fixed reference AP **in the same scan**
and compare the delta, averaged over several scans (`tools/bench/` idea, not yet landed —
the phone was disconnected before a baseline could be taken).

Also: `write_reg` cannot be used to explore RFESEL values. `rtw_write32s_mask()` is a
special multi-page BB write that the driver re-applies, so a poke to `0xcb0` reads back
unchanged.

### Where this leaves the 5 GHz signal

Everything rtw88 controls is set correctly: full power index, correct eFEM registers, both
paths live, right channel and bandwidth. The deficit is real (~13-15 dB vs our own 2.4 GHz
radio, measured same-client same-instant) and is **not fixed**. Two candidate paths remain,
neither a config change:

1. **Determine the board's true RFE type and port the missing tables** from the stock ODM
   driver (`odm_read_and_config_mp_8822b_phy_reg_pg_type*` / `txpwr_lmt_type*`). Stock
   reads the same blank efuse, so how *it* decides the type is the thing to find.
2. **Rule out the antenna path physically.** Both directions are weak (the AP hears the
   client poorly too), which is more consistent with a feedline/connector problem than with
   a PA-only fault — a PA fault would degrade TX alone. This is a hardware check, not a
   software one.
