# Hardware NAT offload on the RTL8197F — how it works and how it was solved

**Status:** ✅ SOLVED and shipping. The RTL8197F switch core routes *and* source-NATs in
silicon at gigabit line rate under mainline OpenWrt: **891 Mbit up / 896 Mbit down with
0.0 % of payload bytes crossing the CPU**, CPU ~0.3 % busy. Believed to be the first
working mainline OpenWrt `rtl865x` ASIC L3/L4 offload.

This document is the authoritative account: the measured result, the datapath, the two
root causes (the RTL8197F keys and hashes on **numeric** values, and **Fork A could never
offload by construction**), the six prerequisite fixes, and how to reproduce it.
[`M7-HWNAT-REVERSE-NAPT.md`](M7-HWNAT-REVERSE-NAPT.md) describes this as an unsolved
paradox with "two remaining blockers" — **that framing is dead and superseded here**; that
file is retained only as the unsanitised chronological investigation log.

---

## 1. Result (measured)

All numbers from the two-port gigabit bench (one host on a LAN jack, one Pi on the WAN
jack — see [`BENCH.md`](BENCH.md)). Every run reports three things together: throughput,
**payload bytes through the CPU**, and CPU busy.

**Final gate — cold NOR boot, commit `86880757ed` (2026-07-31 20:28):**

| | throughput (2 samples) | payload bytes through CPU | CPU busy |
|---|---|---|---|
| `hwnat=0` (software) | UP 184 / 184 · DOWN 187 / 183 Mbit | ~100.6 % | ~52 % |
| **`hwnat=1` (ASIC)** | **UP 889 / 892 · DOWN 896 / 895 Mbit** | **0.0 %** | **0.3–0.7 %** |

The README quotes the rounded pair **891 up / 896 down at 0.0 % / CPU ~0.3 %**.

**The moment both directions landed — `59e2d2cd27` (2026-07-31 14:11), "HARDWARE NAT
OFFLOAD COMPLETE, both directions":**

| direction | throughput | CPU-side rx | payload | through-CPU |
|---|---|---|---|---|
| UPLOAD (LAN→WAN) | 890 Mbit | `eth0.2` rx = **1051 B** | 1,112,500,000 B | **0.0 %** |
| DOWNLOAD (WAN→LAN) | 900 Mbit | `eth0.1` rx = **270 B** | 1,125,000,000 B | **0.0 %** |

1 KB and 270 bytes crossed the CPU while **2.2 GB crossed the router**. Gate criteria:
throughput ≥ 850 → 890/900 PASS; bytes-via-CPU < 10 % → 0.0 % / 0.0 % PASS.

**Re-verifications after consolidation:**

| commit | time | result |
|---|---|---|
| `3ea4906a25` | 15:03 | 887 / 911 Mbit @ 0.0 % — after the runtime byte-order knobs were **deleted** |
| `26964573d5` | 15:23 | 890 / 911 Mbit @ 0.0 % — with **no manual configuration at all** |

**Reference — stock D-Link, same board, same bench:** 913 Mbit up @ ~0.9 % CPU,
923 Mbit down @ ~1.6 % CPU. This port therefore reaches **~97 % of stock throughput**.

**Full regression gate on a NOR cold boot with wired + BOTH radios up + offload in one
boot** (never achieved together before):

```
hwnat=0   UP 186   DOWN 175 Mbit   through-CPU ~100.6%   cpu 44-52%
hwnat=1   UP 871-891  DOWN 811-906 Mbit
          through-CPU 0.0%  (264-500 bytes of ~1.1 GB)   cpu 0.1-0.3%
zero rx/tx errors or drops on eth0, eth0.1, eth0.2
```

### ★ Throughput alone is not a proxy for offload

Commit `f50362be64` measured **812 Mbit up / 371 Mbit down on the same offloaded boot,
both at 0.0 % through-CPU**. Throughput varied by more than 2× under WiFi/TCP contention
while the offload state was byte-for-byte identical. Report throughput, through-CPU %, and
CPU-busy **from the same run**, or the number means nothing.

Download is genuinely noisy: **681–906 Mbit, 1200–2500 TCP retransmits per 10 s run**,
with the box at 0.1–0.3 % CPU and zero interface errors. Take a range, never one sample.
**The loss source has never been identified.**

---

## 2. How the datapath works: conntrack → ASIC NAPT rows

Two files carry the whole mechanism.

