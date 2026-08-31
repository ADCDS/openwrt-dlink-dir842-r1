# The cold-autoboot CPU-TX wedge — root cause, fix, and the fingerprint

**Status: FIXED IN PRACTICE; MECHANISM NOT ESTABLISHED.** Symptom: on a genuine hands-off
power-on boot of the flashed image, everything the box *originates* on the wire is silently
dropped — ARP replies, its own pings — while RX works perfectly. Trigger: **an attached
`ramoops`.** Fix: remove the carve-out (`GWR1200ACV1.dts` node, `CONFIG_PSTORE_RAM=n`,
`CONFIG_PSTORE_CONSOLE=n`). Nothing in the ethernet driver was needed.

> ★ **Read §9 before trusting §2.** §2 attributes the fault to ramoops writing into DRAM
> the allocator had handed to the driver. That overlap is **real and demonstrated** (§9.7:
> the RX ring lands at `CPURPDCR0=03fe2000`, inside the old window) — but it is **not
> sufficient as an explanation.** Removing the overlap entirely, by every means available,
> does not stop the wedge (§9.6, §9.8). Treat §2's mechanism as unproven; the correlation
> "ramoops attached ⇒ wedge" is what is solid.

This bug consumed three full debugging sessions. Most of that time was spent proving
things that were *not* the cause, because the fault leaves **no fingerprint in any
register on either chip**. This document is written so nobody re-walks that.

---

## 1. The fingerprint — how to recognise this bug in ten seconds

★ **`CPUTPDCR0` doubles as the TX engine's *current descriptor pointer*, exactly as
`CPURPDCR0` does for RX.** That single register identifies the fault instantly.

| | wedged | healthy |
|---|---|---|
| `CPUTPDCR0` | ends `0x000` — **never leaves the ring base** | advanced (`...0c0`, `...0ec`, `...0b8`) |
| TX ring | `cur` climbs to 255, `done` **frozen at 0** | `cur == done` |
| SoC port0 `ifOutUcastPkts` | **exactly 0**, forever | climbs 1:1 with TX |
| RX | works normally | works normally |

Measured across the hunt: `CPUTPDCR0` ended `0x000` on **10/10** wedged boots
(`0244b000 0149c000 01418000 02460000 0245a000 03fa9000 0243b000 01bd6000 030cd000
01415000`) and had advanced on **7/7** healthy ones (`0273c0b0 014a50b8 01bc30b8
0270c0b0 0273c0fc 01425068 01bd4118`).

So: the CPU hands descriptors over correctly (own bit set, `TXFD` doorbell rung, `TXCMD`
set, ring base programmed, `TXRINGCR` enables set — all verified identical to a working
boot) and **the switch's TX DMA engine never fetches even descriptor #0.**

⚠ **The "unicast dies, broadcast works" framing in the original bug report was wrong.**
Port0's `outMulticastPkts`/`outBroadcastPkts` do climb on a wedged box, but that is
jack-to-jack flooding across the CPU-tag-multiplexed trunk, not CPU-originated traffic.
*All* CPU-originated TX is dead. Chasing a unicast-specific classifier cost most of one
session.

## 2. Root cause

`GWR1200ACV1.dts` carved the last 128 KB of DRAM for pstore/ramoops, and its comment
claimed *"the kernel excludes this range from the normal allocator, it is not 'lost' RAM
in any other sense."*

★ **That is false on this platform, and it was already known to be false.** The note in
`rtl8197f/config-4.14` (added when `CONFIG_PSTORE_CONSOLE` was turned off) states it
outright: the region

> "is not actually excluded from the allocator on this platform (the `/reserved-memory`
> scan is not reserving it), so the rtl819x driver allocates RX clusters inside it"

That is the *same already-proven mechanism* behind the `skb_shared_info` corruption that
commit `317f252` mitigated with a guard in `rtl819x_swnic.c` — and whose writer that
commit's own comment admits was never identified ("the actual writer ... isn't fully
pinned down yet"). The RX symptom got a guard. The underlying "ramoops scribbles on live
DMA memory" defect was left in place, and it also takes out the CPU TX path.

**Why it left no register fingerprint:** the corrupted state is in DRAM the DMA engine
reads, not in any register. Three sessions of exhaustive register diffing — every
switch-core register, the whole CPU-interface block, and the entire reachable RTL8367S
register space — correctly returned *bit-identical* results between working and wedged
boots. The comparison was sound; it was looking in the wrong medium.

**Why it depended on the boot path** (see §4): autoboot and the loader's `J` command
differ in console volume and timing before driver init, which changes allocation order and
therefore what lands in the un-reserved region.

## 3. The fix, and what to verify

`files/target/linux/realtek/dts/GWR1200ACV1.dts` — `reserved-memory` / `ramoops@3fe0000`
node removed. `files/target/linux/realtek/rtl8197f/config-4.14` — `CONFIG_PSTORE_RAM=n`
(`CONFIG_PSTORE_CONSOLE` was already `n`).

> ★ **This is no longer the shipped fix — see §9.4.** The carve-out is back, with the DRAM
> withheld via the `/memory` node instead of reserved, and both pstore backends are on
> again. What stays true is the mechanism: ramoops writing into allocator-owned DRAM is
> what wedged TX.

