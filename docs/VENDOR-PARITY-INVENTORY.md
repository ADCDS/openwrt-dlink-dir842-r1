# Vendor firmware inventory & parity checklist — DIR-842 R1

> ℹ️ **REFERENCE INVENTORY, partly superseded.** The file-level facts here are still
> the best record of what the stock firmware contains. Some *conclusions* have since
> been overturned — notably §161's "stock does 600–800 Mbit is NOT evidenced": stock
> was later measured at **913/923 Mbit**, and this port now reaches **891/896**. See
> [`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) before trusting a
> conclusion in this file.

Source of truth for the R1–R6 parity work. Everything here is **read from files on
disk**, not inferred, unless marked *(inferred)*.

Stock identity — `stock-rootfs/VERSION`:
`NAME: DIR_842E_RT8197F  VERSION: 3.0.3  VENDOR: D-Link Russia`, kernel `3.10.90+`,
`SoC RTL8197FS-VE4`, `8197F(PA=0) 8812B(PA=0) 8367R NOR RAM=64`.

Stock is **not** OpenWrt/UCI: it is D-Link's `anweb` (civetweb + AngularJS) UI over a
monolithic `dcfg`/`resident` config daemon plus `libdhal.so`. `/etc/init.d/` holds only
`rcS` → `exec /sbin/myinit`; every service lives inside `dcfg`, so there are no per-service
init scripts to read. The authoritative feature list is the generated build config
`stock-rootfs/srv/anweb/autoconf.js`.

---

## ★ Three findings that change the plan

### 1. `rtl8192cd` is 100% real GPL source — no blobs
`sdk-rtl819x/target/linux/realtek/files/drivers/net/wireless/rtl8192cd/`:
**279 `.c` + 363 `.h`, 0 `.o`, 0 `.a`**, `MODULE_LICENSE("GPL")`. **539 038 LoC** of `.c`
(707 612 with headers). Only binaries are chip firmware — `WlanHAL/Data/8197F/rtl8197Ffw.bin`
(35 KB, the 2.4 GHz MAC firmware we would need) and the 8822B equivalents. Per-chip HALs
exist for **RTL8197F** and RTL8822B. So R4 is *feasible* — large, but not blob-blocked.

Interface is the problem, not availability: **WEXT + 114 private ioctls**
(`iw_handler_def` at `8192cd_ioctl.c:1082`), config via an in-kernel parse of
`/etc/Wireless/RTL8192CD.dat`, and **no `struct ieee80211_ops` anywhere**. AP/STA state
machine, PSK/4-way, WPS, DFS, rate control, beamforming all live *in* the driver — it
replaces mac80211 + hostapd's crypto layer. `8192cd_cfg80211.c` exists but is an
experimental add-on shim (`#define RTK_NL80211`), not a mac80211 driver.
Targets kernel **3.10.49**; highest version guard in the tree is `KERNEL_VERSION(4,1,0)` —
**no 4.14 awareness**. Needs out-of-tree glue: `asm/rtl865x/{platform,rtl_glue}.h`,
`net/rtl/rtl_types.h`, `net/rtl/features/fast_bridge.h`, a preallocated SKB pool
(`SKB_BUF_SIZE=3040 MAX_SKB_NUM=400` for 8197F), and MIPS16 disabled for its objects.

### 2. Stock drives BOTH radios with `rtl8192cd` — including the RTL8822BE
Stock bootlog: `Realtek WLAN driver - version 3.8.0(2017-12-26)` →
`rtl8192cd_init_one … vendor_deivce_id(b82210ec) … found 8822B !!! Hardware type = RTL8822BE`.
`lib/modules/modules.builtin` contains exactly one wireless entry:
`kernel/drivers/net/wireless/rtl8192cd/rtl8192cd.ko` (built-in). **Stock never uses
rtw88/mac80211.** Kconfig default is `BAND_5G_ON_WLAN0`, matching stock's
`wifi: wlan0` (5 G) / `wifi_2G: wlan1`.

Consequence for R4: a single `rtl8192cd` port would cover *both* bands, but it would
also *replace* our working rtw88 5 GHz path — so the mac80211-first strategy (G2) stays
the right call, with rtl8192cd (G3) scoped to the 2.4 GHz radio only.

### 3. ★ All RF calibration AND the per-unit MAC live in **mtd1 "MAC"** (flash 0x20000, 64 K)
Realtek's `hw_setting` partition. Confirmed three ways: `PARTITIONS.md`; SDK
`drivers/mtd/maps/rtl819x_flash.c:60,399,405` (`HW_SETTING_OFFSET`, `EXPORT_SYMBOL`,
`/proc/flash/hwpart`); and `stock-rootfs/lib/libhwdata.so` containing the literal
`/dev/mtd1`. The driver reads it directly at
`8192cd_cfg80211.c:295` — `HW_SETTING_OFFSET + sizeof(param_header) + HW_WLAN_SETTING_OFFSET(13) + …`.

Stock's load sequence (bootlog:196-218): `Initializing /dev/mtd1 RLX...` →
`Intialize wifi calibration` → `rlx_wifi_mibs[818]: Some calibrations are not valid,
rewriting...!` → `Set MIB from /etc/Wireless/RTL8192CD.dat`. Two userspace libs do it:
`librlx_wifi_mibs.so` (validates, substitutes computed averages, pushes via
`iwpriv_set_mib`) and `libhwdata.so` (D-Link per-unit factory fields, `bmac_inc` = the
"LAN = WAN + 1" rule).

**Decoded mtd1 layout (empirical, from `mtd1-MAC.bin`)** — two overlaid schemas, D-Link
ASCII fields on top of the Realtek binary region:

| offset | size | field | value on this unit |
|---|---|---|---|
| `0x000` | 18 | **base MAC (ASCII)** = WAN; LAN = +1 | `e0:1c:fc:51:c9:ee` |
| `0x022` | 2 | hw revision | `R1` |
| `0x03c` / `0x03f` | 2 | country / region | `BR` / `US` |
| `0x051` | | serial | `TK19113007443` |
| `0x0aca` | 8 | **WPS PIN** | `92167728` |
| `0x013`–`0x042` | 8×6 | Realtek `wlan[].macAddr` defaults | `00:e0:4c:81:96:c1…c8` |
| `0x0d8` | 14 | `pwrlevelCCK_A[14]` | 2.4 G |
| `0x0e6` | 14 | `pwrlevelCCK_B[14]` | 2.4 G |
| `0x0f4` | 14 | `pwrlevelHT40_1S_A[14]` | 2.4 G |
| `0x102` | 14 | `pwrlevelHT40_1S_B[14]` | 2.4 G |
| `0x11e` / `0x12c` | 14 | `pwrdiffHT20` / `pwrdiffOFDM` | |
| `0x13a`+ | 1 ea | `regDomain, rfType, ledType, xCap, TSSI1, TSSI2, Ther, trswitch` | `…,0x27,0x31,0x23,0x18` |
| `0x171` | **196** | `pwrlevel5GHT40_1S_A[196]` | descending `2a→23` |
| `0x235` | **196** | `pwrlevel5GHT40_1S_B[196]` | descending |
| `0x2f9` / `0x3bd` | 196 | `pwrdiff5GHT40_2S` / `pwrdiff5GHT20` | |
| `0x600`,`0x6cd` | | 802.11ac nibble-packed diff arrays, path A + path B | |

Offsets self-check: `0x171+196=0x235`, `0x235+196=0x2f9` ⇒ `MAX_5G_CHANNEL_NUM_MIB=196`,
`MAX_2G_CHANNEL_NUM_MIB=14` (`apmib.h:1851-1855`).

**Why the 8822BE efuse is blank:** this board deliberately keeps 8822B RF cal *off-chip*
in mtd1. (Stock does read two efuse bytes: `0x3D7=0xf3, 0x3D8=0xf0`.)

---

## Two cheap wins available immediately (fold into R3)

1. **Per-unit MAC.** The port currently ships the Realtek *defaults*
   (`00:e0:4c:81:96:c2` / `c3`, hardcoded in `rtl865x_asichal.c` **and** re-asserted by
   `uci-defaults/99-dir842-m5`), while the real per-unit MAC is the ASCII string at
   mtd1+0x00 (`e0:1c:fc:51:c9:ee`, LAN = +1). `board.d/02_network` already reads it
   correctly via `mtd_get_mac_binary MAC 19` — and then the uci-default overwrites it.
   Fix = derive the ASIC netif MACs from flash instead of the constants, then drop the
   uci-default override.
2. **★ RFE type mismatch.** Stock prints **`RFE TYPE = 0`** for the 8822B (bootlog:148)
   and `[97F] RFE type 0 PHY parameters: DEFAULT` for the 8197F; in the SDK `rfe_type`
   comes from **Kconfig**, not efuse, so the blank efuse is irrelevant to it. Our patch
   `package/kernel/mac80211/patches/realtek/03-rtw8822b-blank-efuse-rfe.patch` forces
   **RFE 2**. Concrete, testable one-line discrepancy affecting TX power / PA-LNA routing.
   Also: the 5 GHz TX-power tables at mtd1 `0x171`/`0x235` can be fed to rtw88 to lift the
   blank-efuse power ceiling.

---

## Feature parity checklist (from `autoconf.js` + shipped binaries)

**Present in stock** — and the OpenWrt equivalent for R5:

| stock feature | evidence | OpenWrt path |
|---|---|---|
| 2.4 G + 5 G concurrent | `wifi:wlan0`(5G) `wifi_2G:wlan1`, both `rtl8192cd` | **R4** |
| MBSSID / guest SSID | `WIFI_MBSSID`, `WIFI_GUEST_ACCESS`, `wlan0-va0…va3` | hostapd multi-BSS |
| WPS | `/bin/wscd`, `simplecfgservice.xml` | `wpad` + button |
| WMM/EDCA, per-client WiFi shaping, 802.11k/v roaming | `WIFI_WMM`, `WIFI_CLIENT_SHAPING`, `/usr/bin/roamd`, `/bin/iapp` | hostapd WMM; roaming = niche |
| per-port bandwidth limit | `BANDWIDTH_CONTROL` (switch rate limiting, not tc) | needs rtl8367b support |
| WiFi MAC filter / client kick | `WIFI_MAC_FILTER` | hostapd |
| AP/repeater/WISP/client modes, WDS×4 | `MODE_*`, `Wds1..4*` | wpa-supplicant/relayd |
| port forwarding, DMZ, NAT loopback | `VIRTUAL_SERVERS`, `DMZ`, `NAT_LOOPBACK` | fw3/fw4 |
| UPnP IGD | `/sbin/miniupnpd` | miniupnpd |
| DDNS (15 providers) | `/usr/sbin/inadyn` | ddns-scripts |
| DHCPv4/v6 + DNS relay, static leases | `dnsmasq`, `dhcp6_pd`, `/etc/ethers` | dnsmasq + odhcpd |
| static routes + **RIP** | `/usr/sbin/ripd` + `zebra` (Quagga) | quagga/frr-ripd |
| 802.1Q VLAN + port segmentation | `VLAN`, `TRAFFIC_SEGMENTATION` | swconfig (have it) |
| firewall / IP-MAC-URL filter | `iptables`, `ebtables`, `xtables-multi` | fw3 (have) + filters |
| parental control (DNS) | `YANDEX_DNS`, `/usr/sbin/locdns` | any DNS filter |
| DoS protection / conn limits | `DOS_FILTER` | iptables limits |
| **IPv6**: IPoE v6 dyn/static, PPPoE v6, MLD, DHCPv6-PD, ULA `fd01::1/64` | `odhcp6c`, `ip6tables` | odhcpd + odhcp6c (**shipped but dead — `ipv6.disable=1`**) |
| WAN: IPoE, PPPoE(+dual/v6), PPTP, L2TP, 802.1X | `pppd`, `pptp`, `xl2tpd`, `wpa_supplicant` | ppp (have), +pptp/xl2tpd |
| WAN failover | `/sbin/wan_failover`, `link_watcher` | mwan3 |
| **IPsec VPN** | `/usr/bin/racoon` + 20 xfrm/esp `.ko` | strongswan (big) |
| IGMP snoop/proxy, IPTV, udpxy | `/bin/igmpx`, `improxy`, `udpxy` | igmpproxy, udpxy |
| SIP/RTSP ALG, PPPoE pass-through | `nf_conntrack_sip/rtsp` | kmod-nf-nathelper-extra |
| firmware upgrade UI (local+auto) | `/sbin/fw_updater` | LuCI + sysupgrade |
| config backup/restore, factory reset | `/usr/sbin/nvram`, `nvramd` | LuCI/sysupgrade |
| syslog (+remote), NTP, ping/traceroute | `syslogd`, `ntpd`, `iperf3` on box | have/trivial |
| HTTPS mgmt | `anweb` + `server.pem` | uhttpd + px5g (**R5**) |
| **TR-069/CWMP** | `/sbin/tr069`, `tr069.xml` (57 K), on by default | no good equivalent — skip |
| LED on/off control | `gpiom.ko` (profile `DIR_842C_RT8197F`), `led_test` | **R3** (we have ZERO LEDs) |
| buttons reset+WPS | `gpiom.ko`, `button_test` | **R3** (we have reset only) |
| scheduler + auto-reboot | `/sbin/autoreboot`, `admin.spool` | cron |
| mDNS/NetBIOS/LLMNR, captive-portal bypass | `tinysvcmdns`, `locdns` | umdns |
| 4 languages, setup wizards, statistics | `srv/anweb/…` | LuCI |
| **hardware NAT** | `init Realtek HW_NAT`, `Realtek FastPath:v1.03` | our rtl865x offload |

**Confirmed ABSENT in stock** (⇒ non-goals): **USB / storage / DLNA / SMB / FTP** — the
board has *no USB at all* (`grep -ci usb` on the stock bootlog = **0**; zero `*USB*` flags
in `autoconf.js`; the `Device.USB.*` UI perms are dead framework code shared with DIR-8xx
models that do have USB). Also absent: **mydlink/cloud** (remote mgmt is TR-069), **SSH**
(telnet only), and **6rd / DS-Lite / 6in4 / 6to4**.

---

## ★ R6 correction: "stock does 600–800 Mbit" is NOT evidenced

Grepped every doc on this machine: **no stock iperf3/wget measurement exists.** What
exists is an *acceptance target* for the port — `RE-notes.md:60`, `HANDOFF-M6.md:205`:
*"Verify: iperf3 WAN↔LAN NAT ~940 Mbit/s with CPU idle"* — inferred from gigabit PHYs +
the presence of ASIC L3/NAPT tables, never measured on stock. `HANDOFF-M6.md:215` records
that WAN throughput was never testable (no second host on jack 4).

What *is* confirmed: stock runs a real hardware L3/L4 gateway — `STOCK-TABLES.md` is a
live `/proc/rtl865x/*` dump showing both netifs `RoutingEnabled` with per-netif ingress
ACL ranges (LAN 0-3, WAN 4-6 — the layout R1's ACL fix already adopted), a 1024-entry
32 B NAPT table, `[0] 192.168.1.0/24 process(ARP)`, `[7] 0.0.0.0/0 process(NxtHop)`, and
`SWTCR1 = 0x2200` (both `L4EnHash1` *and* `EnL4WayH`) — reverse-direction hardware lookup
only makes sense with 4-way hashing on. ⚠ That capture had **no WAN cable**, so its ARP /
NAPT / extIP tables were empty.

⇒ **The single highest-value R6 experiment** (already named in
`M7-HWNAT-REVERSE-NAPT.md`): RAM-boot **stock** with a real WAN host attached, run a
sustained download, and dump `/proc/rtl865x/{napt,acl,netif}` + `MSCR`/`SWTCR0`/`SWTCR1`/
`DACLRCR`. That settles both (a) stock's actual throughput and (b) **whether stock's
WAN→LAN reverse-NAPT is hardware at all, or the same CPU trap we currently have.** Until
that is done, "downloads must be hardware" is an assumption, not a known-good target.

---

# R4 / G0 — 2.4 GHz driver audit: VERDICT

**G0 is complete.** Findings above, distilled into a go/no-go:

| question | answer |
|---|---|
| Source or blobs? | **Real GPL C source.** 279 `.c` + 363 `.h`, **0 `.o`, 0 `.a`**. Only binaries are chip firmware (`rtl8197Ffw.bin`, 35 KB). |
| 8197F supported? | **Yes** — `WlanHAL/RTL8197F/` HAL, 107 files reference 8197F, `Kconfig` has `WLAN_HAL_8197F` with an RFE-type choice. |
| cfg80211/mac80211? | **No.** WEXT + **114 private ioctls**; `iw_handler_def` at `8192cd_ioctl.c:1082`. **No `struct ieee80211_ops` anywhere.** `8192cd_cfg80211.c` is an experimental `RTK_NL80211` shim, not a driver. |
| Size | **539 038 LoC** of `.c` (707 612 with headers). Largest: `8192cd_sme.c` 31 k, `8192cd_hw.c` 28 k. |
| Kernel era | Targets **3.10.49**; highest guard in-tree is `KERNEL_VERSION(4,1,0)`. **No 4.14 awareness.** |
| Out-of-tree glue needed | `asm/rtl865x/{platform,rtl_glue}.h`, `net/rtl/rtl_types.h`, `net/rtl/features/fast_bridge.h`, a preallocated SKB pool (`SKB_BUF_SIZE=3040 MAX_SKB_NUM=400`), MIPS16 disabled for its objects. |
| Config surface | In-kernel parse of `/etc/Wireless/RTL8192CD.dat` + `iwpriv`. Replaces mac80211 **and** hostapd's crypto/state machine (`8192cd_psk.c`, `_sme.c`, `_dfs.c`, `Beamforming.c`). |

### Consequence for the chosen strategy
The user's call — *mac80211 first, vendor stack as fallback* — is the right one, and G0
sharpens why: adopting `rtl8192cd` wholesale would also **replace the working rtw88 5 GHz
path**, since stock drives *both* radios with it (`modules.builtin` has exactly one
wireless entry). So G3 must be scoped to the **2.4 GHz radio only**, or it regresses 5 GHz.

### G1 blocker identified (next concrete step)
The SoC's integrated 2.4 GHz MAC/PHY has **no DTS node at all** and mainline has no driver
for an on-SoC Realtek WMAC (`package/kernel/mac80211/realtek.mk` covers only discrete
PCIe/USB rtlwifi/rtw88 parts). So G2 means *authoring* a driver, using
`WlanHAL/RTL8197F/` + `phydm/rtl8197f/` register tables and `rtl8197Ffw.bin` as the
hardware reference — the same "vendor code as documentation, mainline structure as the
destination" method that cracked the PCIe link training and the ethernet carve.
Realistic scale: comparable to the M4 ethernet carve (which was a multi-session milestone),
not a single sitting.

---

# R3 — LED GPIO map: the one item that needs a human check

Everything else in R3 is determinable from files, but the **LED GPIO numbers are not
safely recoverable**:
- `gpiom.ko`'s profile table (`DIR_842C_RT8197F`) keeps them in `.data` behind
  relocations, in the **vendor's own GPIO numbering**, not Linux gpiochip numbers.
- That same bank carries the **switch SMI lines (GPIO 498 SDA / 499 SCK)** and the
  **8367S reset (474)**. A wrong guess writes to the switch control lines and kills the
  datapath — and an LED cannot be verified without eyes on the unit.

So: the LED **infrastructure is now enabled** (`CONFIG_LEDS_GPIO=y` + `kmod-leds-gpio` in
`DEVICE_PACKAGES`), which is what was actually missing — the trigger infra
(`LEDS_TRIGGER_NETDEV/TIMER`, `SWCONFIG_LEDS`, `LED_TRIGGER_PHY`) was already on with no
provider. Adding LEDs is then a few lines of `gpio-leds` DT + `board.d/01_leds`. The
existing `01_leds` entries must also be **deleted**: they reference
`rtl8192cd:green:wifi0/wifi1` — vendor-blob sysfs names that do not exist in this port,
and a `wlan1` that does not exist either (single radio).

---

# R6 — one hypothesis eliminated from the SDK (negative result)

**extIP `nextHop` is NOT the reverse-egress selector.** The open question was whether our
`ext.nextHop = 0` was causing a reverse-NAT'd reply to egress via the WAN nexthop chain.
Answer: no — the vendor **always** uses 0. `l3Driver/rtl865x_ip.c` hardcodes
`asicIp.nhIndex = 0;` on the masquerade path, `AsicDriver/rtl865x_asicL3.c:33` copies it
straight into `entry.nextHop`, and a tree-wide grep for `nhIndex` finds **no caller that
ever sets it nonzero** (every other hit is a `proc_debug` print or the getter at
`asicL3.c:64`). So our value is vendor-correct and this hypothesis is dead.

R6's live lead is therefore the **`SWTCR0` bits[4:3] `WANRouteMode` Forward(0) → ToCpu(1)**
retest, which must be run with the bench-drift guards in place (the earlier negative was
confounded — tiny had lost `172.16.0.2` *and* the host route had reverted to the house
gateway, and both read as "100% packet loss").

Falsified so far, for the record: (1) extIP `/32` route with `process=2` → reply
black-holed, proving the extIP-table match does **not** pre-empt the L3 route lookup;
(2) dst-MAC→TOCPU ACL rule → works but blanket-traps every WAN frame (software-speed
downloads, wedges under load); (3) extIP `nextHop` semantics → not a factor (this entry).

## R4 — the 2.4 GHz radio: where it actually stands (2026-07-30)

### G1 + G2 done and hardware-verified
The SoC's integrated 2.4 GHz WMAC now has a DTS node and a driver that binds:

    chip id 8197f001, reg [mem 0x18640000-0x1864ffff], irq 6, xtal 25MHz
    WLAN_EN (SR+0x64): 00000000 -> 0000001f
    WMAC regs +0x000=51c0b2f3 +0x004=14020012 +0x008=00003023
    WMAC responds.

`WLAN_EN` reading **zero** before the write is the key result: the bootloader never
sets it, so that undocumented gate is load-bearing — the direct analogue of the PCIe
`SR+0x100` release that cracked M5. Ethernet is unaffected (0% loss all paths).

### The table route was investigated and then RULED OUT on hardware
The vendor's init tables are conditional. Decoding the interpreter gave a flattened
board-specific sequence (144 MAC + 464 BB + 196 AGC + 144 RF writes, files under
`dir842-build/ke/wmac_*_flat.txt`, regenerable with `flatten_8197f.py`). It depended
on two values only the chip can answer, so the driver prints them at probe:

    bonding strap (SR+0x0c)[3:0]    = 10  -> 97FS -> package_type 1   ✓ predicted
    cut version   (WMAC+0xf0)[15:12] = 1  -> ✗ NOT as assumed

★ The vendor gates the whole header-table path on `(cut >= 2)` (0x801f91d4). This
chip is **cut 1**, so stock never applies those tables here — it runs a legacy path
(0x80252750 / 0x80253c90 / 0x80252784). Replaying the flattened tables would have
programmed a register set the vendor does not use on this silicon, and would have
looked entirely plausible while doing it.

### Consequence: G3 (port the vendor driver) now beats G2 (hand-write)
Source IS obtainable — the earlier "no source on this machine" was about this
machine, not the world. Verified GPL publications, newest driver first:

| source | driver vintage | kernel | why it matters |
|---|---|---|---|
| Cudy GP3000 `jameywine/GPL-for-GP3000` | 4.0.8 (`8192cd_hw.c` 1,150,948 B) | **5.10** | newest; full `WlanHAL/RTL88XX/RTL8197F` (verified 20 files), `Data/8197F` + `rtl8197Ffw.bin`, `8192cd_cfg80211.c`, and `wnic_skb_pool` factored out as a standalone module |
| Mercusys MT110 `skraizenn/mercusys-mt110` | 4.0.8 | 5.4 | same driver, already packaged as an OpenWrt `KernelPackage` — best packaging template |
| **8devices `v3.4.11e/openwrt-18.06-rtkmipsel-3.18`** | 1,008,120 B | 3.18 | **right target glue**: `ARCH:=mipsel`, `SUBTARGETS:=rtl8197f` — the SoC-side platform code |
| **`vladisslav2011/openwrt-AC10`** | 981,115 B | 3.18 | ★ **the G4 precedent**: ships `CONFIG_SLOT_0_8822BE=y` next to `CONFIG_SOC_RFE_TYPE_0/5`, `CONFIG_SOC_EXT_PA`, `CONFIG_SOC_EXT_LNA` — an 8197F on-SoC radio running ALONGSIDE an RTL8822BE PCIe card, i.e. our exact pairing |

Two things this changes:
1. The port is **bracketed** (3.18 below our 4.14, 5.10 above), so every kernel-API
   break between them has already been solved by the vendor — 5.10 is an answer key.
2. **G4 has a published precedent.** vladisslav2011/openwrt-AC10 is a shipped config
   driving exactly this hardware combination, which turns "is concurrent dual-band
   even possible on this SoC" from an open question into a configuration problem.

### Dead ends, so nobody repeats them
- **D-Link never published DIR-842 R1 source.** All 7,901 keys in their public GPL
  bucket were enumerated; only MediaTek revisions (A1/B1/C1/C3) exist. The one
  Realtek entry is an RTL8196C. Do not download the 600 MB A1/B1 tarballs.
- **No one has 8197F on-SoC 2.4 GHz working on modern OpenWrt** (forum t/70975, read
  first-hand). The SoC port to 5.10 is done; the wireless driver is the blocker.
- Worth weighing: that thread claims the 8197F WMAC "can be ported to rtw88, as they
  use the same structs". Unverified, but rtw88 already binds the 8822BE on this box,
  so it is a cheap hypothesis to test before committing to a 1 MB driver forward-port.

### Still open
G3 (port), G4 (both radios up). Also unresolved from the table work: the IQK/LCK/DPK
trigger sites (indirect calls, no static call site) and which TXAGC register the
per-unit `pwrlevel*` MIBs land in.

### R4/G2 — the WMAC datapath, mapped (what a driver would have to implement)

Decoded from the stock driver and cross-checked by two independent passes. This is
the raw material for either a mac80211 driver (G2) or for validating a vendor-driver
port (G3). Full dumps: `dir842-build/ke/{txdesc_layout,tx_path_decoded,beacon_decoded,ringbase_decoded}.txt`, `tx_asm/`.

**Model:** the on-SoC WMAC uses its own descriptor DMA into DRAM — NOT the SoC
`swNic`/mbuf infrastructure the ethernet switch core uses. The right mainline
template is `rtl8192ee` / `rtw88-rtl8822be`, not `rtl8192ce`.

- Ring bases are written by `PrepareTXBD88XX` @0x8037aafc (TX) and
  `PrepareRXBD88XX` @0x80379fa8 (RX), both called from `open()` — **not** from
  `init_hw_PCI` or `InitHCIDMAReg88XX` (which writes only the NUM block). They build
  the offset table on the stack, which is why grepping for immediate offsets finds
  nothing. RX_DESA = **0x0338**; ⚠ the vendor's own `proc_desc_info` prints label
  "RDSAR:" then reads 0x0340 — a vendor copy/paste bug, do not trust that dump.
- Ring index → queue is proven via `SetTxDescQSel88XX_V1`, not inferred. BCNQ =
  0x308, 5 slots, no index register. `0x0382` bit12 = beacon kick, bit13 = BD engine
  enable. `0x0304 = 0x00160000` (INT_MIG).
- Interrupts: HIMR = 0x06100C03, HIMRE = 0x0F00.
- Beacon is armed/disarmed via the BD OWN bit.
- Sequence numbers are software in BOTH the 802.11 header and TXDESC w9[23:12]
  SW_SEQ; `EN_HWSEQ` (w8[15]) is never set.

**Deltas from rtw88 — the actual porting work.** Register offsets, doorbell, beacon
OWN bit and every overlapping TXDESC bit position are byte-identical to rtw88. What
differs when lifting its `pci.c`:

| item | this WMAC | rtw88 |
|---|---|---|
| `tx_buf_desc_sz` | 32 (4 segments, all in psb_len) | 16 (2 segments) |
| `tx_pkt_desc_sz` | **40 (see open question)** | 48 (8822b) |
| `psb_len` unit | 256 bytes | 128 bytes |
| TXDESC placement | separate 64-byte-stride array | `skb_push`ed onto the frame |
| memory | cached KSEG0 + explicit `dma_cache_wback` | DMA API |

Porting notes: replace the cache handling with `dma_alloc_coherent` + `dma_sync_*`;
rtw88 has no macros for MACID, NAVUSEHDR, the w4 retry block or MBSSID w6[15:12];
LS / PKT_OFFSET / EN_HWSEQ / SW_DEFINE are never set.

**★ One load-bearing open question:** the ACTIVE TXDESC size on the chipver-23
branch — 40 vs 48 bytes. The stride is 64 either way, but TXBD dword0[15:0] tells the
hardware how many descriptor bytes to fetch, so a wrong value means nothing
transmits. The two decode passes disagreed and the surrounding code reuses a scratch
register, so static analysis cannot settle it. **Discriminating test: read back TXBD
dword0[15:0] after ring init.** Make that the first thing the ring milestone checks.

Chip identity is now settled on silicon (all read at probe): bonding strap 10 →
package_type 1, cut 1 (→ header tables NOT used), HAL type id 14 → **chipver 23** —
which is exactly the branch the TXDESC question lives on.

### R4/G3 — source acquired and the port surface MEASURED (2026-07-30)

Fetched and verified the 8devices `v3.4.11e/openwrt-18.06-rtkmipsel-3.18` driver
(sparse clone of `package/kernel/rtl8192cd`, 36 MB):

    WlanHAL/RTL88XX/RTL8197F/ + RTL8197FE/      present
    WlanHAL/Data/8197F/rtl8197Ffw.bin           61,648 B  (matches the expected fingerprint)
    efuse_97f/                                  present
    8192cd_hw.c                                 1,008,120 B (exact expected size)
    prebuilt .o/.ko/.a                          0     <- full source, GPL-clean
    scale                                       290 .c + 374 .h, 792,378 LoC

★ Its build config is literally G4:

    CONFIG_SOC_WIFI=y            CONFIG_SOC_RFE_TYPE_0=y
    CONFIG_SLOT_0_8822BE=y       CONFIG_SLOT_0_RFE_TYPE_10=y

i.e. the on-SoC 2.4 GHz radio and a PCIe RTL8822BE driven together by one driver —
our exact hardware pairing. And `SOC_RFE_TYPE_0` independently corroborates the
`rfe_type = 0` decoded from *our* stock binary for the on-SoC radio. Two unrelated
sources agreeing on that value is meaningful.

**The 3.18 → 4.14 port surface, measured rather than estimated.** Scanned the tree
for APIs that changed between those kernels, then checked each against the actual
4.14.187 headers in this build:

| API | in 4.14? | files | verdict |
|---|---|---|---|
| `init_timer` | present | 14 | no work (removed in 4.15, not 4.14) |
| `setup_timer` | present | 6 | no work |
| `PDE_DATA` | present | 1 | no work |
| `trans_start` | **absent** | 2 | real break (removed 4.7 → `netif_trans_update`) |
| `create_proc_entry` | **absent** | 3 | real break (likely dead `#ifdef` legs — check first) |
| `strnicmp` | **absent** | 2 | real break (→ `strncasecmp`) |
| `get_ds` | **absent** | 1 | real break |
| `asm/rtl865x/*` | vendor | 5 | supply or stub |
| `net/rtl/*` | vendor | 3 | supply or stub |

**So the kernel-facing break surface is ~8 files of API fixes plus ~8 files needing
vendor headers — out of 664 source files.** That is the opposite of the "port a
792k-LoC driver" framing: this driver carries its own OS abstraction layer, so almost
none of its bulk touches kernel API. The 21 files flagged for timer APIs need nothing
at all, because those were removed in 4.15 and this target is 4.14.

This does NOT make G3 free. The remaining risk is exactly where it always was and is
not measurable by grep: **SoC WMAC bring-up and per-board RF configuration** (RFE
type, external PA/LNA, per-unit calibration out of the MAC partition), plus getting a
WEXT-era driver and mac80211's rtw88 to coexist for G4. But the kernel-API objection
that made G3 look prohibitive does not survive contact with the numbers.

Reproduce the fetch:

    git clone --depth 1 --filter=blob:none --sparse \
      -b v3.4.11e/openwrt-18.06-rtkmipsel-3.18 \
      https://github.com/8devices/openwrt-8devices.git
    cd openwrt-8devices && git sparse-checkout set package/kernel/rtl8192cd

### R4/G3 — the driver now COMPILES (partially); blocked on one characterised issue

Real progress past "source staged". The vendor headers turned out to be a non-problem,
and the driver builds objects.

**`asm/rtl865x/*` is NOT needed — all five references are dead for this config:**

| site | guard | verdict |
|---|---|---|
| `8192cd_osdep.c:158` | `!CONFIG_NET_PCI && CONFIG_RTL8196B` | dead (we are 8197F, PCI_HCI=y) |
| `8192cd_osdep.c:163` | `!CONFIG_NET_PCI && CONFIG_RTL8196C` | dead |
| `8192cd_util.h:34` | inside the `#else` of `#if defined(__LINUX_2_6__)` | dead — we take the `__LINUX_2_6__` leg |
| `8192cd_hw.c:96` | same `#else`; the live leg is `#include <bspchip.h>` for `CONFIG_RTL_8197F` | dead |
| `romeperf.c:18` | commented out | dead |

Same pattern as `create_proc_entry`. What IS required is `bspchip.h` plus the `net/rtl`
headers, and both exist: `bspchip.h` in the 8devices target tree at
`arch/mips/include/asm/mach-rtl8197f/`, and 28 `net/rtl` headers (`rtl_types.h`,
`rtl_glue.h`, `features/fast_bridge.h`, …). All staged into `files-4.14/`.

**Result: the driver compiles.** With `CONFIG_RTL8192CD=m` it builds phydm objects
(`phydm.o`, `phydm_dig.o`, `phydm_edcaturbocheck.o`, …) against kernel 4.14.187.

**One blocker, precisely characterised.** A circular-include tangle around
`wlan_amsdullcsnaphdr_t`:

- the struct lives in `wifi.h`, originally gated on `SUPPORT_TX_AMSDU_SHORTCUT`
- that macro is defined in `8192cd_cfg.h:1125`
- but `8192cd_cfg.h` **includes `wifi.h`**, so on the inner pass the macro is not yet
  defined, `wifi.h`'s `_WIFI_H_` guard latches, and the struct is skipped for good
- the *use* in `8192cd.h` is reached later with the macro defined, so it compiles →
  `field 'amsdullcsnaphdr' has incomplete type`
- main driver units survive by pulling `wifi.h` in first through another path; the
  `phydm/` units include only `8192cd.h` and fail

Making the struct unconditional (kept — it is correct and costs nothing) is not
sufficient on its own, and simply adding `#include "wifi.h"` to `8192cd.h` makes it
worse: it hoists `wifi.h` ahead of the headers defining `UINT8`/`__PACK`/
`__WLAN_ATTRIB_PACK__`, so the struct body itself no longer parses. The fix is to
untangle the `wifi.h` ↔ `8192cd_cfg.h` cycle properly — e.g. split the feature macros
into a leaf header both can include, or move the type definitions ahead of the cycle.

Driver left `# CONFIG_RTL8192CD is not set` so the image builds; re-enable to resume.

#### Correction: there is NO circular include — and the blocker is still open

The entry above blamed a `wifi.h` ↔ `8192cd_cfg.h` cycle. **That is wrong.**
`8192cd_cfg.h` does not include `wifi.h` at all. The real situation is simpler:
`8192cd.h` uses `struct wlan_amsdullcsnaphdr_t` but never included `wifi.h`, which
defines it. Main driver units survive because they pull `wifi.h` in via
`8192cd_util.h`; the `phydm/` units include only `8192cd.h`.

Fixes applied and kept (all correct in themselves):
- struct made unconditional in `wifi.h`
- `wifi.h` given its own `8192cd_cfg.h` + `typedef.h` includes
- `#include "./wifi.h"` added to `8192cd.h` **after** `8192cd_cfg.h`/`sys-support.h`,
  so `__PACK`, `__WLAN_ATTRIB_PACK__` and `UINT8` exist first. (A first attempt put it
  at the top of the include list, which hoisted `wifi.h` ahead of those macros and
  stopped the struct body parsing — worse, not better.)

⚠ **The error persists** — `phydm/../8192cd.h: field 'amsdullcsnaphdr' has incomplete
type`, 5 objects compile. Five attempts have not cleared it, so this stops here rather
than continuing to reason about the include graph from the outside.

**The right next move is mechanical, not inferential:** preprocess the failing
translation unit (`-save-temps` on one phydm object, or `make ... CFLAGS=-E`) and read
what the compiler actually sees around that struct. Every attempt so far has been an
inference about include order; the preprocessed output settles it in one step.

State: driver `# CONFIG_RTL8192CD is not set`, image builds (4.75 MB of 7.9 MB), staged
sources and header fixes all in place — re-enabling one config symbol resumes here.

#### ROOT CAUSE of the G3 compile blocker — it is the ccflags, not the include graph

Preprocessing the failing translation unit (the step named above) settled it, and the
answer was in neither place I had been looking.

`SUPPORT_TX_AMSDU_SHORTCUT` — the macro that gates both the struct and its use — is
defined in `8192cd_cfg.h` behind a chain of **vendor** config macros:

    #if defined(CONFIG_WLAN_HAL_8814AE) || defined(CONFIG_WLAN_HAL_8822BE) && !defined(__ECOS)
    #define SUPPORT_TX_AMSDU
    #endif
    #if !defined(CONFIG_RTL_8198C)
    #ifdef SUPPORT_TX_AMSDU
    #if defined(TX_SHORTCUT) && !defined(CONFIG_RTK_MESH)
    #define SUPPORT_TX_AMSDU_SHORTCUT

Those macros do **not** come from the kernel `.config` — `CONFIG_RTL_8197F` is not in it
at all. They come from the **vendor Makefile's `ccflags-y += -D...` lines** (e.g.
`-DCONFIG_WLAN_HAL_8197F` at `Makefile:62`, and the `CONFIG_SLOT_0_8822BE` /
`CONFIG_SOC_RFE_TYPE_0` family that gave this tree its G4 significance).

So the driver's feature macros are only self-consistent when the full vendor ccflags set
reaches every translation unit. Under this port's Kconfig/Makefile hookup that is not
guaranteed for the `phydm/` subdirectory, which is exactly the set of units that failed
— and it explains why the main units compiled while phydm did not, without any include
cycle being involved.

**Consequence for the port:** the fix is NOT more header surgery (the three header
changes made are individually correct but were treating a symptom). It is to make the
vendor `ccflags-y` block apply to all subdirectories — i.e. drive the build from the
vendor Makefile as the driver's real kernel Makefile (it already carries the complete
`-D` set and the per-subdir `EXTRA_CFLAGS` include paths), rather than layering a
minimal Kconfig/obj-y hookup over it.

