#!/usr/bin/env python3
r"""Build the 3DS HOME Menu banner, banner sound and icon from the WiiWare channel banner.

Input is what tools/banner/wii_banner.py unpacked (build/banner/wii): the
banner layout banner.brlyt (608 x 456), its Start / Loop animations and TPL
textures, the icon's textures and the banner sound.

What the banner shows (top screen, 400 x 240): the Wii banner's checkered
floor fading into black, the "3D Pixel Racing" title logo with its
upside-down reflection on the floor (alpha 70) and the two sun flares at the
logo's top right. The Wii "Licensed by Nintendo" plate is left off. Both the
logo and the reflection bob up and down in opposite phase and the front
flare fades out and back in, as the Wii banner's Loop does:

    pane        Wii                                 3DS
    Picture_00  floor, static                       mesh "floor"      opaque
    Picture_02  reflection, ty -82.8 <-> -92.8      mesh "reflection" blend, alpha 70/255
    Picture_03  big flare, static                   mesh "flare_back" blend
    Picture_01  title, ty 72.2 <-> 85.5             mesh "title"      cutout
    Picture_04  flare, alpha 255 <-> 0              mesh "flare"      blend, Constant0 alpha
    LicensedBY  "Licensed by Nintendo"              left off

The Wii plays Start (frames 0-100) once and then Loop (601 frames, keys from
-100 to 500, period 300). Start's frame 100 is Loop's frame 0, so the two are
one timeline T = Start frame = Loop frame + 100 with keys every 150 frames;
the 3DS animation is one 300-frame period of it starting at T = 0 (so the
banner opens as the Wii one does: logo low, flare lit), looped. Keys keep the
Wii values and slopes; the keys at the loop seam get slope 0 (every key in
the Wii Loop's steady state has |slope| < 0.002 per frame) so the loop has no
kink. The Wii Loop itself jumps at its own wrap (frame 600 -> 0), which is
not copied.

Mapping: Wii layout units -> banner units (10 px each at z = 0) by one
scale + offset, fitted so the logo's opaque pixels at both ends of its bob
stay inside the oval the HOME Menu shows of a banner (seen on a 3DS with the
Hot Pursuit 2 banner: about 93% x 81% of the screen). The floor is stretched
past the screen edges, keeping its horizon where the mapping puts it. Every
quad sits on a YAxial billboard bone (the HOME Menu turns banner models about
Y by itself); the logo and the reflection have bones of their own whose
y translation the skeletal animation drives. Quads are layered 0.05 apart
in z (floor, reflection, back flare, title, flare), their x / y scaled so
each lands where it would at z = 0.

The sound is the Wii banner jingle (1.92 s, mono 11025 Hz), resampled to
32 kHz 16-bit and written as stereo: a mono WAV plays the default chime
instead (seen on a 3DS with the Hot Pursuit 2 banner). The icon is composed
from the banner's own textures at 4x and reduced to 48 x 48: the logo across
the full width over the floor, its reflection, and a small sun in the top
right corner, clear of the letters.

Outputs (default --out "3DS Files/meta"):
    pixelracing/banner.cgfx, pixelracing/banner.wav, icon.png,
    banner.bnr, icon.icn (bannertool makebanner / makesmdh; `make cia` in 3DS Files
    remakes these two from the first three)

Usage:
    python tools/banner/make_banner.py [--static] [--preview] [--frames 0,75,150,225]
        [--wii build/banner/wii] [--out "3DS Files/meta"] [--bannertool exe]

--static writes the banner without animations (the layout the NFS ports
proved on hardware), as a fallback if the animated one misbehaves.
--preview renders frames of the written CGFX (parsed back, so it shows what
the file holds) to build/banner/preview/.
"""
import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import brlyt  # noqa: E402
import cgfx  # noqa: E402
import tpl  # noqa: E402
from cgfx import CAMERA_Y, CAMERA_Z  # noqa: E402

ROOT = HERE.parent.parent

