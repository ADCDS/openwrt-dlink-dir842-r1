# Why `port/main-6.18` wedges — root causes and the fix plan

**2026-09-24. Static analysis, vendor-source comparison and compile tests only — nothing in
this document has been run on hardware yet.** Every claim below is tagged:

- **[code]**: read in this tree or in the exact kernel it builds against (Linux 6.18.44 plus
  every OpenWrt generic patch and this repo's `patches-6.18/`, reconstructed and applied
  cleanly for this investigation).
- **[vendor]**: read in Realtek's own RTL8197F sources, i.e. three SDK generations and the
  8197F bootloader (see *Sources*).
- **[doc]**: bench evidence already recorded in `PORT-MAIN-6.18-STATUS.md` and the older
  docs.
- **[inference]**: my reasoning, not yet measured.

The patches that go with this document are in the same commit series. Every change to
datapath behaviour has a runtime knob, so each one can be A/B'd on one boot without
reflashing (see *What changed*).

## 1. The short answer

"The box wedges" on this branch is three failures that feed each other. It is not one
bug.

1. **The load wedge.** After sustained traffic through the CPU, frames larger than about
   128 bytes stop reaching the CPU and small ones get lossy. Only a full switch-core reset
   clears it. Two driver defects are the leading suspects:
   - **The DMA configuration [code, vendor].** From its first transmitted frame onward, the
     CPU-port DMA engine runs in a configuration no Realtek software uses on this chip. The
     TX doorbell clears the burst-size bit on every transmit, which gives:
     - **32-word (128-byte) bus bursts.**
     - **An inverted FIFO watermark pair.** The high mark is reset to 0x57, below the 0xA0
       low mark.

     **128 bytes is exactly the "large-frame" knee every wedge document measures.**
   - **RX ring pairing [code, vendor].** The RX path assumes the pkthdr and mbuf rings never
     drift apart. Every vendor driver assumes they can.

   That these defects cause the wedge is **[inference]**; the bench plan in §11 is what
   decides it. The 4.14 driver on `main` has the same doorbell line and saw the same wedge,
   but less often, plausibly because hardware NAT kept forwarded traffic off the CPU.
2. **The hard hang** (33 minutes of UART silence, power cycle only). The level-3 recovery
   runs the vendor's `FullAndSemiReset()` (switch-core reset plus clock gate). It does so
   with **interrupts on**, using `msleep()`, for about 650 ms. The vendor only ever runs
   that sequence atomically. The recorded hang went quiet inside exactly that window.
   **Nothing ever arms a hardware watchdog**, so the hang was permanent.
3. **The recovery path itself damages the box.** Two defects:
   - Every recovery **leaked every in-flight TX skb**.
   - It **freed and re-allocated all 256 RX clusters with GFP_ATOMIC**, and on failure
     restarted the DMA engine on freed rings.

Two further defects are independent of the above:

- **Reading `/proc/wlan0/*` crashes the box.** The vendor wifi driver hands
  `struct file_operations` to a kernel that expects `struct proc_ops`.
- **The ISR's interrupt masking does nothing under 6.18's chained interrupt controller**, a
  latent interrupt-storm livelock.

Separately, **every frame the CPU transmits is flooded to all five jacks, WAN
included** (`dsa_tx_flood=1`). That is a LAN→WAN leak, not a wedge cause, but it explains
the "packet to 192.168.0.2 on the WAN wire" anomaly in `PORT-MAIN-6.18-STATUS.md` §4.

The M5 hardware-NAT stall (one TCP segment and every retransmission of it lost) is very
likely the same load wedge. The large frames of the "offloaded" flow are still trapped to
the CPU. The detector for the wedge's starvation form is disabled whenever any hwnat flow
is installed (§7).

## 2. The load wedge: the DMA engine runs a configuration nobody else uses

### 2.1 The doorbell changes the burst size on the first transmit

