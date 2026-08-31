#!/usr/bin/env python3
"""Send one shell command to the DIR-842 over the running uart_daemon.py and
print the reply.

  uart.py [-t TIMEOUT] [-q] CMD...   # CMD words are joined with spaces
  uart.py -r                         # just send a bare CR

Requires uart_daemon.py to be running (see docs/BENCH.md / HANDOFF.md §4.2).
If a reply looks garbled or the shell is stuck at a '>' continuation prompt,
send a bare CR (-r) and retry with a SIMPLER command -- the box's UART drops
characters under printk bursts; keep commands short and avoid long pipelines.
"""
import os, sys, time, re

SP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG = SP + "/dir842-uart.log"
CMD = SP + "/uart-cmd.pending"
PROMPT = re.compile(rb"root@[^\s]*:[^\s]*# $|<RealTek>$")


def send_raw(data, chunk=24, gap=0.06):
    for i in range(0, len(data), chunk):
        while os.path.exists(CMD):
            time.sleep(0.01)
        open(CMD, "wb").write(data[i:i + chunk])
        time.sleep(gap)
    while os.path.exists(CMD):
        time.sleep(0.01)


def main():
    args = sys.argv[1:]
    timeout, quiet = 20.0, False
    if args and args[0] == "-t":
        timeout = float(args[1]); args = args[2:]
    if args and args[0] == "-q":
        quiet = True; args = args[1:]
    cmd = "" if args == ["-r"] else " ".join(args)

    start = os.path.getsize(LOG)
    send_raw(cmd.encode() + b"\r")
    deadline, out, last = time.time() + timeout, b"", time.time()
    while time.time() < deadline:
        with open(LOG, "rb") as f:
            f.seek(start); new = f.read()
        if len(new) != len(out):
            out, last = new, time.time()
        body = b"\n".join(l for l in out.split(b"\n")
                          if not l.startswith(b"[[daemon sent"))
        if PROMPT.search(body.rstrip(b"\r")) and time.time() - last > 0.25:
            break
        time.sleep(0.1)
    text = "\n".join(l for l in out.decode(errors="replace").split("\n")
                     if not l.startswith("[[daemon sent"))
    if cmd and text.lstrip().startswith(cmd):
        text = text.lstrip()[len(cmd):]
    if not quiet:
        sys.stdout.write(text.strip("\r\n") + "\n")


main()
