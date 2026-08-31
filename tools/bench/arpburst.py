#!/usr/bin/env python3
"""Send N raw ARP requests for TARGET from IFACE and count the unicast replies
that come back. Needs root (AF_PACKET).

  sudo arpburst.py [IFACE] [N] [TARGET_IP] [GAP_SECONDS]

Each request forces the DIR-842 to emit exactly one CPU-generated unicast ARP
reply, so "replies seen" is a direct measure of CPU-originated unicast TX
actually reaching the wire -- the core instrument for the cold-boot CPU-TX
wedge investigation (see HANDOFF-5.md).
"""
import socket, struct, sys, time, fcntl, threading

IFACE = sys.argv[1] if len(sys.argv) > 1 else "enp4s0"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
TARGET = sys.argv[3] if len(sys.argv) > 3 else "192.168.100.3"
GAP = float(sys.argv[4]) if len(sys.argv) > 4 else 0.01

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0806))
s.bind((IFACE, 0))
smac = fcntl.ioctl(s.fileno(), 0x8927,
                   struct.pack("256s", IFACE.encode()[:15]))[18:24]
s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sip = fcntl.ioctl(s4.fileno(), 0x8915,
                  struct.pack("256s", IFACE.encode()[:15]))[20:24]
tip = socket.inet_aton(TARGET)
frame = (b"\xff" * 6 + smac + b"\x08\x06"
         + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
         + smac + sip + b"\x00" * 6 + tip)

replies, stop = [0], [False]


def rx():
    s.settimeout(0.2)
    while not stop[0]:
        try:
            pkt = s.recv(2048)
        except socket.timeout:
            continue
        if (len(pkt) >= 42 and pkt[12:14] == b"\x08\x06"
                and pkt[20:22] == b"\x00\x02" and pkt[28:32] == tip):
            replies[0] += 1


t = threading.Thread(target=rx); t.start()
t0 = time.time()
for _ in range(N):
    s.send(frame); time.sleep(GAP)
time.sleep(1.0); stop[0] = True; t.join()
print(f"sent {N} ARP requests for {TARGET} on {IFACE} in "
      f"{time.time() - t0:.1f}s; replies seen: {replies[0]}")