`rtl865x_start()` programs the CPU interface **[code]**:

```c
REG32(CPUICR) = TXCMD | RXCMD | BUSBURST_128WORDS | MBUF_2048BYTES;  /* 0xE4000000 */
REG32(DMA_CR0) = ... | (0xA0 << LowFifoMark_OFFSET) | 0xCE;          /* stock's 0xA0CE */
```

Until this commit, every transmit rang the doorbell like this **[code]**
(`rtl819x_swnic.c`, `_New_swNic_send`):

```c
REG32(CPUICR) = REG32(CPUICR) & ~(1u << 29);   /* "8197F: clear bit29 first, per vendor" */
REG32(CPUICR) |= TXFD;
```

- **Bits 29:28 of CPUICR are the bus burst size** (`rtl819x_regs.h`:
  `BUSBURST_128WORDS = 2 << 28`, `BUSBURST_32WORDS = 0`) **[code, vendor]**. The first frame
  the box ever sends turns 128-word bursts into 32-word bursts, i.e. 128 bytes. That value
  sticks, because every later clear finds the bit already clear.
- **The vendor documents a side effect** of writing that field: "the HiFifoMark value will
  be reset to default value (0x57) after updated the burst size field of CPUICR"
  (`rtl865x_asicCom.c`, comment next to its own CPUICR write) **[vendor]**. The same first
  transmit therefore drops the high FIFO mark to 0x57, below the 0xA0 low mark this driver
  set. That is an inverted watermark pair.
- **The fix recorded as A-2 never took effect.** "DMA_CR0 = 0xA0CE", documented as the fix
  for the "descriptor overtakes payload" drain-lag race, only ever existed between
  `rtl865x_start()` and the first TX. The bring-up `pr_err` that "verified" it is printed
  before any transmit **[code]**. The retraction table's claim that burst size is
  "FALSIFIED" (`RETRACTIONS-AND-METHOD.md`) compared only that init value.
- **No vendor 8197F build ever executes this line.** It exists in an old SDK's *legacy*
  `swNic_send()` under `CONFIG_RTL_8197F`. But every 8197F build defines
  `CONFIG_RTL_SWITCH_NEW_DESCRIPTOR` (`rtl_types.h`), which routes transmit to
  `New_swNic_send()`. In all three SDK generations that function only sets `TXFD`. The
  newest SDK also `#if 0`s the legacy line **[vendor]**. Stock runs the new-descriptor path:
  it ORs `0x140` into `CPUICR1`, i.e. `TX_PKTHDR_SHORTCUT_LSO | CF_TX_GATHER`, as recorded in
  the register-snapshot comment in `rtl819x-eth.c`. So the stock runtime state is 128-word
  bursts with 0xA0CE.
- **Every wedge document measures a 128-byte knee:**
  - `M7-LARGE-FRAME-RX-WEDGE.md`: "hard knee at a ~128-byte frame"; 122 B partial, 130 B
    100% loss.
  - The same doc on 4.14: wedged large frames arrive with a correct length and a stale
    payload (`rx_dump`: "`ph_len==m_len, m_next=0, cached==uncached both stale`"). The frame
    was not in DRAM when the descriptor said it was, or it was in a different cluster (§2.2).

  With 128-byte bursts, 128 bytes is where a frame first needs a second burst. The
  "descriptor overtakes the multi-burst payload" race that doc describes is what you would
  expect from a FIFO whose drain watermarks are inverted **[inference]**. The docs'
  previous explanation (switch buffer cells of about 128 B) was also inference.

**Fix (default on):** the doorbell no longer touches the burst field. The
`tx_kick_clear_burst=1` knob restores the old behaviour. `txdiag` now prints the live
CPUICR burst field and DMA_CR0.

**Cheapest confirmation (current image, read-only, one minute):** after boot and a ping,
run `echo 1 > /sys/module/rtl819x/parameters/txdiag; dmesg | tail -3`.
- The current image should show `CPUICR=c4……`, i.e. bit 29 clear.
- The new image should show `CPUICR=e4…… (burst=2) DMA_CR0=……a0ce`.

