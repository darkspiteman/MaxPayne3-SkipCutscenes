"""Read-only probing helper: search arbitrary wildcard byte patterns in MaxPayne3.exe and hex-dump context.

Usage: python -I probe.py <exe> "<pattern>" [context_before] [context_after] [max_hits]
"""
import re
import struct
import sys


def compile_pattern(p):
    out = b""
    for tok in p.split():
        out += b"." if tok == "?" else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.S)


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    imgbase = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    so = pe + 24 + optsz
    secs = []
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", data, so + i * 40)
        secs.append((name.rstrip(b"\0").decode("latin1"), vsize, va, rsize, roff))
    return data, imgbase, secs


def off2va(imgbase, secs, off):
    for name, vsize, va, rsize, roff in secs:
        if roff <= off < roff + rsize:
            return imgbase + off - roff + va
    return 0


def va2off(imgbase, secs, va):
    rva = va - imgbase
    for name, vsize, sva, rsize, roff in secs:
        if sva <= rva < sva + rsize:
            return rva - sva + roff
    return None


def main():
    path, pat = sys.argv[1], sys.argv[2]
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 32
    maxhits = int(sys.argv[5]) if len(sys.argv) > 5 else 20
    data, imgbase, secs = load(path)
    text = next(s for s in secs if s[0] == ".text")
    tstart, tend = text[4], text[4] + text[3]
    hits = [m.start() + tstart for m in compile_pattern(pat).finditer(data[tstart:tend])]
    print(f"pattern [{pat}] hits={len(hits)}")
    for h in hits[:maxhits]:
        chunk = data[h - before:h + after]
        print(f"  VA 0x{off2va(imgbase, secs, h):08X}: {chunk.hex(' ').upper()}")


if __name__ == "__main__":
    main()
