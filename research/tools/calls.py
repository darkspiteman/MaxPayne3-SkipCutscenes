"""Read-only helpers over the unencrypted part of MaxPayne3.exe .text.

  python -I calls.py <exe> pair            -> list `push 1; call A; add esp,4; mov ecx,eax; call B` sites grouped by (A,B)
  python -I calls.py <exe> xref <VA hex>   -> list E8/E9 rel32 sites that target VA
  python -I calls.py <exe> dump <VA hex> [len] -> hex dump at VA
"""
import struct
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from probe import compile_pattern, load, off2va, va2off  # noqa: E402


def text_range(secs):
    t = next(s for s in secs if s[0] == ".text")
    return t[4], t[4] + t[3]


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    data, imgbase, secs = load(path)
    tstart, tend = text_range(secs)
    if cmd == "pair":
        groups = defaultdict(list)
        rx = compile_pattern("6A 01 E8 ? ? ? ? 83 C4 04 8B C8 E8 ? ? ? ?")
        for m in rx.finditer(data[tstart:tend]):
            off = m.start() + tstart
            va = off2va(imgbase, secs, off)
            a = (va + 7 + struct.unpack_from("<i", data, off + 3)[0]) & 0xFFFFFFFF
            b = (va + 17 + struct.unpack_from("<i", data, off + 13)[0]) & 0xFFFFFFFF
            groups[(a, b)].append(va)
        for (a, b), sites in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            print(f"GetControls?=0x{a:08X} check=0x{b:08X} n={len(sites)} sites: " + ", ".join(f"0x{s:08X}" for s in sites[:12]))
    elif cmd == "xref":
        target = int(sys.argv[3], 16)
        n = 0
        for off in range(tstart, tend - 5):
            if data[off] in (0xE8, 0xE9):
                va = off2va(imgbase, secs, off)
                if (va + 5 + struct.unpack_from("<i", data, off + 1)[0]) & 0xFFFFFFFF == target:
                    print(f"  0x{va:08X}: {'call' if data[off] == 0xE8 else 'jmp '} 0x{target:08X}")
                    n += 1
        print(f"xrefs: {n}")
    elif cmd == "dump":
        va = int(sys.argv[3], 16)
        ln = int(sys.argv[4]) if len(sys.argv) > 4 else 128
        off = va2off(imgbase, secs, va)
        for i in range(0, ln, 16):
            print(f"  0x{va + i:08X}: {data[off + i:off + i + 16].hex(' ').upper()}")


if __name__ == "__main__":
    main()
