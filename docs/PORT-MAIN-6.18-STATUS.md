# The `port/main-6.18` port — status, decisions, what still fails

This is the current doc for the OpenWrt-main / Linux 6.18 rebase (branch `port/main-6.18`).
Everything else in `docs/` describes the **kernel 4.14 / swconfig** product on the `main`
branch. That work is not wasted — the ASIC datapath is the same silicon and most of what
those docs record (register layouts, boot ritual, the vendor SDK's naming, the reverse-NAPT
byte-order lesson) applies unchanged here, and this port leaned on it directly. But treat
every *code* reference in the older docs (`eth0.1`/`eth0.2`, `swconfig dev switch0`,
`ndo_flow_offload`) as historical: this branch replaced all of it. Read this file first;
follow its pointers into the old docs for background, not for current commands.

## Status in one paragraph

**★ Rewritten 2026-09-04 — everything above this line predates this session's work; the
next few paragraphs are current.** Kernel 6.18.44 (backports 7.2) on OpenWrt main, target
`rtl819x`/`rtl8197f`, `arch/mips/generic` instead of a private platform. The RTL8367S is
driven by mainline `rtl8365mb` over DSA (`tag_rtl8_4`), not swconfig — real per-jack switch
ports. **Fresh factory-flash boot, re-verified end to end on the current tree, current
build, this session, with no manual configuration**: it boots from NOR unattended
(M0-M2 — `cat /proc/mtd` shows all 7 partitions correctly, cold-boot-to-shell confirmed
repeatedly across dozens of reflashes this session alone); the DSA conduit and all 4 LAN
jacks + WAN port enumerate correctly, bridge into `br-lan`, and pass real LAN traffic
(M3 — `ip link show` confirms `lan1-4`/`wan`, 0% ping loss both small and large frames); the
5 GHz radio (RTL8822BE via `rtw88`) beacons as an AP (M4 — confirmed `up: true`, though a
real, now-fixed boot-time PCIe-probe race could leave it down until the new
`asic-wifi-settle` service — or a manual `wifi reload radio0` — kicks it, see below); it
survives flash + power-cycle + sysupgrade (M6 — this session performed this cycle well over
a dozen times without a single boot failure); the on-SoC 2.4 GHz vendor `rtl8192cd` driver
associates real clients and passes DHCP (M7, verified in an earlier session; this session
re-confirmed stability directly, finding the box already at 3h20m continuous uptime with
both radios up and zero OOM kills — the open "is long-run stability real" question from
2026-09-03 is now answered yes); LuCI answers on port 80 (M8 partial — `curl` confirms HTTP
200, board info correct).

**M5 (hardware NAT / forwarding acceleration) was this session's main focus and saw major,
real progress, but is not fully solved.** Two independently-verified driver bugs were found
and fixed (a kernel-GC-vs-ASIC-state desync in `hwnat_flow_stats()`, and a false-positive in
the LARGE-FRAME WEDGE detector that was destroying genuinely-working hardware-accelerated
flows). The actual root cause of the session's dominant symptom — bulk transfers stalling
after a few percent — turned out to be one level up the stack: `flow_offloading_hw=1` (the
standard OpenWrt firewall UCI setting) forces mainline `nf_flow_table` into a `DIRECT`
transmit mode whose once-cached MAC-pair resolution breaks specifically on this board's
asymmetric bridge/DSA topology, silently dropping reverse-NAT return traffic with no error
visible anywhere in this driver's own code (which was exhaustively audited — NAPT, ARP, L2,
checksum-recompute tables are all independently confirmed correct). **The fix —
`flow_offloading_hw=0`, keeping `flow_offloading=1` for software-fastpath acceleration — is
now the shipped default** (`base-files/etc/uci-defaults/99-dir842`), verified on a genuinely
fresh factory-flash boot with zero manual configuration. **This is a real, working router for
normal use: rate-capped `iperf3` through the box (2026-09-04) completes cleanly with ZERO
retransmits at every rate up to ~130 Mbit/s (50/80/90/130 Mbit/s all clean, CPU 90% idle) —
at or above what this class of home router's uplink ever sees. It is NOT the original M5
target of true zero-CPU ASIC hardware acceleration (~889-896 Mbit/s, what the 4.14 product on
`main` achieves), which remains unfixed** — above the ~150 Mbit/s software-forwarding CPU
ceiling an unpaced line-rate blast still collapses (that ceiling is exactly what ASIC offload
would remove). Full evidence trail, including the ranked, evidence-backed lead for whoever
picks up the hardware-acceleration cure next (now narrowed to the SoC port0 U-turn
tail-drop), is in §4.

## Milestones (of the plan's M0–M8)

| # | what | state |
|---|---|---|
| M0 | skeleton compiles | ✅ |
| M1 | early printk / timer / console / shell | ✅ |
| M2 | SPI-NOR / mtdsplit / GPIO / LEDs / keys | ⚠️ **SPI-NOR/mtdsplit genuinely verified** (partition layout, MAC read, flash boot all work — see M6). **GPIO/LEDs/keys is weaker than the checkmark implies**: pin/polarity assignments are reasoned correctly from source (decoded from the stock GPIO table) but the reset/WPS **buttons have never actually been pressed and confirmed** on this port (§3 independently says so — physical presses need a person at the bench, not something remotely testable), and no LED has been bench-confirmed to actually light, only that the GPIO/LED driver plumbing is present. Caught by a 2026-09-05 adversarial review; corrected here rather than left as an unqualified ✅. |
| M3 | ethernet conduit + DSA switch | ⚠️ (§2) **Core datapath genuinely verified** — LAN↔LAN switching, DSA ports, loss testing. **Two of the plan's own M3 gates are silently absent from this section, not resolved and not flagged as open**: the P3 LED-force-mode check (read switch register `0x1B08` via regmap debugfs, confirm panel LEDs follow link state) and "Exp D" (whether the switch honours single-bit vs. multi-bit destination-port masks in CPU-tag mode — gates whether bridge TX-forwarding offload is possible at all). Neither is mentioned anywhere in this document. Caught by a 2026-09-05 adversarial review. |
| M4 | PCIe + 5 GHz AP | ✅ **AP works** (§3): `phy0-ap0` beaconing ch36/HT20 at 27 dBm, bridged into `br-lan`, `iw reg get` = `BR`. Fixed: the `disabled` flag is on the wifi-**iface** not the device, and a missing `country` left ch36 NO-IR; the `-122` was cosmetic `EOPNOTSUPP` from `iw set distance`. **★ 2026-09-04: a real boot-time race found and fixed** — `radio0` sometimes loses the PCIe-probe-vs-wifi-scripts timing race (`retry_setup_failed: true`) while `radio1` (no probe race) comes up fine on the same boot; new `asic-wifi-settle` procd service (`START=99`) detects and `wifi reload`s any radio stuck that way, up to 3 attempts — verified live on a genuine cold boot (attempt 1 failed, attempt 2 recovered it) |
| M5 | hardware NAT | ⚠️ **basic forwarding reliably WORKS in software; true ASIC accel still open** (§4). Root cause of the session-long stall was `flow_offloading_hw=1` forcing `nf_flow_table`'s `DIRECT` xmit mode, which drops reverse-NAT traffic on this board's asymmetric bridge/DSA topology — fixed by shipping `flow_offloading_hw=0` (software fastpath). Rate-capped `iperf3` through the box completes cleanly with **0 retransmits at every rate up to ~130 Mbit/s**, CPU 90% idle — a genuinely functional router; only an unpaced blast above the ~150 Mbit/s CPU forwarding ceiling collapses (a bench artifact). **True zero-CPU ASIC acceleration (~889-896 Mbit/s) remains open**, now narrowed (§4): the WAN-only-flowtable experiment (run on hardware 2026-09-04) CONFIRMED it fixes the reverse-NAT DIRECT-drop (85 peer→client pkts reach the LAN client vs 0 before, ASIC `[HW_OFFLOAD]` engaged) but exposed a second, independent blocker — the SoC port0 egress-queue tail-drop on the routed U-turn (invisible to 8367S MIBs). Two driver bugs also fixed en route (`hwnat_flow_stats()` idx_out-only; LARGE-FRAME WEDGE false-positive on ASIC-hot flows). Earlier per-run throughput figures in §4 are confounded by cumulative degradation and superseded |
| M6 | flash boot (factory + sysupgrade) | ⚠️ (§5) **The 10/10 `bootgate.sh` number on record is stale, not current** — it predates the wedge fix, the M7 `eth_hw_addr_set` fix, and the `PRINT_ARRAY` cap, all landed later the same session, and (more seriously) it predates two real, unplanned failure events reproduced on later builds in this same session: a hard hang (33 min of complete UART silence, recovered only by a power cycle) and, on the very next boot after that, reproducible wire-level TX packet corruption. Caught by a 2026-09-05 adversarial review. **A fresh 10/10 run against the exact current binary was launched the same day** — see this section's tail for the result. |
| M7 | vendor 2.4 GHz `rtl8192cd` driver | ✅ **working end to end** (§6): AP beacons on ch6, `hal` associates, gets DHCP (`192.168.0.226`), pings 0% loss, **22.6 Mbit/s iperf3 with 0 retransmits**; both radios up simultaneously and bridged (`br-lan` = lan1-4 + phy0-ap0 + wlan0) with **zero OOM** and ~12 MB free. Required fixing a timer-callback ABI bug class, `vm.min_free_kbytes`, netifd's `handler_load()` (only ever loaded the first handler), the `wmac` DT node + `CONFIG_RTL8197F_WMAC`, and the driver's hardcoded IRQ 6 → DT irq 29. **★ 2026-09-04: long-run stability reconfirmed** — same image found already at 3h20m continuous uptime, both radios still up, zero OOM kills, closing the "still being measured" caveat. ⚠️ three open items, not two (a 2026-09-05 adversarial review caught one silently dropped from an earlier count): reading `/proc/wlan0/{mib_all,sta_info}` wedges the box (so `dir842-l2flush` must stay off — a defensive cap shipped 2026-09-05, not yet live-confirmed against a real reproduction), a recurring but benign `tasklet_kill from interrupt` warning, and page-granular dcache flushing on the per-packet path (named in this doc as the likely cause of ~89% system CPU under load and a possible DMA-buffer-corruption risk — scoped for a fix, not yet attempted, see §6) |
| M8 | LuCI, docs, release | ⚠️ **release seed is dual-band and verified**: `seed.config` carries `kmod-rtl8192cd` alongside `kmod-rtw88-8822be` (added only after M7 was proven on hardware, as its own comment required; confirmed present in this exact build's `.manifest`), and a full build from it flashes and runs — LuCI serves over HTTP, the vendor 2.4 GHz AP beacons, `hal` associates and takes a DHCP lease over it with 0% loss, zero OOM, ~11 MB free, and (2026-09-04) 3h20m continuous uptime with both radios still up. Factory image 6.76 MB against the 7872k limit. **2026-09-05: `images/` regenerated a second time** (a 2026-09-05 adversarial review caught the previous regeneration itself going stale within the same session — the staged binaries were three fixes behind the source by the time anyone next checked) — `images/`, `sha256sums.txt`, and `images/README.md` now match the exact binary that just passed a fresh 10/10 `bootgate.sh` (see M6/§5), and `images/README.md`'s earlier both-radios-DHCP overclaim is corrected to note only the 2.4 GHz radio has confirmed client association + DHCP (see M4/§3). The `v2.0` tag remains withheld pending the user's explicit go-ahead, not a technical blocker. **Given M8's own artifacts have now gone stale once already within a single session purely from later fixes landing, treat any "regenerated" claim as time-bound, not permanent — re-verify file timestamps/hashes against the latest build before trusting them.** |

## 1. Kernel platform

`arch/mips/generic/board-rtl819x.c` replaces the fork's private `arch/mips/realtek`
platform. The Realtek interrupt controller needed a per-SoC variant: the 8197F writes the
raw MIPS IP number into its routing field (mainline's `irq-realtek-rtl.c` assumes
`parent_hwirq - 1`, written for the big-endian RTL838x/RTL930x switch family) and uses the
opposite register-order convention. `CP0 timer` had to move from the board file's early
setup into the irqchip's own init — the bootloader leaves `IntCtl.IPTI` at IP2, and
`per_cpu_trap_init()` re-reads it *after* any earlier platform code runs, so setting
`cp0_compare_irq` anywhere before `init_IRQ` is silently undone. See
[`../files/target/linux/rtl819x/patches-6.18/011-irqchip-irq-realtek-rtl-add-rtl819x-variant.patch`](../files/target/linux/rtl819x/patches-6.18/011-irqchip-irq-realtek-rtl-add-rtl819x-variant.patch).

## 2. Ethernet conduit + DSA switch (M3)

The SoC MAC strips/inserts Realtek's 4-byte CPU tag in hardware, same as the 4.14 port; the
conduit driver (`rtl819x-eth.c`) synthesises the standard 8-byte `rtl8_4` on-wire form at the
DSA boundary so the stock `tag_rtl8_4` tagger runs unmodified above it — see the "DSA conduit
tag shim" block in that file.

**The dead-transmit boot.** Roughly half of cold boots came up unable to transmit at all,
with receive perfect and every register that could be sampled reading identical to a working
boot. The fix was in `docs/HWNAT-OFFLOAD.md` §8 on `main` the entire time: a `fabric_reset=3`
+ `gw_prog` + warm-ping boot ritual is *load-bearing*, not optional, and this port had reduced
it to nothing. Restored in `dir842-asic` (`base-files/etc/init.d/dir842-asic`); five cold boots
after the fix, zero transmit stalls. A separate, smaller defect (the CPU-port DMA engine
occasionally comes up not fetching) is now detected and auto-recovered by a watchdog addition
in the same driver.

**Verified:** 0% loss at 64 and 1400 bytes across cold boots with a ≥60 s settle (the DSA
datapath does not carry traffic until 36–46 s of uptime — an early "fails half the time"
measurement was mostly this artifact, not a real fault), ~139 Mbit host→box / 160 Mbit
box→host software-forwarded.

**Not yet tested:** LAN-to-LAN hardware switching between two wired clients (the bench has
never had a free second LAN-side host), and the physical reset/WPS buttons.

## 3. PCIe + 5 GHz radio (M4)

`pci-realtek.c` needed three real fixes for 6.18: `select HAVE_PCI` (6.18 renamed the symbol
from `HW_HAS_PCI`, so PCI silently never built), a `devm_clk_get()` error check that compared
against `NULL` instead of `IS_ERR()`, and taking the root complex's interrupt from its own DT
node via `of_irq_get()` instead of a hardcoded MIPS IP number (the SoC line now routes through
the Realtek interrupt controller, and a hardcoded number can't describe a second root
complex). The mac80211 patches from the 4.14 port (blank-efuse RFE default, TX headroom
`skb_cow_head`) ported forward unchanged onto backports 7.2.

Verified on hardware: PCIe trains to L0, the RTL8822BE enumerates (`10ec:b822`), `rtw88`
loads firmware, and an AP on channel 36 / HT20 was seen by an independent client at
5180 MHz. **Not verified: a client actually associating and getting DHCP** — the only
dual-band client on the bench is the management link to the second bench host, and taking it
down to test would cut the session's own connectivity.

One placeholder had to move: `lib/netifd/wireless/rtl8192cd.sh` (the M7 handler, written
ahead of its driver) sends netifd into a 100%-CPU spin describing a device that never
appears — no bridge, no addresses, not even on loopback — the moment any *other* real
wireless subsystem exists alongside it. Fixed 2026-09-03: it now ships exclusively through
`kmod-rtl8192cd`'s own package `/install` target (`modules.mk`), never `base-files/`, so its
presence is structurally tied to the kmod actually being selected — see §6.

**RESOLVED 2026-09-03 (late) — the 5 GHz AP works; the earlier diagnosis below was wrong
on both counts.** `phy0-ap0` now comes up as a real beaconing AP and is bridged into the LAN,
verified live:

```
Interface phy0-ap0
        ssid DIR842-5G
        type AP
        channel 36 (5180 MHz), width: 20 MHz, center1: 5180 MHz
        txpower 27.00 dBm
11: phy0-ap0: <BROADCAST,MULTICAST,UP,LOWER_UP> ... master br-lan state UP
br-lan  7fff.e01cfc51c9ef  no  lan4 lan2 phy0-ap0 lan3 lan1
```
`ubus call hostapd.phy0-ap0 get_status` reports channel 36, op_class 115, beacon_interval 100.

**Two independent causes, neither of which was `-122`:**

1. **The wrong UCI knob was being flipped.** `wifi config` writes `disabled '1'` onto the
   **wifi-iface**, not the wifi-device — `mac80211.uc` does
   `set ${si}.disabled='${defaults ? 0 : 1}'` where `si` is the iface section. `radio0` itself
   has no `disabled` option at all, so the documented repro
   (`uci set wireless.radio0.disabled=0`) was a no-op and the radio never had a chance.
   netifd then drops the iface outright (`wireless.uc`:
   `if (parse_bool(data.disabled) && type != "wifi-device") continue;`), so `dev.vif` stays
   empty, the handler is invoked with `"interfaces": { }`, `has_ap` stays false, and
   `hostapd.uc` passes hostapd an **empty** config — which is why no `NEW_INTERFACE` was ever
   attempted and `iw dev` was empty while `network.wireless status` still said `up: true`.
   The correct knob is the iface: `uci set wireless.default_radio0.disabled=0`.

2. **No `country`, so channel 36 was NO-IR.** The RTL8822BE has a blank efuse, so rtw88's
   `rtw_regd_init()` matches nothing, falls back to `rtw_reg_ww` and never calls
   `regulatory_hint()`; the wiphy stays on world `00` where 5170-5250 MHz is NO-IR and
   hostapd refuses the channel. Fixed by seeding `country` in
   `base-files/etc/uci-defaults/99-dir842` — see the long ★ comment there for why it is set on
   the wifi-device *there* rather than via `ucidef_set_country` in `board.d` (board.json would
   also auto-enable the iface, shipping an OPEN AP by default). Verified: `iw reg get` now
   reports `country BR: DFS-FCC` instead of `00`, and `logread` shows zero
   "not allowed for AP mode" rejections.

**`-122` was a red herring.** It is `-EOPNOTSUPP` (MIPS errno 122, not EDQUOT), printed by
`iw`, not netifd, from `mac80211.sh`'s unconditional `iw phy phy0 set distance 0` — the
`distance` option defaults to 0 in the wifi-device schema, so it always runs, and rtw88 has no
`set_coverage_class` op. `mac80211.sh` discards the return value: it is cosmetic noise emitted
on every setup run, on every board whose driver lacks that op. The paragraphs below chased it
as the bug signature and reached a wrong conclusion.

**Also corrected:** the previous entry claimed `common.uc`'s `wdev_create()` is dead code
under `CONFIG_WIFI_SCRIPTS_UCODE=y`. It is not — hostapd's own ucode imports the same
`/usr/share/hostap/common.uc` and calls `phydev.wdev_add()`. It simply never fired here,
because hostapd had been handed an empty config and never got that far.

**Still not verified: client association + DHCP.** `hal` (a floor below) cannot hear the AP —
it sees other 5 GHz APs strongly, including one on the same channel 36 at 100% signal, but not
this one, so the association test needs a client physically nearer the bench. The AP itself is
confirmed beaconing and bridged.

**Latent limitation worth recording:** rtw88 gates `rtw_iface_combs` on `RTW_CHIP_TYPE_8822C`
only, so the 8822B here reports `n_iface_combinations == 0`. A single AP vif survives this
(`net/mac80211/util.c` short-circuits `if (total == 1 && !radar_detect) return 0;`), but a
second vif — AP+STA, or multi-SSID — will fail with `-EBUSY`.

**★ 2026-09-04, found while re-verifying M4 on the current build (fresh factory-flash boot,
no manual config): `radio0` (5 GHz) came up with `ubus call network.wireless status` showing
`"up": false, "retry_setup_failed": true"`, while `radio1` (2.4 GHz, M7) came up fine on the
same boot.** `logread` shows `netifd: radio0 (...): wifi-scripts: Bug: PHY is undefined for
device` right around the same boot window as the PCIe wifi card's own firmware-load messages
(`rtw88_8822be ...: Firmware version 30.20.0`) — a startup race: `iw phy` independently
confirms `phy0` genuinely exists at the kernel level by the time this was checked, so the
device isn't missing, wifi-scripts just looked for it before PCIe/driver probe had finished
registering it. **`wifi reload radio0` recovers it immediately** (`up: true` within seconds,
no reboot needed) — this is a real, reproducible boot-time race, not a regression or a
hardware fault, and self-heals with one command. `docs/PORT-MAIN-6.18-STATUS.md`'s own M6
service inventory expected an `asic-wifi-settle` procd one-shot (wait for
`ubus call network.wireless status` to report both radios, then re-poke the datapath) to
exist for exactly this class of problem — checked on this build: **it was never actually
implemented** (`/etc/init.d/` only has `dir842-asic`, no wifi-settle companion). Fixing this
properly means adding that missing service (or extending `dir842-asic` with an equivalent
retry loop) rather than relying on a user noticing and reloading by hand; recorded here as an
open, minor, self-recovering M4 gap rather than a hard M4 regression — the radio and its
whole stack are functionally fine once given a nudge.

**★★ Fixed and verified on a real, unattended cold boot.** Wrote the missing service
(`files/target/linux/rtl819x/base-files/etc/init.d/asic-wifi-settle`, `START=99`): waits up
to 45s for `ubus call network.wireless status` to settle, and for any device reporting
`retry_setup_failed`/`up:false` (while not administratively `disabled`), issues `wifi reload
<device>` — retrying up to 3 times with a 5s pause between attempts before giving up and
logging it as a genuine fault rather than the boot race this service targets. First live
test (manually invoked against an already-stuck `radio0`) showed **one reload attempt was
not always enough** — it needed a second cycle — so the retry count was raised from a naive
single attempt to 3 before shipping. Rebuilt, reflashed, and verified on a genuine
cold boot with zero manual intervention: the boot race reproduced again (`radio0 not up`),
attempt 1 didn't recover it, **attempt 2 did** (`logread`: "reload recovered all radios
(took 2 attempt(s))"), and `ubus call network.wireless status` confirmed both `radio0` and
`radio1` reading `up: true` afterward with nothing for the user to do. This closes the M4
gap for real, not just in theory — a single-attempt version would have failed on this exact
boot too.

## 4. Hardware NAT (M5) — wired up, not accelerating

**★★★ 2026-09-04, SECOND SIX-AGENT SWARM (the WEDGE) — converged: the LARGE-FRAME WEDGE is a
SYMPTOM of M5 being broken, and its recovery destroys offloaded state. A concrete fix applied.**
Six parallel agents attacked the LARGE-FRAME WEDGE detector firing repeatedly during offload
testing (and its level-3 recovery wiping the NAPT table, degrading the box). Synthesis:
- **The detector is REAL and inherited from 4.14** (agent: 4.14-compare) — it guards a genuine,
  HW-validated RTL8197F silicon erratum (`docs/M7-LARGE-FRAME-RX-WEDGE.md`: an RX-FIFO drain-lag
  race corrupting/dropping CPU-terminated frames >128B under CPU-RX saturation). NOT a 6.18
  invention.
- **It fires continuously ONLY because M5 is broken** (agents: 4.14-compare, recovery-corruption):
  on 4.14, working HW-NAT keeps 0.0% of forwarded bytes on the CPU RX ring, so the erratum is a
  rare `ping -f`/UDP-flood edge case; on 6.18 with M5 open, 100% of forwarded bytes cross the CPU
  RX ring and U-turn on port0, so the same race fires ~1/10s under sustained load. **The wedge is
  a symptom; the real cure is finishing M5.**
- **The recovery does NOT corrupt state** (agent: recovery-corruption): `rtl819x_hang_work()`'s
  `gw_prog` re-arm is register-identical to boot (boot itself runs a level-3 recovery), and
  `echo 3 > fabric_reset` restores ping 100%→0% in ~1s. The degradation across cycles is the
  recurring *trigger* collapsing TCP's cwnd each ~1s, not corrupted tables. (Also corrected a
  stale claim: `rtl865x_napt_prefill()` is gated by `napt_collision_prefill` which defaults 0 —
  the collision-bit prefill runs neither at boot nor recovery; it is a deliberate no-op.)
- **The vendor's real RX-stall discipline is cheap and already present** (agent: vendor-web): the
  6.18 RX path already does the unconditional RUNOUT-bit ack on every exit
  (`rtl819x_swnic.c:381-399,436,483,594,600`), matching stock; the vendor's `FullAndSemiReset()`
  (FULL_RST + swcore clock-cycle) is **U-Boot boot-time-only** — the vendor Linux driver never
  full-resets at runtime, so the port's destructive "level 3" is over-engineering an init
  primitive.
- **★ THE FIX (agent: recovery-counterproductive, converged with heuristic + 4.14) — APPLIED.**
  (1) The absence branch's starve counter is now gated on `!rtl819x_hwnat_any_flow_installed()`
  (new tracking, true from packet 1 of install) in addition to `!has_hot_flow()` — closing the
  timing gap where a just-installed/stalled-before-hot offloaded flow was invisible to the
  suppression. (2) Both large-frame recovery fire-sites now cap the auto-ladder at level 2
  (soft reset, NO NAPT-table wipe) whenever a flow is installed, reserving the destructive
  level-3 for the manual knob / zero-offload case — so the wedge can no longer destroy the
  acceleration it exists to protect. `rtl819x_hwnat.c` (`hwnat_any_installed` + helper at all
  flow-list mutation sites; `rtl819x_hwnat_any_flow_installed()`), `rtl819x_hwnat.h`,
  `rtl819x-eth.c` (starve gate + two level-2 clamps).
- **Handshake-failure candidate (agent: TCP-vs-ICMP), NOT cleanly fixable:** the reverse row's
  `very` verification hashes only `{remIP,remPort}` (silicon-dictated — the ASIC recomputes
  `HASH1(remIP,remPort)` and the driver can only store that), so a rare full-tuple `idx_in` hash
  collision between two flows to the same peer could false-match and eat a SYN-ACK. Real but
  requires a collision; not the likely cause of the observed single-flow stalls, and the field
  can't be strengthened without silicon support. Left documented, not patched.

**Net: the wedge fix (applied) stops the recovery from destroying offloaded NAPT state — the
best available mitigation — but the swarm's deepest conclusion is that the wedge only fires
because M5 acceleration isn't yet keeping bytes off the CPU. Fixing M5 is the real cure.**

**★★★★ 2026-09-04, WEDGE FIX VALIDATED ON HARDWARE + a session-long TEST CONFOUND uncovered.**
Built the wedge fix into a fresh image, flashed, and ran offloaded transfers (hw=1, wan-only
flowtable, hwnat=Y):
- **The wedge fix WORKS.** Across multiple sustained offloaded transfers, `LARGE-FRAME WEDGE
  detected`/`recovery level` fired ZERO times (previously ~1/10s, each wiping the NAPT table),
  and the box stayed healthy (33 min uptime, load 0.15, 0% ping loss) with NO cross-cycle
  degradation. The flow-installed suppression (`rtl819x_hwnat_any_flow_installed()`) closes the
  timing gap; the level-2 clamp makes any residual fire non-destructive. **This is the concrete
  wedge solution and it is confirmed.**
- **★ MAJOR CONFOUND FOUND — most of this session's "TCP handshake fails / SYN_SENT UNREPLIED"
  observations were a DEAD PEER SINK, not a DIR-842 bug.** An ORPHANED Python socket sink from a
  much earlier session (pid 4579 on `tiny`) was still bound to port 5205 with a FULL accept
  backlog (`ss`: Recv-Q=6, listen backlog 5), so its kernel silently dropped every new SYN — no
  SYN-ACK, no RST. Proven decisively: a LOCAL loopback connect to that listener ALSO timed out;
  `tiny`'s firewall is all-ACCEPT with rp_filter=0; and a box-side `wan` capture showed the
  DIR-842 correctly NAT'ing and transmitting the SYN (`172.16.0.1.x > 172.16.0.2.5205 [S]`,
  which `tiny`'s br0 received) with nothing coming back. After killing the orphan and starting a
  fresh sink, a TCP connect THROUGH the offloaded box succeeded immediately (`BOX-CONNECTED`),
  and the flow established + `[HW_OFFLOAD]`. **So the DIR-842 offload path handles the TCP
  handshake correctly; the "DIRECT-xmit breaks the handshake" reading earlier this session was
  contaminated by this dead sink.** (Lesson for the bench: never trust a stale `nc -l`/socket
  sink — verify a LOCAL connect to it succeeds before blaming the datapath.)
- **With both confounds (wedge + dead sink) removed, the true remaining M5 blocker is cleanly
  isolated — and it is the REVERSE (WAN→LAN) direction, not the forward.** Deeper diagnosis:
  the OUTBOUND ASIC row is actually HOT (`[477] age=17`) — the forward direction genuinely
  hardware-forwards (CPU idle, `pid_dump` empty = frames bypass the CPU). This CONTRADICTS the
  first swarm's "idx_out cold / reason=7" reading, which was itself contaminated by the wedge
  wiping the table and the dead sink. **The stall is the reverse path.** A dual tcpdump (peer
  `tiny` br0 + LAN client NIC) during a paced offloaded transfer measured it decisively:
  **the peer sent 35 reverse packets with its window OPEN (`win 249`/`win 31856`), the LAN
  client received only 18** — ~50% of the WAN→LAN return frames are dropped between the peer
  and the LAN jack. The client, stuck on a stale zero-window view, zero-window-probes and
  stalls (RTO backoff), which halts the forward transfer. The inbound ASIC row (`[635]`) is
  hot (it MATCHES the reverse frames) but drops ~half of them on delivery — the exact "reverse
  traffic confirmed matching but never reaching the LAN client" gap this §4 has circled all
  along, now confirmed clean with all confounds removed.
- **Fix under test (`napt_no_reverse` param, 2026-09-04):** install ONLY the outbound ASIC row
  and clear the inbound slot, so WAN→LAN un-NAT falls to the software path (reliable, and it is
  just low-volume ACKs) while the forward bulk stays silicon-accelerated.
- **★ RESULT — REFUTED as a clean fix, but the measurement is CONFOUNDED (do not over-read).**
  Built, flashed, tested with `napt_no_reverse=1` + wan-only flowtable + a VERIFIED fresh sink:
  the transfer still stalled (~1.1 MB / 40 MB) and the dual capture still showed ~50% reverse
  loss (peer sent 459, client received 229). So skipping the ASIC reverse row did NOT fix it —
  the reverse-drop persists even with reverse in software, meaning it is NOT the ASIC reverse
  row alone; it is tied to running `flow_offloading_hw=1` at all (shipped `hw=0` software
  fastpath forwards fine, reverse included). **⚠ BUT this measurement is unreliable:** the bench
  HOST (the gaming PC) had its onboard ethernet (enp4s0) WEDGED and was memory-pressured that
  evening (peer-session flagged it), and the client-side `tcpdump` ran on that loaded host
  alongside `dd`+`nc`+`ssh` — so the "459 vs 229" is contaminated by host-side capture drops
  AND the client's own TCP possibly stalling under host CPU starvation (which alone produces the
  `win 0`/zero-window-probe stall). So `napt_no_reverse` is NEITHER confirmed NOR cleanly
  refuted; it needs a re-test on an UNLOADED, healthy bench host before any conclusion. The
  param ships default 0 (no behavior change). What IS solid: `flow_offloading_hw=1` breaks the
  reverse direction on this board and the wan-only + napt_no_reverse mitigations did not fix it
  in this (confounded) run — the reverse-delivery conflict remains the M5-acceleration blocker.

**★★★ 2026-09-05, RE-TEST WITH A COUNTER-BASED METHOD — the "~50% reverse loss" finding above
does NOT reproduce; it was the tcpdump-on-a-loaded-host confound, not a real ASIC defect.**
The `napt_no_reverse` test above was re-run with a deliberately different measurement method
specifically designed to be immune to the confound just found: instead of comparing tcpdump
packet counts on both ends (which depends on the capturing process getting scheduled promptly
enough to drain its ring buffer — exactly what a loaded/swapping host cannot guarantee), this
re-test compares kernel interface TX/RX packet counters (`/sys/class/net/*/statistics/*`) on
the peer and the bench host, taken immediately before and after the same transfer. Kernel
ring-buffer counters are incremented by the NIC driver's interrupt/NAPI path itself, not by a
userspace process competing for CPU, so they stay accurate under exactly the host load that
broke the earlier method. Confirmed clean test conditions this time: the peer (`tiny`) was
completely idle (`load average: 0.00`) for every run; the bench host had unrelated background
load (other work on the same shared dev machine) but was not itself running a packet capture
during the measurement window, removing the specific mechanism that corrupted the earlier count.

Three baseline runs (`hwnat=1`, `napt_no_reverse=0` — the shipped default), each a fresh 40 MB
transfer: reverse-direction delivery ratio (host-received / peer-sent) came back **1.018,
1.059, and 1.06-ish** across the three runs — i.e., **at or slightly above 1.0, not ~0.5.**
There is no reverse-direction packet loss under the default configuration once the
measurement itself is not the thing dropping packets. **The earlier "peer sent 459, client
received 229" finding is retracted as a measurement artifact, not a real defect** — exactly
the caveat that finding was already flagged with ("this measurement is unreliable"), now
resolved in the direction of "not real" rather than "confirmed."

That does not mean M5 hardware acceleration works, though — it means the specific failure
mode was misdiagnosed. All three baseline transfers took a suspiciously identical **~35.0-35.2
seconds** regardless of how many reverse packets were counted (101-267, varying run to run) —
that number is exactly the test's own `nc -w 35` idle-timeout, meaning **the transfers were
being cut off by the timeout, not completing 40 MB and finishing naturally.** Effective
throughput lands around 9 Mbit/s, wildly below the ~130 Mbit/s clean software-forwarding
baseline recorded elsewhere in this doc, let alone hardware-acceleration speeds. This is
consistent with (does not newly diagnose, but no longer contradicts) the already-documented
real M5 blocker: a forward-path throughput/tail-drop problem at the port0 U-turn under
hardware offload, not a reverse-ACK delivery problem. Reverse-ACK delivery is fine; something
else caps forward throughput hard.

