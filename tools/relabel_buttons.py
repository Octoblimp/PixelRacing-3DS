#!/usr/bin/env python3
"""Show the 3DS's A and B buttons where the game shows the Wii Remote's "2" and "1".

The menus are the Wii version's, held sideways: "2" confirms and "1" goes back. On the 3DS
those are A and B (game_main.c feed_buttons), so `buttons.png` (11 icons of 32x32) gets its
"1" cell (7) redrawn as a round B button and its "2" cell (8) as the round A button from
cell 1. The B letter is lifted from cell 2 (the Wii Remote's B trigger) onto a round button.

  relabel(w, h, rgba) -> rgba          library use (tools/build_romfs.py)
  python -I tools/relabel_buttons.py IN.png OUT.png    write a relabelled copy, to look at
"""
import os
import sys

CELL = 32
A_CELL, B_CELL, ONE_CELL, TWO_CELL = 1, 2, 7, 8
# glyph box inside a cell (x0, x1, y0, y1), from the icon art; the button face spans x 6..25
GLYPH = (11, 22, 9, 24)


def relabel(w, h, rgba):
    if (w, h) != (11 * CELL, CELL):
        raise ValueError("unexpected buttons.png size %dx%d" % (w, h))
    p = bytearray(rgba)

    def get(cell, x, y):
        i = (y * w + cell * CELL + x) * 4
        return list(p[i:i + 4])

    def put(cell, x, y, v):
        i = (y * w + cell * CELL + x) * 4
        p[i:i + 4] = bytes(max(0, min(255, int(round(c)))) for c in v)

    def lum(v):
        return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2]

    x0, x1, y0, y1 = GLYPH
    # B letter coverage from cell 2: how much darker than the button face of its row
    cover = {}
    for y in range(y0, y1):
        face = [get(B_CELL, x, y) for x in (x0 - 1, x1)]
        ref = sum(lum(f) for f in face) / 2
        for x in range(x0, x1):
            v = get(B_CELL, x, y)
            if v[3] > 128 and ref > 1:
                cover[(x, y)] = max(0.0, min(1.0, (ref - lum(v)) / (ref * 0.85)))

    # "2" -> A: the round A button as it is
    for y in range(CELL):
        for x in range(CELL):
            put(TWO_CELL, x, y, get(A_CELL, x, y))

    # "1" -> B: clear the "1" by blending each row's face across the glyph box, then draw B
    for y in range(y0, y1):
        left, right = get(ONE_CELL, x0 - 1, y), get(ONE_CELL, x1, y)
        for x in range(x0, x1):
            t = (x - (x0 - 1)) / (x1 - (x0 - 1))
            face = [left[k] * (1 - t) + right[k] * t for k in range(4)]
            c = cover.get((x, y), 0.0)
            out = [face[k] * (1 - c) + 8 * c for k in range(3)] + [face[3]]
            put(ONE_CELL, x, y, out)
    return bytes(p)


def main(argv):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import pngio
    w, h, rgba, _ = pngio.read_png(open(argv[0], "rb").read())
    pngio.write_png(argv[1], w, h, relabel(w, h, rgba))


if __name__ == "__main__":
    main(sys.argv[1:])
