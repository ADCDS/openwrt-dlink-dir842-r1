#!/usr/bin/env python3
# Sign a Realtek cr6b image for the DIR-842 D-Link v3.4.11B loader.
# Trailer the loader checks:  [cvimg image][ MD5(key || cvimg_image) : 16 ][ 00 C0 FF EE : 4 ]
# key is embedded in the bootcode (mtd0) at 0x291bc.
import sys, hashlib, struct
KEY = bytes.fromhex("8cefeb7b3b274629be7df7d453a64c29")
MAGIC = bytes([0x00,0xC0,0xFF,0xEE])
def sign(inp, outp, entry=None):
    d = bytearray(open(inp,'rb').read())
    assert d[:4]==b'cr6b', f"not a cr6b image: {d[:4]!r}"
    if entry is not None:
        d[4:8] = struct.pack('>I', entry)   # patch start/load address (BE)
    digest = hashlib.md5(KEY + bytes(d)).digest()
    out = bytes(d) + digest + MAGIC
    open(outp,'wb').write(out)
    print(f"signed: {inp} -> {outp}")
    print(f"  entry=0x{struct.unpack('>I',d[4:8])[0]:08x} burn=0x{struct.unpack('>I',d[8:12])[0]:08x} cr6b_len=0x{struct.unpack('>I',d[12:16])[0]:08x}")
    print(f"  cvimg image={len(d)}B  digest={digest.hex()}  total={len(out)}B (0x{len(out):x})")
if __name__=="__main__":
    entry = int(sys.argv[3],0) if len(sys.argv)>3 else None
    sign(sys.argv[1], sys.argv[2], entry)