**`rtl865x_asichal.c`** — a clean re-implementation of the ASIC table engine (netif /
route / nexthop / ARP / L2 / NAPT / extIP), reverse engineered from the stock 3.10 kernel
and cross-checked against the vendor SDK. It exposes a datapath programmer at
`/proc/rtl865x_gw` ("gw_prog"), plus `/proc/rtl865x_napt` and `/proc/rtl865x_fabric`
(`rtl865x_asichal.c:1620-1622`).

**`rtl819x_hwnat.c`** — Linux conntrack → per-flow ASIC NAPT rows through the downstream
`ndo_flow_offload` interface. Per flow it installs **two rows** (outbound + inbound),
tracks them in a shadow bitmap, and runs an aging worker with a lost-DEL reaper. A NAPT
miss **traps to the CPU**, so software forwarding is always the fallback — which is what
makes running with offload armed safe.

**Forwarding chain:** `route(process=5) → nexthop → ARP → L2`.

> ★ **Retracted:** an earlier internal recipe claimed "route → nexthop → L2 bypasses ARP".
> That is **wrong**. The live stock dump shows *both* indirections, and pointing the
> nexthop straight at an L2 index makes the ASIC walk a garbage ARP slot and **hang the
> fabric** on any frame (watchdog reset, no panic). The rule that came out of it: **trust
> the dump over the recipe.**

Runtime toggle:

```sh
echo 1 > /sys/module/rtl819x/parameters/hwnat
```

The **module parameter** defaults to off (`rtl819x_hwnat.c:69-72`,
`"0=off (software fastpath only, default)"`), but **since R4 (2026-08-16) the shipped
image arms it at boot**: `files/target/linux/realtek/base-files/etc/init.d/dir842-asic`
(`START=97`) does MAC-poll → `swconfig apply` → `fabric_reset=3` → `gw_prog` → L2 warm →
`echo 1 > /sys/module/rtl819x/parameters/hwnat` as its last step. Arming before the tables
are warm kills the datapath, which is why it is last and why it is the service's job
rather than a boot arg. To run on the software path instead:
`echo 0 > /sys/module/rtl819x/parameters/hwnat` (or `/etc/init.d/dir842-asic stop`).

*(Earlier revisions of this document said offload was deliberately off at boot. That was
true before R4 — the reverse-NAPT path still trapped to the CPU then and a sustained
download could wedge the box. Both of those are fixed.)*

The whole M6.6 engine landed squashed in `d150b24606` (alias `f096f5d`, 2026-07-16; public
mirror `2d9ef4a`).

---

## 3. The two things that had to be true

Offload required **two independent conditions**, and until *both* held the result was
indistinguishable from "offload does not work on this chip":

1. **CPU-tag / port0-router switch mode** — so a routed unicast has a distinct egress port
   the ASIC can commit to. Under the old "Fork A" trunk model the ingress port and the
   egress port were the same port for every routed flow, and the ASIC does not
   hardware-forward a same-port hairpin. §5.
2. **Numeric (host-order) byte order everywhere** — key, row index, G encoding, and the
   inbound verification hash, all numeric **together**. §4.

Either one alone yields zero measurable improvement, which is exactly why the project
spent eleven days unable to distinguish "close" from "hopeless".

---

## 4. ★ Root cause 1: the RTL8197F keys and hashes on NUMERIC values, not on-wire network order

Commit `8d315c331b` (2026-07-31 13:58). **This is the single most important fact in the
repo, and the older docs state it backwards.**

The premise that held for eleven days — introduced by `fd2cf06a07` (alias `85f01c9`,
2026-07-20) and asserted at `docs/M7-HWNAT-REVERSE-NAPT.md` (the "all hash inputs are NETWORK order" passages) — was:

> "the ASIC hashes/keys the ON-WIRE (network-order) header fields"

**It does not.** The RTL8197F NAPT engine keys and hashes on plain **NUMERIC (host-order)**
values.

### Why the vendor source misleads

The vendor's `naptEntry` fields are raw `__be32`/`__be16` taken straight from conntrack
(`net/rtl/features/rtl_features.c:236-241`). So the vendor's `htonl()`/`htons()` at the
ASIC boundary is **`ntohl()` in disguise** — it converts on-wire bytes *to a number*.

This driver had already normalised to host order upstream (`int_ip = ntohl(...)`,
`int_port = ntohs(...)`), so applying `htonl()`/`htons()` again was a **double conversion**
that landed every row at a byte-swapped index the hardware never probes.

