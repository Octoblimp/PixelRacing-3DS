#!/usr/bin/env python3
"""Extract the iOS 3D Pixel Racing 1.1 IPA's game resources.

Usage:  python -I tools/extract_ios.py [path/to.ipa] [--out build]

Step "raw": copies every resource in Payload/3d_pixel_racing.app/ to <out>/ios_raw/,
leaving out the executable, nibs, launch images, code signature and the store metadata
(iTunesMetadata.plist holds account details and is never extracted). The executable's
armv7 slice is written separately to <out>/bin/3d_pixel_racing.armv7 for Ghidra.

Later steps (romfs conversion) live in tools/build_romfs.py once the formats are known.
"""
import argparse
import glob
import os
import struct
import sys
import zipfile

APP = "Payload/3d_pixel_racing.app/"
EXE = APP + "3d_pixel_racing"
# Bundle files that are iOS plumbing, not game data.
SKIP_SUFFIX = (".nib", ".plist", "PkgInfo", "CodeResources", "ResourceRules.plist")
SKIP_PREFIX = ("Default", "Icon", "iTunesArtwork", "_CodeSignature/", "en.lproj/", "SC_Info/")

CPU_ARM = 12
SUBTYPE_ARMV7 = 9


def thin_armv7(fat: bytes) -> bytes:
    magic, n = struct.unpack(">II", fat[:8])
    if magic != 0xCAFEBABE:
        raise SystemExit("executable is not a fat Mach-O")
    for i in range(n):
        cpu, sub, off, size, _ = struct.unpack(">5I", fat[8 + i * 20:28 + i * 20])
        if cpu == CPU_ARM and sub == SUBTYPE_ARMV7:
            return fat[off:off + size]
    raise SystemExit("no armv7 slice")


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("ipa", nargs="?")
    ap.add_argument("--out", default=os.path.join(here, "build"))
    a = ap.parse_args()
    ipa = a.ipa or (glob.glob(os.path.join(here, "iOS Files & Source", "*.ipa")) or [None])[0]
    if not ipa:
        raise SystemExit("IPA not found; pass its path")

    raw = os.path.join(a.out, "ios_raw")
    os.makedirs(raw, exist_ok=True)
    os.makedirs(os.path.join(a.out, "bin"), exist_ok=True)
    n = 0
    with zipfile.ZipFile(ipa) as z:
        for info in z.infolist():
            name = info.filename
            if not name.startswith(APP) or name.endswith("/"):
                continue
            rel = name[len(APP):]
            if name == EXE:
                with open(os.path.join(a.out, "bin", "3d_pixel_racing.armv7"), "wb") as f:
                    f.write(thin_armv7(z.read(name)))
                continue
            if rel.endswith(SKIP_SUFFIX) or rel.startswith(SKIP_PREFIX):
                continue
            with open(os.path.join(raw, rel), "wb") as f:
                f.write(z.read(name))
            n += 1
    print(f"{n} resources -> {raw}")
    print(f"armv7 slice -> {os.path.join(a.out, 'bin', '3d_pixel_racing.armv7')}")


if __name__ == "__main__":
    sys.exit(main())
