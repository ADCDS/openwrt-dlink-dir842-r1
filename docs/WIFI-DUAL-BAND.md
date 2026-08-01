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

**2.4 GHz requires the vendor SDK at build time** (`VENDOR_SDK=`, see
[`../README.md`](../README.md)). Without it the build still succeeds and you lose *only*
2.4 GHz — wired, switch, NAT, hardware offload and 5 GHz are unaffected
(`build.sh:58-77`).

`rtl8192cd` is **100 % real GPL-style source, no blobs** — 279 `.c` + 363 `.h`,
539 038 LoC of `.c`, `MODULE_LICENSE("GPL")`; the only binaries are chip firmware. It is
nevertheless **not redistributed here**: every file carries *"Copyright Realtek
Semiconductor Corporation. All rights reserved."* with no licence grant, and none of it is
in the pinned ggbruno base. See [`VENDOR-PARITY-INVENTORY.md`](VENDOR-PARITY-INVENTORY.md)
§1.

**Shipped defaults, as of the code in this tree — note they differ per band:**

| | SSID | encryption |
|---|---|---|
| 5 GHz `radio0` | `DIR842-OpenWrt` | **`none`**, key deleted — deliberately no baked credential (`files/target/linux/realtek/base-files/etc/uci-defaults/99-dir842-m5:181-189`) |
| 2.4 GHz `radio1` | `DIR842-2G` | `psk2`, placeholder key `ChangeMeNow123` (`files/…/uci-defaults/09_wireless-dualband-dir842:66-67`) |

The root README's "placeholder WiFi PSK" warning describes the 2.4 GHz seed. The 5 GHz
seed ships open on purpose: a PSK baked into a published image is a published credential.
Set both before this touches a real network.

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

| | 5 GHz (rtw88 / mac80211) | 2.4 GHz (vendor rtl8192cd) |
|---|---|---|
| is it there | `iw dev`, `/sys/class/ieee80211/phy*` | **`/proc/wlan0/mib_all`** (268 MIBs) |
| link / clients | `iw dev wlan1 station dump`, `iwinfo` | **`/proc/wlan0/sta_info`** |
| is it beaconing | `iw dev wlan1 info` | `/proc/wlan0/stats` — `beacon_ok` climbing at the TBTT rate (10/s at 100 TU) |
| AP daemon | `hostapd_cli`, `logread \| grep hostapd` | none — the 4-way handshake is in-kernel |
| config knobs | uci → hostapd conf | uci → `/etc/Wireless/RTL8192CD.dat`, or `iwpriv`/`iwconfig` |
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

## 9. Open items

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
   (`09_wireless-dualband-dir842:65`). The mac80211 side, independently, uses `country='BR'`
   (`99-dir842-m5:167`) — correct there, and *not* interchangeable.
5. ⚠ **Publish drift in the 2.4 GHz build path — found while writing this doc, not yet
   fixed.** Read from the files, **not** confirmed by running a build:
   - `g3-rtl8192cd-portflags.mk:105,109` still ship **`CONFIG_BAND_5G_ON_WLAN0=y`** and
     **`CONFIG_PHY_EAT_40MHZ=y`**, and the file has no `CONFIG_RTL_COMAPI_CFGFILE`. Those
     are precisely the three settings §4 identifies as wrong or missing. The live build
     tree's `…/rtl8192cd/Makefile:189,224,233` has `CONFIG_BAND_2G_ON_WLAN0=y`,
     `PHY_EAT_40MHZ` commented **off**, and `CONFIG_RTL_COMAPI_CFGFILE=y`.
   - Nothing in the live tree includes a `portflags.mk`, and no such file exists there —
     `build.sh:73` copies one in, but the corrected flags live in the driver's `Makefile`.
   - `g3-rtl8192cd-4.14-port.patch` is diffed against **`package/kernel/rtl8192cd/…`** (the
     8devices layout it was developed on), while `build.sh:66-72` unpacks the SDK to
     **`target/linux/realtek/files-4.14/drivers/net/wireless/rtl8192cd/`** and applies it
     with `-p1`. Those paths do not meet. (`g4-rtl-headers-4.14-port.patch` *is* diffed
     against the destination layout and does line up.)

   Net: a `VENDOR_SDK=` build from this repo, as published, is unlikely to reproduce the
   working 2.4 GHz driver. Reconcile against the live tree before publishing.
6. ⚠ **uci-defaults ordering hazard — reasoned from the code, never observed failing.**
   `/etc/init.d/boot:10-16` runs `/etc/uci-defaults/*` in `ls` order, so `09_` → `10_` →
   `99-`. `99-dir842-m5:158` starts with **`delete wireless`**, which on a genuinely virgin
   overlay would wipe the `radio1` stanza `09_` had just seeded — and `09_` self-deletes on
   success, so it would not re-seed. The boot that verified dual-band from a factory flash
   notes *"/etc/uci-defaults was empty after the first boot"*, which implies `radio0`
   already existed when `10_` ran, i.e. that boot was not virgin-overlay. **Unverified in
   either direction.** Worth a deliberate wipe-and-boot test, or moving the 5 GHz seed out
   of the `delete wireless` block.
