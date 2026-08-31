# The RX-stall wedge — reproduction, detector, and what is still open

**Status: the HARD stall now auto-recovers. A softer degraded mode does not, and is
unexplained.** This is the bug behind issue #2 and the "sustained bulk latches the fabric"
family. Read this before touching `rtl819x_hang_check()` or `fabric_autoreset`.

## 1. ★ Reproduction — UDP floods it, TCP never does

```sh
ssh root@box 'iperf3 -s -D -1'
iperf3 -c <box> -u -b 200M -t 10        # wedges, reliably
```

| load | result |
|---|---|
| TCP 20 / 50 / 100 / 300M / unlimited | **clean.** `USEDDSC` never left 138 |
| **UDP 200M** | **wedge**, `USEDDSC` 435-470 |

★ TCP cannot trigger it at any rate — it backs off. Terminated TCP ran to the box's
152 Mbit/s CPU ceiling with the pool flat. That is why this looked erratic and
load-dependent: it needs a source that ignores congestion signals. A speedtest over
5 GHz does it too (the WiFi->ethernet bridge path).

## 2. Two distinct failure modes — do not conflate them

| | HARD stall | SOFT degradation |
|---|---|---|
| `rx_packets` | **frozen** | still advancing |
| `USEDDSC` | 435-470 (pool full) | 18 (pool near empty) |
| loss / RTT | 100%, unreachable | 16-20%, 1.1-2.7 s |
| napi | polling ~61/s, `rx_done=0` every time | polling normally |
| self-heals? | no — 386 s observed, then power cycle | **no** — survives 60 s idle |
| detector | ✅ now fires | ✗ invisible to it |

Issue #2 describes the SOFT one (`USEDDSC=91`, 40% loss, detector silent). The HARD one is
what a 200 Mbit/s UDP flood produces here.

## 3. Why the original detector could never fire

`rtl819x_hang_check()` declares a wedge on large-frame **FCS failures**:
`dfail >= 4 && dfail >= 4 * dok`. The hard stall delivers **zero frames**, so `dok` and
`dfail` are both 0 and that gate is structurally unreachable — no amount of waiting helps.
That is exactly why issue #2 sat 10+ minutes with the detector armed and silent.

The new RX-stall detector declares on: **pool full AND zero frames delivered for two
consecutive ~2.5 s windows.** ★ The pool occupancy is what separates it from an idle box —
idle is `USEDDSC` ~138-157 here, a wedge 435+ — so "no RX" alone is deliberately not
enough. No false positives observed across repeated floods and normal TCP.

## 4. ★ Recovery: level 2 is not enough, and level 3 was bridge-unsafe

| level | clears hard stall? | note |
|---|---|---|
| 2 (old bridge default) | ✗ | re-detected and re-ran **10×**, `USEDDSC` pinned at 435 |
| 3 | ✅ single pass | adds `rtl819x_fabric_full_reset()` — the part that matters |

But level 3 also calls `rtl865x_gw_rearm()`, which on a bridged AP reprograms the router
scaffolding and re-freezes L2 aging — the permanent-blackhole condition fixed in `5ff1a1f`.
Measured on a bridge with the re-arm active: ARP perfect (20/20) while ICMP sat at
**~1000 ms with 20% loss** and ssh timed out.

So `fabric_gw_rearm` (module param, default 1) now gates only that call, and `dir842-asic`
sets it to 0 in bridge role. Bridge role moves 2 -> 3 and gets the reset it needs without
the half that breaks it:

```
RX-STALL WEDGE detected (USEDDSC=459, rx_packets frozen at 4319) - auto recovery
recovery level 3 starting (rx_pkts=4319)
gw re-arm SKIPPED (fabric_gw_rearm=0, bridge role)
recovery level 3 complete
```

→ box back at **0% loss, 0.286 ms**, detector fired once, no loop.

## 5. ★ What is still open

Repeated floods leave the box in the SOFT degraded state of §2: 16-20% loss, 1.1-2.7 s RTT,
`rx_dropped=0`, `rx_errors=0`, `USEDDSC` stuck at **18** against a 157 baseline. It survives
60 s of idle and is cleared only by a power cycle, which restores `USEDDSC=157` and 0.3 ms.

Not yet separated: whether that residue comes from the floods themselves or from
`fabric_full_reset()` running **without** `gw_rearm` (i.e. whether skipping the re-arm
leaves the descriptor pool under-provisioned). A single recovery left the box perfectly
healthy, so it is not a one-shot cost — it accumulates. ★ Deciding this needs a run that
floods without ever triggering recovery, versus one that triggers recovery without
sustained flooding.

Also unfixed: the box remains trivially DoS-able by any host that can send it ~200 Mbit/s
of UDP. Recovery now happens, but prevention (rate limiting / flow control at the CPU port)
is untouched.

## 6. The diagnostic print — two bugs found in our own instrumentation

`++pc` used to sit in the third operand of the `||` guarding the print, so short-circuit
skipped it whenever pressure was true: **`poll#` froze exactly when the box was in
trouble**, logging `poll#20497` 2419 times and reading as "the napi loop has hung" when it
was polling fine. Fixed — the counter now advances during a wedge, which is what proved
napi was alive.

Worse, the pressure branch was true on **every poll** and unthrottled. Measured at 37.7
lines/s = 3772 B/s against a 3840 B/s console — **98% of the serial line**, with every napi
poll blocking on a synchronous printk. Now `pr_err_ratelimited`: 2.02 lines/s, 202 B/s, 5%.

★ But note: rate-limiting did **not** stop the wedge. The flood was aggravating, not
causal — an earlier hypothesis that the logging response *was* part of the wedge is
retracted.
