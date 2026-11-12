#!/usr/bin/env python3
r"""CGFX (NintendoWare for CTR graphics) writer and reader for HOME Menu banners.

Writer
    The DATA/IMAG writer, the DICT Patricia trees, textures, materials, shapes
    and the skeleton are copied from Need for Speed: Hot Pursuit 2 3DS's
    tools/make_banner.py (the same code is in Need for Speed: Underground 2
    3DS), whose banners were checked on a 3DS. Its struct layouts are the ones
    skyfloogle's pycgfx writes (github.com/skyfloogle/pycgfx). What those
    banners taught, and what this file keeps:
      * the model and every animation are named COMMON;
      * RGBA8 textures do not show -- RGBA5551 / RGBA4 are used here;
      * the whole CGFX must stay under 0x80000 bytes;
      * the camera sits at (0, 1, 44.786) with a 30 degree vertical FOV, so
        the z = 0 plane shows 40 x 24 units (10 top-screen pixels per unit);
      * textures are stored top row first, v = 1 at the top of the picture;
      * the HOME Menu turns a banner model about Y by itself (once in ~10 s);
        a YAxial billboard bone keeps what hangs from it facing the camera;
      * only a screen-sized oval of the banner shows.

    Changed or added here (new, NOT yet seen on a 3DS):
      * materials have three modes: opaque, cutout (alpha test >= 128, the
        proven one) and blend (SrcAlpha / InvSrcAlpha, depth test without
        depth writes, TEV stage 0 alpha = texture alpha x Constant0 alpha, so
        a material colour animation of Constant0 fades the mesh);
      * bones carry a base translation (flags without IDENTITY / T0);
      * RGBA4 textures get their alpha dithered too;
      * animations (CANM), below.

Animations (CANM)
    Laid out as Ohana3DS / SPICA (GFXAnimation) read NW4C's ResAnim:

      CANM  "CANM", revision, name, target anim group name, loop mode
            (1 = loop), frame count (f32), DICT of member animations,
            DICT of user data (empty)
      member animation   flags, path, primitive type, then one 4-byte slot per
            component: a self-relative offset to a curve (flag bit clear) or
            an inline constant (flag bit set). Every slot written here is a
            curve and the flags are 0, so the constant / "not exist" bit
            positions (which readers disagree on) do not matter.
              primitive type 4 Transform: 9 slots scale xyz, rotate xyz
                                          (radians), translate xyz
              primitive type 5 RGBA:      4 slots r g b a
            The path is the AnimGroup member's name: the bone name for
            "SkeletalAnimation", 'Materials["m"].MaterialColor.Constant0' for
            "MaterialAnimation" (the model's AnimGroups list every bindable
            member already, as pycgfx writes them).
      curve start frame, end frame (f32), pre / post repeat (u8, 0 = clamp),
            u16 pad, u32 flags (bit 0 constant; bits 2-3 interpolation,
            2 = hermite; bits 5-7 quantization, 0 = Hermite128), u32 key
            count, f32 1 / (end - start), then Hermite128 keys
            (frame, value, in slope, out slope; slopes per frame).

    Assumptions (cannot be checked off hardware): the HOME Menu binds the
    skeletal animation "COMMON" (DATA dict 9) and the material animation
    "COMMON" (DATA dict 10) to the model and loops them at one animation
    frame per 60 Hz frame; animation dict entries point straight at the
    "CANM" magic (no type word, unlike CMDL / TXOB); the CANM revision value;
    the primitive type numbers (NW4C order Float, Int, Vector2, Vector3,
    Transform, RGBA, Texture, BakedTransform, Bool, FullBakedTransform, as
    BCH uses); the single inline segment per curve. build_cgfx(...,
    animate=False) writes the proven static layout (no CANM) as a fallback.

Reader
    parse_cgfx(data) re-reads a file this writer made: header, DATA dicts
    (Patricia lookups checked for every name), model, skeleton, materials,
    shapes, textures (unswizzled) and animation curves; check_cgfx(data)
    asserts the offsets, sizes and bindings are consistent. render(model,
    frame) draws the parsed model through the banner camera (numpy only),
    which make_banner.py's --preview uses, so the previews show what the
    file holds rather than what was meant to go into it.
"""
import math
import struct
import zlib

import numpy as np

CAMERA_Z = 44.786        # HOME Menu banner camera: (0, 1, 44.786), yfov 30
CAMERA_Y = 1.0
CAMERA_FOV = 30.0
CGFX_LIMIT = 0x80000

# --------------------------------------------------------------------------
# CGFX writer
#
# A Node is one struct in the DATA block, a list of (kind, value) fields:
#   i I f h H b B ?   scalars          x       n bytes of padding
#   vec               floats           sig     4-byte signature
#   str               string pointer   ptr     pointer to a Node laid out here
#   ref               pointer to a Node owned somewhere else
#   list / ilist      (count, pointer to a table of Node pointers / of ints)
#   dict              (count, pointer to a DICT of (name, Node))
#   blob              (size, pointer into IMAG)
# Every pointer is self-relative. Nodes are laid out as the template banner
# has them: a node's owned children together in field order, then each
# child's own children the same way; the strings after the last node, then
# the IMAG block with the vertex, index and texel data.


class Blob:
    def __init__(self, data, align=16):
        self.data = bytes(data)
        self.align = align


class Node:
    def __init__(self, fields, tag=""):
        self.tag = tag
        self.off = None
        self.fields = []
        for kind, val in fields:
            if kind == "list":
                val = (len(val), Node([("ptr", n) for n in val], tag + "[]") if val else None)
            elif kind == "ilist":
                val = (len(val), Node([("i", v) for v in val], tag + "[]") if val else None)
            elif kind == "dict":
                val = (len(val), make_dict(val) if val else None)
            self.fields.append((kind, val))
        self.size = sum(field_size(k, v) for k, v in self.fields)
        assert self.size % 4 == 0, (tag, self.size)


FIXED = {"i": 4, "I": 4, "f": 4, "h": 2, "H": 2, "b": 1, "B": 1, "?": 1,
         "sig": 4, "str": 4, "ptr": 4, "ref": 4,
         "list": 8, "ilist": 8, "dict": 8, "blob": 8}


def field_size(kind, val):
    if kind == "x":
        return val
    if kind == "vec":
        return 4 * len(val)
    return FIXED[kind]


def owned(kind, val):
    if kind == "ptr":
        return val
    if kind in ("list", "ilist", "dict"):
        return val[1]
    return None


# The DICT is a Patricia tree over the names, as NintendoWare builds it; this
# is pycgfx's translation of Gericom's PatriciaTreeGenerator.

class _PNode:
    pass


