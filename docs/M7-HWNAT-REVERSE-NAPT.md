# M7 — DIR-842 (RTL8197F) hardware NAT: forward DONE, reverse-NAPT investigation

**Status:** LAN→WAN forward hardware-NAT **works, corruption-free** (first on
mainline). WAN→LAN **reverse**-NAPT is root-caused through 7 layers to **two
isolated remaining blockers**. This doc is the continuation map.

Openwrt tree branch `Realtek`. Relevant commits (newest first):
`9562db2` (asichal: corruption fix + ingress-ACL infra), `3cd6ec0` (hwnat: reverse
row verification encoding + debug), `374bfce`/`e72159b`/`85f01c9` (byte-order,
aging, ring/CRC — the offload-engagement breakthrough).

---

## Bench rig
```
hal enp3s0 (LAN 192.168.0.2/24, no default route, static route 172.16.0.0/24 via .0.1)
   └─DIR-842 LAN jack── [DIR-842] ──WAN jack─┘ tiny eth0 (WAN 172.16.0.2/24)
        box LAN 192.168.0.1  box WAN 172.16.0.1 (=masquerade extIP, KEEP static)
   box LAN MAC 00:e0:4c:81:96:c2   box WAN MAC 00:e0:4c:81:96:c3   hal enp3s0 54:bf:64:18:b8:de   tiny e4:5f:01:04:98:af
```
- OpenWrt is **RAM-booted only** via `catch-robust.sh` (ESC-spam→TFTP→J). **NOTE: the
  NOR flash now holds OpenWrt (Jul 18 build), NOT stock** — the M7.1 flash gate was
  passed. Stock is only in the 8 MB backup (`~/dir842-firmware/dir842-mtd6-ALL.bin`,
  also `~/dir842-r1-openwrt/backups/`). Flash boot currently crash-loops (#11), which
  conveniently gives the catcher frequent windows.
- Background builds/catchers get **killed after ~2-3 min** — run the catcher
  FOREGROUND (`timeout 560 bash catch-robust.sh`), builds in background + relaunch if
  killed (`docker kill $(docker ps -q --filter ancestor=owrt-build)` then re-run).
- Build: `docker run --rm -v /home/agiu/dir842-build:/build owrt-build bash /build/container-fast.sh drivers/net/ethernet/rtl819x/rtl865x_asichal.c` (~3 min).
- gw_prog (routes/mode/extIP/**ACL**) runs ONLY on `cat /proc/rtl865x_gw`, NOT at boot.
- hal ARP for the box goes FAILED after flaps: `sudo ip neigh replace 192.168.0.1 lladdr 00:e0:4c:81:96:c2 dev enp3s0 nud permanent`. Box ARP for peers needs re-warming each boot (box-originated ping tiny + hal). iperf3 server on tiny wedges — always `pkill -9 iperf3; setsid iperf3 -s …` fresh; it's also just slow (retry the ssh).

## Setup ritual each boot
`cat /proc/rtl865x_gw >/dev/null` (programs mode+routes+ACL) → `echo 1 > /sys/module/rtl819x/parameters/hwnat` → warm ARP.

---

## The reverse-NAPT chain — 7 layers, what each was
A LAN→WAN TCP/UDP reply (tiny → extIP 172.16.0.1:G) must be reverse-NAPT'd
(dst extIP:G → LAN-client 192.168.0.2:intPort) and routed out the LAN. It wasn't.

1. **Byte order (85f01c9)** — ASIC hashes/keys the ON-WIRE (network-order) fields;
   host order wrote rows at byte-swapped indices → 100% miss. Fixed → forward matches.
2. **NAPT action bits** — isStatic=1, dedicate=0, TCPFlag 0x3/0x2 (vendor nat.c:1133-42).
3. **Inbound VERIFICATION row (3cd6ec0)** — the reply row is NOT a copy of the outbound
   row; it stores `very`=HASH1(remIP,remPort,0,0) in selEIdx + G split across
   offset/selIPIdx (nat.c:1137-40). Necessary but insufficient alone.
4. **SWTCR1 = 0x2200 (9562db2)** — vendor enables BOTH L4EnHash1 AND EnL4WayH(4-way).
   `=(1<<13)` alone CLOBBERED the loader's 4-way bit and **corrupted the whole L4
   datapath** (hwnat=1 killed even ICMP forward). Restoring 0x2200 → **forward HW-NAT
   corruption-free**. ★ VALIDATED WIN.
5. **ROUTE0 /24 → /32 (9562db2)** — the WAN-subnet route shadowed the extIP (box's own
   WAN IP), classifying replies OUTBOUND. Narrowed so the extIP isn't on a specific
   process=5 route.
6. **MSCR.EN_IN_ACL + catch-all permit + DACLRCR (9562db2)** — reverse-NAPT is gated on
   the ingress-ACL stage. EN_IN_ACL(bit4) on + word7=0x07000000 permit filled in the
   ACL slots + **DACLRCR = the net-decision-miss ACL range** (the register nobody
   programmed — WITHOUT it, EN_IN_ACL dropped all forward). ★ forward survives EN_IN_ACL.
7. **dst-MAC==WAN-MAC → TOCPU rule at ACL slot 4 (9562db2)** — reclassifies a reply as
   "to me" so the reverse stage runs. ★ garbage-MAC flood STOPPED; **ICMP now NATs
   end-to-end both ways (ping 0% loss)**.

## What works now (measured)
- Forward LAN→WAN HW-NAT, corruption-free (hwnat=1, hal→tiny 0% loss; outbound row age pins = ASIC forwarding).
- ICMP through-NAT both directions (ping 0% loss each way).
- Full ingress-ACL stage (EN_IN_ACL + permit + DACLRCR), verified via gw readback:
  `MSCR=0x17`, `DACLRCR=0x1fbf4180`, `ACL[0] w7=0x07000000`, netif `w2=0xfefe8180` (inACL [0..3]).

---

## TWO REMAINING BLOCKERS (start here tomorrow)

### A. HW NAPT-reverse LOOKUP doesn't hit the inbound row  ← the gigabit-down gate
With rule 7, WAN replies reach the CPU (`eth0` rx delta ~25/flow) = they TRAP rather
than HW-un-NAT. So the HW NAPT-reverse lookup for the "to me" reply is MISSING the
inbound row (idx_in). The reverse is landing on TOCPU (software), never HW.
- idx_in = `gw_napt_hash1(is_tcp, htonl(remIp), htons(remPort), htonl(extIp), htons(G))`.
  Verify the SILICON's inbound-direction lookup hashes the reply to THIS index under
  enhanced-hash1+4-way (it may key differently for the inbound/verification path).
- The inbound row's `very` verification (selEIdx) must match what the ASIC recomputes
  from the reply — re-derive under 4-way mode.
- Instrument: read the inbound row's agingTime during a WAN-heavy reply burst (download
  direction) — pin = HW hit, decay = miss. (Aging-log instrument already in 3cd6ec0.)
- If the HW reverse genuinely can't be made to hit: a **live stock ACL/register dump**
  is the ground truth — boot stock from `dir842-mtd6-ALL.bin` (RAM-boot the stock
  kernel, or reflash), run a live download NAT flow, dump the ASIC ACL table (raw 11-word
  rows), MSCR, SWTCR0/1, DACLRCR, and a live inbound L4 row. Items decisive: the inbound
  row encoding under a real flow + whether stock's reverse is HW or the same TOCPU path.

### B. Software fallback doesn't complete TCP (ICMP does)  ← pure netfilter
With the reply trapped to CPU, ICMP NATs fine but TCP freezes at the handshake
(conntrack stuck ~5/4). FORWARD chain HAS `-m conntrack --ctstate RELATED,ESTABLISHED
-j ACCEPT`, so it's not a plain policy drop. Suspect: how the TOCPU-trapped WAN-ingress
frame is injected (local INPUT vs PREROUTING/conntrack/FORWARD), or a TCP-state/invalid
drop. Diagnose with `conntrack -E`, tcpdump on the box's eth0.1/eth0.2, and `iptables
-nvL` counters during a flow. (Note: this only matters if we accept a software reverse;
for gigabit-down, fix A instead.)

## Artifacts
- Agent ACL-encoding spec: `~/dir842-build/wf-specs/spec-acl-encoding.md` (the exact
  catch-all permit + dst-MAC→CPU 11-word arrays, vendor file:line traced).
- NAPT-row specs: `~/dir842-build/wf-specs/spec-napt-hit{2,4,6}.json`.
- Vendor SDK (readable): `~/…/scratchpad/sdk-rtl819x/…/drivers/net/rtl819x/`.
- Stock decoded tables: `~/dir842-build/m6.6-hwnat/STOCK-TABLES.md` (NO WAN cable when
  captured → live ACL/NAPT tables empty; that's the evidence gap fix A/the dump closes).

---

# Session 2 (2026-07-21/29) — uploads offloaded; three reverse-path candidates falsified

## Result
**LAN→WAN (upload) is hardware-offloaded: ~90% of data packets bypass the CPU**
(measured `eth0.2` rx delta = 5670 vs ~54000 data packets over an 8 s flow), at
78.8 Mbit/s = line rate for the 100 Mb WAN link. Committed `2ba78be`, pushed as
`5556d00`.

**WAN→LAN (download) is still CPU-bound** and, worse, a sustained download
*wedges the box outright* (every reply is trapped to the CPU, and large frames
hammering the CPU is exactly the M7 large-frame-wedge trigger; recovery =
`echo 3 > /sys/module/rtl819x/parameters/fabric_reset`). So HW reverse-NAPT is
**required**, not an optimisation.

## What fixed the upload path
`WAN netif ingress ACL range [0..3] -> [4..6]` (stock's layout). The
dst-MAC==WAN-MAC classifier lives at slot 4, so with the range at [0..3] it was
never scanned; every reply fell through to the catch-all permit, was
transit-routed on its un-rewritten dst, and left the WAN with an unresolved
nexthop DMAC. Peer-side garbage-DMAC frames went **13-21 per flow -> 0**.

## Three reverse-path candidates, all falsified on hardware
| # | change | result | what it rules out |
|---|---|---|---|
| 1 | extIP `/32` route, `process=2` (ARP/direct) | reply **black-holed** (~0 pkts) | The extIP-table match does **not** pre-empt the route lookup. Routing happens first; the extIP entry alone never triggers the reverse rewrite. Kills the whole "fix it with routes" family. |
| 2 | dst-MAC==WAN-MAC -> **TOCPU** ACL rule at slot 4 | works, but traps **every** WAN frame (`WAN_RX` == every ACK) -> software-speed downloads, and wedges under sustained load | Correctness via CPU trap is achievable but is the opposite of the goal. |
| 3 | `SWTCR0` bits[4:3] `WANRouteMode` Forward(0) -> **ToCpu(1)** | WAN path appeared dead both ways | ⚠️ **LIKELY CONFOUNDED — retest.** During that run tiny had silently lost `172.16.0.2` and the host's `172.16.0.0/24` route had reverted to the house gateway. Both faults read as "100% packet loss" identically. |

### Why candidate 3 is still the best lead
`WANRouteMode` (`rtl865xc_asicregs.h:1563-1571`, "Route WAN packets":
Forward=0 / ToCpu=1 / Drop=2) sits at the silicon reset default **Forward**, and
**nothing in the entire vendor SDK ever writes it**. Forward is precisely the
observed flood: a WAN-ingress reply that does not come out of the L4
reverse-NAPT stage gets *transit-routed*. Transit routing from the WAN is
meaningless on a NAT gateway. `ToCpu` should divert **only** the packets that
MISS reverse-NAPT, leaving hits on the hardware path — unlike candidate 2, which
blanket-traps everything. Verified live as `SWTCR0=0x000847ec` (bits[4:3]=01).
**Retest it with the bench-drift guards below in place before drawing any
conclusion.**

## ★ Bench-drift trap (cost one wrong conclusion — read before trusting a negative)
Three things silently revert and each one makes EVERY path read 100% loss:
1. **Host USB-eth loses its IPv4** — NetworkManager strips it (`nmcli device set
   <if> managed no`).
2. **Host route `172.16.0.0/24` reverts** to the house gateway via `enp3s0`
   (`ip route get 172.16.0.2` must show the USB NIC).
3. **tiny's br0 loses `172.16.0.2`** (its bench WAN address; `eth0` is a br0
   SLAVE, the address is on br0).
Plus: `cat /proc/rtl865x_gw` **wipes the ASIC L2 tables**, so warming must come
after it, never before. `bench-up.sh` now re-asserts all of this immediately
before measuring and prints `ip route get` so the guard is visible.

## Next step
Re-run candidate 3 (`WANRouteMode=ToCpu`, TOCPU ACL rule removed) with
`bench-up.sh` guards, and measure `eth0.1` rx delta during an `iperf3 -R`
download: near-zero => hardware reverse-NAPT; ~every packet => still trapped.
Also still open from session 1: the extIP entry's `nextHop` field semantics (we
set 0, which may be why a rewritten reply tries to egress via the WAN nexthop
chain).

⚠️ The WAN link negotiated **100 Mb/s** (tiny's NIC advertises gigabit, so it is
the cable). Gigabit cannot be demonstrated until that is replaced.

---

# R1 result (2026-07-30): flash-boot crash FIXED; flash-boot EGRESS is the remaining defect

## ★ The crash was OpenWrt's own crash logger, recursing
`crashlog_printf()` (generic `hack-4.14/930-crashlog.patch`) faults on this SoC on a bogus
`0x00000c00` access **from inside `die()`**. So recording the first oops re-enters the
fault path and recurses — observed `Oops[#448]`, PID 0, `task.stack=NULL` — flooding the
console until the boot hangs. The genuine first fault was never printed, which is why #11
had no root cause. Disabling crashlog removes the crash outright.

**GATE PASSED: 10/10 consecutive unattended cold boots from NOR** — `oops=0`, userspace
reached, `switching to jffs2 overlay` on every boot (previously 100% crash-loop).
Commits: openwrt `36c3810`, mirror `b58a0a5`.

⚠ **Config trap:** `# CONFIG_CRASHLOG is not set` in the subtarget `config-4.14` is NOT
enough. `scripts/kconfig.pl` merges it correctly (verified), but OpenWrt injects top-level
`CONFIG_KERNEL_<SYM>` knobs **after** that merge, so `CONFIG_KERNEL_CRASHLOG=y` silently
won and the built kernel still contained `crashlog_printf` at `800af680` — exactly the
crashing address. Effective switch = `# CONFIG_KERNEL_CRASHLOG is not set` in the
top-level `.config` (git-ignored ⇒ recorded in `seed-m5.config`). **Verify by symbol, not
by config:** `System.map` crashlog count 10 → 0.

## The remaining flash-boot defect: egress is dead, ingress is perfect
A flash boot now reaches a shell but has no working network. Measured, hop by hop:

| hop | result |
|---|---|
| host → 8367S port 2 | ✓ arrives (`ifInOctets` grows) |
| 8367S → trunk port 6 egress (switch→SoC) | ✓ `TRUNKOUT=408` |
| SoC → CPU (`eth0` rx) | ✓ `CPURX=6` |
| VLAN demux (`eth0.2` rx) | ✓ `LANVIF_RX=6` |
| bridge local delivery (`br-lan` rx, unicast) | ✓ `BRLAN2=4` (with a static ARP) |
| **box → host (anything the box emits)** | ✗ **NOTHING on the wire** |

`tcpdump -i <host> 'ether src 00:e0:4c:81:96:c2 or icmp'` during 5 pings captured **only
the 5 echo requests and zero frames from the box.** So ingress works end to end and the
box's TX never leaves. Corroborating: the box's ARP table stays **empty** and broadcast
ARP never reaches `br-lan` (rx=0) while unicast does (rx=4).

Board state is otherwise correct on flash boot: `br-lan` = 192.168.0.1 with `eth0.2`
enslaved, bridge port `state=3` (forwarding), `carrier=1`, `stp=0`, VLAN 2 =
`ports 0 1 2 3 6t`, links up (port2 1000, port4 100, trunk 1000), `gw_prog` = `netif
readback PASS`, and the trunk registers END UP IDENTICAL to a working RAM boot
(`P0GMIICR=0x00037d55`, `PCRP0=42fc0039`, `PITCR=1`).

★ Note the loader difference that makes this the *loader-absent* path: at `trunk-pre` a
flash boot shows `P0GMIICR=0x00037d00` — missing the `0x55` Conf_done/delay bits — so the
driver's **cold replica** runs and fixes it (and `first TX` / `first RX frame` do appear at
boot). The 8367S itself IS loader-configured (`[1219]=0040`). So this is not the 8367S and
not the register values; it is the SoC's **CPU-TX egress** on the cold path — the #13
"box-originated traffic" family resurfacing when the loader has not fully initialised the
port. Next step: diff the CPU-TX path (`_New_swNic_send` portlist / `ph_srcExtPortNum` /
SoC VLAN+PVID membership programmed by `rtl865x_start`) between a loader boot and a cold
flash boot, since every register we currently replicate already matches.

## Bench confound #4: the box reboots mid-test (RAM boot is volatile)

Cost three consecutive wrong conclusions, so it goes here with the other guards.

Symptom: every path reads 100% packet loss, *including* the hand-run recovery
sequence that had worked minutes earlier. It looks exactly like a datapath
regression from whatever you just changed.

Actual cause: the box had **rebooted**. A RAM boot does not persist, so after a
reset the box is no longer running the image under test. The boot log made it
unambiguous — two `DRAM Size` / `Jump to image` pairs, and only the *first* was
followed by `Linux version`:

    DRAM Size ... Jump to image ... Linux version     <- the RAM boot under test
    DRAM Size ... Jump to image                       <- rebooted, never came up

Add to the trust-the-bench checklist, and check these **in this order** before
believing any negative result:

1. `cat /sys/class/net/<usb-eth>/carrier` — 0 means the link is down; a box
   mid-reset reads carrier 0 and every ping fails for a reason that has nothing
   to do with your change.
2. `grep -c 'Linux version' bootlog` — must have advanced since the boot you
   started. A trailing `Jump to image` with no `Linux version` = the box died.
3. Then the three known reverters (host USB-eth IPv4, the `172.16.0.0/24` route,
   tiny's `br0` address).

Rule of thumb that would have saved all three runs: **never measure without first
proving the box is up.** Carrier and a fresh `Linux version` are two cheap reads;
a wrong conclusion costs a rebuild-and-boot cycle plus the bad inference it seeds.

## R6 gate measurement (2026-07-30) — baseline taken, hardware reverse NOT engaging

The plan's R6 gate is: *`eth0.1` rx delta ≈ 0 during a reverse transfer (hardware
reverse) vs ≈ every packet (still trapped).* That measurement had never actually
been taken. It has now.

**Method.** iperf3 was unusable — its server would not stay resident on the WAN peer
across a tool invocation. Reverse-path bulk traffic was driven instead with a plain
ssh stream to the peer's WAN address, which traverses the box as an established
NAT flow with data going WAN→LAN:

    ssh -i <key> agiu@172.16.0.2 "dd if=/dev/zero bs=1M count=300" | dd of=/dev/null

⚠ Two bench traps worth recording: `ssh tiny` resolves over Tailscale (100.64.0.14)
and does NOT traverse the box — always target `172.16.0.2` explicitly. And ssh to the
raw IP does not pick up the `Host tiny` config block, so it needs `-i <key>
-o BatchMode=yes` or it hangs on auth and looks like a network failure.

**Result:**

    eth0.1 rx_packets  before 70  ->  after 55063
    CPU-seen WAN rx delta = 54,993 packets for ~300 MB downloaded

That is the "still trapped" signature: essentially every inbound packet is taken by
the CPU. **Hardware reverse-NAPT is not engaging** — confirmed by measurement now,
not inferred. (`hwnat` was N for this run, so this is the honest software baseline
against which any future hardware-reverse attempt must be compared.)

**The documented consequence reproduced.** After ~180 s of sustained download the box
wedged: 75% loss on small frames, 100% on 1400 B and on the NAT path, and it did NOT
recover by warming. This is the "a sustained download wedges the box" behaviour, and
it is exactly why R6 matters beyond throughput — the CPU-trapped reverse path is what
makes downloads destabilise the box.

**Detector gap confirmed.** The FCS wedge detector did NOT fire (`resets=0`), so no
self-heal occurred; the box needed a manual `echo 3 > .../fabric_reset` (+ gw re-arm
+ warm), which restored all three paths to 0% loss. This is precisely residual
follow-up (2) recorded above — a box wedged without enough large-frame arrivals to
arm the detector stays wedged. Worth raising in priority: it is now observed, not
hypothetical.

**Status: R6's objective (make the ASIC do the reverse) is NOT achieved.** Per the
plan this is the time-boxed stretch, so it aborts cleanly here with the falsified
candidates recorded. What R6 gained this pass: the gate baseline is measured, the
wedge-on-download is reproduced end-to-end, and the detector gap is confirmed.
Still-open candidates are unchanged — `SWTCR0.WANRouteMode=ToCpu` retest under the
now-known confounds (#4 reboot, #5 ARP flush), and the live stock register dump as
ground truth.

## R6 candidate 4 FALSIFIED: SWTCR0.WANRouteMode = ToCpu (2026-07-30)

The plan called for retesting `WANRouteMode` because the original negative was
confounded. It has now been retested properly, on a bench verified healthy first
(0% loss on all three paths), with the mode exposed as a runtime knob so both arms
run inside ONE boot:

    echo {0,1} > /sys/module/rtl819x/parameters/wan_route_mode
    cat /proc/rtl865x_gw >/dev/null      # re-runs the SWTCR0 write

**Result — clean A/B, same boot, same transfer method:**

| WANRouteMode | CPU-seen on eth0.1 | data | ratio |
|---|---|---|---|
| 0 = Forward | 23,420 pkt | 61.2 MB | **382 CPU pkt/MB** |
| 1 = ToCpu   |  8,690 pkt | 20.3 MB | **427 CPU pkt/MB** |

Theoretical max (every packet trapped at 1500 B) ≈ 699/MB. Both modes sit around
55–61% of that, and **ToCpu is marginally WORSE, not better**. It does not engage
hardware reverse-NAPT. ⚠ Caveat on reading the absolute numbers: 61.2 MB / 23,420 =
~2.7 kB per counted packet, i.e. above MTU, so GRO coalescing is inflating "one
packet". The A/B comparison between modes is still valid — same counter, same path.

**Two corrections to the record:**
1. The original claim "ToCpu kills the WAN" is WRONG. With ToCpu set, LAN and NAT
   both measured 0% loss. It breaks nothing; it just does not help.
2. That original result was confounded twice over (WAN peer had lost its address,
   host route had reverted). Retesting it was worth doing, and the answer is a
   clean negative rather than an unknown.

**Also observed:** the sustained-download wedge reproduces under ToCpu as well —
after the download the box read 50% loss on LAN and ~66% on NAT, and needed
`echo 3 > .../fabric_reset` + gw re-arm + warm to return to 0%. So the wedge is not
a function of this bit either.

Candidates falsified so far: extIP `/32` process=2 route, dst-MAC→TOCPU ACL rule,
extIP `nextHop`, and now `WANRouteMode=ToCpu`. The knob is kept (default 0 =
Forward) so the experiment is re-runnable, not re-derivable from scratch.

**What remains for R6** is the ground truth the plan already identified: boot STOCK
from the 8 MB backup with a real NAT flow running and dump the live MSCR / SWTCR0 /
SWTCR1 / DACLRCR plus an actual inbound L4 row, then diff against what this port
programs. Every register-level guess is now exhausted; the remaining unknown is what
stock's inbound row actually looks like.

## R6 ground truth WITHOUT booting stock — the row encoding is NOT the bug

The plan said the remaining path was a live stock register dump. That turned out to
be unnecessary: the row *construction* is in the stock binary, so the encoding can be
decoded statically. It was, function by function, and diffed against this port.

**Well-evidenced negative first.** The inbound row's layout, its hash, its index, its
byte order, its aging seed and its commit protocol are **identical** between stock and
this port. Specifically confirmed:
- Row packing `rtl8651_setAsicNaptTcpUdpTable` @`0x8019e1e4` — every field matches
  `rtl819x_hwnat.c:244-292`. `collision`/`collision2` are hardwired 1 by `ori 0x4002`.
- Stock writes exactly TWO rows per flow (`rtl865x_addNaptConnection` @`0x801ae830`):
  outbound at index `out`, inbound at index `in`, with
  `offset_o = extPort>>10`, `selE_o = extPort & 0x3ff`, and inbound
  `offset_i = extPort & 0x3f`, `selIP_i = (extPort>>6) & 0xf`,
  `selE_i = very & 0x3ff` where `very = HASH(proto|2, htonl(remIp), htons(remPort),0,0)`.
  TCPFlag 3 outbound / 2 inbound. This is exactly what the port already programs.
- All hash inputs are NETWORK order; `gw_napt_hash1()` is a verbatim correct
  transcription of `rtl8651_naptTcpUdpTableIndex` @`0x8019df88`.
- There is **no** separate NAPTR table, no per-flow ACL entry, no per-entry
  inbound-enable bit, and no per-row nexthop in use (`NHIDX`/`NHIDXValid` are 0).

So four falsified candidates plus this: the row is not where the bug lives.

**What actually differs (the roadmap):**

| # | item | stock | this port |
|---|---|---|---|
| B1 ★★★ | route matching the **extIP** | WAN connected /24, `process=2 (RT_ARP)`, `internal=0`, `ARPIpIdx`=extIP row (`rtl865x_route.c:621-653`) | falls through to `[6]/[7]` `process=5, internal=0` |
| B2 ★★ | ARP entry for the **LAN host** | always resolved; NAPT rows torn down with the ARP | only ARP row 64 = the WAN peer; none for any LAN host |
| B3 ★★ | L4 table init | prefills all 1024 rows `collision=1, collision2=1, valid=0` (`0x801af5d8`) | never initialised; clear writes an all-zero row |
| B4 ★ | SWTCR1 | read-modify-write, ends ⊇ `0x2E00` (adds EnNATT2LOG 0x400, ENFRAGTOACLPT 0x800) | absolute `= 0x2200`, clearing bits 10/11 |

B1 matters because this port has **already proven on hardware** (commit `9562db2`)
that a `process=5, internal=0` route on the extIP classifies a WAN→LAN reply as
OUTBOUND, so the reverse stage never runs. The /24 was narrowed to a /32 to kill that
shadow — and `[6]`/`[7]` then re-introduced it.

**B1 implemented and tested — insufficient ALONE.** Added as a runtime knob
(`/sys/module/rtl819x/parameters/wan_connected_route`, default 0) writing the WAN
connected /24 as `process=2` at route idx3. Measured, warm, same method both arms:

    B1 off : 23,420 CPU pkt / 61.2 MB = 382 CPU pkt/MB
    B1 on  : 22,593 CPU pkt / 61.2 MB = 369 CPU pkt/MB

~3% — noise, not the collapse toward 0 that hardware reverse would produce. It does
NOT break anything (LAN and NAT both 0% with it armed), which incidentally confirms
the old "extIP /32 process=2 black-holed" note was itself confounded.

⚠ Caveat on this negative: **B2 is a stated prerequisite, not an independent item.**
A `process=2` route DROPS unless its ARP range holds a resolved entry for the
destination, and this port has no LAN-host ARP entry at all — so B1 alone cannot
produce a fully hardware-forwarded return path. Testing B1+B2 together needs the
ASIC's ARP hash, which is still undecoded (an earlier static-ARP attempt made frames
drop outright). Treat B1 as "necessary, not sufficient — retest with B2".

**Two doc corrections** from the same pass: `ASIC-ENGINE.md` §3 has
`_rtl8651_addAsicEntry` (`0x801910c8`, SWTACR=3) and `_rtl8651_forceAddAsicEntry`
(`0x80191188`, SWTACR=9) **swapped**; and `0xBB804418` is SWTCR0 (ALE_BASE+0x18) whose
bits 18/19 are `EN_STOP_TLU`/`STOP_TLU_READY`, not a generic trigger/done pair.

The full vendor SDK used as documentation is preserved at `dir842-build/sdk-rtl819x/`
(529 MB); stock disassembly dumps are in `dir842-build/ke/`.

## R6 measured properly at last — with a positive control (2026-07-30)

★ **Every earlier R6 measurement this session was invalid**, including the ones used
to falsify `WANRouteMode` and to test B1. They all ran with **`hwnat=N`** — hardware
NAT was never armed, so they measured software forwarding and every packet hit the CPU
by definition. Retracting that as a methodology error, not a hardware finding.

Redone with offload armed AND a positive control, so the metric is shown to be capable
of detecting offload before any negative is believed:

| path | CPU pkt/MB | vs software |
|---|---|---|
| software reference (`hwnat=N`) | 382 | — |
| **forward** LAN→WAN, `hwnat=Y` | **147** | **−61% — offload detected ✓** |
| **reverse** WAN→LAN, `hwnat=Y` + B1 + B2 | **355** | −7% — still CPU-bound ✗ |

The forward arm is the control: the same counter, same method, same box, shows a large
drop when offload engages. So the reverse arm's flatness is a real property of the
datapath, not an insensitive measurement.

**B1 + B2 implemented and both are insufficient.** B2 turned out to be much easier than
feared — the ARP index is **not a hash**:

    arpIndex = route->un.arp.arpsta + (ip & ~route->ipMask)     (l3Driver/rtl865x_arp.c:303)

Purely positional: raw ARP start plus the host part. For the LAN /24 at `arpsta=0`,
host `.2` lands at row 2. That same function returns FAILED unless `route->process==2`,
which independently confirms B1 is a prerequisite for any ARP resolution. Both are now
implemented (LAN ARP row 2; B1's WAN route given raw rows 256..504 so the two /24s
cannot collide on host number). Neither moved the reverse path.

**B3 tried and MEASURED HARMFUL — do not repeat in this form.** Stock's
`rtl865x_nat_init` (vendor `l4Driver/rtl865x_nat.c:176-180`) force-writes all 1024 rows
with `isCollision = isCollision2 = 1`. Applying that as the general free-row pattern —
i.e. inside `rtl865x_napt_clear()` — took the datapath to 100% loss on LAN and NAT with
no recovery by warming. Almost certainly because `napt_clear()` is ALSO the teardown
path for *active* flows, so marking freed rows "collision, keep probing" corrupts the
walk for later lookups instead of repairing it. Stock only writes that pattern at INIT,
before any flow exists, and its teardown is a different function. If retried: apply it
ONLY in a one-shot prefill, never in the per-flow clear. Both are now behind
`napt_collision_prefill` (default 0) and the tree is back to 0% on all paths.

**Remaining untried:** B4 (SWTCR1 read-modify-write; stock ends ⊇ `0x2E00`, adding
EnNATT2LOG `0x400` and ENFRAGTOACLPT `0x800`, where this port writes an absolute
`0x2200` and clears bits 10/11), and B3 done correctly as an init-only prefill.

### B4 tested — no effect. All four stock-vs-port divergences are now exhausted.

`swtcr1_trap_bits` ORs stock's `EnNATT2LOG` (bit10) and `ENFRAGTOACLPT` (bit11) into
SWTCR1, giving `0x2E00` where this port wrote an absolute `0x2200`. Bit names and
positions from the vendor header `AsicDriver/rtl865xc_asicregs.h:1583-1585`.

Measured with offload armed, alongside B1+B2, same method and positive control:

| configuration | CPU pkt/MB | vs software |
|---|---|---|
| software reference (`hwnat=N`) | 382 | — |
| **forward** control (`hwnat=Y`) | **147** | −61% ✓ |
| reverse, B1+B2 | 355 | −7% |
| reverse, **B1+B2+B4** | **360** | −6% |

No effect, as expected from first principles — both bits concern *trapping*, not the
NAPT lookup — but it was the last remaining divergence in that register, so it is now
measured rather than assumed.

### R6 final state

Every difference between stock and this port that is visible in stock's flow-creation
path has now been implemented and measured under a correct methodology:

- **row encoding, hash, index, byte order, aging, commit protocol** — proven IDENTICAL
  by decoding stock's `setAsicNaptTcpUdpTable` / `addNaptConnection` /
  `naptTcpUdpTableIndex`. Not the bug.
- **B1** (extIP's route as `process=RT_ARP`) — implemented, no effect alone.
- **B2** (LAN-host ARP entry; index is `arpsta + host-part`, not a hash) — implemented,
  no effect with B1.
- **B3** (free rows carry `collision=collision2=1`) — implemented as applied to
  `napt_clear()` and MEASURED HARMFUL (100% loss); gated off. Untried as an
  *init-only* prefill, which is how stock actually does it.
- **B4** (SWTCR1 trap bits) — implemented, no effect.

Forward offload remains healthy throughout (147 vs 382 = −61%), so nothing here
regressed the working direction.

**What that leaves.** The reverse path does not engage for a reason that is NOT
visible in the flow-creation path, since that path is now byte-equivalent to stock.
The two candidates that remain are (a) B3 done correctly — a one-shot prefill of all
1024 rows at init, before any flow exists, never touching live teardown; and (b) the
ASIC's 4-way walk semantics themselves (`SWTCR1.EnL4WayH`), which is the only
encoding-adjacent behaviour left unexamined. Both are concrete; neither is a guess.

All knobs default OFF and the tree is verified at 0% loss on LAN 56 B, LAN 1400 B and
NAT, with forward offload intact.

### B3 retried CORRECTLY (init-only) — safe, but still no effect

Reworked exactly as the previous entry prescribed: `rtl865x_napt_prefill()` now writes
the stock pattern (`collision = collision2 = 1`, `valid = 0`) directly to all 1024 rows
once at init, and `rtl865x_napt_clear()` is back to a plain all-zero row because it is
the teardown path for ACTIVE flows.

That distinction is confirmed to be the one that mattered: **the init-only form does
not break anything** (0% loss on LAN and NAT), where the same pattern applied inside
`napt_clear()` took the box to 100%. So the earlier harm was the live-teardown
application, not the pattern.

But it does not fix inbound either.

### R6 — the complete measured picture

All with offload armed, same method, same box, against the forward positive control:

| configuration | CPU pkt/MB | vs software |
|---|---|---|
| software reference (`hwnat=N`) | 382 | — |
| **forward** control (`hwnat=Y`) | **147** | **−61%** ✓ |
| reverse, B1+B2 | 355 | −7% |
| reverse, B1+B2+B4 | 360 | −6% |
| reverse, B1+B2+B3(init-only) | 361 | −6% |

Every reverse configuration lands in 355–361 — indistinguishable from each other and
from software forwarding, while the forward arm on the same counter drops by 61%.
**Hardware reverse-NAPT does not engage under any combination of the divergences that
are observable in stock's flow-creation path.**

That path is now byte-equivalent to stock (encoding, hash, index, byte order, aging
seed, commit protocol all decoded and matched), and B1, B2, B3 and B4 are each
implemented and measured. So the cause lies outside flow creation.

**The one candidate left, and a contradiction worth chasing.** `RTL865X_NAPT_ROWS` is
documented in this port as *"flat 1-way L4 table depth (SWTCR1 EnL4WayH=0)"*, yet the
code SETS `EnL4WayH` (bit 9). With that bit set the table is 4-way associative — 256
sets × 4 ways — and a 10-bit index decomposes into set+way rather than addressing a
flat 1024-entry array. The port writes rows as if the table were flat. Forward still
works because its row is found by the same hash that wrote it; the inbound
*verification* row is the one that must be located by a walk across ways, which is
exactly where a set/way-vs-flat mismatch would bite, and also why the collision bits
(B3) are implicated in the walk but insufficient on their own.

Next step for whoever picks this up: determine, from `rtl865x_asicL4.c`'s four-way
helpers in the preserved SDK (`dir842-build/sdk-rtl819x/`), how a 10-bit index maps to
(set, way) when `EnL4WayH=1`, and whether `rtl819x_hwnat.c` should be writing the
inbound row at a different physical row than it currently computes.

### The 4-way candidate is FALSIFIED too — from vendor source

The previous entry proposed that `EnL4WayH=1` makes the L4 table 4-way (256 sets ×
4 ways) so a 10-bit index should decompose into set+way, and that this port writing
rows "as if flat" was the remaining bug. **That is wrong**, settled from the vendor
source rather than by another hardware experiment:

- `EnL4WayH` is `(1 << 9)` (`AsicDriver/rtl865xc_asicregs.h:1587`) — the port's bit
  position is correct.
- `_Is4WayHashEnabled()` (`AsicDriver/rtl865x_asicL4.c:63`) exists but has **no callers
  anywhere in the SDK**. Nothing in the vendor driver ever branches on it.
- `rtl8651_setAsicNaptTcpUdpTable()` (`rtl865x_asicL4.c:227`) does **not** transform the
  index for 4-way at all. It bounds-checks `index >= RTL8651_TCPUDPTBL_SIZE` (1024) and
  writes that row directly.

So the vendor addresses the flow table as a **flat 1024-entry array regardless of the
4-way bit** — the associativity is internal to the ASIC's *lookup*, not to how software
addresses rows. This port's flat addressing is therefore already stock-identical, and
the "flat vs set/way mismatch" theory is dead. The stale comment on
`RTL865X_NAPT_ROWS` (which says `EnL4WayH=0`) is simply out of date; the code setting
bit 9 is correct and matches stock.

### R6 — where this genuinely leaves things

Everything reachable from stock's flow-creation path has now been decoded, implemented
and measured, and every candidate is exhausted:

| candidate | status |
|---|---|
| row encoding / hash / index / byte order / aging / commit | proven IDENTICAL to stock |
| extIP `/32` process=2 route | falsified (early, later shown confounded) |
| dst-MAC→TOCPU ACL rule | falsified |
| extIP `nextHop` | falsified (stock writes 0 too) |
| `SWTCR0.WANRouteMode=ToCpu` | falsified (stock uses Forward too) |
| B1 WAN connected route as `RT_ARP` | implemented — no effect |
| B2 LAN-host ARP entry (index = `arpsta + host`) | implemented — no effect |
| B3 collision prefill, init-only | implemented — no effect (harmful only if applied to teardown) |
| B4 SWTCR1 trap bits | implemented — no effect |
| 4-way set/way index mapping | **falsified from source — vendor addresses flat** |

Forward offload is healthy throughout (147 vs 382 CPU pkt/MB = −61%).

**The honest conclusion: the reason inbound does not engage is not visible anywhere in
the vendor's software path.** Software-side, this port now does what stock does. What
has never been observed is stock's *hardware* behaviour with a live inbound flow — the
one thing static analysis cannot supply. That means the original plan was right after
all about the last resort, and it is now the ONLY remaining step: boot stock from the
8 MB backup with a real NAT flow running and dump the live L4 table to see what its
inbound row actually looks like in silicon versus what we write.

⚠ That step overwrites the working OpenWrt install on NOR (stock exists only in the
backup), so it needs an explicit decision from the operator rather than being done
opportunistically. Everything short of it has been done.

## ★★★ R6 RESOLVED — by measuring STOCK on the same silicon (2026-07-30)

With the operator's go-ahead, stock firmware was flashed back onto NOR and measured
directly. **The result overturns R6's premise: stock does NOT hardware-offload the
reverse path either.**

### Method
- Backup verified first (all slices match `dir842-mtd6-ALL.bin`, md5 `35d372de…`).
- Wrote stock's kernel+rootfs region (`0x040000..0x67F000`, md5 `d3a35f39…`) to the
  firmware partition from a RAM-booted OpenWrt; flash readback verified byte-exact.
- Stock booted: `Linux version 3.10.90+`, `root=/dev/mtdblock4`, `/proc/hw_nat` = **1**
  (hardware NAT enabled).
- Bench wired to stock's own addressing: host on `192.168.1.2`, stock LAN `192.168.1.1`,
  stock WAN `eth1` = `172.16.0.1` toward tiny at `172.16.0.2`. Added
  `iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE` (stock's own WAN setup was
  bypassed by configuring `eth1` by hand, so TCP did not complete until this was added).
- Same 60 MB transfer and the same CPU-seen-packet metric used on the port.

### Result — stock, measured

| direction | CPU-seen | data | verdict |
|---|---|---|---|
| **forward** LAN→WAN | **10 packets**, ~3 KB | 63 MB | **fully hardware-offloaded** |
| **reverse** WAN→LAN | **43,529 packets** | 63 MB | **every packet through the CPU** |

691 CPU pkt/MB on the reverse path — against a theoretical maximum of ~699/MB at
1500 B MTU. Stock traps essentially every inbound packet.

### What this settles

R6 assumed the ASIC does reverse-NAPT in hardware and that this port was failing to
configure it. **That assumption is false.** The vendor firmware, on this exact silicon,
with `hw_nat=1` and its own driver, offloads **forward only**. There is no hidden
register, table or bit we were missing — there is no reverse offload to find.

> ⚠ **Read the "R6 REVISITED" section at the end of this file before acting on what
> follows.** This part is correct about *where* packets go and survived a deliberate
> falsification attempt. But the throughput conclusions drawn from it below are NOT
> supported: the test ran on a **100 Mb/s** WAN link, so it cannot bound gigabit at all.
> The live question turned out to be **CPU cost per packet**, and this port is ~6× more
> expensive than it should be (≈1 packet per NAPI poll).

This retroactively explains every negative in this investigation and vindicates them:
the encoding was proven identical to stock because it *is* identical; the four
falsified candidates were falsified because none of them could have worked; B1–B4 had
no effect because the capability they were meant to unlock does not exist.

For reference, this port versus stock on the same bench:

| | stock | this port |
|---|---|---|
| forward | ~0.2 CPU pkt/MB | 147 CPU pkt/MB |
| reverse | 691 CPU pkt/MB | 355 CPU pkt/MB |

⚠ Read those across a row, not down a column — stock is kernel 3.10 without GRO, so its
packet counts are raw wire packets, while 4.14 coalesces. The row comparison is what
matters: **both offload forward and neither offloads reverse.**

**One genuine gap this does expose:** stock's forward offload is far more complete
(~0.2 vs 147 CPU pkt/MB). That is a real, measurable improvement available to this
port, and unlike reverse-NAPT it is demonstrably achievable on this hardware — a much
better target than the one R6 was chasing.

### Bench restored
Stock removed, OpenWrt factory image written back and byte-verified, box boots from
NOR, and RAM-boot verified at 0% loss on LAN 56 B, LAN 1400 B and NAT.
⚠ Note for future reflashes: write the **factory** image, not `sysupgrade` — the loader
rejects the latter with `magic not found!!!` (it lacks the D-Link boot magic from M7.1).

---

## ★★★ R6 REVISITED (2026-07-30, later): "stock does not offload" was measured
## correctly but OVER-GENERALISED — the gigabit question is a CPU-COST question

Prompted by the operator's objection: *"if stock does not offload, how was I able to
download at gigabit speed on stock?"* That is a fair challenge and it exposes a real
error of reasoning in the section above.

### 1. What the stock measurement does and does not establish

It establishes **where** packets go (CPU, not ASIC). It says **nothing** about **how
fast** they can go — and the section above slid from the first to the second when it
implied there was nothing left worth chasing.

⚠ The stock test ran over the bench WAN link, which negotiates **100 Mb/s** (documented
at line 169 of this file). At ~94 Mbit the CPU sees ~7,800 pkt/s, which is a trivial
load. **Gigabit was never observable on this bench**, so the measurement cannot
contradict a gigabit field observation.

### 2. An attempt to falsify the stock result — FAILED, so the result stands

Hypothesis: stock's WAN was configured **by hand** (`ifconfig eth1` + a hand-written
`iptables MASQUERADE`), bypassing stock's own config path, so the ASIC's **extIP table**
was never programmed — and inbound classification needs it. That would make the trap an
artifact of the bench, not a property of the silicon.

**Refuted by the vendor source.** `rtl865x_nat.c:678`, first statement of
`rtl865x_addNaptConnection()`:

    /* Make sure natip */
    retval = rtl865x_getIpIdxByExtIp(htonl(naptEntry->extIp), &ipidx);
    if(retval != SUCCESS)
        return RTL_EINVALIDINPUT;

Adding a NAPT mapping **hard-fails** if the external IP is absent from the ASIC IP
table — no row is added in *either* direction. Stock's forward path was **fully
offloaded** (10 CPU packets for 63 MB), so rows *were* being added, so the lookup
succeeded, so **the extIP table was populated**. The bench did not cripple the test.

(Also checked and dead: `/proc/localPublic`, the one path that calls `rtl865x_addIp`,
is written by **no file in the stock rootfs** — verified with positive controls — and
the `localPublic` symbol is absent from the stock kernel. So that is not the mechanism
either.)

**⇒ Stock genuinely traps every inbound packet. The R6 finding is sound.**

### 3. The reconciliation: trapping is only slow if the trap is EXPENSIVE

Both statements can be true at once — *stock traps every download packet* and *stock
downloads fast* — provided the per-packet CPU cost is low. So the real question was
never "hardware or not", it is **cost per packet**. Measured today, on this port:

| run | load | result |
|---|---|---|
| bounded 20 MB download (WAN→LAN, `iperf3 -R -n 20M`) | 83.8 Mbit/s, 2.00 s | **107 jiffies busy (2 sys + 105 softirq) = 1.07 s CPU ⇒ 53.5% of one core** |
| 30 s download (line rate while healthy) | ~94 Mbit/s | 295 jiffies busy over the window; same ballpark (~59%) |

`HZ=100` confirmed self-consistently (602 jiffies over a ~6 s window).

**Extrapolation:** 53.5% CPU at 83.8 Mbit ⇒ 0.64 %CPU per Mbit ⇒ **~600% of one core at
940 Mbit**. This port's CPU-forwarded download path therefore saturates at roughly
**155–160 Mbit/s**. Gigabit is ~6× out of reach *for this port*.

Since stock traps the same packets on the same silicon and (per the operator) does reach
gigabit, **stock's trapped path must be several times cheaper per packet than ours.**

### 4. Root cause of our per-packet cost: the RX path does ~1 packet per NAPI poll

Straight from the driver's own instrumentation during a line-rate download:

    poll#56320  t=284.633  rx_pkts=45745
    poll#62464  t=285.296  rx_pkts=52752
    -> 6,144 polls for 7,007 packets = 1.14 packets per NAPI poll

and every trace line reads `rx_done=0` or `rx_done=1` — **each poll returns after at
most one packet.** A healthy NAPI drains 8–64 per poll. We are paying full poll/IRQ
overhead per packet, which is exactly the kind of constant factor that explains a ~6×
gap against a vendor driver doing the same logical work.

### 5. What this changes

- ✗ **Wrong target:** "find the missing hardware reverse-NAPT bit." It does not exist —
  section above proved that, and this section did not disturb it.
- ✓ **Right target:** make the CPU path cheap. Fix RX batching first (≤1 packet/poll is
  a bug, not a tuning knob). That is a pure-software win, needs no ASIC secrets, and is
  worth up to ~6× on the download path.
- The earlier note that stock's forward offload is far more complete (~0.2 vs 147 CPU
  pkt/MB) still stands as a second, independent win.

### 6. ⚠ R2 IS NOT CLOSED — the wedge reproduces under forwarded load

While measuring the above, a sustained 30 s forwarded download **wedged the entire
datapath**: host→box *and* box→tiny both went to 100% loss, while the CPU stayed
responsive on serial. All three bench guards were verified intact at the time (host
IPv4, host route, tiny's `br0` address), so this is not confound #4/#5.

- `runout=0` and `USEDDSC` 18–28 throughout — **not** descriptor exhaustion, so this is
  a different failure from the classic large-frame RX wedge.
- **`fabric_reset=3` did NOT recover it.** The reset ran to completion and reprogrammed
  everything (`recovery level 3 complete`, `netif readback PASS`) and the path stayed
  dead. Only a power-cycle/RAM-boot recovered it.

R2's gate passed on **box-terminating ICMP**, and its own write-up flagged the coverage
gap ("not bidirectional *forwarded* saturation ... that scenario is still untested").
That untested scenario is the one that fails. **R2 should be reopened.**

---

## R6 FINAL (2026-07-30, later still) — a gigabit-capable bench, the real ceiling
## measured, and TWO of my own earlier claims retracted

The 100 Mb WAN cable was replaced. The replacement did not link (box: all jacks
`link:down` except the host's port 2; tiny: `eth0` `<NO-CARRIER>`, `speed=-1`), so
the bench was rebuilt to **not need the WAN peer at all**.

### The new bench: emulate the WAN peer on the gigabit LAN port

`swconfig dev switch0 vlan 1 set ports "4 2t 6t"` adds the host's jack (port 2) to
the **WAN VLAN as tagged**, alongside the real WAN jack. The host then runs a
802.1Q vid-1 subinterface inside a network namespace holding `172.16.0.2/24`:

    ip link add link <ifP> name wanp type vlan id 1
    ip netns add benchns && ip link set wanp netns benchns
    ip -n benchns addr add 172.16.0.2/24 dev wanp
    ip -n benchns route add default via 172.16.0.1

Root ns keeps `192.168.0.2` (LAN). Traffic between them is forced onto the wire by
the namespace boundary and is **routed and NATed by the box exactly as before** —
same subnets, same zones, same NAT — but now over a **1000baseT** link.

★ Deliberately reuses **vid 1**, not a new vid. The SoC's internal switch has VLAN
entries for vid 1 and 2 only (`sw_add_vlan`); a vid 3 was tried first and TX was
silently dropped because `ph_vlanId` must name a VID whose SoC member mask covers
the portlist (the constraint already documented at `rtl819x-eth.c:1134`). Adding a
third VLAN needs a driver change; reusing vid 1 needs none.

Caveat: every byte crosses the host NIC twice, so the wire carries ~2x the payload.
The NIC is r8152 on **USB3 (5000M)**, and the box saturates its CPU well below that,
so the host is not the limiter — confirmed by the box hitting ~96% CPU.

### The ceiling, measured directly (not extrapolated)

| config | throughput | CPU | cost |
|---|---|---|---|
| baseline | **147 Mbit/s** | **96%** | 54.7 ms CPU/MB |
| + software flow offload | **181 Mbit/s** | **90%** | 40.7 ms CPU/MB |

10 s `iperf3 -R`, CPU from `/proc/stat` on the box, `HZ=100`. The baseline
**confirms the 155-160 Mbit prediction** extrapolated earlier from the 94 Mbit
data — the extrapolation method was sound.

Flow offload is now shipped on by default (`uci-defaults/10_flow-offload-dir842`).
It is a genuine +23%, but note what it means: removing the entire conntrack and
iptables traversal bought only 25% per byte, so **netfilter was never the dominant
cost**. Gigabit needs ~8 ms CPU/MB; we are at 40.7. The remaining ~5x is in the
driver datapath — DMA cache maintenance and uncached descriptor/register reads —
and the RX path is already zero-copy (skb swap + `dma_unmap_single`, no memcpy),
so there is no easy copy to delete. That is the next real project.

### ✗ RETRACTION 1 — "R2 is not closed / the download wedges the box"

Withdrawn as a confirmed defect. **60 s of sustained forwarded download at 147
Mbit/s (1.03 GB, CPU pegged at 96%) ran clean, and the path was fully alive
afterwards** — a heavier load than the 94 Mbit run that "wedged".

The earlier stall has a physical confound I did not rule out: it happened while the
operator was at the bench changing cables, and although I verified the three
software guards (host IPv4, host route, tiny's `br0` address) I never checked
**link state**. A cable pulled mid-transfer explains the stall, both directions
dying, and why `fabric_reset=3` could not fix it. That does not prove the wedge
isn't real — it reproduces ~1 in 3-4 heavy attempts historically — but this
instance is not evidence for it. R2's status returns to what its own gate said:
passed on box-terminating load, forwarded saturation now also clean for 60 s.

### ✗ RETRACTION 2 — "~1 packet per NAPI poll is a bug"

Withdrawn. `rtl819x_eth_poll()` is a correct `while (rx_done < budget)` drain that
breaks on an empty ring. Getting ~1.14 packets per poll at 94 Mbit means the CPU
was **keeping up** — batching only appears once napi falls behind. It is a symptom
of arrival rate, not a defect. The per-packet *cost* is the real finding, and it
stands on the jiffie accounting, not on the poll ratio.

---

## ★★★ GIGABIT WAN LINK — the download is CPU-bound, not cable-bound (2026-07-30)

The WAN cable was replaced and now links at **`port:4 link:up speed:1000baseT
full-duplex`** (tiny: `carrier=1`, `speed=1000`). Every throughput caveat in this
file about the 100 Mb link is now lifted, and the numbers can be taken at face value.

Measured on the real two-port topology, `HWNAT=Y`, 10 s `iperf3`, CPU from
`/proc/stat` on the box (`HZ=100`):

| direction | throughput | CPU | CPU-seen |
|---|---|---|---|
| **download** (WAN→LAN, no HW offload exists) | **173 Mbit/s** | **86%** | 93 pkt/MB |
| **upload** (LAN→WAN, HW offload *should* apply) | **171 Mbit/s** | **92%** | 191 pkt/MB |

### What this settles

**The 100 Mb cable was never what limited downloads — the CPU is.** With gigabit on
both sides the download lands at 173 Mbit/s with the CPU at 86%. This confirms, by
direct measurement on a gigabit link, the 155–160 Mbit ceiling that was extrapolated
from 94 Mbit data and then measured at 147 Mbit on the netns bench. Three independent
routes, same answer.

So the operator's question — *"if stock does not offload, how did I download at
gigabit on stock?"* — has this answer: **not because stock offloads the reverse path
(it does not; that was verified by booting stock), but because stock's software path
must be several times cheaper per packet than ours.** Our side of that comparison is
now pinned down on real gigabit hardware. Stock's side is still an inference — the
decisive test is to reflash stock and sample its CPU during a download on this same
gigabit link, which is now finally possible.

### ⚠ NEW REGRESSION: hardware forward offload is NOT engaging

Upload shows **191 CPU pkt/MB** — the CPU is seeing every packet (GRO coalesces to
~5.2 KB/segment). That contradicts session 2's measured "LAN→WAN is hardware
offloaded, ~90% of data packets bypass the CPU" and stock's ~0.2 CPU pkt/MB.

State at the time: `hwnat=Y`, `cat /proc/rtl865x_gw` → `RESULT PASS (gateway datapath
LIVE in ASIC; MSCR=00000017)`, and rows **are** being programmed (distinct outbound
rows 146/162/165/175/240/660/671/708/757 seen). But the aging instrument reads

    hwnat AGE out[708]=0 in[65]=0

**static at zero** — and a pinned/advancing age is precisely the "ASIC is forwarding
this row" signal this port relies on. Rows exist; the ASIC is not matching them.
That is the next target, and it is worth more than anything else on the list: stock
proves ~0.2 CPU pkt/MB is achievable on this silicon, so restoring it should take
uploads from ~170 Mbit toward line rate.

Not the cause (checked): software flow offload does not steal the flows — the CPU sees
every packet with it both enabled and disabled.

### Bench notes

- `bench-wan-emu.sh` (netns WAN peer on a tagged vid-1 subinterface of the LAN jack)
  remains useful when no second machine is available, but ⚠ **it cannot exercise the
  hardware forward offload**: with ingress and egress on the same physical port the
  ASIC will not hardware-forward, measured as `HWNAT=Y` 167 Mbit @ 131 pkt/MB versus
  `HWNAT=N` 170 Mbit @ 143 pkt/MB — no difference. It measures the *software* path
  only, which is exactly right for the download direction and wrong for upload.
- `HWNAT` is **N after a cold boot** (deliberate — see the DTS `chosen` comment) and
  must be armed after `gw_prog`. Any throughput number must state its hwnat state;
  two measurements in this session were initially confounded by not doing so.
- Software flow offload A/B, same boot, same topology: **147 → 181 Mbit/s**. A later
  171-vs-144 pair is NOT a valid A/B — the `iptables -D` did not match the fw3 rule
  (it carries `-m comment`), so both runs had offload active.

### ★ Measure offload by BYTES, not packet counts — the old "~90% offloaded" is suspect

Today's upload, real two-port, `HWNAT=Y`:

    eth0.2 rx bytes through the CPU : 215.4 MB
    iperf3 payload transferred      : 203.0 MB   -> 106% (>100% = headers)
    avg bytes per CPU-seen packet   : 5242       -> >1518, so GRO-COALESCED

**Every byte of the upload went through the CPU.** Unambiguous, because bytes cannot
be coalesced away the way packets can.

This is why the packet-count method misleads here. Session 2 concluded "LAN→WAN is
hardware offloaded, ~90% of data packets bypass the CPU" from `eth0.2` rx delta 5670
against ~54,000 data packets. But 5,670 CPU-seen segments over that ~79 MB flow is
~13,900 bytes per segment — those were **GRO super-packets**, not wire packets.
Comparing coalesced CPU segments against wire packets makes any CPU-forwarded flow
look ~90% offloaded. By the byte test, that flow's data also went through the CPU.

⇒ The forward-offload claim needs re-verification by bytes before it is trusted, and
the same applies to the `147 CPU pkt/MB` vs stock `0.2 CPU pkt/MB` comparison (stock
ran kernel 3.10 **without GRO**, so its packet counts are raw wire packets while ours
are coalesced — the two columns were never commensurable, as that table's own footnote
half-suspected). Stock's 0.2 pkt/MB remains meaningful on its own terms: at ~700 wire
pkt/MB, seeing 0.2 means stock genuinely offloaded essentially everything.

---

## Hardware-offload goal: one real bug fixed, and the blocker narrowed to a paradox

### ✅ Fixed: the LAN client identity was compiled in

`gw_prog` programmed the ASIC's LAN egress chain from **build-time constants** —
`GW_MAC_HALLAN` (54:bf:64:18:b8:de, one bench machine's NIC) and the /32 route
`0xC0A80002`. The WAN peer was already learned at runtime from the flow-offload dest
path; the LAN side never was. On any other network the L2 entry and host route the
reply direction egresses through name a MAC/IP that does not exist, so **no LAN
client could ever be reached in hardware**. It had already rotted here too: after the
rig was rewired from hal's `enp3s0` to a USB adapter, the ASIC kept pointing at the
departed MAC while the live client was `00:e0:4c:12:59:90`.

Fix: `rtl865x_lan_set_nexthop(mac, ip)` mirroring `rtl865x_wan_set_nexthop` exactly
(shadow-compared, returns 1 on change so the caller flushes rows programmed under the
old identity), fed from `src->eth_dest` + the flow's internal IP in
`hwnat_add_flow()`. Verified: `gw_prog` now reports `L2[hal] = 00:e0:4c:12:59:90`.
Commit `a420c7f`.

⚠ Single-client shadow (matching the single /32 slot). Several active LAN clients
will thrash it — each reprograms and flushes, so flows re-offload continuously and
fall back to software. General fix: drive route[2]'s ARP window (ARPStart=0/
ARPEnd=31) with one ARP row per client.

### ✗ It did not restore forwarding — and the remaining state is a paradox

Upload after the fix: **177 Mbit/s, 223.5 MB through the CPU for a 211 MB payload =
106%**, i.e. everything. Measured by bytes, which GRO cannot hide.

The paradox, stated exactly, because it constrains the answer:

- **The ASIC IS matching the L4 rows.** `RTL865X_NAPT_AGING_RELOAD = 0x11 = 17`, and
  during traffic every row sits pinned at `age=17` — the reload ceiling — decaying to
  0 only when idle. A pinned age *is* the "silicon reloaded this row on a hit" signal.
  Software re-learn cannot explain it: the flowtable re-offers at ~30 s GC while a
  tick is ~6 s (≈102 s full decay), so an unhit row would visibly fall during a 25 s
  transfer. It does not.
- **Yet every byte still reaches the CPU.**

So the L4 NAPT lookup hits and reloads, and the packet is *still* delivered to the
CPU. Everything else checks out on readback: `MSCR=0x17` (EN_IN_ACL on),
`SWTCR1=0x2200` (L4EnHash1 + EnL4WayH), `DACLRCR=1fbf4180`, `ACL[0] w7=0x07000000`
catch-all permit, netif ACL ranges LAN[0..3]/WAN[4..6], `ROUTE0` = tiny/32 process=5
NAPT-NextHop, `NH0` → ARP[64] → `L2[tiny]` with the correct peer MAC, and `EXTIP0`
holding `htonl(172.16.0.1)` (the byte-reversed readback word `010010ac` is correct —
network order is deliberate, vendor `nat.c:690`; checked, not a bug).

Also ruled out this pass: the blanket `dst-MAC==WAN-MAC → TOCPU` ACL rule is already
gone (`rtl865x_asichal.c:820`, R6); software flow offload is not stealing the flows
(every byte hits the CPU with it both on and off); and the WAN nexthop is not stale
(re-running `gw_prog` against the live peer changed nothing).

### Next: the stock dump, which is now the decisive experiment

This is exactly the ground truth this file named at the start — with the operator's
go-ahead to reflash. Boot stock, establish a real NAT flow, and dump, **during the
live flow**: the ACL table (raw 11-word rows, all of 0..6), MSCR/SWTCR0/SWTCR1/
DACLRCR, the L3 route + nexthop + ARP + L2 chain, EXTIP, and a live NAPT row pair
with its aging. Then diff against the readback above. The question it answers is
narrow and well-posed: *given a matching L4 row, what makes stock's silicon forward
where ours traps?* Everything cheap has now been eliminated.

---

# ★★★★ STOCK GROUND TRUTH (2026-07-31) — R6's CENTRAL CONCLUSION IS OVERTURNED

Stock was reflashed onto NOR (backup verified, `mtd write`, flash readback
`d3a35f39…` byte-exact) and measured on the **gigabit** bench, then OpenWrt was
restored. This is the experiment this document has called decisive since session 1.

## The measurement that changes everything

| firmware | direction | throughput | CPU busy (10 s transfer) |
|---|---|---|---|
| **stock** | upload LAN→WAN | **913 Mbit/s** | **9 jiffies = 0.09 s ≈ 0.9%** |
| **stock** | download WAN→LAN | **923 Mbit/s** | **16 jiffies = 0.16 s ≈ 1.6%** |
| this port | upload | 177 Mbit/s | 94% |
| this port | download | 173 Mbit/s | 86% |

**Stock hardware-offloads BOTH directions at ~920 Mbit/s with the CPU essentially
idle.** Not just forward — *download too*.

## ✗ RETRACTED: "stock does not offload the reverse path; there is no reverse
## offload to find"

That conclusion (and everything built on it) is **wrong**. It came from counting
packets on stock's `eth1` and seeing 43,529 for a 63 MB download — "every packet
trapped".

The error: **stock's netdev counters include ASIC-forwarded frames.** Proven
directly here — during the 913 Mbit upload, `br0` rx incremented by the *full*
1,183 MB payload while the CPU logged **9 jiffies** of work. Bytes cannot flow
through a CPU that is 99% idle. So on stock, `/proc/net/dev` counts what the switch
forwards, not what the CPU touched, and every "stock traps everything" measurement in
this file is void.

⇒ **CPU time is the only valid cross-firmware metric here.** Packet *and* byte
counters mean different things in the two firmwares (ours count CPU deliveries in the
NAPI poll; stock's count switch traffic).

⇒ **Reverse-NAPT in hardware is real and achievable on this silicon.** The operator's
"I downloaded at gigabit on stock" was correct all along, and the R6 time-box was
abandoned on a false negative.

## Stock's configuration — the blueprint to copy

`/proc/rtl865x/l3` (live, NAT flow running):

    [0] 192.168.0.0/24 process(ARP)    LAN  dvidx(0) ARPSTA(0)   ARPEND(248) IPIDX(0)
    [1] 172.16.0.0/24  process(ARP)    WAN  dvidx(1) ARPSTA(256) ARPEND(504) IPIDX(0)
    [7] 0.0.0.0/0      process(NxtHop) WAN  NHSTA(0) NHNUM(2) NHNXT(0) NHALGO(2) IPDOMAIN(6)

`/proc/rtl865x/arp`:

    192.168.0.2  00-e0-4c-12-59-90  ARP:  2  L2:119     (LAN window 0 + host octet 2)
    172.16.0.2   e4-5f-01-04-98-af  ARP:258  L2:134     (WAN window 256 + host octet 2)

`/proc/rtl865x/ip`:

    [0] intip(0.0.0.0) extip(172.16.0.1) type(NAPT) nhIdx(0)      <- everything else Invalid

`/proc/rtl865x/nexthop`:

    [0],[1] type(ethernet) IPIdx(0) dstVid(1) pppoeIdx(0) nextHop([5:0]20)

`/proc/rtl865x/napt` (live, offloaded flow):

    [212] 192.168.0.2:58128 {V,D}={1,0} col1(1) col2(1) static(1) tcp(1)
          age(129) offset(57344) tcpflag(3) SelEIdx(784) SelIPIdx(0) NHIDXValid(0)
    [250] 192.168.0.2:58128 ... age(129) offset(16384) tcpflag(2) SelEIdx(312) SelIPIdx(12)

## The structural diff — why ours traps

| | stock | this port |
|---|---|---|
| LAN route | `/24` **process(ARP)**, ARP window 0..248 | `192.168.0.2/32` process=1 (+ /24 ARP fallback) |
| WAN route | `/24` **process(ARP)**, ARP window 256..504 | `172.16.0.2/32` **process=5 (NAPT NxtHop)** |
| default | `0.0.0.0/0` process(NxtHop), NHNUM=2, **IPDOMAIN(6)** | IPDomain=0 |
| host resolution | **dynamic ARP rows** (window base + host octet) | hardcoded L2 entries + `/32` routes |
| NAPT trigger | **the extIP entry's `type(NAPT)`** | the route's `process=5` |
| nexthop→ARP idx | **20** (field printed `[5:0]`, i.e. 6 bits) | **64** — does not fit 6 bits |
| NAPT row age | 129 (0x81) | 17 (0x11 = our `AGING_RELOAD`) |

Two findings stand out:

1. **Stock resolves every host through the subnet route's ARP window** — no `/32`
   host routes at all. This is why our port needed a compiled-in client MAC, and it
   is the general fix already flagged in `a420c7f`. `ARP row = window base + host
   octet` (LAN 0+2=2, WAN 256+2=258), exactly the deterministic mapping our own
   comments predicted.
2. **NAPT is driven by the extIP table entry (`type(NAPT)`), not by `process=5`.**
   Stock's WAN subnet route is a plain process(ARP) route and it still NATs. Our M7
   conclusion that "only NxtHop routes engage the NAPT stage" looks wrong, and the
   `/32 process=5` design built on it is the likely reason our L4 rows match (aging
   reloads) while the packet is still delivered to the CPU: egress resolution never
   completes. The `nextHop=64` overflow of a 6-bit ARP-index field is a concrete
   candidate for the same failure.

## Next

Restructure `gw_prog` to stock's shape: subnet routes with process(ARP) + populated
ARP windows fed from the kernel neigh table, extIP `type(NAPT)` as the NAT trigger,
a default NxtHop route with IPDOMAIN=6, and an ARP index that fits 6 bits. That
deletes the hardcoded-peer design entirely and is now a *transcription* job against a
known-good reference rather than a search.

---

## Stock's netif + L2 tables — and the architectural verdict

The two tables missing from the first stock run, captured during a live offloaded flow.

`/proc/rtl865x/netif`:

    [0] VID[2] e0:1c:fc:51:c9:ef  Routing enabled
        ingress ACL 0-3,  egress ACL 253-253,  MTU 1500
        Untag member ports: 0 1 2 3     Active member ports: 0 1 2 3 8
    [1] VID[1] e0:1c:fc:51:c9:ee  Routing enabled
        ingress ACL 4-6,  egress ACL 253-253,  MTU 1500
        Untag member ports: 4           Active member ports: 4

`/proc/rtl865x/l2`:

    [119,0] 00:e0:4c:12:59:90 FID:0 mbr(2 ) FWD DYN     LAN client -> its jack
    [134,0] e4:5f:01:04:98:af FID:1 mbr(4 ) FWD DYN     WAN peer   -> its jack
    [119,1] e0:1c:fc:51:c9:ef FID:0 mbr(8 ) FWD DYN     router's own MAC -> CPU port 8
    [  0,0] ff:ff:ff:ff:ff:ff FID:0 mbr(0 1 2 3 4 8) CPU STA NH
    [  5,0] 00:00:0a:00:00:0f FID:0 mbr()            CPU STA NH

### Everything cheap has now been tried, and failed

| change | result |
|---|---|
| LAN client identity learned per-flow (`a420c7f`) | correct + necessary; **no offload** |
| stock route/ARP architecture, no /32s (`5d39676`) | NAT still works with zero /32 routes — **confirms NAT is driven by extIP `type(NAPT)`, not `process=5`** — but **no offload** |
| nexthop → ARP index 64 → 20 (6-bit safe) | **no offload** |
| default route `IPDOMAIN` 0 → 6 | **no offload** |
| single-port L2 egress mask 0x3f → 0x01 (`d04879c`) | **no offload** |

Upload sat at 172–177 Mbit/s and ~93% CPU throughout, with ~106% of payload bytes
crossing the CPU every time.

### The verdict: it is the switch model, not a register

Stock's SoC switch addresses the **physical jacks directly** — LAN netif members
`0 1 2 3` + CPU `8`, WAN netif member `4` — and its L2 entries name one real jack per
host. That only works because the RTL8367S is run as a **cascade / port extender**, so
the SoC's L2/L3 engine has a genuine per-jack egress port to commit a routed unicast to.

This port's **Fork A** model does the opposite: every jack lives behind the RGMII trunk,
the CPU is port 6, and the jack is selected by VLAN ID after the frame has left the SoC.
For hardware L3 forwarding the ASIC must resolve a destination to a single egress port;
under Fork A that port is only ever the trunk, and delivery then depends on tagging the
*external* switch acts on afterwards — which is fine for CPU-forwarded frames (they are
re-injected with the right VID) and evidently is not a completable hardware path.

⇒ **Hardware offload on this port requires bringing up the 8367S cascade so the SoC
sees per-jack ports (stock's model), not another table or register fix.** That is
M4-scale work — comparable to the original ethernet carve — and it is now a
well-specified target rather than a search, because stock's exact end state is
recorded above.

The good news from this session stands: **the capability is real** (stock does 913/923
Mbit/s at ~1% CPU in both directions), the reverse path is not the wall R6 believed,
and two genuine correctness bugs were removed along the way.