That is a concrete, bounded change and it is where the next session should start.

#### ⚠ Correction: the "ccflags" root cause above is NOT verified either

The entry above states the blocker's root cause as "the vendor ccflags don't reach the
phydm/ units". **That is a hypothesis, not a confirmed finding, and the evidence now
argues against it:** the vendor Makefile emits `-DCONFIG_WLAN_HAL_8822BE`
*unconditionally* at `Makefile:63`, so the macro chain
`CONFIG_WLAN_HAL_8822BE → SUPPORT_TX_AMSDU → SUPPORT_TX_AMSDU_SHORTCUT` should be
satisfied for every unit built through that Makefile — including phydm's.

So the honest state is: **the blocker is not root-caused.** Four separate explanations
have been proposed and each failed to survive contact with evidence:

1. "`asm/rtl865x/*` headers must be supplied" — disproved (all references dead).
2. "`wifi.h` ↔ `8192cd_cfg.h` circular include" — disproved (`8192cd_cfg.h` does not
   include `wifi.h`).
3. "`8192cd.h` never includes `wifi.h`" — true, and fixed, but the error persists.
4. "vendor ccflags don't reach phydm/" — contradicted by `Makefile:63`.

Two attempts to preprocess the failing unit standalone both aborted before reaching the
struct, on artefacts of the hand-built command line (missing `KERNEL_VERSION`, then
missing mach-specific include paths) rather than on anything about the driver. That
approach is a dead end as executed.