### 2.2 The RX path assumes the two rings never drift apart

- The engine walks the **pkthdr ring and the mbuf ring with two independent pointers**. It
  writes into each pkthdr the address of the mbuf it actually filled (`ph->ph_mbuf`).
- **Every Realtek pkthdr/mbuf implementation locates the cluster through `ph_mbuf`** and
  re-arms *that* mbuf **[vendor]**:
  - `rtl865xc_swNic.c` `increase_rx_idx_release_pkthdr()` / `swNic_receive()`;
  - the 8197F bootloader's `swNic_poll.c`, with the comment "for rx descriptor runout";
  - hackpascal's RE865X driver.
- **This driver paired pkthdr[i] with mbuf[i] and `rx_ri[i]`** **[code]**. If the rings
  ever drift, for example on a runout under CPU saturation (the load-wedge condition), that
  pairing:
  - hands up the **wrong cluster under a correct `ph_len`**. That is exactly the 4.14
    stale-payload signature, and `rx_dump` could not rule it out because it printed
    `RXMB(idx)`, not `ph_mbuf`;
  - leaves the mbuf the engine really used CPU-owned until a later frame's re-arm happens
    to reach it, so the engine runs short by as many mbufs as the rings have drifted;
  - re-points an mbuf the engine may still own at a new cluster while the old one is
    already in the stack **[inference]**.
- The same area has a related aliasing hazard **[code, vendor]**:
  - This driver points rings 1-5 (`CPURPDCR1-5`) at ring 0's array.
  - The vendor gives them their own rings and zeroes the queue→ring map (`CPUQDM0-5`), which
    this driver inherits from the loader.
  - An independent 6.18 driver for the same SoC saw frames land on non-zero rings and
    corrupt memory.

  Left as a documented follow-up, not changed.

**Fix (default on):** the cluster is located through `ph_mbuf`, with a bounds check.
`ph_len` is cleared on re-arm.

**Instrument:** two read-only counters,
`/sys/module/rtl819x/parameters/rx_mbuf_desync` and `rx_mbuf_bad`.
- Any non-zero `rx_mbuf_desync` under load **proves the desync was happening** and is now
  handled.
- `rx_follow_ph_mbuf=0` restores the old pairing.

### 2.3 Flow-control thresholds from a function the 8197F never calls (knob, default off)

- **Our PBFCR values come from `rtl8651_clearRegister()`.** `rtl865x_start()` writes
  per-port FCON 90 / FCOFF 60 on ports 0-5, copied from that function. It is defined but
  never called in all three 8197F SDK generations **[vendor]**.
- **What the 8197F SDKs actually program** in `rtl865x_initAsicL2()` is FCON 0x1AC / FCOFF
  0x1A6 on **PBFCR0-6, CPU port included**. This driver never writes PBFCR6 at all
  **[vendor, code]**.
- **The vendor also runs the trunk with pause on** (8367 host port forced TX+RX pause). This
  port forces it off, because pause at 90-descriptor thresholds collapsed throughput
  **[doc]**. Pause-on was plausibly unusable here only because the thresholds were about 5×
  too low **[inference]**.
- **Test** (the knob is new): `echo 1 > …/pbfcr_vendor; echo 1 > …/fabric_reset`. Measure,
  then re-test `trunk_pause=1` on top.

### 2.4 Why it looks like a 6.18 regression

The swNic engine is essentially unchanged from 4.14 (a diff shows only the runout acks and
diagnostics). **4.14 had the same doorbell line.** Two things changed on 6.18:

- **All forwarded bytes now cross the CPU.** On 6.18, hardware NAT does not carry
  forwarded traffic, so every forwarded byte crosses the CPU RX ring. On 4.14, 0.0% did.
  That turns "~1 in 3-4 heavy floods" into near-certainty per transfer **[doc]**.