def _bit(name, bit):
    return (ord(name[bit // 8]) >> (bit & 7)) & 1


def _lookup(root, length, name, minbit=-1):
    name = name.ljust(length, "\0")
    cur = root
    left = cur.left
    while cur.refbit > left.refbit and left.refbit > minbit:
        cur = left
        left = cur.right if _bit(name, cur.refbit) else cur.left
    return left, cur


def patricia(names):
    """Return (refbit, left, right) for the root entry and for each name."""
    length = max(len(n) for n in names)
    root = _PNode()
    root.refbit, root.left, root.right, root.idx = length * 8 - 1, root, root, 0
    root.name = "\0" * length
    for idx, name in sorted(enumerate(names), key=lambda k: -len(k[1])):
        name = name.ljust(length, "\0")
        node = _PNode()
        node.name, node.idx = name, idx
        left = _lookup(root, length, name)[0]
        bit = length * 8 - 1
        while _bit(left.name, bit) == _bit(name, bit):
            bit -= 1
        left, cur = _lookup(root, length, name, bit)
        node.refbit = bit
        node.left = left if _bit(name, bit) else node
        node.right = node if _bit(name, bit) else left
        if _bit(name, cur.refbit):
            cur.right = node
        else:
            cur.left = node
    root.idx = -1
    out = []
    for i, name in enumerate([""] + list(names)):
        p = _lookup(root, length, name)[0]
        assert p.name == name.ljust(length, "\0"), name
        out.append((-1 if i == 0 else p.refbit, p.left.idx + 1, p.right.idx + 1))
    return out


def make_dict(entries):
    names = [n for n, _ in entries]
    assert len(set(names)) == len(names), names
    tree = patricia(names)
    fields = [("sig", b"DICT"), ("i", 12 + 16 * (len(entries) + 1)), ("i", len(entries))]
    rb, lf, rt = tree[0]
    fields += [("i", rb), ("h", lf), ("h", rt), ("str", None), ("ptr", None)]
    for (name, node), (rb, lf, rt) in zip(entries, tree[1:]):
        fields += [("i", rb), ("h", lf), ("h", rt), ("str", name), ("ptr", node)]
    return Node(fields, "DICT")


def write_cgfx(data_node):
    order = []

    def layout(node, pos):              # a node's children together, then theirs
        kids = [owned(k, v) for k, v in node.fields if owned(k, v) is not None]
        for child in kids:
            child.off = pos
            order.append(child)
            pos += child.size
        for child in kids:
            pos = layout(child, pos)
        return pos

    data_node.off = 0x14
    order.append(data_node)
    end = layout(data_node, 0x14 + data_node.size)

    strings, blobs = {}, []
    pool = bytearray()
    for node in order:
        for kind, val in node.fields:
            if kind == "str" and val is not None and val not in strings:
                strings[val] = end + len(pool)
                pool += val.encode("ascii") + b"\0"
            elif kind == "blob" and val is not None:
                blobs.append(val)
    str_end = end + len(pool)
    imag = (str_end + 8 + 127) // 128 * 128 - 8       # IMAG data 128-aligned
    blob_at = {}
    pos = imag + 8
    for b in blobs:
        pos = (pos + b.align - 1) // b.align * b.align
        blob_at[id(b)] = pos
        pos += len(b.data)
    total = (pos + 15) // 16 * 16

    kind0, _ = data_node.fields[1]
    data_node.fields[1] = (kind0, imag - 0x14)        # DATA section size

    buf = bytearray(total)
    struct.pack_into("<4sHHIII", buf, 0, b"CGFX", 0xFEFF, 0x14, 0x5000000, total, 2)

    def rel(p, target):
        struct.pack_into("<i", buf, p, 0 if target is None else target - p)

    for node in order:
        p = node.off
        for kind, val in node.fields:
            if kind in ("i", "I", "f", "h", "H", "b", "B", "?"):
                struct.pack_into("<" + kind, buf, p, val)
            elif kind == "vec":
                struct.pack_into("<%df" % len(val), buf, p, *val)
            elif kind == "sig":
                buf[p:p + 4] = val
            elif kind == "str":
                rel(p, None if val is None else strings[val])
            elif kind in ("ptr", "ref"):
                rel(p, None if val is None else val.off)
            elif kind in ("list", "ilist", "dict"):
                struct.pack_into("<i", buf, p, val[0])
                rel(p + 4, None if val[1] is None else val[1].off)
            elif kind == "blob":
                if val is not None:
                    struct.pack_into("<i", buf, p, len(val.data))
                    rel(p + 4, blob_at[id(val)])
            p += field_size(kind, val)
        assert p == node.off + node.size
    buf[end:str_end] = pool
    struct.pack_into("<4sI", buf, imag, b"IMAG", total - imag)
    for b in blobs:
        o = blob_at[id(b)]
        buf[o:o + len(b.data)] = b.data
    return bytes(buf)


# --------------------------------------------------------------------------
# CGFX objects


IDENTITY34 = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]

TEX_RGBA5551, TEX_RGB565, TEX_RGBA4 = 2, 3, 4
GL_FORMAT = {TEX_RGBA5551: (0x6752, 0x8034), TEX_RGB565: (0x6754, 0x8363),
             TEX_RGBA4: (0x6752, 0x8033)}
TEX_NAMES = {TEX_RGBA5551: "RGBA5551", TEX_RGB565: "RGB565", TEX_RGBA4: "RGBA4"}

# bone flags
B_IDENTITY, B_T0, B_R0, B_S1, B_UNIFORM = 1, 2, 4, 8, 16
B_RENDER, B_LOCAL, B_WORLD = 64, 128, 256

BILLBOARD_OFF, BILLBOARD_Y = 0, 5          # 5: YAxial, turned about Y to face the camera

# CANM
CANM_REVISION = 0x5000000
LOOP_ONCE, LOOP_REPEAT = 0, 1
PRIM_TRANSFORM, PRIM_RGBA = 4, 5
INTERP_HERMITE = 2
QUANT_HERMITE128 = 0
CURVE_FLAGS = INTERP_HERMITE << 2 | QUANT_HERMITE128 << 5
TRANSFORM_PARTS = ["sx", "sy", "sz", "rx", "ry", "rz", "tx", "ty", "tz"]
RGBA_PARTS = ["r", "g", "b", "a"]
MATERIAL_COLORS = ["Emission", "Ambient", "Diffuse", "Specular0", "Specular1",
                   "Constant0", "Constant1", "Constant2", "Constant3",
                   "Constant4", "Constant5"]


def morton8():
    m = np.zeros(64, int)
    for y in range(8):
        for x in range(8):
            v = 0
            for b in range(3):
                v |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
            m[y * 8 + x] = v
    return m


BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]])


def dither(rgba, bits, alpha_bits=None):
    """Ordered dither ahead of truncation: the colour channels to `bits`, the
    alpha channel to `alpha_bits` when given (left alone otherwise)."""
    h, w = rgba.shape[:2]
    bay = (np.tile(BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w] + 0.5) / 16
    out = rgba.astype(np.float64)
    out[..., :3] += (bay * (256 >> bits))[..., None]
    if alpha_bits:
        a = out[..., 3]
        lvl = (1 << alpha_bits) - 1
        # dither between the two nearest representable levels, keeping 0 and 255 exact
        q = np.floor(a / 255 * lvl + bay[::-1, ::-1]) * (255 / lvl)
        out[..., 3] = np.where((a <= 0) | (a >= 255), a, q)
    return np.clip(out, 0, 255).astype(np.uint8)


def quantize(rgba, fmt):
    """What the GPU will see of an RGBA uint8 image in `fmt`, as RGBA uint8."""
    if fmt == TEX_RGBA5551:
        q = dither(rgba, 5)
        out = q.astype(int) >> 3 << 3
        out[..., :3] |= out[..., :3] >> 5
        out[..., 3] = np.where(q[..., 3] >= 128, 255, 0)
    elif fmt == TEX_RGB565:
        q = dither(rgba, 5)
        out = q.astype(int)
        out[..., 0] = (out[..., 0] >> 3) * 255 // 31
        out[..., 1] = (out[..., 1] >> 2) * 255 // 63
        out[..., 2] = (out[..., 2] >> 3) * 255 // 31
        out[..., 3] = 255
    else:
        q = dither(rgba, 4, 4)
        out = (q.astype(int) >> 4) * 17
    return out.astype(np.uint8)


