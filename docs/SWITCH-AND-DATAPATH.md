# The switch model and the wired datapath: CPU-tag mode, the RGMII trunk, VLANs and boot-medium endianness

How the DIR-842 R1's two cascaded switches are actually driven by the shipped firmware:
the CPU-tag / port0-router model that replaced the original VID-cascade ("Fork A"), the
RGMII trunk bring-up and the two ways it was broken, the VLAN/MAC/L2 plumbing, and the
`CPUICR1` byte-order bug that made a NOR boot behave differently from a TFTP boot.

**This doc absorbs and replaces `docs/M7-TRUNK-FORWARDING-FIX.md`.** That file's root
cause, fix and verification are carried across in §4; its status line and its prescribed
"next step" were both wrong and are corrected there.

---

## 1. Two cascaded switches: the physical topology

```
CPU (eth0)
   └── SoC internal switch  (rtl865x fabric, driver rtl819x-eth)
            │  SoC port 0 = RGMII trunk
            ⇕
         EXT1 = port 6      external RTL8367S (driver rtl8367b, bit-banged SMI)
            ├── port 0 ┐
            ├── port 1 │  LAN jacks
            ├── port 2 │  (bench host sits on jack 2)
            ├── port 3 ┘
            └── port 4    WAN jack (bench WAN peer)
```

There is exactly **one** wire between the SoC and the external switch. Every one of the
five physical jacks reaches the CPU across that single RGMII trunk. That fact is the
whole story of this document: the port model is the question of how the SoC is made to
tell those five jacks apart at the far end of one wire.

### ★ It is an RTL8367**S**, not an 8367R

It was documented as an 8367R for weeks. The chip identifies itself:

| register | value | meaning |
|---|---|---|
| `0x1300` chip_number | `0x6367` | 8367 family |
| `0x1301` chip_ver | `0x0020` | 8367S (8367C-family) — the 8367R-VB reads `0x1010` |

`rtl8367b.c:1857` is where the port accepts it:

```c
if (chip_num == 0x6367 && chip_ver == 0x0020) {
        chip_name = "8367S";
} else switch (chip_ver) { ... case 0x1010: chip_name = "8367R-VB";
```

Why the confusion is load-bearing rather than cosmetic: **the VLAN and MIB registers are
family-common, but the chip-reset and EXT-interface (RGMII) registers differ between the
two parts.** Every bug in §4 lives in exactly the registers that differ. The driver
therefore skips the whole extif-init/reset path for an 8367S deliberately
(`rtl8367b_is_8367s()`, `rtl8367b.c:769`).

Stock's own boot banner is part of why this stuck: it prints `8197F(PA=0) 8812B(PA=0)
8367R NOR RAM=64` (`docs/VENDOR-PARITY-INVENTORY.md` (stock boot banner)). The vendor's SDK filenames say
`rtl8367r/` too. Only the ID registers are authoritative. the early, now-deleted `ASSESSMENT.md` (the early,
now-retired assessment) and several in-tree comments still say 8367R — see §11.

### Where it is described

| thing | location |
|---|---|
| DTS node | `files/target/linux/realtek/dts/GWR1200ACV1.dts:119-125` |
| `cpu_port = <6>` — it is **EXT1**, not EXT0 | `GWR1200ACV1.dts:121` (rationale at `:112-118`) |
| jacks 0-4, WAN = jack 4 | `GWR1200ACV1.dts:115` |
| SMI: MDC = `gpio0 18`, MDIO = `gpio0 19` | `GWR1200ACV1.dts:123-124` (comment `:112`) |
| external-switch driver (~72 KB in the overlay), swconfig-managed | `files/target/linux/generic/files/drivers/net/phy/rtl8367b.c` |
| SoC-side switch/NIC driver | `files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x-eth.c` |

`rtl8367b.c` was cribbed from the mainline `rtl8367b` driver, not written from scratch;
everything DIR-842-specific in it is additive and marked.

---

## 2. Fork A (the old model) and why it was removed

**Fork A** was the original port model: a tagged-trunk VID cascade. The SoC ran two VLANs
— VID 2 = LAN, VID 1 = WAN — both riding the single RGMII trunk on SoC port 0, with the
CPU on SoC port 6. The external switch mapped each VID onto the right jacks. It bridged
correctly and it software-routed correctly, and the box was a working gateway on it.

**It could never hardware-offload, by construction.** Under Fork A every jack sits behind
the one trunk, so for every routed flow the ingress port and the egress port are *the same
SoC port*. The ASIC will not commit a hardware forward for a same-port hairpin.

The measurement that made this explicit was already in the bench notes and had been filed
as a caveat rather than a result — on the netns bench, with ingress and egress on the same
physical port:

| | throughput | packets per MB of payload |
|---|---|---|
| `HWNAT=Y` | 167 Mbit | 131 pkt/MB |
| `HWNAT=N` | 170 Mbit | 143 pkt/MB |

No difference. Offload simply did not engage (repo commit `0b6014c`).

### The evidence chain, in order

Each of these was an honest attempt to make Fork A offload. Each failed, and each failure
narrowed the target. The negative results are the point.

| commit | what it established |
|---|---|
| `884da4b7a1` | The ASIC's LAN egress chain named a **build-time constant** MAC/IP (one bench machine's NIC). Now learned per-flow. Scope stated honestly at the time: it did **not** restore forwarding — upload stayed 106 % through-CPU. |
| `64e9579387` | Stock's route/ARP architecture transcribed from a live stock dump: **no `/32` host routes at all**; both sides are plain connected subnets with `process = RT_ARP`, resolving at `window_base + host octet` (LAN row 2, WAN row 258). Also found that a nexthop → ARP index of 64 **truncates to 0** in a 6-bit field (stock uses 20). Confirms NAT is driven by the extIP entry's `type(NAPT)`, not by `route process=5`. |
| `6919725105` | Stock learns each routed peer with a **single-port** L2 member mask — client `mbr(2)`, peer `mbr(4)`, the router's own MAC `mbr(8)` = CPU. This port wrote `0x3f` (flood), and a routed unicast cannot be hardware-forwarded to a flood mask. Negative result recorded: the single-port mask **alone** changed nothing — still 106 % through-CPU. |
| `d3cb3c775a` | **Direct proof the SoC cannot address the jacks under Fork A.** Setting stock's real jack numbers (LAN `0x0f`, WAN `0x10`) does not merely fail to offload — it kills the datapath outright: 100 % loss both ways, box wedged, power-cycle required. The only single-port value that carried traffic at all was `0x01`, the RGMII trunk. |
| `5d712598a3` | Decoded stock's 8367S CPU-port writes straight out of the binary (`0x890`/`0x891`/`0x892` = `1 << cpu_port`, with the register number riding each `jal`'s delay slot; the loader leaves them at `0x00ff`). Applying them **alone** kills the datapath. Conclusion as written: *"stock's switch configuration is a coherent whole; hardware offload cannot be recovered by matching individual registers."* |

### The removal

Fork A was deleted in `86880757ed` once CPU-tag mode was proven and shipped as default.

* The `cpu_tag` module param and the Fork A VLAN branch (both VLANs = `0x7F` superset,
  PVID 2 on every port) are gone.
* `trunk_cold_force` went with it — **already dead code**. The trunk gate read
  `!loader-configured || trunk_cold_force || cpu_tag`, and with `cpu_tag` defaulting to 1
  that expression was unconditionally true. The replica had been running on every `eth0`
  open regardless.
* Every removal was **behaviourally a no-op**: with `cpu_tag = 1` shipped, none of the
  deleted branches could execute.

Post-removal regression gate on a cold NOR boot (`86880757ed`):

```
wired    0% loss at 64 B and 1400 B      CPUICR1=0x82
radios   br-lan = eth0.2 + wlan0 + wlan1
offload  hwnat=0  UP 184/184  DOWN 187/183 Mbit  through-CPU ~100.6%  cpu ~52%
         hwnat=1  UP 889/892  DOWN 896/895 Mbit  through-CPU   0.0%  cpu 0.3-0.7%
