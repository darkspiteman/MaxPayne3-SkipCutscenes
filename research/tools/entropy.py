"""Read-only: per-block Shannon entropy of the .text section + hex dump of the .rkstr entry stub.

Usage: python -I entropy.py <exe> [block_size]
"""
import math
import struct
import sys
from collections import Counter


def main():
    path = sys.argv[1]
    block = int(sys.argv[2]) if len(sys.argv) > 2 else 0x10000
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    imgbase = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    so = pe + 24 + optsz
    secs = {}
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", data, so + i * 40)
        secs[name.rstrip(b"\0").decode("latin1")] = (vsize, va, rsize, roff)
    vsize, va, rsize, roff = secs[".text"]
    runs = []
    for off in range(0, rsize, block):
        chunk = data[roff + off:roff + min(off + block, rsize)]
        c = Counter(chunk)
        ent = -sum(n / len(chunk) * math.log2(n / len(chunk)) for n in c.values())
        zero = c.get(0, 0) / len(chunk)
        kind = "ZERO" if zero > 0.98 else "HIGH" if ent > 7.5 else "code"
        if runs and runs[-1][0] == kind:
            runs[-1][2] = off + len(chunk)
        else:
            runs.append([kind, off, off + len(chunk), ent])
    for kind, a, b, ent in runs:
        print(f"{kind:5s} VA 0x{imgbase + va + a:08X}..0x{imgbase + va + b:08X} ({(b - a) // 1024} KiB) first-block entropy={ent:.2f}")
    for name in (".rkstr",):
        vsize, va, rsize, roff = secs[name]
        print(f"{name} @VA 0x{imgbase + va:08X}:")
        blob = data[roff:roff + min(vsize, rsize)]
        for i in range(0, len(blob), 32):
            print(f"  {i:04X}: {blob[i:i + 32].hex(' ').upper()}")
    vsize, va, rsize, roff = secs[".tbm"]
    blob = data[roff:roff + 256]
    print(".tbm head:")
    for i in range(0, len(blob), 32):
        print(f"  {i:04X}: {blob[i:i + 32].hex(' ').upper()}")


if __name__ == "__main__":
    main()