def swizzle(rgba, fmt):
    """RGBA uint8 (h, w, 4), top row first -> PICA tiled texels, top first.

    The HOME Menu samples the first stored row at t = 1, so the image is
    stored as it is and the UVs run from v = 0 at its bottom to 1 at its
    top. That is how bannertool's own banner.cgfx stores its picture; a
    3DS show of the banner with the rows flipped had every picture upside
    down."""
    img = quantize(rgba, fmt).astype(np.uint32)
    h, w = img.shape[:2]
    assert h % 8 == 0 and w % 8 == 0, (w, h)
    r, g, b, a = img[..., 0], img[..., 1], img[..., 2], img[..., 3]
    if fmt == TEX_RGBA5551:
        px = (a >> 7) | ((b & 0xF8) >> 2) | ((g & 0xF8) << 3) | ((r & 0xF8) << 8)
    elif fmt == TEX_RGB565:
        px = (b >> 3) | ((g >> 2) << 5) | ((r >> 3) << 11)
    else:
        px = (a >> 4) | (b & 0xF0) | ((g >> 4) << 8) | ((r & 0xF0) << 8)
    tiles = px.reshape(h // 8, 8, w // 8, 8).transpose(0, 2, 1, 3).reshape(-1, 64)
    out = np.zeros_like(tiles)
    out[:, morton8()] = tiles
    return out.astype("<u2").tobytes()


def unswizzle(raw, w, h, fmt):
    """Inverse of swizzle(): PICA tiled 16-bit texels -> RGBA uint8, top row first."""
    tiles = np.frombuffer(raw, "<u2").astype(np.uint32).reshape(-1, 64)[:, morton8()]
    px = tiles.reshape(h // 8, w // 8, 8, 8).transpose(0, 2, 1, 3).reshape(h, w)
    out = np.zeros((h, w, 4), np.uint32)
    if fmt == TEX_RGBA5551:
        for c, sh in enumerate((11, 6, 1)):
            v = (px >> sh) & 31
            out[..., c] = v << 3 | v >> 2
        out[..., 3] = (px & 1) * 255
    elif fmt == TEX_RGB565:
        out[..., 0] = ((px >> 11) & 31) * 255 // 31
        out[..., 1] = ((px >> 5) & 63) * 255 // 63
        out[..., 2] = (px & 31) * 255 // 31
        out[..., 3] = 255
    elif fmt == TEX_RGBA4:
        for c, sh in enumerate((12, 8, 4, 0)):
            out[..., c] = ((px >> sh) & 15) * 17
    else:
        raise ValueError("texture format %d" % fmt)
    return out.astype(np.uint8)


_TXOB_NAMES = {}


def txob_name(txob):
    return _TXOB_NAMES[id(txob)]


def image_texture(name, rgba, fmt):
    h, w = rgba.shape[:2]
    image = Node([("i", h), ("i", w), ("blob", Blob(swizzle(rgba, fmt), 128)),
                  ("i", 0), ("i", 16), ("i", 0), ("i", 0)], "IMAGE")
    glf, glt = GL_FORMAT[fmt]
    tx = Node([("I", 0x20000011), ("sig", b"TXOB"), ("i", 0x5000000), ("str", name),
               ("dict", []), ("i", h), ("i", w), ("i", glf), ("i", glt), ("i", 1),
               ("x", 4), ("i", 0), ("i", fmt), ("ptr", image)], "TXOB")
    _TXOB_NAMES[id(tx)] = name
    return tx


def color_bytes(c):
    return [("B", min(255, int(round(v * 255)))) for v in c]


def pack_rgba8(c):
    r, g, b, a = (min(255, max(0, int(round(v * 255)))) for v in c)
    return r | g << 8 | b << 16 | a << 24


MODES = ("opaque", "cutout", "blend")


def material(name, txob, mode, const=(1.0, 1.0, 1.0, 1.0)):
    """A one-texture unlit material: TEV stage 0 replaces the colour with
    texture 0. `mode`: 'opaque'; 'cutout' alpha-tests at 128 (the proven
    one); 'blend' blends SrcAlpha / InvSrcAlpha without writing depth, its
    alpha texture alpha x Constant0 alpha (`const`, which a material
    animation of MaterialColor.Constant0 can change)."""
    assert mode in MODES, mode
    translucent = mode == "blend"
    cmds = [0, 0x1008E, 0xFF000000, 0x809F0081, 0, 0x2206] + [0] * 8
    texinfo = Node([], "TEXI")
    reftex = Node([("I", 0x20000004), ("sig", b"TXOB"), ("i", 0x5000000), ("str", ""),
                   ("dict", []), ("str", txob_name(txob)), ("ref", txob)], "REFTEX")
    sampler = Node([("I", 0x80000000), ("ref", texinfo), ("i", 1),
                    ("vec", [0, 0, 0, 1])], "SAMPLER")
    texinfo.__init__([("I", 0x80000000), ("i", 0), ("ptr", reftex), ("ptr", sampler)]
                     + [("I", c) for c in cmds] + [("i", 0x38)], "TEXI")

    shader = Node([("I", 0x80000001), ("sig", b"SHDR"), ("i", 0x5000000), ("str", ""),
                   ("dict", []), ("str", "DefaultShader"), ("x", 4)], "SHDR")
    light_table = Node([("i", 0)] * 6, "LIGHTTABLE")
    tev = []
    for i in range(6):
        if i == 0:      # rgb = texture 0; alpha = texture 0 (x constant 0 when blending)
            src_rgb, src_a = 0xFF3, (0xFE3 if translucent else 0xFF3)
            comb_a, color = (1 if translucent else 0), pack_rgba8(const)
        else:           # pass the previous stage on
            src_rgb = src_a = 0xFFF
            comb_a, color = 0, 0
        tev += [("i", 0), ("H", src_rgb), ("H", src_a),          # constant: Constant0
                ("I", 0x804F0000 | ((0xC0 if i < 4 else 0xD0) + i * 8)),
                ("i", 0), ("H", 0), ("H", comb_a), ("I", color), ("H", 0), ("H", 0)]
    alpha_test = (1 | 7 << 4 | 128 << 8) if mode == "cutout" else 0x10   # GEQUAL 128 / off
    fshader = Node([("vec", [0, 0, 0, 0])] + [("i", 0)] * 6 + [("ptr", light_table)] + tev
                   + [("I", alpha_test), ("I", 0xF0104),
                      ("I", 0xFF000000), ("I", 0xF00FD), ("I", 0), ("I", 0x200E0),
                      ("I", 0x400), ("I", 0x201C3)], "FSHADER")

    colors = [(0, 0, 0, 0), (0, 0, 0, 1), (1, 1, 1, 1), (1, 1, 1, 0), (0, 0, 0, 0),
              tuple(const)] + [(0, 0, 0, 0)] * 5
    if translucent:     # SrcAlpha, InvSrcAlpha for colour and alpha
        blend = 0 | 0 << 8 | 6 << 16 | 7 << 20 | 6 << 24 | 7 << 28
    else:               # One, Zero
        blend = 0 | 0 << 8 | 1 << 16 | 0 << 20 | 1 << 24 | 0 << 28
    coord = [("i", 0)] * 4 + [("f", 1.0), ("f", 1.0), ("f", 0.0), ("f", 0.0), ("f", 0.0),
                              ("?", False), ("x", 3), ("vec", IDENTITY34)]
    sampler_hash = zlib.crc32(name.encode() + struct.pack("<14I", *cmds))
    fields = [("I", 0x8000000), ("sig", b"MTOB"), ("i", 0x6000000), ("str", name),
              ("dict", []), ("i", 0), ("i", 0), ("i", 1 if translucent else 0)]
    fields += [("vec", list(c)) for c in colors]
    for c in colors:
        fields += color_bytes(c)
    fields += [("i", 0)]                                       # command cache
    fields += [("i", 0), ("i", 0), ("f", 0.0), ("I", 0), ("I", 0x10040)]   # no culling
    fields += [("i", 1 if translucent else 3),                 # depth test (+ write)
               ("I", 0x41), ("I", 0x10107), ("I", 0x3000000), ("I", 0x80126)]
    fields += [("i", 1), ("vec", [0, 0, 0, 1]), ("I", 0xE40100), ("I", 0x803F0100),
               ("I", blend), ("I", 0), ("I", 0xFF000000), ("I", 0)]
    fields += [("I", 0), ("I", 0xD0105), ("I", 0), ("I", 0xF0106)]          # stencil
    fields += [("i", 1)] + coord * 3
    fields += [("ptr", texinfo), ("ptr", None), ("ptr", None), ("ptr", None),
               ("ptr", shader), ("ptr", fshader)] + [("i", 0)] * 5
    # The runtime compares these to skip resending state between materials,
    # so every one is set and differs per material.
    hashes = [zlib.crc32(b"%s/%d" % (name.encode(), i)) or 1 for i in range(13)]
    hashes[3] = sampler_hash
    fields += [("I", h) for h in hashes] + [("x", 4)]
    return Node(fields, "MTOB")


def material_members(name):
    """The MaterialAnimation group's bindable members for one material."""
    out = []

    def member(path, typ, blend, off, size, unk, ft, vidx, fidx):
        out.append((path, Node([("I", typ), ("str", path), ("str", name), ("str", blend),
                                ("i", off), ("i", size), ("i", unk), ("i", ft), ("i", vidx),
                                ("x", 4), ("str", name), ("i", fidx)], "MEMBER")))

    m = 'Materials["%s"].' % name
    for i, c in enumerate(MATERIAL_COLORS):
        member(m + "MaterialColor." + c, 0x8000000, "MaterialColor", 16 * i, 16, 0, 1, i, 1)
    member(m + "TextureMappers[0].Texture", 0x20000000, "TextureMappers[0]", 8, 4, 1, 3, 0, 0)
    member(m + "FragmentOperation.BlendOperation.BlendColor", 0x4000000,
           "FragmentOperation.BlendOperation", 4, 16, 0, 4, 0, 4)
    for vidx, (what, off, size, unk) in enumerate([("Scale", 16, 8, 2), ("Rotate", 24, 4, 3),
                                                   ("Translate", 28, 8, 2)]):
        member(m + "TextureCoordinators[0]." + what, 0x80000000, "TextureCoordinators[0]",
               off, size, unk, 5, vidx, 0)
    return out


def material_color_path(mat, color="Constant0"):
    return 'Materials["%s"].MaterialColor.%s' % (mat, color)


def shape(name, verts, indices, bone):
    """Positions (n, 3) and UVs (n, 2) interleaved, ushort triangle list."""
    v = np.concatenate([verts[0], verts[1]], 1).astype("<f4")
    lo, hi = verts[0].min(0), verts[0].max(0)
    obb = Node([("I", 0x80000000), ("vec", list((lo + hi) / 2)),
                ("vec", [1, 0, 0, 0, 1, 0, 0, 0, 1]), ("vec", list(hi - lo))], "OBB")
    pos = Node([("I", 0x40000001), ("i", 0), ("i", 0), ("i", 0), ("i", 0), ("blob", None),
                ("i", 0), ("i", 0), ("i", 0x1406), ("i", 3), ("f", 1.0), ("i", 0)], "VS")
    uv = Node([("I", 0x40000001), ("i", 4), ("i", 0), ("i", 0), ("i", 0), ("blob", None),
               ("i", 0), ("i", 0), ("i", 0x1406), ("i", 2), ("f", 1.0), ("i", 12)], "VS")
    ivs = Node([("I", 0x40000002), ("i", 21), ("i", 2), ("x", 4), ("i", 0),
                ("blob", Blob(v.tobytes(), 16)), ("i", 0), ("i", 0), ("i", 20),
                ("list", [pos, uv])], "IVS")
    idx = Node([("i", 0x1403), ("b", 0), ("?", True), ("x", 2),
                ("blob", Blob(np.asarray(indices, "<u2").tobytes(), 16))] + [("i", 0)] * 7,
               "INDEX")
    prim = Node([("list", [idx]), ("ilist", [0]), ("i", 0), ("i", 0)], "PRIM")
    pset = Node([("ilist", [bone]), ("i", 0), ("list", [prim])], "PSET")
    return Node([("I", 0x10000001), ("sig", b"SOBJ"), ("i", 0), ("str", name), ("dict", []),
                 ("i", 0), ("ptr", obb), ("vec", [0, 0, 0]), ("list", [pset]), ("i", 0),
                 ("list", [ivs]), ("i", 0)], "SHAPE")


def bone_node(name, flags, joint, parent):
    return Node([("str", name), ("I", flags), ("i", joint), ("i", parent)], "BONE")


def finish_bone(node, parent, child, prev, nxt, billboard, trans=(0.0, 0.0, 0.0),
                world=(0.0, 0.0, 0.0)):
    """Links, transform and matrices. Bones here only translate: the local
    matrix carries `trans`, the world matrix `world` (the sum down from the
    root) and the inverse base matrix its negation."""
    def mtx(t):
        return [1, 0, 0, t[0], 0, 1, 0, t[1], 0, 0, 1, t[2]]
    node.__init__(node.fields + [("ref", parent), ("ref", child), ("ref", prev), ("ref", nxt),
                                 ("vec", [1, 1, 1]), ("vec", [0, 0, 0]), ("vec", list(trans)),
                                 ("vec", mtx(trans)), ("vec", mtx(world)),
                                 ("vec", mtx([-c for c in world])),
                                 ("i", billboard), ("x", 8)], "BONE")


# --------------------------------------------------------------------------
# Animation curves


def hermite(keys, frame):
    """Evaluate Hermite keys [(frame, value, in slope, out slope)] (slopes per
    frame), clamped outside the first and last key."""
    if frame <= keys[0][0]:
        return keys[0][1]
    if frame >= keys[-1][0]:
        return keys[-1][1]
    for (f0, v0, _, s0), (f1, v1, s1, _) in zip(keys, keys[1:]):
        if f0 <= frame <= f1:
            d = f1 - f0
            t = (frame - f0) / d
            t2, t3 = t * t, t * t * t
            return ((2 * t3 - 3 * t2 + 1) * v0 + (t3 - 2 * t2 + t) * s0 * d
                    + (-2 * t3 + 3 * t2) * v1 + (t3 - t2) * s1 * d)
    raise AssertionError("unreachable")


def as_keys(keys):
    """(frame, value, slope) or (frame, value, in, out) -> (frame, value, in, out)."""
    out = []
    for k in keys:
        out.append(tuple(float(v) for v in (k if len(k) == 4 else (k[0], k[1], k[2], k[2]))))
    frames = [k[0] for k in out]
    assert frames == sorted(frames) and len(set(frames)) == len(frames), frames
    return out


def const_keys(value, frames):
    return [(0.0, float(value), 0.0, 0.0), (float(frames), float(value), 0.0, 0.0)]


def curve_node(keys, frames):
    """One curve: a single Hermite128 segment over [0, frames]."""
    keys = as_keys(keys)
    assert len(keys) >= 2 and keys[0][0] == 0 and keys[-1][0] == frames, keys
    fields = [("f", 0.0), ("f", float(frames)), ("B", 0), ("B", 0), ("H", 0),
              ("I", CURVE_FLAGS), ("I", len(keys)), ("f", 1.0 / frames)]
    fields += [("vec", list(k)) for k in keys]
    return Node(fields, "CURVE")


def member_anim(path, prim, curves, frames):
    return (path, Node([("I", 0), ("str", path), ("I", prim)]
                       + [("ptr", curve_node(c, frames)) for c in curves], "MEMBERANIM"))


def canm(target, frames, members):
    return Node([("sig", b"CANM"), ("I", CANM_REVISION), ("str", "COMMON"), ("str", target),
                 ("I", LOOP_REPEAT), ("f", float(frames)), ("dict", members), ("dict", [])],
                "CANM")


# --------------------------------------------------------------------------
# The model


def bone_world(scene, j):
    t = np.zeros(3)
    while j >= 0:
        t += scene["bones"][j]["trans"]
        j = scene["bones"][j]["parent"]
    return t


def build_cgfx(scene, animate=True):
    """scene:
        textures  [(name, rgba uint8 (h, w, 4) top row first, TEX_*)]
        bones     [{name, parent (-1 for the root, which is bone 0), trans
                    (x, y, z) at frame 0, billboard}]
        meshes    [{name, material, texture, mode, const (rgba 0..1 at
                    frame 0), bone, pos (n, 3) bone-local, uv (n, 2), indices}]
        frames    animation length (the loop)
        bone_anims      {bone: {part: keys}}   parts tx ty tz rx ry rz sx sy sz
        material_anims  {material: {part: keys}}   parts r g b a of Constant0
      keys: [(frame, value, slope)], frames 0 .. `frames`. A part without
      keys stays at the bone's / material's frame-0 value."""
    textures = [(name, image_texture(name, rgba, fmt)) for name, rgba, fmt in scene["textures"]]
    tex = dict(textures)

    cmdl = Node([], "CMDL")
    mats, meshes, shapes = [], [], []
    for i, m in enumerate(scene["meshes"]):
        mats.append((m["material"], material(m["material"], tex[m["texture"]], m["mode"],
                                             m.get("const", (1, 1, 1, 1)))))
        shapes.append(shape(m["name"], (m["pos"], m["uv"]), m["indices"], m["bone"]))
        bone_name = scene["bones"][m["bone"]]["name"]         # the mesh node it hangs from
        meshes.append(Node([("I", 0x1000000), ("sig", b"SOBJ"), ("i", 0), ("str", m["name"]),
                            ("dict", []), ("i", i), ("i", i), ("ref", cmdl), ("?", True),
                            ("B", 0), ("H", 65535), ("x", 72), ("str", bone_name)], "MESH"))

    sb = scene["bones"]
    assert sb[0]["parent"] == -1 and all(b["parent"] == 0 for b in sb[1:]), \
        "one root with every other bone under it"
    animated = set(scene.get("bone_anims", {})) if animate else set()
    bones = []
    for j, b in enumerate(sb):
        if j == 0:
            flags = B_IDENTITY | B_RENDER
        else:
            flags = B_RENDER | B_LOCAL | B_WORLD | B_R0 | B_S1 | B_UNIFORM
            if not any(b["trans"]) and b["name"] not in animated:
                flags |= B_T0 | B_IDENTITY
        bones.append(bone_node(b["name"], flags, j, b["parent"]))
    root, kids = bones[0], bones[1:]
    finish_bone(root, None, kids[0], None, None, sb[0]["billboard"])
    for k, b in enumerate(kids):
        finish_bone(b, root, None, kids[k - 1] if k else None,
                    kids[k + 1] if k + 1 < len(kids) else None, sb[k + 1]["billboard"],
                    sb[k + 1]["trans"], bone_world(scene, k + 1))
    skeleton = Node([("I", 0x2000000), ("sig", b"SOBJ"), ("i", 0), ("str", ""), ("dict", []),
                     ("dict", [(b["name"], n) for b, n in zip(sb, bones)]),
                     ("ref", root), ("i", 0), ("i", 2)], "SKELETON")   # flags as the template's

    def group(name, flags, mtype, members, blendops, timing):
        return (name, Node([("I", 0x80000000), ("i", flags), ("str", name), ("i", mtype),
                            ("dict", members), ("ilist", blendops), ("i", timing)], "ANIMGROUP"))

    skel_members = [(b["name"], Node([("I", 0x40000000), ("str", b["name"]), ("str", b["name"]),
                                      ("str", None), ("i", 0), ("i", 0), ("i", 0), ("i", 0),
                                      ("i", 0), ("x", 4), ("str", b["name"]), ("i", 0)],
                                     "MEMBER")) for b in sb]
    vis_members = [("IsVisible", Node([("I", 0x10000000), ("str", "IsVisible"), ("str", None),
                                       ("str", None), ("i", 212), ("i", 1), ("i", 0), ("i", 6),
                                       ("i", 1), ("x", 4), ("i", 0)], "MEMBER"))]
    for i in range(len(meshes)):
        p = "Meshes[%d].IsVisible" % i
        vis_members.append((p, Node([("I", 0x1000000), ("str", p), ("str", str(i)),
                                     ("str", None), ("i", 36), ("i", 1), ("i", 0), ("i", 7),
                                     ("i", 0), ("x", 4), ("i", i)], "MEMBER")))
    mat_members = [mm for name, _ in mats for mm in material_members(name)]
    groups = [group("SkeletalAnimation", 1, 1, skel_members, [8], 1),
              group("VisibilityAnimation", 0, 3, vis_members, [0], 0),
              group("MaterialAnimation", 0, 2, mat_members, [3, 7, 5, 2], 1)]

    cmdl.__init__([("I", 0x40000012 | 0x80), ("sig", b"CMDL"), ("i", 0x7000000),
                   ("str", "COMMON"), ("dict", []), ("i", 1), ("i", 1), ("i", 0), ("x", 4),
                   ("dict", groups), ("vec", [1, 1, 1]), ("vec", [0, 0, 0]), ("vec", [0, 0, 0]),
                   ("vec", IDENTITY34), ("vec", IDENTITY34),
                   ("list", meshes), ("dict", mats), ("list", shapes), ("dict", []),
                   ("i", 1), ("i", 0), ("i", 0), ("ptr", skeleton)], "CMDL")

    skel_anims, mat_anims = [], []
    if animate:
        frames = scene["frames"]
        bnames = [b["name"] for b in sb]
        members = []
        for bone, parts in scene.get("bone_anims", {}).items():
            b = sb[bnames.index(bone)]
            base = dict(zip(TRANSFORM_PARTS, [1, 1, 1, 0, 0, 0] + list(b["trans"])))
            curves = [parts.get(p) or const_keys(base[p], frames) for p in TRANSFORM_PARTS]
            members.append(member_anim(bone, PRIM_TRANSFORM, curves, frames))
        if members:
            skel_anims = [("COMMON", canm("SkeletalAnimation", frames, members))]
        members = []
        mconst = {m["material"]: m.get("const", (1, 1, 1, 1)) for m in scene["meshes"]}
        for mat, parts in scene.get("material_anims", {}).items():
            assert mat in mconst, mat
            base = dict(zip(RGBA_PARTS, mconst[mat]))
            curves = [parts.get(p) or const_keys(base[p], frames) for p in RGBA_PARTS]
            members.append(member_anim(material_color_path(mat), PRIM_RGBA, curves, frames))
        if members:
            mat_anims = [("COMMON", canm("MaterialAnimation", frames, members))]

    # DATA dicts: models, textures, LUTs, materials, shaders, cameras, lights,
    # fogs, environments, skeletal / material / visibility / camera / light
    # animations, emitters
    dicts = [[("COMMON", cmdl)], textures] + [[]] * 7 + [skel_anims, mat_anims] + [[]] * 4
    assert len(dicts) == 15
    data = Node([("sig", b"DATA"), ("i", 0)] + [("dict", d) for d in dicts], "DATA")
    return write_cgfx(data)


# --------------------------------------------------------------------------
# Reader


DATA_DICTS = ["models", "textures", "luts", "materials", "shaders", "cameras", "lights",
              "fogs", "environments", "skeletal_anims", "material_anims",
              "visibility_anims", "camera_anims", "light_anims", "emitters"]


class Reader:
    def __init__(self, data):
        self.d = data
        self.n = len(data)

    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def s32(self, o):
        return struct.unpack_from("<i", self.d, o)[0]

    def f32(self, o, n=1):
        v = struct.unpack_from("<%df" % n, self.d, o)
        return v[0] if n == 1 else list(v)

    def ptr(self, o):
        r = self.s32(o)
        if r == 0:
            return None
        t = o + r
        assert 0 <= t < self.n, "pointer at 0x%x leaves the file (0x%x)" % (o, t)
        return t

    def str(self, o):
        p = self.ptr(o)
        if p is None:
            return None
        e = self.d.index(b"\0", p)
        return self.d[p:e].decode("ascii")

    def sig(self, o, want):
        got = self.d[o:o + 4]
        assert got == want, "expected %r at 0x%x, found %r" % (want, o, got)

    def table(self, o):
        """(count, pointer) -> list of pointed-to offsets."""
        n, p = self.u32(o), self.ptr(o + 4)
        assert (n == 0) == (p is None), "list at 0x%x: %d entries, pointer %r" % (o, n, p)
        return [self.ptr(p + 4 * i) for i in range(n)] if n else []

    def ints(self, o):
        n, p = self.u32(o), self.ptr(o + 4)
        return [self.s32(p + 4 * i) for i in range(n)] if n else []

    def dict(self, o):
        """(count, pointer to DICT) -> [(name, offset)], every name checked to
        be found by NintendoWare's Patricia lookup."""
        n, p = self.u32(o), self.ptr(o + 4)
        if n == 0:
            assert p is None, "empty dict at 0x%x with a pointer" % o
            return []
        self.sig(p, b"DICT")
        assert self.u32(p + 4) == 12 + 16 * (n + 1), "DICT size at 0x%x" % p
        assert self.u32(p + 8) == n, "DICT count at 0x%x" % p
        ents = []
        for i in range(n + 1):
            e = p + 12 + 16 * i
            ref, left, right = struct.unpack_from("<IHH", self.d, e)
            ents.append((ref, left, right, self.str(e + 8), self.ptr(e + 12)))
        assert ents[0][0] == 0xFFFFFFFF and ents[0][3] is None, "DICT root at 0x%x" % p
        for name, _, in [(e[3], e[4]) for e in ents[1:]]:
            assert dict_find(ents, name) == name, "DICT at 0x%x: lookup of %r fails" % (p, name)
        return [(e[3], e[4]) for e in ents[1:]]


def dict_find(ents, name):
    """NW4C ResDic lookup: walk down while the reference bit falls (unsigned)."""
    key = name.encode("ascii")

    def bit(b):
        w = b >> 3
        return (key[w] >> (b & 7)) & 1 if w < len(key) else 0

    cur = ents[0]
    nxt = ents[cur[1]]
    while cur[0] > nxt[0]:
        cur = nxt
        nxt = ents[cur[2]] if bit(cur[0]) else ents[cur[1]]
    return nxt[3]


def parse_curve(r, o):
    start, end = r.f32(o, 2)
    pre, post = r.d[o + 8], r.d[o + 9]
    flags, count = r.u32(o + 12), r.u32(o + 16)
    assert flags == CURVE_FLAGS, "curve at 0x%x: flags 0x%x" % (o, flags)
    inv = r.f32(o + 20)
    keys = [tuple(r.f32(o + 24 + 16 * i, 4)) for i in range(count)]
    return {"start": start, "end": end, "pre": pre, "post": post, "inv": inv, "keys": keys,
            "size": 24 + 16 * count, "off": o}


def parse_canm(r, o):
    r.sig(o, b"CANM")
    a = {"name": r.str(o + 8), "target": r.str(o + 12), "loop": r.u32(o + 16),
         "frames": r.f32(o + 20), "members": {}, "off": o}
    assert not r.dict(o + 32), "CANM user data"
    for path, m in r.dict(o + 24):
        assert r.u32(m) == 0, "member %r flags" % path
        assert r.str(m + 4) == path, "member %r path" % path
        prim = r.u32(m + 8)
        n = {PRIM_TRANSFORM: 9, PRIM_RGBA: 4}[prim]
        curves = [parse_curve(r, r.ptr(m + 12 + 4 * i)) for i in range(n)]
        names = TRANSFORM_PARTS if prim == PRIM_TRANSFORM else RGBA_PARTS
        a["members"][path] = {"prim": prim, "curves": dict(zip(names, curves))}
    return a


def parse_texture(r, o):
    r.sig(o + 4, b"TXOB")
    assert r.u32(o) == 0x20000011
    h, w, glf, glt, fmt = r.u32(o + 0x18), r.u32(o + 0x1C), r.u32(o + 0x20), r.u32(o + 0x24), \
        r.u32(o + 0x34)
    assert (glf, glt) == GL_FORMAT[fmt], "texture format %d with GL %x/%x" % (fmt, glf, glt)
    img = r.ptr(o + 0x38)
    assert (r.u32(img), r.u32(img + 4)) == (h, w)
    size, raw = r.u32(img + 8), r.ptr(img + 12)
    assert size == w * h * 2, "texture %dx%d with %d bytes" % (w, h, size)
    assert raw % 128 == 0, "texel data not 128-aligned"
    return {"name": r.str(o + 0xC), "w": w, "h": h, "fmt": fmt, "raw_off": raw, "size": size,
            "rgba": unswizzle(r.d[raw:raw + size], w, h, fmt)}


def parse_material(r, o):
    r.sig(o + 4, b"MTOB")
    m = {"name": r.str(o + 0xC), "kind": r.u32(o + 0x20),
         "colors": {c: r.f32(o + 0x24 + 16 * i, 4) for i, c in enumerate(MATERIAL_COLORS)},
         "depth": r.u32(o + 0x118), "blend": r.u32(o + 0x148)}
    texi = r.ptr(o + 0x274)
    reftex = r.ptr(texi + 8)
    r.sig(reftex + 4, b"TXOB")
    m["texture"] = r.str(reftex + 0x18)
    m["texture_off"] = r.ptr(reftex + 0x1C)
    fs = r.ptr(o + 0x288)
    st = fs + 0x2C                                   # TEV stage 0
    m["tev_const"] = r.u32(st)
    src = r.u32(st + 4)
    m["tev_src"] = (src & 0xFFF, src >> 16)
    comb = r.u32(st + 16)
    m["tev_comb"] = (comb & 0xFFFF, comb >> 16)
    m["tev_color"] = [(r.u32(st + 20) >> s & 255) / 255 for s in (0, 8, 16, 24)]
    m["alpha_test"] = r.u32(fs + 0x2C + 6 * 28)
    if m["alpha_test"] & 1:
        m["mode"] = "cutout"
    elif m["kind"] == 1:
        m["mode"] = "blend"
    else:
        m["mode"] = "opaque"
    return m


def parse_shape(r, o):
    r.sig(o + 4, b"SOBJ")
    psets = r.table(o + 0x2C)
    assert len(psets) == 1
    bones = r.ints(psets[0])
    prims = r.table(psets[0] + 0xC)
    idx_streams = r.table(prims[0])
    ib = idx_streams[0]
    assert r.u32(ib) == 0x1403, "index format"
    n_idx_bytes, ip = r.u32(ib + 8), r.ptr(ib + 12)
    indices = np.frombuffer(r.d[ip:ip + n_idx_bytes], "<u2").astype(int)
    ivs = r.table(o + 0x38)[0]
    stride = r.u32(ivs + 0x24)
    vbytes, vp = r.u32(ivs + 0x14), r.ptr(ivs + 0x18)
    assert stride == 20 and vbytes % stride == 0
    attrs = {}
    for a in r.table(ivs + 0x28):
        usage, comps, off = r.u32(a + 4), r.u32(a + 0x28), r.u32(a + 0x30)
        assert r.u32(a + 0x24) == 0x1406
        attrs[usage] = (comps, off)
    assert attrs == {0: (3, 0), 4: (2, 12)}, attrs
    v = np.frombuffer(r.d[vp:vp + vbytes], "<f4").reshape(-1, 5)
    assert indices.max() < len(v), "index past the vertex buffer"
    return {"name": r.str(o + 0xC), "bone": bones[0], "pos": v[:, :3].astype(np.float64),
            "uv": v[:, 3:].astype(np.float64), "indices": indices,
            "blobs": [(vp, vbytes), (ip, n_idx_bytes)]}


def parse_bone(r, o):
    return {"name": r.str(o), "flags": r.u32(o + 4), "joint": r.s32(o + 8),
            "parent": r.s32(o + 12), "scale": r.f32(o + 0x20, 3), "rot": r.f32(o + 0x2C, 3),
            "trans": r.f32(o + 0x38, 3), "local": r.f32(o + 0x44, 12),
            "world": r.f32(o + 0x74, 12), "billboard": r.u32(o + 0xD4)}


def parse_model(r, o):
    assert r.u32(o) == 0x40000092, "not a skeletal CMDL"
    r.sig(o + 4, b"CMDL")
    mdl = {"name": r.str(o + 0xC)}
    mdl["groups"] = {name: {"members": [n for n, _ in r.dict(g + 0x10)],
                            "blendops": r.ints(g + 0x18)}
                     for name, g in r.dict(o + 0x28)}
    mdl["materials"] = [parse_material(r, m) for _, m in r.dict(o + 0xBC)]
    mdl["shapes"] = [parse_shape(r, s) for s in r.table(o + 0xC4)]
    mdl["meshes"] = []
    for m in r.table(o + 0xB4):
        r.sig(m + 4, b"SOBJ")
        assert r.ptr(m + 0x20) == o, "mesh owner"
        mdl["meshes"].append({"name": r.str(m + 0xC), "shape": r.u32(m + 0x18),
                              "material": r.u32(m + 0x1C), "visible": r.d[m + 0x24],
                              "bone_name": r.str(m + 0x70)})
    sk = r.ptr(o + 0xE0)
    r.sig(sk + 4, b"SOBJ")
    bones = r.dict(sk + 0x18)
    mdl["bones"] = [parse_bone(r, b) for _, b in bones]
    assert r.ptr(sk + 0x20) == bones[0][1], "skeleton root"
    return mdl


def parse_cgfx(data):
    r = Reader(data)
    magic, bom, hsize, rev, size, nsec = struct.unpack_from("<4sHHIII", data, 0)
    assert (magic, bom, hsize, nsec) == (b"CGFX", 0xFEFF, 0x14, 2), "CGFX header"
    assert size == len(data), "header size %d, file %d" % (size, len(data))
    r.sig(0x14, b"DATA")
    data_size = r.u32(0x18)
    imag = 0x14 + data_size
    r.sig(imag, b"IMAG")
    assert imag + r.u32(imag + 4) == len(data), "IMAG size"
    out = {"size": len(data), "data_end": imag, "imag": imag}
    dicts = {}
    for i, name in enumerate(DATA_DICTS):
        dicts[name] = r.dict(0x1C + 8 * i)
    out["dict_names"] = {k: [n for n, _ in v] for k, v in dicts.items()}
    assert [n for n, _ in dicts["models"]] == ["COMMON"], "one model named COMMON"
    out["model"] = parse_model(r, dicts["models"][0][1])
    out["textures"] = {n: parse_texture(r, t) for n, t in dicts["textures"]}
    out["texture_offs"] = {n: t for n, t in dicts["textures"]}
    out["anims"] = {}
    for kind in ("skeletal_anims", "material_anims"):
        for n, a in dicts[kind]:
            assert n == "COMMON", "animation %r" % n
            out["anims"][kind] = parse_canm(r, a)
    for kind in ("luts", "materials", "shaders", "cameras", "lights", "fogs", "environments",
                 "visibility_anims", "camera_anims", "light_anims", "emitters"):
        assert not dicts[kind], kind
    return out


def check_cgfx(data, scene=None, animate=True):
    """Re-read a CGFX and assert it is consistent (and matches `scene`).
    Returns the parsed file and a list of human-readable findings."""
    assert len(data) < CGFX_LIMIT, "CGFX is 0x%x bytes, the HOME Menu takes < 0x80000" % len(data)
    g = parse_cgfx(data)
    notes = ["%d bytes (limit %d), DATA 0x14..0x%x, IMAG 0x%x.." % (
        len(data), CGFX_LIMIT, g["data_end"], g["imag"])]
    mdl = g["model"]
    assert mdl["name"] == "COMMON"
    # blobs inside IMAG, not overlapping
    spans = []
    for s in mdl["shapes"]:
        spans += s["blobs"]
    for t in g["textures"].values():
        spans.append((t["raw_off"], t["size"]))
    spans.sort()
    for (a, n), (b, _) in zip(spans, spans[1:]):
        assert a + n <= b, "IMAG blobs overlap at 0x%x" % b
    for a, n in spans:
        assert g["imag"] + 8 <= a and a + n <= len(data), "blob 0x%x+0x%x outside IMAG" % (a, n)
    # materials point at the textures in the DATA dict
    for m in mdl["materials"]:
        assert m["texture"] in g["textures"], m["texture"]
        assert m["texture_off"] == g["texture_offs"][m["texture"]], "texture ref of %s" % m["name"]
    # meshes, shapes, bones
    names = [b["name"] for b in mdl["bones"]]
    for i, b in enumerate(mdl["bones"]):
        assert b["joint"] == i and (b["parent"] == -1) == (i == 0), b["name"]
        assert b["local"][3::4] == b["trans"], "local matrix of %s" % b["name"]
    for i, m in enumerate(mdl["meshes"]):
        assert m["shape"] == i and m["material"] == i
        assert m["bone_name"] == names[mdl["shapes"][i]["bone"]]
    # animation members bind to AnimGroup members of the right group
    for kind, group in (("skeletal_anims", "SkeletalAnimation"),
                        ("material_anims", "MaterialAnimation")):
        a = g["anims"].get(kind)
        if a is None:
            continue
        assert a["name"] == "COMMON" and a["target"] == group and a["loop"] == LOOP_REPEAT
        members = mdl["groups"][group]["members"]
        for path, mem in a["members"].items():
            assert path in members, "%s member %r is not in the model's %s group" % (
                kind, path, group)
            for part, c in mem["curves"].items():
                assert c["start"] == 0 and c["end"] == a["frames"], (path, part)
                assert abs(c["inv"] * (c["end"] - c["start"]) - 1) < 1e-6
                fr = [k[0] for k in c["keys"]]
                assert fr[0] == c["start"] and fr[-1] == c["end"] and fr == sorted(fr), fr
        notes.append("%s COMMON -> %s, %g frames, loop: %s" % (
            kind, group, a["frames"], ", ".join(
                "%s [%s]" % (p, " ".join(k for k, c in m["curves"].items() if len(
                    {round(x[1], 6) for x in c["keys"]}) > 1) or "static")
                for p, m in a["members"].items())))
    if not animate:
        assert not g["anims"], "static CGFX with animations"
    if scene is not None:
        assert [m["name"] for m in mdl["meshes"]] == [m["name"] for m in scene["meshes"]]
        assert names == [b["name"] for b in scene["bones"]]
        for sm, mm, sh in zip(scene["meshes"], mdl["materials"], mdl["shapes"]):
            assert mm["name"] == sm["material"] and mm["texture"] == sm["texture"]
            assert mm["mode"] == sm["mode"], (mm["name"], mm["mode"], sm["mode"])
            assert np.allclose(sh["pos"], sm["pos"], atol=1e-4)
            assert np.allclose(sh["uv"], sm["uv"], atol=1e-6)
            assert list(sh["indices"]) == list(sm["indices"])
        for name, rgba, fmt in scene["textures"]:
            t = g["textures"][name]
            assert (t["w"], t["h"], t["fmt"]) == (rgba.shape[1], rgba.shape[0], fmt)
            assert np.array_equal(t["rgba"], quantize(rgba, fmt)), "texels of %s" % name
        if animate:
            sk = g["anims"].get("skeletal_anims", {"members": {}})["members"]
            for bone, parts in scene.get("bone_anims", {}).items():
                for part, keys in parts.items():
                    assert sk[bone]["curves"][part]["keys"] == [
                        tuple(np.float32(v) for v in k) for k in as_keys(keys)], (bone, part)
    notes.append("textures: " + ", ".join("%s %dx%d %s" % (n, t["w"], t["h"], TEX_NAMES[t["fmt"]])
                                          for n, t in g["textures"].items()))
    notes.append("meshes: " + ", ".join("%s(%s on %s)" % (m["name"], mm["mode"], m["bone_name"])
                                        for m, mm in zip(mdl["meshes"], mdl["materials"])))
    return g, notes


# --------------------------------------------------------------------------
# Preview renderer (what the parsed file draws through the banner camera)


def pose(g, frame):
    """Per-bone world translation and per-material Constant0 at `frame`."""
    mdl = g["model"]
    sk = g["anims"].get("skeletal_anims")
    ma = g["anims"].get("material_anims")
    local = []
    for b in mdl["bones"]:
        t = list(b["trans"])
        if sk and b["name"] in sk["members"]:
            c = sk["members"][b["name"]]["curves"]
            t = [hermite(c[p]["keys"], frame) for p in ("tx", "ty", "tz")]
            for p, v in (("rx", 0), ("ry", 0), ("rz", 0), ("sx", 1), ("sy", 1), ("sz", 1)):
                assert all(abs(k[1] - v) < 1e-6 for k in c[p]["keys"]), \
                    "the preview draws translation-only bone animation"
        local.append(np.array(t))
    world = []
    for i, b in enumerate(mdl["bones"]):
        world.append(local[i] + (world[b["parent"]] if b["parent"] >= 0 else 0))
    consts = {}
    for m in mdl["materials"]:
        c = list(m["colors"]["Constant0"])
        if ma:
            mem = ma["members"].get(material_color_path(m["name"]))
            if mem:
                c = [hermite(mem["curves"][p]["keys"], frame) for p in RGBA_PARTS]
        consts[m["name"]] = np.clip(c, 0, 1)
    return world, consts


def _sample(tex, u, v):
    """Bilinear, wrapping (the samplers repeat), v = 1 at the top row."""
    h, w = tex.shape[:2]
    x = u * w - 0.5
    y = (1 - v) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    t = tex.astype(np.float64)

    def at(yy, xx):
        return t[yy % h, xx % w]
    return (at(y0, x0) * (1 - fx) * (1 - fy) + at(y0, x0 + 1) * fx * (1 - fy)
            + at(y0 + 1, x0) * (1 - fx) * fy + at(y0 + 1, x0 + 1) * fx * fy)


def render(g, frame, width=400, height=240, ss=2, background=(0, 0, 0)):
    """Draw the parsed model at `frame` as the HOME Menu camera sees it (no
    billboard turning: the camera looks straight down -z, where YAxial
    billboards leave the bones as they are). Returns RGB uint8."""
    W, H = width * ss, height * ss
    focal = (H / 2) / math.tan(math.radians(CAMERA_FOV / 2))
    color = np.zeros((H, W, 3)) + np.array(background, float)
    depth = np.full((H, W), -np.inf)
    mdl = g["model"]
    world, consts = pose(g, frame)
    draws = []
    for i, mesh in enumerate(mdl["meshes"]):
        sh, mat = mdl["shapes"][mesh["shape"]], mdl["materials"][mesh["material"]]
        p = sh["pos"] + world[sh["bone"]]
        draws.append((mat["mode"] == "blend", float(p[:, 2].mean()), i, p, sh, mat))
    draws.sort(key=lambda d: (d[0], d[1] if d[0] else 0, d[2]))   # opaque, then back to front
    ys, xs = np.mgrid[0:H, 0:W]
    for translucent, _, _, p, sh, mat in draws:
        tex = g["textures"][mat["texture"]]["rgba"]
        dz = CAMERA_Z - p[:, 2]
        sx = W / 2 + p[:, 0] / dz * focal
        sy = H / 2 - (p[:, 1] - CAMERA_Y) / dz * focal
        const = consts[mat["name"]]
        for a, b, c in sh["indices"].reshape(-1, 3):
            x0, x1 = int(max(0, np.floor(min(sx[[a, b, c]])))), int(min(W, np.ceil(max(sx[[a, b, c]])) + 1))
            y0, y1 = int(max(0, np.floor(min(sy[[a, b, c]])))), int(min(H, np.ceil(max(sy[[a, b, c]])) + 1))
            if x0 >= x1 or y0 >= y1:
                continue
            px, py = xs[y0:y1, x0:x1] + 0.5, ys[y0:y1, x0:x1] + 0.5
            d = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a])
            if abs(d) < 1e-12:
                continue
            l1 = ((px - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (py - sy[a])) / d
            l2 = ((sx[b] - sx[a]) * (py - sy[a]) - (px - sx[a]) * (sy[b] - sy[a])) / d
            l0 = 1 - l1 - l2
            inside = (l0 >= -1e-9) & (l1 >= -1e-9) & (l2 >= -1e-9)
            if not inside.any():
                continue
            # perspective-correct attributes (1/w interpolation)
            iw = l0 / dz[a] + l1 / dz[b] + l2 / dz[c]
            uv = (l0[..., None] * sh["uv"][a] / dz[a] + l1[..., None] * sh["uv"][b] / dz[b]
                  + l2[..., None] * sh["uv"][c] / dz[c]) / iw[..., None]
            z = (l0 * p[a, 2] / dz[a] + l1 * p[b, 2] / dz[b] + l2 * p[c, 2] / dz[c]) / iw
            yy, xx = np.nonzero(inside)
            texel = _sample(tex, uv[yy, xx, 0], uv[yy, xx, 1]) / 255.0
            zz = z[yy, xx]
            gy, gx = yy + y0, xx + x0
            ok = zz >= depth[gy, gx] - 1e-6                     # depth test
            alpha = texel[:, 3]
            if mat["mode"] == "cutout":
                ok &= alpha >= 0.5
            if mat["mode"] == "blend":
                alpha = alpha * const[3]
                gy, gx, alpha, texel = gy[ok], gx[ok], alpha[ok], texel[ok]
                color[gy, gx] = color[gy, gx] * (1 - alpha[:, None]) + texel[:, :3] * 255 * alpha[:, None]
            else:
                gy, gx, texel, zz = gy[ok], gx[ok], texel[ok], zz[ok]
                color[gy, gx] = texel[:, :3] * 255
                depth[gy, gx] = zz
    img = color.reshape(height, ss, width, ss, 3).mean((1, 3))
    return np.clip(np.round(img), 0, 255).astype(np.uint8)