**The only reliable next step is to capture the REAL compile command** the kernel build
issues for a phydm object — `make target/linux/compile V=s` prints it — and re-run that
exact command with `-E -dD`, changing nothing else. Every diagnosis so far has come
from an approximation of that command; none of them held.

Recorded this way deliberately: an unverified root cause left standing in the docs is
worse than an open question, because the next person would build on it.

#### R4/G3 — the real blocker found and fixed; driver now builds 50 objects

The `incomplete type` error that stalled this for eight attempts is **solved**, and the
cause was none of my four earlier guesses. It was a genuine vendor bug that this config
combination exposes:

`wifi.h` splits into an `#ifdef NOT_RTK_BSP` branch (lines 75–133) and an `#else`
branch. `wlan_amsdullcsnaphdr_t` was defined **only in the `#else`**. But
`NOT_RTK_BSP` **is** defined (`8192cd_cfg.h:1885`), while `SUPPORT_TX_AMSDU_SHORTCUT`
is **also** defined (via `CONFIG_WLAN_HAL_8822BE` from the vendor Makefile) — and
`8192cd.h` uses the struct under that second macro. So the type is used but never
defined. The main driver units survive only because they pull the `#else` branch in by
another route; the `phydm/` units do not.

Found by probe, not inference: a `#error` at the use site proved the struct invisible,
a second probe proved the include guard was *not* pre-set, and the conditional nesting
then showed the branch split. Every earlier hypothesis (missing `asm/rtl865x`, circular
include, missing `wifi.h` include, ccflags not reaching phydm) was wrong.

