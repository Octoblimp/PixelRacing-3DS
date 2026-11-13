#!/usr/bin/env python3
"""The help pages (START on the title / main menu) with the 3DS's buttons instead of the iPhone's.

The iOS pages are pictures (page_NN.png, 480x320). Two things in them describe an iPhone:
  - "In Game Controls" (page_06b.png, the one the game shows; page_06.png is the tilt-only
    variant the iOS build never picks): tilt the iPhone, the touch buttons 2 / 1 / pause, the
    touch areas for steering and the view. Its body is redrawn with the 3DS's controls
    (game_main.c feed_buttons, controls.c defaults), the header and footer bars kept.
  - Back / Next / Exit on every page: B / A / START press them (game_main.c), so each gets
    that button drawn in front of its label.

Writes build/help/page_*.png; tools/build_romfs.py uses them in place of the iOS ones.
Needs Pillow (like tools/banner/make_banner.py), so it runs without -I:
    python tools/make_help_pages.py [--raw build/ios_raw] [--out build/help]
"""
import argparse
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngio  # noqa: E402

FONTS = os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts")
SERIF = "times.ttf"      # the pages' body text is set in Times
SANS = "arial.ttf"       # their button labels in Helvetica / Arial
SANS_BOLD = "arialbd.ttf"

INK = (40, 40, 40)
ICON = (70, 70, 70)
LABEL = (90, 90, 90)

# The three buttons of the page art: (x0, y0, x1, y1), the 3DS button, the label.
BUTTONS = [
    ((344, 0, 452, 47), "START", "Exit"),
    ((23, 276, 131, 320), "B", "Back"),
    ((344, 276, 452, 320), "A", "Next"),
]
BODY = (0, 48, 480, 275)   # white area between the bars


def font(name, size):
    path = os.path.join(FONTS, name)
    if not os.path.exists(path):
        raise SystemExit("font %s not found (expected in %s)" % (name, FONTS))
    return ImageFont.truetype(path, size)


def load(path):
    w, h, rgba, _ = pngio.read_png(open(path, "rb").read())
    return Image.frombytes("RGBA", (w, h), bytes(rgba))


# ------------------------------------------------------------------ 3DS button icons

def icon_round(d, cx, cy, letter, filled):
    r = 11 if not filled else 9
    if filled:
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ICON)
        d.text((cx, cy + 1), letter, font=font(SANS_BOLD, 13), fill="white", anchor="mm")
    else:
        d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=ICON, width=2)
        d.text((cx, cy + 1), letter, font=font(SANS_BOLD, 14), fill=ICON, anchor="mm")


