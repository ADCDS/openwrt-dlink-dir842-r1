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

## ★ 5 GHz is much quieter than it should be — measured, and nine explanations refuted

> ★ **Read the later subsections first.** Everything down to "A measurement warning" was
> measured with a hand-held phone before a fixed receiver existed. The "13-15 dB" figure
> below is **superseded** — with a fixed receiver the gap is roughly 15-30 dB depending on
> the boot — and the conclusion that the fault is bidirectional is **retracted** (it is
> TX-only). The findings that stand are in "Instrumentation solved" onward.


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

### The `txpwr_idx_table` blank-efuse gap — analysed, and it is NOT the cause

`rtw8822b.c` copies `efuse->txpwr_idx_table[i] = map->txpwr_idx_table[i]` with **no 0xff
check**, unlike `crystal_cap` / `pa_type` / `lna_type` which all have blank-efuse fallbacks.
On this board that table is therefore all `0xff`, and `tx_pwr_tbl` shows the consequence:

```
pwr = 63 (0x3f)   base = 254   byr = 14   lmt = -2
```

`base + byr` saturates against `max_power_index = 0x3f`. It is tempting to "fix" this by
defaulting the table the way the other fields are defaulted.

★ **Do not — it would make things worse.** Index 63 is *maximum* TX gain, so saturating
high means the chip is already being driven as loud as it goes. Any sane default (0x2D,
etc.) is LOWER than 63 and would reduce output. And an over-driven PA would show as high
RSSI with poor EVM, not the low RSSI actually measured. The power-index path is therefore
ruled out as the cause of the deficit; the loss is downstream of it.

### ★ Instrumentation solved — `tiny`, and a drift-free metric

The blocker above (no fixed 5 GHz receiver) is **solved**. `tiny` is a Raspberry Pi 4
in the same room as the box, on mains power, at a fixed position, reachable over
`ssh tiny`. It scans 5 GHz fine (its `wlan0` Band 2 lists ch36 at 17 dBm; an early
"0 channels" reading was a grep artefact, not a capability limit).

Two rules make its numbers trustworthy, and both are load-bearing:

1. **Average several scans.** Single scans move 2-4 dB.
2. **Report ours MINUS a fixed reference AP seen in the same scan.** Raw RSSI drifted
   ~6 dB over one session for an unchanged config, and ~8 dB across a reboot, while
   the 2.4 GHz control stayed put. `50:4f:3b:32:68:9f` on 5180 is the reference in
   use; our own 2.4 GHz radio is the cross-band control. ★ That reference is **another
   BRAVO AP in this house, co-channel with us on 5180** — not a neighbour, as an earlier
   draft of this section said. It is still a valid fixed reference (that is all the
   metric needs), but do not read "we beat the reference by N dB" as beating a stranger.

`tools/bench/rf-measure.sh` implements this. **Do not compare raw RSSI across
reboots** — that is the trap that invalidated the first `rfe_option` sweep.

### ★★ The deficit is TX-ONLY — this corrects the conclusion above

The previous section's remaining hypothesis 2 said *"Both directions are weak (the AP
hears the client poorly too), which fits a feedline/connector fault."* **That is wrong,
and the correction matters because it rules out a whole class of causes.**

The box's own 5 GHz receiver is healthy. Scanning from the box (`iw dev wlan1 scan`)
and from `tiny` in the same room, across six 5 GHz neighbours seen by both, the box
reads **2-13 dB lower (mean ~7 dB)** — ordinary receiver/antenna variation, not a
fault. The box hears the house's main router on 5745 at **-53 dBm**, and hears it
*louder* than that router's 2.4 GHz radio (-58 dBm).

★ **RX and TX share the same antenna, cable and connector, and antenna loss is
reciprocal.** A feedline, connector, pigtail or antenna fault degrades both directions
equally. RX is fine, so **the antenna path is proven good and cannot be the cause.**
The fault lies in the non-reciprocal part of the chain only: the PA, the T/R switch in
its transmit state, or the transmit baseband.

Measured, same receiver, same room, same instant:

| transmitter | distance | RSSI at `tiny` |
|---|---|---|
| our box, **2.4 GHz** (`rtl8192cd`) | same room, ~3 m | **-32 dBm** |
| our box, **5 GHz** (rtw88 8822BE) | same room, ~3 m | **-62 to -70 dBm** |
| a **neighbour's** 5 GHz AP, another home | through walls | **-55 dBm** |