**Fixes applied, each minimal and justified:**

| fix | why |
|---|---|
| define `wlan_amsdullcsnaphdr_t` in the `NOT_RTK_BSP` branch too | makes both branches agree; the real bug |
| `ccflags-y += -Wno-error=incompatible-pointer-types` (+ 4 similar) | vendor targets gcc 4.x/7.x; this tree is gcc 8.4. Scoped to this driver only |
| `ccflags-y += -I$(srctree)/arch/mips/include/asm/mach-rtl8197f` | `<bspchip.h>` lives there; the kernel only adds the *configured* platform's mach dir |
| `ccflags-y += -DCONFIG_RTL_8197F` | the vendor selects its SoC with this; this tree names it `CONFIG_SOC_RTL8197F`, so gates silently took wrong branches (e.g. `8192cd_osdep.c:174` included `<bsp/bspchip.h>`) |

**Progress: 0 → 50 objects compiling.**

★ Incidental but valuable: the vendor's `bspchip.h` independently confirms the R4/G1
reverse engineering — `BSP_WLAN_BASE_ADDR 0xB8640000`, `BSP_WLAN_MAC_IRQ = CPU_BASE+6`,
`BSP_WLAN_MAC_IE = BIT(29)`. All three were decoded from the stock binary and verified
on silicon *before* this source was obtained.

