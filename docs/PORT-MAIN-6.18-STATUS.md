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

Kernel 6.18.44 (backports 7.2) on OpenWrt main, target `rtl819x`/`rtl8197f`, `arch/mips/generic`
instead of a private platform. The RTL8367S is driven by mainline `rtl8365mb` over DSA
(`tag_rtl8_4`), not swconfig — real per-jack switch ports, no VLAN-cascade trunk model. It
**boots from NOR unattended and survives power cycles and sysupgrade** (10/10 cold-boot gate,
verified 2026-09-02). Wired LAN/WAN works at a software-forwarding rate (~75-160 Mbit
depending on direction, no hardware acceleration yet). The 5 GHz radio (RTL8822BE via `rtw88`)
runs as an AP with a beacon confirmed by an independent client; the on-SoC 2.4 GHz radio has
**no driver at all** — the vendor `rtl8192cd` port (M7) has not been started. Hardware NAT
offload is wired up end to end on the current kernel's interfaces but **does not accelerate
anything yet** — see §4.

## Milestones (of the plan's M0–M8)

| # | what | state |
|---|---|---|
| M0 | skeleton compiles | ✅ |
| M1 | early printk / timer / console / shell | ✅ |
| M2 | SPI-NOR / mtdsplit / GPIO / LEDs / keys | ✅ |
| M3 | ethernet conduit + DSA switch | ✅ (§2) |
| M4 | PCIe + 5 GHz AP | ✅ except a client-association/DHCP test (§3) |
| M5 | hardware NAT | ⚠️ rebuilt, not accelerating (§4) |
| M6 | flash boot (factory + sysupgrade) | ✅ (§5) |
| M7 | vendor 2.4 GHz `rtl8192cd` driver | ⚠️ scoped and started, does not compile yet (§6) |
| M8 | LuCI, docs, release | this pass |

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
wireless subsystem exists alongside it. It now lives outside `base-files/` until the M7
package installs it.

## 4. Hardware NAT (M5) — wired up, not accelerating

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

## 5. Flash boot (M6)

`bootgate.sh`: 10/10 cold boots from NOR, no oops, no panic, jffs2 overlay every time.
`sysupgrade -n`: verified clean back-to-back.

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

## 6. Vendor 2.4 GHz driver (M7) — real progress, not compiling yet

Scoped in the plan at roughly 100 timer conversions (`init_timer`/`setup_timer`/`mod_timer`
→ `timer_setup`), ~62 `virt_to_bus`/`bus_to_virt` sites in the DMA path (silent-corruption
risk, not a compile error), proc/`set_fs`/ioctl-routing cleanup, a new platform-bridge probe
matching the DT node the 6.18 board file already declares (`realtek,rtl8197f-wmac`), and a
full RF bring-up — an estimated two to three weeks of focused work, genuinely a separate
undertaking from the rest of this port.

This pass got the driver from "does not compile, thousands of opaque errors, nothing about
the scope was verified" to "two well-understood, precisely-scoped kernel-API-portability
categories away from compiling clean." Two real build-system bugs, not source bugs, were
hiding the true picture entirely:

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
4.14-era build ever did (unused variables, missing prototypes, `packed` on an
already-aligned array, benign same-value macro redefinition) is not worth hand-fixing
across code this port does not maintain upstream. That took the error count from
"thousands" to 9 genuine, individually verified issues, of which 5 were small and safe to
fix now: three `RTL_R{8,16,32}_F()` register-read helpers returned `void` on one panic-guard
path despite a non-`void` return type (gcc 14 hard error, not a warning; fixed with the
conventional all-1s "failed read" sentinel), a local `crc32()`/`hmac_sha256()` collided in
name (not signature) with kernel-exported symbols of the same name once a header pulled
them in transitively (renamed to `rtl_crc32`/`rtl_hmac_sha256`, 9 call sites), and a
`timeval_to_us()` helper used the kernel's now-removed `struct timeval` with zero callers
anywhere in the tree (dead code, deleted).

**What is left is exactly the two categories the plan predicted, now with real numbers
instead of a guess, confirmed by an actual compiler run rather than reasoned about:**

- **Timers: 102 `init_timer`/`setup_timer` call sites** need converting to `timer_setup()`,
  each paired with its callback's signature changing from `void cb(unsigned long data)` to
  `void cb(struct timer_list *t)` plus a `timer_container_of()` recovery line, plus 12 files
  with non-standard `.data` access on top of that.
- **DMA: this is one connected body of work, not two.** The `virt_to_bus`/`bus_to_virt`
  address-translation side (58 sites, 9 files, 31 of them concentrated in `8192cd_tx.c`
  alone -- the live TX descriptor ring) and the `PCI_DMA_FROMDEVICE`/`PCI_DMA_TODEVICE` +
  `pci_dma_sync_single_for_cpu`/`_for_device` direction/sync side (100+ further sites across
  `8192cd_tx.c`, `8192cd_rx.c`, `8192cd_rx.h`, `8192cd_aes.c`, `8192cd_hw.c`, `8192cd_sme.c`,
  `8192cd_osdep.c`, the WlanHAL RTL88XX descriptor files) are the SAME calls
  (`rtl_cache_sync_wback()`, `get_physical_addr()`, `pci_unmap_single()`) viewed from their
  two different arguments. `pci_dma_sync_single_for_cpu`/`_for_device` no longer exist as
  kernel functions at all (no compat shim remains); the modern equivalent is
  `dma_sync_single_for_cpu`/`_for_device` taking a `struct device *`, and `priv->pshare->pdev`
  (a `struct pci_dev *`) is very likely never populated for this on-SoC, platform-device
  variant of the driver -- confirmed the probe function in
  `files-6.18/drivers/net/wireless/rtl8197f/rtl8197f-wmac.c` takes a
  `struct platform_device *`, not a `struct pci_dev *`.

Getting DMA addressing wrong produces silent RX/TX corruption or intermittent hangs, not a
compile error -- exactly the failure mode this port's own M3 investigation spent real bench
time on for the *hardware-supplied* datapath, and not something to convert 150+ call sites
of blind, then hope. Deliberately not attempted this pass; the honest next step is the same
one the plan always named: convert it carefully, file by file, verified against real RX/TX
traffic on the bench, not against compiler success alone.

The vendor source lives at
`files/target/linux/rtl819x/files-6.18/drivers/net/wireless/rtl8192cd/` (carried forward
from the 4.14 port, same redistribution basis -- see the root README's *Credits &
license*). `seed.config`/`seed-min.config` still do not enable `kmod-rtl8192cd`: it does
not yet produce a working module.

## Bench facts that outlived this write-up

- Test box `192.168.100.3`, `ssh -i ~/.ssh/id_router_rsa` (the ed25519 `id_router` key is
  rejected). UART `/dev/ttyUSB0` 38400 8N1. Smart plug `tomada`.
- `ramboot.sh` / `flash-nor.sh` / `bootgate.sh` at the repo root; `IFACE=<host-NIC>` selects
  the bench NIC cabled to the router's LAN side.
- Build: `SEED=seed-min.config ./build.sh` for bring-up (no LuCI, no wireless — fits RAM
  boot); `./build.sh` (default `seed.config`) for the release set.
- The DSA datapath needs ≥60 s of uptime before it carries traffic; do not judge a boot dead
  before that.