### Proof from the stock binary for this exact board

One numeric value feeds **both** the index and the key:

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

### Four sites, and they had to be numeric together

| # | site | was | knob at the time |
|---|---|---|---|
| 1 | stored key (`intIPAddr`/`intPort`) | `htonl`/`htons` | `napt_key_htonl=0` |
| 2 | row index (`gw_napt_hash1` inputs) | `htonl`/`htons` | `napt_idx_htonl=0` |
| 3 | G (global-port) encoding | `htons` | `napt_g_htons=0` |
| 4 | **inbound row's verification hash** | `htonl`/`htons` | **none — hardcoded** |

★ **Site 4 had no knob, and it is why WAN→LAN stayed at ~100 % through-CPU even after
LAN→WAN reached 0.0 %.** Fixed in `59e2d2cd27`, at
`files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x_hwnat.c:377`:

```c
e.selEIdx  = gw_napt_hash1(is_tcp, rem_ip, rem_port, 0, 0) & 0x3ff;
```

The ASIC recomputes `very` from the inbound packet's **numeric** `{remIP, remPort}` and
rejects the row on mismatch.

### The inbound row is a verification row, not a copy

The inbound row is **not** a TCPFlag-tweaked copy of the outbound row. It is an
enhanced-hash1 **verification** row (`rtl819x_hwnat.c:360-379`):

| field | value |
|---|---|
| `offset` | `G >> bits` (`gport & 0x3f`) |
| `selEIdx` | `very` = `HASH1(remIP, remPort, 0, 0)` |
| `selIPIdx` | `(G & 0x3ff) >> 6` |
| `TCPFlag` | `0x2` |

### The authoritative in-code explanation

`rtl819x_hwnat.c:285-294` — quote this, not the older comments:

```
 * ...The vendor's hsb fields come off the packet in
 * network order, so its htonl()/htons() on a little-endian build yield the NUMERIC
 * value the silicon hashes. This driver has already normalised to host order
 * upstream (int_ip = ntohl(...); int_port = ntohs(...)), so applying htonl()/htons()
 * again is a DOUBLE conversion that swaps it right back -- landing every row at a
 * byte-swapped index the hardware never probes.
```

### ★ Why it took so long — two compounding measurement errors

Both are worth stating, because both are process failures rather than technical ones.

1. **The `napt_fill_all` experiment was run with a poisoned control.** Writing the
   outbound row at all 1024 indices was meant to prove whether the *index derivation* was
   at fault — but it was run while `napt_key_htonl` was still `1`. With a byte-swapped
   **key**, the comparison cannot succeed at *any* index, so the run proved nothing about
   indexing. Its verdict — "the index is not the fault" — was nevertheless banked as
   settled and steered every experiment afterwards for a day.
2. **Each byte-order knob was then A/B'd alone.** `napt_idx_htonl=0` cannot win while
   `napt_g_htons=1` still corrupts G on a hit. Only all three — four, counting the
   verification hash — together show it. **Coupled variables cannot be A/B'd one at a
   time.**

### The whole bug in one line

> The RTL8197F NAPT engine keys and hashes on **NUMERIC (host-order) values, not on-wire
> network order**; every place the driver fed it swapped values — key, index, G encoding,
> verification hash — had to be numeric **together**, and any one left swapped hides the
> win completely.

### Consolidation

- `8d5b5870d4` (14:24) — numeric became the compile-time default for every ASIC byte order.
- `3ea4906a25` (15:03) — the knobs were **deleted**. `napt_key_htonl`, `napt_idx_htonl`,
  `napt_g_htons` and `extip_htonl` no longer exist; verified absent from the shipped
  driver (only the historical comments at `rtl819x_hwnat.c:278`, `:389` and
  `rtl865x_asichal.c:1407` still name them).

### Credit, as recorded in `8d315c331b`

Root-caused by an independent agent given **observations only and no prior hypotheses**,
told to prioritise the stock binary and the vendor init *order* over individual register
values:

> "Every register-value hypothesis this project generated was wrong; the answer came from
> disassembling firmware that works."

---

## 5. ★ Root cause 2: Fork A could never offload — ingress port == egress port

This one is structural, not a bug: **Fork A cannot hardware-offload by construction,
regardless of configuration.**

