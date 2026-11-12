#!/usr/bin/env python3
"""Remove hidden coplanar overlaps from the game's .bin models (docs/formats/models.md).

The voxel models contain triangles that lie exactly on top of earlier triangles: same plane,
same facing (e.g. a hair quad over a skin quad, or a car's access part over its body). The game
draws with GL_LESS, so wherever two such triangles overlap the one drawn first wins and the
later one never shows. Desktop GPUs (and the Azahar emulator) compute equal depths for equal
planes, so the result is stable. The 3DS GPU computes depth per vertex in 24-bit floats and
interpolates it per triangle, so two different triangles in one plane get slightly different
depths at the same pixel: the hidden one shows through in patches that change every frame.

This tool cuts every later triangle down to the part no earlier coplanar, same-facing triangle
covers, which is exactly what GL_LESS shows on hardware with exact depth. Attributes of the
pieces (position, normal, uv) are interpolated from the original triangle, so the visible image
is unchanged.

  python -I tools/smd_dedupe.py [--check] FILE.bin ...      rewrite files in place
  dedupe(data, earlier=[...]) -> data                       library use (tools/build_romfs.py)

`earlier` lists models drawn before this one with the same transform (a car's body before its
access part and windows); their triangles hide this model's coplanar triangles as well.
"""
import struct
import sys
from collections import defaultdict

HEADER = 0x20
VSIZE = 0x24
EPS_AREA = 1e-4


def parse(data):
    if data[:3] != b"SMD":
        raise ValueError("not an SMD .bin")
    nodes, count = struct.unpack_from("<ii", data, 0x18)
    off = HEADER + nodes * 0x40
    verts = []
    for i in range(count):
        o = off + i * VSIZE
        bone = struct.unpack_from("<i", data, o)[0]
        f = struct.unpack_from("<8f", data, o + 4)
        verts.append((bone, f))
    tris = [verts[i:i + 3] for i in range(0, count - count % 3, 3)]
    return data[:off], tris


def build(head, tris):
    out = bytearray(head)
    struct.pack_into("<i", out, 0x1C, len(tris) * 3)
    for tri in tris:
        for bone, f in tri:
            out += struct.pack("<i8f", bone, *f)
    return bytes(out)


def plane_key(tri):
    """(axis, coordinate, facing) for an axis-aligned triangle, else None."""
    ps = [f[:3] for _, f in tri]
    for ax in range(3):
        c = ps[0][ax]
        if all(abs(p[ax] - c) < 1e-4 for p in ps):
            # facing from the geometric normal along the axis (vertex normals agree in the data)
            n = sum(f[3 + ax] for _, f in tri)
            if abs(n) < 1e-6:
                return None
            return (ax, round(c, 3), 1 if n > 0 else -1)
    return None


def area2(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly)))


def clip(poly, a, b, keep_left):
    """Clip a convex polygon to one side of the line a->b."""
    out = []
    n = len(poly)
    for i in range(n):
        p, q = poly[i], poly[(i + 1) % n]
        sp = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
        sq = (b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])
        if not keep_left:
            sp, sq = -sp, -sq
        if sp >= 0:
            out.append(p)
        if (sp >= 0) != (sq >= 0):
            t = sp / (sp - sq)
            out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    return out


def subtract(pieces, cut):
    """pieces (convex polygons, CCW) minus convex polygon cut (CCW) -> convex polygons."""
    out = []
    m = len(cut)
    for poly in pieces:
        rest = poly
        for i in range(m):
            if len(rest) < 3:
                break
            a, b = cut[i], cut[(i + 1) % m]
            outside = clip(rest, a, b, keep_left=False)
            if len(outside) >= 3 and area2(outside) > EPS_AREA:
                out.append(outside)
            rest = clip(rest, a, b, keep_left=True)
        # whatever is left of `rest` lies inside cut: dropped
    return out


def project(tri, ax):
    o = [i for i in range(3) if i != ax]
    pts = [(f[o[0]], f[o[1]]) for _, f in tri]
    return pts, o


def ccw(pts):
    return pts if area2(pts) > 0 else pts[::-1]


def bbox(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), max(xs), min(ys), max(ys)


def bbox_hit(a, b):
    return a[0] < b[1] - 1e-6 and b[0] < a[1] - 1e-6 and a[2] < b[3] - 1e-6 and b[2] < a[3] - 1e-6


def rebuild(tri, ax, o, piece):
    """Fan-triangulate a convex 2D piece of `tri`, interpolating every attribute."""
    (p0, p1, p2) = [(f[o[0]], f[o[1]]) for _, f in tri]
    det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
    bone = tri[0][0]
    fs = [f for _, f in tri]

    def vert(q):
        l1 = ((q[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (q[1] - p0[1])) / det
        l2 = ((p1[0] - p0[0]) * (q[1] - p0[1]) - (q[0] - p0[0]) * (p1[1] - p0[1])) / det
        l0 = 1.0 - l1 - l2
        f = [l0 * fs[0][k] + l1 * fs[1][k] + l2 * fs[2][k] for k in range(8)]
        f[o[0]], f[o[1]] = q[0], q[1]
        f[ax] = fs[0][ax]
        return (bone, tuple(f))

    # keep the original winding: the piece is CCW in the projection; flip if the source was CW
    pts = piece if det > 0 else piece[::-1]
    out = []
    for i in range(1, len(pts) - 1):
        t = [pts[0], pts[i], pts[i + 1]]
        if abs(area2(t)) > EPS_AREA:
            out.append([vert(q) for q in t])
    return out


def dedupe(data, earlier=()):
    """Return (new data, triangles before, triangles after)."""
    head, tris = parse(data)
    covers = defaultdict(list)  # plane -> [(bbox, ccw polygon)] of triangles already drawn
    for e in earlier:
        for tri in parse(e)[1]:
            k = plane_key(tri)
            if k:
                pts = ccw(project(tri, k[0])[0])
                covers[k].append((bbox(pts), pts))
    out = []
    for tri in tris:
        k = plane_key(tri)
        if not k:
            out.append(tri)
            continue
        pts, o = project(tri, k[0])
        if abs(area2(pts)) <= EPS_AREA:
            out.append(tri)
            continue
        tp = ccw(pts)
        bb = bbox(tp)
        pieces = [tp]
        cut_any = False
        for cbb, cut in covers[k]:
            if not bbox_hit(bb, cbb):
                continue
            new = subtract(pieces, cut)
            if new != pieces:
                cut_any = True
            pieces = new
            if not pieces:
                break
        if not cut_any:
            out.append(tri)
        else:
            for piece in pieces:
                out.extend(rebuild(tri, k[0], o, piece))
        covers[k].append((bb, tp))
    return build(head, out), len(tris), len(out)


def main(argv):
    check = "--check" in argv
    files = [a for a in argv if a != "--check"]
    for path in files:
        data = open(path, "rb").read()
        new, before, after = dedupe(data)
        print(f"{path}: {before} -> {after} triangles")
        if not check and new != data:
            open(path, "wb").write(new)


if __name__ == "__main__":
    main(sys.argv[1:])