```

★ **The same commit had to correct comments, and that is the lesson.** The code comments
still documented Fork A as *current* behaviour, and asserted two things that had by then
been disproven: that "a routed unicast here can never resolve to anything but the trunk",
and that per-jack masks kill connectivity. Both were true only of Fork A; the shipped
defaults are per-jack (LAN `0x04` = jack 2, WAN `0x10` = jack 4) and that is precisely
what makes hardware forwarding work. `/proc` was still printing `(Fork A: VID-based, NO
CPU-tag)` to userspace. **Stale comments outlived the model they described and would have
actively misled the next reader.** §11 lists the ones that are still there.

### ⚠ One Fork-A claim that was retracted, and one that survived

An earlier session concluded *"Fork A walls on the single-trunk U-turn"* and cited a clean
MIB trace for it: 15 pings from the LAN host produced port2-in +15 → port6-out +15 →
port6-in **+0**, with `eth0` RX flat — frames dropped inside the SoC L3 engine, neither
forwarded nor trapped.

**That conclusion was wrong at the time it was made.** The actual blocker was
`MSCR.EN_IN_ACL` (bit 4) left **on** while `gw_prog`'s ACL entries were all zero — and an
all-zero ACL entry is *not* permit-all, so every frame was dropped before the routing
stage. Clearing it (`MSCR` `0x17` → `0x07`) made hardware L3 routing work end-to-end.
Separately, `PORT0_ROUTER_MODE` (`MACCR1` bit 0) *does* let a routed frame egress back out
the trunk it arrived on, so the U-turn is not a hard wall for **routing** at all.

What survived is the narrower, structural claim: a same-port hairpin cannot be
**hardware-offloaded**. Even that was stated carefully at the time (`b1a1921`) — the netns
bench and the two-jack bench were both same-SoC-port cases, so the rule was an *inference
from consistent failures*, not an isolated measurement. CPU-tag mode settled it
empirically: once the jacks became distinct SoC ports the flow was genuinely two-port, and
offload engaged.

---

## 3. ★ CPU-tag / port0-router mode — the model that ships

Commit `5e2645d21f` (2026-07-31 05:36), promoted to default in `26964573d5` (15:23).

★ **The reference implementation was in the vendor SDK the entire time.** It was missed on
earlier passes because `AsicDriver/rtl865x_asicL2.c` and `rtl8367r/rtk_api.c` are non-UTF8,
so plain `grep -r` skips them as binary. `grep -a` finds them immediately. Realtek's name
for the mechanism is not "cascade" — it is **"CPU tag" + "port0 router mode"**.

```c
init_8197f_p0()            sdk AsicDriver/rtl865x_asicL2.c:6408
    P0GMIICR |= (3 << CF_SEL_RGTXC_OFFSET);
    P0GMIICR |= (CFG_CPUC_TAG | CFG_TX_CPUC_TAG);   /* bits 25,26 */
    MACCR1   |= PORT0_ROUTER_MODE;                  /* bit 0 */
    P0GMIICR |= Conf_done;                          /* latched LAST */

RTL8367R_cpu_tag()         sdk rtl8367r/rtk_api.c:19641
    0x1219 = 1 << cpu_port          /* loader already sets 0x40 -> cpu_port 6 */
    0x121a = 0x281 | (cpu_port<<3)  /* => 0x2b1 : 4-byte format, insert to ALL */