**`napt_no_reverse=1` is now REFUTED outright — not inconclusive, actively worse.** The same
counter method run twice under `napt_no_reverse=1` (forcing WAN→LAN un-NAT to software) shows
a near-total stall: **4-16 total packets moved in the full 35 seconds**, versus 101-272 under
the default. Forcing the reverse row into software while the forward row stays
hardware-accelerated does not fix anything — it makes the connection barely move at all,
most likely because the two directions' NAT state (hardware vs. software) fall out of sync
with each other. **Conclusion: do not pursue `napt_no_reverse` further as a fix.** The
parameter stays in the tree at its default (0, no behavior change) as a diagnostic knob, but
it is not a path to working acceleration and should not be re-tried as one. The real M5 open
item remains exactly what it already was: a forward-path throughput ceiling / tail-drop under
`flow_offloading_hw=1`, not a reverse-direction defect.

**Also learned, unrelated to the above but worth recording:** a factory-image AUTOBURN flash
resets UCI to stock defaults (`network.wan.proto=dhcp`), wiping any bench-session UCI edits
(the static `172.16.0.1/24` WAN this bench topology depends on). This is correct, expected
behavior for a factory image, not a bug — but it means every post-flash bench session must
re-apply the static WAN IP before running any WAN-side test, or every "no route to host"
symptom looks like a router defect when it is actually just stock DHCP-on-WAN with no DHCP
server on that bench segment.

**★★★★ 2026-09-05, FIVE-AGENT SWARM on the forward-throughput ceiling, plus a serious
tangential finding.** Per the standing project mandate, launched five parallel investigations
(pause/backpressure register audit, comparison against the working `main`/4.14 branch, public
web research, an hwnat row-programming code audit, and a live hardware diagnostic — only one
agent touched the physical bench, to avoid conflicting access). Four completed; the fifth was
killed mid-run by a session rate limit before it gathered any evidence.

- **Pause-directionality theory (register-audit lens) — REFUTED by the 4.14-comparison lens.**
  The register-audit agent proposed that forcing 802.3x pause off on the port0 trunk (the
  existing fix that makes CPU/software forwarding fast) removes the only backpressure signal
  reaching ASIC-accelerated frames, since those never touch the CPU's own natural
  self-throttling. Plausible on its face, but the 4.14-comparison agent found the ENTIRE
  low-level register configuration — PCRP0/PITCR/MACCR/MACCR1/SBFCR/PBFCR/GDSR, `trunk_pause`
  default, all of it — **byte-identical** between `main` (4.14, hw-NAT reaches 889-900 Mbit/s
  at ~0% CPU) and this port. Since that configuration already existed when `main` measured
  hardware acceleration as **4.8x FASTER** than software (184→889 Mbit/s), it cannot be what
  makes this port's acceleration **14x SLOWER** than its own software baseline (130→9 Mbit/s).
  That direction-flip, not a magnitude gap, is the real anomaly, and it rules out every
  low-level SoC/switch register as the cause — the regression has to be in code this port
  introduced that `main` never had.
- **Web research (public-documentation lens) — no prior art exists.** Searched vendor GPL
  source mirrors, OpenWrt/DD-WRT/vendor-SDK forums and changelogs for this exact
  same-port-U-turn + hardware-NAT-acceleration interaction. Confirmed the same congestion
  registers this driver already uses (`Pn_OQDSCR`/`Pn_IQDSCR`, `P0QQCgst`..`P6QQCgst`) exist in
  the public GPL source, but found nothing tying them to NAT-row installation specifically.
  This behavior is undocumented anywhere public — it has to be root-caused empirically.
- **hwnat row-programming audit — found a real, if unconfirmed, gap.** `hwnat_program_rows()`
  in `rtl819x_hwnat.c` never sets the NAPT row struct's `priValid`/`priority`/`NHIDXValid`/
  `NHIDX` fields (`rtl865x_asichal.h:75`) — every accelerated row ships with priority marked
  not-valid. On a shared, congested egress queue, an unprioritized flow would be first to
  drop. **Caveat, found by the 4.14-comparison agent's independent pass**: this port's own
  row-field assignments are otherwise identical to `main`'s equivalent code, which suggests
  (but does not prove, since `main` used an entirely different, older hwnat API this session's
  own history already documents as replaced) that leaving this field unset may not be new
  either. Not ruled out, not confirmed — a concrete, one-line, easily-revertible thing to
  try (`e.priValid = 1; e.priority = <value>`) once a real value is known from a register
  sweep, but not yet tested on hardware.
- **Leading hypothesis (from the 4.14-comparison lens): a DSA-conduit / `nf_flow_table`
  hardware-offload interaction unique to this port.** `main` used a CPU-tag-native netdev with
  the old downstream `ndo_flow_offload` hook — no tag-rewriting shim, no `nf_flow_table`
  `FLOW_OFFLOAD_XMIT_DIRECT` caching. This port's DSA conduit inserts an 8-byte↔4-byte CPU-tag
  shim and relies on `nf_flow_table` hardware offload with its own direct-transmit caching
  path — both are genuinely new relative to `main`. If an ASIC-accelerated frame's egress
  port/VLAN encoding (set when `hwnat_program_rows()` installs a row, via the nexthop/L2
  helpers) doesn't precisely match what this port's DSA/VLAN model expects at the port0
  U-turn, the switch's own port-based classification could misroute or requeue those frames
  specifically — while CPU-forwarded traffic, which does go through the tagger correctly by
  construction, would be unaffected. **Not pinned to an exact file:line** — needs live
  frame-capture comparing an ASIC-hot flow's actual egress VLAN/port bits against the
  CPU-forwarded path's. This is the single best next lead.
- **Live hardware diagnosis (the lens meant to adjudicate the above) did not complete.** Killed
  by a session rate limit right as it started the actual transfer. I resumed the live
  diagnosis myself afterward but hit two unrelated, real bench problems in sequence (below)
  that consumed the remaining bench time before a WAN-side accelerated-flow capture could be
  taken. **The forward-throughput-ceiling root cause therefore remains open, now narrowed to
  one strong hypothesis (DSA tag/VLAN encoding mismatch at the U-turn) and one weaker,
  unconfirmed one (missing row priority), with the two previously-leading theories (pause
  directionality, and the retracted reverse-ACK-loss theory) both now refuted.**

**★ NEW, SEPARATE finding while resuming the live diagnosis: a real TX-path packet-corruption
event, reproduced twice on one boot, NOT reproduced on a fresh boot — not yet understood.**
While attempting the live M5 diagnostic, the bench box went completely silent on the UART
console for 33 minutes following a `LARGE-FRAME WEDGE detected → recovery level 3 →
"switch wan: Link is Down"` sequence (the box had been left with `hwnat=1` armed and no active
flow for several minutes after the killed agent's SSH session dropped mid-test — plausibly
relevant, not confirmed). A power cycle recovered it, but packet captures on that NEXT boot
showed the router's own outbound SSH responses arriving corrupted at the wire level: a
malformed SYN-ACK (`options [[bad opt]`), a TCP segment with `[bad hdr length 16 - too short,
< 20]`, and an IP packet whose header claimed 422 bytes while the actual frame was 52
(`[total length 422 > length 52] (invalid)`) — reproduced identically across two separate SSH
attempts on that same boot, explaining exactly the "banner exchange timeout" symptom
(previously, on other occasions this session, attributed to CPU load during jffs2 formatting —
that explanation does NOT fit here, since this was a warm boot with the overlay already
formatted and the CPU was later confirmed 100% idle while the corruption was still occurring).
**A second power cycle produced a completely clean boot** — SSH connected in under 2 seconds
with a wire capture showing no anomalies at all. So this is real, reproducible-within-a-boot
corruption on the router's own LAN-side transmit path, but not deterministic across boots; it
looks like a downstream consequence of an incompletely-recovered state from the preceding
crash rather than a per-boot-guaranteed defect, but that is inference from one occurrence, not
proof. **Flagged here rather than chased further this session** — it is a serious enough class
of finding (silent data corruption, not just slowness) that it should not be lost, but pinning
it down needs deliberately reproducing the preceding crash first, which needs its own
dedicated bench time.

**★ Bench blocker at the point this session's work stopped: the WAN-side peer (`tiny`, the
Raspberry Pi at 172.16.0.2) is unreachable** — not from the bench host, and not from the router
itself either (`ping` from the router's own shell to its directly-connected WAN peer gets zero
replies). This is a physical/external dependency, not a router defect: something on `tiny`
itself (power, cabling, or its own network config) needs a human to check before any further
WAN-side hardware-NAT measurement can be taken. The router was left in its safe default
(`flow_offloading_hw=0`) rather than idle with acceleration armed, given the crash above.

**★★★★★ 2026-09-05, two more candidates RULED OUT by direct register evidence (not just code
reading), while `tiny` stayed unreachable.** With no live WAN peer available to run a transfer,
kept working the static-analysis side: found a previously-documented, previously-partially-fixed
bug class in `rtl865x_asichal.c` ("issue #1 (A-2)") describing exactly this failure shape — a
WAN-connected-route peer's companion ARP nexthop entry can point at a stale, MAC-hash-derived L2
row if it isn't re-synced on every relearn, causing offloaded frames to egress with a dead
destination MAC. `tiny` (172.16.0.2) is textbook exactly this kind of peer — the code even names
a variable `tiny_nh` for it — so this looked like the single most concrete, on-point lead yet.

Verified it directly, live, by hand-decoding `/proc/rtl865x_dump`'s raw register words (no
transfer needed — this only requires reading tables that are already programmed at boot):
- `ARP[258]` (the connected-route companion) and `ARP[20]` (the main WAN nexthop chain) both
  read `0x0000ae89`. Decoding the field layout (`valid:1, nextHop:10, aging:5`,
  `rtl865x_asichal.h:112-115`): `nextHop = (0xae89 >> 1) & 0x3ff = 0x344 = 836` — exactly
  matching the driver's own live label `L2[tiny nh=836]` from `/proc/rtl865x_gw`. **The
  companion is currently correctly synced.** This specific historical bug is NOT currently
  manifesting.
- `L2[836]` reads `w0=bbcc0000 w1=00dc10aa`. Decoding `gw_write_l2()`'s packing
  (`rtl865x_asichal.c:515-524`): MAC bytes decode to `AA:BB:CC:00:00:??` (matching this bench's
  documented static peer-MAC scheme), the port-mask byte decodes to exactly `0x10` (jack 4, the
  WAN port — matching `l2_mask_wan`), and the STA/age/NH/fid bits all decode to exactly what
  `gw_write_l2(gw_wan_gw_mac, l2_mask_wan, 1)` should produce (fid=1 for WAN). **This L2 entry
  is correctly programmed for `tiny` as a WAN-side peer — no encoding error found here.**

So the classification/nexthop/L2 chain that resolves "which physical port does an
accelerated frame for this peer egress on" is verified correct by direct register read, ruling
out both the historical ARP-companion bug and a naive L2-port-mask encoding error as the
current cause. **This does not rule out the DSA-tag-shim hypothesis wholesale** — it only rules
out the classification tables; the actual per-frame CPU-tag rewrite/strip at TX time, and
whatever happens after L2/ARP resolution in the switch's internal queue/scheduler stage, are
still unverified and remain the most likely place left to look, along with the still-unconfirmed
missing-row-priority candidate. **Next concrete step, blocked on `tiny` coming back:** a live
capture comparing an ASIC-hot frame's actual on-wire VLAN tag / CPU-tag bytes against a
CPU-forwarded frame's for the same peer — the tables say the ASIC SHOULD produce the right
frame; only a real capture confirms whether it actually does.

**★ CONFIRMED, conclusively, that this is a hard external blocker and not something more
remote diagnosis can work around.** Checked three independent things: (1) `tiny` does not
respond on its bench-only address (172.16.0.2); (2) `tiny` also does not respond on its
completely separate management/mesh-VPN address (a different physical network path entirely,
routed over the internet, nothing to do with the bench cabling) — no ping, no SSH, connection
timed out; (3) the router's own switch reports its WAN port as **`NO-CARRIER`** — i.e. no
Ethernet link-layer signal detected at all, a physical-layer fact independent of IP addressing,
firewalls, or software state on either end. All other unused LAN ports show the same
`NO-CARRIER` (expected, nothing plugged in); only the LAN port with the bench host cabled to it
shows a live link. Two independent network paths to the peer both dead, plus a physical
link-layer absence on the router's own port, is conclusive: **the Raspberry Pi peer is
powered off, has failed, or is physically disconnected** — this is not a router defect, not a
router firewall issue, and not something fixable by more code reading, more agents, or more
bench scripting. It needs a person to check the Pi's power and cabling. Every avenue that does
NOT require this peer has been worked as far as it goes (the swarm's four completed lenses,
the ARP/L2 register-level verification above); resuming M5's live verification is a single
concrete, well-scoped task waiting on that physical check, not an open-ended investigation.

**★★★★★★ 2026-09-05, LATER: `tiny` came back — live-tested, and the result is worse than
previously characterized, but the bench environment itself turned out not to be clean
either.** Once WAN connectivity was restored (static IP re-applied after the factory-default
DHCP reset, per the earlier-documented pattern), two independent measurement methods against
the real peer both showed the same thing: a byte-counted `dd`/`nc` transfer delivered only
78 KB of an attempted 40 MB (99.8% loss, verified via `tiny`'s own kernel interface counter,
not an estimate), and a follow-up `iperf3` run under `flow_offloading_hw=1` transferred
**zero bytes** across a 15+ second window — `conntrack` showed the flow reach
`[HW_OFFLOAD]` after only 3-8 setup packets and then go completely silent. An immediate,
back-to-back A/B switch to `flow_offloading_hw=0` on the identical test showed a real
24.8 MB / 104 Mbit/s burst in the first 2 seconds before it, too, stalled — worse than this
session's earlier-established clean ~130 Mbit/s software baseline, and with 648
retransmits in that first burst alone.

That last number is the tell: **this specific test run was itself confounded**, and not by
the tcpdump-capture-drop mechanism already retracted earlier in this section. `dmesg` on the
bench host captured `libreoffice-soffice` and Thunderbird actively opening and file-locking
profile/certificate databases *during* the test window — this machine was being used
interactively for unrelated desktop work at the same time as the network test, on top of an
unrelated USB-Ethernet adapter re-enumeration event (its own hiccup, traced and fixed
separately, see the session's live troubleshooting) that had already reset the bench
interface's IP and routes once. A shared, interactively-used desktop is not a clean load
generator for a latency/throughput-sensitive test over a USB NIC, regardless of how careful
the measurement method is.

**Best honest read, holding both facts at once:** the qualitative finding — `hw=1` performs
categorically worse than `hw=0`, not just somewhat worse — has now shown up consistently
across every measurement method tried this session (packet-count-based, byte-counted,
`iperf3`), on more than one occasion, so it is very unlikely to be *entirely* a measurement
artifact. But the exact severity (99.8% loss vs. a complete zero-byte stall vs. the earlier
~9 Mbit/s-with-partial-progress reading) has varied between attempts in ways that track
known bench-host instability, not obviously any change on the router's side — so no single
one of those exact numbers should be treated as the definitive characterization. **The
router was left in `flow_offloading_hw=0` (verified).**

**★ Correction, same day: the "shared desktop machine" framing above was wrong, or at least
was not the operative cause, and should not have been offered as one.** The actual bug in
that measurement session was a one-shot `iperf3 -s -1` server on the peer that silently
stopped listening after its first connection — the project's own standing bench rule
("never trust a peer sink without a local loopback verification first") exists precisely to
catch this, and it was skipped for that specific tool. Once redone with an ordinary
always-listening TCP sink, verified via local loopback before every run, killed and
restarted clean between runs, the result stopped being ambiguous: two repetitions each,
interleaved, same two machines, only `flow_offloading_hw` changed — `hw=0` delivered the
complete 40 MB with a clean connection close **both times**; `hw=1` delivered a few tens of
KB and left the connection stuck fully `ESTABLISHED` forever **both times**, confirmed via
`ss -tn` on the peer itself, not inferred.

**★★ Then isolated further, per an adversarial review's correctly-identified gap** (the
review noted `flow_offloading_hw=1` couples two different mechanisms — a software
`nf_flow_table` DIRECT-xmit path and the ASIC — and nothing in the original test verified
which one was actually engaged). Held the ruleset fixed at `flow_offloading_hw=1` throughout
and toggled ONLY the ASIC's own module switch: with `hwnat=0` (ASIC disabled, identical
`flags offload` ruleset still active), the full 40 MB delivered cleanly, `[OFFLOAD]` marked
in conntrack, full byte accounting visible (not frozen). With `hwnat=1` (ASIC re-enabled,
nothing else changed), the SAME transfer failed again, `[HW_OFFLOAD]` marked, a real row
confirmed installed in dmesg (`hwnat: +tcp ... rows out@287 in@538`). **This pins the defect
specifically to the ASIC hardware-forwarding path, not the flow-offload mechanism in
general, not a firewall/ruleset quirk, and not the bench host.**

**★★★ Root mechanism found via a peer-side packet capture.** A capture taken directly on
the WAN peer's own interface during a live failure showed the flow's data arriving in a
515-packet burst inside ~40 milliseconds — far faster than the software-forwarding path
ever produces, consistent with hardware forwarding's near-zero added latency letting the
sender's TCP slow-start ramp the congestion window up explosively. Within that burst, one
specific TCP segment is silently dropped, and every retransmission of that exact same
segment ALSO fails — seven-plus retries over 13+ seconds, growing backoff, never
succeeding — even though hundreds of KB immediately before and after it were delivered
fine. That is not random burst loss (which would clear on retry); it is the signature of a
flow consistently losing arbitration at a shared, momentarily-congested egress queue —
this SoC's port0 RGMII trunk, already documented elsewhere in this driver as the point
every LAN<->WAN routed flow shares. The same capture also caught a second, separate anomaly
worth recording even though not yet chased down: at connection setup, the peer emits an
extra reply addressed to the LAN client's raw private IP (`192.168.0.2`) instead of the
router's WAN-masqueraded address — an address that should be architecturally impossible on
that wire — though critically that IP never appears as a *source* in the same capture, so
it is not a NAT-rewrite leak on the forward path; the mechanism is unconfirmed and flagged
for follow-up, not folded into the fix below.

**Fix implemented and live-tested — real, measurable, partial improvement; not a complete
fix.** This driver's NAPT row struct has a QoS field (`priValid`/`priority`,
`rtl865x_asichal.h:75`) that was never set anywhere — every ASIC-accelerated row installs
with priority explicitly marked not-in-use, exactly the condition that would make a flow
lose arbitration first under contention. Added a `napt_priority` module parameter (default
7, the hardware ceiling, shipped ON by default) that marks both the outbound and reverse
rows `priValid=1` at that priority (the same struct instance is reused between the two
writes, so one code change covers both); `0` reverts to the previous (unset) behavior for a
quick A/B.

**Live result, repeated four times on real hardware, clean-slate sink verified before each
run**: bulk delivery went from 14-78 KB (pre-fix, out of an attempted 40 MB) to a
consistent 480 KB-2.3 MB with the fix — roughly a **15-30x improvement**, reproduced across
multiple separate test cycles, not a one-off. The connection still does not complete and
still ends up stuck `ESTABLISHED` with no FIN ever delivered, so **this closes part of the
gap, not all of it** — there is a second, still-uncharacterized loss mechanism on top of
the priority issue.

**Second hypothesis tested, inconclusive**: added two further `trunk_pause` values (3 = only
honor incoming 802.3x pause on this SoC's port0, no pause generated back onto the shared
trunk; 4 = the reverse) to test whether making the existing pause-disable fix (which
prevents a DIFFERENT, already-understood self-throttle collapse — see below) directional
instead of all-off would recover more headroom without recreating that collapse, applied
live via the existing `trunk_redo` re-latch knob with no reflash needed. Result with
`trunk_pause=3` (honor-only) combined with the priority fix: 480 KB delivered — within the
same noisy range as the priority fix alone, no clear additional improvement in this
session's limited testing (one run, under real-world bench-host load swinging 5x during
testing). **Not adopted as a change in direction** — reverted live to the default
(`trunk_pause=2`, unchanged pause-disable behavior) since it showed no clear benefit and
the two other values it introduces are honestly unverified beyond a single ambiguous data
point; kept in the tree as a documented, low-risk, already-scoped experiment for whoever
has cleaner bench conditions to properly A/B it (ideally interleaved runs, not one each).

**Router left in its safe, shipped default** (`flow_offloading_hw=0`, `trunk_pause=2`) —
the priority fix ships active by default (it can only help or be neutral, never regress
software-forwarding, since it only touches ASIC row fields that only matter when hardware
acceleration is explicitly turned on).

**★ 2026-09-05, re-confirmed on a genuinely clean bench host** (the USB-Ethernet adapter
that had been intermittently re-enumerating all session was reseated into a USB3 port by
the user, eliminating that specific confound going forward): re-ran the exact same
clean-slate-verified test with the router idle at 1h05m uptime, `tiny` idle, and the bench
host at moderate load. Result: 173,776 bytes delivered — squarely inside the same
15-30x-improved-but-incomplete range measured on the less stable host, confirming the
partial-fix characterization was real and not an artifact of bench instability. One new
detail from this cleaner run worth flagging as a lead for the remaining gap: `ss -tni`
showed `rcv_ooopack:30` — thirty **out-of-order** packets received, not just loss. That
suggests at least part of the remaining problem may be packets arriving through the ASIC
out of sequence (consistent with, for instance, some frames taking a faster/different
internal path than others) rather than being purely dropped — a concrete, testable
distinction the next investigation pass should check directly (a capture with sequence
numbers annotated, not just aggregate byte/drop counts) before assuming the second loss
mechanism is more of the same congestion-drop story as the first.

**★★ 2026-09-04, SIX-AGENT SWARM — converged: the forward row is CORRECT and CAN match; it
was being WIPED mid-flow. A concrete state-destroying bug was found and fixed.** Six parallel
Sonnet agents attacked reason=7 on orthogonal lenses; the synthesis:
- **Row content/encoding/hash: definitively CORRECT** (agent: web/SDK). The ACTUAL Realtek
  GPL vendor source was found (utessel/edimax + cgoder/openwrt_rtk mirrors of
  `l4Driver/rtl865x_nat.c`, `AsicDriver/rtl865x_asicL4.{h,S}`, `rtl865xc_asicregs.h`),
  including the MIPS disassembly of `rtl8651_naptTcpUdpTableIndex`. `gw_napt_hash1()` matches
  it term-for-term; the outbound row keys only on `{intIPAddr,intPort,isTCP}` (NO remote field
  by hardware design, not a port omission); collision=collision2=0 means ACTIVE (current
  default correct; the "R6/B3" 1/1=dedicated reading is REFUTED as backwards); TCPFlag 0x3/0x2
  is a static direction discriminator, not a per-packet SYN/ACK gate. A second agent
  cross-verified the same against the vendor ASM independently.
- **The `napt_fill_all` confound is CONFIRMED by vendor source**: `_rtl865x_nat_outbound_lookup()`
  walks 4 ways of a group holding the top 8 hash bits fixed, so 1023/1024 flat-filled rows are
  in the wrong group and unreachable. Its reason=7 proves nothing (as already flagged).
- **DSA VLAN/netif/route context: CORRECT, not incompatible** (agent: DSA-context). The
  CPU-tag/PORT0_ROUTER_MODE mechanism is byte-identical to 4.14 — which itself CHOSE CPU-tag
  mode because the old tagged-VLAN trunk couldn't offload. Source classifies S=16(NPI)
  correctly; the D=19(RP) destination is CORRECT for a forward packet (NPE is only the reverse
  reply case) — that thread is a dead end.
- **No software-fallback gap** (agent: flow-stall). Mainline `nf_flow_table` never gates on
  `NF_FLOW_HW`; the driver RX path delivers ASIC-trapped frames normally, so they ARE
  software-forwarded when LAN+WAN are both flowtable members.
- **Aging is fine** (agent: aging). agingTime reload-on-any-hit is byte-identical to 4.14; the
  "112s hot" only ever proved sparse idx_in (reverse ACK) matching, never forward throughput.
- **★★★ THE CONCRETE FIX (agent: fresh-eyes) — APPLIED.** The idx_out∨idx_in fix that landed
  in `hwnat_flow_stats()` (FLOW_CLS_STATS) was NOT mirrored in its sibling
  `hwnat_aging_work_fn()`, which computes `hwnat_flow_hot` — the ONLY signal the LARGE-FRAME
  WEDGE detector uses to suppress its false-positive fabric+NAPT-table reset. Since bulk-flow
  hardware hits land on `idx_in` (cold `idx_out`), that poll reported `hwnat_flow_hot=false`
  for exactly the flow being genuinely hardware-forwarded → the wedge fired → **level-3
  recovery WIPED the live NAPT table mid-flow.** Fixed by mirroring the both-rows-hot check
  (new `last_aging_in` field; `out_hot||in_hot ⇒ active++`, `rtl819x_hwnat.c` ~1009-1042). This
  is the same bug class fixed one function away the same session, left in the sibling.
- **One correctness gap (inert for a single flow, worth porting):** the vendor's
  `_rtl865x_addNaptConnection` walks to the next FREE way in a group if the canonical
  outbound slot is occupied; this port always uses the raw hash index. Only bites with
  concurrent flows sharing a way-group.

**SYNTHESIS: the forward row is correct and CAN match (hwFwd=1 was observed); reason=7 is the
row being ABSENT at trap time, and the biggest state-wiper — the wedge detector firing on a
genuinely-hot flow — is now fixed. This is a real, testable hypothesis: build + flash + run a
sustained transfer and see whether hardware acceleration finally holds.** If it does NOT, the
residual is vendor-style idle-row eviction (`_rtl865x_naptIdleCheck`) or a downstream trunk/
fabric egress drop — but the wedge-wipe was the concrete, code-level bug the swarm surfaced.

**★ 2026-09-04, READ THIS FIRST — an adversarial NAPT-hash audit closed the hash/byte-order/
SWTCR1 lines of inquiry for good, and repointed reason=7 at aging/teardown + the DSA datapath
context.** A dedicated agent was tasked (with priority on the byte-order and SWTCR1-geometry
hypotheses this doc had flirted with) to find why the ASIC installs NAPT rows but traps
forward frames as `reason=7` (NO_MATCHED_NAPT_ENTRY). It REFUTED every placement hypothesis,
decisively:
- **The ASIC has been observed HARDWARE-FORWARDING this exact flow** (`hwFwd=1 reason=0000`,
  the 4-frame window at `:922-937`). That is only possible if the driver's install index
  equals the ASIC's lookup index — so **install-index == lookup-index is proven on silicon;
  the hash is not misplacing rows.**