- **The detectors and auto-recovery now fire far more often** (see §3-§4), so the recovery
  bugs matter far more.

## 3. The hard hang: the level-3 reset ran with interrupts on

**The recorded hang [doc]:**
- `PORT-MAIN-6.18-STATUS.md` §4 records the sequence `LARGE-FRAME WEDGE detected →
  recovery level 3 starting → "switch wan: Link is Down"`, then 33 minutes of silence.
- A successful level 3 prints `L4/NAPT table SRAM cleared` about 0.76 s after "starting";
  this record stops before that line.
- The console is the synchronous legacy 8250 console, and `PANIC_ON_OOPS=y` with
  `PANIC_TIMEOUT=1` turns any oops into a 1-second reboot. Silence therefore means a bus
  stall or a livelock, not a crash.

**What the driver did [code]:** `rtl819x_fabric_full_reset()` ran `SIRR |= FULL_RST;
msleep(300); clock-gate; msleep(300); ungate; msleep(50)`. That leaves about 650 ms in
which the switch core is in reset and then unclocked, while every other context keeps
running:
- phylink/SMI (its "Link is Down" print proves other contexts ran mid-window);
- the WMAC and rtw88 interrupt paths;
- netifd hotplug;
- SPI-NOR page-ins.

The driver's own comment says a stray access in that window "stalls the Lexra bus". The
DTS says the same of dead regions on this fabric, "no bus-error exception".

The window is not only a wedge-recovery path. In router role, `dir842-asic` (S97) writes
3 to `fabric_reset` on **every boot**, while netifd, wifi bring-up (`asic-wifi-settle`,
S99) and dropbear are all starting **[code]**. So every boot runs the unfenced window once.

**What the vendor does [vendor]:** only ever runs the sequence atomically.
`rtl865x_reinitSwitchCore()` masks GIMR and busy-waits. `re865x_reProbe()` then calls
`FullAndSemiReset()` under `SMP_LOCK_ETH`, which is `local_irq_save` on UP, and the reset
uses `mdelay()`.

**Fence audit:** the driver's own automatic paths are all fenced during the window. The ISR
line is masked, NAPI and the timer are stopped, TX is disabled, and every /proc handler and
hwnat path takes `rtl865x_hal_lock`. Only the manual `txdiag`/`trunk_redo` knobs are
unlocked. So the stalled access most likely came from outside this driver, and **only an
atomic window closes that class of problem** **[inference]**.

**Fix (default on):** `fabric_reset_atomic=1` runs the reset and clock gate with
`local_irq_save()` and `mdelay()`, exactly the vendor sequence. Set `fabric_reset_atomic=0`
to get the old `msleep()` variant.

**Why it lasted 33 minutes [code]:** nothing arms the RTL8197F watchdog. `WDTCNR` is only
written by `rtl819x_machine_restart()`, to fire it. There is no `WATCHDOG_CORE` and no
lockup detector. See §8 for the watchdog proposal.

## 4. The recovery path damages the box it is recovering

All **[code]**. Fixed in this series, default on, no knobs, since these are plain bug fixes:

- **TX skb leak.** `New_swNic_init()` zeroed `tx_ri[]` without freeing, and the recovery
  path calls it with the engine stopped and frames still in flight. The result:
  - every recovery leaked those skbs;
  - in the measured dead-transmit signature the whole ring is outstanding, so one level-1
    TX-STALL recovery leaks up to 255 skbs;
  - a forwarded skb is an RX page-frag cluster, so each leaked skb pins its frag page.

  This fits the "cumulative degradation" `PORT-MAIN-6.18-STATUS.md` §4 kept measuring. The
  pre-`61c7cfc` false-positive detector fired a recovery every 15-30 s.