"Fork A" was the original switch model: a tagged-trunk VID cascade where SoC VID2 = LAN,
VID1 = WAN, both riding the single RGMII trunk (SoC port 0), CPU on port 6. Under it every
jack sits behind that one trunk — so **the ingress port and the egress port are the same
port for every routed flow**, and the ASIC does not hardware-forward a same-port hairpin.

Measured directly (mirror commit `0b6014c`):

| | throughput | packets/MB |
|---|---|---|
| `HWNAT=Y` | 167 Mbit | 131 |
| `HWNAT=N` | 170 Mbit | 143 |

No difference. Offload was on, and it did nothing.

**Supporting evidence** — commit `d3cb3c775a`: setting stock's real jack numbers (LAN
`0x0f`, WAN `0x10`) under Fork A does not merely fail to offload, it **kills the datapath
outright** (100 % loss both ways, box wedged, power-cycle required). The only single-port
value that carried traffic was `0x01` — the RGMII trunk itself. The SoC simply could not
address the jacks.

### The replacement

CPU-tag / port0-router mode (`5e2645d21f`, alias `c7389fc`; VLAN/CPU-port split in
`74b90e83ba`). See [`SWITCH-AND-DATAPATH.md`](SWITCH-AND-DATAPATH.md) for the full port
model. What matters here: it makes the five jacks **real SoC ports 0–4 with the CPU on
port 8**, so a routed unicast finally has a distinct egress port to commit to, and the
per-jack L2 masks that killed the datapath under Fork A now program cleanly
(`rtl819x-eth.c:590-615`). Fork A was deleted entirely in `86880757ed`.

### Two corrections worth publishing

1. The concluding analysis of the old investigation doc predicted this would be **"M4-scale
   work — comparable to the original ethernet carve"** and scoped it as **"its own project
   rather than a fix"**. It landed **the same day** (`5e2645d21f` 05:36, `74b90e83ba`
   06:10, `26964573d5` 15:23).
2. That same analysis predicted a register-diff against stock over SMI was **"the one
   experiment that settles it"**. That experiment was **never run**. The actual route was
   reading the vendor SDK.

### ★ And the reference implementation was in the vendor SDK all along

It was missed on earlier passes because the relevant SDK files are **non-UTF8**, so plain
`grep -r` silently skips them as binary. `grep -a` finds them:

```console
$ file target/linux/realtek/files/drivers/net/rtl819x/AsicDriver/rtl865x_asicL2.c
... C source, ISO-8859 text
$ file target/linux/realtek/files/drivers/net/rtl819x/rtl8367r/rtk_api.c
... C source, Non-ISO extended-ASCII text

$ grep -rn "CPU tag" AsicDriver/rtl865x_asicL2.c rtl8367r/rtk_api.c
                                   # <- nothing

$ grep -an "CPU_TAG\|PORT0_ROUTER_MODE" AsicDriver/rtl865x_asicL2.c
6055:#ifdef CONFIG_RTL_CPU_TAG
6417:    #ifdef CONFIG_RTL_CPU_TAG
6420:    REG32(MACCR1) |= PORT0_ROUTER_MODE;
7434:#ifdef CONFIG_RTL_CPU_TAG
7438:        REG32(MACCR1) |= PORT0_ROUTER_MODE;
```

with `AsicDriver/rtl865xc_asicregs.h:1017`:

```c
#define PORT0_ROUTER_MODE   (1 << 0)   /* 1: enable Port0 as router mode, 0: normal mode */
```

**Realtek's names for the mode are "CPU tag" and "port0 router mode", not "cascade"** —
which is part of why searching for it failed even when the tree was open.

> ⚠ **Standing rule for this repo:** in `sdk-rtl819x`, always use `grep -ra` / `grep -a`.
> Plain `grep -r` has silently hidden findings at least twice.

---

## 6. The other fixes that had to land first

None of these produce offload on their own. All of them had to be true before the two root
causes could even be *observed*.

### SWTCR1 = 0x2200 (`EnL4WayH` | `L4EnHash1`), vendor-exact

| write | bits | effect |
|---|---|---|
| `0x2200` | `EnL4WayH` (bit 9) + `L4EnHash1` (bit 13) | ✅ vendor-exact; forward HW NAT corruption-free |
| `(1<<13)` alone | `L4EnHash1` only | ❌ clobbers `EnL4WayH` → whole L4 datapath corrupt; `hwnat=1` killed even ICMP forwarding |
| `0` | neither | ❌ enhanced-hash1 off → replies never reverse-NAT'd, WAN flooded with a bogus `00:00:00:00:00:10` nexthop MAC |

