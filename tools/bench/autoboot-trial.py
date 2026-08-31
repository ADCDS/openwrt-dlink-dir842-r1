#!/usr/bin/env python3
"""One genuine hands-off power-cycle autoboot trial, with the ARP acceptance test.

Why this exists: this box has a bug class (see docs/COLD-BOOT-TX-WEDGE.md) that
only appears on a real power-on autoboot -- never when you boot via the loader's
`J` command, and not reliably on the first try.  RETRACTIONS-AND-METHOD.md rule 7
therefore requires >=5 independent power-cycle trials before believing any result
in this area.  Run this in a loop; do not trust a single pass.

  python3 tools/bench/autoboot-trial.py            # one trial
  DIR842_IFACE=eth0 DIR842_TARGET=192.168.1.1 python3 tools/bench/autoboot-trial.py
  python3 tools/bench/autoboot-trial.py --diag     # ...and dump reservation proof

★ Do NOT run under sudo: tomada reads ~/ekaza-t206m/device.json from the invoking
user's home and fails silently as root, which shows up as a bogus NO_CONSOLE.
The raw-socket burst shells out to sudo on its own.
"""
import subprocess, time, sys, os, re

SP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = SP + "/dir842-uart.log"
TOMADA = os.path.expanduser("~/.local/bin/tomada")
IFACE = os.environ.get("DIR842_IFACE", "enp4s0")
TARGET = os.environ.get("DIR842_TARGET", "192.168.100.3")

def size(): return os.path.getsize(LOG)
def tail(o):
    with open(LOG, "rb") as f:
        f.seek(o); return f.read().decode(errors="replace")

def uart(cmd, t=15):
    r = subprocess.run(["python3", SP + "/tools/bench/uart.py", "-t", str(t), cmd],
                       capture_output=True, text=True)
    return r.stdout.strip()

def burst(n=20):
    r = subprocess.run(["sudo", "-n", "python3", SP + "/tools/bench/arpburst.py",
                        IFACE, str(n), TARGET, "0.05"], capture_output=True, text=True)
    m = re.search(r"replies seen: (\d+)", r.stdout)
    return int(m.group(1)) if m else -1

off = size()
subprocess.run([TOMADA, "off"], capture_output=True); time.sleep(4)
subprocess.run([TOMADA, "on"], capture_output=True)

t0 = time.time(); console = False
while time.time() - t0 < 150:
    if "press Enter to activate" in tail(off):
        console = True; break
    time.sleep(2)
if not console:
    print("RESULT: NO_CONSOLE (is uart_daemon.py running? is tomada reachable?)")
    sys.exit(1)

boot = tail(off)
time.sleep(45); a = burst()
time.sleep(25); b = burst()
ok = b >= 18
print(f"RESULT: t45={a} t70={b} {'PASS' if ok else 'FAIL'}")

# ★ The reservation must be proven, never assumed -- assuming it is what caused
# the three-session hunt in the first place.  A ramoops line in the boot log only
# means the driver bound; it says nothing about whether the memory was reserved.
if "--diag" in sys.argv or not ok:
    uart("dmesg -n 1")
    for m in re.findall(r".*(?:reserved|ramoops|pstore).*", boot, re.I)[:12]:
        print("  boot:", m.strip())
    print("  iomem:", uart("grep -i 'System RAM' /proc/iomem"))
    print("  pstore:", uart("ls /sys/fs/pstore 2>/dev/null | head; echo rc=$?"))
    print("  memfree:", uart("head -2 /proc/meminfo | tr '\\n' ' '"))
if not ok:
    print("  ip:", uart("uci get network.lan.ipaddr"))
    print("  br-lan:", uart("ip -4 addr show br-lan | grep inet"))
    print("  p6 outUc:", uart("swconfig dev switch0 port 6 get mib | grep ifOutUcastPkts"))
    print("  tx/rx:", uart("cat /sys/class/net/eth0/statistics/tx_packets "
                           "/sys/class/net/eth0/statistics/rx_packets | tr '\\n' ' '"))
sys.exit(0 if ok else 1)