- **RX re-allocation on a live ring.** Every re-init freed all 256 RX clusters and
  allocated 256 new ones with GFP_ATOMIC, on a 64 MB box that runs near its watermark
  (`vm.min_free_kbytes=2048`, both radios, first-flash jffs2). On failure, `err_out` freed
  the coherent rings. `rtl819x_hang_work()` ignored the return value and restarted the
  engine, NAPI and xmit on them. The result would be DMA through ring-base registers
  pointing at freed memory, and `COH_V(NULL)` (0x20000000) dereferences.
  - Re-init now **re-arms the clusters the ring already holds** and allocates nothing.
  - The error path is kept and checked. On failure it reboots cleanly rather than DMA into
    freed pages. It should now be unreachable.
- **No settle after stopping the engine.** Clusters and TX buffers were freed immediately
  after `CPUICR=0`. A 200 µs settle has been added in `ndo_stop` and in the recovery path.
- **Loader-armed DMA on RAM boots.** A TFTP/`J` RAM boot hands over with the loader's NIC
  still running, and it keeps writing received frames into the loader's rings, which is
  memory Linux already owns, until `ndo_open`. `board-rtl819x.c` now stops it
  (`CPUIIMR=CPUICR=0`) at early boot, but only when the loader left the switch core
  clocked. An autoboot, with the clock off, skips it, so the block is never touched the
  instant its clock comes on.

## 5. Interrupt masking that does not mask (latent livelock)

**[code]**
- **The old scheme.** `rtl819x_eth_isr()` cleared CP0 Status IM4, and NAPI re-set it along
  with GIMR bit 15. The ISR comment explains why: a switch assertion that CPUIIMR does not
  gate ("CPUIISR can even read 0") would otherwise re-fire forever.
- **Why it no longer works.** On 6.18 the NIC is a child of the chained Realtek intc:
  - `realtek_irq_dispatch()` ends in `chained_irq_exit()` → `irq_eoi` = `unmask_mips_irq()`
    on the IP4 parent. That sets IM4 again the moment the ISR returns.
  - `handle_level_irq()` unmasks GIMR bit 15 after the handler.

  The masking was written for 4.14, where the NIC was a bare CPU line. On 6.18 only the
  CPUIIMR write gates anything.

**Fix (default on):** `disable_irq_nosync()` in the ISR and a balanced `enable_irq()` at
NAPI completion, tracked by a flag. `irq_legacy_mask=1` restores the old scheme.

## 6. `/proc/wlan0/*` reads: the wrong struct type

**[code, confirmed by a GCC 12 cross-build]**
- **The bug.** Since Linux 5.6, `proc_create_data()` takes a `struct proc_ops`. The vendor
  macros `RTK_DECLARE_*_PROC_FOPS` (`8192cd_proc.c`) and `wlan_custom_Passthru_proc_fops`
  (`8192cd_osdep.c`) declare `struct file_operations`. This builds only because the
  Makefile demotes `-Wincompatible-pointer-types`; GCC does warn "passing argument 4 of
  'proc_create_data' from incompatible pointer type".
- **What a read does.** At `proc_ops` offsets:
  - `proc_open` gets `fop_flags` (NULL), and `proc_read_iter` gets `.read` (`seq_read`).
  - So every read calls `seq_read()` with a `struct kiocb *` as its `struct file *`, with no
    `single_open()` ever having run.
  - That is the documented "reading `/proc/wlan0/{mib_all,sta_info}` wedges the box".
  - Writes happened to line up (`proc_write` ← `.write`), which is why write-style controls
    worked.
- **The recorded symptom favours this mechanism [inference].** Those reads ended in a
  reset, not a permanent hang like §3's. With no watchdog armed, the panic path is the only
  thing here that reboots the box (`PANIC_ON_OOPS=y`, `PANIC_TIMEOUT=1`, then
  `rtl819x_machine_restart()`). An oops in the mis-typed `seq_read()` takes that path. The
  status doc's "long `PRINT_ARRAY` loop under `SAVE_INT_AND_CLI()`" theory would hang
  forever instead.
