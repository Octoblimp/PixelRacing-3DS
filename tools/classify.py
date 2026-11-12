#!/usr/bin/env python3
"""Turn decomp/raw/functions.tsv (Ghidra export) into the tracked decomp/symbols.csv.

Usage:  python -I tools/classify.py [--tsv decomp/raw/functions.tsv] [--out decomp/symbols.csv]

Per function: addr, size, name, class, src_file, status, notes.
  class:  platform-gl / platform-al / platform-aq / platform-ui / platform-objc  (rewritten, not ported)
          shim  (wii.m: the Wii-API layer the game calls; reimplemented on 3DS)
          game  (decompiled and ported)
  name/src_file come from __assert_rtn strings (function name + source file) and from the asset
  paths a function uses. Only file basenames are kept. Existing rows keep their name/status/notes
  when re-run, so hand edits survive.
status: raw -> named -> ported -> verified.
"""
import argparse
import csv
import os
import re

SRC_RE = re.compile(r"/([A-Za-z0-9_]+\.(?:m|mm|c|cpp|h))\b")
# Hints from asset strings when there is no assert name.
HINTS = [
    (r"RMS_LoadSavedData|Savefile", "save_load", "save"),
    (r"%s_access\.smd|/models/car_%02d", "car_load_models", "models"),
    (r"_grnd\.pbm|_road\.pbm|_tree\.pbm", "track_load", "track"),
    (r"/models/faces/face_|/models/flags/flag_", "load_faces_flags_logos", "models"),
    (r"/models/faces/small|head_lights", "load_race_extras", "models"),
    (r"font_3_b\.png", "font_load", "ui"),
    (r"Game paused", "race_draw_paused", "race"),
    (r"Drive As", "menu_draw", "menu"),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--tsv", default=os.path.join(root, "decomp", "raw", "functions.tsv"))
    ap.add_argument("--out", default=os.path.join(root, "decomp", "symbols.csv"))
    a = ap.parse_args()

    old = {}
    if os.path.exists(a.out):
        with open(a.out, newline="") as f:
            old = {r["addr"]: r for r in csv.DictReader(f)}

    rows = []
    with open(a.tsv, encoding="utf-8") as f:
        next(f)
        for line in f:
            addr, size, name, cls, callees, strs = (line.rstrip("\n").split("\t") + [""] * 6)[:6]
            callees = callees.split(",") if callees else []
            strings = strs.split(" | ") if strs else []
            src = ""
            for s in strings:
                m = SRC_RE.search(s)
                if m:
                    src = m.group(1)
            # assert function name: an identifier-only string next to the source path
            if name.startswith("FUN_") and src:
                idents = [s for s in strings if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{2,}", s)]
                if idents:
                    name = idents[0]
            notes = ""
            if name.startswith("FUN_"):
                for pat, hint, area in HINTS:
                    if any(re.search(pat, s) for s in strings):
                        notes = f"guess:{hint}"
                        src = src or f"({area})"
                        break
            objc = "::" in name or name.startswith(("-[", "+["))
            if cls == "objc" and not objc:
                cls = "game"  # Global-namespace bug in the first export
            if objc:
                kind = "platform-objc"
            elif src == "wii.m":
                kind = "shim"
            elif cls in ("gl", "al", "aq", "ui", "objc") and not src.endswith(".c"):
                kind = "platform-" + cls if cls != "gl" or not strings else "game"
            else:
                kind = "game"
            if cls == "gl" and kind == "game":
                notes = (notes + " " if notes else "") + "calls-gl"
            o = old.get(addr, {})
            rows.append({
                "addr": addr, "size": size,
                "name": o.get("name") if o.get("name") and not o["name"].startswith("FUN_") else name,
                "class": o.get("class") or kind, "src_file": o.get("src_file") or src,
                "status": o.get("status") or ("named" if not name.startswith("FUN_") else "raw"),
                "notes": o.get("notes") or notes,
            })
    with open(a.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["addr", "size", "name", "class", "src_file", "status", "notes"])
        w.writeheader()
        w.writerows(rows)
    by = {}
    for r in rows:
        k = r["class"]
        by.setdefault(k, [0, 0])
        by[k][0] += 1
        by[k][1] += int(r["size"])
    named = sum(1 for r in rows if not r["name"].startswith("FUN_"))
    print(f"{len(rows)} functions, {named} named -> {a.out}")
    for k, (n, s) in sorted(by.items()):
        print(f"  {k:<16} {n:4d} functions {s:7d} bytes")


if __name__ == "__main__":
    main()