★ A neighbour's AP behind walls beats our same-room AP.

★ **Quote the band delta, not a single figure.** Our 2.4 GHz minus our 5 GHz — same box,
same room, same receiver, same scan — measured **24 to 38 dB** across this session, where
the frequency ratio alone accounts for ~6.5 dB. The spread is real boot-to-boot variance
in the 5 GHz radio (the 2.4 GHz control held within 3 dB throughout), so the honest
statement is **roughly 15-30 dB unexplained, varying by boot**, not a single number. As a
cross-check against free-space loss (~56 dB at 5.18 GHz over ~3 m), a 17 dBm transmitter
should read ~-39 dBm: the 2.4 GHz radio lands within ~4 dB of its own such prediction on
every boot, the 5 GHz radio between ~19 dB (best boot) and ~31 dB (worst) below its own.
★ The absolute figures assume an estimated distance and a nominal 2.4 GHz TX power, so
they are supporting evidence; the band delta and the ours-minus-reference delta are the
measurements that carry the argument.

### ★ Four more explanations refuted, with a working instrument

| hypothesis | test | result |
|---|---|---|
| The `write RF mode table fail` WARN skips the RF mode LUT, leaving TX unprogrammed | read RF `0x3e`/`0x3f` back, then write the skipped sequence by hand via `rf_write` | **REFUTED** — the LUT *already* held the correct values (`0x34`, `0x4080c`). The driver probes twice and a later `config_trx_mode` succeeds. Re-writing changed nothing (-60 -> -58, inside noise) |
| eFEM vs iFEM T/R switch word mis-drives the antenna switch on transmit | poke `REG_TRSW` `0xca0` from `0xa501` (eFEM) to `0xa5a5` (iFEM 5G) live, measure, restore | **REFUTED** — no effect (-64/-65 either way). ★ Unlike `0xcb0`, `0xca0` *is* pokeable with `write_reg` and the value sticks |
| The PA is over-driven into compression: a blank efuse pins the index at max, ~10 dB above this unit's factory calibration, so output collapses | `txpwr_base_override` swept 63/48/42/36/30, re-applied live with `iw reg set` | **REFUTED, decisively** — response is monotonic and linear, ~0.5 dB per index step (17 dB over 33 steps). 63 is genuinely the loudest setting. ★ See the warning below: this also proves the factory values must NOT be written into rtw88 |
| A different RFE type is correct for this board | `rfe_option_override` 2/3/5, one reboot each, drift-free metric | **REFUTED** — 2 (the shipped guess) is best by 6-10 dB: ours-minus-ref **+3..+9 (2)**, **-4 (3)**, **-2 (5)**. This *replaces* the earlier single-sample sweep, which was noise; the ranking happens to agree, but only now is it evidence |

### ★★ This unit's factory RF calibration exists — in NOR, not the efuse

The efuse is blank, but the per-unit calibration is **not missing**. It is in the
read-only `mtd1` "MAC" partition (64 KB), which `sysupgrade` never touches:

```
0x0d8..0x118   2.4 GHz TX power indices: four 14-entry tables (14 = 2.4 GHz channels)
               runs 46x9 48x5 | 45x3 46x6 47x5 | 41x9 43x5 | 41x9 43x5   -> values 41-48
0x11e..0x13e   two 14-byte tables: 35 x14, 68 x14
0x13e..0x142   39 49 35 24   <- the xcap/thermal bytes (already known)
0x171..0x200   5 GHz per-channel TX power indices -> values 35-42
0x235..0x2c8   5 GHz per-channel TX power indices -> values 35-45
0x2f9..0x366   17 (0x11) x109
```

★ **This explains why 2.4 GHz is healthy and 5 GHz is not.** The 2.4 GHz radio is the
vendor `rtl8192cd` driver, which reads this MIB partition, so it transmits at its
calibrated 41-48. rtw88 reads only the (blank) efuse, so 5 GHz has no per-unit data.

★★★ **Do NOT "fix" this by feeding the NOR values into rtw88's `txpwr_idx_table`.**
It is the obvious move and it is measurably wrong. The sweep above shows index and
output are linear with 63 the maximum, so writing 35-45 would cut 5 GHz output by a
further **10-14 dB**. The two scales are not the same quantity: the vendor MIB indices
are the ODM driver's units, not rtw88's `max_power_index = 0x3f` scale. Confirmed by
experiment, not argued from theory.