- **The shipped workaround targets the wrong code.** The `PRINT_ARRAY` cap added on
  2026-09-05 can't help, because the show handler never runs.

**Fix:** the macros and the passthru fops now declare `struct proc_ops` (`.proc_open`,
`.proc_read_iter = seq_read_iter`, `.proc_lseek`, `.proc_release`, `.proc_write`) on ≥ 5.6.
With GCC 12, the whole module now builds with zero `proc_create_data` warnings.
- **Verify:** `head -c 200 /proc/wlan0/mib_all` should print, not crash.
- **Then:** `dir842-l2flush` can be re-evaluated.

## 7. The M5 "one segment lost forever" stall is probably the load wedge

This is a hypothesis to test first once §2 is in, not a conclusion.

- **The flow's large frames still hit the CPU [doc].** With a hwnat row installed, bulk
  LAN→WAN frames still reached the CPU (`hwFwd=0`, reason `0x4e0e`/`0x420a`,
  `PORT-MAIN-6.18-STATUS.md` §4). The peer saw 515 packets in about 40 ms. The status doc
  reads that burst as "far faster than the software-forwarding path". But 515 packets in
  40 ms is 12.9 kpps, about 149 Mbit/s at 1448-byte segments. That is the
  software-forwarding ceiling the same doc measures (about 150 Mbit/s, about 13 kpps), not
  line rate. So the "offloaded" flow's large frames crossed the CPU RX path, which is the
  load-wedge trigger.
- **The retransmissions fit the wedge [inference].** Once the large-frame path is wedged,
  every retransmission, which is always a full-size frame, is lost, while small frames
  (ACKs) pass.
- **The detector for this state is disabled while any hwnat flow is installed [code].**
  When large frames stop arriving altogether, the FCS detector has nothing to sample. The
  LARGE-FRAME starvation counter is gated by `!rtl819x_hwnat_any_flow_installed()`. So
  nothing schedules a recovery, and the stall is permanent. On 4.14 these frames were
  forwarded in silicon, so an offloaded flow never crossed the CPU RX path in the first
  place.
- **Tests, with `flow_offloading_hw=1 hwnat=1`, during a stall:**
  - From the client, `ping -s 1400 192.168.0.1` against `ping -s 56 192.168.0.1`.
  - Then `echo 2 > /sys/module/rtl819x/parameters/fabric_reset`, which keeps the NAPT
    rows. If the transfer resumes, M5's stall is this wedge.
- **What stays open.** The remaining M5 question is why the ASIC traps large LAN→WAN frames
  of a flow whose rows are installed. Decode `pid_dump` reasons with no SSH traffic
  running. Candidates to check: VLAN classification of the trapped frames (`vid=1` from a
  LAN port, in the recorded samples), and the netif MTU.

## 8. Proposed, not implemented: a hardware watchdog

The register facts come from the vendor `rtl865xc_asicregs.h` and the bootloader
**[vendor]**:
- **CDBR** at `0xB8003118`: DIVF [31:16]. The vendor writes `400 << 16`.
- **WDTCNR** at `0xB800311C`:
  - [31:24]: `0xA5` stops it; any other value runs it.
  - Bit 23 kicks it.
  - OVSEL [22:21], plus [18:17] on this chip.
  - Bit 20 is a sticky "a watchdog reset happened" flag. The next boot can print it, which
    turns "was it a hang?" into a fact.

The existing `realtek_otto_wdt` driver has a different layout, so it cannot be reused.

**Plan:** a ~60-line watchdog-core driver plus a DT node and `CONFIG_WATCHDOG_CORE=y`.
procd then pings it automatically. **Not shipped here**, because the overflow period
depends on a clock that needs one bench measurement: enable with pings stopped and time the
reset. Getting it wrong risks a boot loop. Until then, any hang is permanent.

