# DIR-842 R1 kernel 6.18 wedge: first hardware test and stock A/B

**2026-09-24; one DIR-842 R1, isolated wired bench.** This is the first hardware test of the changes in `8f88100` (CPU-port DMA/recovery) and `e9c03bc` (vendor wifi proc_ops), described in [WEDGE-ROOT-CAUSE.md](WEDGE-ROOT-CAUSE.md). That document's analysis is not itself proof that the proposed changes cure the wedge. All network figures below are *host → router CPU-port* unless otherwise stated; we did not test two external hosts switching through the box or LAN → WAN routing in this A/B. Private unit-specific flash dumps, SSH keys, bench profile, and raw UART logs are **not published**.

## Observed on patched Linux 6.18.44

- The D-Link loader validated and booted the signed image from NOR. Linux reached userspace and remained controllable over UART. On an isolated DSA bridge, the box also recovered wired IP and SSH after the correct LAN configuration was installed; a cold boot took about two minutes to bring the network up.
- **The size-selective failure still occurred.** On one boot, small ICMP requests succeeded while larger requests (88-byte ICMP payload and up, including 1400 bytes) failed and TCP connected but SSH stalled before its banner. The CPU-port DMA register remained `CPUICR=e4000000 (burst=2)` and `DMA_CR0=0003a0ce`; `rx_mbuf_desync=0` and `rx_mbuf_bad=0`. A manually triggered level-3 switch-fabric reset completed and immediately restored 1400-byte ping and SSH without a reboot. This is evidence against **the old TX doorbell clearing the burst bit being the sole cause** of the wedge. It does not distinguish RX loss from reply loss without a packet capture or stack counter at the moment of failure.
- The `tx_kick_clear_burst=1` A/B knob changed the live register to `CPUICR=c4000000 (burst=0)` and `DMA_CR0=0003a06f`; turning it back off and requesting recovery level 1 restored the patched register values. A short large-ping sequence succeeded even with the old setting **after** recovery; that short trial is not a long-run stability comparison.
- Reading `/proc/wlan0/mib_all` and `/proc/wlan0/sta_info` returned data without a crash, consistent with the proc_ops fix.
- A full dual-radio/LuCI build hit OOM under one host-to-router load run: netifd, rpcd, and hostapd were killed. Wireless was disabled for later wired-only tests. This is a separate stability warning, not proof of a DMA wedge.

## Matched stock comparison on the same unit

Only the *firmware* partition was rewritten between tests. A private full-NOR stock dump's MAC partition matched the current unit byte-for-byte; bootloader, MAC and config partitions were not overwritten. The original D-Link stock image boots Linux **3.10.90+** and uses LAN `192.168.1.1`. The public release image must **not** contain the private bridge profile used on this bench.

| Isolated host → router test | Patched 6.18 | D-Link stock 3.10 |
|---|---:|---:|
| Fresh-boot ICMP sweep, payload 20–1400 bytes | 33/33 received | 33/33 received |
| 8-second CPU-terminating TCP stream | `iperf3`: 145 MiB, 152 Mbit/s, **901 retransmissions** | `nc` sink: ~102 MiB, ~102 Mbit/s; retransmissions not measured |
| Post-stream size sweep, 56–1400 bytes | 21/21 received | 21/21 received |
| Second stock cold boot, 8-second stream | Not run as part of this direct A/B | Booted; ~107 MiB transferred; 25/25 follow-up ICMP received |
| Extra 1400-byte-payload burst | Not run as part of this direct A/B | 2000/2000 received |

**Interpretation:** Stock did not wedge under these bounded tests. The patched 6.18 image also did not reproduce its earlier, *intermittent* size-selective failure on the fresh comparison boot. Therefore the A/B does **not** establish that stock can never wedge, nor that the 6.18 patch cures the problem. The TCP generators and offered rates differ; comparing their retransmission counts or inferred packet loss numerically would be invalid. A future decisive test needs identical endpoints/tools/rates, several cold boots per image, large/small probes before and after load, and a physical switch-port or peer-side capture. Switch-only LAN↔LAN operation, VLAN behavior and long-run stability remain unverified.

## Deployment and recovery cautions

- **Experimental 6.18 image, not a replacement for the kernel-4.14 `main` release on a production path.** The CPU-port large-frame failure can cut off SSH and management. A level-3 reset worked on this bench, but there is no calibrated hardware watchdog; do not rely on automatic recovery or a successful single boot.
- Do not migrate 4.14 swconfig configuration (`eth0.1`/`eth0.2`) to 6.18 DSA (`lan1`–`lan4`, `wan`). During the first cross-generation test, raw `mtd` writing left the 4.14 JFFS2 overlay in place, yielding an invalid `br-lan` with `eth0.2` until fixed through UART. A cross-generation upgrade must deliberately wipe incompatible configuration. The image advertises `compat_version=2.0`; a 4.14 upgrader at 1.0 may refuse ordinary `sysupgrade -n` and force/loader flashing requires additional care.
- This branch's default CPU transmit behavior `dsa_tx_flood=1` can put CPU-originated frames onto the WAN jack; do not presume port isolation. Bridge-role level-3 recovery with default `fabric_gw_rearm=1` can also freeze L2 aging by programming router scaffolding. Test both before putting this box inline as a household switch.
- On a fresh generic release image, **configure SSH credentials and secure/disable the default 2.4 GHz AP** before connecting it to an untrusted network. The bench-only profile used for these measurements is not part of public images. The experimental public image was rebuilt without `PROFILE`; its rootfs contains neither the private authorized key nor the bench UCI defaults. This public build was checked for format/content and built from the same kernel source, but **was not separately flashed and soak-tested as this exact binary**.

The stock-versus-6.18 A/B ended with the patched image restored on NOR. The loader confirmed the firmware write; the first 6.18 boot after stock required JFFS2 regeneration and produced a new SSH host key. We verified kernel/board identity over UART and SSH, then reasserted the isolated bench settings: management LAN, bridge role, both radios disabled, WAN protocols `none`, and DHCP/firewall services disabled. SSH and 1400-byte ICMP worked. These are runtime/private configuration choices, **not promises about the public release defaults**. Neither firmware reproduced the intermittent large-frame wedge during this particular fresh-boot comparison; do not overinterpret those negative trials.