**Two blockers remain, and they are different in kind:**

1. `BSP_PCIE0_D_CFG0` / `BSP_PCIE0_D_MEM` / `BSP_PCIE_IRQ` — more BSP symbols. Same
   class as the ones just fixed; tractable.
2. ★ **`struct sk_buff has no member 'srcPhyPort'`** (`8192cd_rx.c:7241`, `:8073`) — the
   vendor SDK **adds fields to the kernel's core `sk_buff`**. This is the `net/rtl`
   integration the G0 audit flagged at the outset. It cannot be fixed inside the driver:
   it needs a patch to `include/linux/skbuff.h` adding the vendor fields, which is an
   invasive core-kernel change and the point at which "port the driver" becomes "port
   the vendor's kernel integration layer".

That second item is the honest measure of what G3 still costs, and it is a decision
point rather than a mechanical step: patch core `sk_buff`, or strip the code paths that
use it (the fast-bridge / hardware-forwarding hooks) and accept a reduced driver.

#### R4/G3 final state this session — 53 objects, one error class left

Best achieved: **53 of the driver's objects compile** against kernel 4.14.187, with a
single remaining error class. Progress from zero was driven by four fixes plus one
scoping decision, all recorded in the driver's Makefile and headers.

**Fixes that stuck:**
1. `wlan_amsdullcsnaphdr_t` defined in the `NOT_RTK_BSP` branch of `wifi.h` — the real
   bug, found by `#error` probe after four wrong hypotheses.
2. Five `-Wno-error=` classes, scoped to this driver (vendor targets gcc 4.x/7.x, tree
   is gcc 8.4).
3. `-I$(srctree)/arch/mips/include/asm/mach-rtl8197f` so `<bspchip.h>` resolves.
4. `-DCONFIG_RTL_8197F` — the vendor's SoC selector; this tree names it
   `CONFIG_SOC_RTL8197F`, so gates silently took wrong branches.
5. `CONFIG_RTL_CUSTOM_PASSTHRU` disabled — it was the only thing enabling the
   `srcPhyPort` paths, and `srcPhyPort` is a vendor addition to the core `sk_buff`.
   Turning the feature off avoided patching `include/linux/skbuff.h` entirely.

**Remaining error class:** `pskb->__unused` (`8192cd_rx.c:10105/10256/10262`) — another
vendor field added to the core `sk_buff`, reached because
`#if !defined(NOT_RTK_BSP) && !defined(CONFIG_OPENWRT_SDK)` evaluates true in that unit.

**⚠ Two placement/combination traps, both measured — do not repeat:**
- `-DCONFIG_OPENWRT_SDK` **does** fix `__unused`, but it simultaneously stops
  `<bspchip.h>` resolving in `8192cd_osdep.c` and adds an `arch/mips/uaccess.h` type
  error. Net **worse**: 53 objects → 51 plus new classes.
