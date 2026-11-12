#!/usr/bin/env python3
"""Unpack the WiiWare opening.bnr (IMET) of 3D Pixel Racing.

Usage:  python -I tools/banner/wii_banner.py [opening.bnr|title.wad] [--out build/banner/wii]

Input: Dolphin's decrypted content 0 (0000000c.app) or the WAD itself (its footer carries an
unencrypted copy). Output:
  <out>/titles.txt            IMET channel names (one per language)
  <out>/banner/  icon/        the U8 archives (arc/blyt/*.brlyt, arc/anim/*.brlan, arc/timg/*.tpl)
  <out>/sound.bns             banner sound (BNS, DSP-ADPCM), or sound.bin if not BNS
Later stages (TPL/BRLYT/BRLAN decoding, CGFX writing) build on this output (#33/#34).
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from wii_unpack import lz77, u8_extract, u8_list  # noqa: E402

U8 = b"\x55\xAA\x38\x2D"


def unwrap(data):
    """Strip IMD5 header and LZ77 wrapper(s)."""
    if data[:4] == b"IMD5":
        data = data[0x20:]
    if data[:4] == b"LZ77":
        data = lz77(data[4:])
    elif data[0] in (0x10, 0x11) and data[:4] != U8 and data[:4] != b"BNS ":
        data = lz77(data)
    return data


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("src", nargs="?")
    ap.add_argument("--out", default=os.path.join(root, "build", "banner", "wii"))
    a = ap.parse_args()
    src = a.src or os.path.join(os.environ.get("APPDATA", ""), "Dolphin Emulator", "Wii", "title",
                                "00010001", "57334645", "content", "0000000c.app")
    if not os.path.exists(src):
        src = glob.glob(os.path.join(root, "Wii Files & Source", "*.wad"))[0]
    d = open(src, "rb").read()
    imet = d.find(b"IMET")
    if imet < 0:
        raise SystemExit("no IMET header found")
    base = imet - 0x40
    names = [d[base + 0x5C + i * 0x54:base + 0x5C + (i + 1) * 0x54].decode("utf-16-be").rstrip("\0")
             for i in range(10)]
    u8 = d.find(U8, imet)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "titles.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(names) + "\n")
    meta = d[u8:]
    for path, off, size in u8_list(meta):
        leaf = path.split("/")[-1]
        body = unwrap(meta[off:off + size])
        stem = os.path.splitext(leaf)[0]
        if body[:4] == U8:
            u8_extract(body, os.path.join(a.out, stem))
            print(f"{path}: U8, {len(u8_list(body))} files -> {stem}/")
        else:
            ext = ".bns" if body[:4] == b"BNS " else ".bin"
            open(os.path.join(a.out, stem + ext), "wb").write(body)
            print(f"{path}: {body[:4]!r} {len(body)} B -> {stem}{ext}")


if __name__ == "__main__":
    main()
