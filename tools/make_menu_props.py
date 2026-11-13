#!/usr/bin/env python3
"""Make the port's own menu props: voxel models in the game's style for the Options screen's
Controls and 3D pages (3DS Files/source/game/opt_menu.c).

The game's props are voxel models (5-unit cubes, x across, z up, y depth, centred) with a
"palette strip" texture: one 16 x 16 tile per colour, a darker rim around a flat centre, and
each cube face maps onto one tile (docs/formats/models.md). This draws pixel art the same way:
every character of a grid is a column of cubes of one colour, `depth` cubes deep (centred on
y = 0, so both sides look the same, like the game's flat props).

  python -I tools/make_menu_props.py [--out build/menu_props]

Writes <name>.bin (the game's binary SMD) and <name>.png per prop; tools/build_romfs.py runs
this and converts them with the iOS assets.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngio  # noqa: E402

VOX = 5.0

# colour name -> RGB
COLORS = {
    "w": (236, 236, 236),  # white
    "g": (150, 150, 158),  # grey (pad body)
    "d": (70, 70, 78),     # dark grey
    "k": (34, 34, 38),     # near black
    "r": (220, 48, 40),    # red
    "c": (40, 200, 230),   # cyan
    "b": (50, 90, 220),    # blue
    "e": (60, 190, 70),    # green
    "y": (240, 200, 40),   # yellow
}

# 3x5 letters for the button caps
FONT = {
    "A": [".w.", "w.w", "www", "w.w", "w.w"],
    "B": ["ww.", "w.w", "ww.", "w.w", "ww."],
    "X": ["w.w", "w.w", ".w.", "w.w", "w.w"],
    "Y": ["w.w", "w.w", ".w.", ".w.", ".w."],
    "L": ["w..", "w..", "w..", "w..", "www"],
    "R": ["ww.", "w.w", "ww.", "w.w", "w.w"],
    "Z": ["www", "..w", ".w.", "w..", "www"],
}

# "3D" in chunky 8-bit letters: a red 3 and a cyan D, like the two eyes of a 3D picture.
THREE_D = [
    "rrrrr..ccccc.",
    "rrrrrr.cccccc",
    "....rr.cc..cc",
    "..rrr..cc..cc",
    "....rr.cc..cc",
    "rrrrrr.cccccc",
    "rrrrr..ccccc.",
]

# A generic game pad: grey body, dark cross on the left, four buttons on the right. Upper
# case = the same colour standing one cube further out on both sides.
PAD = [
    "..dddd......dddd..",
    ".gggggggggggggggg.",
    "gggggggggggggggYgg",
    "ggKggggggggggggggg",
    "gKKKgggWWgWWggBgRg",
    "ggKggggggggggggggg",
    "ggggggggggggggEggg",
    "gggggggggggggggggg",
    "gggggg......gggggg",
    ".gggg........gggg.",
]

# A small block for the settings meters (tinted by the game; white texture).
BAR = ["ww", "ww", "ww"]


def cap(letters):
    """A round dark button cap with white letters standing out of it."""
    rows = ["..ddddd..", ".ddddddd.", "ddddddddd", "ddddddddd", "ddddddddd", "ddddddddd",
            "ddddddddd", ".ddddddd.", "..ddddd.."]
    grid = [list(r) for r in rows]
    w = len(letters) * 4 - 1
    x0, y0 = (9 - w) // 2, 2
    for n, ch in enumerate(letters):
        for y, line in enumerate(FONT[ch]):
            for x, p in enumerate(line):
                if p != ".":
                    grid[y0 + y][x0 + n * 4 + x] = "W"
    return ["".join(r) for r in grid]


def palette_tile(rgb):
    """16 x 16 tile like the game's: 2 rows of dark rim at the top and bottom, 1 column at
    the left and right, a flat centre."""
    out = []
    for y in range(16):
        for x in range(16):
            if y < 2:
                k = 0.62
            elif y >= 14:
                k = 0.58
            elif x == 0:
                k = 0.64
            elif x == 15:
                k = 0.6
            else:
                k = 1.0
            out.append(tuple(min(255, int(c * k)) for c in rgb) + (255,))
    return out


def build(grid, depth, raised=1):
    """Cubes from the grid. Lower-case cells are `depth` cubes deep, upper-case ones
    depth + 2 * raised. Returns (triangles, colour keys in tile order)."""
    h, w = len(grid), max(len(r) for r in grid)
    cells = {}
    for y, row in enumerate(grid):
        for x, ch in enumerate(row):
            if ch == ".":
                continue
            col = ch.lower()
            d = depth + (2 * raised if ch.isupper() else 0)
            for k in range(d):
                cells[(x, k - (d - 1) / 2.0, h - 1 - y)] = col
    keys = sorted(set(cells.values()), key=lambda c: "wgdkrcbey".index(c))
    tile = {c: i for i, c in enumerate(keys)}
    ntiles = len(keys)
    # The texture is padded to a power of two: v spans that whole height.
    pot = 16
    while pot < 16 * ntiles:
        pot *= 2
    cx, cz = (w - 1) / 2.0, (h - 1) / 2.0
    tris = []
    faces = [((1, 0, 0), 0), ((-1, 0, 0), 0), ((0, 1, 0), 1), ((0, -1, 0), 1),
             ((0, 0, 1), 2), ((0, 0, -1), 2)]
    for (x, y, z), col in cells.items():
        for n, ax in faces:
            nb = (x + n[0], y + n[1], z + n[2])
            if nb in cells:
                continue
            # the face's centre and two edge vectors, all in cube units
            c = (x - cx + n[0] * 0.5, y + n[1] * 0.5, z - cz + n[2] * 0.5)
            u = [0, 0, 0]
            v = [0, 0, 0]
            u[(ax + 1) % 3] = 0.5
            v[(ax + 2) % 3] = 0.5
            # keep the winding counter-clockwise seen from outside
            cr = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < 0:
                u, v = v, u
            t = tile[col]
            # v = 0 is the bottom row of the image (the game's textures are upside down in GL)
            v0, v1 = 1.0 - (16 * t + 15) / pot, 1.0 - (16 * t + 1) / pot
            corner = []
            for su, sv, tu, tv in ((-1, -1, 0.0, v0), (1, -1, 1.0, v0), (1, 1, 1.0, v1),
                                   (-1, 1, 0.0, v1)):
                p = tuple((c[i] + su * u[i] + sv * v[i]) * VOX for i in range(3))
                corner.append((p, n, (tu, tv)))
            tris.append((corner[0], corner[1], corner[2]))
            tris.append((corner[0], corner[2], corner[3]))
    return tris, keys, pot


def write_bin(path, tris):
    out = bytearray(b"SMD Reference bin File  ")
    out += struct.pack("<ii", 1, len(tris) * 3)
    out += b"SMDImport".ljust(28, b"\0")
    out += struct.pack("<i6f", -1, 0, 0, 0, 0, 0, 0)
    out += b"a".ljust(8, b" ")
    for tri in tris:
        for p, n, uv in tri:
            out += struct.pack("<i8f", 0, *p, *n, *uv)
    assert len(out) == 96 + 36 * len(tris) * 3
    open(path, "wb").write(out)


def write_png(path, keys, pot):
    rgba = []
    for c in keys:
        rgba += palette_tile(COLORS[c])
    rgba += [(0, 0, 0, 255)] * (16 * pot - len(rgba))
    flat = bytearray()
    for px in rgba:
        flat += bytes(px)
    pngio.write_png(path, 16, pot, bytes(flat))


PROPS = {
    "opt_3d": (THREE_D, 2),
    "opt_pad": (PAD, 2),
    "opt_bar": (BAR, 2),
}
for _b in ("A", "B", "X", "Y", "L", "R", "ZL", "ZR"):
    PROPS["opt_btn_" + _b.lower()] = (cap(_b), 1)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(root, "build", "menu_props"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name, (grid, depth) in PROPS.items():
        tris, keys, pot = build(grid, depth)
        write_bin(os.path.join(a.out, name + ".bin"), tris)
        write_png(os.path.join(a.out, name + ".png"), keys, pot)
    print("menu props: %d -> %s" % (len(PROPS), a.out))


if __name__ == "__main__":
    main()