- Clearing `CONFIG_PCI_HCI` collapses the build to **5** objects
  (`struct tx_sc_entry` incomplete). It selects the PCI-style descriptor/ring model,
  which the **on-SoC** WMAC uses too — it is not a "PCIe card present" switch.
- Appending the port flags at the END of the Makefile drops the build to **2** objects:
  it breaks the vendor's own `-I$(src)/WlanHAL/...` paths. They must come first.

**Honest read:** the object count oscillates 46–53 depending on flag placement and
combination, each change trading one error class for another. That is the signature of
a config model that needs untangling systematically — mapping which vendor macros this
build must define and in what order — rather than more one-off flag experiments. That
is the next session's task, and it is bounded work on a known surface, not a search.

Driver left `# CONFIG_RTL8192CD is not set`; image builds at 4.75 MB of 7.9 MB.

#### R4/G3 — session close: 52 objects, ONE error remaining

Final state: **52 of the driver's objects compile** against 4.14.187. One error class
left, and it is a header interaction rather than a driver problem:

    ./arch/mips/include/asm/uaccess.h:73:40: error: invalid type argument of '->' (have 'int')

i.e. `get_fs()`'s `current_thread_info()->addr_limit` — `current_thread_info()` is
resolving to `int` in one translation unit (the failure appears around
`8192cd_proc.o`). The driver defines neither `current_thread_info` nor `thread_info`,
so something it pulls in is shadowing it indirectly.

**The complete working flag set** (the real deliverable, since reconstructing it took
the whole session — it was preserved verbatim as `g3-rtl8192cd-portflags.mk`;
**[2026-08-02: that file has been REMOVED from the repo** — it froze this G3-era set,
which the final build later corrected on three flags (`CONFIG_BAND_2G_ON_WLAN0`,
`PHY_EAT_40MHZ` off, `CONFIG_RTL_COMAPI_CFGFILE=y` — the "three silent failures" of
[`WIFI-DUAL-BAND.md`](WIFI-DUAL-BAND.md) §4), and nothing ever included it. The
working set lives in the `+` side of `g3-rtl8192cd-4.14-port.patch`'s `Makefile`
hunks; see WIFI-DUAL-BAND §9 item 5.]**):

| # | flag | why |
|---|---|---|
| 1 | five `-Wno-error=` classes | vendor targets gcc 4.x/7.x; tree is 8.4 |
| 2 | `-DCONFIG_RTL_8197F` | vendor's SoC selector; tree names it `CONFIG_SOC_RTL8197F` |
| 3 | `-DNOT_RTK_BSP` | auto-defined only when `CONFIG_RTL_8197F` is *unset*, so (2) silently disabled it and re-enabled the `pskb->__unused` paths |
| 4 | `-DRTK_NL80211` | the `rtk` member and `CFG80211_*` enum are gated on it while their use sites are not |
| 5 | `IEEE80211_BAND_* → NL80211_BAND_*` | renamed in kernel 4.7 |
| 6 | six `BSP_*` constants by `-D` | see below |
| 7 | `CONFIG_RTL_CUSTOM_PASSTHRU` disabled (header) | removes the `srcPhyPort` paths, avoiding a core `sk_buff` patch |
| 8 | `wlan_amsdullcsnaphdr_t` added to `wifi.h`'s `NOT_RTK_BSP` branch | the original bug |

★ On (6): the BSP constants are supplied by `-D` rather than by including
`bspchip.h`, because that header is only reachable on some branches of the
`NOT_RTK_BSP` / `CONFIG_OPENWRT_SDK` / `CONFIG_RTL_8197F` matrix, and force-including
it lands it ahead of the kernel's own headers. **Every value matches what R4/G1
decoded from the stock binary and what the `rtl8197f-wmac` driver then read on
silicon** — WMAC base `0xB8640000`, IRQ 6, plus the PCIe entry (`0xB8B10000`,
`0xB9000000`, IRQ 5) that G1 used as its self-check. Three independent sources agree,
which is the strongest validation the hardware map has had.

**Measured traps — do not repeat:**
- `-DCONFIG_OPENWRT_SDK`: fixes `__unused` but breaks `<bspchip.h>` resolution and adds
  the uaccess.h error. Net worse.
- clearing `CONFIG_PCI_HCI`: collapses to 5 objects. It selects the PCI-style
  descriptor/ring model the **on-SoC** WMAC also uses — not a "PCIe card" switch.
- port flags appended at the END of the Makefile: collapses to 2 objects by breaking
  the vendor's own `-I$(src)/WlanHAL/...` paths. They must come first.
- force-including `bspchip.h` with `-include`: breaks `arch/mips` `uaccess.h` and
  `thread_info.h`.

**Next step, and it is one command, not a search:** preprocess the failing unit with
the real build flags (`-save-temps` on `8192cd_proc.o`, or re-run its exact `make V=s`
command line with `-E -dD`) and grep the output for `current_thread_info`. That shows
what is shadowing it. Every remaining unknown here is now a single lookup.

#### R4/G3 — stopping point, and the one thing that will crack it

Best result stands at **52 objects** (parallel build) / 44–47 before the first serial
failure. Two error sites remain, both in `8192cd_osdep.c` and both the *same* root
issue: `BSP_WLAN_CONF_ADDR` / `BSP_WLAN_BASE_ADDR` / `BSP_WLAN_MAC_IRQ` undeclared at
lines 372–374, plus `BSP_BOND_97F*` in `8192cd_hw.c`.

**What was tried for that one symbol class, and measured:**

| attempt | result |
|---|---|
| `-I .../mach-rtl8197f` | path present, symbols still undeclared |
| `-include .../bspchip.h` | symbols resolve, but breaks `arch/mips/uaccess.h` + `thread_info.h` (`current_thread_info()` implicitly declared → `int`) |
| supply the constants by `-D` | works for 6 symbols; does not scale — the driver needs 20+ (`BSP_BOND_97F*`, `BSP_PCIE0_*`, `BSP_GPIO_*`, `BSP_MISC_PINSR`, …) and `BSP_WLAN_MAC_IRQ` clashes with the header's `(BSP_IRQ_CPU_BASE + 6)` spelling |
| `-DUSE_RLX_BSP` (to reach the `<bspchip.h>` branch at `osdep.c:175`) | no change |

`8192cd_osdep.c:168-178` is the deciding block: `<bspchip.h>` sits in the `#else` of
`#if !defined(USE_RLX_BSP)`, nested inside
`#if defined(CONFIG_RTL_819X) && defined(__LINUX_2_6__)`. `CONFIG_RTL_819X` is defined
(`8192cd_cfg.h:168`); `__LINUX_2_6__` was **not verified** and is the most likely
culprit — if it is unset the entire block is skipped and no BSP header is included at
all, which matches the symptom exactly and would explain why neither `-I` nor
`USE_RLX_BSP` changed anything.