Commits `ef6fee2cb9` (= `3cd6ec0`) and `808b2fd9f5` (= `9562db2`); the decode table above
is the shipped comment at `rtl865x_asichal.c:1388-1402`.

### `EN_IN_ACL` + a catch-all Ethernet permit + ★ `DACLRCR`

With `EN_IN_ACL` on and `DACLRCR` unset, **all routed forward traffic dropped**. `DACLRCR`
is the ACL range used when the net-decision (VLAN→netif) *misses*, and it had never been
written (`rtl865x_asichal.c:358`, `:1046-1056`).

The field layout was then found **wrong** (`92eae00018`): it was packed with 7-bit fields;
the 8197F uses 8-bit.

| field | bits |
|---|---|
| `ACLI_STA` | `[7:0]` |
| `ACLI_EDA` | `[15:8]` |
| `ACLO_STA` | `[23:16]` |
| `ACLO_EDA` | `[31:24]` |

The old `0x1FBF4180` decoded to ingress `[128..65]` and egress `[191..31]` — both
**inverted**, satisfiable by no rule. The vendor writes `PERMIT_ALL` (253) in all four:

```c
REG32(GW_DACLRCR) = (253u << 0) | (253u << 8) | (253u << 16) | (253u << 24);  /* 0xFDFDFDFD */
```

### ACL ranges (`e677d7cb3b`)

The dst-MAC→TOCPU classifier sat in ACL slot 4, but **both** netifs had ingress ACL range
`[0..3]` — so slot 4 was never scanned. Point the WAN netif at `[4..6]`, exactly as stock
does (`rtl865x_asichal.c:751-758`); `start = (inACLStartH << 1) | inACLStartL`, so 4 ⇒
H=2, L=0:

```c
nif.inACLStartL = 0; nif.inACLStartH = 2; nif.inACLEnd = 6;   /* WAN ingress ACL [4..6] */
```

The `00:00:00:00:00:10` garbage-MAC flood — visible for the *entire* investigation — went
from **13–21 frames per flow to zero**.

### extIP byte order (`fd606f4428`, alias `3f8fc72`)

★ `gw_prog` hardcoded `ext.externalIP = htonl(...)`, **ignoring the `extip_htonl` knob**.
Every `cat /proc/rtl865x_gw` therefore silently re-swapped the row and undid the runtime
setting — which is why that experiment stayed inconclusive across several sessions.

`externalIP` is the field that decides NAPT-EXTERNAL (NPE) classification. On a miss the
destination degrades to RP, and `_RTL8651_PROC[RP][RP]` = WAN-side routing — so **reverse
NAPT is never attempted at all** (`rtl865x_asichal.c:1405-1420`).

Measured: `dst=RP(19) reason=5` became `dst=idx0=NPE reason=7`. Both directions collapsed
to **one** failure.

### MEMCR L4 SRAM init (`e619d13efe`)

The fabric-reset MEMCR init wrote `0x24` and polled `0x2400`, which **do not cover the L4
table**. Vendor `rtl8651_clearAsicNaptTable()` — gated
`#if defined(CONFIG_RTL_8198C) || defined(CONFIG_RTL_8197F)`, i.e. written for this exact
chip — uses MEMCR **bit 1 set / poll bit 9** (`rtl819x_regs.h:136-148`,
`rtl819x-eth.c:352-368`):

```c
REG32(MEMCR) &= ~(1<<1);
REG32(MEMCR) |=  (1<<1);                            /* L4 */
while ((REG32(MEMCR) & (1<<9)) != (1<<9)) ;         /* wait L4 clear done */
```

**The L4 table SRAM had never been initialised.**

### VLAN FID (`8b5a1d5a69`)

`sw_add_vlan()` never wrote the VLAN entry's FID, so both VLANs claimed `fid 0` — while
WAN peer L2 entries live in `fid 1` (`rtl865x_asichal.c:530,773,829`). The L2/FDB lookup
for routed egress is keyed by `{MAC, FID}`, so the WAN-bound lookup missed
(`rtl819x-eth.c:173-210`):

```c
sw_add_vlan_fid(RTL865X_VID_LAN, 0x10F, 0x00, 0); /* jacks 0-3 + CPU8, fid0 */
sw_add_vlan_fid(RTL865X_VID_WAN, 0x110, 0x00, 1); /* jack 4    + CPU8, fid1 */
```