## 9. The CPU TX flood (security, not a wedge)

**[code, doc]**
- **What happens.** `dsa_tx_flood=1` (the default) sends every CPU-originated frame with
  port list 0x3F and VLAN 0. The VLAN 0 member mask is 0x13F, and the 8367S does no VLAN
  filtering. So SSH replies, DHCP offers, DNS answers and software-forwarded WAN→LAN
  traffic also leave by the WAN jack, and WAN-side frames leave by every LAN jack.
- **What 4.14 did.** Its VLAN 1/2 member masks restricted egress to the correct side.
- **It is one frame on the trunk, not five copies.** eth0 `tx_packets` +65 matched
  8367S p06 `ifInUcastPkts` +65 **[doc]**. So this is a confidentiality problem, not a
  throughput one.
- **Why flooding became the default.** The switch to flooding (commit `973f647`) rested on
  cold-boot tests taken before the dead-transmit-boot fix (`b46eaf1`) and patch 703.
  `4ad815d` had measured single-port transmit working at the same throughput.
- **Proposal:** re-test `dsa_tx_flood=0` on ≥5 cold boots. Not changed here.

## 10. What changed in this series, and how to undo each part at runtime

All module parameters live under `/sys/module/rtl819x/parameters/`.

| change | default | revert / A-B |
|---|---|---|
| TX doorbell leaves the burst field alone | on | `tx_kick_clear_burst=1` (the old behaviour takes effect at once; going back to 0 also needs `echo 1 > fabric_reset`) |
| RX follows `ph->ph_mbuf`; `ph_len` cleared on re-arm | on | `rx_follow_ph_mbuf=0`; counters `rx_mbuf_desync`, `rx_mbuf_bad` |
| Level-3 reset atomic (irqs off, `mdelay`) | on | `fabric_reset_atomic=0` |
| ISR masks the line with `disable_irq_nosync` | on | `irq_legacy_mask=1` |
| Vendor PBFCR0-6 thresholds | **off** | `pbfcr_vendor=1`, then `echo 1 > fabric_reset` |
| TX skbs freed on re-init; RX clusters re-armed, not re-allocated; re-init failure checked; 200 µs DMA settle | always | — (bug fixes) |
| Loader DMA stopped at early boot | when the loader left the switch clocked | — (skipped on autoboot) |
| rtl8192cd proc fops → `struct proc_ops` | always | — (type fix) |
| `txdiag` prints live CPUICR burst / DMA_CR0 and the desync counters; its "ring freed" check fixed | always | — |

Compile-tested only. The build used GCC 12.4 `mipsel-linux-gnu` against Linux 6.18.44,
with all OpenWrt generic patches and this repo's target patches applied, and the target's
`rtl8197f/config-6.18`. Results:
- `vmlinux` links with zero warnings.
- The `rtl819x` driver also builds warning-free with `W=1`.
- `rtl8192cd.ko` builds through modpost with no `proc_create_data` warnings left. Its other
  vendor warnings are pre-existing and unchanged.

**None of it has run on a DIR-842 yet.**

## 11. Bench plan, in order

Follow the bench rules: one measurement per cold boot, a loopback-verified sink before
every run, and `tcpdump -s 96` only.

1. **Before flashing: confirm the premise (current image, read-only).** After boot and a
   few pings, `echo 1 > /sys/module/rtl819x/parameters/txdiag`. `CPUICR=c4……` confirms §2.1
   on the running box.
2. **Flash the new image.** `txdiag` should now show `burst=2` and `DMA_CR0=……a0ce`, both
   after traffic. `rx_mbuf_desync` and `rx_mbuf_bad` should read 0 at idle.
