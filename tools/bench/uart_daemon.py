#!/usr/bin/env python3
"""Single persistent owner of the DIR-842 UART. Continuously drains+logs to
LOG (so `tail -f` keeps working), and polls CMDFILE for anything to send —
write a line to CMDFILE and this daemon will send it (as-is, no eol added;
include \r yourself) and delete the file once sent.

Usage:
  python3 tools/bench/uart_daemon.py > daemon.log 2>&1 &
  disown
  printf 'COMMAND\r' > uart-cmd.pending

Must be killed (pkill -f uart_daemon.py) before running sx/sb (XMODEM),
which needs exclusive read+write access to /dev/ttyUSB0. Restart it after
the XMODEM transfer finishes to resume logging/interaction.

LOG and CMDFILE live at the REPO ROOT (see SP below), which is where
uart.py, loader.py and autoboot-trial.py expect them.
"""
import os, time, serial

PORT = "/dev/ttyUSB0"
BAUD = 38400
# Repo root (two levels up from tools/bench/). LOG and CMDFILE must stay at the
# repo root: uart.py, loader.py and autoboot-trial.py all resolve them there.
SP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = SP + "/dir842-uart.log"
CMDFILE = SP + "/uart-cmd.pending"

def main():
    s = serial.Serial(PORT, BAUD, timeout=0.05, rtscts=False, dsrdtr=False)
    try:
        s.dtr = False
        s.rts = False
    except Exception:
        pass
    with open(LOG, "a", buffering=1) as logf:
        while True:
            chunk = s.read(4096)
            if chunk:
                logf.write(chunk.decode(errors="replace"))
                logf.flush()
            if os.path.exists(CMDFILE):
                with open(CMDFILE, "rb") as f:
                    data = f.read()
                os.remove(CMDFILE)
                if data:
                    s.write(data)
                    s.flush()
                    logf.write(f"\n[[daemon sent {data!r}]]\n")
                    logf.flush()

if __name__ == "__main__":
    main()
