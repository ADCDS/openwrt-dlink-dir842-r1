# Retractions, disproven hypotheses, and the measurement rules they produced

Every claim this port made and later falsified, in one index — with the bench confounds that
manufactured the false results and the measurement rules derived from them. Read it before
designing an experiment on this hardware: roughly a third of the elapsed project time was spent
re-running dead hypotheses or trusting a metric that could not detect the effect it was testing.

★ The single most expensive class of error here was never a wrong register value. It was a
**metric that could not have detected the effect** (index #12, #13, #14) or an **A/B on a
variable that was coupled to another** (#19, #21). Both produce clean, repeatable, wrong answers.

---

## 1. Why this file exists

The port's own engineering history is a chain of self-corrections. Several of them overturned
claims that had already been written into these docs as settled fact and had steered days of
work. Two concrete examples:

- `docs/M7-HWNAT-REVERSE-NAPT.md` still states, as a finding, that "**All hash inputs are
  NETWORK order**". That is backwards, and it was the whole root cause. It stood
  for eleven days and every experiment run under it read flat. (Index #17.)
- `docs/VENDOR-PARITY-INVENTORY.md` (the "R6 correction" section) is headed "★ R6 correction: 'stock does 600–800 Mbit'
  is NOT evidenced" — a retraction of a number that had been circulating as an acceptance
  target. That section is itself now obsolete: stock *was* subsequently measured. (Index #30.)

Those files are kept as written, because the sequence in which a wrong belief was held and
dropped is part of the evidence. This file is the index over them.

A secondary purpose: **three stale comments are still in the shipped tree today.** They are
listed in §6 so a future reader does not re-seed a dead hypothesis from them, exactly as
happened once already (#11).

## 2. How to read it

**Verdicts.**

| verdict | meaning |
|---|---|
| `FALSIFIED` | tested, the claim is false |
| `RETRACTED` | withdrawn — the *measurement* was invalid, so the claim is unproven, not disproven |
| `RED HERRING` | true statement, causally irrelevant |
| `RIGHT, WRONG REASON` | conclusion survived; the stated mechanism did not |
| `WRONG BUT USEFUL` | false, but it produced the instrument or the reframe that cracked the real bug |
| `FALSE POSITIVE` | a *success* that was announced and then withdrawn |
| `OPEN` | disproven with no replacement cause on record |

★ **`RETRACTED` is not `FALSIFIED`.** Four entries here (#4, #5, #19, #26) are retracted
measurements whose underlying claim was never actually tested. Re-testing those is legitimate
work; re-testing a `FALSIFIED` row is not.

**Commit hashes.** ★ The engineering branch was rebased. Commit *messages* and the older docs
cross-reference a pre-rebase hash set that is **no longer an ancestor of `Realtek` HEAD**,
though the objects still resolve in a local clone. Verified aliases (left = on branch today,
right = the hash quoted inside commit text):

| on branch `Realtek` | quoted in commit messages / older docs |
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

Verified: `git merge-base --is-ancestor` returns true for every left-hand hash and false for
every right-hand one. Hashes marked **mirror** are in the publishable repo
(`openwrt-dlink-dir842-r1`), not the build tree.

---

## 3. The retraction index

| # | claim | verdict | commit |
|---|---|---|---|
| 1 | M4: "the RX interrupt storm is the TX wedge" | **RED HERRING** — pure polling wedged too; the wedge was TX-triggered. Real fix: `nicTx.flags = PKTHDR_HWLOOKUP`, not a hand-guessed direct `tx_dp=0x1` (a direct portmask corrupts unless the switch port/VLAN tables are already programmed) | notes repo v1–v11 |
| 2 | "Descriptor-pool exhaustion causes the A-2 fabric latch" | **DISPROVEN → OPEN** — `USEDDSC` never exceeds 53 of 1023, no `DSCRUNOUT`. ⚠ **No replacement cause is on record; the A-2 latch root cause is still open** | `d150b24606` |
| 3 | R2: "bus burst size differs from stock" | **FALSIFIED** — stock `CPUICR = 0xE4000000`, byte-identical to ours | `0754f3759d` |
| 4 | R2: "masking `CPUIIMR` bit16 leaves RX dead across cold boots" | **RETRACTED** (see §4 #5) — the whole "datapath regression" was the harness running `ip neigh flush` immediately before every ping. The masked value is **UNTESTED, not falsified** | `1bb3d673d1` → `53a0c39e66`, gate `c3592fa547` |
| 5 | R2: "the per-unit MAC from mtd1 breaks the datapath" | **RETRACTED** — same confound as #4 | `53a0c39e66` |
| 6 | R3: "`/dev/mtd1` is unreadable that early, hence MAC non-determinism" | **WRONG** — it was **two writers** (`board.d` and `uci-defaults`) | `484e4db4bd` |
| 7 | R3: "missing RF power tables cause the 5 GHz hang" | **WRONG** — `write RF mode table fail` came from `rtw8822b_set_channel_bb()` polling RF reg `0x33`; that is **RF register access failing**, not a missing table. Real cause: rtw88's blank-efuse fallback sets `crystal_cap = 0` (minimum load capacitance) → synthesiser off-frequency | `f5a3ac7993` |
| 8 | R4: "replay the flattened vendor PHY tables to bring up 2.4 GHz" | **RULED OUT ON SILICON** — see below | `5cdd707ff9` / `d5a4a33746` |
| 9 | R4/G3: "the blocker is vendor `ccflags`" | **RETRACTED AS UNVERIFIED** | mirror `4b408de` → `659fb1a` |
| 10 | R6: "`SWTCR0.WANRouteMode = ToCpu` is the fix" **and** "ToCpu kills the WAN" | **BOTH FALSIFIED** by a clean same-boot A/B (382 → 427 CPU pkt/MB, marginally worse); the wedge reproduces under ToCpu too | `d1e5ea0feb` |
| 11 | "NAPT rows must be addressed set+way (4-way)" | **FALSIFIED FROM VENDOR SOURCE** — `_Is4WayHashEnabled()` has **no callers anywhere** and `rtl8651_setAsicNaptTcpUdpTable` never transforms the index. ★ The hypothesis was seeded by a **stale comment in this project's own header** | `3712fd5e36` |
| 12 | ★ **METHODOLOGY**: every R6 measurement in one whole session | **INVALID** — all of them ran with `hwnat=N`, i.e. they measured **software forwarding by definition**. See below | `38d335f93b`; `ea6de9f464`; `37ed423dac` |
| 13 | ★★ "Stock does not hardware-offload the reverse path either; the capability does not exist" | **MEASURED CORRECTLY, THEN RETRACTED AS A FALSE NEGATIVE** — see below | mirror `537cb63` → `8d7f7d0` |
| 14 | ★ "~90 % of the forward path is offloaded" | **A GRO ARTEFACT** — see below | mirror `efedad9` |
| 15 | "A sustained download wedges the box" | **RETRACTED** — a cable was being physically changed mid-transfer | mirror `855b726` |
| 16 | "~1 packet per NAPI poll is a bug" | **RETRACTED** — it is a correct budget-bounded drain | mirror `855b726` |
| 17 | ★★★ "The ASIC keys/hashes **on-wire network order**" | **BACKWARDS — AND THIS WAS THE WHOLE ROOT CAUSE.** See below | asserted `fd2cf06a07` (2026-07-20), overturned `8d315c331b` (2026-07-31) |
| 18 | ★★ "FIRST HARDWARE FORWARDING — a two-factor NAPT key bug" | **FULLY RETRACTED, FALSE POSITIVE**, for two independent reasons. See below | `dc372efd28` retracted by `224f31964e` |
| 19 | ★ "`napt_fill_all` exonerates the index derivation" | **CONFOUNDED** — run while `napt_key_htonl` was still 1, so with a byte-swapped key **no index could match**. Its verdict was banked as settled and steered every experiment for a day | `8d315c331b` |
| 20 | "TCP dies only when rows install with the new indices" | **RETRACTED** — it was the WAN peer's `iperf3` server wedged by a timeout-killed client; reproduced with `hwnat=0` and no rows at all | `97ba3369e4` |
| 21 | "Hash-input byte order is NOT the fix" (100.7/100.6 vs 100.9/100.3) **and** "EnL4WayH 4-way hash is not the blocker" | **BOTH CLEAN, CORRECTLY-EXECUTED A/Bs WITH FLAT RESULTS — AND BOTH MISLEADING.** The variables are **coupled** | `97ba3369e4`, `6e53ee9915` |
| 22 | WiFi: "OOM causes the 2.4 GHz AP failure" → then "OOM is not the cause of the WiFi problems generally" | **BOTH WRONG, IN OPPOSITE DIRECTIONS** — two distinct bugs with overlapping symptoms. See below | `42ecc2308d` → `261c833bd4` |
| 23 | "`PCRP0` ForceSpeed explains the dead NOR-boot datapath" | **A REAL LATENT BUG THAT WAS NOT THE BUG** — fixing it moved the register to the correct value **and the port stayed 100 % dead**. Cost most of a session. Actual cause: `CPUICR1` bit 1 | `1a63ae8deb` → `07fa6a8627` |
| 24 | "The wired datapath is 100 % dead" | **THE WRONG FRAME** — RX worked throughout; `swnic rx#… port=02 vid=2` showed the correct jack and VLAN. Reframed the hunt from ingress/trunk to "received but not delivered" | `07fa6a8627` |
| 25 | "The GPIO58/gpio474 8367S-reset breakthrough" | **REFUTED** — a 3/3 result at a ~50 % per-boot success rate is not evidence. ★ Causality was also **inverted**. See below | mirror `9c2aabd` (which records "~33-55 % baseline", "~10 consecutive RAM-boots", "P~=0.1 %"; the exact p-value once quoted here was not in any source and has been removed) |
| 26 | "The full rebuild introduced an RX regression" | **DISPROVEN** by correlating boot outcomes across many boots: the 3.7 MB image was RX-live 16/29 (55 %), the 2.9 MB image 15/40 (37.5 %). **Identical binaries flip `rx=0` ↔ `rx-ok` across boots.** Two sessions were spent bisecting a defconfig for a bug that did not exist | 16/29/15/40 from `dir842-r1-openwrt` `docs/HANDOFF-M6.md:422`; verdict in mirror `9c2aabd` |
| 27 | "Fork A walls on the single-trunk U-turn" (as stated at the time) | **WRONG AS STATED** — the actual blocker was `MSCR` `EN_IN_ACL` (bit 4) left ON with `gw_prog`'s all-zero ACL entries, which are **not** permit-all, so every frame was dropped before routing. The **deeper** structural claim (ingress port == egress port for every routed flow) survived and justified deleting Fork A | `86880757ed` |
| 28 | FORWARDING-RECIPE's "route → nexthop → L2 bypasses ARP" | **WRONG** — the live stock dump shows **two** indirections: route (`process=5`) → nexthop → **ARP** → L2. Pointing the nexthop straight at an L2 index makes the ASIC walk a garbage ARP slot and **hang the fabric** on any frame (watchdog reset, no panic). ★ **Trust the dump over the recipe** | — |
| 29 | "The external switch is an RTL8367**R**" | **WRONG** — it is an RTL8367**S**, documented as 8367R for weeks. `chip_number` (`0x1300`) = `0x6367`, `chip_ver` (`0x1301`) = `0x0020`. VLAN/MIB registers are family-common; **chip-reset and ext-interface (RGMII) registers differ** | `rtl8367b.c:753`, `:776` |
| 30 | "Stock does 600–800 Mbit" | **NEVER EVIDENCED** when written — it traced to an inferred acceptance target. **Now closed by direct measurement: stock is 913/923 Mbit at ~1 % CPU.** `VENDOR-PARITY-INVENTORY.md:161` is therefore obsolete | mirror `8d7f7d0` |
| 31 | R3: "the LED GPIO numbers are not safely recoverable, this needs a human check" | **WRONG** — they *were* recovered by the same static-decode method the doc called unsafe (`gpiom.ko`'s `dev_leds` / `dev_buttons` tables). ★ And the first LED node was **actively dangerous**. See below | `70c16cab8f` |
| 32 | "WAN ingress-ACL overlap concern" (docs-only) | **RETRACTED as a decode error** — the config matches stock exactly | mirror `d53dcfd` |
| 33 | R4/G4: "rtw88's 5 GHz is `wlan1` and the vendor root device is `wlan0`" | **RECORDED AS MEASURED — and the recommended fix caused the next bug.** Pinning load order by giving the vendor module AUTOLOAD made `rtl8192cd` claim `wlan0` first, so mac80211 lost its derived name and the 5 GHz AP died | `35ec543891` → `9edc6bc5a8` → `b830957df9` |
| 34 | ★★ "The cold-autoboot CPU-TX wedge is a switch-core clock / reset / config problem" | **ALL FALSIFIED — the cause was `ramoops` corrupting DMA memory.** Ten hypotheses over three sessions (clock gate, settle time, `srcExtPortNum`, jack-PHY AN restart, external-chip cold bring-up, switch-core reset at four different points, role/`gw_prog`) plus bit-identical register sweeps of BOTH chips. The fault leaves **no register fingerprint** because it is in DRAM, not a register. See below | `docs/COLD-BOOT-TX-WEDGE.md` |
| 35 | "The wedge kills CPU **unicast** while broadcast still works" | **THE WRONG FRAME** — port0's out-multicast/broadcast counters climb from jack-to-jack flooding across the CPU-tag trunk, not from CPU-originated frames. *All* CPU-originated TX is dead. Cost most of a session chasing a unicast-specific classifier | `docs/COLD-BOOT-TX-WEDGE.md` §1 |
| 36 | "`0x1219 != 0` proves the loader configured the external switch, so preserve it" | **THE GATE HAS NEVER FIRED ON THIS BOARD** — `0x1219` reads `0x0040` on every boot path, so `rtl8367b_setup()`'s EXT1 cold bring-up has never executed here. Forcing it to run is harmless (it does NOT desync the RGMII pair — index #25's warning applied to a *partial* replica plus a gpio474 reset) but changes nothing | `docs/COLD-BOOT-TX-WEDGE.md` §4-5 |
| 37 | ★★ R6: "the kernel excludes this range from the normal allocator" (the `ramoops` `/reserved-memory` node) | **FALSE, AND SILENTLY SO.** `arch/mips` (4.14) never `select`s `OF_RESERVED_MEM`, so `fdt_reserved_mem_save_node()`/`fdt_init_reserved_mem()` are no-op stubs: the node is matched and **nothing** is reserved, with no warning. ★ And forcing `OF_RESERVED_MEM=y` is **still** not enough — measured, non-image reserved moved 697K→700K, not +128K — because MIPS rebuilds all memory state in `bootmem_init()` from `boot_mem_map`, i.e. from the DT `/memory` node | `docs/COLD-BOOT-TX-WEDGE.md` §9.1-9.2 |
| 38 | "`ramoops: attached 0x20000@0x3fe0000` in the boot log means the carve-out is working" | **PROVES ONLY THAT THE DRIVER BOUND.** `of_platform_default_populate_init()` special-cases ramoops and `ramoops_parse_dt()` reads `reg` straight from the DT — **the binding path never consults the reservation.** This log line was present throughout the entire three-session hunt while the memory was being handed to the allocator | `docs/COLD-BOOT-TX-WEDGE.md` §9.1 |
| 39 | ★★ **Our own** §2 claim: "the wedge is ramoops overwriting DRAM the allocator handed to the rtl819x driver" | **RETRACTED THE SAME DAY IT WAS WRITTEN.** The overlap is real and was later *demonstrated* (`CPURPDCR0=03fe2000`, the RX ring inside the old ramoops window) — but it is not sufficient. Removing the overlap by every available means still wedges TX: `/memory` withheld so the region is outside the kernel map (5/5 pass with pstore off, **fail with it on**), `PSTORE_CONSOLE=n`, `unbuffered`, and relocating to a hole at 48 MB — 2/2 failing trials each. ★ The mechanism was inferred from one correlation and written up as settled; only the missing control (shrunk `/memory` **with pstore off**) exposed it | `docs/COLD-BOOT-TX-WEDGE.md` §9.6-9.8 |

### The ones that cost the most

**#8 — the flattened vendor PHY tables (`5cdd707ff9` / `d5a4a33746`).**
`array_mp_8197f_phy_reg` is 1492 pairs, of which only **59 % are real registers**. Entries like
`0x80001003` and `0x40000000` are Realtek **conditional-branch markers**, so a flat replay writes
garbage into the PHY. The decisive read was not a sweep but two values off the silicon: cut
version = 1, and the vendor gates the entire header-table path on `p_dm[0x3E4] = (cut >= 2)` —
**stock does not apply those tables on this silicon at all.** From the commit:

> A wrong-but-well-formed register set is the hardest failure to debug.

**#12 — a whole session measured with the offload switched off (`38d335f93b`).**
`hwnat` is a module parameter that defaults to **off** (`rtl819x_hwnat.c:69-71`,
`bool rtl819x_hwnat_enabled;` — uninitialised) and the boot-time ASIC programming deliberately
did not arm it at the time (the service's "hwnat is deliberately NOT armed here" comment;
since R4 the service DOES arm it, after the warm-up).
Every R6 number in that session therefore measured **software forwarding**, and twelve
hypotheses were "falsified" against it. Redone with a **positive control**:

| arm | CPU pkt/MB | reading |
|---|---|---|
| software (`hwnat=0`) | 382 | baseline |
| forward, `hwnat=1` | 147 | **−61 % — the metric can detect offload** |
| reverse + B1 + B2 | 355 | −7 % — no meaningful reverse offload |

Two other results were settled in the same redo: the **ARP index is not a hash** —
`arpIndex = route->arpsta + (ip & ~ipMask)`, purely positional; and **B3 measured harmful**
(100 % loss, no recovery) because `napt_clear()` is also the teardown path for *active* flows.
B4 was no-effect (`ea6de9f464`); B3 reworked as an init-only prefill was safe but still no
effect (`37ed423dac`).

**#13 — the false negative that closed the project's central question for a week
(mirror `537cb63` → `8d7f7d0`).**
"Stock does not offload the reverse path either" was **measured correctly** and was still
wrong. The negative rested on counting **packets** on stock's netdev — and **stock's counters
include ASIC-forwarded frames**: `br0` rx incremented by the full 1183 MB while the CPU logged
**9 jiffies** of work. Re-measured on the gigabit bench:

| firmware | up | CPU | down | CPU |
|---|---|---|---|---|
| stock D-Link | 913 Mbit | ~0.9 % | 923 Mbit | ~1.6 % |
| this port (then) | 177 Mbit | 94 % | 173 Mbit | 86 % |

★ **Bytes cannot pass through an idle CPU. CPU time is the only valid cross-firmware metric.**

**#14 — "~90 % offloaded" was GRO (mirror `efedad9`).**
CPU-seen packets averaged **5242 bytes** (one session ~13,900 B). Comparing GRO super-packets
against wire packet counts makes a **fully CPU-forwarded** flow look ~90 % offloaded:
**5,670 GRO super-packets over ~79 MB — ~13,900 bytes per CPU-seen segment** (`efedad9`), i.e. each one is roughly nine 1500 B wire frames coalesced. (An earlier revision of this line quoted "~54,000 wire packets"; that figure was back-derived from 79 MB / 1500 B, not measured, and belongs to a different flow in `e677d7cb3b`.) The honest number for the same run:
**215.4 MB through the CPU for a 203 MB payload = 106 %, i.e. zero hardware offload.** Stock's
0.2 pkt/MB is not commensurable with ours at all, because stock runs kernel 3.10 **without
GRO**. This is where the project adopted **payload bytes through the CPU** as the metric
(§5 rule 1).

**#17 — the root cause, held backwards for eleven days (`fd2cf06a07` → `8d315c331b`).**
The RTL8197F wants plain **numeric** values; the vendor's `htonl()` at the ASIC boundary is
`ntohl()` in disguise, because its `naptEntry` fields are raw `__be32`/`__be16` taken straight
from conntrack. Proven on the stock binary for this exact board, where one numeric value feeds
both the index and the key:

```
801ae9cc  lw   a1,4(s0)      ; naptEntry->intIp (__be32)
801ae9d0  wsbh a1,a1
801ae9d4  ror  a1,a1,0x10    ; = ntohl -> NUMERIC
801aea00  jal  0x8019df88    ; rtl8651_naptTcpUdpTableIndex   <- index
801aed08  lw   v0,4(s0)
801aed0c  wsbh v0,v0
801aed10  ror  v0,v0,0x10    ; = ntohl -> NUMERIC (same value)
801aed14  sw   v0,52(sp)     ; asic_nat.insideLocalIpAddr     <- key
```

The driver already de-swapped to numeric (`int_ip = ntohl(...)`) and then **re-swapped** for the
index and for G. ★ Note the second half of the old `M7-HWNAT-REVERSE-NAPT.md:370` was **right**:
`gw_napt_hash1()` *is* a verbatim-correct transcription of the vendor hash. It was the **inputs**
that were wrong — which is precisely why single-variable A/Bs kept reading flat (#21). Result
after `key + index + G` numeric together: 489 Mbit upload with **1304 bytes** across the CPU for
a 611 MB payload = 0.0 %. See `HWNAT-OFFLOAD.md`.

**#18 — the archetypal false positive (`dc372efd28`, retracted by `224f31964e`).**
Announced as "FIRST HARDWARE FORWARDING". Two *independent* defects, either sufficient to
invalidate it:

- **(a) The oracle scored the wrong frames.** The `hwFwd=1` frames it counted had `l2Tr=0`
  (`ph_asic0` bit 2) and `vid=2` — periodic **broadcasts being L2-flooded** with the CPU in the
  flood list (`extPL=12`), which the ASIC has always done. They decode to **reason 8, not 7**.
  The sweep filtered on `port=2` and ignored `l2Tr`: **it scored bridging as offload.**
- **(b) It measured rows that did not exist.** `hwnat_program_rows()` wrote the key with the
  knob-selected byte order but verified the readback against `htonl(int_ip)` **unconditionally**.
  With `napt_key_htonl=0` every install wrote both rows, failed readback, tore both down and
  returned `-EIO` — *before* the log line, making the symptom indistinguishable from the offload
  offer never arriving. Every decline path was additionally a silent `-EOPNOTSUPP`. Fixed by
  adding `hwnat_debug`, default **on** (`rtl819x_hwnat.c:155-157`).

**#21 — two clean A/Bs, both misleading, because the variables are coupled.**
`napt_idx_htonl=0` alone cannot win while `napt_g_htons=1` still corrupts G on a hit. The fix
required **key + index + G + the inbound verification hash numeric together**. ★ Eliminating
coupled variables one at a time is invalid, and it does not fail loudly — it produces
publication-quality flat results.

**#22 — the same subsystem, wrong in both directions (`42ecc2308d` → `261c833bd4`).**
First OOM was blamed for the 2.4 GHz AP failure — wrong; setup provably never ran (the netifd
handler had not registered its driver), memory or not. Then OOM was declared "not the cause of
the WiFi problems" — **also wrong**: OOM *is* the cause of the 5 GHz regression on a RAM boot.
The netifd log is unambiguous:

```
radio0 (1956): ./mac80211.sh: eval: line 30:  can't fork: Out of memory
radio0 (1956): Could not find PHY for device 'radio0'
```

"Could not find PHY" is a **consequence**: mac80211's PHY detection forks helpers, the forks
fail, so it cannot find a PHY it has already enumerated. Only 5 GHz loses because
`rtl8192cd` does WPA2-PSK **in kernel** with minimal forking, while mac80211 forks repeatedly
and then forks hostapd. `261c833bd4` carries the correction **in both directions** in its own
message — that is the pattern to copy.

**#25 — a refuted breakthrough, with the causality inverted (mirror `9c2aabd`).**
Boot 95 with a freshly reset chip stayed dead 600 s; boot 96 with a residue-laden chip came up
alive. ★ The `8367S` port6 `ifOut` "freeze" was **802.3x backpressure from the undrained SoC
FIFO**, not the external switch failing. The real cause was two omissions in the ported
`rtl865x_start()`:

1. the vendor's 8197F **"driver can't receive packet" erratum poke** — a read-modify-write of
   ACL table entry 0 (`sdk-ref/rtl865x_asicCom.c:1411-1413`, `#if RTL_8197F`), now
   `REG32(0xBB0C0000) = REG32(0xBB0C0000);` at
   `files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x-eth.c:514`;
2. **any engine quiesce before ring programming** — the D-Link loader leaves `CPUICR` `RXCMD`
   set, so `New_swNic_init()`'s `CPURPDCR0`/`CPUTPDCR0` writes have no 0→1 edge to re-latch the
   ring bases. Fixed with `rtl865x_down()` at the top of `open()` (`rtl819x-eth.c:1310-1314`).

10/10 boots RX-live afterwards, versus a 33–55 % dead-boot baseline.

**#31 — a "needs a human check" item that was both solvable and dangerous (`70c16cab8f`).**
The first LED node used `<&gpio1 25>` — **one pin away from `<&gpio1 26>`, the RTL8367S hardware
reset**. A second mechanism has the same effect independently: `bgpio_set()` rewrites the whole
cached 32-bit DAT word from a snapshot taken once in `bgpio_init()`, so **any** gpio1 LED write
rewrites bit 26. The real WPS LED is `<&gpio0 22> GPIO_ACTIVE_LOW`
(`files/target/linux/realtek/dts/GWR1200ACV1.dts:107`) — a different bank, both hazards gone.
The statically predicted gpiochip bases 448/480 matched live.

---

## 4. Bench confounds (the consolidated catalogue)

These were previously split across three files. Each one has produced at least one wrong
conclusion on this project. Numbering is preserved from the original notes.

**#4 — THE BOX REBOOTS MID-TEST.** A RAM boot is volatile and the box can reboot under load. A
run that spans a reboot silently mixes two configurations into one number.

**#5 — `ip neigh flush` BEFORE MEASURING FAKES A TOTAL OUTAGE.** It recreates the cold-unicast
condition: the ASIC L2/ARP tables start empty and cold unicast is not delivered until traffic
has flowed. Same boot, no code change — flush+ping gave 100 % loss on all three paths, warm
tables gave 0 %/0 %/0 %. This confounded an entire bisect whose conclusions (#4, #5) were
retracted.

**#6 — A KILLED `iperf3` CLIENT WEDGES THE SERVER.** `iperf3 -s` handles **one** test at a time;
after a timeout-killed client it accepts nothing, silently turning trials into zero-byte runs
that read as negatives. Restart the server per measurement **and** assert `ss -lnt` shows the
listener — `hwnat-measure.sh:39-46` does exactly this and refuses to print a number otherwise.

**#7 — A BOX IN THE LARGE-FRAME RX WEDGE STILL ANSWERS DEFAULT-SIZE PINGS.** Preflight every
measurement at **64 B and 1400 B**, 0 % loss, or the run is not a result
(`hwnat-measure.sh:27-37`; see `docs/M7-LARGE-FRAME-RX-WEDGE.md`).

**#8 — BENCH GUARDS DRIFT.** NetworkManager strips the USB NIC's IPv4 and re-adds a default via
the house gateway, stealing `172.16.0.0/24`; the WAN peer's address lives on `br0` and is lost
on reboot; the loader's nvram IP resets to `192.168.1.6`. **Every drift reads as 100 % loss.**
This already caused one wrong published conclusion (`SWTCR0.WANRouteMode`, index #10). Re-assert
before *and* after: `bench-up.sh:24,28,47,48`, `hwnat-ab.sh:39-40`.

**#9 — STRAY BACKGROUND PINGS FAKE SUCCESS.** A leftover host→box ping produced a fake box→host
"5/5" and "20/20 warm".

**#10 — A HARD FABRIC WEDGE SURVIVES A WARM RAM RELOAD.** Only a physical power-cycle clears it.
Never trust large-frame results taken after a session of wedge testing.

**#11 — SERIAL-READ FAILURES MASQUERADE AS DEAD HARDWARE.** A degraded USB-UART returns empty
counter reads after 3–5 rapid reload cycles, which was repeatedly scored as "RX dead". Prefer
serial-independent checks. Related: a printk burst makes the box's UART drop characters, which
truncates a command mid-quote and wedges the shell at a `>` continuation prompt — hence the
6-character chunked writes and `dmesg -n 1` in `hwnat-ab.sh:19-30`.

**#12 — A PING INTERVAL THAT IS A HARMONIC of the driver's 100 ms watchdog poll gives a phantom
flat RTT.** `-i0.2` produced a fake flat 40 ms.

**#13 — RAM-BOOT AND NOR-BOOT ARE NOT INTERCHANGEABLE** for anything memory-sensitive. A fresh
RAM boot has `MemAvailable` **984 kB** against **7576 kB** on NOR; after ~20 min of testing a RAM
boot cannot fork at all. On a RAM boot the rootfs is a RAM-resident initramfs and is
unreclaimable. This is the whole of index #22's 5 GHz half.

**#14 — A SOFTWARE-FLOWTABLE A/B WAS INVALID** because `iptables -D` did not match the fw3 rule
(fw3's rule carries `-m comment`), so **both** arms had offload active.


**#15 — `openwrt/files/` SILENTLY BAKES THE PRIVATE HOUSE PROFILE INTO EVERY IMAGE.**
An earlier session left `etc/ap-profile/` and `etc/uci-defaults/99-zzz-dir842-ap` in
`openwrt/files/`, which OpenWrt copies into every build. Images built with a plain `make`
and believed "generic" were in fact house-profile, **bridge role**, `192.168.100.3`,
hostname `gwr1200ac-ap`. This hid the router-vs-bridge variable for three sessions. For a
true generic build, clear `openwrt/files/` and confirm the box reports `OpenWrt` /
`192.168.0.1`.

**#16 — A WRONG TARGET IP IS INDISTINGUISHABLE FROM A TOTAL OUTAGE.** The pre-ramoops
reference image boots at `192.168.0.1`; an ARP test aimed at `192.168.100.3` returned
0/20 and **nearly refuted the one correct hypothesis of the whole hunt**. Confirm
`uci get network.lan.ipaddr` before trusting any zero-reply result. (This is #8 again,
in a new costume.)

**#17 — NEVER RESET THE FABRIC AFTER THE RINGS ARE PROGRAMMED.** `rtl819x_fabric_full_reset()`
invalidates CPU-port descriptor state and must be followed immediately by
`New_swNic_init()`. Calling it from inside `rtl865x_start()` (i.e. after the rings are up)
wipes them: `CPUIISR` bit 17 `PKTHDR_DESC_RUNOUT` asserts forever, producing an IRQ storm
that escalates into a tty-layer oops and needs a power-cycle **and reflash**.


**#18 — A PSTORE BACKEND AND A RESERVATION CHANGE IN THE SAME BUILD PANICS THE BOX.**
Re-enabling `CONFIG_PSTORE_RAM` **and** `CONFIG_PSTORE_CONSOLE` while the carve-out was
still (unknowingly) unreserved killed the box in ~74 s: a slab freelist pointer overwritten
with `0x30303030` — ASCII `"0000"`, i.e. console text — then `Kernel panic … Fatal
exception in interrupt`. ★ Prove the memory is out of the allocator FIRST, on a build with
pstore off, using the `Memory:` / `MemTotal` arithmetic. Only then turn a backend on.
(It reboots and recovers on its own; no reflash needed.)

**#20 — A SINGLE ARP BURST AT t+45 s CAN READ AS A TOTAL WEDGE ON A HEALTHY BOX.** A known-good
purged build was caught at `t45=0 t70=20` — the box simply had not finished bringing
`br-lan` up at 45 s. ★ Any trial that samples once, early, can manufacture a failure that
looks identical to the CPU-TX wedge. `tools/bench/autoboot-trial.py` therefore bursts twice
(t+45 s and t+70 s) and judges on the later one. The lone unexplained failure in the
"15/16" ramoops-free run was measured with a single early burst and is most likely this.

**#23 — A HIGHER REGULATORY CEILING IS NOT MORE SIGNAL.** BR allows 17 dBm on 5 GHz ch36 but
30 dBm on ch149, so moving the AP up looked like a free +13 dB. `iw` duly reported
`txpower 17.00 -> 30.00 dBm` — and the client's RSSI went **-67 to -87 dBm**, i.e. ~20 dB
WORSE. On this unit the upper band radiates far less than the lower one regardless of what
the regulatory table permits. ★ Measure the client, never the `txpower` the driver claims.

**#22 — "write RF mode table fail" IS NOT A MISTUNED CRYSTAL.** `03-rtw8822b-blank-efuse-rfe.patch`
reasons that the blank efuse leaves `crystal_cap = 0` and that this is "consistent with the RF
synthesiser being off-frequency from a mistuned crystal", and adds `xtal_cap_override` with the
instruction "sweep it instead of guessing". ★ The sweep was run and the hypothesis is REFUTED:
the failure is **intermittent (~50%) and independent of the value**. The four values decoded from
the stock MAC partition (39, 49, 35, 24) all failed; a coarse 0-63 sweep gave a non-monotonic
scatter (16 ✓, 28 ✗, 44 ✓, 56 ✗, 63 ✓); and re-running a SINGLE value three times gave
pass/fail/fail at 0 and pass/fail/pass at 16. A detuned crystal would produce a contiguous
working range, not a coin flip. This is a probe-time race in RF register access.
★ `xtal_cap_override` is a dead end — do not sweep it again.

**#21 — AN INTERFACE COUNTER MEASURES THE WHOLE LAN, NOT THE BOX.** A kill-switch harness
watched `enp4s0` rx pkt/s to catch the box "flooding the LAN", and duly fired at
10512-17777 pkt/s — on BOTH arms of an experiment, including the control. ★ All false: the
counter includes the bench host's own downloads and every other host's traffic. An
attributed capture over a full boot showed the box emitting **12 frames in 100 s** while
the gateway and the bench host accounted for 109k. Two hypotheses built on that reading
("a wedged box floods the LAN", and a `br-lan` STP-loop theory) were withdrawn. Count
frames **by source MAC**; never trust an interface total on a shared segment.

**#19 — `memblock=debug` VIA DT `bootargs` PRODUCES NO OUTPUT ON THIS PLATFORM.** Zero
`memblock_dbg()` lines, not even from `bootmem_init()`'s `memblock_add_node()` calls,
despite `/proc/cmdline` showing the flag, both the macro and `early_param("memblock")`
compiled in, and an unwrapped log buffer. **Cause not established.** Do not budget a cycle
on this expecting a trace — use the `Memory:` line, which is direct and needs no flag.

---

## 5. Measurement rules

Each is an imperative, and each traces to a retraction above.

1. **MEASURE PAYLOAD BYTES THROUGH THE CPU** — never throughput, never packet counts.
   `delta(/proc/net/dev rx bytes) / payload`, on `eth0.2` for upload and `eth0.1` for download.
   Implemented at `hwnat-measure.sh:50-66`. *(#14)*
2. **THROUGHPUT CANNOT DISTINGUISH "OFFLOADED" FROM "THE CPU IS KEEPING UP".** Commit
   `f50362be64` measured **812 up / 371 down on the same offloaded boot**, both at 0.0 %
   through-CPU. Report throughput, through-CPU %, and CPU-busy **from the same run**. *(#13)*
3. **ALWAYS RUN A POSITIVE CONTROL.** Twelve hypotheses were "falsified" against a metric never
   shown capable of detecting offload. *(#12)*
4. **COUPLED VARIABLES CANNOT BE A/B'd ONE AT A TIME.** Enumerate the coupling first, then move
   the whole group. *(#21, #19)*
5. **STATE THE `hwnat` SETTING OF EVERY NUMBER.** The module parameter defaults to off
   (`rtl819x_hwnat.c:69`), but **since R4 the `dir842-asic` service arms it at boot**, so a
   cold-booted shipped image measures the *offloaded* path unless you disarm it. *(#12)*
6. **NEVER MEASURE OUTSIDE THE HARNESS.** The asserts exist because each one corresponds to a
   conclusion this project already published and retracted. *(#20)*
7. **ONE CLEAN RUN PROVES NOTHING FOR THE WEDGE CLASS OF BUG** — it reproduces ~1 in 3–4 heavy
   attempts. Compare **reset counts** across several runs per setting, and A/B across **cold**
   boots, because flipping a knob at runtime cannot recover an already-wedged engine. *(#25, #26)*
8. **CHECK COUNTERS BEFORE DECLARING A PATH DEAD.** Reading the log for what *did* arrive
   reframed a "100 % dead" datapath into "received but not delivered" and found the bug. *(#24)*
9. **CHECK YOUR PRINTER.** `print_hex_dump(..., groupsize=1, ...)` prints raw memory order;
   confirming `groupsize` is what proved the **RX buffer**, not the printer, was byte-swapped.
   *(#24)*

---

## 6. Method lessons

★ **`grep -a` IN THE VENDOR SDK.** `AsicDriver/rtl865x_asicL2.c` is ISO-8859 text and
`rtl8367r/rtk_api.c` is non-ISO extended-ASCII (`file -b` on both). **Plain `grep -r` skips them
silently as binary.** The CPU-tag reference implementation sat there unfound for the whole
project. This cost findings at least twice.

★ **THE SHIPPED STOCK BINARY BEATS THE PUBLIC SDK.** Three separate answers came from
disassembling firmware that works, not from reading SDK source:

| item | public SDK | shipped stock | where |
|---|---|---|---|
| `DMA_CR0` FIFO marks | `0xA0A0` (`asicCom.c:1408`) | **`0xA0CE`** (Low `0xA0` / Hi `0xCE`) | stock vmlinux `0x80192be4`, `ori v0,v0,0xa0ce` on the chip-ID==0x8197 branch |
| `CPUIIMR` bit16 `MBUF_DESC_RUNOUT` | armed | **masked** — `CPUIIMR = 0x807E31FE` | `rtl819x-eth.c:65,127` |
| NAPT key/index byte order | "network order" | **numeric** | `0x8019df88` / `0x801ae9cc` (index #17) |

The public SDK ships a **degenerate `DMA_CR0`** with `HiFifoMark == LowFifoMark` — a
no-hysteresis drain config (`rtl819x-eth.c:486-503`).

**REPRODUCE KNOWN-GOOD, NOT "MORE VENDOR-CORRECT".** `CPUICR1` is deliberately set to **bit 1
only** (`rtl819x-eth.c:481`, `REG32(CPUICR1) |= (1u << 1);`) even though the vendor line also ORs
`CF_TXRX_DIV_LX` (bit 0) and `CF_TSO_ID_SEL` (bit 4) — because **every boot that measured
890/900 Mbit ran with exactly `0x82`**. The comment at `:473-479` states this as policy.

**PREFER DECODING THE STOCK BINARY TO BOOTING STOCK — BUT USE BOTH.** Commit `16277f280e`
decoded the row layout, hash, index, byte order, aging seed and commit protocol from the binary
(`0x8019e1e4` vs `rtl819x_hwnat.c:244-292`; hash `0x8019df88` vs `gw_napt_hash1`) and correctly
concluded "the row is not where the bug lives". But **booting stock** is what finally overturned
#13 and #30. Decoding is cheaper and safer; booting is the only thing that produces a
cross-firmware ground truth.

★ **STALE COMMENTS OUTLIVE THE MODEL THEY DESCRIBE.** A stale comment in this project's own
header seeded the 4-way hypothesis (#11); Fork A's comments survived Fork A (#27). **Three are
still in the shipped tree today** — kept deliberately, listed here so they are read as history,
not as fact:

| file:line | what it says | why it is stale |
|---|---|---|
| `rtl819x_hwnat.c:471-481` | "The ASIC hashes/keys the ON-WIRE (network-order) header fields" | **Backwards** — index #17. The code below it is numeric |
| `rtl819x_regs.h:166` | `SW_CPU_PORT 6 /* CPU = L2 port 6 (bit 6) */` | CPU-tag mode moved the CPU to **port 8** (`74b90e83ba`) |
| `base-files/etc/board.d/01_leds:49-52` | "5 GHz is rtw88; the on-SoC 2.4 GHz radio has no driver yet" | The vendor `rtl8192cd` driver **is** now used for 2.4 GHz (`d4ad0ee`, mirror) |

**THE ANSWER CAME FROM DISASSEMBLING FIRMWARE THAT WORKS.** The closing line of the commit that
cracked hardware NAT (`8d315c331b`), quoted in full because it is the summary of this whole
file:

> Every register-value hypothesis this project generated was wrong; the answer came from
> disassembling firmware that works.

---

## Related

- `docs/M7-HWNAT-REVERSE-NAPT.md` — the seven-layer reverse-NAPT hunt. **Lines 42 and 370 are
  retracted by index #17** and are kept as written.
- `docs/M7-LARGE-FRAME-RX-WEDGE.md` — the wedge behind confounds #7 and #10; index #2 is its
  open item.
- `docs/VENDOR-PARITY-INVENTORY.md` — §161 ("stock does 600–800 Mbit is NOT evidenced") is
  itself superseded by index #30.
- `docs/M7-TRUNK-FORWARDING-FIX.md` — the RGMII trunk fix; index #25 revisits its 8367S reset.
