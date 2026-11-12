#!/usr/bin/env python3
"""Build the 3DS RomFS from the extracted iOS resources.

Usage:  python -I tools/build_romfs.py [--raw build/ios_raw] [--out build/romfs] [--tex3ds PATH]

Run tools/extract_ios.py first. Output layout (flat, like the iOS bundle; the port's fs layer
drops the Wii-style directories the game code asks for, as the iOS build did):
  *.t3x          textures (tex3ds; rgb8 if fully opaque, else rgba8), padded to power-of-two
  textures.txt   name  width height pot_width pot_height format   (one line per texture)
  *.bin          models ("SMD Reference bin File", see docs/formats/models.md)
  *.smd          only road_00.smd (its material is an .ifl texture list; kept for reference)
  *.map *.pbm *_objs.txt   tracks (docs/formats/tracks.md)
  *.wav          PCM16 mono, played by ndsp as-is
  lang_*.txt sys_msg_*.txt  text
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngio  # noqa: E402

# Store-only images the game never loads.
SKIP = {"iTunesArtwork.png"}


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default=os.path.join(root, "build", "ios_raw"))
    ap.add_argument("--out", default=os.path.join(root, "build", "romfs"))
    ap.add_argument("--tex3ds", default=os.path.join(os.environ.get("DEVKITPRO", "C:/devkitPro"), "tools", "bin", "tex3ds"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    manifest = []
    counts = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name in sorted(os.listdir(a.raw)):
            src = os.path.join(a.raw, name)
            base, ext = os.path.splitext(name)
            if name in SKIP:
                continue
            if ext == ".png":
                w, h, rgba, _ = pngio.read_png(open(src, "rb").read())
                opaque = all(v == 255 for v in rgba[3::4])
                pw, ph, padded = pngio.pad_pot(w, h, rgba)
                fmt = "rgb8" if opaque else "rgba8"
                tmp_png = os.path.join(tmp, name)
                pngio.write_png(tmp_png, pw, ph, padded)
                r = subprocess.run([a.tex3ds, "-f", fmt, "-z", "none", "-o",
                                    os.path.join(a.out, base + ".t3x"), tmp_png],
                                   capture_output=True, text=True)
                if r.returncode:
                    raise SystemExit(f"tex3ds failed on {name}: {r.stderr or r.stdout}")
                manifest.append(f"{base} {w} {h} {pw} {ph} {fmt}")
                counts["t3x"] = counts.get("t3x", 0) + 1
            elif ext == ".smd" and base != "road_00":
                continue  # the .bin carries the same mesh
            else:
                shutil.copyfile(src, os.path.join(a.out, name))
                counts[ext] = counts.get(ext, 0) + 1
    with open(os.path.join(a.out, "textures.txt"), "w", newline="\n") as f:
        f.write("\n".join(manifest) + "\n")
    total = sum(os.path.getsize(os.path.join(a.out, n)) for n in os.listdir(a.out))
    print(", ".join(f"{v} {k}" for k, v in sorted(counts.items())), f"-> {a.out} ({total / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