FRAMES = 300                 # one period of the Wii Loop, 60 fps
T0 = -100                    # Loop frame at 3DS frame 0 (= Start frame 0)
SCREEN_W, SCREEN_H = 40.0, 24.0                  # z = 0 plane, banner units
OVAL_A, OVAL_B = 0.93 * SCREEN_W / 2, 0.81 * SCREEN_H / 2   # visible oval, half axes
FIT_MARGIN = 0.94            # the logo stays inside this fraction of the oval
LIFT = 1.3                   # logo bob range centre this far above mid-screen
LAYER_Z = {"floor": 0.0, "reflection": 0.05, "flare_back": 0.10, "title": 0.15, "flare": 0.20}

SOUND_RATE = 32000


# --------------------------------------------------------------------------
# Wii data


def load_wii(wii):
    arc = Path(wii) / "banner" / "arc"
    lay = brlyt.parse_brlyt((arc / "blyt" / "banner.brlyt").read_bytes())
    loop = brlyt.parse_brlan((arc / "anim" / "banner_Loop.brlan").read_bytes())
    start = brlyt.parse_brlan((arc / "anim" / "banner_Start.brlan").read_bytes())
    tex = {}
    for name in lay["textures"]:
        w, h, px = tpl.decode_tpl((arc / "timg" / name).read_bytes())[0]
        tex[name] = np.frombuffer(bytes(px), np.uint8).reshape(h, w, 4).copy()
    panes = {p["name"]: p for p in lay["panes"]}
    return lay, panes, loop, start, tex


def pane_texture(lay, tex, pane):
    mat = lay["materials"][pane["material"]]
    return tex[lay["textures"][mat["textures"][0]]]


def track(anim, pane, tag, target):
    for t, tg, keys in anim["panes"].get(pane, []):
        if t == tag and tg == target:
            return keys
    return None


def wii_eval(keys, frame):
    """NW4R Hermite keys (frame, value, slope), clamped outside."""
    return cgfx.hermite([(f, v, s, s) for f, v, s in keys], frame)


def loop_keys(keys):
    """One 300-frame period of a Wii Loop track starting at Loop frame T0,
    as 3DS keys (frame, value, slope) over [0, FRAMES]; seam keys flat."""
    v0 = wii_eval(keys, T0)
    v1 = wii_eval(keys, T0 + FRAMES)
    assert abs(v0 - v1) < 0.01 * max(1.0, abs(v0)) + 0.5, (v0, v1)
    out = [(0.0, v0, 0.0)]
    for f, v, s in keys:
        if T0 < f < T0 + FRAMES:
            out.append((f - T0, v, s))
    out.append((float(FRAMES), v0, 0.0))
    return out


def check_start_matches_loop(loop, start):
    """The Start animation must lead into Loop frame 0 (one timeline)."""
    for pane, tag, target in (("Picture_01", "RLPA", 1), ("Picture_02", "RLPA", 1),
                              ("Picture_04", "RLVC", 16)):
        s, l_ = track(start, pane, tag, target), track(loop, pane, tag, target)
        a, b = wii_eval(s, 100), wii_eval(l_, 0)
        assert abs(a - b) < 0.5, (pane, a, b)
        assert abs(wii_eval(s, 0) - wii_eval(l_, T0)) < 0.5, pane


# --------------------------------------------------------------------------
# Geometry


ORIGIN_X = {0: 0.0, 1: 0.5, 2: 1.0}       # left, centre, right
ORIGIN_Y = {0: 0.0, 1: 0.5, 2: 1.0}       # top, centre, bottom


