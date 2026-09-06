# Prebuilt images (`port/main-6.18`)

Built 2026-09-05 from `port/main-6.18` (kernel 6.18.44, OpenWrt main), natively — no
container, no pinned toolchain. `seed.config` (the release package set): **both radios**
(the vendor `rtl8192cd` 2.4 GHz driver alongside `rtw88`/RTL8822BE 5 GHz — the manifest
confirms `kmod-rtl8192cd` and `rtl8822be-firmware` are both in this exact build), firewall4,
PPPoE, and LuCI. This build adds three fixes on top of the 2026-09-04 release: a kernel
warning on every boot from the vendor 2.4 GHz driver writing its own MAC address
incorrectly (harmless in practice, now fixed properly), a defensive bound on a vendor
debug-output routine that could otherwise loop far past its intended array size if a length
field it trusts were ever corrupted (proposed fix for a real, previously-observed hard
crash reading certain wireless statistics — compiles clean and boots clean, but not yet
re-confirmed against a live reproduction of that original crash), and a real, live-tested
partial fix to hardware NAT acceleration's forward-throughput problem (a missing priority
field on accelerated flows — measured 15-30x improvement on hardware, does not yet fully
close the gap; see the status doc's M5/§4 section for the full investigation and numbers).

**This is a rebase-in-progress, not a finished release.** Wired ethernet and reliable
software-forwarded LAN↔WAN traffic both work well; true zero-CPU ASIC hardware NAT
acceleration is the one piece that does not work yet (a real root-cause fix for an earlier
bulk-transfer-stall bug already landed — `flow_offloading_hw=0` is the shipped default —
but genuine silicon-accelerated throughput remains an open investigation, see the status
doc §4). **Correction from an earlier version of this file**: it previously claimed a real
client associates and gets DHCP on both radios. That is only confirmed for the vendor
2.4 GHz radio. The 5 GHz radio has been confirmed beaconing and bridging correctly, but no
wireless client has yet actually associated to it and received a DHCP lease over the air —
the only 5 GHz-capable test device available this session couldn't hear the AP reliably
enough to complete that specific check. Full status: the root
[`../README.md`](../README.md) and
[`../docs/PORT-MAIN-6.18-STATUS.md`](../docs/PORT-MAIN-6.18-STATUS.md). If you want gigabit
hardware-accelerated NAT specifically, the `main` branch's images still deliver that; on
everything else this build has reached parity.

Verified before the 2026-09-04 base of this build was staged: `sysupgrade -n` clean, LuCI
reachable and serving over HTTP, and — on a genuinely fresh factory-flash boot with zero
manual configuration — bulk LAN↔WAN transfers completing cleanly under packet-capture
verification (3 of 4 runs 99.6%-100%+ complete). Also fixed in that base: a real boot-time
PCIe-probe race that could occasionally leave the 5 GHz radio down (`retry_setup_failed`)
is now auto-recovered by a new `asic-wifi-settle` service, verified live on a cold boot; and
the vendor 2.4 GHz driver's long-run stability was directly reconfirmed at 3h20m continuous
uptime with both radios still up and zero OOM kills. **`bootgate.sh`'s 10/10 clean-cold-boot
claim predates this exact binary** — the underlying tree has been reflashed many times
since that run, including after two real, unplanned failure events found later in the same
session (a hard hang recovered only by a power cycle, and once, wire-level packet
corruption on the very next boot after that). A fresh 10/10 run against this precise binary
is still outstanding; see the status doc's M6 section.

> **The `.bin` files are not committed to git** (they are build artifacts). Build them
> yourself (`git checkout port/main-6.18 && ./build.sh`) or get them from a release attached
> to this branch if one exists, then verify:
>
> ```sh
> sha256sum --ignore-missing -c sha256sums.txt
> ```
>
> The checksums here are the authoritative record of what this build contains.

| file | use |
|---|---|
| `*-initramfs-kernel.bin` | RAM boot via the loader (TFTP/XMODEM) — **never writes flash**; a power-cycle discards it. Try this first. Built from `seed-min.config` (no LuCI, no wireless) if you need it to fit comfortably in this board's 64 MB RAM — see the status doc §5. |
| `*-squashfs-factory.bin` | **The one most people want.** Upload through stock's web UI ([OTA-INSTALL](../docs/OTA-INSTALL.md), no serial), or NOR-flash via the loader's `AUTOBURN`. Already carries the D-Link keyed-MD5 trailer. |
| `*-squashfs-sysupgrade.bin` | NOR flash via `sysupgrade` from a running OpenWrt. |

⚠ Both squashfs images **replace stock firmware**. Back up all 8 MB of NOR first —
after flashing, stock exists only in your backup. And read the root README's security
box: default images ship with the 5 GHz radio **disabled and unconfigured** (no SSID, no
key baked in) and **no root password set**.