### ★★ Two-way link budget — the strongest evidence, and it needs no assumptions

Associating a client to our own 5 GHz AP gives both directions across **one reciprocal
path**, so path loss, distance and antenna gain all cancel. With `tiny` associated:

| direction | measured |
|---|---|
| our AP -> tiny (client's view) | **-60 dBm** |
| tiny -> our AP (`station dump`) | **-52 dBm**, avg -49 |

Path loss is identical both ways, so `TX_ap - TX_tiny = -60 - (-52) = -8 dB`:
★ **our AP transmits about 8 dB less than a Raspberry Pi 4's onboard Wi-Fi.** A router
with external antennas should beat an RPi by several dB, so the true shortfall is ~13 dB.
This is the cleanest statement of the deficit in this document — no estimated distance,
no free-space model, no cross-receiver calibration.

★ `station dump` also showed `signal: -52 [-64, -52]`, a 12 dB spread between the two RX
chains. **Do not build on that** — it is one instantaneous sample and the per-chain TX
test below found the chains equal; treat it as multipath at that instant unless it
reproduces.

### ★ Per-chain test — no dead chain (and it closes a hole in the reciprocity argument)

The reciprocity argument above proves the *shared passive path* is good, but RX combines
both chains, so **a single dead TX chain could hide behind the good one on receive.**
That gap was closed by testing each chain alone. ★ `iw phy1 set antenna` returns
`-122 Not supported` while the AP is running — it must be done with the radio down
(`wifi down` -> `iw phy1 set antenna <tx> <rx>` -> `wifi up`); an earlier attempt that
skipped this silently measured the default config three times.

| config | ours-minus-ref |
|---|---|
| TX/RX path A only (`1 1`) | **+11 dB** |
| TX/RX path B only (`2 2`) | **+11 dB** |
| both (`3 3`, default) | **+12 dB** |

Both chains are individually healthy and equal, and one chain alone radiates essentially
what two do. **No dead chain, and no per-chain fault to fix.**

### ★★ Throughput is NOT limited by the 5 GHz link, and NOT by the box's CPU

The complaint that started this ("speedtest tops at 130 Mbit/s") is a separate question
from the signal deficit, and both obvious explanations are wrong:

| PHY rate | width | TCP throughput |
|---|---|---|
| 351 Mbit/s (VHT-MCS8) | 80 MHz, ch36 | 105 Mbit/s |
| 200 Mbit/s (VHT-MCS9) | 40 MHz, ch48 | 97.5 Mbit/s |

★ **Throughput is flat across a 1.75x change in PHY rate.** Whatever limits it is not the
radio link. The RPi4's onboard Wi-Fi caps around 100-120 Mbit/s and is the likely ceiling
*in this test* — note the user's phone reached 130, i.e. more than `tiny` can do, so
**`tiny` cannot be used to characterise the box's maximum throughput.**

★★★ **The box is not CPU-bound.** Measured with `/proc/stat` deltas over an 8 s window:
**8 % busy idle, 17 % busy (82 % idle) during a ~98 Mbit/s transfer.** See confound #29 —
a single `top -bn1` sample during the same transfer read "75 % sys, 25 % idle" and was
nearly written up here as "the throughput ceiling is the SoC's CPU". It is not.

### ★ `config_trx_mode`'s early return is fully accounted for

The `write RF mode table fail` WARN early-returns from `rtw8822b_config_trx_mode()`. Every
TX-path register (`REG_AGCTR_A/B`, `REG_CDDTXP`, `REG_TXPSEL`, `REG_TXPSEL1`, `REG_ADCINI`,
`REG_RXDESC`, `REG_RXPSEL`) is written **before** the poll. The return skips only
`rtw8822b_toggle_igi()` and `rtw8822b_set_channel_cca()` (both RX-side), the RF mode LUT
(proven already correct) and `set_channel_rfe()` (proven applied later). ★ **Nothing
TX-critical is skipped**, so the intermittent WARN cannot explain the deficit or the
boot-to-boot variance.

### ★★★ The stock firmware, and what it settles

The stock 8 MB NOR backup is in the private gitea repo **`adriel/dir842-firmware`** (full
dump, per-mtd slices, extracted rootfs, `RESTORE-TO-STOCK.md`). ★ Clone it over **HTTPS**
-- `ssh://git@gitea.adr:2222` times out.

```
git clone https://gitea.adr/adriel/dir842-firmware.git
# kernel is raw LZMA at offset 0x3818 inside mtd3-kernel.bin:
dd if=mtd3-kernel.bin bs=1 skip=$((0x3818)) of=k.lzma
python3 -c "import lzma;open('vmlinux.bin','wb').write(
  lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(open('k.lzma','rb').read()))"
```

★ **`mtd1-MAC.bin` in the repo is byte-identical to what this box reports live**
(`md5 ed88837e88b54d5f160b1a5d12f3c699`), which independently validates the calibration
decode above.

**Stock carries the complete 8822B ODM in-kernel** -- `odm_read_and_config_mp_8822b_
{phy_reg_pg,txpwr_lmt}_type{2,3,4,5,12,15,16,17,18}` -- i.e. every RFE type rtw88 lacks.
It drives the 8822B from the **vendor `rtl8192cd` driver**, not a separate module, and
hard-codes this board's settings (`hard_code_8822_mibs`, `set_8822_trx_regs` are
`rtl8192cd` symbols). That is how stock works with a blank efuse.

