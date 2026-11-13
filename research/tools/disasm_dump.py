"""Disassemble the hex blocks ("dump ...") that MP3SkipCutscenes writes to its log with DumpMovieCode=1.

Usage: python disasm_dump.py <log> [out.txt]
Addresses are RVAs (image base taken from the log's "image 0x..." line, only used to turn absolute operands into RVAs).
"""
import re, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

def main():
    log = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
    out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else sys.stdout
    base = 0
    blocks = []  # (title, start, bytearray)
    cur = None
    for line in log:
        m = re.search(r"image 0x([0-9A-F]+)\.\.", line)
        if m: base = int(m.group(1), 16)
        m = re.search(r"\] dump ([a-z ]+): RVA 0x([0-9A-F]+)\.\.0x([0-9A-F]+)", line)
        if m:
            cur = (m.group(1), int(m.group(2), 16), bytearray())
            blocks.append(cur)
            continue
        m = re.search(r"\]\s+([0-9A-F]{6,8}):\s+((?:[0-9A-F]{2} ?)+)\s*$", line)
        if m and cur:
            cur[2].extend(bytes.fromhex(m.group(2)))
            continue
        cur = None
    # merge overlapping blocks into a memory map
    mem = {}
    for _, start, data in blocks:
        for i, b in enumerate(data): mem[start + i] = b
    addrs = sorted(mem)
    ranges = []
    s = p = addrs[0]
    for a in addrs[1:]:
        if a != p + 1: ranges.append((s, p + 1)); s = a
        p = a
    ranges.append((s, p + 1))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    print(f"image base 0x{base:X}; {len(blocks)} blocks, {len(ranges)} ranges", file=out)
    starts = {b[1] for b in blocks}
    for lo, hi in ranges:
        print(f"\n===== RVA 0x{lo:X}..0x{hi:X} =====", file=out)
        data = bytes(mem[a] for a in range(lo, hi))
        # linear sweep, restarting at int3 padding / block starts
        off = 0
        while off < len(data):
            n = 0
            for ins in md.disasm(data[off:], lo + off):
                if ins.address in starts and n: break
                ops = ins.op_str
                def fix(mm):
                    v = int(mm.group(0), 16)
                    if base and base <= v < base + 0x12200000: return f"{mm.group(0)}[rva {v - base:X}]"
                    return mm.group(0)
                if ins.mnemonic not in ("call", "jmp") and not ins.mnemonic.startswith("j"):
                    ops = re.sub(r"0x[0-9a-f]{6,8}", fix, ops)
                mark = "\n" if ins.address in starts else ""
                print(f"{mark}{ins.address:08X}  {ins.bytes.hex():<22} {ins.mnemonic} {ops}", file=out)
                n += 1
                off = ins.address + ins.size - lo
                if ins.address + ins.size in starts: break
            if n == 0: off += 1
main()
