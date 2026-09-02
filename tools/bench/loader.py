#!/usr/bin/env python3
"""Drive the DIR-842's RealTek boot monitor over the running uart_daemon.py.

  loader.py <op> [args] [<op> [args] ...]

  catch                 power-cycle (tomada) + ESC-spam to catch the monitor
  flr                   reload the CURRENTLY-FLASHED kernel NOR->RAM (no flash write)
  dw <ADDR> <N>         dump N 32-bit words at ADDR (hex, no 0x)
  ew <ADDR> <VAL>       write one 32-bit register (hex, no 0x)
  j <ADDR>              jump into a RAM image
  flash <FILE>          AUTOBURN 1 + TFTP-upload FILE, box auto-reboots
  ramload <FILE>        AUTOBURN 0 + TFTP-upload FILE to 0x82000000 (then use j)
  cmd "<STR>"           send an arbitrary monitor command

Notes learned the hard way (see HANDOFF-5.md §4):
 * `flr` can take minutes; its "Flash Read Successed!" confirmation is polled
   for up to 480s and a miss is reported but NOT treated as fatal.
 * `j` sent right after flr's Y can race the loader's own "Flash Read
   Successed!" printf and get fragmented into "Unknown command!". Harmless --
   just run `loader.py j <addr>` again; it works on the retry.
 * IMPORTANT: run this as the NORMAL user, never under sudo -- tomada reads
   ~/ekaza-t206m/device.json from the invoking user's home and silently fails
   to power-cycle under sudo, which looks like a boot timeout.
"""
import os, sys, time, re, subprocess, threading

SP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = SP + "/dir842-uart.log"
CMD = SP + "/uart-cmd.pending"
TTY = "/dev/ttyUSB0"
TOMADA = os.path.expanduser("~/.local/bin/tomada")


def size():
    return os.path.getsize(LOG)


def tail(off):
    f = open(LOG, "rb"); f.seek(off); return f.read().decode(errors="replace")


def send(s, gap=0.06, chunk=16):
    b = s.encode()
    for i in range(0, len(b), chunk):
        while os.path.exists(CMD):
            time.sleep(0.01)
        open(CMD, "wb").write(b[i:i + chunk]); time.sleep(gap)
    while os.path.exists(CMD):
        time.sleep(0.01)


def waitfor(pat, off, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if re.search(pat, tail(off), re.S):
            return True
        time.sleep(0.2)
    return False


def cmd(s, timeout=15):
    off = size(); send(s + "\r")
    waitfor(r"<RealTek>\s*$", off, timeout)
    return tail(off)


def catch():
    # ★ Open the tty ONCE and write ESC in a loop.
    #
    # Two earlier versions both failed, each for its own reason, and both
    # surfaced only as a bare "catch: FAIL" while the box power-cycled fine:
    #
    #  1. `printf "\033" > /dev/ttyUSB0` in a 25 Hz shell loop -- REOPENING the
    #     tty that fast starves uart_daemon.py's read loop, so the loader banner
    #     never reaches the log and waitfor() times out. (Verified: the same
    #     power-cycle without the spam captures the banner perfectly.)
    #  2. Spamming through the daemon's command file -- the daemon logs a marker
    #     per send, which throttles it to ~8 Hz and floods the log.
    #
    # Writing to an already-open fd costs nothing and does not disturb the
    # daemon's reads.
    flag = SP + "/esc-spam.flag"; open(flag, "w").close()

    def spam_tty():
        try:
            f = open(TTY, "wb", buffering=0)
        except OSError:
            return
        with f:
            while os.path.exists(flag):
                try:
                    f.write(b"\x1b")
                except OSError:
                    return
                time.sleep(0.04)

    spam = threading.Thread(target=spam_tty, daemon=True)
    spam.start()
    subprocess.run([TOMADA, "off"], capture_output=True); time.sleep(4)
    off = size(); subprocess.run([TOMADA, "on"], capture_output=True)
    ok = waitfor(r"Escape booting by user", off, 45)
    os.remove(flag); spam.join(timeout=2)
    print("catch:", "OK" if ok else "FAIL")
    if not ok:
        sys.exit(1)
    time.sleep(2.5); send("\r"); time.sleep(2)
    while True:
        a = size(); time.sleep(1.5)
        if size() == a:
            break


def flr():
    off = size(); send("FLR A0FFFFF0 40000 543000\r")
    if not waitfor(r"\(Y\)es , \(N\)o", off, 20):
        print("flr: no prompt"); sys.exit(2)
    off = size(); send("Y")
    ok = waitfor(r"Flash Read Successed", off, 480)
    print("flr:", "OK" if ok else "UNCONFIRMED (continuing)")
    time.sleep(1.5)


def tftp_put(img, autoburn):
    off = size(); send("IPCONFIG 192.168.0.1\r")
    print("ipconfig:", "OK" if waitfor(r"192\.168\.0\.1", off, 8) else "FAIL")
    off = size(); send(f"AUTOBURN {autoburn}\r")
    print(f"autoburn{autoburn}:",
          "OK" if waitfor(rf"AutoBurning={autoburn}", off, 8) else "FAIL")
    if not autoburn:
        off = size(); send("LOADADDR 82000000\r")
        print("loadaddr:", "OK" if waitfor(r"82000000", off, 8) else "FAIL")
    off = size(); send("TFTP +\r")
    print("tftp+:", "OK" if waitfor(r"tftp_boot", off, 8) else "FAIL")
    off = size()
    r = subprocess.run(["curl", "-s", "-S", "--connect-timeout", "10",
                        "--max-time", "240", "-T", img,
                        "tftp://192.168.0.1/img"],
                       capture_output=True, text=True)
    print("curl rc", r.returncode, r.stderr.strip()[:150])
    if autoburn:
        ok = waitfor(r"Flash Write Successed!.*Flash Write Successed!", off, 300)
        print("flash write:", "OK (auto-reboot)" if ok else "TIMEOUT")
    else:
        print("tftp upload:",
              "OK" if waitfor(r"TftpExitCode: OK", off, 60) else "FAIL")


def dw(addr, n):
    d = cmd(f"DW {addr} {n}")
    for base, ws in re.findall(r"([0-9A-Fa-f]{8}):\s+((?:[0-9A-Fa-f]{8}\s*)+)", d):
        print(" ", base + ":", " ".join(re.findall(r"[0-9A-Fa-f]{8}", ws)))


def j(addr):
    off = size(); send(f"J {addr}\r")
    print("kernel console:",
          "OK" if waitfor(r"press Enter to activate", off, 220) else "TIMEOUT")


a = sys.argv[1:]; i = 0
while i < len(a):
    op = a[i]
    if op == "catch": catch(); i += 1
    elif op == "flr": flr(); i += 1
    elif op == "dw": dw(a[i+1], a[i+2]); i += 3
    elif op == "ew": print(cmd(f"EW {a[i+1]} {a[i+2]}").strip()[-60:]); i += 3
    elif op == "j": j(a[i+1]); i += 2
    elif op == "flash": tftp_put(a[i+1], 1); i += 2
    elif op == "ramload": tftp_put(a[i+1], 0); i += 2
    elif op == "cmd": print(cmd(a[i+1])); i += 2
    else: print("unknown op", op); sys.exit(9)
