# M7 — RGMII Trunk L2-Forwarding Fix (8367S EXT1 ↔ SoC-P0)

**Date:** 2026-07-17 · **Status:** ✅ FIXED for loader/RAM boots (verified on HW); flashed-boot cold path still open.

This was the "final blocker" for the DIR-842 gateway: L2 **data** did not forward across
the RTL8197F↔RTL8367S RGMII trunk in either direction, even though every link reported
up 1000/full and VLANs applied.

## Topology recap

Two cascaded switches in series:

```
CPU (eth0) ── SoC-internal switch (rtl865x, driver rtl819x-eth)
                     │ port0 = RGMII trunk  ⇄  EXT1/port6  external RTL8367S (driver rtl8367b, SMI)
                                                   └ ports 0-4 = the 5 physical GbE jacks
```

Host is on 8367S **port 2**; the SoC↔8367S trunk is SoC **port0 (P0GMIICR@swcore+0x414C)** ⇄
8367S **EXT1 (port6)**. The loader (`init_97f_8367r` + `RTL8367R_init`) brings up **both ends**
on its TFTP/monitor path — which is why TFTP RAM-boot works.

## Symptom

- SoC `eth0 rx_packets = 0`, `CPUIISR = 0` (hardware RX-interrupt status, not just stats).
- 8367S `port6 ifIn = 0` — nothing arrives from the SoC.
- Link forces up 1000/full both ends; VLANs apply; **zero L2 frames pass either way.**

## Diagnostic method (the key lever)

The `rtl8366_smi` driver exposes **`/sys/kernel/debug/rtl8367b/{reg,val,mibs}`**:

- `echo 0xNNNN > reg; cat val` → `reg = 0xNNNN, val = 0xVVVV` (read ANY 8367S SMI register)
- `echo 0xVV > val` (write it)
- `cat mibs` → per-port MIB counters

This split the fault precisely. The **8367S per-port `etherStats`** counters showed:

| port | ifInOctets | etherStatsOctets | meaning |
|------|-----------|------------------|---------|
| 2 (host jack) | 82944 | 82944 | ingress from host works; egress 0 |
| 6 (EXT1 trunk) | **0** | **86208** | **egresses 816 pkts to SoC**, ingress 0 |

⇒ **The 8367S internal L2 WORKS** (forwards jack port2 → EXT1, frames egress toward the SoC),
but the **RGMII trunk itself delivers nothing.** SoC side (read live by cycling `ip link set
eth0 down/up` → the driver's `trunk-pre/post` dump, since CONFIG_DEVMEM is off and busybox has
no `devmem`): `P0GMIICR = 0x00037d55` = **Conf_done (bit6) set + TX-delay=1 + RX-delay=5** — the
loader's correct values, intact. `PITCR=1`, `MACCR bit12=1`.

Then **exhaustively swept the 8367S EXT1 side live** — mode (0x1305 bits7:4 = 0..7), delay
(0x1307 = 0..15), pause off (0x1311 = 0x1016), rate meters (0x00cf/0x0398 → already unlimited),
EXT2/port7 bring-up — **every combination transmits into a void; the SoC receives nothing.**
An ARP flood from the SoC reached **no** 8367S port. So the break is not a register *value* on
either configured end.

A **Fable-5 research agent** (bootcode `RTL83XX_init`/`init_97f_8367r` + rtl8367c SDK + mainline
`rtl8365mb`) independently confirmed: the switch-side sequence is loader-exact; PORT_ISOLATION
0x08A2 is a PERMIT mask (0xFF = allow, not inverted); STP default = forwarding; `0x1D11` bits
6/11 = 0 IS the correct RGMII mux; the SoC-side `Conf_done` gate is where the loader's coherent
bring-up lives.

## Root cause

`rtl8367b_reset_chip()` fires an **in-kernel gpio474 HW-reset at driver probe — *after* the
loader has already brought up the SoC-P0 side of the trunk.** That re-straps the 8367S, and the
driver's *partial* EXT1 reconfig then overwrites the loader's *complete* analog/PLL bring-up with
an incomplete replica. The two ends of the RGMII pair desync: link forces up, but the data plane
is dead both directions and no register poke re-syncs it (the analog/PLL state is gone).

The `rtl8367b_setup()` comment already warned this reset "must happen BEFORE the loader
initialises the SoC-side trunk … doing it here would sabotage that sequence" — the in-kernel
pulse had been re-enabled, a regression.

## The fix (`rtl8367b.c`)

On a **loader-configured boot, preserve the loader's working power-on trunk**; only cold-bring-up
when the loader was absent (a true flashed boot):

- `rtl8367b_reset_chip()`: **no gpio474 pulse** for the 8367S (`return 0`).
- `rtl8367b_setup()`: gate the EXT1 cold bring-up on **`0x1219 == 0`** (the loader's
  `RTL8367R_init` sets CPU-port-mask = 0x40; strap default = 0). Loader present ⇒ skip & log
  `preserving power-on RGMII trunk`. Loader absent ⇒ run the cold bring-up.

## Verification (on hardware)

- Boot log: `RTL8367S: loader-configured uplink (0x1219=0040) — preserving power-on RGMII trunk`.
- **host → box ping: 15–20/20, 0% loss, ~0.4 ms** (rock-solid, repeated across boots).
- **box → host: 5/5** with the host MAC resolved.
- **Reboot-survives:** a clean software reboot → loader reconfigures → driver preserves → trunk
  up again (no power cycle needed; RAM-boot reboot is reliable).

## Remaining (separate issues)

1. **Box-*initiated* traffic / natural ARP is flaky** (task #13). host→box is solid, but the SoC
   *internal* switch does not reliably flood CPU-originated unknown-unicast/broadcast out the
   trunk port, and/or L2 entries age out fast — box→host works only while the host MAC is warm
   from recent host→box traffic. Static ARP both ends is the historical workaround. This is a
   SoC-L2 flood/aging matter (ties into M7.3 "aging refresh" / restoring M4-style direct-port-flood
   TX), **not** the RGMII trunk.
2. **Flashed-boot cold bring-up** (task #14): the `0x1219==0` path is still the incomplete replica.
   A real flashed gateway (no loader `RTL8367R_init`) needs the full loader recipe: SoC-side
   "Fork B" (`PCRP0` force+MacSwReset+EnablePHYIf, `P0GMIICR` Conf_done LAST, `PITCR` bit0,
   `MACCR` bit12 — the `(void)GW_*` regs in `rtl865x_asichal.c`) + complete 8367S init + a
   loader-timing 1000ms×3 reset.

## Files

- `target/linux/generic/files/drivers/net/phy/rtl8367b.c` — the fix (reset_chip + setup gate).
- `target/linux/realtek/files-4.14/arch/mips/realtek/setup.c` — CLKMANAGE bit11 switch-clock force
  (mode-2 freeze fix, kept).
- `target/linux/realtek/files-4.14/drivers/net/ethernet/rtl819x/rtl819x-eth.c` — trunk-pre/post
  diagnostic dump (retained; useful for SoC-side debugging).
- `target/linux/realtek/files-4.14/drivers/spi/spi-sheipa.c` — bounded SPI waits + the
  `SPI_HANG_TRACE` breadcrumb tracer (ships **disabled**).