def rot_matrix(rx, ry, rz):
    """NW4R pane rotation, degrees, x applied first. The only rotated pane is
    the reflection (6.4 degrees about all three axes, about its right edge);
    turned clockwise for positive angles it meets the logo's bottom edge
    (mean gap ~11 layout units), counter-clockwise it would hang 55 units
    below it, so the angles are taken as clockwise (the x / y tilts only
    foreshorten by 0.6% either way)."""
    rx, ry, rz = (-math.radians(a) for a in (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), \
        math.cos(rz), math.sin(rz)
    mx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    my = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    mz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return mz @ my @ mx


def pane_corners(p):
    """The pane's TL, TR, BL, BR corners relative to its translation, layout
    units, y up, after scale and rotation (projected: the Wii draws
    orthographically). Negative sizes flip, as NW4R does."""
    w, h = p["size"]
    w, h = w * p["scale"][0], h * p["scale"][1]
    left = -w * ORIGIN_X[p["origin"] % 3]
    top = h * ORIGIN_Y[p["origin"] // 3]
    pts = np.array([(left, top, 0), (left + w, top, 0), (left, top - h, 0), (left + w, top - h, 0)],
                   float)
    return (rot_matrix(*p["rot"]) @ pts.T).T[:, :2]


def pane_uvs(p, tex_shape, inset=True):
    """TL, TR, BL, BR (u, v) with v = 1 at the texture's top row; pulled in by
    half a texel so the repeating sampler does not bleed the far edge in."""
    s = np.array(p["uv"][0], float).reshape(4, 2)
    if inset:
        h, w = tex_shape[:2]
        c = s.mean(0)
        s = s + np.sign(c - s) * np.array([0.5 / w, 0.5 / h])
    return np.stack([s[:, 0], 1 - s[:, 1]], 1)


QUAD = [0, 2, 3, 0, 3, 1]       # TL BL BR, TL BR TR: counter-clockwise from the front


class Mapping:
    """Layout (x, y) -> banner units on the z = 0 plane."""

    def __init__(self, s, cx, cy):
        self.s, self.cx, self.cy = s, cx, cy

    def xy(self, pts):
        pts = np.asarray(pts, float)
        return np.stack([(pts[..., 0] - self.cx) * self.s,
                         (pts[..., 1] - self.cy) * self.s + CAMERA_Y], -1)


def at_depth(xy, z):
    """Banner-plane (x, y) -> world (x, y, z) that the camera sees at the same spot."""
    k = (CAMERA_Z - z) / CAMERA_Z
    xy = np.asarray(xy, float)
    return np.concatenate([np.stack([xy[..., 0] * k, CAMERA_Y + (xy[..., 1] - CAMERA_Y) * k], -1),
                           np.full(xy.shape[:-1] + (1,), z)], -1)


def opaque_points(p, tex, ty=None, step=2):
    """Layout positions of the texels of pane `p` with alpha >= 128."""
    h, w = tex.shape[:2]
    ys, xs = np.nonzero(tex[::step, ::step, 3] >= 128)
    u, t = (xs * step + 0.5) / w, (ys * step + 0.5) / h
    c = pane_corners(p)
    tl, tr, bl = c[0], c[1], c[2]
    pts = tl + u[:, None] * (tr - tl) + t[:, None] * (bl - tl)
    x0, y0 = p["trans"][0], p["trans"][1] if ty is None else ty
    return pts + np.array([x0, y0])


def fit_mapping(panes, tex_title, loop):
    """Largest scale that keeps the logo inside the oval at both bob extremes,
    the logo's horizontal centre on the screen centre and its bob range
    centred LIFT above mid-screen."""
    title = panes["Picture_01"]
    ty = [k[1] for k in track(loop, "Picture_01", "RLPA", 1)]
    pts = np.concatenate([opaque_points(title, tex_title, min(ty)),
                          opaque_points(title, tex_title, max(ty))])
    cx = (pts[:, 0].min() + pts[:, 0].max()) / 2
    mid = (pts[:, 1].min() + pts[:, 1].max()) / 2
    a, b = OVAL_A * FIT_MARGIN, OVAL_B * FIT_MARGIN
    rel = pts - np.array([cx, mid])
    # inside the oval centred LIFT below the logo centre: ((x/a)^2 + ((y + LIFT/s)/b)^2) <= 1
    lo, hi = 0.001, 1.0
    for _ in range(60):
        s = (lo + hi) / 2
        q = (rel[:, 0] * s / a) ** 2 + ((rel[:, 1] * s + LIFT) / b) ** 2
        lo, hi = (s, hi) if q.max() <= 1 else (lo, s)
    s = lo
    cy = mid - LIFT / s
    return Mapping(s, cx, cy)


def quad_mesh(name, corners_xy, uvs, z, pivot=(0.0, 0.0)):
    """A quad on the banner plane, put at depth z, bone-local around pivot
    (a banner-plane point, also put at depth z)."""
    world = at_depth(corners_xy, z)
    piv = at_depth(np.array(pivot), z)
    piv[2] = 0.0
    return {"name": name, "pos": world - piv, "uv": np.asarray(uvs, float),
            "indices": list(QUAD)}


# --------------------------------------------------------------------------
# Textures


def premul_resize(rgba, w, h):
    """Resize RGBA without dark fringes: premultiplied, then colours grown
    into the transparent texels (so filtering at a cutout edge stays clean)."""
    f = rgba.astype(np.float64) / 255
    f[..., :3] *= f[..., 3:4]
    chans = [np.asarray(Image.fromarray(f[..., c].astype(np.float32), "F").resize(
        (w, h), Image.LANCZOS), np.float64) for c in range(4)]
    out = np.clip(np.stack(chans, -1), 0, 1)
    a = out[..., 3:4]
    rgb = np.where(a > 1e-4, out[..., :3] / np.maximum(a, 1e-4), 0)
    rgb = grow(np.clip(rgb, 0, 1), a[..., 0] > 0.02)
    return np.clip(np.round(np.concatenate([rgb, a], -1) * 255), 0, 255).astype(np.uint8)


def grow(rgb, have, steps=8):
    rgb = rgb.copy()
    have = have.copy()
    for _ in range(steps):
        acc = np.zeros_like(rgb)
        cnt = np.zeros(have.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            acc += np.roll(rgb * have[..., None], (dy, dx), (0, 1))
            cnt += np.roll(have, (dy, dx), (0, 1))
        new = ~have & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        have |= new
    return rgb


# --------------------------------------------------------------------------
# The scene


def build_scene(wii):
    lay, panes, loop, start, tex = load_wii(wii)
    check_start_matches_loop(loop, start)
    t_floor = pane_texture(lay, tex, panes["Picture_00"])
    t_title_src = pane_texture(lay, tex, panes["Picture_01"])
    t_flare = pane_texture(lay, tex, panes["Picture_04"])
    assert pane_texture(lay, tex, panes["Picture_02"]) is t_title_src
    assert pane_texture(lay, tex, panes["Picture_03"]) is t_flare
    m = fit_mapping(panes, t_title_src, loop)

    # the logo is drawn ~2.4:1 from a square texture: store it 256 x 128
    t_title = premul_resize(t_title_src, 256, 128)
    textures = [("COMMON1", t_floor, cgfx.TEX_RGBA5551),
                ("COMMON2", t_title, cgfx.TEX_RGBA5551),
                ("COMMON3", t_flare, cgfx.TEX_RGBA4)]

    meshes = []
    bones = [{"name": "root", "parent": -1, "trans": (0.0, 0.0, 0.0), "billboard": cgfx.BILLBOARD_OFF},
             {"name": "floor", "parent": 0, "trans": (0.0, 0.0, 0.0), "billboard": cgfx.BILLBOARD_Y}]

    # floor: the pane's quad, stretched to the screen edges; the horizon
    # (first lit texture row) stays where the mapping puts it
    p = panes["Picture_00"]
    c = m.xy(pane_corners(p) + np.array(p["trans"][:2]))
    rows = t_floor[..., :3].mean((1, 2))
    horizon_t = (int(np.argmax(rows > 2)) - 2) / t_floor.shape[0]
    top, bottom = c[0, 1], c[2, 1]
    y_h = top + (bottom - top) * horizon_t
    xl, xr = -SCREEN_W / 2 - 1.5, SCREEN_W / 2 + 1.5
    yt, yb = max(top, CAMERA_Y + SCREEN_H / 2 + 1.0), CAMERA_Y - SCREEN_H / 2 - 1.0
    uv = pane_uvs(p, t_floor.shape)
    u0, u1, v_top, v_bot = uv[0, 0], uv[1, 0], uv[0, 1], uv[2, 1]
    v_h = 1 - horizon_t
    xy = np.array([(xl, yt), (xr, yt), (xl, y_h), (xr, y_h), (xl, yb), (xr, yb)])
    uvs = np.array([(u0, v_top), (u1, v_top), (u0, v_h), (u1, v_h), (u0, v_bot), (u1, v_bot)])
    floor = {"name": "floor", "pos": at_depth(xy, LAYER_Z["floor"]), "uv": uvs,
             "indices": [0, 2, 3, 0, 3, 1, 2, 4, 5, 2, 5, 3]}
    meshes.append(dict(floor, material="mt_floor", texture="COMMON1", mode="opaque", bone=1))

    # reflection and title: a bone each, at the pane's translation
    anims = {}
    for pane, mesh, bone_name, mode, alpha in (
            ("Picture_02", "reflection", "reflection", "blend", panes["Picture_02"]["alpha"] / 255),
            ("Picture_01", "title", "title", "cutout", 1.0)):
        p = panes[pane]
        z = LAYER_Z[mesh]
        keys = track(loop, pane, "RLPA", 1)
        k3 = loop_keys(keys)
        ty0 = k3[0][1]
        pivot = m.xy([0.0, ty0])                          # x = cx: the bone sits at x 0
        pivot[0] = 0.0
        corners = m.xy(pane_corners(p) + np.array([p["trans"][0], ty0]))
        q = quad_mesh(mesh, corners, pane_uvs(p, t_title.shape), z, pivot)
        bone_y = at_depth(pivot, z)[1]
        bones.append({"name": bone_name, "parent": 0, "trans": (0.0, float(bone_y), 0.0),
                      "billboard": cgfx.BILLBOARD_Y})
        k = (CAMERA_Z - z) / CAMERA_Z
        anims[bone_name] = {"ty": [(f, CAMERA_Y + ((v - m.cy) * m.s) * k, sl * m.s * k)
                                   for f, v, sl in k3]}
        meshes.append(dict(q, material="mt_" + mesh, texture="COMMON2", mode=mode,
                           const=(1.0, 1.0, 1.0, alpha), bone=len(bones) - 1))

    # flares, on the floor bone
    for pane, mesh in (("Picture_03", "flare_back"), ("Picture_04", "flare")):
        p = panes[pane]
        corners = m.xy(pane_corners(p) + np.array(p["trans"][:2]))
        q = quad_mesh(mesh, corners, pane_uvs(p, t_flare.shape), LAYER_Z[mesh])
        alpha = p["alpha"] / 255
        meshes.append(dict(q, material="mt_" + mesh, texture="COMMON3", mode="blend",
                           const=(1.0, 1.0, 1.0, alpha), bone=1))
    fk = loop_keys(track(loop, "Picture_04", "RLVC", 16))
    meshes[-1]["const"] = (1.0, 1.0, 1.0, fk[0][1] / 255)

    # Wii pane order: floor, reflection, (plate), back flare, title, flare
    order = ["floor", "reflection", "flare_back", "title", "flare"]
    meshes.sort(key=lambda q: order.index(q["name"]))
    return {
        "textures": textures, "bones": bones, "meshes": meshes, "frames": FRAMES,
        "bone_anims": anims,
        "material_anims": {"mt_flare": {"a": [(f, v / 255, s / 255) for f, v, s in fk]}},
        "mapping": m, "title_src": t_title_src, "floor_src": t_floor, "flare_src": t_flare,
        "wii": (lay, panes, loop),
    }


# --------------------------------------------------------------------------
# Sound


def resample(x, n_out):
    """Band-limited resample by zero-padding the spectrum."""
    spec = np.fft.rfft(x)
    out = np.zeros(n_out // 2 + 1, complex)
    k = min(len(spec), len(out))
    out[:k] = spec[:k]
    return np.fft.irfft(out, n_out) * (n_out / len(x))


def build_sound(wii):
    """The Wii jingle as 16-bit stereo at 32 kHz (both channels the same)."""
    path = Path(wii) / "sound.wav"
    with wave.open(str(path), "rb") as w:
        rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
        assert sw == 2
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(np.float64)
    if ch > 1:
        x = x.reshape(-1, ch).mean(1)
    n = int(round(len(x) * SOUND_RATE / rate))
    y = resample(x, n)
    fin, fout = int(0.004 * SOUND_RATE), int(0.03 * SOUND_RATE)
    y[:fin] *= np.linspace(0, 1, fin)
    y[-fout:] *= np.linspace(1, 0, fout)
    peak = np.abs(y).max()
    if peak > 32767 * 0.98:
        y *= 32767 * 0.98 / peak
    y = np.round(y).astype("<i2")
    return SOUND_RATE, np.stack([y, y], 1), (rate, ch, len(x) / rate)


def write_wav(path, rate, frames):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(frames.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.ascontiguousarray(frames, "<i2").tobytes())


# --------------------------------------------------------------------------
# Icon


ICON_LOGO_ASPECT = 1.45      # the banner draws the logo 2.4:1; taller letters read at 48 px


def build_icon(scene, size=48, ss=4):
    """48 x 48: the floor under a black sky, the logo across the full width
    (drawn a little taller than the banner's 2.4:1 so both words read), and a
    small sun above its top right corner -- clear of the letters, which the
    Wii icon's flares smeared at this size. No reflection: at 48 px it is
    only noise."""
    S = size * ss
    floor = scene["floor_src"]
    rows = floor[..., :3].mean((1, 2))
    hz = int(np.argmax(rows > 2))
    # the floor from a little above its horizon down, softened (its tiles
    # alias into noise at 48 px), filling the square
    crop = floor[max(0, hz - 70):, :, :3]
    bg = Image.fromarray(crop).resize((S, S), Image.LANCZOS).filter(
        ImageFilter.GaussianBlur(ss * 0.6))
    canvas = Image.eval(bg, lambda v: int(v * 0.8)).convert("RGBA")

    title = scene["title_src"]
    ys, xs = np.nonzero(title[..., 3] >= 16)
    crop = title[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    tw = S - 2 * ss
    th = int(round(tw / ICON_LOGO_ASPECT))
    logo = premul_resize(crop, tw, th)
    # a dark outline so the white "RACING" stands off the grey floor
    a = Image.fromarray(logo[..., 3]).filter(ImageFilter.MaxFilter(2 * ss + 1)).filter(
        ImageFilter.GaussianBlur(ss * 0.5))
    halo = np.zeros((th, tw, 4), np.uint8)
    halo[..., 3] = (np.asarray(a) * 0.85).astype(np.uint8)
    lx, ly = ss, int(round(S * 0.58 - th / 2))
    canvas.alpha_composite(Image.fromarray(halo, "RGBA"), (lx, ly))
    canvas.alpha_composite(Image.fromarray(logo, "RGBA"), (lx, ly))

    # the sun: the flare texture's brightest spot, padded so it is not cut
    fl = np.pad(scene["flare_src"], ((64, 64), (64, 64), (0, 0)))
    blur = np.asarray(Image.fromarray(fl[..., 3]).filter(ImageFilter.GaussianBlur(4)))
    fy, fx = np.unravel_index(np.argmax(blur), blur.shape)
    r = 40              # the texture is drawn ~2:1 wide (128 x 256 on a square pane)
    core = fl[fy - r:fy + r, fx - r // 2:fx + r // 2]
    d = int(S * 0.42)
    sun = premul_resize(core, d, d)
    canvas.alpha_composite(Image.fromarray(sun, "RGBA"), (S - d + int(S * 0.08), ly - d // 2 - ss))

    icon = canvas.convert("RGB").resize((size, size), Image.LANCZOS)
    icon = icon.filter(ImageFilter.UnsharpMask(radius=0.8, percent=50, threshold=2))
    return icon


def build_bottom(g, frame=150):
    """320 x 240: the banner scene for the game's bottom screen (shown when the game draws
    nothing there). The banner camera frames a 400 x 240 view; the vertical field of view is
    widened 7% so the logo keeps a margin on the narrower 4:3 screen while the floor still
    reaches its bottom edge."""
    fov = cgfx.CAMERA_FOV
    try:
        cgfx.CAMERA_FOV = math.degrees(2 * math.atan(math.tan(math.radians(fov / 2)) * 1.07))
        return Image.fromarray(cgfx.render(g, frame, width=320, height=240))
    finally:
        cgfx.CAMERA_FOV = fov


# --------------------------------------------------------------------------
# bannertool and the BNR check


def lz11_decompress(d):
    assert d[0] == 0x11
    n = d[1] | d[2] << 8 | d[3] << 16
    p = 4
    if n == 0:
        n = struct.unpack_from("<I", d, 4)[0]
        p = 8
    out = bytearray()
    while len(out) < n:
        flags = d[p]
        p += 1
        for bit in range(8):
            if len(out) >= n:
                break
            if not flags & (0x80 >> bit):
                out.append(d[p])
                p += 1
                continue
            b0 = d[p]
            ind = b0 >> 4
            if ind == 0:
                length = (((b0 & 15) << 4) | (d[p + 1] >> 4)) + 0x11
                disp = ((d[p + 1] & 15) << 8 | d[p + 2]) + 1
                p += 3
            elif ind == 1:
                length = (((b0 & 15) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111
                disp = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1
                p += 4
            else:
                length = ind + 1
                disp = ((b0 & 15) << 8 | d[p + 1]) + 1
                p += 2
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out[:n])


def check_bnr(bnr, cgfx_bytes):
    """The BNR's common CGFX (LZ11 in the CBMD) must be ours, byte for byte."""
    d = Path(bnr).read_bytes()
    assert d[:4] == b"CBMD", "not a CBMD"
    off = struct.unpack_from("<I", d, 8)[0]
    cwav = struct.unpack_from("<I", d, 0x84)[0]
    got = lz11_decompress(d[off:cwav if cwav > off else len(d)])
    assert got == cgfx_bytes, "banner.bnr does not hold banner.cgfx"
    assert d[cwav:cwav + 4] == b"CWAV", "no CWAV in banner.bnr"
    return len(d), cwav


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or "[ERROR]" in (r.stdout + r.stderr):
        raise SystemExit("failed: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))


# --------------------------------------------------------------------------
# Preview


def preview(g, scene, out, frames, icon):
    out.mkdir(parents=True, exist_ok=True)
    imgs = []
    for f in frames:
        img = Image.fromarray(cgfx.render(g, f))
        img.save(out / ("frame_%03d.png" % f))
        imgs.append((f, img))
    # contact sheet: frames with the visible oval and the logo-fit oval drawn in
    pad = 8
    sheet = Image.new("RGB", (2 * 400 + 3 * pad, 2 * 240 + 3 * pad + 220), (40, 40, 48))
    dr = ImageDraw.Draw(sheet)
    for i, (f, img) in enumerate(imgs[:4]):
        x, y = pad + (i % 2) * (400 + pad), pad + (i // 2) * (240 + pad)
        sheet.paste(img, (x, y))
        for k, col in ((1.0, (255, 80, 80)), (FIT_MARGIN, (80, 200, 255))):
            a, b = OVAL_A * k * 10, OVAL_B * k * 10
            dr.ellipse([x + 200 - a, y + 120 - b, x + 200 + a, y + 120 + b], outline=col)
        dr.text((x + 4, y + 4), "frame %d" % f, fill=(255, 255, 0))
    y = 2 * 240 + 3 * pad
    sheet.paste(icon.resize((192, 192), Image.NEAREST), (pad, y + 12))
    sheet.paste(icon, (pad + 200, y + 12))
    # the curves
    cx0, cw, ch = pad + 270, 2 * 400 + pad - 270, 190
    dr.rectangle([cx0, y + 12, cx0 + cw, y + 12 + ch], outline=(120, 120, 120))
    bn = [b["name"] for b in g["model"]["bones"]]
    ti, ri = bn.index("title"), bn.index("reflection")
    world_fn = [(lambda f: cgfx.pose(g, f)[0][ti][1], (255, 200, 80), "title y"),
                (lambda f: cgfx.pose(g, f)[0][ri][1], (160, 160, 255), "reflection y"),
                (lambda f: cgfx.pose(g, f)[1]["mt_flare"][3], (255, 255, 255), "flare alpha")]
    fs = np.arange(0, FRAMES + 1, 2)
    for i, (fn, col, label) in enumerate(world_fn):
        v = np.array([fn(f) for f in fs])
        lo, hi = v.min(), v.max()
        if hi - lo < 1e-9:
            hi = lo + 1
        pts = [(cx0 + f / FRAMES * cw, y + 12 + ch - 6 - (val - lo) / (hi - lo) * (ch - 12))
               for f, val in zip(fs, v)]
        dr.line(pts, fill=col)
        dr.text((cx0 + 6, y + 16 + 12 * i), "%s %.2f..%.2f" % (label, lo, hi), fill=col)
    sheet.save(out / "preview_sheet.png")
    icon.resize((240, 240), Image.NEAREST).save(out / "icon_x5.png")
    return [out / ("frame_%03d.png" % f) for f in frames] + [out / "preview_sheet.png",
                                                             out / "icon_x5.png"]


# --------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--wii", default=str(ROOT / "build" / "banner" / "wii"))
    ap.add_argument("--out", default=str(ROOT / "3DS Files" / "meta"))
    ap.add_argument("--bannertool", default=str(ROOT / "tools" / "3ds-bin" / "bannertool.exe"))
    ap.add_argument("--static", action="store_true", help="no animations (proven layout)")
    ap.add_argument("--preview", action="store_true", help="render frames to build/banner/preview")
    ap.add_argument("--preview-dir", default=str(ROOT / "build" / "banner" / "preview"))
    ap.add_argument("--frames", default="0,75,150,225")
    ap.add_argument("--bottom", default=str(ROOT / "build" / "banner" / "bottom_logo.png"),
                    help="bottom-screen logo image (tools/build_romfs.py puts it in the RomFS)")
    args = ap.parse_args()
    animate = not args.static

    scene = build_scene(args.wii)
    data = cgfx.build_cgfx(scene, animate=animate)
    g, notes = cgfx.check_cgfx(data, scene, animate=animate)

    out = Path(args.out)
    sub = out / "pixelracing"
    sub.mkdir(parents=True, exist_ok=True)
    (sub / "banner.cgfx").write_bytes(data)
    rate, pcm, (src_rate, src_ch, src_len) = build_sound(args.wii)
    write_wav(sub / "banner.wav", rate, pcm)
    icon = build_icon(scene)
    icon.save(out / "icon.png")
    Path(args.bottom).parent.mkdir(parents=True, exist_ok=True)
    build_bottom(g).save(args.bottom)

    m = scene["mapping"]
    print("banner.cgfx (%s):" % ("static" if args.static else "animated"))
    for n in notes:
        print("  " + n)
    print("  mapping: %.4f units per layout unit, layout (%.1f, %.1f) at screen centre x / y %.1f"
          % (m.s, m.cx, m.cy, CAMERA_Y))
    print("banner.wav: %.2f s, %d Hz %d ch -> %d Hz 2 ch 16-bit" % (src_len, src_rate, src_ch, rate))
    print("icon.png 48x48, %s 320x240" % Path(args.bottom).name)

    bt = args.bannertool
    if os.path.exists(bt):
        run([bt, "makebanner", "-ci", str(sub / "banner.cgfx"), "-a", str(sub / "banner.wav"),
             "-o", str(out / "banner.bnr")])
        # SMDH text follows the user's other ports (ZombieOutbreakSim-3DS, NFSHP2-3DS): the
        # HOME Menu bubble shows the long description over the publisher.
        run([bt, "makesmdh", "-s", "3D Pixel Racing",
             "-l", "3D Pixel Racing iOS Game Port to Nintendo 3DS", "-p", "Octoblimp",
             "-i", str(out / "icon.png"), "-o", str(out / "icon.icn")])
        size, cwav = check_bnr(out / "banner.bnr", data)
        print("banner.bnr %d bytes (CGFX checked, CWAV at 0x%x), icon.icn %d bytes"
              % (size, cwav, (out / "icon.icn").stat().st_size))
    else:
        print("bannertool not found (%s): banner.bnr / icon.icn not made" % bt)

    if args.preview:
        frames = [int(f) for f in args.frames.split(",")]
        for p in preview(g, scene, Path(args.preview_dir), frames, icon):
            print("preview", p.relative_to(ROOT) if ROOT in p.parents else p)


if __name__ == "__main__":
    main()