★★★ **And it settles the previous "leading remaining explanation": porting those tables
CANNOT fix this.** `phy_reg_pg` (power-by-rate) and `txpwr_lmt` (regulatory limit) are both
**offsets applied to the base index**, and this board's base saturates at 254 -> clamped to
`max_power_index` 63 for every rate. Offsets can only bring power **down** from that
ceiling. No RFE-type table, correct or not, can raise output above index 63 -- which we
already have. ★ This is the same trap as the NOR calibration values: an obvious,
plausible "we found the missing data!" fix that measurement shows would only make it
quieter. **Do not spend a session porting ODM tables to chase this deficit.**

Two further comparisons against stock, both negative:

| checked | result |
|---|---|
| Stock's `RFE_Init` vs rtw88's `rtw8822b_phy_rfe_init` | **IDENTICAL** -- disassembly of the stock function shows the same seven writes in the same order: `0x64` mask `0x30000000` = 3, `0x4c` mask `0x06000000` = 0, `0x40` bit2 = 1, `0x1990` mask `0x3f` = `0x30`, `0x1990` bits 11:10 = 3, `0x974` mask `0x3f` = `0x3f`, `0x974` bits 11:10 = 3 |
| Stock's requested TX power | `"5G_TxPower": "100"` in `/etc/config.default` -- 100 %, exactly what we ask for |

### ★ RFE pad polarity swept — the front-end control is live and already correct

`REG_RFEINV` (`0xcbc`) bits 0-5 invert the six RFE output pads that drive the external
front end. If a PA-enable pad were the wrong way round, flipping it would switch the PA on.
Swept live (`write_reg`, baseline `0x100`):

| RFEINV | ours-minus-ref |
|---|---|
| `0x100` (baseline, pads as rtw88 sets them) | +4 dB |
| `0x101` / `0x102` bits 0,1 | +5 / +3 dB |
| **`0x104` bit 2** | **-10 dB** |
| `0x108` / `0x110` / `0x120` bits 3,4,5 | +6 / +6 / +8 dB |
| `0x13f` all six | -9 dB |

★★ **Pad 2 is load-bearing: inverting it costs 14 dB.** That pad carries the PA-enable /
T-R control, and rtw88's existing polarity is the correct one. So there is no fix here --
but it is positive evidence that **the RFE pad path is wired, live and correctly driven**,
which together with the identical `RFE_Init` means the front-end *control* is not the fault.

### ★★★★ ROOT CAUSE: this board is RFE type 10, and rtw88 configures it as type 2

The answer was in this repo the whole time. `files/target/linux/realtek/files-4.14/
drivers/net/wireless/rtl8192cd/Makefile` records the vendor build flags for this board:

```
# (G4) CONFIG_SLOT_0_8822BE=y
# (G4) CONFIG_SLOT_0_RFE_TYPE_10=y      <- the DIR-842's 8822BE is RFE TYPE 10
```

and the vendor PHYDM defines what type 10 *is*
(`phydm/rtl8822b/phydm_hal_api8822b.c`, `phydm_init_hw_info_by_rfe_type_8822b()`):