def icon_pill(d, cx, cy, text, w, filled):
    h = 16
    box = (cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2)
    if filled:
        d.rounded_rectangle(box, radius=h // 2, fill=ICON)
        d.text((cx, cy + 1), text, font=font(SANS_BOLD, 9), fill="white", anchor="mm")
    else:
        d.rounded_rectangle(box, radius=h // 2, outline=ICON, width=2)
        d.text((cx, cy + 1), text, font=font(SANS_BOLD, 9), fill=ICON, anchor="mm")


def icon_shoulder(d, cx, cy, letter):
    # the R button: a flat shoulder key, rounded on the outer (right) end
    box = (cx - 15, cy - 8, cx + 15, cy + 8)
    d.rounded_rectangle(box, radius=7, outline=ICON, width=2)
    d.text((cx, cy + 1), letter, font=font(SANS_BOLD, 13), fill=ICON, anchor="mm")


def icon_circle_pad(d, cx, cy):
    d.ellipse((cx - 13, cy - 13, cx + 13, cy + 13), outline=ICON, width=2)
    d.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=(150, 150, 150), outline=ICON, width=2)


def icon_dpad(d, cx, cy, lit):
    """The D-pad; `lit` = the arms to fill: any of 'lrud'."""
    a, k = 13, 5   # arm reach, half width
    arms = {"l": (cx - a, cy - k, cx - k, cy + k), "r": (cx + k, cy - k, cx + a, cy + k),
            "u": (cx - k, cy - a, cx + k, cy - k), "d": (cx - k, cy + k, cx + k, cy + a)}
    outline = [(cx - k, cy - a), (cx + k, cy - a), (cx + k, cy - k), (cx + a, cy - k),
               (cx + a, cy + k), (cx + k, cy + k), (cx + k, cy + a), (cx - k, cy + a),
               (cx - k, cy + k), (cx - a, cy + k), (cx - a, cy - k), (cx - k, cy - k)]
    for arm in lit:
        d.rectangle(arms[arm], fill=ICON)
    d.polygon(outline, outline=ICON, width=2)


# ------------------------------------------------------------------ page edits

def label_buttons(im):
    """Draw the 3DS button that presses it in front of each Back / Next / Exit label."""
    px = im.load()
    d = ImageDraw.Draw(im)
    lab = font(SANS, 19)
    for (x0, y0, x1, y1), key, text in BUTTONS:
        mid = px[(x0 + x1) // 2, (y0 + y1) // 2]
        if not (abs(mid[0] - mid[1]) < 16 and abs(mid[1] - mid[2]) < 16 and mid[0] > 150):
            continue   # no such button on this page (the last page has no Next)
        # the label: the dark pixels inside the button
        xs, ys = [], []
        for y in range(y0 + 4, y1 - 2):
            for x in range(x0 + 4, x1 - 4):
                p = px[x, y]
                if p[0] < 160 and abs(p[0] - p[2]) < 20:
                    xs.append(x)
                    ys.append(y)
        if not xs:
            continue
        # clear it: the button's face only shades from top to bottom, so copy a clean column
        clean = x0 + 6
        for y in range(min(ys) - 2, max(ys) + 3):
            for x in range(x0 + 6, x1 - 5):
                px[x, y] = px[clean, y]
        cy = (min(ys) + max(ys)) // 2 + 1
        tw = d.textlength(text, font=lab)
        iw = 40 if key == "START" else 18
        gap = 6
        left = (x0 + x1) / 2 - (iw + gap + tw) / 2
        if key == "START":
            icon_pill(d, int(left + iw / 2), cy, "START", iw, True)
        else:
            icon_round(d, int(left + iw / 2), cy, key, True)
        d.text((left + iw + gap, cy), text, font=lab, fill=LABEL, anchor="lm")


def controls_page(im):
    """'In Game Controls' with the 3DS's controls (the defaults; Settings > Controls changes them)."""
    d = ImageDraw.Draw(im)
    d.rectangle((BODY[0], BODY[1] + 1, BODY[2] - 1, BODY[3]), fill="white")
    body = font(SERIF, 16)
    note = font(SERIF, 14)   # the page is shown at 2/3 size on the bottom screen
    tx = 133
    rows = [
        ("steer", "Circle Pad or D-pad to steer, or tilt the 3DS"),
        ("A", "Accelerator"),
        ("B", "Brake"),
        ("R", "Turbo (if available)"),
        ("START", "Pause / exit race"),
        ("view", "D-pad Up/Down to Change View"),
    ]
    y, step = 70, 31
    for kind, text in rows:
        if kind == "steer":
            icon_circle_pad(d, 34, y)
            icon_dpad(d, 72, y, "lr")
        elif kind == "view":
            icon_dpad(d, 53, y, "ud")
        elif kind == "R":
            icon_shoulder(d, 53, y, "R")
        elif kind == "START":
            icon_pill(d, 53, y, "START", 44, False)
        else:
            icon_round(d, 53, y, kind, False)
        d.text((tx, y), text, font=body, fill=INK, anchor="lm")
        y += step
    d.text((tx, y - 4), "Gas, brake, turbo and tilt steering: Settings > Controls",
           font=note, fill=(110, 110, 110), anchor="lm")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default=os.path.join(root, "build", "ios_raw"))
    ap.add_argument("--out", default=os.path.join(root, "build", "help"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    pages = sorted(glob.glob(os.path.join(a.raw, "page_*.png")))
    if not pages:
        raise SystemExit("no page_*.png in %s (run tools/extract_ios.py)" % a.raw)
    for path in pages:
        name = os.path.basename(path)
        im = load(path)
        if name in ("page_06.png", "page_06b.png"):
            controls_page(im)
        label_buttons(im)
        out = os.path.join(a.out, name)
        pngio.write_png(out, im.width, im.height, im.tobytes())
    print("wrote %d pages to %s" % (len(pages), a.out))


if __name__ == "__main__":
    main()