**Verified 7/7 on genuine hands-off power-cycle autoboots**, house profile, bridge role,
`192.168.100.3` — the exact configuration that failed every previous attempt. Each trial:
`sudo python3 tools/bench/arpburst.py enp4s0 20 192.168.100.3 0.05` → 20/20 replies.
Healthy fingerprint on the fixed build: `[txring] cur=59 done=59 CPUTPDCR0=01bf90ec`,
port0 `outUcastPkts=65`, `ping` 0% loss.

★ **Acceptance for any change in this area is ≥5 independent power-cycle trials**, per
`RETRACTIONS-AND-METHOD.md` rule 7. One clean boot proves nothing for this bug class.

### Cost of the fix, and the better end-state

> ★ **Superseded — see §9.** Crash logging is back, and without any reservation: the DT
> `/memory` node now withholds the top 128 KB outright. The paragraph below is kept because
> it records what was believed at the time, and §9.2 shows the "make the reservation take
> effect" plan it proposes does **not** work on this platform.

This **loses persistent crash logging** (`/sys/fs/pstore/`), which `317f252` added
deliberately and which is genuinely useful here. The better end-state is to make the
reservation actually take effect — find why `/reserved-memory` is not honoured on this
platform (`early_init_fdt_scan_reserved_mem()` ordering, or an explicit `memblock_reserve()`
in `arch/mips/realtek/setup.c`) — so the region is truly excluded and ramoops can come
back with crash logs intact and no DMA corruption. If that lands, the `skb_shared_info`
guard in `rtl819x_swnic.c` and `CONFIG_PSTORE_CONSOLE=n` are both mitigations for this
same root cause and may become unnecessary.

## 4. Bootloader behaviour (RE'd — reusable beyond this bug)

Verified two independent ways: disassembly of this board's own `mtd0-boot.bin`, and the
matching upstream source tree `github.com/7felix7/bootcode_rtl8197f` (identified by a
character-for-character match on the loader's own log strings `"---Ethernet init Okay!"`
and `"---Escape booting by user"`).

★ **Every *automatic* jump-to-kernel path clears `CLKMANAGE` (`0xB8000010`) bit 11 — the
switch/NIC core clock gate — immediately before jumping. The interactive `J` monitor
command has those same lines present but commented out.**

The autoboot teardown, in order: `0xB8003000=0` (timer0); `0xB800350C |= 0x00400000`;
`0xB8003004=0` (timer1); `0xB8003114=0`; **`0xB8000010 &= ~(1<<11)`**;
`0xBBDC0300=0xFFFFFFFF`; `0xBBDC0304=0xFFFFFFFF`; mask IRQs; cache flush; jump.
`J` does only: stop timer0, mask IRQs, cache flush, jump.

