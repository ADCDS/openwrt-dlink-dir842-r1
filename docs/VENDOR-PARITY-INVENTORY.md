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