Fork A never noticed, because it never resolved a per-port egress at all.

### FFCR (`2dc1e821e0`)

`FFCR` had **never been written at all** — the defines existed with no write site. Vendor
gateway mode clears `EnUnkUC2CPU` (`rtl865x_asicL2.c:6966`) so an unknown-DA unicast
*floods* instead of being punted (`rtl865x_asichal.c:938-954`).

### PPPoE offload (`9c79b0f0c1` → `03b4cf76dc` = `823265d` → `195aeab4a9`)

Type-11 session table, type-1 nexthop with `PPPoEIndex=0`, dynamic ext-IP from live
`ppp0` — all programmed correctly, and the encap silicon black-holed every frame.

★ Root cause: `gw_wan_netif_prog_locked()` set the WAN netif MTU to **1492**. **Stock
always uses 1500**, so the ASIC treated the encapsulated frame as oversized and trapped it
on egress. Pinning MTU 1500 produced 1440-byte PPPoE data frames on egress (was zero),
~157 Mbit at the time.

---

## 7. Getting the chip to state its own failure

Commit `1d80271cd5`. Every register-value hypothesis this project generated was wrong; the
turn came from making the silicon report its own trap reason.

Added `sel_cpu_reason` = **SWTCR1 bit 8, `EN_51B_CPU_REASON`**
(`rtl865x_asichal.c:49,86-88`). Realtek's bit-accurate ASIC model `l34Model.c` — shipped
only in the "otto" U-Boot GPL drops — documents `ph_reason` as
`[14:10] dst-type | [9:5] src-type | [4:1] reason`, but **every Realtek L34 test sets bit 8
before reading it**. This driver wrote SWTCR1 absolutely as `0x2200`, leaving bit 8 clear,
so every previously captured value (`0x0c03` / `0x0c09` / `0x0c20`) was **legacy-encoded**
and decoded to nonsense.

With bit 8 set the chip says it outright:

```
LAN ingress: reason=0x4e0e -> src=16(NPI) dst=19(RP) reason=7  *** NO MATCHED NAPT ENTRY ***
WAN ingress: reason=0x4e6a -> src=19(RP)  dst=19(RP) reason=5
```

> ★ This also **retired the "aging pins" evidence**: rows pinning at the reload ceiling was
> never proof of hardware hits.

Two supporting instruments landed alongside it:

- **`4177e0bb4e`** decoded the ASIC's own `hwFwd` / `isOriginal` flags (`ph_asic0` bits,
  vendor `common/mbuf.h:87-94`) and measured **`hwFwd=0` on every frame** — killing the
  "offloading but also copying the CPU" hypothesis and confirming the bytes-through-CPU
  metric had been accurate all along.
- **`fb2950d149`**: the vendor NAPT read path is **not a plain memory read**. It needs a
  dummy read of an unused entry to refresh the ASIC latch, plus `READ_MULTIPLECHECK`
  (double-read + retry ×10). This port did the dummy read **after** the entry — which is
  *worse* than omitting it, since a write+readback pair could confirm itself out of the
  latch. Reordered + multi-checked, this proved the rows really were in table RAM and were
  not phantoms.

---

## 8. How to reproduce and verify offload on your own box

### Boot ritual (already automated)

`/etc/init.d/dir842-asic` (`START=97`) runs this for you; it is reproduced because the
**order is load-bearing**:

```sh
swconfig dev switch0 set apply 1                       # 1. re-apply 8367S VLAN/forwarding
echo 3 > /sys/module/rtl819x/parameters/fabric_reset   # 2. level-3 fabric reset (SIRR FULL_RST)
cat /proc/rtl865x_gw > /dev/null                       # 3. gw_prog: netifs, routes, nexthops, ACL ranges
ping -c2 192.168.0.2; ping -c2 172.16.0.2              # 4. warm the ASIC L2/ARP tables
```

★ **`gw_prog` WIPES the ASIC L2 tables**, so the warm-up must come **after** it, never
before. ★ A level-3 `fabric_reset` clears the TLU tables, so it must **always** be paired
with a following `gw_prog`.

### Offload arming

```sh
echo 1 > /sys/module/rtl819x/parameters/hwnat
```

R4 (2026-08-16): **boot now arms `hwnat` automatically** — the `dir842-asic` service does
it as its last step, strictly after the L2 warm-up (arming against cold tables kills the
datapath, measured). The manual `echo 1` is only needed on pre-R4 images or after a
manual disarm. ★ Still: state the `hwnat` setting of every number you report — at least
two measurements in this project were confounded by omitting it.