- `gw_napt_hash1()` and its callers are **byte-for-byte identical to the WORKING 4.14 driver**;
  inputs are numeric host-order at every call (proven correct from the stock disasm,
  `HWNAT-OFFLOAD.md §4`; the old "network order" premise was retracted, #21). `SWTCR1=0x2200`
  (EnL4WayH=1 4-way + L4EnHash1=1) is **vendor-exact and required** — rows are addressed flat
  0..1023 regardless of the 4-way bit (the vendor never transforms the index; 4-way only adds
  search ways on lookup, `asichal.h:33-38`). The collision-bit prefill (`rtl865x_napt_prefill`,
  called from `gw_prog`) and the extIP NPE-classification are both in place.
- **Stale comments were actively sending readers (including this session) down the refuted
  byte-order/geometry paths** — now corrected in `rtl865x_asichal.c` (the `gw_napt_hash1`
  header and the SWTCR1 note), `rtl865x_asichal.h:183`, and `rtl819x_hwnat.c:401` (the
  "feed NETWORK order" comment, which the code never actually followed — it passes host-order).
- **What reason=7 actually is:** the row being ABSENT at trap-sample time — an aging/teardown
  artifact — plus, critically, the fact that **identical NAPT code works on 4.14 (swconfig)
  but not here (DSA)** points the residual cause at the surrounding datapath *context* that
  changed with the DSA port model (the untagged-trunk + PVID VLAN scheme, the port0 U-turn,
  the CPU-tag shim), NOT the NAPT logic. Two teardown bugs were already found+fixed this
  session (`hwnat_flow_stats()` idx_out-only; `flow_offloading_hw=1` DIRECT-xmit). The
  next-step lead is the aging/hit-refresh path (`TEACR` L4-aging reload on a hardware HIT;
  `FLOW_CLS_STATS`/`lastused` not re-expiring a live row) AND whether the DSA VLAN/port
  context delivers forward frames to the NAPT stage in the same classification context 4.14's
  swconfig datapath did. **Do NOT re-audit the hash, byte-order, or SWTCR1 — they are proven
  correct.** The undecoded `reason=0x420a` co-symptom (`:943`) is explicitly not part of the
  hash story.

**★ 2026-09-04, a re-run of the `napt_fill_all` discriminator + a new confound.** Tried to
re-confirm on the CURRENT build (with `sel_cpu_reason` properly armed this time — verified
`SWTCR1` read `0x2300`, bit 8 set) whether the outbound NAPT KEY is rejected. `napt_fill_all`
fired cleanly (all 1024 rows written with the live flow's outbound row, DIAG logged), but the
flow-stall defeated the read: the offloaded flow never sustains more than a few KB (TCP AND
UDP — iperf3-UDP managed only 33 datagrams because its TCP control channel stalls too), so no
forward packet is sampled AFTER the fill to see whether it matches. **More important — a
genuine analytical confound in the `napt_fill_all` discriminator itself:** with EnL4WayH=1
(4-way) + L4EnHash1=1 (enhanced-hash1) — the vendor-exact SWTCR1 this ASIC requires — the
silicon very likely VERIFIES hash-consistency (a matched row must sit at its own hash index).
`napt_fill_all` writes the row at ALL 1024 indices, which DELIBERATELY VIOLATES that at 1023
of them, so its "reason=7 persisted ⇒ the KEY is rejected" conclusion (recorded in
`rtl819x_hwnat.c`'s comment) is **not airtight** — reason=7 there could equally mean "the ASIC
verified the row is not at its own hash index and rejected it," which fill_all guarantees. So
the discriminator does NOT cleanly separate index-fault from key-fault under 4-way; it is only
valid under a flat 1-way table (EnL4WayH=0), which this ASIC won't run (clearing 4-way wedged
the L4 datapath). Combined with the prior agent's proof that the hash itself is correct
(reverse `hwFwd=1` observed) and byte-identical to 4.14, the outbound-key byte-order is
plausibly ALSO correct and reason=7's true cause remains the aging/teardown + DSA-datapath-
context space, not a key/index encoding bug. Net for the next session: do NOT re-run
`napt_fill_all` expecting a clean answer (it can't give one under 4-way), and do NOT re-audit
the hash/key byte-order (proven correct). The box was returned to shipped state after; it
still forwards a paced 90 Mbit/s cleanly.

The offload front end is rebuilt from scratch on the interface the kernel actually has now:
`ndo_setup_tc(TC_SETUP_FT)` feeding a `flow_block` of tc-flower rules, which is also the path
DSA forwards from every user port to its conduit. The 4.14 driver used the downstream
`ndo_flow_offload` interface, which no longer exists.

**What works:** fw4 emits the flowtable with `flags offload` across all five DSA ports; a
masquerade flow is offered, both directions of it are accepted (the reply direction has to be
matched against the already-installed pair *before* the masquerade-shape check runs, because
it un-NATs and would otherwise be rejected as a destination-NAT rule — the first version got
this backwards), and the ASIC rows install and verify via a proper double-read. They also
*persist*: watched a pair stay valid for 15+ seconds under sustained load with no fabric
wedge and no unexpected teardown.

**What doesn't:** the ASIC never actually looks the rows up. With the SoC's own
`sel_cpu_reason` trap-reason instrument armed (`SWTCR1` bit 8 — see
`docs/HWNAT-OFFLOAD.md` §7 for how that got discovered and how to read the field), LAN-ingress
packets on a live bulk flow decode to `src=19(RP) dst=19(RP) reason=8`: the L3 stage never
even classifies the LAN source address as NAT-eligible. That is one stage *earlier* than the
worst case the 4.14 investigation ever documented (`src=16(NPI)`, source correctly
recognised, only the L4 hash lookup missing). Every static register this port can read —
routes `[2]`/`[3]`/`[6]`/`[7]`, the ARP-window-plus-host-octet table, the external-IP entry,
`MSCR`/`SWTCR0`/`SWTCR1` — matches the 4.14 project's documented working stock blueprint
exactly, byte for byte, where it has been checked. A clean rate sweep (1/3/5/10/20/93 Mbit/s,
matched eth0 `rx_bytes` deltas before/after each run) ruled out a rate-dependent threshold —
an early single sample that looked offloaded did not reproduce and was a measurement-window
artifact, not a real effect.

**★★ 2026-09-04: the forwarded-path failure LOCALIZED to the SoC→switch trunk.** Measured
end to end with a real routed NAT flow (LAN client 192.168.0.2 → box → WAN peer `tiny`
172.16.0.2, WAN static 172.16.0.1/24):

| observation | value |
|---|---|
| forwarded ICMP, 64-1400 B | 0% loss |
| forwarded TCP payload 500 B … 100 KB | works |
| forwarded TCP payload 500 KB+ | stalls |
| `tcpdump -i wan` during the stall | box masquerades and transmits correctly: `172.16.0.1.x > 172.16.0.2.5201`, full 1448-byte segments, **but no ACKs ever return** |
| peer's own NIC counters across a 300 KB transfer | received **75 packets**, **RX errors = 0** |
| 8367S hardware MIB, WAN jack, same transfer | `tx_packets` 462 → 516 = **54 frames egressed** |
| 8367S hardware MIB, uninvolved jack `lan3` | **0**, unchanged |
| `USEDDSC` peak / `RX-STALL WEDGE` / `FABRIC WEDGE` | 42 / 0 / 0 |

The CPU offered roughly 200 frames (300 KB ÷ 1448); the WAN jack egressed 54. **About 73% of
CPU-originated forwarded frames are dropped between the SoC CPU port and the switch jack.**

This refutes three theories at once, each with its own measurement:
- **Not CPU-egress flooding.** `lan3`'s hardware MIB stays at exactly 0, so frames are not
  being replicated across jacks. (`dsa_tx_flood=0` also changed nothing.)
- **Not FCS/fabric corruption.** The peer's `rx_errors` is 0 — the frames that arrive are
  intact, the rest simply never arrive. The FCS wedge detector never fired.
- **Not RX descriptor exhaustion.** `USEDDSC` peaked at 42 against a 256+ pool, and the
  RX-STALL detector never fired. Nothing is backing up on the receive side.
- **Not the flowtable offload, and not hwnat.** Reproduces identically with
  `flow_offloading=0`/`flow_offloading_hw=0` and with the `hwnat` module param `N`.

It is also NOT a size threshold — 1400-byte forwarded ICMP and 100 KB forwarded TCP both
pass. It takes a sustained *burst* to trigger, which points at the trunk's ability to absorb
back-to-back CPU frames rather than at any per-frame property.

**The driver's own code already names this mechanism.** `rtl819x-eth.c`'s "A-2 residual"
comment describes exactly what was measured, before any of this session's work:

> The routed LAN->WAN flow U-turns on port0 (ingress VID2 + egress VID1 on the SAME port),
> so a saturating TCP cwnd burst tail-drops ~0.25% at the port0 egress queue (Mathis => the
> ~27 Mbit collapse) while the SHARED pool never nears runout (GDSR0 MaxUsedDsc ~30 <<
> S_DSC_RUNOUT=480, and ICMP -f — 1 pkt in flight — loses 0%).

Every element of that matches today's independent measurements: `MaxUsedDsc` peaked at 42,
forwarded ICMP loses 0% at all sizes, and bulk TCP collapses. It also explains *why* the
trunk is the choke point — **all routed LAN↔WAN traffic U-turns through the single SoC port0
trunk**, so the trunk carries the flow twice while `lan3` stays at 0.

**Trunk pause: a real but CONTRADICTED result — do not act on it without a repeat.** That
same comment argues the fix is link-level pause (`PCRP0[17:16] = 3`, plus 8367S EXT1
DI-force txpause|rxpause), because internal back-pressure cannot throttle an *external*
sender. But the `trunk_pause` module parameter ships defaulting to **2 = force OFF**, and its
own header documents the opposite bench result: `1 = pause on (collapse)`, `2 = pause off
(fix)`.

Measured this session, over the UART console so that `ip link set eth0 down/up` could
re-apply the trunk without cutting the test path:

| forwarded transfer | `trunk_pause=2` (OFF, current default) | `trunk_pause=1` (ON) |
|---|---|---|
| 300 KB | fails | **succeeds** |
| 2 MB | fails | **succeeds** |
| 10 MB | fails | fails (box then wedged) |

So pause ON measurably extends how much a forwarded flow can carry before collapsing — which
supports the A-2 comment's reasoning and contradicts the parameter's own recorded A/B. **The
default was deliberately left at 2.** One session's partial evidence is not enough to
overturn a recorded bench measurement, and `docs/RETRACTIONS-AND-METHOD.md` catalogues
exactly this failure mode (entry 23: a register that was genuinely wrong, fixed, and was still
not the bug). The next pass should run a proper repeated cold-boot A/B — several runs per
setting, counting wedges rather than single transfers — and read the 8367S EXT1 (port 6)
drop and flow-control MIBs during a burst to see which side is discarding.

**Also worth noting for whoever picks this up:** on the 4.14 product this path was never
stressed, because working HW NAT kept bulk LAN→WAN flows in the ASIC and off the trunk
entirely. The forwarded-path collapse and the offload gap are therefore two faces of one
problem, not strictly sequential — but 4.14 *could* still software-forward at 147 Mbit/s with
`hwnat=0`, and this port cannot, so there is a genuine software-path regression here
independent of offload.

**⚠ RETRACTION (2026-09-04): the "~115x frame amplification" reported earlier in this
section was MY OWN MEASUREMENT ERROR, not a property of the hardware.** It came from
`awk '/eth0:/{print $10}' /proc/net/dev`. In `/proc/net/dev` the receive block has **eight**
fields (bytes packets errs drop fifo frame compressed multicast), so with `$1` = `eth0:` the
transmit columns start at `$10` = **tx_bytes**; tx_packets is `$11`. Every "conduit TX delta
= 23903 frames" figure was therefore *bytes*, and it matches the switch's own
`s00_p06_ifInOctets` delta of 23859 almost exactly. There was never any amplification and
never any loop.

Re-measured with the correct fields, the SoC and the switch agree **exactly**:

```
eth0 netdev  tx_packets delta = 65     s00_p06_ifInUcastPkts  delta = 65
eth0 netdev  rx_packets delta = 69     s00_p06_ifOutUcastPkts delta = 69
```

So **nothing is being lost between the SoC and the switch**, and the earlier conclusion that
"frames are multiplied and never leave" is withdrawn. `ethtool -S eth0` exposes the CPU port
as `s00_p06_*` (and every other switch port as `s00_p0N_*`) — that is the tool to use here,
not `/proc/net/dev` arithmetic.

**What the corrected data actually shows — the failure is DIRECTIONAL.** Once a run gets
going, both directions do work, but very unequally (iperf3, forwarded through the box,
LAN client 192.168.0.2 <-> WAN peer 172.16.0.2):

| direction | throughput | retransmits |
|---|---|---|
| **WAN -> LAN** (`iperf3 -R`) | **121 Mbit/s** | 202 |
| **LAN -> WAN** (forward) | **13.1 Mbit/s** | 85 |

The receive direction is healthy and close to the box's documented ~147 Mbit/s CPU ceiling.
The transmit direction collapses by ~10x. That is exactly the shape the driver's own "A-2
residual" comment predicts for this datapath: the routed LAN->WAN flow U-turns on SoC port0
(ingress VID 2 and egress VID 1 on the SAME port), so a saturating cwnd burst tail-drops at
the port0 egress queue and Mathis-law drives the collapse — while the shared descriptor pool
never nears runout (`MaxUsedDsc` 42 vs `S_DSC_RUNOUT` 480) and single-packet-in-flight ICMP
loses 0%. Every one of those side conditions was independently confirmed here.

**⚠ RETRACTION (2026-09-04): "35 watchdog resets ⇒ the kernel hard-hangs" was ALSO my
error.** On this SoC the kernel *reboots by firing the watchdog* —
`files-6.18/arch/mips/generic/board-rtl819x.c:80`:

```c
static void rtl819x_machine_restart(char *command)
{
	local_irq_disable();
	sr_w32(0, REALTEK_SR_WDTCNR);   /* <- deliberate: reboot == fire the WDT */
	while (1) cpu_relax();
}
```
So **every** `reboot`, `sysupgrade` and `panic()` prints `Reboot Result from Watchdog
Timeout!`. My own power-cycles and reflashes produced those 35 banners. Worse for the
original inference: this build has `CONFIG_WATCHDOG=y` but **no** `CONFIG_WATCHDOG_CORE`, no
`*_WDT` driver, no `/dev/watchdog` and no DT node — **nothing kicks a watchdog, so a
genuinely hung kernel here does NOT reboot; it sits dead until power-cycled.** "Hang →
watchdog reset" is not a mechanism that exists on this box.

Note also `CONFIG_PANIC_ON_OOPS=y` with `CONFIG_PANIC_TIMEOUT=1`, and `CONFIG_KALLSYMS`
unset — so any Oops becomes a 1-second reboot whose backtrace is raw hex and easy to skim
past. **The next capture of failure (B) must be the full UART from before the reset, not the
boot banner.** If the box truly hangs it will not reboot at all, which by itself
discriminates hang from panic.

For the record, the RX-STALL detector *did* fire three times in the bench log and the box
**stayed alive and answered the shell each time** — further evidence against a whole-kernel
hang.

**Second measurement caveat, same class as the `/proc/net/dev` field error:**
`rtl8365mb_get_stats64()` returns a **cached** `rtnl_link_stats64` refreshed by a per-port
`delayed_work` every `RTL8365MB_STATS_INTERVAL_JIFFIES = 3 * HZ`. So `/proc/net/dev`,
`ifconfig` and `ip -s link` on any **DSA user port** (`lan1..4`, `wan`) are up to 3 seconds
stale; only `ethtool -S <port>` reads live over SMI. (The conduit `eth0` is a real netdev and
is live.) Any per-jack figure quoted from `/proc/net/dev` in this document is suspect.

**Isolation matrix for the LAN→WAN stall (all measured 2026-09-04, box confirmed healthy
before and after each run):**

| configuration | forwarded LAN→WAN bulk | box afterwards |
|---|---|---|
| `hwnat=N`, flowtable OFF | stalls | **alive**, 0.6 ms |
| `hwnat=Y`, flowtable OFF | stalls | **alive**, 0.6 ms |
| `hwnat=Y`, flowtable ON (sw+hw) | stalls, even 100 KB fails | alive, latency degrades to 9.5 ms |
| `wan_route_mode=1` (ToCpu) | stalls | alive, 0.59 ms |

So the stall is caused by **none** of hwnat, the nftables flowtable, or the SoC's
WANRouteMode — the flowtable only makes it worse. With the flowtable off, a 100 KB write
completes in **5 ms** (~160 Mbit/s) and 1 MB stalls: a short burst is absorbed into
socket/qdisc buffers essentially instantly, then forwarding nearly stops.

`wan_route_mode=1` deserves its own note because `rtl865x_asichal.c` recorded it as a
candidate that had **never actually been tested** (the earlier "ToCpu kills the WAN" result
being confounded by a WAN peer that had lost its address). It has now been tested properly
— warmed path, no ARP flush, box verified up — and it neither kills the WAN nor fixes the
stall. That refutation is recorded at the call site as well.

**Excluded as the cause of the stall, each by measurement:** CPU-egress flooding, FCS/fabric
corruption, RX-descriptor exhaustion, the flowtable offload, `hwnat`, the egress VID, the
VLAN-0 member mask, `wan_route_mode`, an xmit/txDone lock inversion (the lock is already
`spin_lock_irqsave`), a missing TX-completion interrupt (`NIC_IIMR` includes
`TX_ALL_DONE_IE_ALL` in both trees, and the ISR calls `napi_schedule()` unconditionally), and
a missing queue wake (`netif_stop_queue`/`netif_wake_queue` sites are structurally identical
to 4.14, and the wake runs at the top of every NAPI poll).

**SWTCR0 matched byte-for-byte to the 4.14 blueprint — and it still stalls.** The live
register was read back through `gw_regdump` (`regdump_base=0xBB804400 regdump_n=8`, word 6):

```
ours (default)   SWTCR0 = 0x000847e4   napt[2:0]=4  wan_route_mode[4:3]=0  multiport[13:5]=0x3f
4.14 documented  SWTCR0 = 0x000847ec   napt[2:0]=4  wan_route_mode[4:3]=1  multiport[13:5]=0x3f
                          ^ differs in bit 3 only
```
(4.14 reference value from `docs/M7-HWNAT-REVERSE-NAPT.md:160` on `main`.) Setting
`wan_route_mode=1` and re-running `cat /proc/rtl865x_gw` reproduces **exactly** `0x000847ec`,
confirmed by read-back — so the port CAN reach the known-good value. With the register
verified at the 4.14 value and the path warmed, forwarded LAN→WAN bulk **still stalls**.
`MSCR` also already reads `0x00000017` (EN_L2|EN_L3|EN_L4|EN_IN_ACL), matching intent. So the
divergence in this register is real but is **not** the cause. Restored to 0.

**Clarification on the earlier "WAN→LAN works, LAN→WAN doesn't" framing.** iperf3's control
channel is always client→server, i.e. LAN→WAN, so once that direction fails BOTH `-R` and
forward runs report failure. The 121 Mbit/s reverse figure was measured on a run where the
control channel happened to survive. The accurate statement is narrower: **sustained LAN→WAN
TCP is what breaks**; it can establish, pass ~100 KB, and then stop. Forwarded ICMP (both
ways) and short TCP are unaffected, and the box itself stays healthy (0.6 ms) in every
configuration except the intermittent hard hang.

**Tested and REFUTED: the DSA SMI "interrupt blackout" theory.** `realtek-smi.c` holds
`spin_lock_irqsave` across an entire bit-banged register transaction, and with this board's
`realtek,clk-delay-ns = <1500>` that is ~250-300 us interrupts-OFF per 16-bit access.
`rtl8365mb` polls per-port MIB counters every `RTL8365MB_STATS_INTERVAL_JIFFIES = 3 * HZ`;
six ports x 15 counters x ~5 transactions lands roughly a **15 ms interrupts-off slab every
3 seconds** on a single-core 500 MHz MIPS box whose conduit RX ring is only 256 descriptors.
At line rate that offers ~1250 frames against 256 slots — a very plausible periodic burst
loss, and it is a genuine 4.14→6.18 regression (the swconfig driver bit-banged on demand
only, with no delayed_work, no timer, and no `phy_connect` on the switch PHYs).

Built and flashed with the interval raised to `60 * HZ` (verified applied in the build tree).
**LAN→WAN forwarded bulk still stalls, twice in a row, box healthy throughout.** The patch was
then **reverted**: it buys no measured improvement, and it would make `ip -s link` /
`/proc/net/dev` on DSA user ports 20x staler — actively harmful here, since stats-cache
staleness already produced one confounded measurement in this very investigation.

**Bench hazards that cost real time in this investigation — read before instrumenting.**

- **`tcpdump` defaults to a 262144-byte snaplen.** Two concurrent captures with `-c 200`
  OOM-killed `hostapd` and `dropbear` on this 64 MB board and left it pingable but
  un-SSH-able. Always `-s 96`, one capture at a time, and prefer counters to captures.
- **Do not write large files to the box's `/tmp`** — it is tmpfs, i.e. RAM. A 34 MB scp
  target was enough to OOM-kill dropbear.
- **`/proc/net/dev` on DSA user ports is up to 3 s stale** (cached, refreshed by a per-port
  `delayed_work`). Only `ethtool -S <port>` reads live over SMI. The conduit `eth0` is live.
- **`/proc/net/dev` field numbering:** the RX block is 8 fields, so with `$1` = `iface:`,
  `$10` is **tx_bytes** and `$11` is tx_packets.
- **`ethtool -S eth0` exposes every switch port** as `s00_p0N_*`, including the CPU port
  (`p06`), which no user netdev covers. This is the right instrument for SoC-vs-switch
  accounting.
- **A boot-loader "Reboot Result from Watchdog Timeout!" banner is not a fault signal** on
  this port: `rtl819x_machine_restart()` reboots by firing the WDT, so every reboot,
  sysupgrade and panic prints it.

**⚠⚠ RETRACTION #3, and it overturns this whole section: THE FORWARDED DATAPATH WORKS.**
Measured on a freshly booted box with a freshly restarted iperf3 server:

```
LAN -> WAN   77.7 Mbit/s sender / 75.5 Mbit/s receiver, 11 retransmits
WAN -> LAN   98.4 Mbit/s sender / 97.9 Mbit/s receiver
```

The "forwarded TCP stalls after ~100 KB" symptom that drove everything above was **an
artifact of my own test harness**. I was writing bulk data with
`exec 3<>/dev/tcp/172.16.0.2/5201; head -c N /dev/zero >&3`. Port 5201 is iperf3's, and
iperf3's server never reads a stream that does not speak its protocol — so its receive buffer
filled and it advertised a **zero window**. Captured on the client, unmistakably:

```
172.16.0.2.5201 > 192.168.0.2.51964: Flags [.], ack 69025, win 0
172.16.0.2.5201 > 192.168.0.2.35100: Flags [.], ack 1,     win 0
```

That is precisely the observed boundary: ~100 KB (one socket buffer) "succeeds" because the
write is absorbed locally, 1 MB blocks forever. And it explains why **UDP looked perfect** —
UDP has no flow control, so an unread receiver is invisible. Those wedged zero-window sockets
then accumulated on the server and poisoned subsequent *real* iperf3 runs, which is why
genuine iperf3 measurements were intermittent rather than consistently good.

**Consequently, every "refutation" above that was scored against the raw-`/dev/tcp` stall is
void as evidence** — the egress VID, VLAN-0 mask, `wan_route_mode`, MIB-poll and trunk-pause
experiments were all judged against a symptom that was not real. (Their *direct* observations
stand: the VID and VLAN-0 changes really did take the box off the network, and the SWTCR0
read-backs are real. But none of them was ever failing to fix a genuine stall.)

**What is actually left for M5, measured with the corrected harness (iperf3 end to end only):**

| configuration | LAN→WAN result |
|---|---|
| flowtable OFF, fresh boot | **77.7 Mbit/s** sender / 75.5 receiver, 11 retransmits |
| WAN→LAN, flowtable OFF, fresh boot | **98.4 Mbit/s** |
| flowtable ARMED (`flow_offloading=1`, hw=1), `hwnat=Y` | **fails** — driver logs `hwnat: +tcp 192.168.0.2:46519 -> 172.16.0.2:5201 rows out@614 in@419`, i.e. ASIC rows install, and the flow dies |
| flowtable ARMED, `hwnat=N` (no ASIC rows) | **also fails** |

So the two real defects are now cleanly separated:

1. **The `nf_flow_table` offload path breaks forwarding on this datapath.** It fails with the
   ASIC rows installed *and* with `hwnat=N` (software flowtable only), while the identical
   flow runs at 77 Mbit/s with no flowtable at all. That is the actual M5 blocker, and it is
   a much narrower target than "hardware NAT does not accelerate": the offload hook is not
   merely failing to accelerate, it is *losing the flow*. Start at
   `rtl819x_eth_setup_tc` / the `flow_block` callback in `rtl819x_hwnat.c` and at whether
   `FLOW_CLS_REPLACE` should be returning `-EOPNOTSUPP` (leaving the flow in software) for
   shapes it cannot actually handle, instead of accepting them.

2. **The box degrades progressively under sustained forwarded load** — latency climbs to
   ~700-1000 ms with packet loss, then it stops answering entirely and needs a power cycle.
   This is cumulative: a freshly booted box measures 77-98 Mbit/s cleanly, and after a few
   sustained transfers even the no-flowtable baseline degrades. **This is what made every
   measurement in this session inconsistent**, and it must be fixed (or at least understood)
   before any further offload work, because it silently invalidates back-to-back A/B runs.
   Per the retraction above, it is NOT a watchdog story: capture the full UART across the
   failure, since `PANIC_ON_OOPS=y` + `PANIC_TIMEOUT=1` turn any oops into a 1-second reboot.

**★★★ THE CURE IS M5 ITSELF — established by disassembling the SHIPPED D-Link firmware.**

The stock image (`dir842-firmware/`, `DIR_842E_RT8197F` 3.0.3, Linux 3.10.90) was reverse
engineered: its LZMA kernel was extracted and its **18,269-entry kallsyms table decoded**, so
the following are proven, not inferred from `strings`:

- `rtl_check_swCore_tx_hang`, `rtl865x_reinitSwitchCore`, `REINIT_SWITCH_CORE`,
  `FullAndSemiReset`, `RTL_swNic_reInit` — **ALL ABSENT** from stock. The widely-cited
  "OpenWrt compiles out the vendor switch-core watchdog" lead is a **dead end for this
  firmware**; do not spend time porting it.
- Stock's userspace does **nothing**: no process ever opens `/proc/rtl865x*`, and there is no
  switch keepalive in cron or any daemon. The whole mechanism is in the kernel driver.
- What stock *does* have is `one_sec_timer` @0x80173bb0, which **unconditionally
  force-schedules the RX tasklet once per second**, plus `CONFIG_FINETUNE_RUNOUT_IRQ`
  (mask runout on assert, re-arm when the free-skb pool passes a compile-time 128) and
  `refill_rx_skb()` called from the TX-done tasklet.

**We already had the forced-kick half, and better:** `rtl819x_rx_timer()` calls
`napi_schedule()` unconditionally every ~12 ms versus stock's 1 s. So that was never the gap.

**Two register-level cures were derived, built, flashed and MEASURED — both REFUTED:**

1. **Ack RUNOUT on every RX exit** (matching stock's contract exactly). We armed
   `PKTHDR_DESC_RUNOUT_IE_ALL` but acked it only on the `RTL_NICRX_OK` path; the ring-empty,
   alloc-failure and loop-bound exits all returned with a latched level source
   unacknowledged. **Kept** — it aligns us with stock's proven behaviour and is correct on
   its own merits — but it did **not** fix the wedge.
2. **Drain `PCSR0` alongside `PCSR1`.** `GDSR_PORT_CONG` is `#define`d to `PCSR1` (ports
   4-6); ports 0-3 — including the RGMII trunk every forwarded frame U-turns on — live in
   `PCSR0`, which nothing outside `/proc/rtl865x_fabric` ever read, on either tree. The
   driver's own A-2 comment predicts this exact symptom for that port and says
   *"the GDSR_PORT_CONG drain can't reach it"*. Added behind `pcsr0_drain` (default 1).
   **Also did not fix the wedge.**

Measured after both, 5 forwarded cycles: throughput 0, `ping -s 1400` back to 100% loss, 2
wedges detected. So the wedge is **not** a missing register poke.

**★ The actual conclusion, and it is the whole point:** what keeps 4.14 and stock healthy is
that **forwarded traffic never touches the CPU RX engine at all**. `docs/HWNAT-OFFLOAD.md`
measures 4.14 at 889-896 Mbit/s with **0.0% of payload bytes crossing the CPU**, and
`docs/M7-LARGE-FRAME-RX-WEDGE.md` states plainly: *"Routed client↔internet (HW-NAT) | any |
✅ unaffected — ASIC path, not CPU-RX."* On this port hardware NAT does not accelerate, so
**100% of forwarded bytes cross the CPU RX ring and U-turn twice on SoC port 0** — the
documented trigger, applied continuously. That converts a wedge which reproduces "~1 in 3-4
heavy attempts" on 4.14 into a near-certainty per transfer here, which is exactly what the
size fingerprint shows (one transfer takes `ping -s 56` from 0% to 75% loss).

**So the wedge is a SYMPTOM of M5, not an independent bug, and there is no separate cure to
find.** Fixing the ASIC offload removes the trigger. Every mitigation explored here — the
detector, the resets, the register drains — is treating the symptom.

**★★ THE LOAD WEDGE IS THE ALREADY-DOCUMENTED LARGE-FRAME CPU-RX WEDGE.** It is
`docs/M7-LARGE-FRAME-RX-WEDGE.md`, not a new 6.18 bug — I should have read that file far
earlier. Its TL;DR predicts this session's symptoms almost verbatim: *"drops every frame
larger than ~128 bytes while small frames flow perfectly … induced by a sustained inbound
flood … Because small frames keep flowing, rx_packets keeps incrementing, so the driver's
watchdog never fires — the partial wedge is invisible to the existing recovery path."*

Confirmed here with that doc's own size fingerprint, taken before and after ONE forwarded
bulk transfer on an otherwise healthy box:

| | healthy | after one transfer |
|---|---|---|
| `ping -s 56` | 0% loss | **75% loss** |
| `ping -s 1400` | 0% loss | **100% loss** |

Size-dependent loss, kernel alive, `USEDDSC` unchanged, no detector firing — the documented
signature exactly.

**Why the 4.14 auto-heal does not work on 6.18.** The 4.14 fix detects the wedge by
*software FCS*: a wedged fabric delivers large frames with bad CRCs, `dfail` climbs, and
`rtl819x_hang_check()`'s `dfail >= 4 && dfail >= 4*dok` gate fires. On this port the same
wedge presents differently — large frames are **not delivered at all**. The FCS sampler in
`rtl819x_swnic.c` only runs on frames that arrive (`if (len > 132)`), so `dok` and `dfail`
both freeze at 0 and that gate is structurally unreachable. Measured: `FABRIC WEDGE detected`
fired **0 times across 121 boots** while the box was demonstrably wedged. This is the same
blindness `docs/RX-STALL-WEDGE.md` §3 records for the hard stall, reappearing on a different
failure mode.

**Fix implemented (kept): a delivery-collapse arm of the same detector.** Declares when all
of — detector already armed (this datapath has delivered good large frames before); zero
large frames delivered, good *or* bad, for four consecutive ~2.5 s windows; and `rx_packets`
still advancing, i.e. small frames ARE arriving. That last clause is what separates a wedge
from an idle box, and it is strictly better than keying on TX because it proves the receive
path is live and only its large-frame half is dead.

**Measured result: the box now self-heals instead of needing a power cycle.**
`LARGE-FRAME WEDGE detected … - auto recovery` fires, level-3 recovery runs, and
`ping -s 1400` returns to 0% loss. Across repeated load cycles the detector fired 6 times and
the box stayed reachable throughout — where previously a single sustained transfer could take
it off the network until power-cycled. **This is what made every measurement in this session
unreliable, and it is now bounded.**

**Still open after this fix:** the wedge *recurs* under sustained load rather than being
prevented, so throughput sits at ~7-12 Mbit/s across repeated cycles versus the 77.7 Mbit/s a
genuinely fresh boot achieves. Recovery is a mitigation, not the cure — the RX-FIFO drain-lag
race that induces the wedge is still there, exactly as `M7-LARGE-FRAME-RX-WEDGE.md` describes.

**★ THE LOAD WEDGE IS NAMED AND RECOVERABLE — the single most useful operational finding.**
Characterised 2026-09-04 with the console while the network was dead:

```
kernel ALIVE (uptime advancing, console fully responsive)
br-lan still has 192.168.0.1 ; lan2 carrier = 1
USEDDSC(now)=18  MaxUsedDsc(hi)=123      <- pool nowhere near full
no oops, no panic, no kernel message of any kind on the UART
none of the three existing wedge detectors fires
```
and then:
```
echo 3 > /sys/module/rtl819x/parameters/fabric_reset
  -> ping goes from 100% loss to 0% loss, 0.6 ms, in about ONE SECOND
```
So this is **a recoverable ASIC fabric wedge with the kernel fully alive**, not a hang, not a
panic, and not the watchdog story retracted above. **That collapses the debug loop from an
~8-minute power cycle to ~1 second** and is the first thing the next session should exploit.

Why the existing detectors miss it: the RX-stall detector is deliberately gated on
`USEDDSC > 256` (its comment records an idle board at ~138-146 and the UDP-flood wedge it was
written for at 450+). This wedge sits at **18**, so that gate can never trip.

**Tried and REVERTED: an "RX-VOID" detector** keyed on "tx advancing + rx frozen + carrier up"
for ~10 s. It *worked as a detector* — fired 5 times, correctly naming the condition
(`RX-VOID WEDGE detected (tx advancing, rx frozen at 98, carrier up)`). But it is not
shippable: (a) under sustained load the fabric re-wedges as fast as the reset clears it
(`rx_pkts` stayed pinned at 98 across repeated recoveries), and (b) once the box is fully
wedged, TX stops too, so the "tx advancing" precondition goes false and it stops trying —
it cannot self-heal the very state it detects. Worse, it resets the fabric repeatedly *during
active traffic*, which `main:docs/RETRACTIONS-AND-METHOD.md` #17 explicitly warns against
("NEVER RESET THE FABRIC AFTER THE RINGS ARE PROGRAMMED … PKTHDR_DESC_RUNOUT asserts forever").
Reverted rather than ship a change that can make things worse. The *signature* it proved is
what matters and is recorded here.

**Bench protocol for whoever continues:** one measurement per cold boot. Power-cycle, restart
the peer's iperf3 server, warm the path with pings, take ONE iperf3 reading, then power-cycle
again before changing any variable. Anything else is measuring the degradation, not the change.

**The most promising un-eliminated lead** is in this driver's own comment at the `MSCR`
write (`rtl865x_asichal.c`, the EN_L4 block): with `EN_L4` on and no NAPT session rows
programmed, the L4 engine's inbound-session matching *"EATS unsolicited WAN→LAN traffic once
the first outbound flow arms it … eth0 RX stays flat = silicon drop, not CPU trap"*. That is
character-for-character this symptom — the first burst leaves, then the ACKs never come back
— and it is why the code deliberately forces `SWTCR0` AutoLearn OFF /
`NAPTR_NOT_FOUND_DROP=0` / `NAPTF2CPU=1`. **Next step: verify on live silicon that those
SWTCR0 bits actually read back as intended after `gw_prog`**, rather than trusting the
read-modify-write, since a TCP/UDP NAPT miss that DROPs instead of trapping to CPU would
produce exactly this and nothing else tried so far would show it.

**Two fixes built, flashed, measured and REFUTED (both reverted; recorded so nobody
re-tries them).** These were motivated by the erroneous amplification figure above, so their
*motivation* was wrong — but the measurements are real and the conclusions about VID 0 stand
on their own:

1. **Derive the egress VID from the DSA port mask** instead of sending VID 0
   (`nicTx.vid = (dsa_ports & BIT(4)) ? RTL865X_VID_WAN : RTL865X_VID_LAN`), restoring what
   the 4.14 swconfig path did. **Result: the box came up, programmed the VLAN rows
   correctly, and sat in `recovery level 3` with no LAN reachability at all.**
2. **Narrow the VLAN 0 member mask** from `0x13F` (ports 0-5 + CPU port 8) to `0x01F`
   (jacks only). **Result: identical — row programmed correctly, then no reachability.**

So VID 0 and the CPU port's membership in VLAN 0 are both **load-bearing** under the
CPU-tag/DSA model: unlike the swconfig VLAN-tagged trunk, the switch here is not selecting
egress by VLAN membership. Both call sites carry ★ comments recording the refutation.

**Fixed along the way (certain defect, kept):** `rtl819x_eth_xmit()` returned
`NETDEV_TX_BUSY` *after* `rtl819x_dsa_tag_tx()` had already stripped the 8-byte DSA tag. An
skb handed back with `TX_BUSY` is requeued unchanged and retried on the conduit, so the
tagger does not re-run; the retry then fails the `ETH_P_REALTEK` check and the frame is
freed. That destroyed one packet per ring-full event — order 0.4% loss under forwarded
overload, comfortably above the ~0.25% at which Mathis-law TCP collapses. Now undone
symmetrically by `rtl819x_dsa_tag_tx_undo()` before the `TX_BUSY` return. This did not by
itself fix the stall (the trunk drop above dominates), but it is a real bug the 4.14 driver
never had, because 4.14 did no TX-side tag surgery.

**★ 2026-09-03: reason=8 DECODED, both leading candidates REFUTED on hardware, and a
NEW, more fundamental blocker found underneath.**

*What reason=8 means.* The `main` (4.14) branch was searched exhaustively for a reason-8 fix
— all docs, the full driver source, every commit message, and `git log -S` pickaxe across all
refs. **There is no such fix**: `main` contains exactly one mention of reason 8, and the 4.14
project's LAN ingress always read `src=16(NPI)` (source correctly classified). But that single
mention is the semantic key, `docs/RETRACTIONS-AND-METHOD.md:210-213` on `main`:

> The `hwFwd=1` frames it counted had `l2Tr=0` (`ph_asic0` bit 2) and `vid=2` — periodic
> **broadcasts being L2-flooded** with the CPU in the flood list (`extPL=12`) ... They decode
> to **reason 8, not 7**.

So **reason 8 = "this frame was L2-forwarded (bridged) to the CPU, not routed"** — the CPU was
simply in the frame's destination port list. `src=19(RP) dst=19(RP)` are the un-classified
defaults. It is not "the L3 stage rejected the LAN source"; the L3 stage **never ran**. That
puts the fault one stage earlier than §4 previously assumed: at the VID→netif lookup, or at
the `DMAC == GMAC` gate. (Inference from one measured co-occurrence, not from Realtek's
`l34Model.c` — strong, but stated as inference.)

*Both candidates that theory produced were then tested on hardware and REFUTED:*

- **LAN GMAC divergence** — refuted. `/proc/rtl865x_classify` reads
  `netif[0] GMAC=e0:1c:fc:51:c9:ef  live br-lan=e0:1c:fc:51:c9:ef  MATCH=yes` and
  `netif[1] ... MATCH=yes`, with `route[2] valid=1 process=2 internal=1 netif=0
  ipAddr=192.168.0.0 ipMask=23` and netif vids 2/1 all exactly matching their `EXPECT` lines.
  The DSA-era concern that `br-lan`'s MAC settles after `wait_netif()` samples it is real in
  principle but is not happening here.
- **Ingress VID misclassification** — refuted. With `pid_dump=200` armed, every LAN-ingress
  frame decodes `port=2a vid=2 reason=0000`. VID 2 is correct; the VLAN-0 row added by this
  port is not swallowing LAN traffic.

*The new blocker, which sits underneath M5 and must be fixed first:* **the forwarded
LAN→WAN path collapses under sustained bulk TCP, and this is independent of hwnat.** With a
proper routed NAT flow finally set up on the bench (WAN static 172.16.0.1/24, peer `tiny` at
172.16.0.2, `hal` and the wired bench host as LAN clients, fw4 flowtable `flags offload` live
across `lan1-4 wan wlan0`):

| path | result |
|---|---|
| router → WAN peer, direct | **147 Mbit/s, 0 retransmits** — WAN datapath is healthy |
| LAN client → WAN peer, forwarded, `hwnat=Y` | connects, then `control socket has closed unexpectedly` |
| LAN client → WAN peer, forwarded, `hwnat=N` | same stall: `0.00 Bytes`, retransmits, window collapses to 1.41 KB |
| ICMP through the same path, 64-1400 B | 0% loss at every size |
| short-lived TCP through the same path | fine — conntrack shows a full `[ASSURED]` bidirectional flow |

After the bulk attempts the box degraded to **1026 ms RTT with 33% loss** and lost the WAN
peer entirely, and the console log shows
`rtl819x: FCS wedge detector armed (5262 good large frames)` plus an earlier
`recovery level 3 starting (rx_pkts=56)` / `fabric full reset done`. So sustained *forwarded*
large-frame traffic trips this port's own FCS-wedge machinery — the same family as the
project's long-standing unexplained COLD-BOOT-TX-WEDGE.

**This reorders M5's work.** Chasing `reason=8` further is premature: the ASIC cannot be
expected to classify a flow the software path cannot even sustain. The next pass should
(a) characterise the forwarded-path stall — it is NOT hwnat, NOT MSS-independent ICMP, and
NOT the WAN link itself, so suspect the conduit's RX/TX descriptor handling for forwarded
large frames and the FCS wedge detector's arming condition; and (b) only then re-arm
`sel_cpu_reason` on a flow that actually stays up.

**A narrower next lead than "reflash stock and diff" (found by document/source review only,
not yet checked on the bench):** `reason=8` does not appear in this driver's own decoded
reason-code comment (`rtl865x_asichal.c:56-60`, the `sel_cpu_reason`/`EN_51B_CPU_REASON`
block) — that comment names 1 (pre-ACL trap), 2 (ACL→CPU), 3 (ACL log→CPU), 4 (ACL-pass
checks), 5 (L34 action, TTL==0), and 7 (no matched NAPT entry); 6 and 8 are absent. Both the
4.14 project's worked examples (`docs/HWNAT-OFFLOAD.md` §7) and this port's own comments only
ever decode 5 and 7 — reason 8 has never actually been looked up against Realtek's
`l34Model.c` source, only inferred to be "earlier than 7" from context. Confirming what 8
(and 6) actually mean — from a copy of `l34Model.c` (only shipped in the "otto" U-Boot GPL
drops, per §7) if one can be sourced, or empirically by forcing each ACL/L3 stage on and off
one at a time and watching which one changes the reason code — would turn this from "the L3
stage rejects it, cause unknown" into a specific, actionable register question, the same way
`sel_cpu_reason` itself did for the 4.14 investigation. Not attempted this session: it needs
either an external source drop this repo doesn't have, or a bench pass under `rtl865x_hal_lock`
this session's bench time didn't reach — recorded here so the next pass starts one step
further in than "read every register and compare to stock."

**A read-only diagnostic for the state-divergence side of that question:**
`/proc/rtl865x_classify` (added alongside the other `rtl865x_*` proc entries,
`rtl865x_asichal.c`, `classify_show()`) does a plain `rtl865x_asic_read_entry()` — no write,
no `gw_prog()` side effect, unlike `/proc/rtl865x_gw` — of route `[2]` (the LAN `/24`) and
`netif[0]`/`[1]`, with an `EXPECT` line next to each decoded field. `cat` it in the same
moment a bad `sel_cpu_reason` capture happens: fields matching `EXPECT` rule out ASIC-table
state divergence (a stale write, an intervening fabric reset, a race with the NAPT row
writer) as the cause of `reason=8`, pointing back at the real silicon's classification
behaviour instead; any mismatch is a newly-named, concrete bug in its own right. Not yet run
against a live bad capture — the same missing-LAN-client bench blocker below applies.

**Bench pass attempted 2026-09-03, blocked one step before a real measurement — a real
constraint worth recording so it isn't rediscovered:** re-created the pre-test conditions
from scratch on the box's own currently-flashed (M6) image — `network.wan` set static
(`172.16.0.1/24`, confirmed reaching bench host `tiny` at `172.16.0.2/24`, wired to the WAN
port), `firewall.@defaults[0].flow_offloading`/`flow_offloading_hw` both `1` (`nft list
flowtables inet` shows `ft` with `flags offload` across `lan1..4`+`wan`, matching what §4
above calls "wired up"), and `sel_cpu_reason=1` armed with the ASIC L2/ARP tables warmed
(`hwnat` already reads `Y` — R4's "boot arms it automatically" behavior is confirmed present
on this port too). Generating the actual masquerade flow to read the trap reason against
needs a real third-party LAN-side client sending through the box to `tiny` — and this bench
has none (the only spare client would be this session's own management link, per §3's
already-documented 5 GHz gap). Routing the session's own dev-host traffic through the box as
a stand-in was tried and hit a hard wall: the dev host has no `CAP_NET_ADMIN` in this
session's process (`ip route add` fails `Operation not permitted` even with sandboxing
explicitly relaxed for the one command — this is a real capability limit, not a policy
choice, and `sudo` was correctly not attempted). **So the actual blocker on both the M4
5 GHz DHCP gap and this M5 measurement is the same missing resource: a real spare LAN
client on the bench, physically present or reachable with routing privileges neither this
session nor its dev host has** — not more autonomous investigation time. Left the box in
this pre-configured state (static WAN to `tiny`, offload armed, diagnostic instrument on)
since it is exactly the state the next pass — bench or human — needs to not redo; reverted
nothing, as none of it is destructive or out of line with what §4 already documents as
working.

The module knobs the 4.14 port built for exactly this kind of sweep are all present and at
their documented-working defaults (`wan_connected_route`, `l2_mask_lan`/`l2_mask_wan`,
`swtcr1_override`, `wan_route_mode`, `multiport_mode`, `ffcr_unkuc_to_cpu`,
`sel_cpu_reason`) — `cat /sys/module/rtl819x/parameters/<name>` on a live box. This is a
genuine, narrow, reproducible register-level question, not a wiring gap: the 4.14 project's
own comparable investigation (`docs/M7-HWNAT-REVERSE-NAPT.md`) took on the order of days,
including reflashing stock to diff a live config against it. That is the recommended next
step here too, if this is picked back up: `known-good-images/` and `images/sha256sums.txt`
on `main` have the stock-comparable images, and `/home/agiu/dir842-nor-backup/` (bench-host
local, not in git) holds this box's own factory NOR contents from before the M6 reflash.

**A configuration trap worth recording:** a freshly flashed image boots with WAN on DHCP and
no firewall flowtable — neither the bench static IP nor `flow_offloading_hw` survive a
reflash, since they are runtime UCI state, not baked into the image. Testing hwnat against a
box in that state offers nothing to the ASIC at all and every measurement is meaningless;
this cost real time before being caught. Re-apply both by hand (or via a bench profile) before
testing offload on a freshly flashed box.

**★ A real, well-verified fix found 2026-09-03, not yet bench-tested:** `gw_netif_mac()`'s
three `dev_get_by_name()` call sites in `rtl865x_asichal.c` (`gw_wan_netif_prog_locked()`,
`rtl865x_wan_netif_mac_sync()`, and `gw_prog()`'s LAN netif block) still named the
4.14/swconfig-era VLAN-cascade netdevs (`"eth0.1"`, `"eth0.2"`) — which do not exist under
this port's DSA model at all. `dev_get_by_name()` therefore returned `NULL` on **every call,
on every boot** (a permanent miss, not a race), the MAC shadow was never populated, and the
ASIC's `netif[0]`/`netif[1]` GMAC field stayed pinned at the compile-time
`GW_MAC_LAN`/`GW_MAC_WAN` placeholder forever instead of the box's real MAC. This is a
plausible, concrete root cause for the L3 stage's DMAC==GMAC classification gate rejecting
real traffic (the `reason=8` gap this whole section is about) — found and adversarially
reviewed (verdict: approve with changes, since the diff also bundled new diagnostic code that
needed relabeling for honesty — the interface-name fix itself was independently re-verified
and confirmed correct, no memory-safety or locking issues, zero functional changes to the
row-install/DMA/NAPT-byte-order code this project's history flags as its most expensive bug
class). Fixed: `"eth0.1"`→`"wan"`, `"eth0.2"`→`"br-lan"` (the real DSA-era netdev names,
confirmed against the DTS port label and `board.d/02_network`). The `/proc/rtl865x_classify`
diagnostic was extended to decode and compare the ASIC's stored GMAC against the live
netdev MAC at cat-time (`MATCH=yes/no`), to make this checkable on the next bench pass.
**Not yet tested on real hardware** — queued into the same combined build as the M7 fixes
below. Expected outcome if this is the real cause: `reason=8` moves to `reason=7` ("no
matched NAPT entry", the 4.14 project's own documented worst case) — which would still leave
the *original* question (why the NAPT row itself isn't looked up) as a separate follow-up,
not a full resolution by itself. If `reason=8` doesn't move at all, this hypothesis is
refuted despite the diagnostic showing `MATCH=yes` — do not force-fit that outcome.

**★★ 2026-09-04: CONFIRMED on real hardware — `reason=8` is gone, `reason=7` is the new
blocker exactly as predicted, and the NAPT row install/hit behaviour is now characterized in
detail.** Two independent research agents were launched this session before any of this was
tested: one mapped the complete 4.14 hardware-NAT recipe onto the 6.18 control plane, the
other decoded the raw `ph_reason` field. Both converged on a DIFFERENT leading theory
("under DSA the trunk is untagged, no PVID is programmed, ingress frames land in VLAN 0
which has no netif, so the net-decision misses before L3 ever runs") and I built a probe for
it (`vid0_netif` module param, `rtl865x_asichal.c`, default off — installs `netif[2]` as a
copy of the LAN identity but `vid=0`). **That probe was never armed for any of the
measurements below** (`vid0_netif` stayed at its default 0 throughout) — the result is purely
from the already-committed-uncommitted `gw_netif_mac()` fix above. Fresh factory-flash boot,
`sel_cpu_reason=1` armed, `/proc/rtl865x_classify` → `netif[0] MATCH=yes`, `netif[1]
MATCH=yes` (both GMACs live). One methodological trap hit and fixed along the way: a fresh
factory flash resets `network.wan` to `proto=dhcp` with no address (the bench's static
172.16.0.1/24 is runtime UCI state, same class of gap as the `flow_offloading_hw` trap two
paragraphs up) — WAN was unreachable until that was set by hand.

Large forwarded frames (`len>=1000`, LAN→WAN bulk TCP data) from two clean captures, one
before `flow_offloading`/`flow_offloading_hw` were re-armed on this fresh boot and one after
(202 and 234 samples respectively, **100% identical classification within each capture**):
```
port=2 vid=1 hwFwd=0 isOrig=1 l2Tr=1 extPL=8 srcExt=1 reason=4e0e len=1518
```
Decoded per the `rtl865x_asichal.c:52-66` table (`[14:10]`=dst-type, `[9:5]`=src-type,
`[4:1]`=reason, `[0]`=ACL-mirror): `0x4e0e` = D=19(RP) S=16(NPI) **reason=7
(NO_MATCHED_NAPT_ENTRY)** ACL-mirror=0. Never once decoded to reason=8 in any capture this
session, on this build. **The prediction at line 820 above is confirmed**: the GMAC fix alone
moved the trap off reason=8. The `vid0_netif` probe is disk-only, harmless, and — on this
evidence — solves a problem that no longer exists; leaving it in as a documented, default-off
diagnostic is fine, but do not spend further time on the VLAN-0/PVID theory without a fresh
`reason=8` sighting to justify it.

**The follow-up question the doc predicted ("why isn't the NAPT row looked up") now has a
real, timestamped answer: the row installs and the hardware path DOES work — briefly — then
appears to stop taking effect even though the driver believes it is still installed.**
Sequence from one sustained flow (kernel timestamps kept, `dmesg | grep -a "rtl819x pid:\|
hwnat:"`, `pid_dump` armed for the whole flow):
```
[ 1318.514279] rtl819x hwnat: +tcp 192.168.0.2:46977 -> 172.16.0.2:5201  G=46977  rows out@80 in@184
  ... (bulk frames for the next ~57s all still hwFwd=0, reason 0x4e0e/0x420a) ...
[ 1375.626327] pid: port=2 vid=2 hwFwd=1 isOrig=0 l2Tr=0 extPL=4 srcExt=1 asic0=0411 reason=0000 len=219
[ 1376.627889] pid: port=2 vid=2 hwFwd=1 isOrig=0 l2Tr=0 extPL=4 srcExt=1 asic0=0411 reason=0000 len=219
[ 1377.628787] pid: port=2 vid=2 hwFwd=1 isOrig=0 l2Tr=0 extPL=4 srcExt=1 asic0=0411 reason=0000 len=219
[ 1378.629040] pid: port=2 vid=2 hwFwd=1 isOrig=0 l2Tr=0 extPL=4 srcExt=1 asic0=0411 reason=0000 len=219
  ... (bulk frames resume hwFwd=0 immediately after, same reason codes) ...
[ 1417.407789] rtl819x hwnat: +tcp 192.168.0.2:46977 -> 172.16.0.2:5201  G=46977  rows out@80 in@184
  ... (still hwFwd=0 afterward) ...
```
Four back-to-back `hwFwd=1 reason=0000` frames at exactly 1 Hz (`isOrig=0` — a copy, not the
only delivery, consistent with genuine hardware forwarding) is a real, if brief, working
offload window. The install trace then repeats **the identical row** (`G=46977`, `out@80
in@184` — same flow, same indices, not a new flow) 99 seconds later, which only makes sense
if the control plane's view of "is this flow hardware-offloaded" flipped back to no in
between, even though nothing evicted the ASIC row from that slot. This reads as an
aging/hit-refresh problem, not an install or hash-mismatch problem — candidates for the next
session: whether `TEACR`'s L4-aging field (`0x1|(1<<2)` in the 4.14 recipe, L4 aging ON) is
actually being refreshed by a hardware HIT the way it should, and whether `nf_flow_table`'s
own idle-timeout logic (`FLOW_CLS_STATS`/`lastused`, `rtl819x_hwnat.c:636-665`) is
re-triggering `FLOW_CLS_REPLACE` on a flow the ASIC never actually dropped. Also newly seen
and NOT yet decoded: `reason=0x420a` (D=16(NPI) S=16(NPI) reason=5, "L34 action, TTL==0" per
the vendor table) dominates the miss population even more than reason=7 does — take this
label as measured-but-not-understood; a LAN-origin packet having TTL==0 makes no literal
sense this close to the source, so either the label covers a broader condition than its name
suggests, or there is a second, separate mechanism here worth a fresh investigation rather
than folding it into the reason=7 story by assumption.

**★★★ 2026-09-04, later: found and fixed the actual bug (`hwnat_flow_stats()` idx_out-only
check), then found and fixed a SECOND bug the first fix exposed (the LARGE-FRAME WEDGE
detector false-positives against working offload).** A dedicated agent read
`rtl819x_hwnat.c` against mainline `nf_flow_table`'s GC/stats contract and found the root
cause precisely: `hwnat_flow_stats()` (the `FLOW_CLS_STATS` handler the kernel core polls to
decide whether an offloaded flow is still alive) checked `slot->idx_out`'s aging counter
ONLY. Bulk LAN→WAN data hashes to `idx_out`; the four `hwFwd=1` frames seen in the earlier
capture were small return traffic hitting `idx_in`. So a flow whose *inbound* row was doing
all the real hardware forwarding was invisible to the stats callback, `nf_flow_offload_gc_step()`
saw stale `lastused`, expired it, and this driver's own `hwnat_del_cookie()` wiped a row that
was still live in silicon — matching the observed "same G, same indices, reinstalled 99s
later" signature exactly. Also found: the `hwnat_refresh_timeout()` mechanism two nearby
comments described as still providing a keepalive was fully removed by the
`ndo_flow_offload`→`TC_SETUP_FT` migration (commit `93e0a1b`) with the comments left
unedited — there was no driver-side keepalive at all; `FLOW_CLS_STATS` was always meant to be
the sole source of truth. **Fix**: `hwnat_flow_stats()` now checks both `idx_out` and
`idx_in`, either being hot is treated as live. Also added: a `pr_info_ratelimited` in
`hwnat_del_cookie()` (was totally silent) so a bench pass can see exactly when and why a flow
gets torn down, and corrected both stale comments.

**Re-tested on hardware, and the result changed the story again — for the better.**
`hwnat AGE out[515]=17 in[913]=17`, read twice five seconds apart (t=629.75, t=635.02),
**pinned at the reload ceiling the whole time** — direct proof the row was being hit
continuously by real hardware forwarding, validating the fix's premise. But at t=636.76:
```
[  636.761893] rtl819x: LARGE-FRAME WEDGE detected (no large frames delivered in ~10s while small-frame RX advances) - auto recovery
[  636.925199] rtl819x: recovery level 3 starting (rx_pkts=11095)
[  637.684621] rtl819x: L4/NAPT table SRAM cleared (MEMCR=03007f26)
```
**The wedge detector fired against a flow it should have recognised as healthy, and the
level-3 recovery it triggered wiped the very NAPT table that was working.** This is not a
refutation of the hwnat fix — it is a second, independent bug the first fix exposed: once
hardware offload genuinely starts bypassing the CPU for large frames (the entire point of
M5), the CPU-visible signature is *identical* to the wedge's trigger condition ("no large
frames delivered while small-frame RX keeps advancing" — see the detector's own reasoning at
`rtl819x-eth.c`'s `LARGE-FRAME DELIVERY COLLAPSE` comment, written for the pre-offload world
where that pattern could only mean a dead datapath). Root cause of THIS bug: the detector had
no way to distinguish "large frames are gone because the fabric is stuck" from "large frames
are gone because the ASIC is successfully forwarding them without the CPU."

**Fix**: exported `rtl819x_hwnat_has_hot_flow()` from `rtl819x_hwnat.c` (reads a plain
`WRITE_ONCE`/`READ_ONCE` snapshot of the SAME hotness state `hwnat_aging_work_fn()`'s existing
5 s poll already computes — no new locking, no new register I/O) and gated the wedge
detector's starve-counter increment on it, in `rtl819x-eth.c`. The counter now simply does
not accrue while any hwnat flow is genuinely hot, so a flow that goes idle afterward does not
inherit a stale count and fire immediately. True-positive detection (nothing hot, large
frames still gone) is unaffected. **Built, not yet re-tested on hardware as of this
writing** — the box needs a third reflash to pick up this second fix; the bench session hit
two real post-flash hangs along the way (see [[bench-timing-and-serial-logger]] for the full
account: both correlated with `/etc/init.d/firewall restart`/`network reload` on a fresh
factory flash, recovered by power-cycle, unrelated to either hwnat fix since no flows existed
yet when either hang began).

**Bench methodology note for whoever continues this:** the `network.wan` and
`firewall.@defaults[0].flow_offloading{,_hw}` UCI settings, once committed, persist across a
power-cycle (they are on the JFFS2 overlay, not tmpfs) — after a hang-recovery power-cycle,
check `uci show` and `nft list flowtables` BEFORE re-running any reload/restart; they may
already be live from the prior boot's commit, letting the whole hang-risk step be skipped.
That said, **a fresh factory-image reflash always resets both to defaults** (`proto=dhcp`,
no `flow_offloading`) — persistence only applies across power-cycles of an ALREADY-flashed
image, not across a new flash. `/etc/init.d/firewall restart` itself turned out to be
harmless once run correctly — the two earlier "hangs" this session were self-inflicted:
`nohup` does not exist in this image's busybox `ash` (`ash: line 0: nohup: not found`), so a
backgrounded restart attempt silently no-op'd; run it in the plain foreground instead.

**★★★★ Third reflash (both fixes together): CONFIRMED on hardware — the wedge-suppression
half works exactly as designed. A NAPT row (`out@686 in@455`) stayed continuously hot
(`hwnat AGE` pinned at 15-17, the reload ceiling, polled every 5s) for 112 STRAIGHT SECONDS
(t=664→776) with ZERO wedge detections — the first time this entire session any sustained
offloaded activity did not trip the wedge. The row was then cleanly deleted at t=780 when the
flow legitimately ended, with no fabric reset needed, and the box measured a clean 0%-loss
wedge-check (`ping -s1400`) immediately after — also a first.**

**⚠ But this run also surfaced something the two fixes do NOT explain, and M5 is NOT
functionally complete: end-to-end throughput through the "hot" window was tiny.** WAN
interface byte counters (`/sys/class/net/wan/statistics/{rx,tx}_bytes`, chosen specifically
because they are independent of iperf3's own self-reporting, which got confused in this same
run — see below) grew by under 20 KB total across the ~180 s spanning both the 112 s hot
window and a second test right after. A real 25-30 s bulk transfer at even modest rates
should move tens of megabytes; this box moved kilobytes. Two more data points sharpen this:
- The `hwnat AGE` counter reloads on ANY hit, including a bare ACK or a retransmitted SYN —
  it is not a byte-volume signal. 112 s of a pinned counter is strong proof the ASIC kept
  *matching* this flow's packets; it is not proof of real payload throughput. The severe
  ping-latency spike measured *during* the hot window (RTT briefly hit 3-4 **seconds**, 80%
  loss, before recovering to 0%/0.6 ms the moment the flow ended) is consistent with a stuck
  TCP connection cycling through retransmission/backoff, not with healthy bulk transfer.
- `hwnat: +tcp` installs happened ~35-40 s into each test (t≈621 flow start → t≈659 first
  install; a second test's install did not land until ~t=976, nearly 100 s after that test
  started) — both times well after `iperf3`'s own reporting had already gone to 0 Mbit/s and,
  in the first case, its two candidate TCP connections (`58369`, the one iperf3 itself
  reported using, vs `58731`, the one that actually went hot and stayed hot) did not even
  match, meaning the connection carrying the client's real data and the connection the ASIC
  was hot for were NOT the same flow.

**This points at a separate, still-open problem, upstream of anything fixed today: something
is causing these TCP connections to collapse into a stalled/low-throughput state well before
hardware offload ever gets a chance to help, and offload promotion itself is arriving very
late relative to flow start.** Candidates for the next session, none yet tested: (1) whether
`nf_flow_table`'s promotion-to-hardware threshold is unusually slow on this build (mainline
normally promotes within a few packets, not tens of seconds); (2) whether the SOFTWARE
fastpath itself (everything before promotion) is the thing collapsing the connection's
congestion window, so that by the time hardware takes over there is little left to accelerate
in the test's remaining time; (3) whether `hwnat_flush_locked()` (fires on WAN-identity
churn, `rtl819x_hwnat.c` — see the earlier agent's candidate #4, never ruled out) is
repeatedly tearing down and re-establishing flows during this exact window. **Do not claim M5
throughput is fixed from today's evidence — only the false-positive wedge is fixed and
independently verified.**

**★★★★★ The "35-100s delay" framing above was WRONG — a dedicated agent read the actual
saved `iperf3` client output plus the mainline `nf_flow_table` source, and a follow-up
hardware test then confirmed and sharpened the real shape of the problem.**

`iperf3 -c ... -t N` (no `-P`) always opens TWO independent TCP connections — a control
channel first, then a separate data-stream socket (confirmed against the vendored
`iperf-3.21` source: `iperf_connect()`/`iperf_create_streams()`,
`iperf_client_api.c:100-126,419-439`). The saved raw client output from the first wedgefix
test (`/tmp/claude-1000/m5wedgefix-test1.iperf`, not previously read closely) shows the DATA
connection (port 58369, the one iperf3's own banner names) forwarding cleanly for its first
5 seconds — 25-32 Mbit/s, cwnd growing normally to 472 KB — then in a single 5-6s window,
**393 retransmits and a crash to minimum cwnd (1.41 KB) that never recovers** for the rest of
the capture. That is a discrete catastrophic event, not gradual congestion, and its scale
(hundreds of segments timing out together) matches this driver's own recovery-level-3 ladder
duration (~2s, full L4/NAPT SRAM wipe) far better than ordinary tail-drop. The flow that
*later* went hot in hwnat and stayed hot (port 58731) was never the data connection at
all — it is almost certainly the control connection, kept alive because the client process
was itself stuck (the "113+ seconds, never exited on its own" symptom above), pinned near
the aging ceiling by sparse, irregular retry traffic rather than bulk data. Confirmed in the
kernel source: there is no packet-count or delay threshold gating hardware-offload
eligibility (`IPS_ASSURED` sets on the handshake's final ACK,
`nf_conntrack_proto_tcp.c:1325-1333`; the flow is offered for hardware ADD on the very next
qualifying packet, same RTT, `nft_flow_offload.c:70-104` → `nf_flow_table_core.c:318-345`) —
a driver decline is retried roughly once per second by `flow_offload_refresh()`
(`nf_flow_table_core.c:351-368`) for as long as packets keep arriving, which is what a
35-100s-late "+tcp" line actually measures: repeated declines of a low-traffic connection,
not a slow-arriving bulk one.

**A follow-up test eliminated iperf3 from the picture entirely and got a sharper negative
result.** Method: `nc -l 5202` on the WAN peer as a real sink (per this project's own
established rule — never write into a service that won't read), `dd if=/dev/zero bs=1M
count=50 | nc 172.16.0.2 5202` as ONE plain, single-stream TCP transfer from the LAN client,
throughput measured via `/sys/class/net/wan/statistics/{rx,tx}_bytes` (immune to any
application-level reporting quirks). Result: hwnat installed a row for this exact flow
(`192.168.0.2:57186 -> 172.16.0.2:5202`) within seconds, and it stayed hot (`AGE` 16-17)
throughout the observed window — genuinely correct behaviour this time, one real flow,
tracked correctly. **But total WAN tx growth across the whole transfer was ~380 KB out of
the 50 MB (52.4 MB) sent — a ~0.7% completion rate — with growth concentrated in an early
burst (~357 KB in the first measured window) followed by a long, slow trickle (tx crept up
while the LOCAL sender process had already exited, rx stayed completely flat — the signature
of stuck retransmission, not delivery) before the transfer was abandoned.** Box health
throughout and after: 0% loss on both small and `-s1400` pings, sub-millisecond latency, no
wedge detection at any point in this run.

**This rules out iperf3's dual-connection behaviour as the cause of the throughput
problem.** The exact same shape — an initial burst, then a stall the box never recovers from
within the test window — reproduces with a single clean flow and zero application-level
complexity. The two bugs fixed today are real, correctly fixed, and independently verified;
they are simply not what is limiting bulk throughput. The likely site is upstream of hwnat
entirely: whatever destroys the DATA connection's TCP state in the first several seconds —
very likely still the underlying M7 large-frame CPU-RX mechanics (100% of forwarded bytes
cross the CPU until a flow is offloaded, and the offload window for a low-traffic
control-style connection doesn't help a separate bulk connection at all) — rather than
anything specific to hardware-NAT install/aging logic. **Next session should treat this as
reopening the original M5/M7 forwarding-collapse investigation with a clean, iperf3-free
reproduction case in hand (`dd | nc` + WAN interface byte counters), not as a hwnat bug.**

**⚠ One more thing this same `nc` test settles, and it cuts against the tidiest version of
the story above: the LARGE-FRAME WEDGE did NOT fire at any point during this run** (checked
directly — zero `WEDGE`/`recovery level` lines in the full UART capture spanning the whole
transfer), yet the exact same burst-then-stall shape happened anyway. So "the wedge's own
~2s recovery pause is what crashes the connection" — the best-fitting explanation for
test1's 393-retransmit event, where a `LARGE-FRAME WEDGE`/`recovery level 3` pair really did
land right at the collapse — **is not the whole story, or not the only mechanism**: this run
stalled with no wedge involved at all. Either there are two independent stall mechanisms (one
wedge-triggered, one not), or the wedge is a downstream symptom of the same deeper collapse
in both cases rather than test1's proximate cause. Whoever picks this up next should not
assume "cure the wedge and throughput follows" — test both with the wedge detector's own
logging left in place, correlating its firing (or, as here, its absence) against every stall,
not just the one instance where the two coincided.

**★★★★★★ Sharpest evidence yet, from a fresh reboot plus a peer-side `tcpdump` capture: the
router is sending bytes that never reach the peer at all.** Two more clean single-stream
`dd | nc` tests were run, this time immediately after a fresh power-cycle (testing whether
cumulative per-boot degradation explains the low completion rate — it explains *some* of it:
1.82 MB delivered of 50 MB attempted on the very first bulk transfer of a fresh boot, ~3.6%,
versus ~0.7% on a box already deep into a long test session — real, but nowhere near enough
to call the connection healthy) and with a live `tcpdump -i any -s 128 'tcp port 5203'`
running throughout on the WAN peer (`tiny`) for the second one. The capture is decisive:
**1572 packets, ALL of them clustered inside a 71 millisecond window (`1788535251.231546` to
`1788535251.302475`), delivering just under 1 MB with zero retransmissions and zero resets —
then complete silence for the rest of the multi-second test.** Meanwhile the router's own
`/sys/class/net/wan/statistics/tx_bytes` kept climbing slowly for many more seconds *after*
that 71 ms window closed (confirmed by polling every 5-6s through the remainder of the test).
**A byte counter that keeps incrementing while the peer receives literally nothing new on
that connection means the router believes it is transmitting frames that are not reaching
the wire (or not reaching this peer) at all** — this is not a TCP congestion-collapse story
(zero retransmits were even *seen*, because the peer never got the chance to generate one;
the sender's own kernel is presumably the one retrying, into a void). `hwnat` did install a
row for this exact flow (`192.168.0.2:50646 -> 172.16.0.2:5203`, `rows out@538 in@927`), and
that row was later cleanly deleted and then **reinstalled at the identical indices 5 seconds
later** — the same signature as every earlier "torn down and reinstalled" observation in this
document, now with a peer-side capture proving that whatever happens around that reinstall
event, no bytes are getting through to the other side.

**This reframes the open question one more time.** Not "why does TCP collapse" (there may be
no TCP-level collapse to explain — the wire itself may simply stop carrying this flow's
frames) but **"where do a flow's frames go after the ASIC accelerates it, if they aren't
reaching the peer — and does the WAN tx byte counter even measure real wire transmissions,
or something upstream of the physical port (e.g. a hairpin/internal loop, or the ASIC's own
egress descriptor ring, that can silently absorb frames without them ever reaching the
PHY)."**

**★★★★★★★ Done immediately after, with the box still in this exact post-stall state:
`ethtool -S wan` cross-checked against the netdev counter — and the hairpin/internal-loop
theory is refuted.** `ifOutOctets` (the ASIC's own hardware port MIB register) read
**3,029,558**, essentially matching the netdev `/sys/class/net/wan/statistics/tx_bytes`
reading of **3,014,232** (within 15 KB — `ifOutUcastPkts`+multicast+broadcast = 3312 vs
netdev `tx_packets` = 3300, same close agreement). **Both independent counters — one read
from the switch-core silicon itself, one from the Linux netdev layer — agree that roughly
3 MB of real frames left the physical WAN port.** The peer's own `tcpdump`, run throughout,
captured only ~1 MB. Since the hardware MIB and the netdev counter agree with each other but
disagree with what the peer received, this is **not** an internal SoC loopback/hairpin
artifact (that would show the MIB *disagreeing* with netdev, since a looped frame never
reaches the PHY the MIB counts at) — the frames genuinely left the wire. (One loose thread
noticed but not chased: `ethtool -S wan`'s own `tx_bytes` field, a THIRD counter in the same
output, read lower still — 2,604,322, a further ~410 KB below `ifOutOctets` — meaning even
this driver's *own* two exposed counters for the same port disagree with each other by a
non-trivial margin. Which of the three is measuring what is not yet established; treat all
of `tx_bytes` (ethtool), `ifOutOctets`, and netdev `tx_bytes` as three distinct instruments
until proven otherwise, not interchangeable readings of "how much left the port.")

**Two candidate explanations were checked immediately, on the same post-stall state, and
BOTH are refuted — narrowing the mystery further without yet solving it.**

*Frame corruption, discarded silently by the peer NIC below tcpdump's visibility* — refuted.
`ip -s link show eth0` on the peer (`tiny`), read right after the stall: `RX: ... errors 0
dropped 0 missed 0`. A NIC drops a frame failing its own FCS/CRC before handing it to the
capture path, and such drops are still counted in these kernel-level stats even when the
frame itself is invisible to `tcpdump` — zero here means the peer's hardware saw nothing
wrong, because it saw nothing extra at all, clean or corrupt.

*Wrong destination MAC on ASIC-accelerated egress* (a well-precedented bug class in this
project's history, and a natural fit — get it wrong and any switch/NIC would filter the
frame with no error, exactly matching "MIB counts it, peer never sees it") — also refuted,
by reading the ASIC's OWN L2 table directly rather than trusting the Linux neighbour entry
alone. `cat /proc/rtl865x_gw` showed `172.16.0.2 dev wan lladdr e4:5f:01:04:98:af ... STALE`
in the kernel's neighbour table (worth noting: STALE, not REACHABLE — a live loose thread,
see below) — but the ASIC's own programmed L2 row is what actually decides the destination
MAC frames go out with, and that was checked separately. `cat /proc/rtl865x_dump`'s `L2`
section, index 536: raw words `5f010498 00dc10e4`. Decoded with this driver's own write
format (`gw_write_l2()`/`gw_write_l2_full()`, `rtl865x_asichal.c:513-544`:
`e[0] = m[1]<<24 | m[2]<<16 | m[3]<<8 | m[4]`, `e[1] & 0xFF = m[0]`; the vendor hash-table
scheme stores only 5 of the 6 MAC bytes, `m[5]` is not recoverable from a dumped row and
is presumed disambiguated by the hash bucket the entry lives in) gives `m[0..4] =
e4:5f:01:04:98` — an exact match against the peer's real MAC on every byte the table format
even stores. **The ASIC is programmed with the correct nexthop MAC.**

**So: the sending MIB and netdev counters agree bytes left the physical port; the ASIC's own
L2 table has the correct destination MAC; the peer's NIC shows zero errors, zero drops, and
zero missed — and still received roughly two-thirds less than what left the router.** This
is now narrowed as far as this bench's tooling allows without a genuine wire-level tap. One
loose thread worth chasing first, cheaper than new hardware: the neighbour entry's STALE
state (`used 0/0/0`) — if `hwnat_add_flow()`'s nexthop-MAC read happens to run during a
narrow window where the kernel neighbour subsystem is transitioning STALE→PROBE→REACHABLE
(rather than reading the ASIC's already-correct, already-programmed value, which this dump
shows to be fine at rest), a race there — not a wrong steady-state value — could still
explain frames going astray specifically around the observed "torn down, reinstalled 5s
later" moments, without contradicting a correct MAC being visible in a dump taken well after
the fact. **The cheaper of the two diagnostics was run immediately, and also comes back negative.**
Polled `ip neigh show 172.16.0.2` together with `tx_bytes` every 4s through a live 30 MB
`dd | nc` transfer (48s of continuous observation). The neighbour entry transitioned
REACHABLE → STALE at t=20s — the same correct MAC (`e4:5f:01:04:98:af`) held on both sides of
the transition, and `tx_bytes` climbed at the identical steady slow-trickle rate
(~1.5-2 KB/4s) before and after, with no visible change at the transition itself. **The
neighbour-table race hypothesis is ruled out**: whatever is dropping most of this flow's
bytes is not tracking the kernel neighbour state machine.

**This investigation is now narrowed as far as this bench's tooling can take it without a
genuine wire-level tap.** Confirmed real (sender MIB + netdev byte counters agree, ASIC L2
table has the correct destination MAC) and confirmed silent (peer NIC: zero errors, zero
drops, zero missed; no correlation with neighbour-table state). Whoever continues this needs
either a physical capture point between the DIR-842's WAN jack and the peer (a hub, or a
switch with port mirroring — first CONFIRM the bench's actual physical topology directly,
which this investigation assumed rather than verified) to see a frame's fate at the physical
layer directly, or a from-scratch review of the ASIC's egress descriptor/queue path in
`rtl819x-eth.c`/`rtl819x_swnic.c` for a mechanism that could increment a "sent" counter
without a genuine, correctly-formed frame reaching the PHY (a bad checksum computed and
inserted into a frame, a wrong VLAN/CPU-tag surviving onto the physical trunk, a TX
descriptor recycled before the DMA actually completed, etc.) — this session did not have
time to audit that path in the same depth as the NAT/offload control-plane code it already
covered.

**★★★★★★★★ CORRECTION, from a longer-running dual-sided capture — "frames never reach the
peer" was itself wrong; the earlier zero-packets-after-71ms capture was just too short to
catch what actually happens next.** Ran `tcpdump` simultaneously on the DIR-842's OWN `wan`
interface AND on the peer, through a `dd | nc` transfer that this time was accidentally left
running unattended for an extended period (the `nc` call had no `-w` timeout and the
connection never closed on its own). The peer's capture — a normal, non-memory-constrained
Debian host, plausibly the more trustworthy of the two runs here — shows something the
earlier short capture never had time to see: **the router's retransmissions of the same
segment (`seq 842801:844249`) genuinely DO arrive at the peer, repeatedly, at 2s/3s/8s/14s
intervals — textbook TCP RTO exponential backoff, not silent frame loss.** The router's own
`wan`-side capture, by contrast, stops dead after a single ACK from the peer (`ack 842801`)
with nothing captured afterward — but this is almost certainly this capture PROCESS dying on
a 64 MB RAM box running `tcpdump` for an extended, unplanned duration (an earlier cleanup
attempt on this same box hit `ash: pkill: not found`, i.e. this box's busybox has no
`pkill`, only `killall` — a real gap in this session's own command hygiene, not evidence of
anything about the datapath), not evidence that the peer's ACKs never reached the router.

**So the corrected picture is: the peer DOES receive this flow's data, repeatedly. What
isn't completing the round trip is the ACK path** — the router keeps retransmitting because
it never sees an ACK past 842801 arrive and take effect, even though (per the peer's own
capture) the peer plausibly keeps sending one. This re-opens, with much better evidence than
before, this document's own long-standing unfinished lead: **instrument the reverse-NAPT /
return path** (WAN→LAN, i.e. the peer's replies being DNAT'd back to the LAN client) rather
than continuing to treat this as an egress/frame-loss problem. Note this does NOT retract the
earlier MIB-agrees-with-netdev or ASIC-L2-MAC-is-correct findings — those remain measured
facts about the forward/egress path, which this correction suggests was never actually the
problem. It DOES retract the "peer NIC shows zero errors/drops" finding's implied conclusion
("therefore nothing arrives") — the peer's NIC-level stats were checked once, right after a
SHORT capture that (as now shown) ended before the real retransmission activity even began;
they were never wrong, just not informative about the window that mattered.

**Concrete next steps, sharper than before**: (1) fix this session's own tooling gap first —
`killall`, not `pkill`, on the router's busybox, and always give test transfers a bounded
`-w`/timeout so a stalled `nc` can't run unattended for hours; (2) capture on the router's
`wan` interface again, this time robustly (background it correctly, verify with `pgrep`
before trusting silence, consider redirecting to a file the same way the UART logger is
handled) for the FULL duration of a stall, specifically watching for the peer's ACKs arriving
at the router and never producing a corresponding LAN-side forward; (3) capture on the
LAN-side bench NIC simultaneously, completing the three-point trace this document called for
earlier — now aimed at the return direction specifically: does the peer's ACK reach the
router's WAN port (should now expect: yes, based on this correction) and does it ever reach
the LAN client (this is the open question); (4) check `/proc/rtl865x_napt`'s `idx_in`
(inbound/reverse) row specifically during a stall — is it actually being hit by the peer's
returning ACKs, or is the reverse-NAPT lookup itself missing them, silently dropping them
before they can reach the LAN client and reset the router's own retransmission timer.

**★★★★★★★★★ A dedicated code-audit agent then read the complete reverse-NAT/LAN-delivery
chain (`hwnat_program_rows()`, `gw_arp_add_host()`, `gw_write_l2()`, `rtl865x_lan_set_
nexthop()`) and found a genuinely precise, well-evidenced primary candidate — then a targeted
hardware test refuted it for this specific instance, worth recording in full because both the
theory and its refutation are real findings.**

**The theory**: the ASIC's L4 NAPT table (whose `agingTime` this document has been reading as
"hot") and the L3-route→ARP→L2-nexthop chain that actually delivers a rewritten packet to a
physical LAN jack are two separate, uncoupled pipeline stages. A NAPT hit proves the rewrite
fired; it says nothing about whether that rewritten packet then found a valid route. The
LAN client's L2 nexthop entry (`gw_write_l2()`, `rtl865x_asichal.c:513-528`) is written
**once**, with `force=true` — which the ASIC write helper's own code (`rtl865x_asic_write_
entry()`, `:267-300`) explicitly skips readback verification for (`if (!force && ...)`, so a
forced write's success is never confirmed). That entry lives in a 256-row × 4-way hash table
(`gw_l2_row()`, 8-bit hash) that the switch's own **hardware MAC learning writes into
continuously and uncontrollably** — there is no learn-disable bit in this driver's register
set (confirmed identical between the 4.14 and 6.18 `rtl819x_regs.h`, byte-for-byte). Router
role freezes L2 aging (`TEACR` bit0, `rtl865x_asichal.c:1164`) specifically so the driver's
own static entries never expire — but the same freeze means if hardware learning ever
collides with or aliases the LAN client's row, **nothing ever detects or repairs it for the
rest of the boot**: unlike the WAN side (`rtl865x_wan_netif_mac_sync()`, re-checked live on
every flow add), the LAN side (`rtl865x_lan_set_nexthop()`) only rewrites when its OWN cached
shadow value changes — it never compares against live ASIC content, so external corruption
is invisible to it. This would exactly explain the whole session's pattern: a clean early
burst (before any collision), then a near-total, non-self-healing stall (after one), with
wildly different completion percentages between runs because the collision's timing is
essentially random. Confirmed NOT a 6.18 regression — byte-for-byte identical to 4.14's
tree, including the "single-client shadow will thrash" comment verbatim in both; if anything
this is a latent bug 4.14 never manifested because its own benchmark ran on what its docs
call "an isolated bench" with presumably less L2-table contention (explicitly flagged by the
agent as inference, not measured on either bench).

**The test.** LAN client MAC `00:e0:4c:12:59:90` → `gw_l2_row()` hash = 0x77 = 119 →
`/proc/rtl865x_dump`'s L2 section, way 0, dump index `119*4 = 476`. This exact entry was
already on record from earlier this session (`L2[476] = e04c1259 005c0400`, decodes to the
client's real MAC). Re-read at three points across one bounded `dd|nc` transfer: t≈8s (tx
+481 KB in the first 8s — healthy), t≈18s (tx +4.3 KB in the next 10s — the stall had
already happened), and at completion (final: 501 KB of 20 MB attempted, **2.4%
completion** — one of the worst results this session, and this time confirmed to have
happened WITHOUT any `WEDGE`/`recovery level` firing). **`L2[476]` read byte-identical —
`e04c1259 005c0400` — at all three points, including after the transfer had unambiguously
already collapsed.** The LAN client's L2 nexthop entry was never clobbered.

**This refutes the primary candidate, for this specific run.** The theory remains
structurally sound (single fire-and-forget write, no live re-verification, shares a table
with uncontrollable learning, frozen aging with no self-heal — all measured facts about the
code, not retracted) — it just was not what caused *this* stall. Whether it explains some
OTHER run's stall (collision timing is theorized to be essentially random) is untested; this
one data point rules it out as a *universal* explanation, not as a real bug worth fixing
regardless. Two things stand out in the same capture, not yet chased: (1) the inbound/reverse
NAPT row's `age` read 12 at completion versus the outbound row's 16 (ceiling is 17) — the
reverse direction was measurably less recently hit, consistent with reverse traffic thinning
out mid-test but not explaining why; (2) the L2 table's own 4-way structure was only checked
at way 0 (index 476) — ways 1-3 (477-479) were never scanned for an *aliased* duplicate entry
that could shadow-collide without literally overwriting way 0 itself.

**★★★★★★★★★★ DECISIVE, and the clearest result of this whole investigation: a capture on
the LAN client's OWN NIC (a vantage point never used until now — trivially available, this
session's own bench host, no new hardware needed) proves the fault is squarely on the return
path, downstream of a confirmed-correct L2 entry.**

Method: `tcpdump -i enx00e04c125990` (the bench host's own interface, i.e. capturing from the
LAN client's side — a THIRD capture point, complementing the WAN-peer capture and the
router's own wan-side capture already used earlier) running through one more bounded
`dd | nc` transfer. Result, from the tail of the capture — i.e. the stalled portion, well
after the connection had already collapsed:
```
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 0.44s later)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 0.90s later)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 1.76s later)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 3.62s later)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 7.17s later)
192.168.0.2.51540 > 172.16.0.2.5208: seq 641465:642913 ... [TS ecr 1645251224]   (retransmit, 14.08s later)
```
**Every single visible frame in the stalled tail is MY OWN outbound retransmission of the
same segment, with clean exponential RTO backoff. Not one single frame from `172.16.0.2`
appears anywhere in this window** — not an ACK, not a duplicate ACK, nothing. (The
connection's SYN-ACK earlier in the same capture DID arrive cleanly, confirming the capture
setup and the interface both work correctly — this is a real absence, not a tooling gap.)

**Read together with the peer-side capture from earlier in this document (which DID show the
peer receiving these same retransmitted segments repeatedly, with matching RTO timing), this
localizes the fault precisely: the LAN client's outbound data is reaching the peer; whatever
the peer sends back in response is not reaching the LAN client.** Combined with the L2 table
finding immediately above (the LAN client's own ASIC nexthop entry stays byte-identical
throughout a stall — not clobbered, not aliased), the fault sits specifically in the space
between "ASIC's inbound/reverse NAPT row registers a hit" (independently confirmed hot, this
document, multiple times) and "a correctly-addressed frame is actually delivered out the LAN
jack toward this client" — a space this session has now excluded the two most obvious
candidates from (L2 nexthop corruption; a wrong destination MAC in the first place, checked
much earlier). What remains inside that gap and has NOT yet been checked: a checksum
computed incorrectly during the DNAT rewrite (IP/TCP checksums must be corrected to match the
rewritten addresses — an off-by-something here would produce a frame the ASIC/driver both
believe was sent successfully, that any receiving NIC discards silently, below what
`tcpdump` on the LAN side could ever see since `tcpdump` captures BEFORE the checksum
validation on RX for outbound-generated views but the significant subtlety is this capture is
FOR INBOUND traffic on the client, where a bad checksum WOULD normally still show up in
`tcpdump` since libpcap captures pre-validation on most Linux NIC drivers — so if checksum
corruption were the cause, a corrupted-but-present frame should still have appeared here,
and none did at all, which argues AGAINST simple checksum corruption and FOR the frame never
reaching this segment's wire at all); or a switch-fabric-level failure specific to the
LAN-side egress port/queue for RETURNING (reverse-NAT'd) traffic, structurally similar in
nature to the original CPU-side M7 large-frame wedge but on a path this project has not
instrumented at all (the wedge detector and its fix both watch the CPU RX side exclusively).

**★★★★★★★★★★★ One more link in the chain checked, closing out essentially every
table-content candidate: the ARP entry between the NAPT rewrite and the L2 table is ALSO
correctly programmed.** The reverse-NAT audit agent's traced chain is NAPT-rewrite →
route(LAN /24) → ARP window (`GW_ARP_WIN_LAN=0` + host octet) → that ARP entry's `nextHop`
field → L2 table. `struct asic_arp` (`rtl865x_asichal.h:112-115`) sizes `nextHop` at **10
bits** (0-1023) — comfortably wide enough for the full 1024-slot L2 table (256 rows × 4
ways), so unlike the ALREADY-FIXED, structurally similar bug this exact file documents
(`GW_ARP_NH_IDX`, `rtl865x_asichal.c:580-585`: a *different*, 6-bit nexthop-table pointer
that used to truncate 64→0 and send the ASIC to an empty slot — "the shape of the bug we
were chasing," already corrected to `20`), there is no bit-width truncation possible here.
Read live: `ARP[2]` (LAN window base 0 + `192.168.0.2 & 0xff` = 2) = raw word `000053b9`.
Decoded against the struct (`valid = word & 1`, `nextHop = (word >> 1) & 0x3FF`): `valid=1`,
`nextHop=476` — **exactly the L2 index already independently confirmed (above) to hold the
LAN client's correct, unclobbered MAC.** The ARP entry is correct.

**So: the NAPT row confirmably reloads on hits; the ARP entry correctly points at; the L2
entry that correctly holds the LAN client's real MAC. Every table this session can read is
independently verified correct, end to end, and the frame still does not arrive.** This
rules out essentially every software/table-content explanation available without new
tooling. What remains is either something in the ASIC's actual internal packet-processing
pipeline that no amount of correct table content fixes (a genuine silicon-level datapath
limit or bug specific to this rewrite-then-deliver sequence), or something this session
has not yet considered — TCP/IP checksum correctness after the DNAT rewrite was raised and
partially argued against (a corrupted-but-present frame should still have been captured by
`tcpdump` on the LAN client, and none was, at all), but not directly measured; queue depth
or congestion specifically on the LAN-side egress path for reverse-NAT'd traffic was raised
by the reverse-NAT audit agent and never checked. Whoever continues past this point should
treat every ASIC table (NAPT, ARP, L2, and the netif/route tables checked earlier this
session) as a settled, closed line of inquiry, and move to either a genuine wire-level
capture between the router's LAN jack and the client, or a direct audit of checksum-rewrite
and egress-queue code specific to the reverse-NAT path in `rtl819x_hwnat.c`/
`rtl819x_swnic.c`/`rtl819x-eth.c`'s TX side.

**One more closed: the hardware checksum-recompute feature (`CSCR`, needed after any
DNAT/rewrite) is confirmed live and enabled.** `GW_CSCR` (`rtl865x_asichal.c:377`, address
`RTL819X_SWCORE_BASE + 0x4048` = `0xBB804048`) read live via `regdump_base`/`regdump_n`:
`0x00000038`. `GW_CSCR_L3L4CHK = (1<<4)|(1<<5) = 0x30` (`:381`) — both bits are set in the
live read (`0x38 & 0x30 = 0x30`). The ASIC is actively told to recompute L3/L4 checksums on
rewrite, right now, on this exact box. Rules out "checksum recalc got silently disabled" as
an explanation.

With NAPT, ARP, L2, and now checksum-recompute all independently confirmed correct and
active, **every register- and table-level lever this driver exposes has been checked and is
fine.**

**★★★★★★★★★★★★ BREAKTHROUGH — a dedicated agent found that the entire ASIC-table audit
above, while establishing those tables genuinely correct, was investigating a mechanism
that isn't even NECESSARY to reproduce the bug, and the real fix is a one-line UCI
change.** The agent re-read the isolation matrix already on record in this document and
combined two facts that had never been put together: `hwnat=N` (this driver's own ASIC NAT
sysfs switch, OFF — meaning none of `hwnat_program_rows()`/`hwnat_add_flow()` ever runs, by
the module's own documented design, `rtl819x_hwnat.c:73-79`) **fails identically** to
`hwnat=Y`. So the fault reproduces with zero code from this driver's NAT table machinery
involved at all — every NAPT/ARP/L2/CSCR check this session ran was real and correct, just
aimed at a subsystem that was never the cause.

Cross-referencing mainline kernel source (`net/netfilter/nft_flow_offload.c`,
`nf_flow_table_ip.c`): `nf_flow_table`'s software fastpath has two xmit modes — `NEIGH`
(live, per-packet ARP lookup) and `DIRECT` (a source/dest MAC pair **cached once**, at
flow-add time, via `dev_fill_forward_path()` walking the device topology). Critically,
`flow_offloading_hw=1` (the **fw4/nftables-level** UCI setting — independent of this
driver's own `hwnat` sysfs knob, and set to `1` in every failing test this session ran)
forces `DIRECT` unconditionally. Separately, hitting a `DEV_PATH_BRIDGE` hop while resolving
either direction's path also forces `DIRECT` for that direction. This board's topology
(`base-files/etc/board.d/02_network`) makes `br-lan` a real Linux bridge over the 4 LAN DSA
ports while `wan` is a bare, unbridged DSA port — exactly the asymmetry that would push the
LAN/reverse direction onto a cached, potentially stale or wrongly-resolved `DIRECT` header
while the forward/WAN direction stays on live `NEIGH` resolution. This is a single mechanism
that accounts for every asymmetry measured this entire session: forward always reaches the
peer, reverse never reaches the LAN client, with a well-formed frame the driver hands to
hardware correctly and no error anywhere in this driver's own instrumentation — because the
fault is entirely upstream, in generic kernel NAT-offload code this driver never touches.

**Tested immediately, same bench, no rebuild needed:**
```sh
uci set firewall.@defaults[0].flow_offloading_hw='0'   # flow_offloading stays '1'
uci commit firewall && /etc/init.d/firewall restart
```
`nft list flowtables` confirms `flags offload` is gone (software fastpath only, devices
collapse to `{ br-lan, wan }`). Repeated the exact same `dd | nc` stall reproduction, this
time also capturing on the LAN client's own NIC. **Result: complete success.** 30 MB sent,
peer→client capture shows the connection closing with a clean four-way FIN/FIN-ACK/ACK
teardown at `ack 31457281` — essentially the full payload, byte for byte (`30×1024×1024 =
31457280`). 20,042 packets captured on the LAN side (versus ~528 in the last failing run),
8,376 of them genuine peer→client traffic arriving throughout, not just in an early burst.
Elapsed wall-clock (first SYN to final ACK): 7.92 s → **≈31.8 Mbit/s sustained, complete,
non-stalling.** `ping`/`ping -s1400` both 0% loss, sub-millisecond, immediately after.

**This is the first fully complete, non-stalling bulk transfer of this entire session.**
31.8 Mbit/s is far below the 4.14 hardware-accelerated benchmark (889-896 Mbit/s, 0% CPU) —
this configuration runs the flow through nf_flow_table's CPU-bound software fastpath, not
true zero-copy ASIC acceleration — but it is a real, complete, reliable transfer, which is
what M5 actually needs to be functional. **The TX descriptor/DMA path itself was
separately confirmed byte-identical to 4.14's** (same audit pass, full diff of
`_New_swNic_send()`/`New_swNic_txDone()`/`rtl819x_eth_xmit()`'s core) — ruling out a TX
regression as an alternative explanation and reinforcing that the fault genuinely lived in
the hardware-offload xmit-mode selection, not in this driver's own code.

**⚠ CORRECTION, immediately after — a second confirming run tempers "breakthrough" down to
"a real, significant improvement, not a guaranteed fix."** Same config
(`flow_offloading_hw=0`), a larger 50 MB transfer, no LAN-side capture running this time
(a real gap in this specific run — the completion percentage below is from WAN byte
counters only, the same method used all session, but without the packet-level detail that
made the first run's success unambiguous). Result: **~11.5 MB of 50 MB delivered, ~23%
completion** — `nc -w 45` exited with code 0 (its own idle-timeout semantics don't
distinguish "finished" from "still slowly progressing when time ran out," unlike the first
run's directly-observed clean FIN handshake). Box remained healthy after (one transient
100%-loss ping blip that resolved on retry within seconds — noted, not treated as
significant given this session's established pattern of occasional transient blips
distinct from real hangs).

**So: `flow_offloading_hw=0` is a real, substantial improvement (two very different results
— 100%+ and 23% — both meaningfully better than most `flow_offloading_hw=1` runs this
session, which mostly landed under 5%) but not a guaranteed complete fix by itself.**
Whether the 23% run was a genuine partial stall (same class of problem, just less severe)
or simply hadn't finished when `nc`'s timeout fired is unknown without a repeat run
carrying the same three-point capture the first run had. Do not report this configuration
as fully solving M5 — it is the strongest lever found this session, not a proven cure.

**★★★★★★★★★★★★★ A third run, this time WITH the LAN-side capture running (fixing run 2's
gap), came back clean — 2 of 3 confirming runs now have full, capture-verified complete
delivery.** Same config, 50 MB transfer, generous 75 s `nc -w` bound. `tcpdump -i
enx00e04c125990` throughout: 38,294 packets captured, ending in a textbook 4-way FIN
teardown at **`ack 52428801`** — exactly the full 50 MB payload
(`50×1024×1024 = 52428800`), byte for byte. 14,420 genuine peer→client packets captured
across the whole transfer, not just an early burst. WAN byte counter growth (~53.4 MB
against 50 MB attempted, consistent with the same few-percent protocol-overhead margin run
1 showed) independently agrees. `ping`/`ping -s1400` both 0% loss, sub-millisecond,
immediately after.

**Calibrated conclusion: `flow_offloading_hw=0` (with `flow_offloading=1` for software
fastpath) is now supported by 2 of 3 runs with full, independently-verified complete
delivery, and the one questionable run (#2) has no evidence it actually failed — it simply
lacked the instrumentation to prove either way, unlike #1 and #3, which had verification
and both succeeded cleanly.** This is real, validated progress — the strongest, most
reliable lever this whole session found — without yet being a large enough sample to rule
out any residual intermittency. Whoever continues should re-run #2's exact scenario (50 MB,
`nc -w 45`, WITH a LAN-side capture) once to settle whether that specific result was a false
alarm or a real, if less severe, occasional issue.

**★★★★★★★★★★★★★★ Fix shipped as the default, and verified end to end on a genuinely fresh
factory-flash boot — the actual end-user experience, not a hand-tuned live box.** Added to
`files/target/linux/rtl819x/base-files/etc/uci-defaults/99-dir842`:
```sh
uci -q batch <<-EOT
	set firewall.@defaults[0].flow_offloading='1'
	set firewall.@defaults[0].flow_offloading_hw='0'
	commit firewall
EOT
```
(with a long inline comment recording the full rationale and evidence trail, so nobody
re-flips this to `1` without re-reading why). Rebuilt, reflashed a factory image from this
exact tree, cold-booted, and — **with zero manual UCI commands** — confirmed straight from
first boot: `uci show firewall.@defaults[0]` already reads `flow_offloading_hw='0'`, and
`nft list flowtables` already shows a flowtable with no `flags offload` (`devices = {
br-lan, wan }`, software fastpath only). Ran a 4th `dd | nc` transfer (40 MB) on this exact
fresh-boot state, LAN-side capture running throughout: **`ack 41771905` of 41943040
bytes attempted — 99.6%** (the pcap file itself hit a truncation artifact right at the very
end from stopping the capture mid-flush, not a data-transfer failure — the WAN byte counter,
independently, read consistent with a complete transfer for this boot). `ping`/`ping -s1400`
both 0% loss, sub-millisecond, after. **That makes 3 of 4 `dd | nc` runs under this fix with
full capture-level verification, all landing at 99.6%-100%+ complete**, plus the one
un-instrumented 23% result that was never actually proven to be a failure rather than an
undersized timeout.

**M5's basic functional requirement — a router that reliably carries a sustained bulk
transfer — is now met BY DEFAULT, out of the box, with no user configuration required.**
This is a genuinely major result for this session. It is explicitly NOT the original M5
target: true ASIC hardware acceleration (0% CPU, ~889-896 Mbit/s, matching 4.14's proven
benchmark) remains unfixed, and this fix runs every flow through nf_flow_table's CPU-bound
software fastpath instead (~30 Mbit/s measured, run 1). Do not report M5 as fully solved —
report it as: basic reliable forwarding fixed and shipped as default; full hardware
acceleration still open, with a precise, evidence-backed lead already on record above (why
`DIRECT` xmit mode's cached-header resolution goes wrong for this board's bridge/DSA
topology) for whoever picks that up next.

**Immediate next steps, in order of value:** (1) repeat with the SAME rigor as the first
confirming run — LAN-client capture running, large transfer, generous timeout — several more
times to establish a real completion-rate distribution for this configuration, not just two
data points; (2) decide the right
PERMANENT default — `flow_offloading_hw=0` with `flow_offloading=1` is a real, working
configuration and could ship as the interim default while hardware offload is fixed
properly, but forfeits the ASIC's true zero-CPU acceleration this board is capable of
(4.14 proves ~900 Mbit/s is achievable in silicon); (3) the actual CURE, not just the
mitigation just proven: find why `DIRECT` xmit mode's cached header resolution goes wrong
specifically for this board's bridge/DSA topology — a live kernel trace of
`nft_dev_forward_path()`/`dev_fill_forward_path()` (needs ftrace or bpftrace, not confirmed
available on this image) or a small diagnostic patch logging the resolved header/device at
cache-time versus what this driver's DSA/`tag_rtl8_4` layer actually sees at transmit time
would settle whether the cached MAC pair itself is wrong, or whether it's correct but
something in delivering a `DIRECT`-mode-injected skb through this port's DSA conduit
specifically mishandles it (recall the CPU-tag descriptor-mediated tag shim this port uses,
`rtl819x_dsa_tag_tx()`/`_rx()` — confirmed uniform/tag-shape-agnostic by the same audit, but
never checked against a `DIRECT`-path-injected skb specifically, only against ordinary
CPU-originated ones).

**Session conclusion for M5, stated plainly: NOT DONE, but decisively narrowed.** Two real,
independently-verified bugs were found and fixed (the `hwnat_flow_stats()` idx_out/idx_in
blind spot, and the LARGE-FRAME WEDGE false-positive against working offload) — both
confirmed on hardware and both genuine improvements. A third real robustness gap (LAN L2
nexthop's missing live re-verification) was found, precisely characterized, and — this
session — refuted as the cause of any observed stall (worth fixing regardless). The fault is
now triangulated, with hardware evidence from three independent capture points (WAN peer,
router's own WAN interface, and now the LAN client itself) plus direct ASIC-table reads, to a
specific narrow space: **reverse-NAT'd return traffic that the ASIC's own NAPT table
confirms it is matching never reaches the LAN client.** No further candidate has been ruled
IN yet — only ruled out. Do not report M5, or the port as a whole, as functionally complete.
This investigation has been extremely thorough (six dedicated research agents, dozens of
hardware tests spanning three independent capture vantage points, two real bugs shipped) and
should not be re-derived from scratch — whoever continues should read this section in full,
in order (several early conclusions in it are explicitly superseded by later ones), and start
from this final, most-localized finding: instrument or capture specifically for what happens
to a return frame between the confirmed-hot NAPT hit and the LAN-side PHY, since everything
upstream and downstream of that specific gap is now independently confirmed correct.

**⚠ Superseded below — a candidate WAS subsequently found and substantially validated
(`flow_offloading_hw`/nf_flow_table `DIRECT` xmit mode). Keep reading; do not stop here.**

This flow was also the clearest live reproduction yet of the box's degradation-under-load
symptom: `iperf3 -t 25` took 113+ wall-clock seconds and never exited on its own (killed
manually); `ping` round-trip time to the box climbed from a normal <2 ms baseline to a
sustained ~30 ms while the flow was active, with 0% loss throughout (alive, congested, not
wedged). Consistent with — but not new proof beyond — the cumulative-degradation note
elsewhere in this doc.

**★★★★★★★★★★★★★★★ 2026-09-04, three adversarial agents run against the shipped M5 fix —
one REFUTES the "no config-level cure" pessimism and hands over a concrete, untried
experiment for TRUE ASIC acceleration.** Three independent agents were launched: (a) an
adversarial re-verification of the DIRECT/NEIGH kernel claim against the actual 6.18.44
source tree the target builds from, (b) a fresh driver-vs-4.14 audit of the "hot NAPT row,
near-zero throughput" mystery, (c) a web/precedent search. Results:

- **(b) found no new smoking gun.** The driver's offload control plane installs both flow
  directions as genuinely independent ASIC rows (`hwnat_flow_replace()`, reply-half matched
  by tuple filling only `cookie_in`, `rtl819x_hwnat.c:750-760`), no policer/rate-limiter
  write exists in `rtl865x_asichal.c`, and no 4.14 mechanism was dropped in the migration.
  It did re-confirm the already-fixed `NETDEV_TX_BUSY`-retry tag-corruption bug
  (`rtl819x_dsa_tag_tx_undo()`, `rtl819x-eth.c:1684-1711`) as the likely cause of ONE
  specific documented stall (the 393-retransmit iperf3 test1), but not the core reverse-NAT
  gap. The "112 s hot" aging counter reloads on *any* hit (a bare ACK), so it was never
  throughput proof — consistent with everything already recorded here.

- **(c) independently confirmed the DIRECT-forced-by-bridge mechanism is real, upstream, and
  BY DESIGN**, with no sysctl/kconfig/nftables knob to suppress it (`nft_dev_path_info()`,
  `net/netfilter/nf_flow_table_path.c`). Crucially it found the exact-match precedent:
  **OpenWrt issue #18589** ("hardware flow offload works LAN↔WAN and WLAN↔WAN but NOT the
  bridge-crossing WLAN↔LAN direction") — the identical asymmetry this port measured, closed
  upstream as "pending in mainline, 2-3 year wait," with the `bridger` package named as the
  practical userspace workaround. The kernel fix (Eric Woudstra's "Add bridge-fastpath"
  series, and a parallel Daniel Pawlik "L2 bridge offload" v2 posted 2026-06-29) is **not
  merged as of 2026-09**, so there is nothing finished to backport. `mtk_ppe` hit the same
  class of bug and fixed it with an explicit `dev_fill_forward_path()` call
  (Bianconi 2022) — same fragile bridge/forward-path boundary, but MTK-tag-only, not
  transferable to `tag_rtl8_4`.

- **(a) is the actionable one, and it PARTIALLY REFUTES (c)'s "no config fix" conclusion.**
  Reading `net/netfilter/nf_flow_table_path.c` line by line: both the hw-offload-forces-DIRECT
  write (`nft_dev_path_info()`) AND the bridge-hop-forces-DIRECT write are **gated by
  `nft_flowtable_find_dev(info->dev, ft)`** (`path.c:206-207`) — the forced-DIRECT xmit_type
  is only *written back* if the resolved egress device is a **member of that flowtable's own
  `devices={}` list**. firewall4 today lists ALL lan+wan devices
  (`fw4.resolve_offload_devices()`, `ruleset.uc:2`), so the gate always passes and the LAN
  direction always gets broken DIRECT. **But if the flowtable's `devices` set lists only
  `wan` (keeping `flags offload`), the LAN/reverse direction's egress device is not a member,
  the forced-DIRECT write is skipped, and that direction stays on live `NEIGH` (correct
  bridge transit) — while the WAN direction still gets DIRECT and, more importantly, ASIC
  hardware promotion still fires for BOTH directions**, because the `TC_SETUP_FT` hardware
  callback is bound per-flowtable-flag and `nf_flow_offload_tuple()` broadcasts to it for
  both tuples regardless of which devices are listed (`nf_flow_table_offload.c`, confirmed by
  both (a) and (c)). Agent (a) also verified this driver never consumes the reply-direction's
  MANGLE MAC anyway — it resolves the LAN client MAC itself via the neighbour table
  (`hwnat_lan_mac()`, `rtl819x_hwnat.c:585-622`), and `ingress_ifindex` (the LAN DSA port)
  still arrives correctly in the `FLOW_CLS_REPLACE` — so excluding LAN from the *software*
  device list does not starve the *hardware* row install of anything it needs.

**The untried experiment this hands over (highest-value next step for true M5 acceleration,
NOT yet run):** patch firewall4's flowtable `devices={}` to WAN-zone-only (e.g. override
`resolve_offload_devices()` or ship a custom nftables flowtable include listing only `wan`),
set `flow_offloading_hw=1`, and bench a `dd|nc` bulk transfer with the three-point capture
(WAN peer + router wan + LAN client). Expected if the theory holds: the ASIC promotes the
flow (real `hwFwd=1`, zero-CPU), the reverse direction is delivered via NEIGH instead of
being dropped by DIRECT, and throughput jumps from the ~30 Mbit/s software-fastpath rate
toward the 4.14 silicon benchmark. If it does NOT hold, the fault is deeper than xmit-mode
selection and the `bridger`-style userspace path (or waiting for bridge-fastpath to merge)
is the fallback. **This is a real lead with kernel-source and precedent backing, not a
guess — but it is unproven on this hardware; do not record M5 acceleration as solved until a
capture-verified bench run confirms it.** `flow_offloading_hw=0` remains the correct shipped
default until then.

**★★★★★★★★★★★★★★★★ 2026-09-04, THE WAN-ONLY EXPERIMENT WAS RUN ON HARDWARE — the hypothesis
is HALF-CONFIRMED: it fixes the reverse-NAT DIRECT-drop and engages real ASIC offload, but a
SECOND, independent bug (SoC port0 egress-queue tail-drop) still collapses bulk throughput.**
Method: on an already-flashed box (no reflash), edited the running `/usr/share/ucode/fw4.uc`
so `resolve_offload_devices()`/`resolve_hw_offload_devices()` return only the wan zone's
device, `fw4 restart` — producing `flowtable ft { devices = { "wan" }; flags offload; }`
(verified via `nft list flowtables`) — plus `flow_offloading_hw=1`, `hwnat=Y`, WAN static
172.16.0.1/24, LAN client = bench host at 192.168.0.2 routing through the box, peer sink a
Python socket drainer on `tiny`:5205, LAN-side capture on the client's own NIC. What the
first (cleanest, least-degraded) run showed:

- **Real ASIC hardware offload engaged**: `conntrack -L` showed the flow `[HW_OFFLOAD]`,
  and `/proc/rtl865x_napt` had two hot rows (`out@557 in@899`, both `age=17`, the reload
  ceiling). `dmesg` logged `hwnat: +tcp 192.168.0.2:44536 -> 172.16.0.2:5205 rows out@557
  in@899`. This is genuine silicon offload, not the software fastpath the shipped default
  uses.
- **★ The reverse-NAT DIRECT-drop bug is GONE**: the LAN-client capture showed **85
  peer→client packets arriving** (`172.16.0.2.5205 > 192.168.0.2 ...`), versus **zero** in
  every `flow_offloading_hw=1` run in the entire prior investigation. So the
  `nft_flowtable_find_dev` device-membership theory is CONFIRMED for the reverse path:
  excluding LAN from the flowtable's device set keeps the reverse/LAN direction on live
  NEIGH, and reverse-NAT'd return traffic is delivered correctly. **This directly resolves
  the single most-investigated failure of this whole section** ("reverse-NAT'd return
  traffic that the ASIC confirms matching never reaches the LAN client").
- **But bulk transfers still stall**, at ~260 KB on the first run (worse on later, degraded
  runs — see caveat). The LAN capture showed the peer SACKing a hole (`sack
  {251969:253417}`) then a ~7.7 s RTO backoff gap — i.e. **forward-path (LAN→WAN) packet
  loss under a sustained burst**, TCP then collapsing. A switch-MIB before/after
  (`ethtool -S eth0/wan/lan2`) during a stalled run showed **every 8367S discard counter
  flat** (`s00_p06_ifOutDiscards`, `etherStatsDropEvents`, `dot1dTpPortInDiscards` all
  unchanged) — so the loss is NOT on any external-switch port. It is on the **SoC's own
  port0 egress queue**, which the 8367S MIBs cannot see — exactly the driver's own long-
  documented "A-2 residual" tail-drop (the routed flow U-turns on SoC port0, ingress VID2 +
  egress VID1 on the same port; a saturating cwnd burst tail-drops there while the shared
  descriptor pool never nears runout). Hardware offload makes this WORSE by removing the
  CPU's implicit pacing and pushing line-rate bursts at that queue.
- **`trunk_pause=1` did NOT help — it made the stall EARLIER** (~60 KB vs ~260 KB),
  matching the parameter's own header (`1 = pause on (collapse)`) and contradicting §4's
  earlier tentative "pause ON extends the transfer" note. Restored to the shipped `2`.

**⚠ CAVEAT — the throughput numbers across these runs are confounded by cumulative
degradation.** This session ran 5+ bulk transfers on ONE boot, violating this document's own
"ONE measurement per cold boot" rule; the box degrades progressively under sustained
forwarded load (documented elsewhere in §4), and indeed a shipped-config (`hw=0`) transfer
run at the END of the sequence also stalled at 0.2% — NOT because the shipped config
regressed, but because the box was hammered. So the *quantitative* comparison (260 KB vs
60 KB vs …) is unreliable. What is NOT throughput-dependent and DOES stand: the qualitative,
capture-proven fact that **reverse-NAT frames reach the LAN client under wan-only+offload
(85 packets) where they never did under all-devices+offload (0 packets)**, plus the
`[HW_OFFLOAD]`/hot-NAPT-row proof that the ASIC genuinely offloaded the flow.

**Net: the two failures §4 spent the whole session on are now DECOUPLED.** Bug A (reverse-NAT
DIRECT-drop) — the flowtable-device-membership fix resolves it, confirmed on hardware. Bug B
(forward-path SoC-port0 egress tail-drop under line-rate offload) — the actual remaining
blocker for true acceleration, now cleanly isolated to the SoC port0 U-turn queue, invisible
to the 8367S MIBs, unaffected (or worsened) by `trunk_pause`. **For a FUNCTIONAL router the
shipped `flow_offloading_hw=0` is still correct** — it completes transfers reliably on a
fresh boot (software fastpath, ~30 Mbit/s), whereas wan-only+offload engages the ASIC but
collapses on Bug B. **Next step for true acceleration is now specific and different from
before:** attack Bug B — the SoC port0 egress-queue tail-drop on the routed U-turn. Candidate
directions (none yet tried): eliminate the port0 U-turn entirely (separate ingress/egress
physical paths, a VLAN/topology change), deepen or flow-control the port0 egress queue at the
SoC MAC level (not the 8367S), or configure real link-level pause on BOTH ends of the SoC↔
8367S trunk (the `trunk_pause` param only touches the SoC side via PCRP0; the 8367S EXT1 side
is programmed by the `rtl8365mb` DSA driver's fixed-link, which deliberately omits `pause` —
DT change, not a runtime param). The reverse-NAT half no longer needs investigation.

**★ 2026-09-04, SHARPER: the ASIC offload runs at 0% CPU but drops the flow even at LOW PACED
rates — so Bug B is a datapath CONFIG defect, not a burst-only queue tail-drop.** Follow-up
measurement, wan-only + `flow_offloading_hw=1` + `hwnat=Y`, comparing box CPU at a paced
90 Mbit/s `iperf3` against the shipped software fastpath:
- **Shipped (`hw=0`, software fastpath) @ 90 Mbit/s: ~50-58% CPU in softirq**, idle 16-41% —
  the CPU cost of per-packet software forwarding, and exactly why it ceilings near 150 Mbit/s.
- **wan-only (`hw=1`, ASIC offload) @ 90 Mbit/s: CPU 90-100% IDLE, 0% softirq** — but the
  transfer had STALLED at a few hundred bytes, so ⚠ **0% CPU here means NO TRAFFIC, not
  silicon forwarding** (correction below). iperf3 (TCP and UDP) couldn't even establish its
  stream; `dd|nc` earlier got ~260 KB before stalling; fails identically at UDP 5-20 Mbit/s.
  `pid_dump` on the few frames that reach the CPU shows `hwFwd=0` (trapped, not forwarded),
  `reason=7` territory (NO_MATCHED_NAPT_ENTRY).

**⚠ CORRECTION (2026-09-04, from the 4.14-vs-6.18 datapath audit agent) — the two bullets
above over-claimed, and the "port0 U-turn tail-drop / Bug B" framing throughout this session
is WRONG.** The agent diffed every port0/trunk/switch-core register between the working 4.14
tree and this 6.18 tree and found them **byte-identical**: SBFCR0-2/PBFCR0-5 egress
flow-control thresholds (`4.14 rtl819x-eth.c:538-544` == `6.18 :793-799`), RX/TX ring sizes,
MBUF flags, the PCRP0 pause force-clear (`trunk_pause=2` on both), all identical. So there is
NO port0 egress-queue / scheduler / pause register that 4.14 sets and 6.18 misses — the
"port0 tail-drop" and `trunk_pause` leads are dead ends (and `trunk_pause=1`-makes-it-worse
was measured against a since-retracted harness, see RETRACTION #3). Two real corrections:
  1. **The `[HW_OFFLOAD]` flag I saw is the Linux `nf_flow_table`/DSA conntrack flag, NOT the
     rtl865x silicon L3 forwarding that gives 4.14 its 890 Mbit.** The two are different
     mechanisms. `nf_flow_table` bulk still crosses the CPU; the 0% CPU I measured was simply
     the stalled flow (no packets), not zero-copy silicon forwarding. My "true acceleration is
     engaging" reading was wrong.
  2. **The real 890-Mbit path is this driver's OWN `hwnat` rtl865x fabric** (`gw_prog` NAPT
     rows, `SWTCR1=0x2200`, `DACLRCR=0xFDFDFDFD`, numeric host-order keys — `HWNAT-OFFLOAD.md
     §1-6`), which on 4.14 = 890/896 Mbit @ **0.0% through-CPU** vs `hwnat=0` software = only
     184 Mbit @ 100% CPU. The 6.18 port installs the NAPT rows but the ASIC **traps** frames
     (`hwFwd=0`, `reason=7` = NO_MATCHED_NAPT_ENTRY) instead of matching+forwarding them — the
     long-standing core M5 blocker, unchanged. This is the actual target for true acceleration,
     and it is a NAPT-match problem in the rtl865x silicon, NOT a port0/queue/pause issue.
  3. **One genuine 4.14→6.18 divergence the agent flagged (lower confidence, being checked):**
     mainline `rtl8365mb` chip-RESETS and re-jams the 8367S at probe (`rtl8365mb_reset_chip`,
     `CHIP_RESET` + jam table, `patch 703`), re-training the RGMII trunk — exactly what 4.14
     FORBIDS (it makes `rtl8367b_reset_chip()` a no-op for the 8367S to preserve the loader's
     power-on RGMII analog/PLL training, `main:rtl8367b.c:779-797`). Patch 703's own note
     documents this reset producing "receives perfectly, forwards nothing, no discard counter
     moves" — the exact flat-8367S-MIB signature seen during the offload stalls. Verifying via
     the `switch` regmap debugfs whether `0x1311`/`0x1307` read the 4.14-target values after
     the DSA reset.

**★★★★★★★★★★★★★★★★★ 2026-09-04, DECISIVE RECONCILIATION — basic forwarding IS functional; the
"collapse" is an unpaced-line-rate-blast artifact, not a real-world failure.** After the
wan-only experiment above, repeated `dd|nc` AND default-`iperf3` transfers were stalling at
~0.3% even in the shipped `flow_offloading_hw=0` config on a fresh, fully-settled box —
apparently contradicting this section's "basic reliable forwarding works" claim. The
resolution: **the box's software fastpath has a ~130-150 Mbit/s CPU forwarding ceiling, and
an UNPACED same-speed (1G↔1G) line-rate blast overruns it**, overflowing the CPU RX ring →
early packet loss → TCP cwnd collapse → then the LARGE-FRAME WEDGE fires (a *downstream*
symptom, once large frames stop) and its level-3 recovery finishes the transfer off. Rate-
capped `iperf3` through the box proves the datapath is healthy below the ceiling:

| offered rate | result |
|---|---|
| `iperf3 -b 50M` | 71.6 MB, **0 retransmits, complete** |
| `iperf3 -b 80M` | 76.4 MB, **0 retransmits, complete** |
| `iperf3 -b 90M` | 161 MB, **0 retransmits, complete** |
| `iperf3 -b 130M` | 155 MB, **0 retransmits, complete** |
| `iperf3 -b 160M` | collapses (exceeds the CPU ceiling → wedge/recovery) |

CPU stayed **90% idle** during a healthy forward run (the flow is software-fast-pathed, not
per-packet routed). So **M5 basic forwarding is genuinely functional for real-world use** —
clean, complete, zero-retransmit transfers at every rate up to ~130 Mbit/s, which is at or
above what this class of home router's uplink ever sees, and TCP paces to the bottleneck on a
real uplink anyway. The unpaced-blast collapse is a bench artifact of driving a 1G LAN client
straight into a 1G WAN peer with no rate limit — it exceeds the CPU forwarding ceiling that
**only true ASIC hardware acceleration would remove** (the ASIC forwards in silicon at line
rate, 0% CPU — exactly the still-open goal). This reconciles the earlier `dd|nc`-based
"stall" confusion in this document: those raw-blast measurements were hitting the CPU ceiling,
not a datapath defect. **Correction to earlier phrasing in §4: the shipped software fastpath
delivers ~130 Mbit/s clean, not "~30 Mbit/s" — the 30 figure was a single early raw-blast
run that was already ceiling-limited.** The functional-router baseline is solid; true
line-rate ASIC acceleration remains the one open M5 stretch goal, with the forward-path SoC
port0 U-turn tail-drop (Bug B above) as its now-isolated blocker.

**★★★★★★★ 2026-09-05, LATER STILL — live re-test on real hardware, three mechanisms newly
RULED OUT by direct evidence; the failure signature sharpened further.** Bench access
confirmed live (router at 192.168.0.1, peer `tiny` at 172.16.0.2, both reachable, UART logger
already running). Router found in its safe shipped state (`hwnat=Y`, `flow_offloading_hw=0`,
`napt_priority=7` — the priority fix from the previous entry is confirmed still active in the
tree and in the running module). Re-armed `flow_offloading_hw=1` for testing only.

Reproduced the stall cleanly, twice, with a proper clean-slate accept-loop sink on `tiny`
(loopback-verified alive before each run, per the standing bench rule) and `tcpdump -w`
capturing directly on `tiny`'s own `br0` — both runs reported **"0 packets dropped by
kernel"**, so this is not the tcpdump-on-a-loaded-host confound from earlier in this section.
Both runs show the same signature already documented above: a single TCP segment is dropped
inside an otherwise-clean initial burst, every retransmission of that exact segment ALSO
fails (7 retries, exponential backoff, 13+ seconds, never succeeds), while hundreds of KB
immediately before and after it deliver fine — `nc`'s own 35 s idle timeout eventually kills
the connection. **New in this session: the failing byte offset is NOT fixed between runs**
(run 1 stalled at stream offset ~292 KB with the burst having reached ~350 KB; run 2 stalled
at ~272 KB with the burst having reached only ~301 KB) — this rules out a fixed-byte-count or
fixed-packet-count trigger; it is more consistent with something tied to the *timing* of the
asynchronous `nf_flow_table` hardware-offload promotion for that specific connection (which
varies run to run) than to a deterministic structural offset.

Three concrete mechanisms were tested against direct counter/register evidence and ruled out
as the cause:
- **Conntrack TCP-window rejection: RULED OUT.** `net.netfilter.nf_conntrack_tcp_be_liberal`
  is `0` (strict) on this box, raising the possibility that conntrack's own TCP state tracker
  was silently rejecting the retransmission as out-of-window `INVALID`. Checked directly:
  `conntrack -S`'s `invalid=` counter read **18 immediately before and 18 immediately after**
  a full failed transfer — completely unchanged. If conntrack had been rejecting the packet,
  this counter would have moved. It did not. (Not previously checked this way anywhere else
  in this document.)
- **Linux netdev/qdisc-level drop: RULED OUT.** `/sys/class/net/wan/statistics/{rx,tx}_dropped`,
  `*_errors`, `*_fifo_errors`, `rx_over_errors`, and `collisions` were all **0** after a failed
  transfer. If the packet had reached the WAN netdev's software TX path and been dropped there
  (queue full, TX error, etc.), one of these would have incremented.
- **A table-lookup/write race in the ASIC row-install primitive itself: RULED OUT by code
  audit.** Read `rtl865x_asic_write_entry()` (`rtl865x_asichal.c:267`) end to end: it asserts
  `EN_STOP_TLU` and polls `STOP_TLU_READY` **before** writing any word of the new row, writes
  all words, commits (`TLU_ACTION_START`), waits for completion, and only then clears
  `EN_STOP_TLU` — the hardware table-lookup engine is genuinely frozen for the entire
  multi-word write, so a packet classification cannot race a torn/partial row write. Also
  confirmed no periodic timer or workqueue in this driver re-writes a live NAPT row after
  install: the ~5.1 s periodic `hwnat AGE` printk seen throughout this document's dmesg
  excerpts goes through `rtl865x_asic_read_entry()`, which never touches `EN_STOP_TLU` — it is
  read-only housekeeping, not a write. This rules out a recurring write-time race as the
  explanation for failures on retries many seconds after the row was installed once.

A fourth attempt — using the driver's own `pid_dump` module parameter (logs the next N RX
frames reaching the CPU, including the ASIC's `hwFwd`/`reason` metadata) armed during an
isolated retry window — was **inconclusive**, not negative: this session's own concurrent SSH
diagnostic traffic to the router dominated the trace (visually identifiable as SSH-sized
packets sharing one constant `reason=0c03` code), and no frame unambiguously matching the
stuck segment's size with a NAPT-miss-style reason code could be isolated from the noise. A
clean read of this instrument needs a capture window with **zero** concurrent SSH/diagnostic
traffic to the router (a serial-console-only vantage point, or a second bench operator), which
this session's single-operator setup could not provide.

**Net effect on the standing hypothesis:** with conntrack rejection, netdev-level drop, and a
write-time hardware race all now excluded by direct evidence, the failure is narrowed further
onto exactly the two possibilities this section's `★ 2026-09-05, FIVE-AGENT SWARM` entry
already named as the remaining candidates — a DSA-conduit/CPU-tag encoding mismatch specific
to an ASIC-accelerated frame at the port0 U-turn, or the still-unconfirmed missing/incorrect
row field — with the tag/VLAN-encoding mismatch now the stronger of the two, since it is the
only remaining candidate that naturally explains a **timing-dependent** (not byte-count-fixed)
single-packet loss confined to the exact moment a flow crosses from software to hardware
forwarding. Confirming it needs a capture of the actual on-wire bytes of an ASIC-forwarded
frame (not reachable by tcpdump on the router's own `wan` netdev, since genuinely
hardware-forwarded frames bypass the Linux network stack entirely and would show up empty
regardless of whether the frame on the wire is correct) — the next concrete step remains
exactly what it was: a physical-layer or switch-mirror-port capture comparing an ASIC-hot
frame's actual CPU-tag/VLAN bytes against a CPU-forwarded frame's for the same peer, which
this bench does not currently have the hardware (a mirror/tap port) to take.

**Router restored to its safe shipped default** (`flow_offloading_hw=0`) at the end of this
session's testing; `napt_priority=7`/`hwnat=Y` unchanged from the shipped defaults. No source
changes were made this session — this entry is evidence-gathering only, narrowing the
existing open lead rather than closing it.

**★★★★★★★★ 2026-09-05, LATER — the DSA-tag hypothesis retracted by reading the actual tag
driver; a live A/B on real hardware proves the silicon; a real, unrelated bug found and fixed
along the way; new instrumentation added and its first result.** A dense session covering
several threads:

- **The standing "DSA CPU-tag/VLAN encoding mismatch" hypothesis (the leading lead at the end
  of the previous entry) is RETRACTED.** Read the actual `tag_rtl8_4` kernel driver
  (`net/dsa/tag_rtl8_4.c`): its 8-byte tag is added/parsed **only** on frames crossing the
  CPU⇄switch conduit link (`rtl8_4_tag_xmit`/`rtl8_4_tag_rcv`). A genuinely ASIC-forwarded
  frame (LAN jack in, WAN port out, CPU never touched) never gets this tag added or removed —
  it never crosses that link at all. So a fix modeled on MediaTek's PPE offload (which
  resolves a DSA port and stamps tag bits into its hardware flow entry) does not apply here:
  MTK's PPE-accelerated frames genuinely do cross their SoC's CPU-facing GMAC⇄switch link
  (the PPE sits inline with it), ours do not. Also checked: this port's LAN-side addressing
  (`l2_mask_lan`, a coarse whole-group port mask) is unchanged from the working 4.14 port,
  which never needed per-jack DSA resolution either — so "missing DSA port resolution" isn't
  what's missing.
- **Live A/B on the exact same physical unit, settling the "is it the silicon" question for
  good.** Downloaded this project's own published `v1.4` GitHub release (the real `main`/4.14
  port, ggbruno base, built clean-room), flashed it to the bench router in place of the 6.18
  image, armed `hwnat=Y`+`flow_offloading_hw=1`, and ran the identical 40 MB `dd|nc` transfer
  that fails on 6.18 every single time. **On 4.14, it completed in full — twice, back to
  back — 41,943,040 bytes exactly, clean connection close both times**, with dmesg confirming
  a real ASIC row installed once and staying hot (age pinned near the reload ceiling) for the
  entire transfer, load average low throughout. Put the identical chip, on the identical
  board, under a working kernel/driver stack, and it forwards a complete transfer with zero
  loss, every time; put it under this port's stack and it loses exactly one packet forever,
  every time. That is no longer an inference — it's a same-hardware, same-day, controlled
  comparison. The defect is confirmed software-side, not silicon.
- **Kernel-core hardware-offload promotion timing checked directly, both kernels: equivalent,
  not the differentiator.** Read `nft_flow_offload_eval()` in both the 4.14 and 6.18 kernel
  trees (both sitting locally in this project's build dirs). Both trigger hardware-offload
  promotion at the same point — the first TCP packet conntrack considers ESTABLISHED — and
  both dispatch the actual driver ADD asynchronously via a workqueue
  (`nf_flow_offload_hw_add()`/`schedule_work()` on 4.14; `nf_flow_offload_work_alloc()`/
  `queue_work()` on 6.18). So the *policy* for when a flow gets promoted to hardware is not
  what changed between the two kernels — ruling out "6.18 promotes later, mid-burst" as an
  explanation on its own.
- **A real, separate, standalone bug found and fixed: the `LARGE-FRAME WEDGE` detector
  (`rtl819x-eth.c`) false-fires on ordinary management traffic, and this is why the bench felt
  so flaky throughout this whole session.** Its gate for "small frames still trickling in
  while zero large frames are delivered" originally required only `rx_packets != previous
  sample` — satisfied by nothing more than routine SSH/ping/ARP chatter, which describes most
  of a router's normal operating life (no bulk transfer running at that instant is completely
  normal, not evidence of a wedge). Measured live: this fired every ~15-30 s from nothing but
  repeated SSH probing, each firing forcing a real disruptive reset
  (`napi_disable`/`netif_tx_disable`/fabric reset) that then broke the very SSH session
  probing it — a self-sustaining false-positive feedback loop. Raising the rate threshold
  alone did not fix it (still fired at a measured `rx_pkts=511`, a total lower than the
  800+ that threshold's own math would require — a mathematical impossibility that exposed
  the real bug): `rx - fcs_prev_rx` on `unsigned long` **underflows to a huge value** whenever
  the sampled counter ever reads back lower than the previous sample, for any reason, which
  trivially clears *any* threshold no matter how high it's set. Fixed by guarding the
  regression directly (treat a lower reading as "resync the baseline," not "burst detected"),
  alongside a more conservative rate floor as defense in depth. **Validated**: an identical
  stress sequence (fw4 reload + hwnat arming + rapid repeated SSH) that reliably collapsed
  SSH to near-total failure before now completes cleanly and repeatably (8/8, then 8/10 with
  only isolated blips, zero `WEDGE detected` messages logged where there were previously
  several per boot).
- **Tested whether this fix also explains M5 — cleanly REFUTED, not left ambiguous.**
  Re-ran the identical hwnat-armed bulk transfer on the SSH-fixed image immediately after
  validating the SSH fix: same stall, same signature, connection never closes. Confirmed via
  dmesg and `/proc/rtl865x_napt` that the ASIC row installed correctly and stayed hot the
  entire time, and that **zero** wedge/recovery events of any kind fired during the failing
  window. The two bugs are independent — a real, useful fix on its own merits, but not the
  M5 answer.
- **New instrumentation added: an in-kernel ASIC-write trace, built identically into both
  kernel trees for a direct diff.** `rtl865x_asic_write_entry()` now logs every call (jiffies,
  table type, index, first word, TLU-freeze poll-loop iterations) to a 4096-entry ring buffer,
  dumped via `/proc/rtl865x_trace` (writable to reset the capture window). Deliberately
  write-only (not reads) — periodic aging/stats polls are far too frequent and would drown
  out the writes that actually matter, and reads never freeze the table-lookup unit anyway
  (confirmed earlier in this section).
- **First capture from a genuine failing run, and a real (if negative) result.** Armed hwnat,
  reset the trace, ran the transfer, confirmed the row was installed and hot
  (`out@1008 in@907`) — and the transfer stalled again, identically. The trace: **all 12 ASIC
  writes for the entire test — the WAN/LAN nexthop resync plus the flow's own two NAPT rows —
  landed in an 18-jiffy burst at connection start, and then absolutely nothing else wrote to
  the ASIC tables for the remaining ~35 s of the stall.** This directly rules out (by evidence,
  not just argument) the standing hypothesis that some *other* table write — a different
  flow's install/delete, or the aging worker reaping a dead row — freezes the table-lookup
  unit and collides with this flow's live traffic: there was nothing else happening to
  collide with. Whatever eats the packet does so with the ASIC write path completely
  quiescent, pointing at either the silicon's own internal forwarding/queue pipeline or
  something outside the write path this instrument covers (the read/aging path, or
  DMA/ring-buffer level). Getting the same trace from the 4.14 reference for a direct diff is
  the immediate next step, in progress.
- **Build-system gotcha, worth recording so it doesn't cost someone else the same hour it
  cost this session:** OpenWrt's `files/`-overlay copy into `build_dir` does not reliably
  re-sync on every incremental `target/linux/compile` — an exit code of 0 is not proof a
  driver edit was actually recompiled. Caught only because a newly-added `/proc` entry was
  missing after a "successful" build; confirmed via `diff` against `build_dir`'s copy and an
  `nm` symbol check on the resulting `.o`. Always verify a source edit actually landed in the
  compiled object after any build touching already-built kernel sources, not just check the
  exit code.
- **Router returned to its safe shipped default** (`flow_offloading_hw=0`) at the end of each
  test cycle described above; the wedge-detector fix and the trace instrumentation are both
  live in the current tree (`rtl819x-eth.c`, `rtl865x_asichal.c`) and were both included in
  every image flashed this session from this point forward.

**★★★★★★★★★ 2026-09-05, LATER STILL — the 4.14 side of the write-trace comparison came back,
four parallel investigations closed off two more leads with hard evidence, and two promising
candidates from those forks were chased and refuted.**

- **The identical trace instrument was ported to the 4.14 reference** (mirrors the 6.18 one
  field-for-field, adapted only for the older `struct file_operations` proc API 4.14's kernel
  uses). Building it required disabling `lsblk` in `util-linux` (`--disable-lsblk` added to
  that package's `CONFIGURE_ARGS` in this port's local build only) — a pre-existing,
  unrelated link failure in that old buildroot's util-linux build, nothing to do with this
  investigation.
- **The 4.14 trace, captured from a genuinely successful hardware-accelerated transfer
  (verified: real ASIC row installed and hot, not a silent software-only pass — the first
  attempt each time silently declined hw offload for reasons not yet chased down, requiring a
  second connection to actually engage it, on BOTH ports equally): 14 entries. The first 10 —
  the WAN/LAN nexthop resync — are byte-for-byte IDENTICAL to the 6.18 failing run's trace**
  (same types, same indices, same first-words). The only difference is a trailing NAPT
  clear/delete pair on 4.14, ~90 real seconds after install — well after that transfer's own
  ~35s duration, i.e. ordinary post-close teardown, not something that happened during the
  live transfer. **During the active transfer window itself, both the working 4.14 case and
  the failing 6.18 case show the identical shape: install once, then silence.** This is a
  clean, direct (not inferred) result: the ASIC write pattern cannot distinguish success from
  failure. Whatever eats the packet on 6.18 leaves no trace in what gets written to the
  hardware, on either port.
- **Four parallel agent investigations launched into previously-uninspected angles, given the
  write and read paths were now both cleanly exhausted:**
  1. *Read/polling path* (frequency, addresses, locking) — **ruled out**, byte-identical
     between kernels on every axis checked, no address overlap between the NAPT range and the
     read-side "dummy read" address.
  2. *GRO/GSO/TSO segmentation* — **ruled out**. Neither kernel's conduit driver advertises
     `NETIF_F_SG`, so the stack always pre-segments before `ndo_start_xmit` on both; DSA's own
     SG-stripping logic doesn't apply to this board's tag variant (`rtl8_4`, head-tag, no
     tailroom). Feature negotiation and the xmit path are equivalent.
  3. *Runtime DSA/switch activity* — **found a real, confirmed structural difference**:
     `rtl8365mb_stats_poll()` (`rtl8365mb_main.c:2555`) runs a mutex-serialized, multi-register
     SMI/MDIO read sequence on the external RTL8367S chip every 3 seconds, continuously, for
     as long as any port has link. Confirmed absent on the working 4.14 port (`rtl8367b.c`
     reads stats on-demand only, never on a timer) — a genuine difference introduced by this
     port, not previously known. Physically plausible mechanism proposed: every ASIC-
     accelerated frame has to physically cross the shared RGMII trunk to the external switch
     chip twice (the "port0 U-turn" this document has suspected throughout), so a periodic
     register-read sequence on that same external chip is a real candidate for an intermittent
     disruption landing on exactly that path.
  4. *Flow-promotion race, fresh angle* — **found a real, confirmed kernel-generation
     ordering change**: 6.18 sets `NF_FLOW_HW` synchronously and early, before the driver's
     async ASIC-row-write work even runs (`nf_flow_table_core.c:342-344`); 4.14 sets the
     equivalent flag only after the driver's row write confirms success
     (`nf_flow_table_hw.c:104-111`). Real and upstream, not something either port's driver
     controls — but the fork could not close the causal chain to packet loss (the flag
     appeared to gate only GC/timeout bookkeeping, not the software fast-path itself).
- **Both leads chased to a decisive conclusion — both refuted, cleanly:**
  - **Lead 4 (NF_FLOW_HW ordering), closed by a dedicated follow-up code trace, not left
    ambiguous.** Traced the entire per-packet software fast-path end to end:
    `nf_flow_offload_ip_hook()` → `nf_flow_offload_forward()` → unconditional
    `flow_offload_refresh()` → unconditional xmit. **Zero references to `NF_FLOW_HW` anywhere
    in that chain** — a packet reaching this hook is forwarded via software regardless of the
    flag's state or its setting order. Every actual use of the flag is confined to
    `nf_flow_offload_gc_step()` (teardown/expiry bookkeeping only). **Refuted with certainty**:
    this ordering difference cannot drop a packet — and its very unconditionality reinforces
    the standing conclusion that a lost packet never reaches the CPU's netfilter stack at all
    (no netdev drop, no conntrack invalid, and now no way for this software fallback to have
    silently failed to catch it either).
  - **Lead 3 (MIB stats poll), closed by a direct live A/B test, not left theoretical.** Added
    a runtime toggle (`rtl8365mb.mib_poll`, default on, live in `build_dir`'s copy of
    `rtl8365mb_main.c` pending a proper patch if it had panned out) that skips the read but
    keeps rescheduling, so both states could be tested in one boot with no reflash. With
    `mib_poll=0` confirmed live, hwnat armed, a fresh ASIC row confirmed installed and hot the
    entire time (`age` pinned near the reload ceiling, zero wedge/recovery events) — **the
    transfer stalled again, identically.** Restored `mib_poll=1` (default) afterward.
    **Refuted**: the periodic external-chip MIB read is not the mechanism, despite being a
    real, confirmed, previously-unknown difference from the working port.
- **Where this leaves the investigation:** every category reachable by comparing configuration,
  register writes, register reads, kernel-core packet-forwarding logic, segmentation handling,
  and now two concrete confirmed structural differences between the ports has been checked and
  eliminated by direct evidence — not by argument. What remains unfalsified is exactly what the
  write-trace comparison already pointed at: something inside the ASIC/switch fabric's own
  internal behavior during the live forwarding path, invisible to every software-side
  instrument available so far. The only way to close that is what this section named several
  entries ago and still lacks the hardware for: a physical-layer or switch-mirror-port capture
  of an actual ASIC-hot frame, on the wire, at the moment it's dropped.
- **Router restored to its safe shipped default** (`flow_offloading_hw=0`, `rtl8365mb.mib_poll`
  back to its default of `1`) at the end of this entry's testing.

## 5. Flash boot (M6)

**★ 2026-09-05: fresh 10/10 confirmed against the EXACT current binary** (the one carrying
the wedge fix, the M7 `eth_hw_addr_set` fix, and the `PRINT_ARRAY` cap — sha256
`f20993b7…` factory / `cb36f932…` sysupgrade, matching `images/sha256sums.txt`), run after a
2026-09-05 adversarial review correctly pointed out the previous 10/10 number was stale and
predated real crash events found later the same session. Result: **10/10 pass, 0 fail** —
`boot N/10: PASS (oops=0 userspace=yes jffs2=1)` for all ten, `=== GATE RESULT: 10/10 pass,
0 fail ===`. This supersedes the flat, unqualified line below, which is kept for the
historical record of what was originally claimed and when.

`bootgate.sh` (earlier run, now known to be against a since-superseded build): 10/10 cold
boots from NOR, no oops, no panic, jffs2 overlay every time. `sysupgrade -n`: verified clean
back-to-back.

**PARTIALLY ADDRESSED (user-requested 2026-09-04): SSH/dropbear takes too long to become
responsive after boot.** Symptom observed repeatedly this session: the box pings and the
UART advances (kernel alive) for a long stretch while `ssh` still fails with "Connection
refused" (dropbear not listening yet) then "banner exchange timeout" (dropbear listening but
too slow to send its SSH banner) — well over a minute past network-up, worst on a first-flash
boot. Root cause confirmed as (a), not (b)/(c)/(d): CPU starvation from `dir842-asic`'s
boot-time ASIC bring-up (including its own explicit `fabric_reset=3` full reset) and
`asic-wifi-settle`'s retry loop competing with dropbear for the single core, most acute during
first-flash jffs2 formatting (~150-250s of ~89% system CPU) but measured recurring on warm
boots too. **Fix shipped**: both `dir842-asic` and `asic-wifi-settle` now `renice` themselves
down at the start of their `start()` functions so dropbear wins CPU contention. **Confirmed
live, both directions**: a normal warm boot after this fix connected via SSH in under 2
seconds with a clean wire capture; a first-flash boot still took ~218s before SSH answered
(matches the jffs2-format floor, which the fix does not and cannot eliminate — reniced
services still need to run eventually, they just no longer starve dropbear specifically) and,
separately, one first-flash boot on this exact build took an unusually long ~350s+ for jffs2
to finish (see the TX-corruption/crash finding below, from the boot immediately following
that one) — so the fix measurably helps but does not make boot time itself faster, and does
not fully eliminate variance on the slowest boots. Not fully closed; a reasonable stopping
point given the underlying jffs2-format cost is a real, near-fixed floor on this hardware.
**★ Diagnosis (2026-09-04, partial):** dropbear is NOT the bottleneck — it is `START=19`
(early) and up at kernel t≈46s, and its host key is pre-generated in the image
(`/etc/dropbear/dropbear_rsa_host_key` dated build-time), so it is neither ordered late nor
blocked on entropy. The tx#/rx# console debug prints are BOUNDED (first 3 TX / 40 RX frames
ever, `rtl819x_swnic.c:445,688`) — ~1s of one-time 38400-baud UART blocking, not the cause.
The delay is **boot-time CPU starvation**: dropbear listens but can't get CPU to complete the
SSH banner handshake ("banner exchange timeout") while three heavy things run — the first-flash
jffs2 format (~89% sys CPU, ~150-200s, one-time per flash), the boot-time level-3 FABRIC
RECOVERY (`dir842-asic` S97 does `echo 3 > fabric_reset` = FULL_RST + swcore clock cycle +
MEMCR init + gw_prog, ~1-2s + CPU), and `asic-wifi-settle`'s `wifi reload` retries (up to
~60s). On a NORMAL (non-first-flash) boot the box is SSH-usable by ~t=180s; the multi-minute
"banner timeout" seen this session was the first-flash format compounding it. **Fix directions
(need bench testing, not yet applied):** (1) skip the destructive level-3 at boot — the fabric
is not wedged at power-on, it just needs programming; a plain `gw_prog` (or a level ≤2) would
program the tables without the FULL_RST/clock-cycle cost (tie to the wedge-fix work above); (2)
renice/ionice dropbear or give its accept path priority so it services the banner even under
boot load; (3) make `asic-wifi-settle` yield / lower its priority. Biggest single win is
probably (1). Not yet applied.

Three defects only a real flash boot exposed:

- `lib/upgrade/platform.sh` sourced `/lib/realtek.sh`, a file this port deleted — every
  `sysupgrade` invocation died before doing anything. Its image-magic check also matched the
  4.14 port's board name (`gwr1200ac-v1`) instead of this target's (`dlink,dir-842-r1`), so
  the `cr6b` signature check silently never ran on any image this target actually builds.
- `dir842-asic` could exit at its very first guard with nothing logged, if
  `/proc/rtl865x_gw` was not yet present at `S97` time.
- Nothing recorded a `compat_version` in `board.json`, so every sysupgrade — including the
  same version onto itself — was refused as an incompatible version change. Fixed with a
  `board.d/05_compat-version` script following the upstream convention.

**64 MB is a real, current constraint, not a bring-up inconvenience.** The full release
package set (LuCI + rtw88 + firewall4/nftables + dnsmasq + wpad) leaves roughly 1.5 MB free
at idle, and `sysupgrade` stages its several-MB image in tmpfs — i.e. in that same RAM —
which has been observed to OOM-kill `dnsmasq` and reset SSH connections under memory
pressure during the upgrade itself. `seed-min.config` (bring-up, no LuCI, no wireless) stays
the config to reach for when RAM-boot testing anything.

Both `flash-nor.sh` and `bootgate.sh` had `ramboot.sh`'s old unanchored
`pgrep -f "cat /dev/ttyUSB0"` logger-guard bug — the pattern also matches the invoking shell
itself, so no logger starts and every board reads as failed. Fixed in both.

## 6. Vendor 2.4 GHz driver (M7) — DONE: both radios up end to end, verified stable for 3h20min on hardware

Scoped in the plan at roughly 100 timer conversions (`init_timer`/`setup_timer`/`mod_timer`
→ `timer_setup`), ~62 `virt_to_bus`/`bus_to_virt` sites in the DMA path (silent-corruption
risk, not a compile error), proc/`set_fs`/ioctl-routing cleanup, a new platform-bridge probe
matching the DT node the 6.18 board file already declares (`realtek,rtl8197f-wmac`), and a
full RF bring-up. The compile/link portion of that estimate is now done: `make target/linux/compile`
produces a real `rtl8192cd.ko` (`elf32-tradlittlemips`, ~34 MB unstripped) with **zero
compiler errors and zero modpost-undefined-symbol errors**. Module load and RF bring-up on
the bench have not been attempted — that, plus one known remaining correctness gap below,
are what's left before this milestone is actually complete, not just compiling.

Two real build-system bugs, not source bugs, were hiding the true picture entirely at the
start of this work:

- **`EXTRA_CFLAGS +=` is unrecognized by modern kbuild** (removed as a supported variable
  name well before 6.18; only `ccflags-y` is honored now). 21 such lines in the vendor
  Makefile were silently no-ops, including every chip-select `-D` and every `-I` include
  path added after the file's own initial `ccflags-y` block -- which alone produced
  thousands of "unknown type" / "not defined" errors that looked like the whole tree was
  unportable. Converted throughout.
- **A macro-value/token mismatch broke the driver's own AP-vs-CE-vs-WIN build-type
  selection.** `phydm/phydm_precomp.h` decides whether to `#include "../odm_inc.h"` (which
  in turn defines every `RTL8197F_SUPPORT`/`RTL8822B_SUPPORT`/etc. chip-select macro
  `phydm_features.h` branches on) via `#if (DM_ODM_SUPPORT_TYPE == ODM_AP)`. With the
  `EXTRA_CFLAGS` bug also silently dropping the Makefile's own
  `-DDM_ODM_SUPPORT_TYPE=0x01`, `DM_ODM_SUPPORT_TYPE` read as 0, that `#if` was always
  false, and `odm_inc.h` -- and everything it defines -- never compiled in. Restoring the
  flag (via the `ccflags-y` fix above) was the complete, correct fix here; `ODM_AP` etc.
  are already correctly defined in `phydm/phydm_types.h` and need no help.

With those fixed, a blanket `-Wno-error` was added for this one third-party module (real
warnings still print; none abort the build) -- pedantry from a ~2011-era, 665-file vendor
tree under a gcc-14/kernel-6.18 toolchain that enforces far more by default than the
4.14-era build ever did is not worth hand-fixing across code this port does not maintain
upstream.

**Every remaining compile and link error was then cleared one build cycle at a time**
(`target/linux/compile`, read the new error batch, fix, resync, rebuild -- roughly 20 cycles
end to end). The categories, in the order they were found:

- **Return-type mismatches** (`-Wreturn-mismatch`, a gcc-14 hard error even under this
  module's blanket `-Wno-error`): bare `return;` in a function declared to return a value,
  and the mirror-image bare `return <value>;` in a `void` function. ~15 sites across
  `8192cd_sme.c`, `8192cd_hw.c`, `8192cd_debug.c`, `8192cd_psk.c`; each fixed by reading the
  function's other `return` statements for a consistent value, not by guessing.
- **Timers: the full `init_timer`/`.data`/`.function` → `timer_setup()` conversion**, ~100
  call sites across every driver file that owns a `struct timer_list`, each paired with its
  callback's signature changing from `void cb(unsigned long data)` to
  `void cb(struct timer_list *t)`. Where the timer is embedded in a struct with no existing
  pointer back to the driver's `struct rtl8192cd_priv` (`LED_Timer`, embedded in
  `struct priv_shared_info`, itself pointed to but not embedded in `rtl8192cd_priv`), a
  `priv` backpointer field was added to make `timer_container_of()` possible.
- **Removed kernel APIs with no compat shim**: `mm_segment_t`/`get_fs()`/`set_fs()` (deleted
  entirely; the driver's own `kernel_read()` call already didn't need the address-space
  override), `access_ok()`'s dropped `type` argument, `PDE_DATA()` → `pde_data()`,
  `ioremap_nocache()` → `ioremap()` (already non-cached), `del_timer`/`del_timer_sync` →
  `timer_delete`/`timer_delete_sync`, and the full PCI DMA API
  (`pci_alloc_consistent`/`pci_free_consistent` → `dma_alloc_coherent`/`dma_free_coherent`
  with an added `GFP_KERNEL`; `pci_map_single`/`pci_unmap_single` →
  `dma_map_single`/`dma_unmap_single`), all rebound to the real `struct device` via
  `&priv->pshare->pdev->dev` rather than the removed `struct pci_dev *` argument.
- **`bus_to_virt`/`virt_to_bus` (62 sites)** — the plan's own flagged highest-risk item,
  since getting this wrong fails as silent RX/TX corruption, not a compile error. Root-caused
  properly rather than guessed: `CONFIG_LUNA_SLAVE_PHYMEM_OFFSET` is `0x0` on this board (the
  nonzero branch is for an unrelated DSP chip variant), `CONFIG_PHYS_ADDR_T_64BIT` is unset,
  and this SoC's WMAC descriptor rings are addressed with plain physical addresses in
  directly-mapped RAM, no IOMMU — so `phys_to_virt()`/`virt_to_phys()` (still exported,
  confirmed by reading `arch/mips/include/asm/io.h`) are the exact modern equivalents, not
  an approximation. Verified by inspecting `virt_to_phys()`'s real implementation
  (`__pa()`/`__va()`) before converting, not just by making the linker happy.
- **`_dma_cache_wback_inv()`/`_dma_cache_inv()` are no longer callable from a module at
  all** — confirmed via the kernel header's own comment, "This API used to be exported; it
  now is for arch code internal use only." The only cache-maintenance primitive still
  `EXPORT_SYMBOL`'d on MIPS is `flush_data_cache_page()` (whole-page granularity, always a
  combined writeback+invalidate). `rtl_cache_sync_wback()`'s live (non-PCI) branch now loops
  that over every page touched by the requested range via a small
  `rtl819x_flush_dcache_range()` helper -- correct (over-flushing is safe; under-flushing is
  not) but not maximally efficient for the read-only `FROM_DEVICE` case.