**The one command that settles it** — check `__LINUX_2_6__`, then preprocess that file:

    grep -rn 'define __LINUX_2_6__' drivers/net/wireless/rtl8192cd/*.h
    # then, with the driver enabled, take the exact `make ... V=s` line for
    # 8192cd_osdep.o and re-run it with -E -dD, grepping for BSP_WLAN_BASE_ADDR

Stop reasoning about the include graph from outside; that has now failed on this file
five times. Read what the preprocessor actually did.

Everything else is banked: the complete working flag set is recorded in the `+` side
of `g3-rtl8192cd-4.14-port.patch`'s `Makefile` hunks (`g3-rtl8192cd-portflags.mk`,
which froze an earlier iteration of it, was removed 2026-08-02 — see the note above),
and the traps above are recorded so none are rediscovered.

#### R4/G4 — hardware precondition CONFIRMED on silicon

Before the 2.4 GHz driver exists, the question "can both radios actually be live at
once on this board?" can be answered independently — and it is now answered YES.

Measured on a running RAM-boot with the G2 bring-up driver bound and the 5 GHz AP up:

    WLAN_EN (SR+0x64): 00000000 -> 0000001f      (WMAC gate lifted and HELD)
    WMAC regs +0x000=51c0b2f3 +0x004=14020012    (on-SoC block responding)
    wlan1: ESSID "DIR842-OpenWrt", Mode Master, Channel 36 (5.180 GHz)
    /sys/class/ieee80211/ -> phy1                 (rtw88's phy, the only one so far)

So the on-SoC WMAC is powered, gated on, and answering register reads at the same time
as the PCIe RTL8822BE is beaconing. Nothing about lifting `SR+0x64` disturbs rtw88,
and the ethernet datapath stays at 0% loss throughout. This matches stock, which runs
`wlan0` + `wlan1` together.

**What that means for G4:** the remaining work is purely software — a driver that
registers a second `phy` for the on-SoC radio, plus the userspace wiring to run an AP
on each. There is no hardware arbitration, power or clock conflict to solve, which was
the main open risk.

The G4 gate is therefore: `/sys/class/ieee80211/` shows **two** phys, an AP on each,
and a client associated on both simultaneously.

## ★★ R4/G3 — rtl8192cd.ko LINKS. Root cause was dead code, not include paths.

**`rtl8192cd.ko` builds and links: 116 objects, exit 0.** The wall that resisted eight
of my inference attempts had a cause none of them could have reached, and it was found
by *measuring* — recovering the real kbuild command line from `.8192cd_tx.o.cmd` and
re-running it with `-E -dD`.

**Root cause: the `#include <bspchip.h>` sites are UNREACHABLE DEAD CODE.** The
189,406-line preprocessed TU contains zero `bspchip.h` markers, zero `platform.h`
markers, zero `BSP_*` defines, and no `#define CONFIG_RTL_819X`. `8192cd_osdep.c:168`
gates the whole block on `CONFIG_RTL_819X`, whose only self-define site
(`8192cd_cfg.h:168`) is nested under **both** `#if !defined(__LINUX_2_6__)` **and**
`#if defined(CONFIG_RTL8196B|C|8198)` — both false on 4.14/8197F. In the vendor SDK the
symbol comes from the *kernel's* `.config`. **No `-I` could ever have helped.**
`8192cd_hw.c` uses a different dead gate, `USE_RTL8186_SDK`.

Two of my own conclusions were disproven by the same evidence:
- `-DUSE_RLX_BSP` is **inert** — `8192cd_cfg.h:1820` defines it, then `:2007` does
  `#undef USE_RLX_BSP` under `#ifdef NOT_RTK_BSP`, which this port sets.
- the `uaccess.h` breakage was **never** caused by `-include` — it is the vendor's own
  include order (`8192cd_security.c:15` includes `<asm/uaccess.h>` before
  `<linux/module.h>`), and it reappeared with no `-include` in the build.

**The fixes that got it linking:**

| # | change | effect |
|---|---|---|
| 1 | `#include <bspchip.h>` appended to `8192cd_cfg.h` under `CONFIG_RTL_8197F && __KERNEL__` — every TU reaches it, *after* the kernel's headers | 52 → 55 objects, all BSP errors gone |
| 2 | staged `include/net80211/` (4 headers) from the vendor SDK | needed by the `RTK_NL80211` sources |
| 3 | ★ `<asm/uaccess.h>` → `<linux/uaccess.h>` in **14** files — the correct post-3.4 spelling, which pulls `linux/thread_info.h` first | **55 → 101 objects** |
| 4 | two genuine vendor-rot fixes in `Hal88XXTxDesc.c` (`TXBD_BEACON_OFFSET_8197F` → `_V1`, a rename the vendor missed in one block; and dropping an AMSDU write-back that dereferences a struct member that does not exist) — both in `CONFIG_NET_PCI` paths the vendor never compiled | **101 → 116 objects, .ko LINKED** |
| 5 | `<linux/sched/signal.h>` in `8192cd_util.c` (4.10 split the signal API out) | last implicit declaration gone |

Module: `ELF 32-bit LSB relocatable, MIPS32 rel2`, **1.87 MB stripped** (6.6 MB with
debug), `license=GPL`. Fits the 7.9 MB budget with ~3.1 MB free.
Packaged via a new `target/linux/realtek/modules.mk` (`kmod-rtl8192cd`); confirmed in
the rootfs at 1.81 MB.

Full port captured in `g3-rtl8192cd-4.14-port.patch` (25 files) with the fetch recipe.

**Caveat, stated plainly: this is compile-and-link only.** The driver has never been
loaded. Remaining warnings (non-fatal, expected for 3.18-era code on gcc 8.4): 104
`-Wincompatible-pointer-types`, 44 `-Wint-conversion` — downgraded from errors and the
likeliest home for latent runtime bugs.

## R4/G4 — concurrent dual-band: verdict, mechanism, and the blocker that would have bitten

**Verdict: achievable.** `rtl8192cd` is cfg80211-only (`wiphy_new`/`wiphy_register`, and
**no `ieee80211_alloc_hw` anywhere**), so it never competes with mac80211. It either
adds a second `phyN` (Path A) or stays a pure WEXT netdev (Path B).

★★ **The blocker that would have silently destroyed the working 5 GHz AP:**
`rtl8192cd_pci_tbl[]` contains `{ REALTEK, 0xB822 }` under `CONFIG_WLAN_HAL_8822BE`, and
`MODULE_DEVICE_TABLE` + `pci_register_driver` are live (`CONFIG_NET_PCI` is force-set by
`8192cd_cfg.h` under `NOT_RTK_BSP`). Loading the driver as built would have made it
**claim the RTL8822BE away from rtw88**. Fixed: `CONFIG_WLAN_HAL_8822BE`,
`SLOT_0_8822BE`, `SLOT_0_RFE_TYPE_10`, `SLOT_0_TX_BEAMFORMING`, `RTL_5G_SLOT_0`,
`BAND_5G_ON_WLAN0` and `USE_PCIE_SLOT_0` are all compiled out; `CONFIG_PCI_HCI` stays ON
(it is the descriptor/ring model the on-SoC WMAC also uses, not a "PCIe card" switch).
Verified the module still links with them off.

**Stock's model — and it is Path B.** Stock's rootfs has **no hostapd at all**; its only
802.1X binary is Realtek's `bin/auth` (RADIUS/Enterprise only), and `libdhal.so` logs
"auth daemon isn't needed!" for PSK. **WPA2-PSK's 4-way handshake runs inside the
driver.** So a WEXT-only 2.4 GHz radio alongside mac80211's 5 GHz is exactly what ships
in production on this hardware.

**Configuration mechanism (better than per-ioctl setup, and present in our source):**
stock writes one file and lets the driver ingest it —
`CFG_FILE_PATH "/etc/Wireless/RTL8192CD.dat"` (`8192cd_comapi.c:3431`), read by
`CfgFileProc()` which `8192cd_osdep.c:7703` calls on open, reloadable via the `cfgfile`
ioctl. Lines are `<ifname>_<mib>=<value>`. Bringing the interface up applies the whole
config atomically, including MIBs that must be set *before* open. Stock symlinks
`/etc/Wireless` → `/tmp` so it lands in tmpfs.

**Artifacts now in the tree:**
- `base-files/lib/netifd/wireless/rtl8192cd.sh` — netifd handler; writes the `.dat`
  from UCI then brings the interface up. All MIB names verified against
  `8192cd_ioctl.c`. `/sbin/wifi` dispatches on UCI `type` → `/lib/netifd/wireless/$type.sh`.
- `base-files/etc/uci-defaults/09_wireless-dualband-dir842` — creates the
  `/etc/Wireless` symlink, seeds `radio1` (`type 'rtl8192cd'`). `radio0` (rtw88,
  `type mac80211`) is untouched.

**Interface naming (corrected, proven three ways):** stock is `wlan0` = **5 GHz**
(RTL8822BE) and `wlan1` = **2.4 GHz** (on-SoC) — the reverse of the intuitive reading.
Evidence: `librlx_wifi_mibs.so`'s calibration keys (CCK/HT40 → `wlan_index=1`, 5G_* →
`wlan_index=0`), `libdhal.so` pairing `RadioOff`↔`wlan1` and `5G_RadioOff`↔`wlan0`, and
`etc/config.default`'s `"wifi": "wlan0", "wifi_2G": "wlan1"`.

**Two further findings worth acting on:**
- ⚠ `librlx_wifi_mibs.so` hard-checks the driver name string `rtl8192cd` before applying
  anything, then pushes per-radio TX-power calibration from `/dev/mtd1`. **An
  rtw88-driven 5 GHz radio is silently skipped and comes up uncalibrated.** That is the
  same signature as the RX-deaf symptom on the other router in this fleet, so it is
  worth measuring rather than assuming. The stock 5 GHz tables are mapped
  (`pwrlevel5GHT40_1S_A/B`, `pwrdiff_5G_*`).
- WPA3/SAE is absent from *this* driver vintage (no `wpa3/`, no `sae_` symbols; only
  `dot11IEEE80211W` PMF). Stock's kernel *does* contain `rtl8192cd/wpa3/src_mbedtls/`,
  so a newer vendor drop adds in-kernel SAE. A vintage limitation, not architectural.

**If Path A (second `phy`) is pursued later:** the kernel has no in-tree cfg80211 — it
comes from backports 5.8 — and `CONFIG_MODVERSIONS` is off, so symbols bind by name with
no CRC check. 4.14-shaped structs meeting a 5.8 cfg80211 would corrupt silently. Path A
must therefore move the driver into `package/kernel/mac80211`. The only cfg80211 op the
driver uses that 5.8 removed is `mgmt_frame_register` → `update_mgmt_frame_registrations`
— one shim, not a rewrite. (Consistent with what we hit: enabling the cfg80211 objects
produced `cfg80211_inform_bss` signature errors and a `cfg80211_mgmt_tx_params`
redefinition.)

## ★★★ R4/G3 COMPLETE — the vendor driver LOADS AND RUNS on the hardware

`rtl8192cd` is bound to the RTL8197F's on-SoC 2.4 GHz WMAC on real silicon, with the
mainline rtw88 5 GHz AP still beaconing alongside it. Measured:

    insmod rtl8192cd            -> rc 0
    "Realtek WLAN driver - version 1.7", DFS 2.0.14, Adaptivity 9.3.4
    6x rtl8192cd_init_one, RFE TYPE =0
    netdevs: wlan0, wlan0-va0..va3, wlan0-vxd
    /proc/interrupts:  6:  596  MIPS 6  wlan0     <- WMAC at 0xB8640000, IRQ 6, live

`ifconfig wlan0 up` then ran genuine hardware init — no oops:

    [97F] Bonding Type 97FS, PKG1      <- strap read by our own rtl819x_bond_option(),
                                          independently matching the strap=10 the G2
                                          driver measured months of analysis earlier
    RFE type 0 ... clock 40MHz ... load efuse ok ... rom_progress
    PHY_REG_PG_8197Fmp_Type0 ... rtl8197Ffw firmware handed to HW
    Default BB Swing=30 ; rings allocated (RDSAR 0x01b96000, TMGDA 0x01b82000)

**rtw88's 5 GHz AP is untouched** — `wlan1`/`phy1`, SSID `DIR842-OpenWrt`, ch36/80 MHz,
and confirmed still beaconing *on air* by an external scan from the host's own WiFi
adapter. The vendor driver never calls `pci_register_driver()` at all now (its device
table index 0 is `TYPE_EMBEDDED`), so it cannot claim `10ec:b822`.

**Image: 5.188 MB of the 7.9 MB partition** (65%), `kmod-rtl8192cd` 0.65 MB packaged.

### Three findings that were load-bearing

1. ★ **`CONFIG_BAND_2G_ON_WLAN0` was missing and is essential.** The on-SoC WMAC's
   `wlan_device[]` entry is gated on it. Without it the table degenerates to three
   all-zero `TYPE_PCI_BIOS` entries and `init_module()` only calls
   `pci_register_driver()` — the embedded radio is never touched at all. With it:
   `wlan_device[0] = {base 0xb8640000, irq 6}`, verified in the object file. It also
   flips `WLANIDX` so the radio gets 2.4 GHz ring/buffer sizes.
2. **New `8192cd_owrt_bsp.c`** supplies the two Realtek-BSP exports this tree lacks.
   `rtl819x_bond_option()` is transcribed from the vendor's `arch/mips/rtl8197f/gpio.c`
   and selects `ODM_CMNINFO_PACKAGE_TYPE`. `PCIE_reset_procedure_97F()` is a
   **deliberate read-only stub** — the vendor version drives PERST# low for 300 ms,
   which would drop the very bus rtw88's 8822BE sits on.
3. ⚠ **Build trap:** `make modules` alone **silently skips** the undefined-symbol check
   when `vmlinux` is absent — it returns rc=0 with undefineds present. Use
   `make vmlinux modules`. Four undefineds were hiding behind this.

### Not yet working

The 2.4 GHz radio **does not beacon**: `up_flag=0`, `tx_packets=0`, and an external scan
sees no BSS on 2412 MHz. It is *configured* (opmode 0x10 = AP, ch 1) but the vendor's
userspace "start" step — normally driven by the vendor apmib tooling, and in our design
by the `RTL8192CD.dat` + netifd handler already in the tree — has not been performed.
That is the next milestone, not a regression.

**Load ordering matters:** insmod *after* netifd brings the 5 GHz AP up, or the vendor
root device grabs `wlan1` and netifd then configures the wrong interface.

### Two PRE-EXISTING bench faults, both proven independent of this work

- **Ethernet datapath is wedged** (100% loss, `rx_done=0`). Control: identical with
  `rtl8192cd` **not loaded** on the same NOR image. This is the known M7 large-frame
  CPU-RX wedge, not a driver regression.
- ★ **The box kernel-panics every 45–90 s**, and the trigger is **traffic from the host
  desktop**: KDE Connect (`kdeconnectd`, UDP 1716) at ~4000 pkt/s. A ~544-byte inbound
  frame corrupts kernel memory → `Unhandled kernel unaligned access` in
  `ep_send_events_proc`, or a SIGSEGV storm across ubusd/netifd/logd → "Attempted to
  kill init". Proven independent two ways: it fired on a boot where the module was never
  inserted, and with the module loaded the box survived 375 s while `eth0` was held
  down, dying 1.1 s after `eth0` came back up. Capture in `scratchpad/bench.pcap`.
  ⚠ Do **not** block udp 1714–1764 on the host OUTPUT chain — it also breaks the
  loader's TFTP and ramboot stops working. Stop `kdeconnectd` instead.

# ★★★ R4 COMPLETE — CONCURRENT DUAL-BAND WORKS

Verified by an external scan from a separate host adapter (not the box's self-report):

    BSS e0:1c:fc:51:c9:f0 (on wlp4s0)   freq 5180.0   SSID: DIR842-OpenWrt
    BSS 00:e0:4c:81:86:86 (on wlp4s0)   freq 2412.0   SSID: DIR842-2G
                                        RSN: CCMP / PSK, beacon interval 100 TUs

**Both radios beacon simultaneously** — mainline **rtw88** on the PCIe RTL8822BE at
5 GHz, and the **vendor `rtl8192cd`** on the SoC's integrated WMAC at 2.4 GHz. PCI
`01:00.0` stays bound to `rtw_8822be` throughout.

Client association proven on the 2.4 GHz radio:
- **Open, ch 1** — associated, ping 6/6, 0% loss.
- **WPA2-PSK** — associated at HT MCS4 43.3 Mbit/s, **DHCP lease obtained from the box
  over the air** (192.168.0.226/24), ping 8/8 0% loss avg 3.9 ms, `total_psk_fail: 0`.
  The 4-way handshake runs **inside the driver** — no hostapd, exactly as stock does it.

This is the goal the whole R4 milestone was defined around, and it closes the port's
last functional gap against vendor firmware.

## ★ The bug that kept it silent: the wrong crystal

The AP "start" step was never missing — that premise was wrong. `/proc/wlan0/stats`
showed `beacon_ok` climbing at exactly the TBTT rate (10/s at 100 TU), i.e. the MAC was
already beaconing on `ifconfig up`. (`up_flag` is a red herring: it is `CLIENT_MODE`-only,
set when a *station* associates, never in AP mode.) The failure was **RF**.

`8192cd_hw.c:14672` selects the 8197F WLAN crystal *solely* from `CONFIG_PHY_EAT_40MHZ`.
The vendor's strap-reading alternative immediately above it is dead code here — gated on
`CONFIG_AUTO_PCIE_PHY_SCAN`, which is only defined for `CONFIG_RTL_8196E`/`__OSK__`.
Our Makefile had the flag set, so `XTAL_CLK_SEL_40M` went into `InitPONHandler()` and the
WLAN PLL came up for a 40 MHz part. **LO off by 40/25 = 1.6×** → MAC, BB and firmware all
report success, IQK completes, and nothing is on air; RX equally deaf (`rx_packets: 0`).

Three independent sources say 25 MHz, and one of them is our own earlier work:
1. stock firmware's boot log on this unit prints `clock 25MHz`;
2. **our mainline `rtl8197f-wmac` bring-up driver reads bootstrap `SR+0x08 BIT(24)` and
   prints `xtal 25MHz`** — the G2 stub written long before this driver existed;
3. the radio went from silent to loud when the flag was cleared.

(Stock's `clock 40MHz` line belongs to the 8822B — a different chip.)

## Second silent failure: the config path was compiled out

`CONFIG_RTL_COMAPI_CFGFILE` had **never** been set, so `CfgFileProc()` *and* its call site
at `8192cd_osdep.c:7691` were both preprocessed away — the netifd handler already in the
tree would have been a silent no-op. Additionally `CfgFileRead()` called `fp->f_op->read`
directly, which is NULL on tmpfs (and `/etc/Wireless` → `/tmp`); it now uses 4.14's
`kernel_read()`.

## Remaining polish (none of it blocks the milestone)

- ★ **Interface-naming hazard.** Measured on two boots *before* any insmod: rtw88's 5 GHz
  is **`wlan1`** and the vendor root device is **`wlan0`** — the opposite of what
  `uci-defaults/09_wireless-dualband-dir842` and `rtl8192cd.sh` currently assume. As
  shipped, a netifd bring-up would down and reconfigure rtw88's interface. Fixing it
  properly means pinning load order first (e.g. `AUTOLOAD` the module).
- The radio was driven **by hand**, not through netifd — `wifi up` is not yet exercised
  for it. The MIB names the handler emits *are* validated (`Set MIB from … Success`).
- The handler writes `${phy}_regdomain=$country`; that MIB is numeric, so a real code
  like `BR` would be rejected. Works today only because the uci-default sets `country='1'`.
- **TX power is uncalibrated** — `pwrlevelCCK_A/B` and `pwrlevelHT40_1S_A/B` read zeros.
  The per-unit tables are in mtd1 "MAC" (CCK_A `0x0d8`, CCK_B `0x0e6`, HT40_1S_A/B
  `0x0f4`/`0x102`, xcap/ther at `0x13e`). Running on driver defaults; obvious next win.
- `iwinfo` cannot see this radio (WEXT `SIOCGIWNAME` handler is NULL) — expected for a
  WEXT driver, not a fault. `iwpriv`/`iwconfig` work.

## Bench note

RAM-boot panics ~50 s in: `/etc/rc.local` and `/etc/init.d/dir842-asic` both write
`fabric_reset=3`, and on an initramfs boot the first RX frame after that full fabric
reset corrupts memory (simultaneous SIGSEGV across logd/ubusd/netifd/procd → "Attempted
to kill init"). NOR boots survive it. Worked around at runtime only (no tree change) by
killing those two scripts early and setting `fabric_autoreset=0`.
Also: keep serial command lines under ~100 chars — a 210-char line lost bytes at 38400.
