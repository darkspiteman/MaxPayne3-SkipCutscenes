"""Minimal read-only PE inspector (no third-party deps). Usage: python -I peinfo.py <file> [--strings N]"""
import struct, sys, re


def rva2off(sections, rva):
    for name, vsize, va, rsize, roff, ch in sections:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + roff
    return None


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("latin1")


def main(path, min_str=0):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    machine, nsec, ts, _, _, optsz, chars = struct.unpack_from("<HHIIIHH", data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    is64 = magic == 0x20B
    ep = struct.unpack_from("<I", data, opt + 16)[0]
    imgbase = struct.unpack_from("<Q" if is64 else "<I", data, opt + (24 if is64 else 28))[0]
    subsys, dllchars = struct.unpack_from("<HH", data, opt + 68)
    sizeimg = struct.unpack_from("<I", data, opt + 56)[0]
    ddoff = opt + (112 if is64 else 96)
    dirs = [struct.unpack_from("<II", data, ddoff + i * 8) for i in range(16)]
    print(f"file={path}\nsize={len(data)} machine=0x{machine:04X} ({'x64' if machine == 0x8664 else 'x86' if machine == 0x14C else '?'}) "
          f"magic=0x{magic:X} timestamp={ts} chars=0x{chars:X} dllchars=0x{dllchars:X} subsys={subsys}")
    print(f"imagebase=0x{imgbase:X} entry=0x{ep:X} sizeofimage=0x{sizeimg:X}")
    print(f"ASLR={'yes' if dllchars & 0x40 else 'no'} DEP={'yes' if dllchars & 0x100 else 'no'} relocs_stripped={'yes' if chars & 1 else 'no'}")
    sections = []
    so = opt + optsz
    for i in range(nsec):
        name, vsize, va, rsize, roff = struct.unpack_from("<8sIIII", data, so + i * 40)
        ch = struct.unpack_from("<I", data, so + i * 40 + 36)[0]
        sections.append((name.rstrip(b"\0").decode("latin1"), vsize, va, rsize, roff, ch))
        print(f"  sec {sections[-1][0]:8s} va=0x{va:08X} vsize=0x{vsize:08X} raw=0x{roff:08X} rsize=0x{rsize:08X} flags=0x{ch:08X}")
    # imports
    irva, isz = dirs[1]
    if irva:
        off = rva2off(sections, irva)
        print("IMPORTS:")
        while off is not None:
            oft, _, _, nrva, ft = struct.unpack_from("<IIIII", data, off)
            if not nrva:
                break
            dll = cstr(data, rva2off(sections, nrva))
            names = []
            t = rva2off(sections, oft or ft)
            step = 8 if is64 else 4
            while t is not None:
                v = struct.unpack_from("<Q" if is64 else "<I", data, t)[0]
                if not v:
                    break
                if v >> (63 if is64 else 31):
                    names.append(f"#{v & 0xFFFF}")
                else:
                    no = rva2off(sections, v)
                    names.append(cstr(data, no + 2) if no is not None else f"?{v:X}")
                t += step
            print(f"  {dll}: {', '.join(names)}")
            off += 20
    # delay imports
    drva, dsz = dirs[13]
    if drva:
        print("DELAY IMPORTS present")
    # exports
    erva, esz = dirs[0]
    if erva:
        off = rva2off(sections, erva)
        nfun, nnames = struct.unpack_from("<II", data, off + 20)
        afun, anames, aords = struct.unpack_from("<III", data, off + 28)
        dn = cstr(data, rva2off(sections, struct.unpack_from("<I", data, off + 12)[0]))
        print(f"EXPORTS (dllname={dn}, {nfun} funcs, {nnames} names):")
        for i in range(min(nnames, 400)):
            nr = struct.unpack_from("<I", data, rva2off(sections, anames) + i * 4)[0]
            o = struct.unpack_from("<H", data, rva2off(sections, aords) + i * 2)[0]
            fr = struct.unpack_from("<I", data, rva2off(sections, afun) + o * 4)[0]
            print(f"  {cstr(data, rva2off(sections, nr))} @0x{fr:X}")
    if min_str:
        print("STRINGS:")
        for m in re.finditer(rb"[\x20-\x7E]{%d,}" % min_str, data):
            print(f"  0x{m.start():06X}: {m.group().decode('latin1')}")
        for m in re.finditer(rb"(?:[\x20-\x7E]\0){%d,}" % min_str, data):
            print(f"  0x{m.start():06X} (utf16): {m.group().decode('utf-16le')}")


if __name__ == "__main__":
    n = 0
    if "--strings" in sys.argv:
        n = int(sys.argv[sys.argv.index("--strings") + 1])
    main(sys.argv[1], n)