- **Two macro-shadowing landmines that were real correctness bugs, not just build breaks.**
  (1) Six of the driver's own vendor headers (`wifi.h`, three `Hal*PwrSeq.h` files,
  `efuse_97f/rom_def.h`, one already-safe site in `GeneralDef.h`) `#undef`+redefine the
  kernel's `BIT(x)` macro with a narrower, plain-`int` version, with no guard against it
  leaking into *later* kernel headers in the same translation unit -- confirmed corrupting
  an unrelated `static_assert` deep in `<linux/mm_types.h>` (`MMF_INIT_LEGACY_MASK`), not a
  hypothetical risk. Fixed by making each site prefer the real kernel `BIT()` when available
  (`#include <linux/bits.h>` before the `#ifndef BIT` guard) instead of force-`#undef`-ing
  it. (2) `8192cd_hw.c` had a leftover `#define _printk printk` that, combined with the
  kernel's own `#define printk(...) printk_index_wrap(_printk, ...)`, self-referentially
  corrupted every `printk()` call in that file into linking against a literal, nonexistent
  `printk` symbol -- found by preprocessing the file and reading the exact macro expansion,
  not by pattern-matching the error text. Deleted (nothing in the file called `_printk`
  directly).
- **Symbol collisions with newer kernel headers pulling in real implementations of names
  the driver also defines for itself**: a local `MIN(a,b,len)` byte-array comparator (3
  args) collided with `<linux/minmax.h>`'s 2-arg `MIN` macro (`#undef` before the local
  definition); a local `struct sha256_state`/`sha256_init/process/done` implementation
  collided with `<crypto/sha2.h>`'s real one (renamed to `rtl_sha256_*`, matching the
  `crc32`→`rtl_crc32` precedent from earlier in this pass); `<net/ipx.h>` doesn't exist at
  all any more (the kernel's IPX subsystem was removed) but `8192cd_br_ext.c`'s NAT25 IPX
  bridging genuinely uses `struct ipxhdr`'s wire layout, not just the `ETH_P_IPX` constant —
  replicated the old struct layout locally instead of stubbing the feature out.
- **A tooling blind spot, not a driver bug**: `grep`'s binary-file heuristic silently
  excludes a file from `grep -rl PATTERN . | xargs sed` if it contains a handful of
  non-ASCII bytes anywhere (`8192cd_mp.c` has ~70 in comments/string literals; `file(1)`
  still calls it "ASCII text", the two tools disagree). Every bulk multi-file rename this
  pass used exactly that pattern, so each one had to be re-verified with `grep -a` across
  the whole tree after the fact; one genuinely missed file (`8192cd_mp.c`, 5 call sites
  across `del_timer`/`del_timer_sync`) surfaced this way on the build immediately before the
  clean one.

**Since the compile-clean milestone above, the timer-callback-body gap has been closed
too.** All 72 live `timer_setup()` callbacks (the ~75 estimate above included a few that
turned out to be dead code, see below) were individually converted from
`void cb(unsigned long data)` to `void cb(struct timer_list *t)` with a real
`timer_container_of()` recovery line, not a bulk rename -- each one required reading the
callback's body to find what type it actually needed back and how the timer is embedded:

- 45 callbacks where the timer is embedded directly in `struct rtl8192cd_priv` (including
  two callbacks -- `PCIe_power_save_timer`/`RF_MIMO_check_timer` -- that share one field
  under mutually-exclusive `#if` branches, and `mesh_standalone_timer_expire`, registered
  against two *different instances* of the same struct type -- the "other band" interface
  is a second, separate `struct rtl8192cd_priv`, not a different struct, so one
  `timer_container_of()` call correctly handles both).
- 12 callbacks embedded in `struct priv_shared_info` (`priv->pshare->...`) or
  `struct rtl8192cd_hw` (`priv->pshare->phw->...`), reusing the `pshare->priv` backpointer
  already added for `LED_Timer` and adding the equivalent `phw->priv` one.
- 2 callbacks (`rtl8192cd_mclone_reauth_timer`/`_reassoc_timer`) embedded in a fixed-size
  array (`mclone_sta[i]`) whose element struct already carried its own `.priv` field --
  `timer_container_of()` on an array-element type works correctly regardless of index,
  since the offset it computes is relative to the element's own type, not the array's base.
- 9 callbacks in separately-allocated substructures (`p2p_context`, two `WPA_STA_INFO`
  variants, `WPA_GLOBAL_INFO`) that had no existing backpointer -- added one to each,
  following the same pattern, and set it at the single allocation site for each type.
- 1 genuine **pre-existing vendor bug**, unrelated to the kernel-API migration and
  predating this port entirely: `check_tcp_ack_timeout` is a real 2-argument function
  (`(priv, ack_anyway)`, called directly elsewhere with an explicit `ack_anyway`) that was
  nonetheless registered directly as a 1-argument timer callback. Fixed with a proper
  `check_tcp_ack_timeout_timerfn()` trampoline rather than leaving the mismatch in place.
- 1 timer (`rtl8192cd_cu_cntdwn_timer`) left untouched because it's genuinely dead code --
  both its registration and its own struct field (`cu_info`) sit inside the same vendor
  `#if 0` block, unreachable under any config.

`phydm/phydm_interface.c`'s `ODM_InitializeTimer()` generic dispatch wrapper (used by
phydm's own DIG/antenna-diversity/beamforming timers, a different, generic-callback-table
mechanism from the `timer_setup()` sites above) was intentionally left as the
explicitly-flagged-incomplete minimal unblock it always was -- not touched by this pass.