### Measure with bytes-through-CPU, not throughput and not packet counts

```
through-CPU % = delta(/proc/net/dev rx bytes) / payload bytes
                eth0.2 for upload (LAN->WAN),  eth0.1 for download (WAN->LAN)
```

GRO coalescing makes CPU-side *packet* counts meaningless, and throughput alone cannot
distinguish "offloaded" from "the CPU is keeping up" (§1). The harness that enforces this —
frame-size pre-flight gate, per-measurement `iperf3` listening assert, byte accounting — is
described in [`BENCH.md`](BENCH.md).

---

## 9. Known limitations of the offload

- **The ASIC's inbound NAPT row is FULL-CONE.** The hardware entry stores only
  `{proto, extIP, G → intIP, intPort}` — no remote endpoint; the remote is encoded only in
  the *outbound* row's hash index. Linux masquerade preserves source ports and guarantees
  only 5-tuple uniqueness, so two LAN hosts using the same source port to different remotes
  share one G: the first offloads, the second correctly declines (shadow bitmap) and stays
  in software — but its **inbound** packets match the first flow's row and are rewritten to
  the wrong host (`rtl819x_hwnat.c:36-46`). The same mechanism can hijack replies to the
  router's own connections. Low frequency, but silent. Stock avoided it by owning the whole
  port namespace (it allocated every G). Mitigation: a NAPT miss **always traps to the
  CPU**, so software is the safe fallback — but note the shipped image **arms offload at
  boot** (R4), so choosing the software path is now an explicit
  `echo 0 > /sys/module/rtl819x/parameters/hwnat`.
- **Download throughput is variable** (**681–906 Mbit** across runs) with 1200–2500 TCP retransmits per 10 s
  run. The router is not the bottleneck (CPU 0.3 %, zero interface errors). **The loss
  source has not been identified.**
- **Everything here is measured on an isolated bench**, never a real household gateway.

---

## 10. Where the blow-by-blow investigation lives

| document | what it is |
|---|---|
| [`M7-HWNAT-REVERSE-NAPT.md`](M7-HWNAT-REVERSE-NAPT.md) | The chronological investigation log, kept **unsanitised**. Its status header ("two isolated remaining blockers") and its byte-order premise at `:42` / `:370` are **both superseded by this document**. Read it for the path, not the conclusions. |
| [`SWITCH-AND-DATAPATH.md`](SWITCH-AND-DATAPATH.md) | CPU-tag / port0-router mode and the full port model. |
| [`BENCH.md`](BENCH.md) | The rig, the harness, and why each assertion in it exists. |
| [`RETRACTIONS-AND-METHOD.md`](RETRACTIONS-AND-METHOD.md) | Every falsified hypothesis, collected. |
| [`M7-LARGE-FRAME-RX-WEDGE.md`](M7-LARGE-FRAME-RX-WEDGE.md) | The large-frame RX wedge that invalidates measurements silently — the reason the harness pre-flights at 1400 B. |

---

## 11. Stale comments still in the shipped code (fix these)

Three comments survive that contradict the working code. They are listed here so the next
reader is not misled by them, and so they can be cleaned up deliberately.

1. **`files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x_hwnat.c:471-481`**
   still says *"The ASIC hashes/keys the ON-WIRE (network-order) header fields … Feed
   NETWORK order for the index AND for the stored key + G-encoding (below)"* — immediately
   above the calls at `:482` and `:491` that now pass **numeric** values. It directly
   contradicts the authoritative comment at `:285-294`. **The comment is wrong; the code is
   right.**

2. **`files/target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x_regs.h:166`**
   still has `#define SW_CPU_PORT 6 /* CPU = L2 port 6 (bit 6) */`, while the CPU is on
   **port 8** in CPU-tag mode (see `rtl819x-eth.c:597-599`, which cites vendor
   `rtl865x_netif.h:626`: `RTL_CPU_PORT 8`).

3. **`files/target/linux/realtek/base-files/etc/board.d/01_leds:51`** still says the vendor
   `rtl8192cd` driver is *"not used by this port (5 GHz is rtw88; the on-SoC 2.4 GHz radio
   has no driver yet)"*. **Both halves are now false** — `rtl8192cd` drives the 2.4 GHz
   radio and both radios run concurrently.
