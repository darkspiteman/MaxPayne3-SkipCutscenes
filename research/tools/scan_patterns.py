"""Read-only static check: are the byte signatures the mod relies on present in MaxPayne3.exe?

Usage: python -I scan_patterns.py "<path to MaxPayne3.exe>"
The executable is only opened for reading. Nothing is written next to it.
"""
import re
import struct
import sys

PATTERNS = {
    # name: (pattern, expected match count)
    "game_process_call": ("E8 ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 8B 15 ? ? ? ? 89 0D", 1),
    "get_native_address": ("53 8B 59 04 85 DB", 1),
    "native_handler_ptr": ("B9 ? ? ? ? E8 ? ? ? ? 85 C0 75 07", None),
    "cutscene_manager_ptr": ("83 3D ? ? ? ? 00 75 28 68 00 3A 00 00", 1),
    "movie_player": ("8B 44 24 04 50 6A 00 B9 ? ? ? ? E8 ? ? ? ? C3", 1),
    "skip_flag_reset": ("C6 86 F8 39 00 00 01", 2),
    "skip_check": (
        "56 8B F1 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4 04 8B C8 E8 ? ? ? ? "
        "8A 0D ? ? ? ? 0A 0D ? ? ? ? 74 0B 80 3D ? ? ? ? 00 74 02 32 C0 80 BE F8 39 00 00 00",
        1,
    ),
    "unpack_marker": ("C6 44 24 ? ? F3 0F 11 44 24 ? F3 0F 11 44 24 ? 89 4C 24 54", None),
}


def compile_pattern(p):
    out = b""
    for tok in p.split():
        out += b"." if tok == "?" else re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.S)


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    imgbase = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    so = pe + 24 + optsz
    secs = []
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", data, so + i * 40)
        secs.append((name.rstrip(b"\0").decode("latin1"), vsize, va, rsize, roff))
    return imgbase, secs


def off2rva(secs, off):
    for name, vsize, va, rsize, roff in secs:
        if roff <= off < roff + rsize:
            return off - roff + va, name
    return None, None


def main(path):
    data = open(path, "rb").read()
    imgbase, secs = sections(data)
    text = next(s for s in secs if s[0] == ".text")
    tstart, tend = text[4], text[4] + text[3]
    code = data[tstart:tend]
    ok = True
    results = {}
    for name, (pat, expected) in PATTERNS.items():
        hits = [m.start() + tstart for m in compile_pattern(pat).finditer(code)]
        results[name] = hits
        status = "OK" if (expected is None and hits) or (expected is not None and len(hits) == expected) else "MISMATCH"
        if status != "OK":
            ok = False
        locs = ", ".join("0x%08X" % (imgbase + off2rva(secs, h)[0]) for h in hits[:6])
        print(f"{status:8s} {name:22s} hits={len(hits)} expected={expected} at [{locs}]")
    print("ALL SIGNATURES FOUND" if ok else "SOME SIGNATURES MISSING/AMBIGUOUS")
    return results, data, imgbase, secs


if __name__ == "__main__":
    main(sys.argv[1])
