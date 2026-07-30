# Vendor firmware inventory & parity checklist — DIR-842 R1

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
