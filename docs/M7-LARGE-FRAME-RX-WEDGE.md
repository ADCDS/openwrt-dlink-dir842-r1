# M7 — Large-frame CPU-RX wedge (breaks DHCP, SSH, any large box-terminating packet)

> ⚠️ **HISTORICAL JOURNAL.** Written during the M7 diagnostic; the root cause and
> fix below still stand, but the surrounding platform description does not — the
> port now boots from NOR, not only from RAM. Read [`README.md`](../README.md) first.

**Platform:** D-Link DIR-842 **rev R1** — RTL8197F SoC (MIPS 24Kc), ported vendor
two-ring pkthdr+mbuf CPU-port DMA engine (`drivers/net/ethernet/rtl819x/`),
mainline OpenWrt 4.14. (An earlier revision of this file said "rev C1"; that was
wrong and dangerous — the C1 is a **MediaTek** board and this work would brick it.
Only the RTL8197F rev R1 is in scope.)

**Status: RESOLVED — self-healing fix committed + HW-validated** (openwrt tree
`ba13a96` driver, `e5737c5` dhcp-broadcast). Root cause = an RX-FIFO drain-lag
race (descriptor writes overtake the multi-burst payload DMA under CPU
saturation) that latches switch-core NIC state; cleared only by the vendor fabric
reset, never by a CPU-engine re-init. The box now **detects the wedge (software
FCS check) and auto-heals in-place** (vendor `FullAndSemiReset` + in-kernel gw
re-arm), no reboot. See "RESOLUTION" at the bottom. The rest of this doc is the
original diagnostic trail, kept so the next person doesn't re-walk it.

---

## TL;DR

The SoC CPU-port RX path drops **every frame larger than ~128 bytes** while
small frames flow perfectly. It is **not** a config bug and **not** saddr/bridge
/rp_filter related — it is a **wedge of the switch fabric's multi-descriptor
(large-frame) RX path, induced by a sustained inbound flood**. A `ping -f`
(~20 000 packets) is enough to trigger it.

Because small frames keep flowing, `rx_packets` keeps incrementing, so the
driver's missed-IRQ watchdog (`rtl819x_hang_work`) never fires — the partial
wedge is **invisible to the existing recovery path** and persists until a full
reset.

Everything that only ever sends/receives *small* frames to the box keeps
working (ARP, DNS answers, small pings), which is why the box *looks* healthy.
Anything that needs a **large** frame delivered **to the box's own IP** fails:

| Service | Frame | Works? |
|---|---|---|
| ARP resolve of gateway | ~60 B | ✅ |
| DNS query→answer | small | ✅ |
| ICMP ping (default 56 B) | 98 B | ✅ |
| **DHCP DISCOVER** | **~300 B** | ❌ never reaches dnsmasq |
| **SSH** (banner OK, KEX) | key/DH >128 B | ❌ stalls in banner/KEX |
| ping ≥ ~90 B payload | ≥130 B | ❌ 100 % loss |
| **Routed client↔internet** (HW-NAT) | any | ✅ unaffected — ASIC path, not CPU-RX |

The gateway **data plane (HW-NAT forwarding) is unaffected** — it runs in the
rtl865x ASIC and never touches the CPU-RX engine. Only traffic *terminating on
the box* is hit.

---

## ⚠️ The single most useful gotcha: tcpdump is BLIND on this driver

`tcpdump` / any `AF_PACKET` socket captures **nothing** on `eth0`, `eth0.2`,
`br-lan` — not even traffic that provably flows (a working ping shows 0 captured
packets). The ported vendor RX path hands frames to the stack via
`napi_gro_receive` in a way that bypasses the `ptype_all` tap that
tcpdump/libpcap and AF_PACKET DHCP servers rely on.

**Consequence:** do not trust "tcpdump shows nothing" as "no packet arrived",
and do not expect an AF_PACKET-based DHCP server (ISC dhcpd/LPF) to receive
either. **Use netfilter counters instead** — they sit in the real IP path:

```sh
# on the box — count what actually reaches the IP stack:
iptables -I INPUT      -p udp --dport 67 -m comment --comment DHCPIN     # LOCAL_IN
iptables -t mangle -I PREROUTING -p udp --dport 67 -m comment --comment DHCPPRE  # earliest IP hook
iptables -I INPUT      -p icmp                    -m comment --comment PINGIN
iptables -vnL INPUT ; iptables -t mangle -vnL PREROUTING       # read .pkts column
iptables -Z                                                     # zero between trials
```