Also: **`"---Ethernet init Okay!"` never appears on a genuine hands-off autoboot** — the
loader's own switch/PHY bring-up runs only on the interactive ESC-catch path. And
`0x1219` (the CPU-port mask the driver's cold-bring-up gate keys on) reads `0x0040` on
*every* boot path on this board, so `rtl8367b_setup()`'s EXT1 cold bring-up has **never
executed on this hardware**. Forcing it to run changes nothing (§5) — but it also does
*not* desync the RGMII pair, contrary to the warning in `M7-TRUNK-FORWARDING-FIX.md`,
which applied to a *partial* replica combined with a gpio474 hardware reset.

This clock-gate asymmetry is real and was the best-supported hypothesis for a long time —
it correctly predicts *which* boot paths fail. It is nonetheless **not** the mechanism:
acting on it every available way does not fix the fault (§5).

## 5. Refuted hypotheses — do not re-test without new evidence

All tested on hardware, in this bug's own context.

| # | hypothesis | verdict |
|---|---|---|
| 16 | Switch-core clock gate (`SYS_CLK_MAG` bit 11) left off | **REFUTED** — reads `0x80047800` (bit 11 already set) at `rtl865x_start()` entry on a wedged boot; `setup.c:129` forces it on far earlier, every path |
| 17 | Wall-clock hardware settle time | **REFUTED** — +15 s before switch bring-up, no effect |
| 18 | `ph_asic0` / `srcExtPortNum` descriptor field wrong for CPU-tag mode | **REFUTED** — 2-bit field, all 4 values swept live, none fixed it |
| — | Jack-PHY auto-negotiation restart (`0x2000/2020/2040/2060/2080 = 0x1340`) | **REFUTED** — written live, no effect |
| 19 | `rtl819x_fabric_full_reset()` as first statement of `rtl865x_start()` | **CRASHED, invalid test** — see §6 |
| 20 | External-chip EXT1 cold bring-up never runs (gated on `0x1219==0`) | **REFUTED** — forced it to run for the first time ever; trunk still came up correctly; wedge unaffected |
| 21 | Switch-core reset at the earliest kernel moment (`setup.c`, clock just restored, no driver state) | **REFUTED** — `SIRR` self-cleared, `MEMCR` reached `0x2400` done, box booted clean, wedge unaffected |
| 22 | `CPUICR` bit22 `SOFTRST` ("re-initialize all descriptors") before ring programming | **REFUTED** — `CPUTPDCR0=02457000`, still at ring base |
| 23 | Full `fabric_full_reset()` at first `open()` only, correct hang_work ordering | **REFUTED** — reset verifiably ran, `CPUTPDCR0=02454000`, still at ring base |
| — | Register parity, both chips (incl. `0x1d11`, `0x1233/37/39/3a`, `0x18e0`, `0x1d32`, `0x09da`, `0x03f7`) | **all bit-identical** wedged vs working; `0x1d11=0x1500` (both "RGMII pads disconnected" trap bits already clear) |
| — | Bridge vs router role / `gw_prog` / `fabric_reset=3` at boot | **REFUTED** — a passing image forced to bridge role, with `dir842-asic` logging "bridge role: skipping gw_prog" and `fabric_reset` run 0 times, still passes |

★ Taken together, #19/#21/#22/#23 close out **"resetting the NIC or switch engine at any
point in the boot"** as a cure — correctly, since the fault was never in the engine's
registers.

**Live engine state worth knowing:** two undocumented CPU-interface words are *stable and
different* between wedged and working boots — `0xB8010048` (`48048fbe` vs `00800040`) and
`0xB8010050` (`90830124` vs `02778902`, the working value being a plausible in-DRAM
pointer and the wedged one not). They read `0x00000000` with the engine stopped, so they
are derived internal state, not config, and are not writable. They are a *readout* of the
fault, not a lever on it.

## 6. The #19 crash — and the ordering invariant it exposed

Calling `rtl819x_fabric_full_reset()` from inside `rtl865x_start()` produced, reproducibly
2/2: an interrupt storm (isr/poll counters >20000/sec, `rx_pkts` frozen at 0, `CPUIISR`
stuck at `0x00020000`) escalating into a kernel oops in the serial tty layer
(`n_tty_receive_buf_common`) — a full crash needing power-cycle **and reflash**.

★ **`CPUIISR 0x00020000` is bit 17 = `PKTHDR_DESC_RUNOUT` for ring 0.**
`fabric_full_reset()` invalidates the CPU-port descriptor ring state, so it must
**always** be followed immediately by `New_swNic_init()` reprogramming the ring bases.
Both legitimate call sites honour that (`hang_work`: `down → [SOFTRST] → fabric_full_reset
→ New_swNic_init → start`). Running it from inside `rtl865x_start()` executes it *after*
the rings are programmed, wiping them with nothing left to restore them.

**Invariant: never reset the fabric after the rings are programmed.**

## 7. Bench confounds found the hard way

Additions to the catalogue in `RETRACTIONS-AND-METHOD.md` §4.

★ **`openwrt/files/` silently bakes the private house profile into every image.** An
earlier session left `etc/ap-profile/` and `etc/uci-defaults/99-zzz-dir842-ap` there.
OpenWrt copies `openwrt/files/` into every build, so images built with a plain `make` and
believed to be "generic" were in fact **house-profile, bridge-role, `192.168.100.3`,
hostname `gwr1200ac-ap`**. This hid the role variable for three sessions. For a true
generic build, clear `openwrt/files/` and confirm the box reports `OpenWrt` /
`192.168.0.1`.

★ **A wrong target IP reads exactly like a total outage.** The pre-ramoops reference image
boots at `192.168.0.1`; the first ARP test against it was run at `192.168.100.3` and
returned 0/20 — **a false negative that nearly refuted the correct hypothesis.** Always
confirm `uci get network.lan.ipaddr` before trusting a zero-reply result. (This is bench
confound #8, "every drift reads as 100 % loss", biting again.)

- **`uart_daemon.py` dies silently** across long sessions. If commands stop being answered
  or `loader.py catch` fails, check `ps aux | grep uart_daemon` and the `192.168.0.2/24`
  alias on `enp4s0` *before* suspecting the box.
- **Never run `tools/bench/loader.py` under `sudo`** — `tomada` reads
  `~/ekaza-t206m/device.json` from the invoking user's home and silently fails to switch
  power under sudo, which presents as a boot timeout.
- **`J` sent immediately after `FLR`'s `Y`** can be fragmented by the loader's own
  "Flash Read Successed!" printf into two `Unknown command !` lines. Harmless — re-run the
  `j` step.
- The OpenWrt build **fails once with a generic `make -r world: build failed`** after
  almost any `files/` change, then succeeds after
  `make -C <KDIR> ... olddefconfig`. Expect the two-step; do not debug the first failure.

## 8. Tooling

`tools/bench/` (persisted after three sessions of re-inventing it):

- `uart_daemon.py` — **start this first**; single persistent owner of `/dev/ttyUSB0`.
  Drains the port into `dir842-uart.log` at the repo root and sends anything written to
  `uart-cmd.pending`. Everything else here talks through it.
  `python3 tools/bench/uart_daemon.py > daemon.log 2>&1 &`
  ★ It has died silently mid-session more than once, which shows up as a bogus
  `catch: FAIL` or `NO_CONSOLE` — check it is alive before believing either.
- `autoboot-trial.py` — one genuine hands-off power-cycle trial: `tomada` off/on, wait for
  console, ARP burst at t+45 s and again at t+70 s, and on failure (or with `--diag`) dump
  the on-box state that distinguishes a real wedge from a bench confound — LAN IP, `br-lan`
  inet, port-6 `ifOutUcastPkts`, eth0 counters. ★ Never run it under `sudo`.
- `uart.py` — one-shot command/response over `uart_daemon.py`, with prompt detection and
  echo filtering. Recovery for a garbled line: send `-r`, retry with a simpler command.
- `arpburst.py` — **the acceptance instrument.** N ARP requests → count unicast replies =
  a direct measure of CPU-originated unicast TX reaching the wire.
- `loader.py` — scripts the RealTek monitor: `catch`, `flr`, `dw`/`ew`, `j`, `flash`,
  `ramload`, `cmd`. Lets you A/B boot-time state **without reflashing**.
- `debug-procfs-reg.patch` — optional, not applied. Adds `/proc/rtl865x_reg`: the
  CPU-interface register block, per-port SoC MIB, and TX-ring/descriptor dumps from both
  the reclaim and producer pointers. This is what read the §1 fingerprint. Its `w` poke
  writes an arbitrary MMIO address and **will** panic the box on a bad one.

---

## 9. Bringing ramoops back — why `/reserved-memory` is a no-op on this platform

§3 said the better end-state was to make the reservation *real* and re-enable crash
logging. This section is the record of doing that, including the parts that failed. Read
it before touching `/reserved-memory`, `pstore`, or the DT `/memory` node.

### 9.1 The layer under the root cause: mips never selected `OF_RESERVED_MEM`

★ `arm`, `arm64`, `arc`, `powerpc` and `xtensa` all `select OF_RESERVED_MEM`. **`arch/mips`
(4.14) does not.** With it unset:

| Step | What actually happened |
|---|---|
| `arch_mem_init()` → `early_init_fdt_scan_reserved_mem()` | ran — it lives in `drivers/of/fdt.c`, always built |
| → `fdt_reserved_mem_save_node()` / `fdt_init_reserved_mem()` | **no-op inline stubs** from `<linux/of_reserved_mem.h>` |
| net effect | `/reserved-memory` walked, node matched, **nothing reserved** |
| warning printed | **none** |
| ramoops still bound? | **yes** |

`drivers/of/of_reserved_mem.o` was simply never compiled in. Confirm on any build with
`grep fdt_init_reserved_mem System.map` — absent means stubbed.

★ **Why "ramoops: attached 0x20000@0x3fe0000" never meant the memory was reserved:**
`of_platform_default_populate_init()` (`drivers/of/platform.c`) special-cases ramoops by
*path*, entirely outside the reservation machinery —

```c
	/*
	 * Handle ramoops explicitly, since it is inside /reserved-memory,
	 * which lacks a "compatible" property.
	 */
	node = of_find_node_by_path("/reserved-memory");
	if (node) {
		node = of_find_compatible_node(node, NULL, "ramoops");
		if (node)
			of_platform_device_create(node, NULL, NULL);
	}
```

— and `ramoops_driver` then binds through `.of_match_table = dt_match` and reads `reg`
straight from the DT in `ramoops_parse_dt()`. **The binding path never consults the
reservation.** That log line means the driver bound, nothing more.

### 9.2 Enabling `OF_RESERVED_MEM` is NOT sufficient either — measured

`patches-4.14/0009-*` adds `select OF_RESERVED_MEM` to `config REALTEK`. It works as
intended: `CONFIG_OF_RESERVED_MEM=y`, `of_reserved_mem.o` builds, and
`fdt_init_reserved_mem` / `fdt_reserved_mem_save_node` become real `T` symbols in
`System.map` instead of stubs.

★ **The region still was not reserved.** The `Memory:` line is the instrument — subtract the
kernel image, which changes between builds, and compare what is left:

| | no-ramoops build | +`OF_RESERVED_MEM` +ramoops |
|---|---|---|
| `Memory: … available` | 58236K | 58168K |
| `… reserved` | 7300K | 7368K |
| kernel image (code+rwdata+rodata+init+bss) | 6603K | 6668K |
| **non-image reserved** | **697K** | **700K** |

+3K, not +128K. The carve-out is not in the reservation. (`MemTotal` is the same
measurement from userspace: it equals `available + init`, so 58236+1172 = **59408 kB** on
the no-ramoops build — a real 128K reservation must *lower* it.)

★ **The cause is MIPS-specific and structural:** `bootmem_init()` rebuilds all memory state
from `boot_mem_map` — which is populated from the DT `/memory` node by
`early_init_dt_add_memory_arch()` and knows nothing about reservations — calling
`memblock_add_node()` and `free_bootmem()` per entry. It runs *after*
`early_init_fdt_scan_reserved_mem()`. `arch_mem_init()` does end with a
`for_each_memblock(reserved, …) → reserve_bootmem()` transfer loop, but the measurement
above shows the 128K does not survive to the page allocator regardless.

### 9.3 ★ The crash this caused — do not repeat it

Re-enabling `CONFIG_PSTORE_RAM=y` **and** `CONFIG_PSTORE_CONSOLE=y` on the still-unreserved
region panicked the box within ~74 s of boot:

```
CPU 0 Unable to handle kernel paging request at virtual address 30303030
epc : __kmalloc_track_caller+0x154   BadVA : 30303030
Oops[#2] … Kernel panic - not syncing: Fatal exception in interrupt
```

★ **`0x30303030` is ASCII `"0000"`.** A slab freelist pointer overwritten with *text* — the
console ring writing into memory the allocator had handed out. `PSTORE_CONSOLE` mirrors
every printk, so it turns the slow corruption of §2 into a fast one: the first oops's own
register dump fed more text into the ring, corrupting more slab, producing oops #2 and the
panic. The box reboots and recovers on its own; no reflash was needed.

This is the same mechanism as §2, just louder — and it is independent confirmation that
the reservation never took effect.

**Ordering invariant, alongside the one in §6:** never enable a pstore backend in the same
change that is supposed to make the reservation work. Prove the reservation first, with
the `Memory:`/`MemTotal` arithmetic above, on a build with pstore *off*. Only then turn a
backend on.

### 9.4 Attempt: withhold the memory instead of reserving it (worked as designed, did NOT fix the wedge)

Reserving failed twice for two different reasons, so the fix stops trying to reserve at
all. **`GWR1200ACV1.dts` `/memory` now stops 128 KB short of the top of DRAM:**

```dts
	memory {
		device_type = "memory";
		reg = <0x0 0x3fe0000>;	/* 63.875 MB, not 0x4000000 */
	};

	ramoops@3fe0000 {		/* root level -- NOT under /reserved-memory */
		compatible = "ramoops";
		reg = <0x3fe0000 0x20000>;
		record-size = <0x8000>;
		console-size = <0x18000>;
	};
```

Why this is the right shape on this platform:

- **It cannot silently fail.** MIPS builds `boot_mem_map` from this node and derives
  `memblock`, bootmem, `max_low_pfn` and the buddy allocator from it. Memory the kernel was
  never told about cannot be handed out — no reservation plumbing is involved, so there is
  nothing to be stubbed out or rebuilt over.
- **ramoops still binds.** Any root-level node with a `compatible` gets a platform device
  from `of_platform_default_populate()`, and `ramoops_parse_dt()` reads `reg` from the DT.
- **The mapping is better.** With the pages outside the kernel map `pfn_valid()` is false,
  so `persistent_ram_buffer_map()` takes its `ioremap()` path rather than vmap — uncached,
  which is what a crash log wants.
- ★ **The `/reserved-memory` container had to go**, not just become redundant. With a
  shrunk `/memory`, the transfer loop at the end of `arch_mem_init()` would call
  `reserve_bootmem()` on a range starting exactly at `node_low_pfn`, which falls through
  `mark_bootmem()`'s node loop and hits its closing `BUG()`. Shrunk `/memory` **and** a
  `/reserved-memory` node is a guaranteed boot panic.

`patches-4.14/0009-mips-realtek-select-of-reserved-mem.patch` (adding `select
OF_RESERVED_MEM`) was written, measured, and then **deliberately dropped**: with no
`/reserved-memory` node it does nothing, and shipping it would imply reserved-memory works
on this platform when §9.2 shows it does not.

**Verified — the instrument is the total, not just the reserved figure:**

| | full `/memory` | shrunk `/memory` |
|---|---|---|
| `memory: … (usable)` | `04000000` | **`03fe0000`** |
| zone / node 0 | `0x0-0x3ffffff` | **`0x0-0x3fdffff`** |
| `Memory: … available` | `…/65536K` | **`…/65408K`** |

65536 − 65408 = exactly 128 KB, gone from the map (`MemTotal` 59408 → 59260 kB). ★ This is
the check to repeat after any change here: if `/65408K` is not in the boot log, the
carve-out is not working, whatever `ramoops: attached …` says.

> ★★ **This achieved its stated goal and still did NOT fix the wedge — see §9.6.** The
> memory is provably outside the kernel map, and TX dies anyway. Keep the section above for
> the mechanism it documents, but do not read it as a working fix.

### 9.5 Bench note: `memblock=debug` does not work via DT bootargs here

An attempt to trace the reservation with `memblock=debug` appended to the DTS `chosen`
`bootargs` produced **zero** `memblock_dbg()` output — not even from the
`memblock_add_node()` calls in `bootmem_init()`, which certainly run. `/proc/cmdline`
showed the flag, `memblock_dbg` and `early_param("memblock")` are both compiled in, the log
buffer had not wrapped, and `boot_command_line` does carry the DT bootargs before
`parse_early_param()`. ★ **Cause not established — do not spend time on it a second time
expecting it to work.** Use the `Memory:` / `MemTotal` arithmetic instead; it is direct,
needs no kernel flags, and is what actually settled both §9.2 and §9.4.

### 9.6 ★★ The result that breaks the §2 story: withholding the memory did NOT fix the wedge

With the `/memory` shrink verified working — `memory: 03fe0000`, `Memory: …/65408K`,
`MemTotal` 59260 kB, i.e. the ramoops window provably outside the kernel's map — and both
pstore backends on, **the wedge came straight back**:

| check | result |
|---|---|
| power-cycle trial 1 | `t45=0 t70=0` **FAIL** |
| power-cycle trial 2 | `t45=0 t70=0` **FAIL** |
| direct ARP on the settled box | `replies seen: 0` / 20 |
| `ping 192.168.100.3` | 100 % loss |
| `br-lan` inet / `uci get network.lan.ipaddr` | `192.168.100.3` — correct, not confound #16 |
| port 6 `ifOutUcastPkts` | **5** (dead), while `ifOutOctets` 721940 climbs from flooding |
| RX | healthy, `rx_pkts` climbing throughout |

★ **This partially refutes §2.** §2's *mechanism* — "the allocator hands the ramoops region
to the rtl819x driver, so ramoops overwrites live descriptor/cluster memory" — was inferred
from a correlation (remove ramoops → 15/16 pass), never directly observed. Removing the
collision entirely, while leaving ramoops active at the same address, **does not fix the
wedge**. So the collision with *Linux-allocated* memory is not the whole cause, and may not
be the cause at all.

What still holds: **ramoops/pstore active ⇒ wedge; absent ⇒ 15/16 passing trials.** The
correlation is solid and reproducible. The mechanism is not established.

★★★ **The control that settles it.** The cell I had not tested was *shrunk `/memory` with
pstore OFF* — i.e. is the shrink itself harmful? It is not:

| `/memory` | ramoops | result |
|---|---|---|
| full `0x4000000` | on, unreserved | **FAIL** (the original bug) |
| full `0x4000000` | off | PASS 15/16 |
| shrunk `0x3fe0000` | on, `PSTORE_CONSOLE=y` | **FAIL** 2/2 |
| shrunk `0x3fe0000` | on, `PSTORE_CONSOLE=n` | **FAIL** 2/2 |
| shrunk `0x3fe0000` | **off** | **PASS 5/5** |

★ So the shrink is harmless, and **an active ramoops wedges CPU TX regardless of where its
memory lives or how little it writes.** With `PSTORE_CONSOLE=n` and `PSTORE_RAM=y`, ramoops
writes essentially nothing at runtime — it only dumps on oops/panic — and the box still
wedges. That is very hard to reconcile with any "ramoops overwrites memory" story, and
points instead at what ramoops does *at probe*.

Candidates not yet separated (in the order worth testing):

1. **`CONFIG_PSTORE_CONSOLE` specifically** — it mirrors *every* printk through
   `pstore_console_write()`, in any context including atomic/interrupt during driver
   bring-up. This platform already needed `patches-4.14/0008-fs-pstore-dont-block-in-atomic-context.patch`
   for a pstore-in-atomic-context hang, so pstore is known to interact badly with this
   SoC's early paths. `CONFIG_PSTORE_RAM` alone writes only on oops/panic — effectively
   never during bring-up — and still yields the crash dumps that are the actual goal.
2. **Something other than Linux owns the top of DRAM** (bootloader or ASIC buffer pool), so
   ramoops corrupts it regardless of the kernel's map. Searched and *not* supported for the
   ethernet driver: no hardcoded high physical address, no `ioremap` of a fixed DRAM
   address, no top-of-memory arithmetic in `rtl819x/`. Not yet checked in `rtl8192cd`.
   Testable cheaply by moving the window down (`/memory` → `0x3fc0000`, ramoops →
   `0x3fc0000`) and seeing whether TX survives.
3. **Kernel-image layout.** Enabling pstore changes code/init sizes and therefore where
   everything else lands, which on this box has repeatedly mattered more than it should.

### 9.7 Narrowing what about ramoops actually wedges TX

Two things were eliminated, each with 2/2 failing power-cycle trials:

| variant | rationale | result |
|---|---|---|
| `CONFIG_PSTORE_CONSOLE=n`, `PSTORE_RAM=y` | console mirroring is the only high-frequency writer; with it off ramoops writes essentially nothing until an oops | **FAIL 2/2** |
| + `unbuffered;` on the DT node | `ramoops_parse_dt()` → `mem_type=1` → `persistent_ram_iomap()` uses plain uncached `ioremap()` (CKSEG1) instead of the default write-combining `ioremap_wc()` | **FAIL 2/2** |

★ So it is neither the volume of writes nor the mapping type. An attached ramoops that
writes almost nothing, through a plain uncached mapping, into memory the kernel does not
manage, still kills CPU TX.

#### ★ The descriptor rings land at the very top of DRAM

Worth knowing independently of this bug. The driver prints its bring-up state, and
`CPURPDCR0` is the RX pkthdr ring's physical address:

```
grep -a 'rtl819x bringup:' dir842-uart.log
```

- **There are two NIC bring-ups per boot** — an early one at t≈10.7 s (`CPUIISR=80000000`,
  `GDSR0=00120012`) and a later one at t≈31-53 s (`CPUIISR=00000000`, `GDSR0≈0088-009b`).
- The **early** ring is consistently allocated at the very top of DRAM. On full-64 MB
  builds: `CPURPDCR0` = `03fa2000`, `03f7e000`, `03fce000`, `03fae000`, `03fab000`, … and
  ★ **`03fe2000` — inside the old ramoops window `0x3fe0000-0x3ffffff`.**
- With `/memory` shrunk to `0x3fe0000` the early ring moves down to ≈`0x03b7xxxx`, i.e. the
  shrink does relocate it as intended.

That is direct evidence that the original (unreserved) carve-out really did overlap live
DMA ring memory — §2's mechanism was **real**, just not the whole story, since removing the
overlap does not remove the wedge.

### 9.8 Address is irrelevant too — and where this leaves the bug

The remaining spatial hypothesis was that something outside Linux (bootloader or ASIC
buffer pool) owns the top of DRAM, so ramoops corrupts it wherever the kernel's map ends.
Tested by moving the window off the top entirely — a 128 KB **hole at 48 MB**, with
`/memory` given two ranges:

```dts
	reg = <0x0 0x3000000 0x3020000 0xfe0000>;	/* hole at 0x3000000 */
	…
	ramoops@3000000 { reg = <0x3000000 0x20000>; … };
```

**FAIL, 2/2.** Address is not the variable either.

#### The full matrix

| `/memory` | ramoops | trials |
|---|---|---|
| full `0x4000000` | off | **PASS 15/16** |
| shrunk `0x3fe0000` | off | **PASS 5/5** |
| full `0x4000000` | on, unreserved | FAIL (the original bug) |
| shrunk `0x3fe0000` | on, `PSTORE_CONSOLE=y` | FAIL 2/2 |
| shrunk `0x3fe0000` | on, `PSTORE_CONSOLE=n` | FAIL 2/2 |
| shrunk `0x3fe0000` | on, `+ unbuffered` | FAIL 2/2 |
| hole at `0x3000000` | on, `PSTORE_CONSOLE=n` | FAIL 2/2 |

★ **`ramoops` attached ⇒ wedge. Absent ⇒ works. Nothing else moves the outcome.** Not the
memory's ownership, not its address, not the mapping type, not how much is written.

#### What that leaves

`CONFIG_PSTORE_RAM=y` with `PSTORE_CONSOLE=n` writes essentially nothing until an oops. If
it still wedges TX, the damage is not plausibly ramoops' *writes*. What remains:

1. ★ **Kernel image size / boot timing.** Enabling pstore moves kernel code 4160K→4175K and
   init 1172K→1220K, shifting every subsequent allocation and the whole boot timeline. This
   box has repeatedly proven far more sensitive to boot-time layout and timing than it
   should be, and **the ramoops-free build still failed 1 trial in 16** — so the wedge may
   be *marginal* rather than deterministic, with pstore merely pushing it over the line.
   ★ **This is the next experiment: add ~15 KB of UNRELATED kernel code (any harmless
   config symbol), with no ramoops at all, and see whether the wedge appears.** If it does,
   the target is not ramoops and never was — it is whatever makes this box marginal.
   ★ **Caveat added after the purge (§9.9): the "1 failure in 16" evidence for marginality
   is weak.** That trial sampled ARP only once, at t+45 s, and a healthy purged build has
   since been caught at `t45=0 t70=20` — a slow boot, not a wedge. The lone historical
   failure was most likely the same thing, which would make the ramoops-free config 16/16.

   ★★ **TESTED — AND STILL NOT SETTLED. See §9.10.** +31K of unrelated kernel code
   (inert ciphers, no pstore anywhere) failed 3 of 13 judged trials against 0 of 12 for the
   unmodified control. That is NOT separable (Fisher p ≈ 0.25) and the run was
   underpowered, so it neither confirms nor refutes marginality — and the direction of the
   difference mildly favours it. What it does show is that padding is nowhere near the
   *deterministic* failure ramoops produced, so size/layout alone is not a sufficient
   explanation. Retraction row 39 stands.
2. Probe-time side effects other than the ring write: `request_mem_region()` inserting into
   `iomem_resource`, `pstore_register()`'s 32 KB `kmalloc` for `psinfo->buf`, the extra
   platform device on the bus, the registered `kmsg_dumper`.

**Current state: crash logging stays off.** The tree ships full `/memory`, no ramoops node,
both pstore backends `n` — the configuration measured at 15/16, restored and re-verified
after these experiments.

### 9.9 Purged

pstore is now off at the **core**, not just the backends — `# CONFIG_PSTORE is not set`.
Verified in the built kernel:

```
fs/pstore object count:            0
pstore symbols in System.map:      0
ramoops symbols in System.map:     0
```

The `/memory` node is back to the full `0x0 0x4000000`, there is no `ramoops` or
`reserved-memory` node, and the initramfs shrank ~16 KB. **Verified 6/6** on hands-off
power-cycle autoboots, plus 20/20 ARP and 0 % ping loss on the settled box.

Two things deliberately kept rather than deleted:

- **`patches-4.14/0008-fs-pstore-dont-block-in-atomic-context.patch`** — inert, since
  `fs/pstore/` is no longer compiled, but retained with a header saying so. The bug it
  fixes is real, was found on this hardware, and presents as a dead box rather than a
  crash; anyone re-enabling pstore needs it and should not have to rediscover it.
- **The `skb_shared_info` guard in `rtl819x_swnic.c`** (commit `317f252`). Its comment
  records four crashes whose corruption was ASCII kernel-log text — the same signature as
  the `0x30303030` slab panic in §9.3 — which points hard at the pstore console ring as the
  writer. But that was never confirmed, so the guard stays.

★ **A note on those two, together.** The ASCII-text corruption (§9.3, and the `317f252`
forensics) is a *different failure* from the TX wedge, and only the first one behaves like
"ramoops scribbles on live memory": it needs `PSTORE_CONSOLE` and an actual overlap. The
wedge survives removing both. Treating them as one phenomenon is what produced the wrong
§2 explanation — do not merge them again.

### 9.10 ★ The marginality experiment — RUN TO COMPLETION

§9.8 asked the one question that would settle §9: does the wedge track **ramoops
specifically**, or is this box merely MARGINAL, with pstore only pushing it over the line?
Answer: **the marginality hypothesis is weakened. Padding does not reproduce the wedge.**

#### Design

Two arms, identical but for padding. Padding was `CONFIG_CRYPTO_SERPENT` +
`CONFIG_CRYPTO_TWOFISH` (+`_COMMON`) — inert ciphers nothing on this board selects or
invokes, deliberately chosen so the change touches neither the ethernet driver nor the DTS.
Both arms have **no pstore and no ramoops anywhere**.

| | control | padded |
|---|---|---|
| kernel code | 4131K | **4162K (+31K)** |
| rodata / rwdata | 892K / 157K | 896K / 161K |
| init | 1212K | 1172K |
| `vmlinux` text | 4495473 | 4528193 (**+32720 B**) |

Padding confirmed live (`grep -c serpent /proc/crypto` = 2). The perturbation is comparable
to what pstore added (+16K code, +48K init).

#### Result

Judged on the **t+70 s** burst only (rule 7; confound #20 — a healthy box has been seen at
`t45=0 t70=20`). Trials where the harness aborted early are excluded from pass/fail.

| arm | judged trials | FAIL | rate |
|---|---|---|---|
| A — control | **12** | **0** | 0% |
| B — padded | **13** | **3** | 23% |

★ **Not statistically separable.** Fisher exact on 0/12 vs 3/13 gives two-tailed p ≈ 0.25.
Three failures out of thirteen against zero out of twelve is the kind of split that arises
by chance at this sample size, and it must not be read as an effect.

★★ **And the comparison that actually matters:** with ramoops present the wedge was
**deterministic — it failed essentially every hands-off boot**, which is what made it
findable at all. Padding produces at most an intermittent 23%, indistinguishable from
noise. A +31K code shift is therefore **not** equivalent to attaching ramoops.

#### Conclusion — ★ read this carefully, it is weaker than it first looks

- ★ **The experiment is UNDERPOWERED AND INCONCLUSIVE. It did not settle marginality.**
  Resist the tidier summary. Control 0/12 vs padded 3/13 is not significant (p ≈ 0.25) —
  but note the *direction*: the padded arm failed and the control never did. If anything
  that leans mildly TOWARD marginality, not away from it. "Padding did not reproduce the
  wedge, therefore the box is not marginal" does **not** follow from this data.
- **What the data does support, and only this:** +31K of unrelated code is not equivalent
  to attaching ramoops. Ramoops failed essentially *every* hands-off boot; padding failed 3
  of 13. Those are different regimes, and that gap survives the small sample. So kernel
  size/layout alone is not a sufficient explanation for what ramoops did.
- **Retraction row 39 stands as written.** "ramoops attached ⇒ wedge" remains the solid
  correlation. The mechanism is still unknown — this experiment eliminates one candidate
  explanation and supplies none.
- ★ **The 3 Arm-B failures are the loose end.** At n=13 they carry no statistical weight,
  but they are the only signal here pointing at marginality and they went unexplained.
  Settling this properly needs a far larger sample than a hands-off power-cycle rig
  produces in an evening (~3 min/trial); at 23% vs 0%, separating them at p<0.05 needs
  roughly 40+ trials per arm.

#### ★ A confound this experiment created, and then caught

An unguarded Arm-B run coincided with a **house-wide LAN outage**, and the working
hypothesis became "a wedged box floods the LAN" — which would have neatly explained the
historical outages in issue #2's preamble. A kill-switch harness was built to bound the
damage (cut power on sustained high rx), and it duly "fired" on 10512-17777 pkt/s several
times, including **on the control arm**.

★ **All of it was false.** The harness measured *total* rx on the bench host's NIC, which
includes that host's own downloads and all other house traffic. An attributed capture over
a full 100 s boot cycle settles it:

| source MAC | frames in 100 s |
|---|---|
| `50:4f:3b:32:68:9c` (house gateway) | 69346 |
| `00:e0:4c:a7:00:6c` (**the bench host itself**) | 40193 |
| `e0:1c:fc:51:c9:ef` (box `br-lan`/`eth0.2`) | **12** |
| `00:e0:4c:81:86:86` (box `wlan0`) | **0** |
| `e0:1c:fc:51:c9:f0` (box `wlan1`) | **0** |

**The box emits 12 frames in 100 seconds across a full boot. It does not flood.** The
"floods" were the bench host's own traffic. Two hypotheses built on that reading — "a
wedged box floods the LAN" and the `br-lan` STP-loop theory — have **no supporting evidence
and are withdrawn**. The cause of the observed house outage remains unknown and is NOT
attributable to this box on the evidence collected.

★ Rule for anyone measuring "is the box flooding": count frames **by source MAC**, never
interface totals. An interface counter on a shared LAN measures the whole LAN.