3. **Load-wedge A/B.** Use the recipe that wedges today: an unpaced `iperf3` LAN→WAN (no
   `-b`), `flow_offloading_hw=0`. Per run, record:
   - the number of `LARGE-FRAME WEDGE`, `RX-STALL` and `recovery level` lines;
   - throughput;
   - `ping -s 1400` / `ping -s 56` afterwards;
   - the `rx_mbuf_desync` delta.

   Run five runs with the defaults, then five with `tx_kick_clear_burst=1` plus
   `rx_follow_ph_mbuf=0` (the old behaviour). If the wedge disappears or becomes rare,
   bisect the two knobs.
4. **Recovery safety.** Loop `echo 3 > …/fabric_reset` every 8 s under traffic while
   toggling the WAN peer's link and running `echo 3 > /proc/sys/vm/drop_caches`. Pass
   criteria:
   - no hang across 50+ recoveries;
   - `free` stays flat across the loop (the leak fix);
   - the box stays reachable.
5. **`/proc/wlan0`.** `head -c 200 /proc/wlan0/mib_all` and `head /proc/wlan0/sta_info`
   should print.
6. **Only after 3-5:**
   - M5 (§7 tests);
   - `pbfcr_vendor=1` and then `trunk_pause=1`;
   - `dsa_tx_flood=0` on cold boots;
   - the watchdog calibration (§8).

If step 3 shows no change, the wedge is not the burst/FIFO misconfiguration. The next
lead is the legacy descriptor mode itself: every vendor 8197F build uses the
new-descriptor engine (`CPUICR1` bit 8). There is a GPL-2.0 Linux 6.18 driver in that mode
for this exact SoC plus an RTL8367RB (Putpocket's ipTIME A2004MU port), hardware-tested
with hardware NAT and a one-hour soak, to crib from.

## 12. Checked and ruled out (one line each)

- **Device DMA treated as coherent:** no. `dma_default_coherent` is false and there is no
  `dma-coherent` property, so cache maintenance runs.
- **RX cluster layout:** fine. There are 2048 DMA bytes after the 64-byte `NET_SKB_PAD`,
  32-byte aligned at both ends, and `skb_shared_info` sits on its own cache lines.
- **`pref`/PrepareForStore touching DMA buffers:** no. It only appears in `clear_page` and
  `copy_page`.
- **Cache aliasing on the datapath:** no. DMA buffers are touched only through KSEG0/KSEG1.
- **pstore/ramoops text writer (the 4.14 shinfo forensics):** not present. `CONFIG_PSTORE`
  is unset on 6.18.
- **CP0 Status lost updates:** none. Every IM writer is net-zero.
- **realtek-smi with interrupts off:** about 0.2 ms per access, far too short to matter.
- **"Five copies per flooded frame on the trunk":** no; the counters match 1:1 (§9).

**Not explained by anything here:**
- **The corrupted-TX boot after the 33-minute hang** (`PORT-MAIN-6.18-STATUS.md` §4). The
  router's own SSH replies left with one packet's header and another's length, on one boot
  only. Nothing above explains a TX fault confined to one boot. If it recurs, take a
  peer-side capture and the new `txdiag` output (burst, DMA_CR0, ring state) before
  anything else.
- **Why the ASIC traps large frames of an installed hwnat flow** (§7).

## Sources

- **Vendor RTL8197F SDKs:**
  - 8devices SDK v3.4.11e:
    <https://github.com/8devices/openwrt-8devices> (`target/linux/rtkmipsel/files/drivers/net/rtl819x`)
  - 7felix7 3.10 SDK: <https://github.com/7felix7/openwrt_rtk8197f>
  - tobyw121 RTL8197F+RTL8367RB SDK: <https://github.com/tobyw121/Openwrt_RTL>
- **8197F bootloader:** <https://github.com/7felix7/bootcode_rtl8197f>
- **Independent Linux 6.18 RTL8197F+RTL8367RB port (GPL-2.0):**
  <https://github.com/Putpocket/iptime-a2004mu-openwrt>
- **Clean-room legacy-mode driver:** hackpascal `lede-rtl8196c`, branch `realtek`.
