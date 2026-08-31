# docs/ — index and reading order

The engineering record for the DIR-842 R1 port: what each subsystem actually does, why it
is built that way, and which explanations were falsified on the way there. The root
[`../README.md`](../README.md) is the product-facing entry (status, headline numbers,
build, install, licence) and is **not** restated here. This file only routes you.

## What this directory is

Two kinds of file live in `docs/`:

- **Current docs** — rewritten to describe the *finished* state of a subsystem. Read these
  for the answer.
- **Historical journals** — chronological, self-correcting logs written while the answer
  was still unknown. Kept unsanitised. Read these for *how the answer was found*, and for
  the dead ends you would otherwise re-walk.

Everything here is engineer-to-engineer, evidence-first: register name and value, `file:line`,
commit hash, or a measured number. Where a claim was later shown to be wrong, the retraction
stays in the file rather than being quietly edited out.

## Status in one paragraph

Mainline-style OpenWrt (kernel 4.14, on the ggbruno RTL8197F fork base) on the **D-Link
DIR-842 rev R1** — RTL8197F SoC, MIPS 24Kc mipsel, 64 MB RAM, 8 MB NOR. It **boots from
NOR unattended and survives power cycles**. Wired ethernet works through a managed RTL8367S
in the vendor's **CPU-tag / port0-router** mode, with a real WAN/LAN split — `eth0.1` = WAN
(VID 1), `eth0.2` = LAN (VID 2); **there is no `eth1`**, the SoC has a single CPU-port netdev
that Linux splits by VLAN (`files/target/linux/realtek/base-files/etc/board.d/02_network:10-17`).
On top of that: fw3 + NAT + PPPoE, the software flowtable, and **hardware NAT offload at
891 Mbit up / 896 Mbit down with 0.0 % of payload bytes crossing the CPU** (stock D-Link on
the same bench: 913/923). Both radios run concurrently and bridge into `br-lan` — 5 GHz
**RTL8822BE via rtw88**, 2.4 GHz the **on-SoC WMAC via the vendor `rtl8192cd` driver**,
which ships in `files/` (licensing rationale: root README, *Building*). **v1.1** adds
**802.11r (Fast Transition) on both radios**, and closes a real roaming bug: run as a
bridge/dumb-AP on a home LAN, the ASIC bring-up sequence now auto-detects router vs.
bridge role and skips the router-only steps that were freezing L2 aging and silently
blackholing roamed-in wireless clients — the fix that closed two real house-wide
outages during development (`docs/SWITCH-AND-DATAPATH.md` §10). Bench numbers above are
still bench-only, but this port has since run as a real home dumb-AP under live client
traffic and roaming; treat it as **pre-production** regardless and keep a flash backup.

## Reading order for the three kinds of reader

**(a) You want to build and flash it.**

1. [`../README.md`](../README.md) — the warnings first, then `build.sh`. (You do not need
   `tools/sign-dlink.py`: build.sh signs the images for you.)
2. [`BENCH.md`](BENCH.md) — serial is **38400 8N1** (`ramboot.sh:20`), how to catch the
   loader, `ramboot.sh` / `flash-nor.sh`, and power control.
3. [`WIFI-DUAL-BAND.md`](WIFI-DUAL-BAND.md) — how each radio is driven, the naming
   collision between them, and the 2.4 GHz calibration caveats.

**(b) You want to understand how the hardware offload was achieved.**

1. [`HWNAT-OFFLOAD.md`](HWNAT-OFFLOAD.md) — the answer.
2. [`SWITCH-AND-DATAPATH.md`](SWITCH-AND-DATAPATH.md) — the switch model the offload sits on.
3. [`M7-HWNAT-REVERSE-NAPT.md`](M7-HWNAT-REVERSE-NAPT.md) — the investigation log, for how
   it was actually found.
4. [`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) — what the wrong turns cost and
   what measurement rules came out of them.

**(c) You are a future maintainer picking this back up.**

1. [`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) — **first**. It is the cheapest
   way to avoid re-running twenty already-falsified experiments.