| | **type 10 — what this board IS** | **type 2 — what rtw88 assumes** |
|---|---|---|
| comment | `QFN iFEM AP PCIE` | eFEM |
| BOARD_TYPE | `ODM_BOARD_EXT_TRSW` | `EXT_LNA_5G \| EXT_PA_5G` |
| `5G_EXT_PA` | **FALSE** | **TRUE** |
| `5G_EXT_LNA` | **FALSE** | TRUE |
| PACKAGE_TYPE | 1 | 2 |

★★★ **rtw88 believes there is an external 5 GHz power amplifier on this board. There is
not.** The only external RF part is a **T/R switch**. That mismatch is a real, documented
defect in how we configure this board.

★★★★ **But it is NOT, by itself, the 13 dB — correcting it was tried and made things
worse.** The tempting mechanism ("eFEM mode makes the chip drive low expecting external
gain") was written here first and then **tested**: a type-10 entry was added to rtw88
(iFEM CCA/tables + the eFEM external-TRSW pin config, the combination this board actually
needs), built, flashed and measured. Result: **ours-minus-ref 0 dB, versus +3..+12 for the
shipped type 2** -- i.e. ~7 dB *worse*, landing alongside the other iFEM options. See
retraction #44. ★ The reason is that rtw88 **does not model a drive-level difference
between eFEM and iFEM at all**: in rtw88 the RFE type selects only CCA thresholds, the RFE
pin pattern, and `bb_pg`/`txpwr_lmt` offsets -- and those offsets cannot raise output past
the already-saturated `max_power_index`. The ODM behaviour that *would* differ lives in the
vendor PHYDM's gain/AGC handling, which rtw88 has no equivalent of.

★★ **It also explains the result that made no sense before: why rtw88's iFEM options
measured WORSE.** rtw88 has exactly two shapes and this board fits neither:

| rtw88 option | ext-PA assumption | ext-TRSW handling | measured |
|---|---|---|---|
| **2** (eFEM) — shipped | **wrong** (assumes ext PA) | drives TRSW `0xa501` — works with the real ext TRSW | best available, still ~13 dB low |
| **3 / 5** (iFEM) | right (no ext PA) | sets TRSW `0xa5a5`, i.e. no ext TRSW — **wrong for this board** | 6-10 dB worse |

★ So the shipped guess is best not because it is right, but because *its* error costs less
than the other error -- and that ranking held up when the "correct" shape was actually
built and measured:

| rfe_option | ours-minus-ref |
|---|---|
| **2 (eFEM, shipped)** | **+3 .. +12 dB** |
| 3 (iFEM) | -4 dB |
| 5 (iFEM ext) | -2 dB |
| **10 (iFEM + ext TRSW, purpose-built for this board)** | **0 dB** |

★★ **Type 2 remains the right thing to ship**, on measurement, despite being the
theoretically wrong description of the hardware. The board being type 10 is a true and
useful fact; "therefore configure rtw88 as type 10" is a false conclusion from it.

★★ This supersedes the earlier note that "the board's true RFE type is unknown, and if it
is 4/12/15/16/17/18 rtw88 cannot represent it". The type is **10**, it is *also* absent from
rtw88, and the reason it hurts is **not** the missing power tables (those are offsets and
cannot raise output past the saturated maximum — retraction #43) but the **eFEM/iFEM
front-end assumption**.

### ★★★ The vendor's own config for this board measures WORSE — the contradiction that ends the software hunt

Reading the vendor PHYDM's RFE pin function (`phydm_rfe_8822b()`,
`phydm/rtl8822b/phydm_hal_api8822b.c`) settles what type 10 is *supposed* to write.
Type 10 shares a branch with types 0/3/5/8/12/13/14, and for 5 GHz (`channel > 35`) it
writes:

| register | vendor, type-10 group | rtw88 `rtw8822b_set_channel_rfe_ifem` |
|---|---|---|
| `0xcb0` RFESEL0 | `0x477547` | `0x477547` ✓ |
| `0xcb4` RFESEL8 byte1 | `0x75` | `0x75` ✓ |
| `0xcbc` RFEINV bits 5:0 / 11:10 | `0x0` / `0x0` | `0x0` / `0x0` ✓ |
| `0xca0` TRSW | **`0xa5a5`** | **`0xa5a5`** ✓ |

★★★★ **rtw88's iFEM path already reproduces the vendor's configuration for this board
exactly** -- and on this unit that configuration measures **6-10 dB WORSE** than the eFEM
config rtw88 picks by accident (options 3/5/10: -4/-2/0 dB; option 2: +3..+12 dB).

★★ That is a direct contradiction between *what the vendor says this board is* and *what
actually performs best on this unit*, and it is not resolvable by reading more source.
Only two explanations remain, and both are outside rtw88:

1. **The unit does not match its nominal board type** -- a hardware variance or fault in
   the 5 GHz transmit chain, which would also explain why eFEM (a configuration that
   assumes an external gain stage) happens to suit it better.
2. **The type-10 attribution is wrong.** It comes from a commented `# (G4)
   CONFIG_SLOT_0_RFE_TYPE_10=y` line in the vendor Makefile; it is good evidence but not
   proof of *this* unit's strap.

★★★ **Both are settled by the same experiment, and only by it: flash stock and measure.**
Everything needed is now on hand -- `adriel/dir842-firmware` has the verified 8 MB dump
and per-mtd slices, serial/loader recovery works, and `known-good-images/` restores us.
★ Note `RESTORE-TO-STOCK.md` in that repo is **stale**: it says the port is "RAM-boot only
-- it never writes flash", which stopped being true at v1.1. Restoring stock now means
writing the firmware partition, not just power-cycling.

### ★★★★★ THE STOCK A/B — RUN AT LAST. The hardware is FINE; stock is 17 dB louder

Stock was flashed back onto this unit and measured with the same fixed receiver in the
same session. **This settles the question the whole hunt was blocked on.**

| firmware | 5 GHz ch | 5 GHz at `tiny` | 2.4 GHz at `tiny` (control) |
|---|---|---|---|
| **stock D-Link** | 149 (5745) | **-42 dBm** (-42,-42,-44,-43,-41) | -33..-41 |
| **our OpenWrt** | 36 (5180) | **-59 dBm** (-59,-60,-60,-60) | -32..-38 |

★★ The **2.4 GHz radio reads the same on both** -- it is the same SoC WMAC driven by the
same vendor driver in both firmwares -- which validates receiver, position and geometry.
Against that control, **stock's 5 GHz is ~17 dB stronger on this exact unit.**

★★★★ **Therefore the 8822BE transmit chain is NOT faulty.** Every "maybe it is hardware"
line of reasoning in the sections above is closed: the same board, same antennas, same
room produces -42 dBm under stock. The deficit is **software**.

### ★ What stock does differently (measured, not inferred)

* **It uses channel 149, which OpenWrt refuses.** `iw phy1 info` reports
  `5745 MHz [149] (20.0 dBm) (no IR)` under `country BR` -- **no-IR = may not initiate
  radiation**, so `wifi reload` on ch149 leaves the radio down and the AP never starts.
  Stock ignores this and its own ACS actively prefers 149
  (`d-link channel[36+40+44+48] = 1400` vs `channel[149+153+157+161] = 800`).
* **It loads `PHY_REG_PG_8822Bmp_Type0`** -- PG **type 0**. rtw88 has `bb_pg` types 2/3/5
  only; there is no type 0 for 8822b.
* ★★★ **It transmits at a LOWER power index than we do and is louder.** Stock's live
  `/proc/wlan0/mib_rf`:

```
pwrlevel5GHT40_1S_A: ...2a2a2a2a 29292929 28282828...   (42, 41, 40)
pwrlevel5GHT40_1S_B: ...2d2d2d2d 2c2c2c2c 2b2b2b2b...   (45, 44, 43)
TXPowerOffset: 2     txpwr_reduction: 0
```

  These are **exactly the NOR MAC-partition values decoded earlier**. Stock runs index
  ~40-45 and reaches -42 dBm; rtw88 runs index **63** (its maximum) and reaches -59.
  ★ The two index scales are therefore **not the same quantity**, which is the measured
  proof behind retraction #41's warning: copying the NOR numbers into rtw88 lowers output.
* The board string confirms the front end: `MIPS: machine is 8197F(PA=0) 8812B(PA=0) 8367R`
  -- **PA=0, no external PA**, matching RFE type 10.

### ★ The honest limit of this A/B

Stock could not be pinned to ch36: a D-Link channel selector re-picks 149 about 9 s after
every interface restart, and it survives `acs_type=0` and `killall iwcontrol`. So the two
sides differ by channel, and **part of the 17 dB is regulatory** -- OpenWrt is capped at
17 dBm on ch36 while ch149 permits more. That cannot account for all of it (the 2.4 GHz
control is identical, and higher frequency costs slightly *more* path loss), but the split
between "regulatory band" and "driver deficit" is **not yet separated**. What is settled
beyond doubt is the part that mattered: **the hardware can do far better than rtw88 gets
out of it.**

★ Bench notes: `tiny` cannot scan UNII-3 under `country BR`; `sudo iw reg set US` makes
5745 visible (set it back afterwards). Stock has a **BusyBox shell on the serial console**
(unlike our OpenWrt, which has no getty), so `/proc/wlanN/*` can be read live. Stock also
boots as a **router with dnsmasq on 192.168.1.1** -- kill `dnsmasq`/`tinysvcmdns`/`locdns`
immediately after boot or it serves rogue DHCP on the house LAN.

### ★ Separating "band" from "driver" — ~14 dB is the driver, only ~3 dB is regulatory

Under `country BR` this radio may legally *initiate radiation* on **only** ch36/40/44/48,
all at **17 dBm**; every other 5 GHz channel is `no IR` (the DFS ones for lack of radar
support, 149-165 outright):

```
* 5180/5200/5220/5240 MHz [36/40/44/48]  (17.0 dBm)
* 5260..5700 MHz                          (20.0 dBm) (no IR, radar detection)
* 5745..5825 MHz [149..165]               (20.0 dBm) (no IR)
```

Stock runs ch149 at a 20 dBm ceiling; we run ch36 at 17 dBm. **That is at most a 3 dB
regulatory advantage** -- and 5745 costs ~1 dB *more* path loss than 5180 -- yet stock
measured **17 dB** stronger. ★★ So roughly **14 dB is a driver deficit, not a band or
regulatory one.** Unlocking ch149 (a compliance decision, not a technical one) would buy
only ~3 dB; it is not the fix.

### ★ Refuted: rtw88's `max_power_index` is NOT an artificial cap

The obvious follow-up: stock reaches -42 dBm at index ~40-45 in vendor units while rtw88
runs its own maximum 63 and reaches -59, and the earlier base sweep was linear to 63 with
no visible saturation -- so perhaps `max_power_index = 0x3f` caps us below the hardware.
A `txpwr_cap_override` was added to raise the clamp, and `tx_pwr_tbl` confirmed indices of
72/84/96/112/127 really were written.

A first sweep looked spectacular -- ours-minus-ref -2 dB at 63 rising to +8 dB at 84, with
a decline beyond, exactly the shape of an under-driven PA reaching compression. ★★★ **It
did not survive an interleaved A/B:**

| round | cap 63 | cap 84 | delta |
|---|---|---|---|
| 1 | -2 | +5 | **+7** |
| 2 | +4 | +4 | 0 |
| 3 | -10 | -9 | +1 |
| 4 | -1 | -2 | -1 |

**Mean delta +1.75 dB, spread -1..+7.** Round 3 shows the whole session drifting to -77/-76
with no configuration change at all. The apparent +10 dB was drift, and the single-direction
sweep was the wrong experiment. ★ **Always interleave A/B/A/B here; a monotonic-looking
sweep in one direction is worthless against this noise floor** (confound #28).

### ★★★ The deficit is NOT in the TX-power-index path — it is in RF/analog init

The vendor's TXAGC writer (`config_phydm_write_txagc_8822b()`,
`phydm/rtl8822b/phydm_hal_api8822b.c`) is **functionally identical to rtw88's**
`rtw8822b_set_tx_power_index_by_rate()`: same registers `offset_txagc[2] = {0x1d00,
0x1d80}`, same `rate_idx = HwRate & 0xfc`, same 4-byte packed write, **no extra clamp and
no extra scaling**.

★★★★ That is decisive, because it makes the two measurements contradictory *unless* the
difference lies elsewhere: if both drivers write the same register with the same meaning,
stock writing **40-45** should be QUIETER than rtw88 writing **63**. It is **17 dB
LOUDER**. Therefore the gap is **not in the power-index path at all** -- which also
explains why raising `max_power_index` did nothing (#45), why the index sweep was linear
but simply offset low, and why the RFE pins and RF mode LUT all read back correct.

★ **The remaining candidate is the RF (radio) initialisation itself**, and there is a
concrete structural difference:

| | vendor | rtw88 |
|---|---|---|
| RF init table | `halhwimg8822b_rf.c`, 15 333 lines, with **per-type variants** (`_Type3`, `_Type5`, `_Type17`, `RadioA_8822B`) | `rtw8822b_table.c`: **one** pair, `rtw8822b_rf_a[]` / `rtw8822b_rf_b[]`, no type variants |
| BB init table | `halhwimg8822b_bb.c`, 12 051 lines | (in the same 22 204-line table file) |

rtw88 loads a single RF table regardless of board type; the vendor selects a variant. If
this board's type needs different radio-side gain settings, rtw88 has no way to express
that -- and RF gain is exactly the layer the measurements now point at.

★★ **Next session starts here**: determine which RF variant the vendor selects for RFE
type 10 and diff it against rtw88's `rtw8822b_rf_a/b[]`. This is a table-porting job with
a clear acceptance test (the -42 dBm stock reaches on this unit), not another knob to
sweep. ★ And note the trap that caught two hypotheses already: PG/`txpwr_lmt`/power-index
changes are all offsets against a saturated base and **cannot** raise output (#43, #45) --
only the RF/analog path can.

### Where this leaves the 5 GHz signal — still unfixed, but the search space is much smaller

Everything rtw88 controls has now been measured rather than reasoned about, and all of
it is correct: maximum power index (and linear response proving it *is* the maximum),
correct eFEM registers, correct RF mode LUT, both paths live, best of the three
available RFE types, right channel and bandwidth. **The 5 GHz deficit is real,
TX-only, roughly 15-30 dB depending on the boot, and is not fixed.**

What remains is no longer a config change:

0. ★ **Nothing in rtw88 is left to try.** Every knob it exposes has now been *measured*,
   not reasoned about: power index (maxed, and proven linear so it really is the maximum),
   RFE type (all three, best one shipped), front-end and T/R switch registers (read back
   correct), RF mode LUT (correct), both RF chains (equal, neither dead), and the one
   intermittent init failure (skips nothing TX-critical). The two-way link budget puts the
   shortfall at ~13 dB against an RPi4. **A software fix in rtw88 is ruled out**, which is
   a result rather than a gap.

1. ~~**Port the board's true RFE type tables from the stock ODM driver.**~~
   ★ **REFUTED — see "The stock firmware, and what it settles" above.** The stock driver
   was obtained and read; its tables are offsets against a base that already saturates at
   maximum, so they can only reduce output. This was the previous leading explanation and
   it is now closed.
2. **A hardware fault in the transmit-only chain** (PA or T/R switch). Now the leading
   physical candidate, because reciprocity has excluded the antenna and feedline.
3. ★ **A stock-firmware A/B is still the one experiment that would settle whether this
   unit's 5 GHz was EVER better.** It remains an untested assumption that it was.

### Bench technique learned here

* **Module parameters survive a reboot via `/etc/modules.d/<pkg>`**, not
  `/etc/modprobe.d/` — OpenWrt's kmodloader ignores the latter. Append options to the
  module's own line: `rtw88_8822b rfe_option_override=3`. Verified working.
* **`iw reg set <cc>` re-applies TX power live.** `rtw_regd_notifier()` calls
  `rtw_phy_set_tx_power_level()`, so a power-affecting module parameter can be swept
  at runtime with no reboot and no module reload — which the `rmmod` warning above
  otherwise rules out.
* To re-run any of this, re-enable `RTW88_DEBUG`/`RTW88_DEBUGFS` with
  `config-y += RTW88_DEBUG RTW88_DEBUGFS` in `openwrt/package/kernel/mac80211/realtek.mk`,
  and re-add the two diagnostic module parameters (`rfe_option_override` in `rtw8822b.c`'s
  `read_efuse`, `txpwr_base_override` overriding `*base` in `rtw_get_tx_power_params()`).
  ★ All three were **reverted from the tree after this session** and deliberately never
  placed in `files/`: `openwrt/` is not scratch space — `cp -a files/. openwrt/` is
  add-only, so a patch left in `openwrt/` persists and one once shipped in a release
  (see `RETRACTIONS-AND-METHOD.md`). Rebuild with
  `make package/kernel/mac80211/{clean,compile}` then `make`; the box takes the image by
  `scp -O -l 4000` + `sysupgrade -n` (there is no `nohup` on the box — run `sysupgrade`
  in the foreground and let ssh drop).