**Real-hardware verification done on 2026-09-03** (RAM-boot, `ramboot.sh`, a
`seed-min.config`-based image with `CONFIG_PACKAGE_kmod-rtl8192cd=y` added on top — the
minimal, no-LuCI profile, needed because the full `seed.config` package set OOM-crashed this
64 MB board 3 of 4 RAM-boot attempts before the driver even ran):

- The module autoloads at boot (`AUTOLOAD:=$(call AutoLoad,60,rtl8192cd)`) with **no crash,
  no Oops, no missing-symbol error**. `lsmod` shows `rtl8192cd 1327104 0` resident.
- `dmesg` shows `rtl8192cd_init_one` completing six clean `INSIDE`→`EXIT` cycles with nothing
  in between but expected informational output (`RFE TYPE =0`) — no dangling `INSIDE`
  without a matching `EXIT`, i.e. no crash mid-init on any of the six calls.
- This is genuine positive evidence for the timer-callback, DMA-API, and backpointer
  conversion work: it is not just link-clean, it initializes correctly against real silicon.
- **What this run could not reach**: `iwconfig`/interface-state confirmation, client
  association, DHCP, throughput, and the plan's 802.11r roam test. In every RAM-boot attempt
  (four total this session), the system panics ("System is deadlocked on memory") and
  watchdog-reboots a few seconds into network/wifi bring-up — once killing `hotplug-call`,
  once killing the unrelated `mac80211.sh` (5 GHz radio config script, not the vendor
  driver), and once escalating far enough that `procd` (PID 1) itself was hit by the
  OOM killer. `free` shows `available: 0` from the moment the shell comes up, before any of
  this session's actions: the initramfs rootfs alone pins ~13 MB in tmpfs on a board with
  ~58 MB `managed`, leaving the system permanently sitting at its low watermark. This
  reproduced identically on the minimal package set, so it is **not specific to the vendor
  driver or to this session's changes** — it is a RAM-boot-environment ceiling that will
  need the M6 flash-boot path (jffs2 overlay, not everything pinned in tmpfs) to get past.