```

**The SoC MAC inserts and strips the 4-byte Realtek `0x8899` tag in hardware.** There is no
software tag handling anywhere in the datapath. The RX descriptor's source-port field
carries the real jack; TX can name a real destination port. The jacks become SoC ports 0-4
and the CPU moves to SoC port 8.

The single most consequential field is `0x121a`. The loader leaves it at `0x00b5`:

| `0x121a` | bits | decode |
|---|---|---|
| `0x00b5` (loader) | EN=1, INSERTMODE=2 ("to **NONE**"), 8-byte format | tag turned on and then never inserted |
| `0x02b1` (ours) | EN(0)=1, INSERTMODE(2:1)=0 ("to ALL"), TRAP_PORT(5:3)=6, RXBYTECOUNT(7)=1, TAG_FORMAT(9)=1 → 4-byte | tag actually inserted, in the format the SoC decodes |

That one field is what hid the real jacks from the SoC. See `rtl8367b.c:1024-1062`
(`rtl8367s_cpu_tag_enable()`), whose comment carries the full decode.

### Measured, with both ends flipped together

```
before:  port=0  vid=1     for every frame        (the trunk — Fork A's signature)
after:   port=2  vid=2     LAN host on jack 2
         port=4  vid=1     WAN peer on jack 4
```

The datapath stayed up throughout the switchover (host→box 4/4, NAT'd host→peer 0 % loss
×3, box→peer 0 % loss). ★ **The per-jack L2 masks that had killed the datapath outright
under Fork A (`d3cb3c775a`) now program cleanly and keep NAT working** — that is the direct
confirmation that the ports are real, not just differently numbered.

Honest scope at `5e2645d21f`: upload was 187 Mbit and still 106 % of payload bytes through
the CPU at 99 % CPU. Four things were still Fork A-shaped and all four had vendor
references. They are the next four commits.

### Follow-through commits

**`74b90e83ba` — split VLAN membership, move the CPU to port 8.**
Per stock, taken from `include/net/rtl/rtl865x_netif.h:626,760-765` (`RTL_CPU_PORT 8`,
`RTL_LANPORT_MASK 0x10f`, `RTL_WANPORT_MASK 0x10`):

```c
sw_add_vlan_fid(RTL865X_VID_LAN, 0x10F, 0x00, 0); /* jacks 0-3 + CPU8, fid0 */
sw_add_vlan_fid(RTL865X_VID_WAN, 0x110, 0x00, 1); /* jack 4    + CPU8, fid1 */
```
`rtl819x-eth.c:614-615`. Per-jack PVIDs follow at `:620-622` (WAN jack defaults to the WAN
VID). The STP loop was extended from port 6 to port 8 — leaving it at 6 would strand the
new CPU port in blocking forever:

> `/* PCRP(n) is documented "port cfg 0..8". CPU-tag mode puts the CPU on port 8, so the loop must reach it or the CPU port never leaves blocking. (Fork A only ever went to 6.) */`
> — `rtl819x-eth.c:582-585`, constant at `:418`

Deliberate deviation from the vendor, documented in-tree at `:601-607`: **the vendor's WAN
mask omits the CPU port while its LAN mask includes it.** That asymmetry is real but
unexplained, and dropping the CPU out of the WAN VLAN is exactly how WAN dies silently, so
this port stays symmetric (`0x110`, CPU in both). The literal `0x10` is flagged as worth a
deliberate A/B, nothing more. Base L2 entries take stock's member `0x1f` / extmember `0x4`.

**`8b5a1d5a69` — write the VLAN entry's FID.**
`sw_add_vlan()` never wrote the FID field, so **both** VLANs claimed fid 0, while WAN peer
L2 entries are written under fid 1 (`rtl865x_asichal.c:530,773,829`). The L2/FDB lookup for
a routed egress is keyed by `{MAC, FID}` and the FID comes from the frame's VLAN entry — so
the WAN-bound lookup searched fid 0, found nothing, and missed. **Fork A never noticed
because it never resolved a per-port egress at all.** Fixed by adding `sw_add_vlan_fid()`
(`rtl819x-eth.c:176`) with `sw_add_vlan()` kept as a fid-0 wrapper (`:208`). Correct on its
own terms and verified non-regressing, but it did **not** engage offload by itself: upload
still 180 Mbit, 228 MB through the CPU for a 216 MB payload.

**`2dc1e821e0` — write FFCR at all.**
`FFCR` had **never been written** — the defines existed in `rtl819x_regs.h` with no write
site, leaving `EnUnkUC2CPU` at whatever the loader or reset left. Vendor gateway mode
explicitly clears it (`rtl865x_asicL2.c:6966`) so an unknown-DA unicast **floods** instead
of being punted to the CPU, and sets `EnUnkMC2CPU` (`:6967`). The rationale: a routed frame
whose egress L2 lookup misses becomes an unknown-DA unicast, and with the trap bit set the
ASIC resolves its destination portlist to the CPU and gives up forwarding — exactly the
measured signature (`hwFwd=0`, `isOriginal=1`, `extPortList=8` = the CPU ext port) despite
route and NAPT row both matching. In tree: `rtl865x_asichal.c:366-368` (defines),
`:938-950` (the write), exposed as `ffcr_unkuc_to_cpu` (`:176-180`). FFCR now reads
`0x00000001` = the vendor's exact gateway value. Also non-regressing, also **not** the
thing that engaged offload (184 Mbit, `hwFwd` still 0 on every frame).

> Note the distinction from the standing `v13/v14/v16` warning above `rtl865x_start()`:
> that warning is about *enabling* trap-to-CPU paths, which DMA'd into an unconfigured
> buffer and corrupted kernel RAM. This change *clears* one, moving traffic away from the
> CPU.

**`92eae00018` — `DACLRCR` 8-bit field layout** (was written with 7-bit offsets; the 8197F
uses 8-bit). `GW_DACLRCR` = `SWCORE + 0x4424`, written at `rtl865x_asichal.c:1056`. Full
treatment in `docs/HWNAT-OFFLOAD.md`.

**`26964573d5` — promote to default; the box self-configures.** Two gaps had to close
before the shipped defaults actually took effect at boot:

1. The `P0GMIICR` CPU-tag block was gated on `!(PITCR & bit0) || trunk_cold_force`, and
   **`PITCR` bit 0 is already set on a loader boot** — so the block was skipped and
   `cpu_tag=1` alone did nothing.
2. ★ **`rtl8367s_cpu_tag_enable()` was `EXPORT_SYMBOL`'d with zero callers.** The 8367S-side
   value had only ever been poked in by hand over debugfs, on every boot. Without it the
   SoC decodes a tag the switch never inserts: **100 % loss at every frame size** — exactly
   what the first defaults-only boot did. It is now called from the same trunk bring-up
   that programs the SoC end, so both ends of the RGMII link flip together
   (`rtl819x-eth.c:663-671`).

Verified on a boot with **no manual configuration whatsoever**:

```
8367S 0x121a = 0x02b1        (at boot, unpoked)
ping 64 B / 1400 B           0% loss
UPLOAD   (LAN->WAN)  890 Mbit   eth0.2 rx = 652 B / 1 112 500 000 B = 0.0% through CPU
DOWNLOAD (WAN->LAN)  911 Mbit   eth0.1 rx = 218 B / 1 138 750 000 B = 0.0% through CPU
```

`rtl8367s_cpu_tag_enable()` reads `0x1301` to derive the CPU port
(`(((v1301 >> 4) & 0xf) == 1) ? 5 : 6`, matching stock's `cpuPort` global) and then writes
the stock-exact CPU-tag registers — `0x1219 = 0x0040`, `0x121a = 0x02b1`, `0x121b = 0x00b5`,
`0x0890`-`0x0893` = `0x00ff`. Those values were read live from a stock → loader → RAM boot
(stock's 8367S config survives the loader), not derived. ⚠ The derived `cpuport` is
currently only *printed*; the register writes are hardcoded to the port-6 values. Correct
on this board, but it is not the general form the comment describes.

---

## 4. The RGMII trunk L2-forwarding fix (8367S EXT1 ↔ SoC-P0)

*(Carried over from `docs/M7-TRUNK-FORWARDING-FIX.md`, 2026-07-17, commit `574ea8d073`.)*

**Symptom.** Link forces up 1000/full on both trunk ends, VLANs apply, and **zero L2 frames
pass in either direction.** SoC `eth0 rx_packets = 0` and `CPUIISR = 0` (the hardware RX
interrupt status, not merely a stat); 8367S `port6 ifIn = 0`.

**The split that located it** — 8367S per-port `etherStats`, read live over debugfs (§10):

| port | ifInOctets | etherStatsOctets | meaning |
|---|---|---|---|
| 2 (host jack) | 82944 | 82944 | ingress from host works |
| 6 (EXT1 trunk) | **0** | **86208** | **egresses 816 packets toward the SoC**, receives nothing |

⇒ the 8367S internal L2 works and pushes frames at the SoC; the **RGMII trunk itself
delivers nothing**. The SoC side read back the loader's correct values the whole time
(`P0GMIICR = 0x00037d55` = Conf_done + TX-delay 1 + RX-delay 5; `PITCR = 1`;
`MACCR` bit 12 = 1), read by cycling `ip link set eth0 down/up` to trigger the driver's
`trunk-pre`/`trunk-post` dump — `CONFIG_DEVMEM` is off and busybox has no `devmem`.

An exhaustive live sweep of the 8367S EXT1 side then found nothing: mode (`0x1305`
bits 7:4 = 0..7), delay (`0x1307` = 0..15), pause off, rate meters (already unlimited),
EXT2/port7 bring-up — every combination transmitted into a void. An ARP flood from the SoC
reached **no** 8367S port. So the fault was not a register *value* on either configured end.

**Root cause.** `rtl8367b_reset_chip()` fired an in-kernel **gpio474 hardware reset at
driver probe — after the loader had already brought up the SoC-P0 side of the trunk.** That
re-straps the 8367S, and the driver's *partial* EXT1 reconfig then overwrites the loader's
*complete* analog/PLL bring-up with an incomplete replica. The two ends of the RGMII pair
desync: link forces up, data plane dead both ways, and **no register poke re-syncs it** —
the analog/PLL state is gone. `rtl8367b_setup()`'s own comment had already warned that the
reset "must happen BEFORE the loader initialises the SoC-side trunk"; the pulse had been
re-enabled, a regression.

**Fix** (`rtl8367b.c`): on a loader-configured boot, preserve the loader's working power-on
trunk; cold-bring-up only when the loader was absent.

* `rtl8367b_reset_chip()` — **no gpio474 pulse** for an 8367S, `return 0`
  (`rtl8367b.c:779-797`, the reasoning is in the comment at `:786-796`).
* `rtl8367b_setup()` — gate the EXT1 cold bring-up on **`0x1219 == 0`**. The loader's
  `RTL8367R_init` sets the CPU-port mask to `0x40`; the strap default is 0
  (`rtl8367b.c:1207-1258`).

Boot log on a loader boot (`rtl8367b.c:1210`):

```
RTL8367S: loader-configured uplink (0x1219=0040) — preserving power-on RGMII trunk
```

**Verification on hardware:** host → box ping 15-20/20, 0 % loss, ~0.4 ms, repeated across
boots; box → host 5/5 with the host MAC resolved; survives a clean software reboot (loader
reconfigures → driver preserves → trunk up, no power cycle needed).

**Co-landed in `574ea8d073`: `setup.c` must force `CLKMANAGE` bit 11 (switch-core clock)
on.** The D-Link v3.4.11B loader only enables it during its network/TFTP phase, so a
flashed boot intermittently left the entire switch register block at `0xBB800000`
un-clocked — it reads all-zero, `ndo_open`'s config writes become no-ops, and the box hangs
the instant the bridge forwards traffic into the unconfigured RX ring, right at
`br-lan: entered forwarding state`. Measured: `CLKMANAGE 0x...800` ⇒ clocked, `MSCR` reads
1, boots cleanly; `0x...000` ⇒ un-clocked, `MSCR` reads 0, hangs. **~80 % of flashed boots
landed in the un-clocked state.** `setup.c:112-130`.

### ★ Status correction

The old doc closed with *"FIXED for loader/RAM boots; flashed-boot cold path still open"*
and prescribed, as the next step, a SoC-side loader replica it called **"Fork B"**: `PCRP0`
force + `MacSwReset` + `EnablePHYIf`, `P0GMIICR` with `Conf_done` latched last, `PITCR`
bit 0, `MACCR` bit 12.

**That diagnosis was wrong, and it is exactly the trap the real fix had to escape.** The
trunk registers already ended up *identical* on both boot media — the old doc says so
itself, quoting `P0GMIICR = 0x00037d55` on the working boot, and the replica in §5 is
written precisely so that it is a data-plane no-op over the loader's own bring-up. The
difference between a working TFTP boot and a dead NOR boot was `CPUICR1` bit 1 (§6), one
register away from the trunk entirely.

**"Fork B" is now an orphan term.** It appears nowhere in `files-4.14/`, and its
counterpart Fork A was deleted in `86880757ed`. Treat any reference to it as historical.
The old doc's topology diagram is still physically correct; only its *port model* is
superseded.

---

## 5. Loader-absent cold bring-up

Commit `b76d2dbba4` completed the loader-absent path into a full replica of the vendor's
`init_97f_8367r` (RE'd: init ~`0x80197ed4`, trunk setup `0x80194728`, `P0GMIICR` write
`0x801947a0`). It now runs on **every** `eth0` open, including loader boots, because
CPU-tag mode needs the `P0GMIICR` tag bits and only this block programs them. That is
itself the standing proof of correctness: *a correct replica must be a data-plane no-op
over the loader's own bring-up*, and an earlier **partial** replica provably killed the
trunk in exactly this test.

Order, at `rtl819x-eth.c:681-745` — every step loader-exact:

1. board RGMII pad mux/drive — `0xB8000850`
2. `MACCR` `CF_SYSCLK_SEL` (bits 13:12 = 01) + `MACCR1` bit 0 (`PORT0_ROUTER_MODE`)
3. `EXTPCR0` bits 19:16 = `0x8`
4. `P0GMIICR`: GMAC = RGMII, loader fields bits 17:16 = 3 and 15:8 = `0x7d`, TX delay
   bit 4 + RX delay 5, **plus** `CFG_CPUC_TAG | CFG_TX_CPUC_TAG` (bits 25,26) and
   `CF_SEL_RGTXC` (bits 18,19). `Conf_done` stays **clear** here.
5. `PITCR` port 0 = RGMII (chip default is UTP)
6. `PCRP0` force-link `0x02940009` with the vendor `EnForceMode` double-toggle latch
7. `msleep(1000)`, then **latch `Conf_done` last**. Final `P0GMIICR` must read `0x00037d55`.

8367S side, in the `0x1219 == 0` branch: loader-exact `0x121a = 0xb5`, `0x121b = 0xb5`,
`0x0893 = 0xff`, plus the full `s_init[]` table and the EXT1 delay-before-mode ordering
(`rtl8367b.c:1214-1254`). `MACCR1` (`0xBB804058`) and `EXTPCR0` (`0xBB805108`) were added to
the fabric snapshot at the same time (`FAB_CFG_WORDS` 153 → 155).

### Two CPU-injected-TX fixes that belong here

**`3bf4b661f3` — mark CPU-injected TX as sourced from the CPU.**
`_New_swNic_send()` left `ph_srcExtPortNum = 0` on the direct-portlist TX path, so the
switch read the source as **physical port 0 — which *is* the RGMII trunk** — and
source-port-filtered it back out of the egress mask (`egress = destMask & ~(1 << srcPort)`).
Box-originated frames aimed at ports 0-5 had the trunk stripped and never reached the wire,
while host-originated ingress worked fine. Fix: `ph_asic0 = (3 << 0)` (srcExtPort = CPU) for
every CPU-injected frame (`rtl819x_swnic.c:633`), and widen the portlist mask `0x1f` → `0x3f`
to match intent (`rtl819x-eth.c:1262`). Box → host broadcast/ARP started working; the box's
ARP for the host went from FAILED to REACHABLE.

**`242eb10f32` — the half-ported flag table (#13, cold unicast).**
The port carried the **legacy RTL865x** used/own bit positions (`MBUF_USED = 0x02`,
`PKTHDR_USED = 0x0200`); the **8197F moved them** to `MBUF_USED_8197F = 0x80` and
`PKTHDR_USED_8197F = 0x8000`. CPU-injected TX therefore armed descriptors with
`m_flags = 0x1E`, `ph_flags = 0`, so the switch did its **own L2 lookup** on box-originated
frames and dropped unicast on a cold FDB (broadcast and ARP still flooded out via VLAN
membership, which is why the failure looked selective). TX-only fix — the RX pre-seed keeps
the legacy macros untouched:

| field | value | meaning |
|---|---|---|
| `m_flags` | `0x9C` | `MBUF_USED_8197F \| MBUF_EXT \| MBUF_PKTHDR \| MBUF_EOR` — `rtl819x_swnic.c:594` |
| `ph_flags` | `0x8800` | `PKTHDR_USED_8197F \| PKT_OUTGOING` — vendor direct-TX: egress `ph_portlist` verbatim, **no L2 lookup** — `rtl819x_swnic.c:620` |

Bench-validated: box → LAN host (cold FDB) 10/10, box → WAN peer (cold) 10/10,
host-initiated 20/20, NAT 10/10, large-frame RX 20/20, TX never wedged.

---

## 6. ★ `CPUICR1` bit 1 `CF_NIC_LITTLE_ENDIAN` — why a NOR boot differed from a TFTP boot

Commit `07fa6a8627`. **This is the root cause of "wired datapath dead on a NOR cold boot."**

The driver never programmed `CPUICR1` and silently inherited whatever the bootloader left.
**The bootloader only sets `CF_NIC_LITTLE_ENDIAN` when it runs its own network init:**

| boot medium | loader touches the NIC? | `CPUICR1` | wired datapath |
|---|---|---|---|
| TFTP RAM boot (loader fetches the image over the wire) | yes | `0x82` | works |
| NOR flash boot (loader never touches the NIC) | no | `0x80` | **dead** |

With the bit clear, the NIC master bus writes every received frame into DRAM
**32-bit-word byte-swapped**. Measured on the bench — a ping from `aa:bb:cc:00:00:02` lands
in the mbuf as:

```
51 fc 1c e0 | e0 00 ef c9 | 90 59 12 4c | 02 00 00 81 | 00 45 00 08
```

i.e. each word of `e0 1c fc 51 | c9 ef 00 e0 | 4c 12 59 90 | 81 00 00 02 | 08 00 45 00`
reversed. `eth_type_trans()` then reads:

* the destination MAC as `51:fc:1c:e0:e0:00` — octet 0, bit 0 set ⇒ classified
  `PACKET_MULTICAST`;
* the EtherType as `0x0200` instead of `0x8100` ⇒ the 802.1Q untag path is **never taken**.

Frames are received and counted, and reach neither the bridge nor the IP stack. The box
answers nothing.

### ★ Fingerprint, for regressions

**`eth0.2 rx_packets` and `rx_multicast` rise 1:1 while `br-lan` rx stays flat.**

| | eth0.2 rx | eth0.2 multicast | br-lan rx | ping to the box's own LAN IP |
|---|---|---|---|---|
| before | 42 | 42 | 16 (flat) | 100 % loss |
| after | 32 | 4 | 33 | 0 % loss at 64 B and 1400 B, settled jffs2 NOR cold boot |

### In tree

```c
REG32(CPUICR1) |= (1u << 1);        /* rtl819x-eth.c:481 */
```
Comment at `rtl819x-eth.c:450-480`. `CPUICR1` is defined at `rtl819x_regs.h:61` as
`(0x0a4 + CPU_IFACE_BASE)` = `0xB80100A4` (`CPU_IFACE_BASE` at `:30`).

★ **It deliberately sets bit 1 only.** The vendor's line
(`AsicDriver/rtl865x_asicL2.c:7647`) also ORs `CF_TXRX_DIV_LX` (bit 0) and `CF_TSO_ID_SEL`
(bit 4), but *every* boot that measured 890/900 Mbit of hardware offload ran with exactly
`0x82`. **Rule: reproduce known-good, not "more vendor-correct."**

**Placement matters.** The write goes *before* the `CPUICR` write (`:484`) so the bus byte
order is correct before Tx/Rx are enabled, and *before* the fabric snapshot at the end of
`rtl865x_start()` so that a `fabric_reset` restores the corrected value rather than the
loader's. `rtl819x-eth.c:275` lists `0xB80100A0` with count 2 in the snapshot table
(`DMA_CR4` + `CPUICR1`).

### ★ The general lesson, stated on its own

> **A TFTP boot runs the loader's full NIC init; a flash boot runs none of it. Any register
> the driver only read-modify-writes, or never writes at all, can therefore differ by boot
> medium.**

Two bugs of exactly that shape were found on the same day, and `CLKMANAGE` bit 11 (§4) is a
third, earlier instance of the identical class.

### The other one — a real bug that was *not* the bug

`1a63ae8deb`. The trunk bring-up did `v = REG32(PCRP0) | 0x02940009u`, OR-ing
`ForceSpeed1000M` (`2 << 19`) into whatever the loader left. **A NOR boot leaves ForceSpeed
= 1 (100M), and `1 | 2 = 3` = the reserved speed code**: the MAC then sits in force mode
with an invalid speed and the port passes nothing. `IPMSTP_PortST[22:21]` had the same
shape. Fixed by masking both fields before the OR (`rtl819x-eth.c:735-736`), and the 3 → 2
and 3 → 0 corrections were confirmed on hardware.

**And the port was still dead.** It cost most of a session.

```
PCRP0 = 42fc0039   NOR cold boot   ForceSpeed = 3 (reserved)
PCRP0 = 16942039   TFTP RAM boot   ForceSpeed = 2 (1000M)
```

★ **The reframe that cracked it:** *"the wired datapath is 100 % dead"* was the wrong frame.
**RX worked throughout.** The driver's own log line — `swnic rx#... port=02 vid=2` — was
printing the correct jack and the correct VLAN the entire time. Reading the log for what
*did* arrive moved the hunt from "ingress / trunk" to "received but not delivered", and
that led straight to the byte swap. **Check the counters before declaring a path dead.**

---

## 7. 802.3x pause on the trunk: a self-inflicted feedback loop

Commit `6ee32b7a9d`. **Measured opposite to the first hypothesis.**

The loader **forces** 802.3x pause on both trunk ends (8367S EXT1 `0x1311 = 0x1076`; SoC
`PCRP0[17:16] = 3`). A routed LAN→WAN flow crosses the **single** trunk **twice** — VID 2
ingress and VID 1 egress on the same SoC port 0 — so that pause becomes a self-inflicted
feedback loop: WAN-egress back-pressure PAUSEs the LAN ingress on the same wire and
throttles the flow into the ground.

Bench A/B, plain routed host → WAN peer, fresh `iperf3` (software-routed era, before
offload):

| trunk pause | sustained |
|---|---|
| loader's untouched value | ~27 Mbit |
| explicitly forced **on** (`trunk_pause=1`) | ~0.9 Mbit — collapse reproduced |
| force-**cleared** (`trunk_pause=2`, the shipped default) | ~95 Mbit |

The mechanism, from `rtl8367b.c:1065-1088`: because the flow U-turns on the single trunk,
the port0 egress queue is the tightest stage under a saturating TCP burst, and when the
SoC's per-port descriptor threshold (`PBFCR` `FCON = 90`) fires, the only lossless way it
can shed load is to PAUSE the 8367S.

**The fix is surgical on purpose.** `rtl8367s_trunk_pause_set()` (`rtl8367b.c:1090-1117`) is
a read-modify-write of the **DI1 force register only** — `0x1311` =
`RTL8367B_DI_FORCE_REG(1)` — touching `FORCE_TXPAUSE`/`FORCE_RXPAUSE` and nothing else. It
never touches mode, speed, duplex, link or delay: that is the §4 desync hazard class, and
the driver deliberately runs no extif-init for the 8367S, so a loader boot would otherwise
inherit the loader's EXT1 force value untouched. The SoC half is at `rtl819x-eth.c:773-788`
(`PCRP0[17:16]`, latched with the same `EnForceMode` double-toggle), idempotent so
self-heal re-runs of `rtl865x_start()` don't bounce the trunk. Module param at `:439-441`:
`2` = force off (default/fix), `1` = force on (reproduces the collapse), `0` = leave the
loader's value.

---

## 8. VLANs, MACs and the per-jack masks

### The two VLANs

| swconfig vlan | VID | role | 8367S ports | Linux |
|---|---|---|---|---|
| 1 | 1 | WAN | `4 6t` | `eth0.1` |
| 2 | 2 | LAN | `0 1 2 3 6t` | `eth0.2` |

`files/target/linux/realtek/base-files/etc/uci-defaults/99-dir842-m5:48-58`;
`ucidef_set_interfaces_lan_wan "eth0.2" "eth0.1"` at
`files/target/linux/realtek/base-files/etc/board.d/02_network:17`.

★ **The `rtl8367b` swconfig driver IGNORES the uci `vid=` attribute and uses the VLAN
*index*.** So `vlan '1'` ⇒ 8367S VID 1 and `vlan '2'` ⇒ VID 2 regardless of what `vid=`
says. The numbering must therefore be **WAN = 1 / LAN = 2** to match the SoC. Backwards, a
routed frame egressing SoC VID 1 (WAN) hits the 8367S's VID 1 = LAN jacks and never reaches
the WAN peer. Documented at `99-dir842-m5:16-22`; it was found with the debugfs `vlan_mc`
dump (§10) and it cost a long debug.

**There is no `eth1`.** The SoC has a single CPU-port netdev, split into `eth0.2` / `eth0.1`.
The phantom `eth1` had a real consequence: `config_generate` emitted a `wan_eth1_dev`
device section and hung the per-unit WAN MAC off a device that never appears, so `eth0.1`
silently inherited `eth0`'s LAN MAC — and since the ASIC programs its WAN netif from
`eth0.1`'s live MAC and rewrites HW-routed egress source to it, LAN == WAN MAC broke the
`LAN = WAN + 1` invariant and blackholed CPU↔WAN traffic while HW-routed frames still
worked (`02_network:10-17`).

**Only VID 1 and VID 2 exist in the SoC VLAN table.** TX on any other VID is silently
dropped, because `ph_vlanId` must name a VID whose SoC member mask covers the portlist
(`rtl819x-eth.c:1269-1276`). The historical M6.2 fallback of VID 9 had had no table entry
since Fork A, so untagged bare-`eth0` TX rode a VID with a garbage member mask; the
untagged fallback is now VID 2.

The trunk egresses **tagged** in both VLANs (`untag_mask = 0x00`). That is a deliberate
divergence from the vendor, which sends untagged and derives the VID from the ingress
port's PVID — both are coherent, this one is the one that is measured working
(`rtl819x-eth.c:609-613`).

### Per-jack L2 egress masks

| param | default | meaning |
|---|---|---|
| `l2_mask_lan` | `0x04` | jack 2 — the LAN host |
| `l2_mask_wan` | `0x10` | jack 4 — the WAN peer |

`rtl865x_asichal.c:187-193`, used at `:530`, `:773`, `:829`. Under Fork A these exact values
killed the datapath outright; under CPU-tag mode they are what makes a routed unicast
resolve to a real, distinct egress port.

### Per-unit MACs

Commits `d9e7d49960` → `16cea5ae63` → `484e4db4bd`.

`mtd1` ("MAC", flash `0x20000`) holds **two overlaid schemas**:

| offset | content | per-unit? |
|---|---|---|
| `0x00` | D-Link per-unit base MAC, 17-byte **ASCII** string — this is what stock reads via `libhwdata.so`. **This base IS the WAN MAC; LAN = base + 1.** | yes |
| `0x13` | Realtek default `macAddr[]` **binary** block | **no** — identical on every unit |

`board.d` had been reading `0x13`, so every board came up with the same MAC.

★ **The non-determinism had a second, separate cause: two independent writers.** Both
`board.d/02_network` *and* the `99-dir842-m5` uci-defaults pinned `network.lan/wan.macaddr`,
and which one won varied boot to boot — so some boots came up per-unit and identical later
boots came up on the fallback. The first diagnosis, an unreadable `/dev/mtd1`, was wrong.

Fix, in two parts:

* **`board.d` is the single source of truth.** It reads the ASCII MAC from `mtd1 + 0x00` and
  publishes it via `board.json` before netifd configures anything. The pin in
  `99-dir842-m5` was removed and the file now carries a "do not re-add" note (`:34-42`).
* **`gw_netif_mac()`** (`rtl865x_asichal.c:407`, called at `:745` and `:1109`) programs the
  ASIC netif from the interface's **live** MAC, so the software and hardware datapaths
  cannot disagree by construction.

Gate: two cold boots, `lan e0:1c:fc:51:c9:ef` / `wan …ee`, 0 % loss both.

WiFi BSSID = **base + 2** (`b68c51ced1`) — rtw88 had been inventing a random MAC every boot
because the RTL8822BE efuse on this board is blank.

---

## 9. Cold unicast, ASIC L2 warm-up, and the boot service

Cold unicast (task #13) is **not "fixed" — it is institutionalised.** The ASIC L2/ARP
entries start empty, and a cold unicast is not delivered until traffic has flowed. Without
a warm-up a freshly booted box reads as "100 % packet loss" and looks broken.

`files/target/linux/realtek/base-files/etc/init.d/dir842-asic` (`START=97`) exists to do
that, in a strict order. **The ordering comment in that file is the authority** and is
reproduced here because each step is there for a measured reason:

1. **`swconfig apply`** — the external 8367S keeps a wedged trunk→jack forwarding state
   across warm reloads, precisely *because* `rtl8367b_reset_chip()` deliberately skips the
   8367S soft reset to preserve the loader's power-on RGMII uplink (§4). Re-applying the
   VLAN/forwarding tables after netifd clears it; without it, routed frames cross the trunk
   but never egress the jacks.
2. **`fabric_reset = 3`** — ★ **measured-required.** Without it a fresh boot comes up with
   the switch core in a state where **nothing forwards**: every path reads 100 % loss even
   though links are up, VLANs are right, `br-lan` has its IP, and `gw_prog` reports
   `netif readback PASS`. With it, the very same boot goes to 0 % loss. Level 3 = engine
   reinit + `CPUICR` soft-reset + full fabric reset (`SIRR FULL_RST` → swcore clock cycle →
   `MEMCR` SRAM re-init → register restore), and it re-arms the ASIC gw tables in-kernel.
3. **`gw_prog`** (via `cat /proc/rtl865x_gw`) — programs netifs, routes, L2 nexthops and the
   ACL ranges. ★ **This WIPES the ASIC L2 tables, so it MUST come before the warm-up, never
   after.**
4. **warm** — a few box-originated pings to the configured peers prime the L2/ARP entries.

The whole sequence is backgrounded behind a `sleep 5` so boot never blocks, but it is
strictly ordered inside, and it is idempotent — `/etc/init.d/dir842-asic restart` is a
supported recovery. The initial wait is a **poll** for the per-unit flash MAC (R4; was a
fixed 5-second sleep): `gw_prog` must read the real netif MACs through `gw_netif_mac()`
rather than the fallback constants, or an ASIC-vs-Linux MAC mismatch blackholes the
datapath (§8).

★ **`hwnat` is armed by this service as its LAST step** (R4 2026-08-16), strictly after
the warm-up — measured: enabling it before the tables are warm kills the datapath
outright (100 % loss, recovers on `echo 0` + `fabric_reset`). Pre-R4 images left it off
at boot entirely (the reverse path still CPU-trapped back then); the runtime toggle
remains `echo 0/1 > /sys/module/rtl819x/parameters/hwnat`.

R4 also made this service the **single owner** of the bring-up. It used to be duplicated
inline in `rc.local` because invoking the service from there "produced no effect" — root
cause: the script shipped without the execute bit, so the baked `S97` symlink (and any
exec of it) failed with a silent "Permission denied" every boot, while the *sourced*
`rc.local` ran. The exec bit is fixed and `rc.local` is empty. Before all that, an even
older `rc.local` `( sleep 5; … ) &` fired at a fixed 5 s regardless of netifd, never
warmed the L2 tables, and never re-ran after a netifd reload — so a boot could easily
land with the gateway half-programmed.

---

## 10. The debugfs SMI window (the key diagnostic lever)

**Keep this.** There is no `/dev/mem` and no busybox `devmem` on this box, so without it
every register experiment costs a full rebuild-and-reflash cycle.

`CONFIG_RTL8366_SMI_DEBUG_FS` is forced on in the overlay — the `#ifdef` was changed to
`#if 1` in both `rtl8366_smi.c` (`:25`, `:701`) and its header. It ships that way. That
gives live 8367S register access under `/sys/kernel/debug/rtl8367b/`
(`debugfs_create_dir(dev_name(smi->parent), …)`, `rtl8366_smi.c:938`):

| node | use |
|---|---|
| `reg` + `val` | read/write **any** 8367S SMI register (`:947`, `:955`) |
| `vlan_mc` | the VLAN member-config table (`:963`) |
| `vlan_4k` | the 4K VLAN table (`:979`) |
| `pvid` | per-port PVIDs (`:987`) |
| `mibs` | per-port MIB counters (`:995`) |

```sh
# read
echo 0x121a > /sys/kernel/debug/rtl8367b/reg
cat         /sys/kernel/debug/rtl8367b/val
# write
echo 0x121a > /sys/kernel/debug/rtl8367b/reg
echo 0x02b1 > /sys/kernel/debug/rtl8367b/val
```

What it cracked:

* **The swapped-VID bug** (§8) — `cat vlan_mc` exposed `vid1 = LAN` / `vid2 = WAN`
  instantly, which no amount of reasoning about `vid=` was going to find.
* **The trunk-forwarding bug** (§4) — the per-port MIB table that split "8367S L2 works" from
  "the RGMII trunk delivers nothing".
* **CPU-tag mode** (§3) — the `0x121a = 0x02b1` value was proven by hand over this interface,
  on every boot, for a whole day before `rtl8367s_cpu_tag_enable()` acquired a caller.

On the **SoC** side there was no equivalent: stock exposes `/proc/rtl865x/memory` and this
port had nothing, so a register diff against stock was impossible. Commit `40f5dfb069` added
a switch-core register-window dump to close that gap.

---

## 11. Stale constants and comments to be aware of

Everything below is *known* stale and left in place; none of it changes behaviour, but all
of it will mislead a reader who trusts comments over code. Recorded here rather than
silently fixed, because §2 is the standing evidence that stale comments do real damage.

| where | what it says | reality |
|---|---|---|
| `rtl819x_regs.h:166` | `#define SW_CPU_PORT 6` — "CPU = L2 port 6 (bit 6)" | In CPU-tag mode the CPU is on **port 8** (`rtl819x-eth.c:418`, `:582-585`, `:597-599`). A reader will trip over this. |
| `rtl819x-eth.c:568-577` | An M4-era block describing "VLAN 9 = the stock LAN vid", "every internal port (0-5) + the CPU port (6) as untagged members", "PVID 9 on every port" | The code immediately below it installs VID 1 / VID 2, members `0x10F` / `0x110`, CPU on port 8, **tagged**, per-jack PVIDs. The comment describes neither Fork A's final state nor the shipped one. |
| `rtl819x-eth.c:600` | "`sw_add_vlan()`'s flat mask splits at bit 6" | Correct about the mask layout, but the call site is `sw_add_vlan_fid()`; `sw_add_vlan()` (`:208`) is now a fid-0 wrapper with no remaining callers. |
| `rtl819x-eth.c:1269-1276` | "only VID 2 (LAN) and VID 1 (WAN) are installed (`sw_add_vlan`, members `0x7F`, trunk egress TAGGED)" | The VIDs and the tagging are right; **`0x7F` is the Fork A superset** and no longer what is programmed (`0x10F` / `0x110`). |
| `99-dir842-m5:15` | "M6.6 HW-routing cascade (**VID-based, no CPU-tag**)" | The shipped model *is* CPU-tag. The VID-swap gotcha documented right below it (`:16-22`) is still entirely correct and load-bearing. |
| `rtl819x-eth.c:391,393,576,699,1255,1258`; `rtl8367b.c` comments; the deleted `ASSESSMENT.md` | "RTL8367R" | The part is an RTL8367**S** (§1). The vendor SDK directory really is named `rtl8367r/` and stock's own banner really does print `8367R`, so the name is not always a mistake — but the chip on this board is not one. |
| anywhere | "Fork B" | Orphan term. Appears in no shipped file; its counterpart Fork A was deleted in `86880757ed`. See §4. |
| `rtl8367b.c:1024-1062` | derives `cpuport` from reg `0x1301` | The derived value is only *printed*; the register writes are hardcoded to the port-6 values (`0x1219 = 0x0040`, `0x121a = 0x02b1`). Correct on this board, not general. |

---

## Cross-references

| topic | doc |
|---|---|
| The L3/L4 NAPT offload itself — byte order, `DACLRCR`, `SWTCR1`, the 890/896 Mbit result | `docs/HWNAT-OFFLOAD.md`, `docs/M7-HWNAT-REVERSE-NAPT.md` |
| The CPU-RX large-frame fabric wedge and its self-heal | `docs/M7-LARGE-FRAME-RX-WEDGE.md` |
| Stock firmware inventory and parity checklist | `docs/VENDOR-PARITY-INVENTORY.md` |
| Superseded by this file | `docs/M7-TRUNK-FORWARDING-FIX.md` |
