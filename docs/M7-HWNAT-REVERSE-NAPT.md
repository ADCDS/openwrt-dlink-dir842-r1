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