2. [`BENCH.md`](BENCH.md) — you cannot test anything until the bench is back up.
   Bench tooling lives in [`../tools/bench/`](../tools/bench/).
3. [`COLD-BOOT-TX-WEDGE.md`](COLD-BOOT-TX-WEDGE.md) — if the box boots but answers
   nothing it originates, read §1 first: one register tells you in ten seconds
   whether it is this bug. §9 is the open end: do not re-enable pstore without it.
4. Then whichever subsystem doc covers what you are touching.

## The current docs

| file | covers |
|---|---|
| [`HWNAT-OFFLOAD.md`](HWNAT-OFFLOAD.md) | The solved account of ASIC L3/L4 offload, and the numeric-byte-order root cause. |
| [`SWITCH-AND-DATAPATH.md`](SWITCH-AND-DATAPATH.md) | The RTL8367S + SoC switch model, CPU-tag / port0-router mode, the RGMII trunk fix, VLANs, MACs, the `CPUICR1` boot-medium endianness bug, and (§10, v1.1) the dumb-AP roaming blackhole — two real house-wide outages, root-caused and fixed. |
| [`BENCH.md`](BENCH.md) | The physical bench, serial console, power control, build container, and the unattended RAM-boot / NOR-flash automation. |
| [`WIFI-DUAL-BAND.md`](WIFI-DUAL-BAND.md) | The RTL8197F + PCIe wall, the blank efuse, the 25 MHz crystal, the vendor 2.4 GHz driver, the two radios' interface-naming collision, and (v1.1) 802.11r on both radios plus the cold-boot wireless-datapath fix. |
| [`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) | Every falsified hypothesis and retracted claim, plus the measurement rules and bench confounds they produced. |
| [`OTA-INSTALL.md`](OTA-INSTALL.md) | Install OpenWrt over the air with **no serial cable** — upload the factory image through the stock D-Link web UI — and revert just as easily. Verified end-to-end on hardware. Product-facing. |
| [`RESTORE-STOCK.md`](RESTORE-STOCK.md) | How to return to pristine D-Link firmware and back to OpenWrt at will — the flash partition map, why it's reversible, and both restore routes (verified on hardware). Product-facing. |
| [`COLD-BOOT-TX-WEDGE.md`](COLD-BOOT-TX-WEDGE.md) | The cold-autoboot CPU-TX wedge: everything the box originated was dropped on a hands-off power-on boot, while RX stayed healthy. **Fixed in practice** by removing the `ramoops` carve-out — nothing in the ethernet driver was needed — but ★ the mechanism is NOT established: §9 records four measured attempts to keep ramoops (withholding the DRAM outright, `PSTORE_CONSOLE=n`, `unbuffered`, relocating the window) that all still wedge, which retracts §2's "it corrupts DMA memory" story. Carries the refutation list, the one-register fingerprint, the bootloader jump-path RE, and the next experiment to run. |
| [`M7-LARGE-FRAME-RX-WEDGE.md`](M7-LARGE-FRAME-RX-WEDGE.md) | Root cause and fix for the RX-FIFO drain-lag race that corrupted box-terminating frames larger than ~128 B — the bug that broke DHCP and SSH. Part reference, part journal — it carries a banner. |

## The historical journals

These are **chronological logs written as the work happened**, not summaries written after.
They contain claims that are contradicted later in the same file, sometimes several times.
Each carries a **dated banner at the top naming the commit that superseded it — read the
banner before you trust anything below it.** They are kept unsanitised on purpose: the
sequence of wrong models is the most reusable content in them.

- [`M7-TRUNK-FORWARDING-FIX.md`](M7-TRUNK-FORWARDING-FIX.md) — the original RGMII
  trunk L2-forwarding diagnostic, superseded by `SWITCH-AND-DATAPATH.md`.
- [`M7-HWNAT-REVERSE-NAPT.md`](M7-HWNAT-REVERSE-NAPT.md) — the offload investigation,
  ~1400 lines, written while the answer was still unknown. Its original header declared the
  problem unsolved with "two remaining blockers"; **both are now closed.** Read
  `HWNAT-OFFLOAD.md` for the answer and this for how it was found.
- [`VENDOR-PARITY-INVENTORY.md`](VENDOR-PARITY-INVENTORY.md) — the stock-firmware inventory
  (mtd1 calibration layout, the feature-parity table, what stock does and does not ship),
  bolted to the R4 journal that ported the vendor 2.4 GHz driver.

## Conventions used in these docs

- **★** marks the load-bearing finding of a section — the one thing that, if you skip it,
  makes the rest not work.
- **✗ RETRACTED** means a claim made *earlier in the same file* has been falsified; the
  correction follows immediately. It is not a warning about the current text — it is a
  pointer backwards.
- Measured numbers are always from hardware, with the bench and the method named. Ranges are
  given as ranges; a single run is labelled as a single run.
- **Commit hashes refer to the branch `Realtek` of the engineering build tree**, which is
  published at [`ADCDS/openwrt`](https://github.com/ADCDS/openwrt/tree/Realtek) (the
  full OpenWrt tree this repo overlays onto). Hashes prefixed `mirror` are commits in
  *this* repo. If you only have this repo, a bare hash will not resolve here — that is
  expected; it lives in the build tree.
- ⚠ **Hash aliasing.** That history was rebased. Commit *messages* and older doc text
  cross-reference a pre-rebase hash set that is **no longer an ancestor of `Realtek` HEAD**
  (the objects themselves still resolve in a full clone). If a hash looks unfamiliar, this
  is why. Verified aliases:

  | current (on branch `Realtek`) | pre-rebase (as quoted in old commit messages) |
  |---|---|
  | `d150b24606` | `f096f5d` |
  | `fd2cf06a07` | `85f01c9` |
  | `ef6fee2cb9` | `3cd6ec0` |
  | `808b2fd9f5` | `9562db2` |
  | `03b4cf76dc` | `823265d` |
  | `6ee32b7a9d` | `344889a` |
  | `5e2645d21f` | `c7389fc` |
  | `dc372efd28` | `9b35802` |
  | `224f31964e` | `13d7a5b` |
  | `fd606f4428` | `3f8fc72` |
  | `59e2d2cd27` | `72d5d35` |

## Cross-references you will not be able to follow

A few docs cite working notes that were never published here — `HANDOFF-M6.md`,
`RE-notes.md`, `ASIC-ENGINE.md`, `STOCK-TABLES.md`, `PARTITIONS.md`, and the deleted
`ASSESSMENT.md`. They live in the private engineering tree. Where their content mattered
it was folded into the current docs; the citations are left in place because they date the
claim around them.

## What is deliberately not here

- **The Realtek vendor SDK proper.** Not redistributable: its sources carry a Realtek
  copyright and *"All rights reserved."* with no licence grant (e.g.
  `include/net/rtl/rtl865x_netif.h`: `Copyright c Realtek Semiconductor Corporation, 2008` /
  `All rights reserved.`). What ships in `files/` — the ~37 `include/net/rtl/*` headers and
  the ~120-file `rtl8192cd` driver — is our own reverse-engineered work against that SDK,
  not the SDK itself. An earlier `VENDOR_SDK=` build flag that tried to pull the real SDK
  in directly was verified broken and withdrawn 2026-08-02
  ([`WIFI-DUAL-BAND.md`](WIFI-DUAL-BAND.md) §9 item 5). As of **v1.1** the 2.4 GHz radio
  (including 802.11r) **does** build from this repo — `build.sh` overlays `files/` and the
  kernel builds it like any other in-tree driver, no separate flag needed.
- **The 8 MB stock NOR backup.** It is the only remaining copy of stock for a flashed unit;
  take your own before you flash.
- **The raw serial boot log.**
- **`docs/ASSESSMENT.md`** — removed. It was a point-in-time review from 2026-07-14 whose
  every conclusion has since been overturned by the work that followed: "no WAN port",
  "~4 Mbit/s", and "there is no hardware-offload backend to enable". Keeping it would have
  been actively misleading; it is recorded here so that anyone who saw it knows it is dead.
