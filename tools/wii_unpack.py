#!/usr/bin/env python3
"""Unpack the WiiWare 3D Pixel Racing contents (as decrypted by Dolphin's NAND import).

Usage:  python -I tools/wii_unpack.py [content_dir] [--out build/wii]

content_dir defaults to %APPDATA%/Dolphin Emulator/Wii/title/00010001/57334645/content.
Each <id>.app is copied to <out>/<id>.app; LZ77 (0x10/0x11) contents are decompressed to
<id>.dec, and U8 archives (raw or after decompression) are extracted to <id>_u8/.
Library module: lz77(), u8_list(), u8_extract() are reused by tools/banner/wii_banner.py.
"""
import argparse
import os
import struct
import sys


def lz77(data):
    """Nintendo LZ77 type 0x10 or 0x11 (LZ11). data[0] is the type byte."""
    typ = data[0]
    size = data[1] | data[2] << 8 | data[3] << 16
    p = 4
    if size == 0:
        size = struct.unpack("<I", data[4:8])[0]
        p = 8
    out = bytearray()
    while len(out) < size:
        flags = data[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[p])
                p += 1
                continue
            if typ == 0x10:
                b0, b1 = data[p], data[p + 1]
                p += 2
                n = (b0 >> 4) + 3
                disp = ((b0 & 0xF) << 8 | b1) + 1
            else:
                b0 = data[p]
                ind = b0 >> 4
                if ind == 0:
                    n = ((b0 & 0xF) << 4 | data[p + 1] >> 4) + 0x11
                    disp = ((data[p + 1] & 0xF) << 8 | data[p + 2]) + 1
                    p += 3
                elif ind == 1:
                    n = ((b0 & 0xF) << 12 | data[p + 1] << 4 | data[p + 2] >> 4) + 0x111
                    disp = ((data[p + 2] & 0xF) << 8 | data[p + 3]) + 1
                    p += 4
                else:
                    n = ind + 1
                    disp = ((b0 & 0xF) << 8 | data[p + 1]) + 1
                    p += 2
            for _ in range(n):
                out.append(out[-disp])
    return bytes(out)


def u8_list(data):
    """Return [(path, offset, size)] for files in a U8 archive."""
    if data[:4] != b"\x55\xAA\x38\x2D":
        raise ValueError("not U8")
    root, hsize, doff = struct.unpack(">III", data[4:16])
    n = struct.unpack(">I", data[root + 8:root + 12])[0]
    strtab = root + n * 12
    nodes = [struct.unpack(">BBHII", data[root + i * 12:root + i * 12 + 12]) for i in range(n)]

    def name(i):
        o = strtab + (nodes[i][1] << 16 | nodes[i][2])
        return data[o:data.index(b"\0", o)].decode("latin1")

    out, stack = [], [("", n)]  # (dir path, end index)
    for i in range(1, n):
        while stack and i >= stack[-1][1]:
            stack.pop()
        typ, _, _, a, b = nodes[i]
        path = stack[-1][0] + name(i)
        if typ == 1:
            stack.append((path + "/", b))
        else:
            out.append((path, a, b))
    return out


def u8_extract(data, dest):
    for path, off, size in u8_list(data):
        full = os.path.join(dest, *path.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data[off:off + size])


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("content", nargs="?", default=os.path.join(
        os.environ.get("APPDATA", ""), "Dolphin Emulator", "Wii", "title", "00010001", "57334645", "content"))
    ap.add_argument("--out", default=os.path.join(root, "build", "wii"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for fn in sorted(os.listdir(a.content)):
        if not fn.endswith(".app"):
            continue
        data = open(os.path.join(a.content, fn), "rb").read()
        base = fn[:-4]
        open(os.path.join(a.out, fn), "wb").write(data)
        note = f"{len(data)} B"
        typ = data[0]
        if typ in (0x10, 0x11) and data[:4] != b"\x55\xAA\x38\x2D":
            try:
                data = lz77(data)
                open(os.path.join(a.out, base + ".dec"), "wb").write(data)
                note += f", LZ77 0x{typ:02x} -> {len(data)} B"
            except (IndexError, ValueError):
                pass
        if data[:4] == b"\x55\xAA\x38\x2D":
            u8_extract(data, os.path.join(a.out, base + "_u8"))
            note += f", U8 ({len(u8_list(data))} files)"
        print(fn, note, data[:8].hex())


if __name__ == "__main__":
    sys.exit(main())