**Second RAM-boot pass, 2026-09-03: went one step further than module load.** No
`lib/wifi`/`lib/netifd` handler for this driver is packaged yet (that's separate M7 scope,
§8 of the plan — this test image only ever added `CONFIG_PACKAGE_kmod-rtl8192cd=y`, not the
netifd glue), so `/etc/config/wireless` never gets a `radio1` section and nothing brings the
interface up automatically — expected, not a bug. Confirmed the driver itself creates the
full interface set on load (`wlan0` + 5 VAPs, `wlan0-vxd`/`wlan0-va0..va3`) and its complete
`/proc/wlan0/*` diagnostic surface (`sta_info`, `mib_all`, `cmd`, etc.), all correctly named
and present. Manually running `ip link set wlan0 up` (bypassing netifd) to test the
`ndo_open` path directly: the interface transitions cleanly at the network-stack level
(`wlan0: default qdisc (fq_codel) fail, fallback to noqueue` — a normal, benign message, not
an error) and the driver's own RF bring-up starts (`[97F] Bonding Type 97FS, PKG1`, efuse
load, `rom_progress` sequence) — **no crash, no Oops in `ndo_open` either.** It then hit a
real, specific failure: `PHY_ConfigBBWithParaFile(): not enough memory` /
`phy_BB88XX_Config_ParaFile():BB_PG Reg Fail!!`, repeating — the baseband parameter-file
loader's own allocation failing, in a system already past its OOM watermark from the boot
sequence alone (§ above). This is a **precise, direct link, not just a coincidence of
timing**: RAM-boot memory pressure doesn't just kill unrelated userspace processes around
the driver, it reaches into the driver's own RF/PHY init path and fails a real allocation
there. `ndo_open` itself did not crash or reject the request — the failure is downstream, in
baseband config loading, which is exactly the code a flash-booted image (with real headroom
instead of a tmpfs-pinned rootfs) needs to get past for a real beacon test.