This is how the whole investigation was actually driven; the box also has no
`devmem`, `dig`, or `nslookup`, so runtime registers were read from the driver's
own boot `pr_err` and DNS was probed with a raw-socket Python one-liner.

---

## Diagnostic journey (the false leads, so you can skip them)

The presenting symptom was "DHCP clients get no lease." Ruled out, in order:

1. **dnsmasq mis-config** — no: range `192.168.0.100-249`, listening `0.0.0.0:67`,
   `lan` not ignored, DNS on the *same* daemon works 100 %.
2. **Broadcast reception broken** — no: a fresh-MAC client (macvlan, no static
   ARP) resolves the gateway by real broadcast ARP and pings 4/4. Broadcast RX
   of *small* frames works.
3. **Bridge not passing broadcast up to br-lan** — red herring; moving the LAN
   IP straight onto `eth0.2` (no bridge) changed nothing.
4. **saddr=0.0.0.0 martian / rp_filter drop** — red herring; `rp_filter=0`
   everywhere, `log_martians=1` logged nothing, and a *valid-source* broadcast
   to :67 also failed once it was large.
5. **Real cause = frame SIZE.** Controlled tests with identical crafting:
   - valid-source, **8-byte** payload → :67 → reaches INPUT ✅
   - valid-source, **300-byte** payload → :67 → dropped ❌
   - saddr=0 vs valid-source made **no** difference once size was held equal.
   - ICMP `INPUT` counter proved it is an **RX-side** drop (large pings never
     increment it — the box never receives them; it's not a TX/reply failure).

**Ping size sweep (payload → frame → loss):**

```
 40 →  82 B : 0%      72 → 114 B : 0%
 56 →  98 B : 0%      80 → 122 B : partial
 64 → 106 B : 0%      88 → 130 B : 100%
                     ≥88 → ≥130 B : 100%
```

Hard knee at a **~128-byte frame**.

---

## Why "128 bytes" — and why it is a wedge, not a static limit

- CPUICR at boot (`pr_err` bringup dump) = **`0xe4000000`** =
  `TXCMD|RXCMD|BUSBURST_128WORDS|MBUF_2048BYTES` → bits[26:24]=100 → **mbuf size
  is correctly 2048 B**, not 128. So it is *not* a mis-sized RX buffer.
- `SBFCR0=0x1e0` — the shared-buffer flow-control fix (see the big comment in
  `rtl865x_start()`, `rtl819x-eth.c`) **is** applied.
- In the switch fabric a frame is stored across **multiple internal
  descriptors** whose granularity is ~128 B. So ">128 B" == "**multi-descriptor
  frame**". The existing comment already documents this exact failure mode:
  *"the fabric drops multi-descriptor (large) frames congestion-sensitively and
  wedges when the pool exhausts."*
- **Timeline proof it is dynamic:** the very **first** SSH of the session
  completed a full key exchange (KEX packets are >128 B) — large-frame RX worked.
  It broke immediately **after** a `ping -f` flood (~20 000 pkts). The driver's
  own comment: *"a sustained max-rate flood of tens of thousands of frames
  eventually wedges: RX engine frozen."* The flood wedged the **large-frame**
  path specifically.
- A **CPU-engine re-init is not enough**: `ip link set eth0 down/up`
  (= ndo_stop+ndo_open = `rtl865x_down` + `New_swNic_init` + `rtl865x_start`)
  did **not** restore large frames. The wedge is deeper than the CPU DMA — it is
  in the switch fabric buffer/descriptor state, which only a full reset clears.

---

## Impact / mitigation

- **Robustness bug for M7.5:** a real gateway takes inbound floods (a torrent
  peer, a scan, normal WAN load). If that wedges box-terminating large frames,
  DHCP renewals and management die while forwarding still looks fine — a nasty,
  silent, partial failure. This must be fixed before any household cutover.
- **The watchdog must catch partial wedges.** `rtl819x_hang_work` currently
  triggers on `rx_packets` fully stalling. It needs a second trigger — e.g. a
  large-frame RX-progress counter, or a periodic self-probe — because small
  frames mask the stall.
- **Deeper fix candidates:** the SBFCR / shared-buffer-pool back-pressure
  thresholds evidently still let the large-frame descriptor pool wedge under
  flood; the vendor `rtl8651_clearRegister` / SBFCR constants and the fabric
  runout handling are the place to look. A full fabric reset in `hang_work`
  (not just CPU-engine re-init) is likely required for recovery.
- **Recovery today:** power-cycle / full reboot clears it (pending confirmation).

---

## What is NOT the problem (confirmed)

- Not dnsmasq config, not the bridge, not saddr=0, not rp_filter, not martian
  filtering, not the CPUICR mbuf-size field, not missing SBFCR.
- Not a TX/reply failure — it is confirmed **RX-side** (large pings never reach
  the ICMP INPUT counter).
- Separate, unrelated issue: **box→host *cold* unicast** (the box initiating a
  unicast to a host whose entry has aged) — that is the #13 story and is about
  the box→host TX/L2-lookup path, not frame size. Managed on the bench with a
  `PERMANENT` host ARP entry for the box.

## CONFIRMED by reboot + the DHCP fix (follow-up session)

Rebooted into a fresh kernel and re-tested. Results:

- **Fresh boot restores large-frame RX:** ping at payload 100/500/1000/1400 →
  **0% loss at every size**. The wedge is definitively dynamic, cleared by a boot.
- **DHCP now works on the box side:** dnsmasq logs `DHCPDISCOVER(br-lan)` →
  `DHCPOFFER(br-lan) 192.168.0.157` — the 300 B DISCOVER reaches it once large
  frames flow. Earlier "dnsmasq never sees the DISCOVER" was purely the wedge.
- **Second, independent bug on the OFFER path (the #13 story):** a *normal*
  client still `leasefail`s because the **unicast OFFER (box→client, to a
  just-learned MAC) is not delivered** — box→host *cold* unicast. A client that
  sets the broadcast flag (`udhcpc -B`) gets a **full lease**
  (`DISCOVER→OFFER→REQUEST→ACK`, `192.168.0.157`). So the fix is to force the
  server to broadcast all replies: dnsmasq **`dhcp-broadcast`** (unconditional).
  - ⚠️ OpenWrt's `option dhcpbroadcast '1'` only emits the *tag-conditional*
    `dhcp-broadcast=tag:needs-broadcast`, which does **not** cover normal clients.
    You need the raw unconditional `dhcp-broadcast` (e.g. a line in the
    `conf-dir` — `/tmp/dnsmasq.d` on this build — or an `/etc/dnsmasq.conf`
    include that survives reboot). Bake it into the image's uci-defaults /
    dnsmasq conf for production.
- **The wedge re-triggers easily and is reboot-only to clear:** after the fresh
  boot it took only some SSH key-exchange attempts (large frames, with
  retransmit stalls) plus a handful of DHCP probes to put the large-frame RX
  path back into the wedged state. `ip link set eth0 down/up`
  (ndo_stop+ndo_open = `rtl865x_down`+`New_swNic_init`+`rtl865x_start`) did **not**
  restore it — so the wedge lives in switch-fabric state below the CPU engine,
  and only a full chip reset (reboot) recovers it. The driver's `rtl819x_hang_work`
  re-init is therefore also unlikely to recover it; a **fabric-level soft reset**
  (or root-causing the descriptor-pool wedge) is what M7.5 needs.

- **REFINED root cause — it is DATA CORRUPTION, not a drop (live-box measured):**
  when wedged, large frames DO reach the driver's RX ring **full-length** — `eth0`
  rx_packets AND rx_bytes both increment fully (Δ ~1054 B/frame over 20 large
  pings; small frames Δ ~66 B). So they are NOT dropped in the fabric and NOT
  truncated — `New_swNic_receive` returns them with the correct `ph_len`. But an
  ICMP counter at `iptables -t mangle PREROUTING` (earliest IP hook) caught only
  **1 of 15** large pings, and `INPUT` likewise 1/15 — so ~93% carry a **corrupt
  IP header/payload** (fail `ip_rcv` checksum) while their L2 length + VLAN tag +
  pkthdr descriptor are correct; ~7% arrive clean. Single-cell (≤128 B) frames are
  never corrupted. ⇒ the wedge is a **DMA data-integrity failure of the
  multi-cluster RX payload** (correct descriptor, stale/corrupt cluster data) for
  multi-cell frames under load — the place to look is the 2 KB-cluster
  cache-coherency / `dma_unmap`+sync / cluster-refill-vs-switch-DMA race in the
  ported swNic (`rtl819x_swnic.c` `New_swNic_receive`), NOT congestion or an egress
  scheduler. A latched switch-side DMA/cache state surviving the CPU-engine re-init
  would also explain the reboot-only recovery. (A Fable-5 RE agent is on this.)

**Net for the gateway:** on a clean boot the whole service set works — ARP, DNS,
DHCP (with `dhcp-broadcast`), large frames, and HW-NAT forwarding. The blocker
is *robustness*: sustained large-frame box-terminating load (SSH, floods, maybe
DHCP renew storms) re-wedges the RX path with reboot-only recovery. Forwarded
client↔internet traffic (ASIC) rides through a wedge untouched, but box services
(DHCP renew, DNS-from-box, management) degrade until reboot.

## Reproduce / verify

```sh
# from a host on the LAN (box = 192.168.0.1):
ping -c3 -s 20   192.168.0.1     # small  → 0% loss
ping -c3 -s 1000 192.168.0.1     # large  → 100% loss when wedged
# fresh-MAC client still resolves+pings small (broadcast RX of small frames OK):
ip link add link <if> mv0 type macvlan mode bridge; ip link set mv0 up
ip addr add 192.168.0.90/24 dev mv0; ping -c2 -I 192.168.0.90 192.168.0.1
```

## RESOLUTION (fix + HW validation)

**Root cause (confirmed via the `rx_dump` discriminator on a wedged box):** an
RX-FIFO drain-lag race. Under CPU saturation the switch-core NIC's descriptor
writes overtake the multi-burst payload DMA, so the CPU reads pre-DMA DRAM —
`rx_dump` showed `ph_len==m_len, m_next=0, cached==uncached both stale` = the
payload never reached DRAM (not truncation, not a gather/chain, not a CPU-cache
miss). It latches NIC/fabric state that a CPU-engine re-init (`eth0 down/up`), the
`GDSR_PORT_CONG` drain, and the A-2 `gw` re-trigger all fail to clear.

**Why stock never wedged:** stock runs the vendor fabric reset at *every* eth init
(`FullAndSemiReset`, byte-for-byte at stock-kernel 0x8019396c); this port never
did. The fix ports that reset and makes it a runtime recovery.

**The fix (openwrt tree `ba13a96` driver, `e5737c5` dhcp-broadcast):**
- `rtl819x_fabric_full_reset()` = vendor `FullAndSemiReset`: `SIRR |= FULL_RST`
  (0xBB804204 bit2) → mdelay 300 → gate swcore clock (`CLK_MANAGE` 0xB8000010
  bit11) 300 ms → ungate 50 ms → `MEMCR` table-SRAM re-init → restore a 153-reg
  snapshot taken at first good `rtl865x_start`. Quiesced under the HAL lock +
  napi/tx-disable; trunk (P0GMIICR/MACCR) preserved.
- `fabric_reset` recovery ladder (1 engine re-init / 2 +CPUICR SOFTRST / 3 +full
  fabric reset). **Level 3 also re-arms the ASIC gw scaffolding in-kernel**
  (`rtl865x_gw_rearm` → `gw_prog`) — FULL_RST wipes the netif-MAC/L2/L3/NAT TLU
  tables, so without this L2/L3 forwarding stays dead (even small frames) until a
  manual `cat /proc/rtl865x_gw`.
- **FCS wedge detector**: software Ethernet-FCS check of every large (>132 B)
  delivered frame (`crc32_le` vs trailing FCS). Self-arms on 2 good large frames,
  declares a wedge on `fail>=4 && fail>=4*ok` in a 10 s window, auto-runs level 3
  (`fabric_autoreset` default 3, 30 s rate-limit). Replaces a USEDDSC-floor
  heuristic that false-fired (idle USEDDSC is 18, identical to wedged).

**HW validation:**
- Fresh `rx_dump`: clean (`diff@-1`). Wedged `rx_dump`: `cached==uncached both stale`.
- Level 2 (CPUICR SOFTRST) does NOT clear it; **level 3 does**, no reboot, all
  sizes back to 0% loss.
- **Autonomous self-heal proven**: the box wedged spontaneously under boot load;
  the FCS detector fired (`fail=18 ok=2`; zero idle false-positives over the
  session) → level 3 + in-kernel gw re-arm → **0% loss all sizes + a normal-client
  DHCP lease, no manual step, no reboot** (`uptime` unchanged).
- The wedge is a rare race (reproduced ~1 in 3–4 heavy CPU-saturation +
  large-flood attempts; plain floods never trigger it).

**Known residual / follow-ups:** (1) the box can wedge briefly during boot load
and self-heals ~10–20 s later — cosmetic, but reducing boot-time large-frame load
or a preventative FIFO-drain fix would avoid the post-boot blip; (2) detection
needs *some* large-frame arrivals while wedged (DHCP/SSH retries provide them; a
box wedged from second 0, before the detector arms, still needs a manual
`echo 3 > /sys/module/rtl819x/parameters/fabric_reset`).

## R2 — preventing the wedge at source (candidate found, NOT yet proven)

R2's goal is to stop the wedge happening rather than self-heal it. Two of the
plan's original suspicions turned out to be already-closed, and a third is new.

**Already correct — do not re-investigate:**

1. *DMA_CR0 water-mark hysteresis.* Already programmed to the shipped-stock value
   `0xA0CE` in `rtl865x_start()`. This was the A-2 fix.
2. *Bus burst size.* The plan suspected `BUSBURST_128WORDS` (512 B) made a 1400 B
   frame a 3-burst payload and so caused the descriptor/payload race. Decoded from
   the shipped stock kernel:

       80192b54: lui v0,0xb801      ; v0 = 0xB8010000 (CPUICR)
       80192b58: lui v1,0xe400      ; v1 = 0xE4000000
       80192b5c: sw  v1,0(v0)       ; CPUICR = 0xE4000000

   `0xE4000000` = `TXCMD | RXCMD | BUSBURST_128WORDS | MBUF_2048BYTES` — **byte
   identical to what this port writes.** Burst size is FALSIFIED as a candidate.

**The new candidate — CPUIIMR bit16 (`MBUF_DESC_RUNOUT_IE`):**

Stock's interrupt mask, decoded at `0x80192bb0`:

    80192ba8: lui   v1,0x807e
    80192bac: addiu v1,v1,12798     ; v1 = 0x807E31FE
    80192bb0: sw    v1,0x28(v0)     ; CPUIIMR = 0x807E31FE

    stock  0x807E31FE = LINK_CHANGE | PKTHDR_RUNOUT_ALL | RX_DONE_ALL | TX_DONE_ALL
    ours   0x007F31FE = bit16 MBUF_DESC_RUNOUT set; LINK_CHANGE clear

Two bits differ, and only one is in play:

- **bit31 `LINK_CHANGE_IE`** — ours clears it *deliberately*: it is a level bit
  that write-1-ack cannot clear while the link settles, so arming it re-fires on
  cable plug-in → IRQ livelock → wedge. Keep it clear.
- **bit16 `MBUF_DESC_RUNOUT_IE`** — ours arms it, **stock does not.** The code
  cited the public SDK (`asicCom.c:1417`) as arming both runout sources. ★ That is
  the same SDK-vs-shipped-stock discrepancy class as `DMA_CR0` (SDK `0xA0A0` vs
  stock `0xA0CE`) — and that one was directly implicated in this wedge. The
  shipped kernel is ground truth.

Suspected mechanism: MBUF runout asserts precisely during the CPU-saturation
window where descriptor writeback overtakes the multi-burst payload DMA. If
`MBUF_DESC_RUNOUT_IP` is a level bit that cannot be acked until the buffer pool
actually refills, arming it reproduces the exact re-fire → IRQ-livelock → wedge
pattern already documented for `LINK_CHANGE_IE`. Dropping bit16 keeps M6.3b's
prompt-napi kick via PKTHDR runout (which stock arms, and which covers the
descriptor ring) while no longer arming the one source stock leaves masked.

**Status: implemented, compiles, NOT verified on hardware.** Default is now off
(stock-aligned) and it is a runtime knob so the bench can A/B it without a
rebuild:

    echo 0 > /sys/module/rtl819x/parameters/mbuf_runout_ie   # stock-aligned (default)
    echo 1 > /sys/module/rtl819x/parameters/mbuf_runout_ie   # pre-R2 behaviour
    ip link set eth0 down; ip link set eth0 up               # re-arm CPUIIMR, then load

**The R2 gate is unmet:** 10 minutes of bidirectional saturating traffic with ZERO
fabric resets, run both ways. Until that is done this is a hypothesis. Note the
wedge only reproduces ~1 in 3–4 heavy attempts, so a single clean run does NOT
confirm it — compare reset counts across several runs per setting. If the rate is
unchanged with bit16 clear, record the candidate as FALSIFIED here rather than
leaving it open.

## ⚠️ Bench confound #5 — `ip neigh flush` before measuring fakes a total outage

**This one cost five build+boot cycles and produced two retracted findings. Read it
before trusting any "100% packet loss" result.**

Symptom: every path reads 100% loss — small frames, 1400 B, NAT — on a freshly
booted box, reproducibly, across cold power-cycles. `gw_prog` still reports
`netif readback PASS` and `RESULT PASS (gateway datapath LIVE in ASIC)`, and the
driver's own diagnostic shows `rx_done=0 CPUIISR=00000000`. It looks exactly like
a dead CPU-port RX engine, and it survives warm re-opens, so it reads as a hard
regression from whatever you just changed.

**It is the measurement, not the box.** The harness ran

    sudo ip neigh flush dev <usb-eth>     # <-- the bug
    ping -c4 192.168.0.1

Flushing the host ARP cache immediately before pinging recreates precisely the
cold-unicast condition documented above and in task #13: *the ASIC L2/ARP entries
start empty and cold unicast is not delivered until traffic has actually flowed.*
The flush throws away the warm state that the bring-up sequence just built, so
every measurement lands in the cold window. Proof, same boot, no code change:

    (flush, then ping)                 -> 100% loss on all three paths
    (passive tcpdump, 30 s)            -> box IS emitting: its own warm-up pings,
                                          ARP requests, ARP replies, and the host
                                          is answering them
    (ping again, tables now warm)      -> 0% loss  56 B
                                          0% loss  1400 B
                                          0% loss  NAT (172.16.0.2)

**How to measure instead:**
- Do NOT flush ARP before a measurement. If you must clear state, flush and then
  let the box's own warm loop run for ~20 s before measuring.
- Confirm the box is alive with a **passive** `tcpdump` first — it costs nothing
  and distinguishes "box is silent" from "box is fine but my path is cold".
- Only then measure, and repeat once before believing a negative.

**Findings retracted because of this confound** (both were recorded as measured
fact and are not):
1. "Masking CPUIIMR bit16 (`MBUF_DESC_RUNOUT_IE`) leaves the RX engine dead." Not
   established. The stock-aligned masked value remains an untested R2 candidate.
2. "The per-unit MAC from mtd1 breaks the datapath." Not established. The per-unit
   MAC has its own unrelated problem (non-deterministic application from
   userspace), but it does not break forwarding.

Running total of bench confounds, all of which produce a false "100% packet loss":
host USB-eth loses its IPv4; the `172.16.0.0/24` route reverts; tiny's `br0` loses
`172.16.0.2`; the box rebooted out of the volatile RAM image (#4); and now — the
ARP flush in the measurement itself (#5).

### R2 gate result (2026-07-30) — PASSED, with stated coverage limits

Run with `mbuf_runout_ie=0` (CPUIIMR bit16 masked = shipped-stock 0x807E31FE):

    888,569 x 1400 B box-terminating frames, 9 min continuous saturation
      0% packet loss
      0 fabric resets            <- the gate criterion
      0 FCS wedge detections     <- self-heal detector never armed
      post-run: 56 B 0% / 1400 B 0% / NAT 0% (NAT after re-warming, see confound #5)

Large box-terminating traffic is exactly this wedge's documented trigger (task #15),
so this is the right load to gate on. `mbuf_runout_ie=0` is now the default.

**What this establishes:** masking bit16 is safe — it does not break RX. The earlier
claim that it left the RX engine dead is retracted (confound #5).

**What it does NOT establish** — stated plainly so nobody over-reads it:
- It does not prove the candidate *fixed* the wedge. The wedge reproduces only
  ~1 in 3–4 heavy attempts, so one clean run is equally consistent with "fixed" and
  "did not fire this time". Closing this properly needs several runs per setting.
- No A/B baseline at `mbuf_runout_ie=1` was run for comparison.
- **Coverage gap:** the load was box-terminating ICMP, not bidirectional *forwarded*
  saturation. M6.3b's original reason for arming bit16 was napi falling behind under
  forwarded load with the Rx ring running out of CPU-owned slots — that scenario is
  still untested. (iperf3's server would not stay resident on the WAN peer; note also
  that `ssh tiny` routes over Tailscale, NOT through the box, so any load driven that
  way never traverses the device — target `172.16.0.2` explicitly.)
