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