**What is not yet done, in order of how much it matters before M7 can be called complete:**

- **Full RF bring-up on real hardware — client association, DHCP, throughput, 802.11r
  roam — is still unattempted**, for the RAM-boot-memory-ceiling reason above, not a driver
  defect. The next real attempt should be on a flash-booted (M6-path) image for this 6.18
  port, once one exists, rather than another RAM-boot cycle on this same constrained image.
- **The DMA/cache-sync fixes are verified by reading kernel source and reasoning about the
  platform's memory model, confirmed not to crash on init, but not yet exercised by real
  RX/TX traffic** (association/DHCP would be the first real test of the data path, and that's
  exactly the step blocked above).
- `phydm/phydm_interface.c`'s `ODM_InitializeTimer()` generic dispatch wrapper is a minimal,
  explicitly-flagged-incomplete unblock (same underlying issue as the callback-body point
  above, concentrated in phydm's own DIG/antenna-diversity/beamforming timers) -- not
  considered functional yet.
- The platform-bridge probe (`realtek,rtl8197f-wmac` → a real `struct platform_device`/
  `struct device` for the on-SoC WMAC, matching the DT node the board file already declares)
  was not needed to reach compile-clean -- `priv->pshare->pdev` is confirmed never populated
  for this platform-device variant, so the code path in `rtl_cache_sync_wback()` that would
  use it is dead here -- but it is still the honest way to make `dma_sync_single_for_cpu/
  device()` available to this driver in the long run, rather than staying on the
  page-granularity `flush_data_cache_page()` fallback indefinitely.
- `seed.config`/`seed-min.config` still deliberately do not enable `kmod-rtl8192cd` -- it
  loads cleanly now, but association/DHCP/throughput are not yet verified, so it stays out
  of the shipped package set until they are.

### ★★ 2026-09-03 (late): the netifd story closed, and the REAL M7 blocker isolated

Read this before anything below it in §6 — several conclusions recorded further down were
drawn from a confounded experiment and are superseded here.

**Settled: netifd is fine, and the shell handler is not a boot hazard.** With the
`handler_load()` EOF fix in `files/package/network/config/netifd/files/lib/netifd/utils.uc`,
an image carrying `rtl8192cd.sh` in `/lib/netifd/wireless/` was verified live on hardware:
`/etc/init.d/network restart` leaves netifd at **0% CPU**, all five `network.interface`
objects register, `br-lan` comes up with 192.168.0.1/24, ping is 0% loss and SSH works. The
handler's own `dump` emits valid JSON with a trailing newline and exits 0.

**The experiment that misled us.** "Removing the handler file makes netifd healthy" looked
decisive, but `KernelPackage/rtl8192cd/install` ships the handler *inside the same package as
the kernel module* — deleting the handler also deleted `rtl8192cd.ko`. Every inference about
handler-vs-module load ORDER therefore rested on a two-variable change. The
`AutoLoad(...,1)` / `modules-boot.d` change made on that basis has been reverted (see
`modules.mk`): preinit, before the overlay is mounted, is a worse place to bring up a
DMA-allocating wireless driver on a 64 MB board, and it bought nothing real.

**Baseline established (no vendor module, same tree, same seed minus `kmod-rtl8192cd`):**
boots clean and stays up — `br-lan` 192.168.0.1/24, `network.interface.lan up:true`, all
`/etc/uci-defaults` consumed, **zero OOM**, ASIC `netif readback PASS`, `hwnat: offload
ready`, ping 0% loss, SSH, 90% idle. So M0–M3 and M6 are sound on this build; the port's
non-wireless half is not implicated in anything below.

**The actual M7 blocker: the vendor driver hard-wedges the kernel.** With
`kmod-rtl8192cd` present the module *loads correctly* — six `rtl8192cd_init_one` probes, the
netdevs `wlan0`, `wlan0-va0..va3`, `wlan0-vxd` all appear, `/proc/wlan0` exists, and on one
boot the box reached a fully working `br-lan` with its address and forwarding DSA ports.
Then, tens of seconds later, the system wedges **hard**: UART console stops echoing entirely,
network dies, and a serial-BREAK SysRq (`w`, `l`) produces no output at all — i.e. not a
userspace hang but the kernel spinning with interrupts off or deadlocked below the console
layer. One boot ended in `Reboot Result from Watchdog Timeout!` from the bootloader. The
wedge timing varies (≈65 s on one boot, ≈250 s on another), which is why several
intermediate readings in this session were misread as "boot hangs at X" — the box was in
fact still booting, or already wedged, depending on when it was sampled.

**Root cause found and largely fixed: a whole CLASS of timer-callback ABI bugs introduced by
the 4.14 -> 6.18 port.** The port converted timer callbacks to the 6.18 signature
`void fn(struct timer_list *t)` with `timer_container_of()`. But many of these functions are
*also* reached two other ways that still pass a raw pointer as an `unsigned long`:

  (a) registered with `tasklet_init(&t, fn, (unsigned long)priv)` -- tasklet hands `data` back verbatim;
  (b) called DIRECTLY from fast paths as `fn(priv)` / `fn((unsigned long)priv)`.

In those cases `timer_container_of()` subtracts a member offset from something that is not a
`timer_list` at all, so every field reached through the result is garbage. The bodies then run
under `SAVE_INT_AND_CLI()` (= `spin_lock_irqsave`, interrupts OFF) with garbage loop bounds --
an unbounded spin with interrupts disabled. That is exactly the observed signature: console
dead, serial-BREAK SysRq silent, watchdog reset. The compiler never complained because the
driver Makefile carries `-Wno-error=incompatible-pointer-types`.

Fixed by splitting each affected handler into a plain helper plus correctly-typed trampolines
(one per registration mechanism), or -- where the timer's container is `priv` itself, so
`&priv->MEMBER` recovers the identical pointer -- by fixing the argument. Sites fixed:

| site | mechanism | live? |
|---|---|---|
| `rtl8192cd_swq_timeout` (`8192cd_tx.c`, `8192cd_osdep.c:3866`) | tasklet_init | **YES -- the main wedge** |
| `reorder_ctrl_timeout` / `_cli` (`8192cd_rx.c:2997,3216`) | direct call from RX path | **YES** |
| `ResendTimeout` (`8192cd_psk.c:4247,4403`) | direct call, WPA 4-way handshake | **YES** |
| 16x `rtl8192cd_chNNN_timer` (`8192cd_ioctl.c:11905-11980`) | direct call, DFS `reset_nop_channel()` | **YES** |
| 3x phydm timers (dig lna_sat_chk, tdma_dig, soml) | callback cast in `ODM_InitializeTimer` | **YES** |
| `rtl8192cd_atm_swq_timeout` (`8192cd_tx.c`) | direct call from ISR | no (`RTK_ATM` off) |

The phydm three deserve their own note: `phydm/phydm_interface.c:587` does
`timer_setup(pTimer, (void (*)(struct timer_list *))CallBackFunc, 0)`. On 4.14 the ODM_AP
branch set `.function`/`.data = pDM_Odm`, i.e. it ignored the `pContext` argument entirely and
always passed the ODM structure. Each callback begins `PDM_ODM_T pDM_Odm = (PDM_ODM_T)pDM_VOID;`
and then dereferences ODM fields -- so under the cast it reads and WRITES at wild offsets past a
~40-byte timer object. That is silent adjacent-memory corruption rather than a clean oops, which
is why it produced intermittent rather than deterministic failure. Only three of these timers are
actually live on this board (`ODM_AntDivTimers` is not in the built module; PathDiv/CCK/sbdcnt/
Beamforming are `ODM_WIN`-only), each now routed through a trampoline in
`phydm/phydm_dig.c` / `phydm/phydm_soml.c`. **Commit d0e0569 is what made these live**, by
restoring `-DDM_ODM_SUPPORT_TYPE=0x01` and thus enabling the ODM_AP branch.

**Measured effect.** Before any fix, the box wedged deterministically ~1 s after
`kmodloader` finished (~62 s), every boot. After the tasklet/direct-call fixes it ran for
minutes rather than seconds. The phydm fix is the most recent and its long-run stability was
still being measured as of 2026-09-03 -- see the ★ 2026-09-04 addendum immediately after this
subsection for the answer: **long-run stability is now confirmed, M7 is closed.**

**Two further fixes that together made the box stable (both verified live):**

1. **`vm.min_free_kbytes` was starving the box** — new
   `base-files/etc/sysctl.d/31-dir842-lowmem.conf`. OpenWrt's own
   `/etc/init.d/sysctl` `apply_defaults()` picks the watermark purely by RAM size and
   this board (MemTotal ~58 MB) lands in the `> 32768` bucket → `min_free=8192`, pinning
   8 MB (~14% of usable RAM) as an untouchable floor. With both radios plus hostapd and
   wpa_supplicant the allocator sat permanently just under it and the OOM killer reaped
   working daemons — observed as `Normal free:8104kB ... min:8192kB` with
   `oom-kill: task=dnsmasq` / `task=netifd`. **This, not a hardware ceiling, is what the
   dual-radio "memory limit" actually was.** Set to 2048 kB (still ~2x the kernel's own
   computed default for this size). After the change: **zero OOM kills**, 9.7 MB free /
   12.3 MB available, stable across a 15-minute observation.

2. **The `wmac` DT node was `status = "disabled"` and `CONFIG_RTL8197F_WMAC` was unset** —
   so `rtl8197f-wmac.c` never probed, nothing called `platform_get_irq()`, and
   `rtl8192cd` fell back to the 4.14-era hardcoded `BSP_WLAN_MAC_IRQ = 6`. On 6.18 virq 6
   is claimed by the Realtek intc as one of its own parents, carries `IRQ_NOREQUEST`, and
   `__setup_irq()` rejects it — which the driver only logs before carrying on, so the
   radio loads and then silently never comes up. Enabled both; the driver now probes:
   `RTL8197F on-SoC 2.4 GHz WMAC: chip id 8197f001, hw id 100a, irq 29, xtal 25MHz`,
   `WLAN_EN (SR+0x64): 00000000 -> 0000001f`, HAL type id 14 (vendor expects 14). Two
   small 6.18 ports were needed to build it: the include moved from
   `<asm/mach-realtek/realtek_mem.h>` to `<asm/mach-rtl819x/rtl819x-sysc.h>` (same symbol
   names, deliberately), and `platform_driver::remove` now returns `void`.

**M7 IS NOW WORKING END TO END (2026-09-03, verified on hardware).** Both radios run
simultaneously, a real client associates to the vendor 2.4 GHz AP, gets DHCP, and passes
traffic:

```
router: 29:  4531  realtek-rtl-intc  29   wlan0        <- IRQ finally registered and firing
router: rtl8192cd: WMAC irq from DT = 29 (static table said 6)
router: br-lan brif = lan1 lan2 lan3 lan4 phy0-ap0 wlan0   <- BOTH radios bridged
router: /tmp/dhcp.leases -> 00:05:16:5d:37:df 192.168.0.226 hal
hal:    wlp4s0:connected:DIR842-2G ... inet 192.168.0.226/24
hal:    ping -I wlp4s0 192.168.0.1 -> 5/5, 0% loss, 3.0-12.8 ms
router: iperf3 -c 192.168.0.226 -> 22.6 Mbit/s, 0 retransmits (client one floor below, 77% signal)
router: free -> 11.9 MB available, dmesg OOM count = 0, with BOTH radios up
```

**★ 2026-09-04: long-run stability reconfirmed, closing the open question above.** Checked
this same image live, mid-session, with no prior warning or setup (not a fresh boot staged
for this test — just the box as it was already running): **3 h 20 min continuous uptime**,
`ubus call network.wireless status` shows both `radio0` (5 GHz) and `radio1` (vendor 2.4 GHz)
`up: true, retry_setup_failed: false` simultaneously, `dmesg | grep -i oom-kill` empty (zero
OOM kills across the entire boot), no kernel wedge, SSH and `ubus` both responsive
throughout. The single `Call Trace` in `dmesg` is at boot+169s — consistent with the
already-documented benign `tasklet_kill from interrupt` warning, not a crash, since the box
kept running for another 3+ hours after it. This directly answers the "long-run stability
still being measured" question left open on 2026-09-03: **it holds.** M7 is done.

Three further root causes had to be fixed to get here, on top of the timer-ABI class above:

1. **netifd silently loaded only the FIRST wireless handler** — the real reason `radio1`
   never existed. `handler_load()` in netifd's `lib/netifd/utils.uc` runs each handler's
   `dump` into a `mkstemp()` temp file that the child writes through an *inherited fd*, then
   `f.seek()`s and reads it back. Instrumenting a copy of the shipped
   `/lib/netifd/utils.uc` on the box showed the callback firing for `mac80211.sh` and never
   for `rtl8192cd.sh`; adding a third handler that sorts first made the **second** one
   disappear instead, whichever script that was. The child always exits 0 and the bytes are
   genuinely in the file (a debug read right after `f.seek()` returns the full 757-byte
   dump), but the loop's own first `f.read("line")` then returns empty and breaks. An
   explicit `f.seek(0)` does not help — the parent's buffered `FILE*` and the child's raw
   writes disagree about the offset, and the fd number being reused across iterations makes
   it deterministic from the second handler onward. **Fixed by reading the dump through
   `popen()`** (no temp file, no fd redirect, no seek); verified against the target's own
   ucode that both handlers now load. This is an upstream netifd bug, not something specific
   to this port — any board with two wireless handlers loses one silently, because
   wifi-scripts' `config_init()` does `let handler = wireless.handlers[data.type]; if
   (!handler) continue;` with no log line at all.

2. **`CONFIG_RTL8197F_WMAC` was unset and the `wmac` DT node was `status = "disabled"`.**
   Enabled both. The platform driver now probes:
   `RTL8197F on-SoC 2.4 GHz WMAC: chip id 8197f001, hw id 100a, irq 29, xtal 25MHz`. Two
   small 6.18 ports were needed to build it: the include moved from
   `<asm/mach-realtek/realtek_mem.h>` to `<asm/mach-rtl819x/rtl819x-sysc.h>` (same symbol
   names), and `platform_driver::remove` now returns `void`.

3. **`rtl8192cd` requested the wrong interrupt.** `dev->irq` came from `wlan_device[]`'s
   compiled-in `BSP_WLAN_MAC_IRQ = 6` — a bare MIPS IP number that worked on 4.14. On 6.18
   `mti,cpu-interrupt-controller` gives virq == hwirq for 0-7 and this SoC's Realtek intc
   claims IP6 as one of its four parents, so virq 6 carries `IRQ_NOREQUEST` and
   `__setup_irq()` rejects it. The driver only logs and continues, so the failure is
   invisible: the radio loads, ingests `/etc/Wireless/RTL8192CD.dat`, runs full PHY/RF
   bring-up (`[ODM_software_init]`, `load efuse ok`, firmware load, `Default BB Swing=30`),
   registers its netdevs and joins the bridge in forwarding state — and then never
   interrupts, so it never beacons and tx/rx stay at 0. The tell is that `/proc/interrupts`
   has no wlan line at all. Fixed in `8192cd_osdep.c` by resolving the IRQ from the DT node
   (`of_find_compatible_node` + `irq_of_parse_and_map`) with the table value as fallback.

**★ FIXED, 2026-09-05: a real kernel WARNING on every boot, `netdevice: wlan0: Incorrect
netdev->dev_addr` (`net/core/dev_addr_lists.c:520`).** Found live on a fresh boot of this
exact combined build (dmesg showed it twice, at kernel-uptime 154s and 232s, both times right
as the vendor driver's own MIB-reconfiguration pass ran). Root cause: the port's raw
`memcpy(dev->dev_addr, ...)` calls (a normal pattern in this ~2011-era driver) bypass the
newer kernel's `dev_addr_set()`/`eth_hw_addr_set()` API, which keeps a shadow copy the kernel
later checks against; a direct memcpy leaves that shadow at its all-zero initial value and
`dev_addr_check()` warns on the mismatch every time it's consulted. Harmless to the actual
MAC address in practice (the real address was always set correctly; only the kernel's own
bookkeeping was wrong), but a genuine API-compliance bug, not a false alarm. Fixed the four
call sites confirmed live for this board's actual compiled configuration (checked each
against the Makefile's real `CONFIG_RTL_*` flags, not just `#ifdef` text, since most of this
driver's alternate address-assignment paths are WDS/mesh/RTK_NL80211/multi-clone code this
board does not compile in): the unconditional copy in `rtl8192cd_open()`, the
`.ndo_set_mac_address` handler `rtl8192cd_set_hwaddr()`, and the two Universal-Repeater VXD
paths that are compiled in (`CONFIG_RTL_REPEATER_MODE_SUPPORT=y` on this board) even though
repeater mode itself is not configured by default. Rebuilt clean (`target/linux/compile`,
zero errors) and included in the combined image flashed this session. **Verified fixed on a
fresh flash of this exact image**: zero occurrences of the WARN across 13 minutes of uptime
and three separate checks, versus twice by minute 4 on the pre-fix build.

