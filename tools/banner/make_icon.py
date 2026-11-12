#!/usr/bin/env python3
"""Compose the 3DS HOME Menu icon (3DS Files/meta/icon.png, 48x48) from the Wii channel icon.

Renders the first frame of icon.brlyt: each picture pane is its texture stretched over the pane
rectangle, rotated by its z rotation, alpha-blended in layout order. The Wii icon is 128x96-ish
(Picture_00 is the 170x96 background); the 3DS icon is square, so the centre is cropped.

Usage: python -I tools/banner/make_icon.py [--wii build/banner/wii] [--out "3DS Files/meta/icon.png"]
"""
import argparse
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import brlyt  # noqa: E402

SCALE = 4  # supersample


def uv_crop(tex, uv):
    """Crop/flip a texture to a pane's (axis-aligned) UV rectangle; tiles past [0,1]."""
    if not uv:
        return tex
    s0, t0, s1, _, _, t1, _, _ = uv  # TL, TR, BL, BR
    img = tex
    if s1 < s0:
        img, s0, s1 = img.transpose(Image.FLIP_LEFT_RIGHT), 1 - s0, 1 - s1
    if t1 < t0:
        img, t0, t1 = img.transpose(Image.FLIP_TOP_BOTTOM), 1 - t0, 1 - t1
    tw, th = int(round((s1 - s0) * img.width)), int(round((t1 - t0) * img.height))
    if tw <= 0 or th <= 0:
        return img
    out = Image.new("RGBA", (tw, th))
    ox, oy = int(round(s0 * img.width)), int(round(t0 * img.height))
    for y in range(oy - (oy % img.height), oy + th, img.height):
        for x in range(ox - (ox % img.width), ox + tw, img.width):
            out.paste(img, (x - ox, y - oy))
    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("--wii", default=os.path.join(root, "build", "banner", "wii"))
    ap.add_argument("--out", default=os.path.join(root, "3DS Files", "meta", "icon.png"))
    a = ap.parse_args()
    arc = os.path.join(a.wii, "icon", "arc")
    lay = brlyt.parse_brlyt(open(os.path.join(arc, "blyt", "icon.brlyt"), "rb").read())
    texs = [Image.open(os.path.join(arc, "timg", os.path.splitext(t)[0] + ".png")).convert("RGBA")
            for t in lay["textures"]]
    W, H = 170, 112  # background pane (170x96) is stretched to fill this
    first = True
    canvas = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 255))
    for p in lay["panes"]:
        if p["kind"] != "pic1" or not p["visible"]:
            continue
        if p["size"][1] < 0:
            continue  # the logo's floor reflection: unreadable at 48x48
        mat = lay["materials"][p["material"]]
        if not mat["textures"]:
            continue
        tex = uv_crop(texs[mat["textures"][0]], p["uv"][0] if p["uv"] else None)
        w, h = p["size"]
        if first:
            w, h, first = W, H, False
        img = tex
        if w < 0:
            img = img.transpose(Image.FLIP_LEFT_RIGHT)
        if h < 0:
            img = img.transpose(Image.FLIP_TOP_BOTTOM)
        w, h = abs(w) * p["scale"][0] * SCALE, abs(h) * p["scale"][1] * SCALE
        img = img.resize((max(1, int(round(w))), max(1, int(round(h)))), Image.LANCZOS)
        if p["alpha"] != 255:
            r, g, b, al = img.split()
            al = al.point(lambda v, k=p["alpha"]: v * k // 255)
            img = Image.merge("RGBA", (r, g, b, al))
        if abs(p["rot"][2]) > 1.0:  # sub-degree jitter in the layout is not worth resampling
            img = img.rotate(p["rot"][2], resample=Image.BICUBIC, expand=True)
        cx = W * SCALE / 2 + p["trans"][0] * SCALE
        cy = H * SCALE / 2 - p["trans"][1] * SCALE
        layer = Image.new("RGBA", canvas.size)
        layer.paste(img, (int(round(cx - img.width / 2)), int(round(cy - img.height / 2))))
        canvas = Image.alpha_composite(canvas, layer)
    # square around the logo (Picture_05: 100x48 at y = +11.9)
    side = 108 * SCALE
    cx, cy = canvas.width // 2, int(H * SCALE / 2 - 6 * SCALE)
    icon = canvas.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)).resize((48, 48), Image.LANCZOS)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    icon.convert("RGB").save(a.out)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
