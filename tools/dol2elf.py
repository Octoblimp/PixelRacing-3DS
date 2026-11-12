#!/usr/bin/env python3
"""Convert a Wii/GameCube DOL to a big-endian PPC32 ELF executable Ghidra can import natively.

Usage:  python -I tools/dol2elf.py in.dol out.elf
Import in Ghidra with language PowerPC:BE:32:Gekko_Broadway (or PowerPC:BE:32:default).
"""
import struct
import sys


def main(src, dst):
    d = open(src, "rb").read()
    offs = struct.unpack(">18I", d[0x00:0x48])
    addrs = struct.unpack(">18I", d[0x48:0x90])
    sizes = struct.unpack(">18I", d[0x90:0xD8])
    bss_addr, bss_size, entry = struct.unpack(">III", d[0xD8:0xE4])

    segs = []  # (vaddr, data, filesz, memsz, flags)
    for i in range(18):
        if sizes[i]:
            segs.append((addrs[i], d[offs[i]:offs[i] + sizes[i]], sizes[i], sizes[i], 5 if i < 7 else 6))
    # BSS overlaps some data sections in many DOLs; emit only the part past the last data section.
    end = max(a + s for a, _, s, _, _ in segs)
    if bss_addr + bss_size > end:
        start = max(bss_addr, end)
        segs.append((start, b"", 0, bss_addr + bss_size - start, 6))

    ehsz, phsz = 52, 32
    data_off = ehsz + phsz * len(segs)
    out = bytearray()
    out += b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    out += struct.pack(">HHIIIIIHHHHHH", 2, 20, 1, entry, ehsz, 0, 0x80000000, ehsz, phsz, len(segs), 40, 0, 0)
    body = bytearray()
    for vaddr, data, filesz, memsz, flags in segs:
        off = data_off + len(body) if filesz else 0
        out += struct.pack(">IIIIIIII", 1, off, vaddr, vaddr, filesz, memsz, flags, 0x20)
        body += data
    open(dst, "wb").write(out + body)
    print(f"{len(segs)} segments, entry {entry:#x} -> {dst}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