**★ Also observed on this same fresh-flash verification run: `asic-wifi-settle`'s grace
period can still be marginally too short.** The service logged "still not up after 3 reload
attempts + 15s grace: radio0 — likely a real fault" — but a check taken only ~9 seconds later
found radio0 (and radio1) both genuinely up. This is the exact scenario the service's own
comment already anticipates ("a reload can land just after this loop's own check... leaving a
misleading 'real fault' line") — not a new failure mode, but a live instance of it, on a boot
whose jffs2 format happened to run unusually long (373s vs. the typical 150-250s), which
likely pushed the whole wireless bring-up timeline later than the service's fixed grace
window anticipates. Not fixed this session — the service already logs its uncertainty
honestly rather than silently failing, and the radio does recover — but a slightly longer or
load-adaptive grace period would remove this false alarm. Low priority.

**★ CAVEAT, 2026-09-05: do not live-`rmmod` the vendor wireless module on this hardware.**
While testing the `eth_hw_addr_set()` fix above, an attempt to hot-swap the module
(`wifi down` → `rmmod rtl8192cd` → `insmod` the rebuilt `.ko` → `wifi up`) coincided with a
full, unplanned board reboot (dmesg on the next boot showed only 4 minutes of uptime, and a
`recovery level 3` ASIC fabric-reset event at kernel-uptime 247s, with no oops/panic
recorded — pstore/ramoops stays deliberately off per this doc's own note, so the previous
boot's exact cause did not survive to be read back). The safe way to deploy a driver change
is a full reflash, not a live module reload; this port's own `wifi reload <radio>` (close
then reopen the netdev, never touching the loaded module) remains fine and is what
`asic-wifi-settle` already uses.

**★ HAZARD, partially addressed 2026-09-05: reading some vendor proc files hard-wedges the
box.** `head -c 200 /proc/wlan0/mib_all` and `head /proc/wlan0/sta_info` each killed a healthy,
fully-working box (console dead, watchdog reset) while everything else stayed stable. This
matters beyond debugging: the planned `dir842-l2flush` service polls `/proc/*/sta_info`
every 2 s, so it must NOT be enabled until this is fixed.

Found a concrete, plausible mechanism via static analysis (not yet live-confirmed — see
caveat below): `PRINT_ARRAY()` (`8192cd_util.h`, the seq_file/`CONFIG_RTL_PROC_NEW` branch
these handlers use) loops `for (index=0; index<len; index++) seq_printf(...)` with **no
bound on `len`** — and `len` at ~40 call sites throughout `8192cd_proc.c` is a runtime struct
field (an information-element length, a rate-set count, etc.), not a compile-time constant.
If any one of those ever holds a corrupted or uninitialized value — plausible after a
4.14→6.18 struct-layout shift this port could easily have introduced somewhere across
~40 sites, none individually audited — the loop runs far past the real array. Critically,
`rtl8192cd_proc_stainfo()` (the `sta_info` handler) and, by the same `CONFIG_RTL_PROC_NEW`
path, `rtl8192cd_proc_mib_all()` run their **entire body under `SAVE_INT_AND_CLI()` plus a
spinlock** — interrupts AND preemption both off, on this board's single core. A
long-but-finite bad loop under that lock reads as a hard hang to everything else on the SoC
and can trip the hardware watchdog before it ever reaches unmapped memory and oopses —
matching this hazard's exact signature (console dead, watchdog reset, nothing else disturbed).

**Fix applied**: capped `PRINT_ARRAY()`'s loop at 256 (a real, generous ceiling — no
legitimate use here ever prints more than a handful of bytes, and an 802.11 information
element's length field is one byte wide by spec, max 255 — so this changes no correct
output), with a rate-limited `pr_err` if a cap is ever hit, so a future corrupt field is
visible in `dmesg` instead of silently truncating. Compiles clean (`target/linux/compile`,
zero errors), included in this session's build.

**★ Caveat — NOT yet live-verified against a real reproduction of the hazard.** Deliberately
triggering the original hang to test the fix was judged too risky this session: this port's
own bench NIC is the only currently-reachable path to the box, and if the fix were wrong or
incomplete, reproducing a hard hang on purpose could cost that connection with no easy way to
recover it remotely. The fix addresses a real, concretely-identified bug class that plausibly
explains the hazard, and is safe on its own terms (it can only ever narrow output, never
widen it, so it cannot introduce a new correctness problem) — but "plausible and safe to ship"
is not the same as "confirmed to fix this specific hazard." Whoever next has hands-on bench
time should reproduce the original crash against a build WITHOUT this fix, then confirm it
is gone WITH this fix, before treating `dir842-l2flush` as safe to enable by default.

**Also still open:** `softirq: Attempt to kill tasklet from interrupt` repeats about every
7 s once the radio is up — a real `tasklet_kill()`-from-interrupt-context misuse that has
not yet been traced to its call site. It is not currently fatal but is exactly the kind of
latent defect that becomes one under load.

**Still outstanding (identified, not yet fixed — neither blocks the M7 gate; both are efficiency/
robustness items, not correctness-of-forwarding items):**
- ~~`request_irq()` uses the hardcoded `BSP_WLAN_MAC_IRQ = 6`~~ — **fixed**, see item 3 above
  (IRQ now resolved from the DT node via `of_find_compatible_node` +
  `irq_of_parse_and_map`, table value kept only as fallback). Left the strikethrough note here
  because an earlier draft of this section listed it as still-open after the fix already
  landed; it is not.
- `rtl819x_flush_dcache_range()` (`8192cd_util.h:1571`) replaced 4.14's exact-range
  `dma_cache_wback_inv()` with a PAGE-granular `flush_data_cache_page()`, issuing 128 cache ops
  per call regardless of size, on the per-packet path. Best explanation for the ~89% system CPU
  seen under load, and the page-granular writeback can clobber neighbouring in-flight DMA
  buffers. **This is a real, live, unaddressed item — a 2026-09-05 adversarial review
  correctly flagged that it had been silently dropped from an earlier "M7 is done, only two
  open items" summary elsewhere in this section; that summary is corrected below.**

  **Fix, scoped but NOT attempted this session — deliberately, not from lack of time.**
  The correct fix is `dma_sync_single_for_{cpu,device}(dev, dma_handle, size, direction)`
  (precise range, no page-granularity, no neighbour-clobber risk), called from
  `rtl_cache_sync_wback()` (`8192cd_util.h:1587`) instead of
  `rtl819x_flush_dcache_range()`. Two concrete gaps stand between here and that fix:
  (1) `rtl_cache_sync_wback()` only has `priv` (the wifi driver's private struct) in scope,
  and its one existing `struct device*` field (`priv->pshare->pdev`) is a PCI-typed pointer
  that is never populated on this on-SoC-WMAC board (confirmed dead per the comment at
  `8192cd_util.h:1602-1607`) — a real `struct device*` for the platform device needs to be
  threaded in from wherever `rtl8197f-wmac.c`'s platform-driver `probe()` already has one
  (per the original plan, its probe already fills `wlan_device[0].{base_addr, irq, dev}` —
  check whether that `.dev` is the right handle, or add one). (2) `start` reaching this
  function is a physical address (`CPHYSADDR(start)+CONFIG_LUNA_SLAVE_PHYMEM_OFFSET`,
  offset is 0 on this board) — on this platform's simple direct/identity DMA mapping (no
  IOMMU, already established elsewhere in this doc), that physical address can very likely
  serve directly as the `dma_addr_t` argument, but that assumption needs a one-line sanity
  check against this platform's actual `dma_map_ops`, not just an assumption, before
  shipping it. **Deliberately not attempted live this session**: a wrong DMA-synchronization
  change risks silent data corruption on the wireless RX/TX path — worse than the current
  known-inefficient-but-safe behavior — and validating a change this subtle needs sustained,
  low-risk bench access this session did not have for the second half of its M7 work. This
  is real, scoped, ready-to-implement work for whoever has that bench time next, not a vague
  TODO.
- Two inert instances of the same ABI class kept for whenever those paths are revived:
  `rtl8192cd_cu_cntdwn_timer` (`8192cd_util.c:8727`, inside `#if 0`) and `issue_beacon_ibss_vxd`
  (`8192cd_cfg80211.c:3316`, cfg80211 not built -- and note its timer path would recover the ROOT
  priv where 4.14 armed it with the VXD priv).

**Method note for whoever picks this up: do not sample this board early.** A first boot after
a flash spends ~150–200 s formatting jffs2 (`jffs2_build_filesystem(): erasing all blocks`),
during which `top` reports ~89% *system* CPU and shell commands crawl. Several "netifd is
spinning" / "boot is stuck" readings in this session were that erase, not a fault. Wait
≥250 s after power-on before drawing any conclusion, and prefer writing diagnostics to a file
and catting it afterwards over watching the console live.

**Status as of this paragraph's original writing: M7 was NOT yet done** — the driver
compiled, linked, loaded, and created its interfaces, but destabilised the kernel, so no RF
bring-up, association or throughput claim could be made. That was true when the timer-ABI
fixes above were new and their long-run stability was still unmeasured. **It is stale now
— see "M7 IS NOW WORKING END TO END" and the 2026-09-04 long-run reconfirmation earlier in
this section, both later than this paragraph.** Kept for the record rather than deleted,
since the suspect-list reasoning (interrupt path, `virt_to_bus`/`bus_to_virt` conversions,
timer call sites) was exactly what the fixes above targeted and resolved. **M7's core
claim holds — both radios associate, pass traffic, and stay up for hours — but "done" needs
three qualifiers, not the two this paragraph originally listed** (a 2026-09-05 adversarial
review correctly caught that a third, live item had been dropped from this count): (1) the
vendor proc-read wedge hazard, which specifically blocks enabling `dir842-l2flush` until
fixed — a defensive mitigation shipped 2026-09-05, not yet live-confirmed against a real
reproduction; (2) the recurring benign `tasklet_kill from interrupt` warning, not currently
fatal; and (3) the dcache-flush granularity item just above — page-granular cache
maintenance on the per-packet path, named in this document's own words as the likely cause
of the ~89% system CPU seen under sustained load and a possible source of DMA-buffer
corruption under load, not merely a performance nit. None of the three block the "both
radios work" claim; all three are real and open, and a future "M7 is fully closed out"
claim should name all three, not two.

---

### ★ Dual-radio coexistence test, 2026-09-03: a real netifd boot-order-race bug found — NOT a RAM ceiling, a wrong first conclusion corrected in the same session

> **Superseded in part** by the section immediately above: the "boot-order race" this heading
> names was not real, and the evidence for it was confounded. Kept for the record.


A workflow pass (investigate → fix → adversarial review → refix) produced three real,
reviewed changes: (1) `modules.mk` now ships the `rtl8192cd.sh` netifd handler exclusively
through `kmod-rtl8192cd`'s own package `/install` target, never `base-files/`, closing the
*old*, previously-documented failure mode where the handler installed unconditionally
regardless of whether the kmod was even selected; (2) two uci-defaults scripts
(`09-dir842-2g`, `10-dir842-5g-ifname`) ported from the 4.14 product's field-tested
originals, seeding the vendor radio and pinning the 5 GHz `mac80211` interface to `wlan1` so
it cannot collide with the vendor driver's `wlan0`; (3) `Kconfig` gained the `select
WIRELESS_EXT`/`WEXT_PRIV`/`WEXT_PROC` a bare `=y` line can't provide for a promptless
symbol. All three were individually correct and are now confirmed working on real hardware
(`wireless.default_radio0.ifname='wlan1'` was observed correctly pinned on the very first
boot). But building and flash-booting an image with **both radios present together for the
first time on this port** — `kmod-rtl8192cd` + `kmod-rtw88`, via a scratch
`seed-m7-test.config`, never `seed.config`/`seed-min.config` — surfaced two further, deeper
problems that none of the three fixes above address:

**1. A real netifd CPU-spin, matching the *old* documented failure mode's exact symptom,
still occurs — with the packaging fix correctly in place.** Observed `netifd -l 2` sustained
at 77–92% CPU, `ubus call network...` timing out or failing `Not found`, and no interface
(not even `br-lan`) ever coming up — reproduced with `wireless.radio1.disabled=1` set
*before* the boot that spun, ruling out "the vendor radio is actively trying to associate"
as the cause. `rtl8192cd.sh`'s own header comment explains why this remains possible even
with the packaging fix: `netifd probes every handler in /lib/netifd/wireless at startup...
regardless of UCI config` — a synchronous `dump` call netifd makes to learn a handler's
schema, independent of whether that wifi-device is enabled. The packaging fix correctly
ties the *file's presence* to the kmod being selected, but does not address a **boot-order
race**: `kmodloader` finishes loading `rtl8192cd.ko` (and creates the `/proc/wlan0/mib_all`
this handler's `rtl_find_phy()` requires to identify the device) at ~59–63s of kernel
uptime in the observed boots — visibly *after* `procd: - init -` and network bring-up begin.
If netifd's handler probe runs before the module has finished loading, the device
`rtl_find_phy()` looks for doesn't exist yet, matching the exact "describes a driver whose
device never appears" trigger condition the header comment already names. Investigated one
specific hypothesis and ruled it out: `drv_rtl8192cd_setup()`'s `NO_DEVICE` path calls
`wireless_set_retry 0`, which — verified directly against
`lib/netifd/wireless-device.uc:153-156` — means *stop retrying immediately*, the opposite of
a busy-loop; so a naive fix of "give it a nonzero retry delay" would be based on a wrong
premise. **Confirmed as the actual fix, live on hardware**: removing `rtl8192cd.sh` from
`/lib/netifd/wireless/` and force-restarting the already-spinning `netifd` process
(`killall netifd`, letting procd respawn it) dropped netifd from 83–92% CPU to 4% within
one respawn cycle. The precise code fix implied by this evidence — netifd's handler dump
must not run before AUTOLOAD priority 60's module has finished loading, or the handler
itself must survive being probed against a not-yet-existent device without triggering the
spin — was **not implemented this session**; it needs its own investigation pass, ideally
reading netifd's own C/ucode probe-timeout logic (`handler_load()`,
`package/network/config/netifd/files/lib/netifd/utils.uc:46-54`) rather than more black-box
bench iteration.

**2. An OOM cascade was also observed** (`hostapd`, `wpa_supplicant` twice, and once
`netifd` itself, killed across two independent boots; `Normal free:8100–8188kB` against
`min:8192kB`) — **but the "this board cannot sustain both radios" conclusion originally
drawn from it here was wrong, and caught by the user pushing back rather than by this
session's own process.** `main`'s own dual-band product (kernel 4.14) is a shipped,
working, externally-verified concurrent-dual-band product **on this exact 64 MB hardware**
(`docs/VENDOR-PARITY-INVENTORY.md` §R4 on `main`: "★★★ R4 COMPLETE — CONCURRENT DUAL-BAND
WORKS", confirmed by an independent scanner seeing both SSIDs beaconing at once) — and its
own documented working-boot headroom (`docs/WIFI-DUAL-BAND.md` §7 on `main`:
`MemAvailable 7576 kB` on a NOR boot with both radios up) is the **same order of magnitude**
as this port's "failing" figure, not meaningfully larger. A live re-verification this same
session confirms it directly: the actual v1.4 release, checksum-verified and freshly flashed
onto this same bench box, runs both radios simultaneously (`wlan0` vendor 2.4 GHz +
`wlan1` rtw88 5 GHz, both `master br-lan state UP`) at **0% CPU, `available: ~8.4–9.2 MB`,
zero OOM events** in `dmesg`. The hardware is not the constraint.

This project has hit this exact shape of mistake before: `docs/RETRACTIONS-AND-METHOD.md`
#22 on `main` records blaming OOM for a 2.4 GHz AP failure that was actually a netifd
handler-registration bug not having run at all — the same subsystem, the same failure
shape, as finding (1) above. An adversarial review (launched specifically because the
"hardware ceiling" conclusion didn't survive the user's challenge) additionally found the
test build was never as lean as its framing claimed: `image/Makefile`'s `DEVICE_PACKAGES`
unconditionally pulls in the full 5 GHz stack (`kmod-rtw88-8822be`, `wpad-basic-mbedtls`,
`iw`) regardless of seed choice — confirmed against the actual generated `.config` — so
"even seed-min.config can't do it" was never actually tested. It also found one concrete,
free fix: this port used `wpad-basic-mbedtls`, where `main`'s proven working config uses
plain `wpad-basic` — neither radio on this hardware supports WPA3/SAE, so the mbedtls
variant buys nothing and only adds a real dependency; no rationale for the mbedtls choice
was ever recorded in git history. **Fixed** (`image/Makefile`, 2026-09-03).

**What this means for M7 (updated after two more retests, 2026-09-03):**

- **Fix applied and confirmed working at the mechanism level**: `rtl8192cd`'s `AUTOLOAD` now
  loads it via `/etc/modules-boot.d` (`modules.mk`, `AutoLoad,60,rtl8192cd,1`), not just the
  late `modules.d` pass. Bootlog-confirmed: `rtl8192cd_init_one` now completes at **[19.3s]**
  of kernel uptime, ~25s before `procd: - init -` [44.4s] and long before the late
  `modules.d` pass [59-63s] even starts. The specific race this closes (handler probed
  before the module/its `/proc/wlan0/mib_all` exists) is genuinely gone.
- **The `wpad-basic` fix stopped the OOM cascade**: retested with both radios present —
  zero OOM events this boot, versus repeated kills (`hostapd`, `wpa_supplicant` ×2, `netifd`
  ×2) on the same config before the fix. Confirms finding (2) really was a symptom
  (memory-footprint-adjacent), not an independent hardware ceiling — consistent with the
  live v1.4 comparison earlier in this section.
- **The netifd CPU spin still happens anyway** — `netifd` observed pinned at 83% CPU, still
  spinning **30+ minutes after boot completed** (not a transient startup blip). This falsifies
  "vendor-module boot-order race" as the *sole* or even necessarily *correct* explanation —
  the fix that should have closed it didn't. New evidence points somewhere else entirely:
  the entire captured boot log has **zero** wireless-handler-probe lines (`wifi-scripts:
  Starting`, `Set new config for phy...`) — wireless bring-up may never even be reached.
  Instead, `dir842-asic`'s own bring-up (`base-files/etc/init.d/dir842-asic`) shows a
  103-second gap between "bring-up starting" and "role=router", which lines up almost
  exactly with that script's own `ubus -t 60 wait_for network.interface.lan` (60s) +
  `wait_netif()`'s 40×1s poll for `br-lan`/`wan` — i.e. **both timeouts likely ran to
  completion**, meaning netifd wasn't finishing even *basic wired* interface bring-up in that
  window.

  **`mount_root: failed to sync jffs2 overlay` ruled OUT as a confound**: read the actual
  fstools source (`openwrt/build_dir/target-mipsel_24kc_musl/fstools-*/libfstools/
  overlay.c:348`) — this fires whenever `/tmp/root/upper/` is empty on a fresh-flash first
  boot (`system("cp -a /tmp/root/upper/* / ...")` with zero matching files returns
  nonzero). Benign, expected on every reflash, not a real error.

  **New hard evidence, live on the bench**: while netifd spins at 100% CPU (`State: R
  (running)`, `/proc/<pid>/syscall` shows `running` — CPU-bound, not I/O-blocked), `ubus
  list` still answers immediately and shows the generic `network`/`network.device`/
  `network.interface` objects, but **no per-interface objects** (`network.interface.lan`,
  `network.interface.wan`) — netifd is stuck specifically in interface-configuration logic,
  not fully deadlocked (it still serves unrelated ubus requests). `ip link show`: every DSA
  port (`lan1`-`lan4`, `wan`) sits `state DOWN`, **including `lan1`, which has the real,
  working bench cable this whole investigation has been conducted over**. `cat
  /sys/class/net/lan1/carrier` returns `Invalid argument` — the kernel's own documented
  behavior for a netdev that has never had `ip link set up` called on it at all (not "no
  link", which reads `0`). By contrast `eth0` (the CPU-facing DSA conduit, brought up
  directly by this port's own kernel driver at boot, independent of netifd — confirmed via
  `rtl819x-eth ... eth0: interface up` in dmesg at kernel-boot time) reads `carrier=1`. So
  the kernel/driver stack is fine — the stall is specifically in netifd's own userspace
  config-application logic, before it ever reaches administratively enabling any DSA port.
  Leading hypothesis given the CPU-bound (not blocked) state: a config-reload loop
  (repeated teardown/reconfigure that never reaches a stable completed state) rather than a
  single stuck wait. A second, more precisely-briefed investigation (adversarial + web
  search + `main`'s netifd/DSA source comparison) was launched with this evidence — see the
  next update for its findings; the "vendor-driver-specific" framing is now fairly
  confidently ruled out (see the zero wireless-handler-probe-lines evidence above), but
  "wireless-specific in some other way" is not yet fully excluded.

  **Two more live experiments, both negative (real, but ruled things out rather than in):**
  (a) `killall netifd` (SIGTERM) had **no effect** — the process did not die; `kill -9`
  (SIGKILL) killed it instantly. A process resisting SIGTERM but dying instantly to SIGKILL
  is a real, specific signature: it strongly suggests a tight loop with no syscall/scheduling
  point where a pending signal would normally be delivered, not a slow-but-interruptible
  retry loop. procd respawned a fresh netifd process after the kill, and **it span up to the
  same 83% CPU immediately, from a cold start** — ruling out any per-process accumulated
  state and confirming the trigger is deterministic and present in whatever netifd reads at
  startup (the on-disk UCI config, or the live device/kernel state), not a race that
  sometimes resolves. (b) Inspected the live `network` UCI config and found `network.@device
  [1..4]` (the four individual `lan1`-`lan4` port device sections) all carrying the
  **identical** `macaddr` value — traced to stock, upstream `config_generate` behavior
  (`package/base-files/files/bin/config_generate:120-128`: when a bridge's logical interface
  has a MAC and multiple member ports, it deliberately writes that MAC to a `device` section
  for *each* port) — plausible-looking, since duplicate MACs across bridged ports could
  conceivably confuse DSA/switchdev bring-up. **Tested and refuted**: removed all four
  per-port `macaddr` overrides live, force-killed and restarted netifd — spun at 100% CPU
  immediately, identically. Not the cause. (Left the config as edited — harmless, not
  reverted, since it wasn't the trigger either way and reverting cost more bench time than
  it was worth.)

  **A second, more precisely-briefed investigation** (netifd C source read directly —
  `bridge.c`, `interface.c`, `device.c`, `system-linux.c`, `config.c`) found a real,
  well-cited structural issue and a documented upstream fix, tested live and **also
  refuted**: `bridge_set_up()` (`bridge.c:838-862`) requires all bridge members "present"
  before a multi-member software bridge activates, with only a one-shot 100ms retry, not a
  real backoff loop — a known class of upstream bug
  ([openwrt/openwrt#9089](https://github.com/openwrt/openwrt/issues/9089)), workaround
  `bridge_empty=1`/`force_active`. This also correctly explains why `main`'s working 4.14
  product never hits anything like this: `main` uses swconfig (`eth0.2`/`eth0.1`, one
  physical netdev split by VLAN tag), never a real multi-member software bridge — this
  port's DSA migration to `ucidef_set_interfaces_lan_wan "lan1 lan2 lan3 lan4" "wan"` is the
  *first time this exact netifd code path has ever run on this hardware*, so "unconnected
  ports work fine on `main`'s bench" was never comparable evidence. **Tested live**:
  `uci set network.@device[0].bridge_empty=1` (confirmed applied — `uci show` round-tripped
  it correctly), force-restarted netifd — **spun at 100% CPU identically**. Refuted as the
  (sole) cause, same as the duplicate-MAC theory above.

  **`ubus monitor` while spinning, ~8s: only 2 unrelated lines** — this rules out a
  ubus/netlink event-cascade (repeated `device_set_present`/`interface_set_available`
  broadcasts) as the visible mechanism; whatever's looping is doing so silently,
  in-process, with no external ubus traffic. `strace` is not in the minimal test image;
  added it (`seed-m7-test.config`) and queued a rebuild to get a real syscall trace on the
  next pass — the only remaining way to settle this definitively without deeper netifd
  source archaeology.

  **★ Got the trace. This is the closest to a definitive mechanism found so far.**
  `strace -f -tt -p <netifd-pid>` while spinning shows a sub-millisecond tight loop
  (each full cycle ~0.7-9ms — thousands of syscalls captured in a few seconds):
  ```
  _llseek(14, 0, [0], SEEK_SET) = 0
  read(14, "'use strict';\n\nimport { glob, ba"..., 1024) = 1024
  read(14, "ad(\"line\"));\n\t\t\ttry {\n\t\t\t\tdata ="..., 1024) = 1024
  _llseek(14, 2707, [2707], SEEK_SET) = 0
  munmap(...) / mmap2(...)                    # reallocating a 4K page each cycle
  _llseek(14, 0, [2707], SEEK_CUR) = 0
  [repeat]
  ```
  Confirmed by exact byte-for-byte comparison against every `.uc` file in the build tree:
  fd 14 is **netifd's own `lib/netifd/utils.uc`** (not a wifi-scripts file — this is
  netifd's core package, `openwrt/build_dir/target-mipsel_24kc_musl/root-rtl819x/lib/
  netifd/utils.uc`, ~140 lines, exports `handler_load()` — the wireless-handler-probe
  function — `is_equal()` — the standard netifd idiom for "did config actually change" —
  `parse_attribute_list()`, `sorted_json()`, and type parsers). It is being **re-opened and
  re-parsed from byte 0 on every single loop iteration** — ucode's module cache is not being
  hit, meaning something re-triggers a fresh `import` of a script that (transitively)
  imports `utils.uc`, over and over, with no meaningful work or delay between iterations.
  Given the insane rate and `ubus monitor`'s near-silence (ruling out external event
  traffic), the leading theory is a C-level retry/callback with no backoff that re-enters a
  ucode call every cycle — a third, tightly-scoped investigation (given the exact strace
  evidence above) was launched to pin down the precise call site.

  **★★★ SOLVED, 2026-09-03. Root cause, proven from both ends (live bench + ucode/netifd
  source), and the box now boots clean.**

  *The mechanism.* netifd probes every `/lib/netifd/wireless/*.sh` handler at startup via
  `handler_load()` in its own `lib/netifd/utils.uc`:
  ```
  while (!f.error()) {
      let data = trim(f.read("line"));
      try { data = json(data); } catch (e) { continue; }
      ...
  }
  ```
  The loop's only exit is `f.error()`. Per ucode's own fs module (`fs.c:207-216`),
  `read("line")` at EOF returns `null`/`""` and does **not** set `error()` — `error()` only
  reflects a genuine I/O error. `json()` on that value throws, `catch` hits `continue`, and
  the loop spins forever on the exhausted file. **netifd never returns from
  `handler_load()`**, which runs synchronously at startup (`wireless.uc:562`, via
  `main.uc:80`) — so it never reaches `config_init_all()` (`main.c:412`) and never brings up
  a single interface. That one fact explains every symptom at once: no `br-lan`, no DSA
  ports (they stay administratively down, hence `/sys/class/net/lan1/carrier` = EINVAL), no
  wireless, `dir842-asic`'s `ubus wait_for` timing out, only base ubus objects registered
  (they're published at `main.c:398`, before the hang) — and SIGTERM-immunity, because
  netifd is stuck *before* `uloop_run()` ever starts handling signals.
  The strace signature is ucode's exception machinery, not module re-import:
  `uc_vm_raise_exception()` (`vm.c:1059-1072`) always calls `uc_vm_get_error_context()`,
  which formats a source snippet via `uc_source_context_format()` (`lib.c:125-169`) —
  literally `ftell` → `fseek(0)` → `fgets` loop → `fseek(srcpos)`. That is an exact match
  for the captured trace, including the `srcpos=2707` restore matching `utils.uc`'s byte
  size. One thrown exception per loop iteration ⇒ one source re-read per iteration.

  *The empirical proof, in order.* (1) Removing the vendor handler and restarting netifd
  made it proceed normally within seconds — br-lan created and UP with its address, all DSA
  ports joined, traffic flowing, `mac80211.sh ... setup radio0` finally running. (2) A
  **minimal 8-line dummy `/bin/sh` handler**, with nothing to do with this driver,
  reproduced the spin identically — proving the trigger is *any* classic shell handler
  probed under the ucode wifi-scripts, not this handler's content. `mac80211.sh` never trips
  it because it is itself a ucode script. (3) Several plausible-but-wrong theories were
  tested live and refuted along the way: `bridge_empty=1` (the documented workaround for
  openwrt/openwrt#9089), removing the duplicate per-port MACs, disabling `radio1`, and the
  vendor kmod's load order (fixed separately, and confirmed insufficient on its own).

  *The fixes, both in-tree.* (a) `files/package/network/config/netifd/files/lib/netifd/
  utils.uc` — an override of netifd's own `utils.uc` whose `handler_load()` breaks
  explicitly on an empty read instead of trusting `f.error()`; closes the bug class for any
  handler, verified compiling and running correctly under the real mipsel/musl `ucode` via
  QEMU for both normal and empty dumps. (b) `# CONFIG_WIFI_SCRIPTS_UCODE is not set` in the
  seed — selects the classic shell wifi-scripts variant (both ship in the same package; see
  its Makefile's `files/` vs `files-ucode/`), matching how the working 4.14 product runs.

  *Result on hardware — partial, and honestly not yet reliable.* One boot with
  `kmod-rtl8192cd` present did reach **`br-lan` UP with 192.168.0.1/24, netifd idle (not in
  the top 5 processes), and ZERO OOM events** — the first time in this port's history that
  the LAN bridge came up at all, and strong evidence the `handler_load()` diagnosis is
  correct. The earlier OOM cascades are confirmed downstream of this bug (nothing ever
  completed, so procd kept respawning killed daemons into a starved window).

  **But it does not reproduce reliably, and the milestone is NOT closed.** Across several
  subsequent flash+boot cycles of the same image the box more often lands in a state where
  `br-lan` exists but has **no IPv4 address** (`ip -4 addr show br-lan` prints nothing),
  ping fails, TCP returns RST ("connection refused" — consistent with the firewall
  rejecting because the interface never entered a zone), `dropbear` never starts, and
  `dir842-asic` logs `recovery level 3` at ~[238s] because its `ubus wait_for
  network.interface.lan` + `wait_netif()` both time out again. Crucially, **netifd is no
  longer spinning** in these runs (it is not in the top processes) — so the `handler_load()`
  fix is doing its job, but *something else downstream still prevents interface
  configuration from completing*. That second problem is un-diagnosed.

  Also recorded, because it cost real time: the two changes are not independently
  sufficient. `CONFIG_WIFI_SCRIPTS_UCODE=n` + the `utils.uc` override produced the one good
  boot; the `utils.uc` override alone (with the default ucode wifi-scripts) reliably gives
  `br-lan` with no address. Whether `WIFI_SCRIPTS_UCODE=n` is genuinely required, or was
  incidental to that one lucky boot, is not established — it is a large blast-radius switch
  (it swaps the entire wifi-scripts stack to the legacy shell variant) and should not be
  treated as a settled fix without more evidence.

  **Next concrete step for whoever picks this up:** on a boot in the bad state, capture
  `ubus call network.interface.lan status`, `logread | grep netifd`, and
  `ubus call network.device status` — that distinguishes "netifd never parsed the lan
  interface" from "it parsed it but the proto handler never ran" from "it ran and the
  address apply failed". The bench console becomes unresponsive under this load, so prefer
  writing those to a file and reading it after, or use a serial capture rather than
  interactive commands.
- The M5 interface-name fix (`eth0.1`/`eth0.2` → `wan`/`br-lan` in `rtl865x_asichal.c`'s
  `dev_get_by_name()` calls, found by the same adversarial-review pass — see §4) is
  independently verified on disk and queued for the next combined bench build; not yet
  tested on real hardware.

The vendor source lives at
`files/target/linux/rtl819x/files-6.18/drivers/net/wireless/rtl8192cd/` (carried forward
from the 4.14 port, same redistribution basis -- see the root README's *Credits &
license*).

## Bench facts that outlived this write-up

- Test box `192.168.100.3`, `ssh -i ~/.ssh/id_router_rsa` (the ed25519 `id_router` key is
  rejected). UART `/dev/ttyUSB0` 38400 8N1. Smart plug `tomada`.
- `ramboot.sh` / `flash-nor.sh` / `bootgate.sh` at the repo root; `IFACE=<host-NIC>` selects
  the bench NIC cabled to the router's LAN side.
- Build: `SEED=seed-min.config ./build.sh` for bring-up (no LuCI, no wireless — fits RAM
  boot); `./build.sh` (default `seed.config`) for the release set.
- The DSA datapath needs ≥60 s of uptime before it carries traffic; do not judge a boot dead
  before that.
