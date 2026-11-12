"""Wii NW4R layout (BRLYT) and animation (BRLAN) readers, enough for channel banners.

parse_brlyt(data) -> dict(width, height, textures, materials, panes)
  pane: dict(kind 'pan1'|'pic1'|'txt1'|'wnd1'|'bnd1', name, parent, children, visible, alpha,
             origin, trans(x,y,z), rot(x,y,z), scale(x,y), size(w,h), material, uv)
  material: dict(name, texture indices)
parse_brlan(data) -> dict(frames, loop, panes={name: [(tag, target, [(frame, value, slope)])]})
  tag: 'RLPA' pane SRT (target 0-9: tx ty tz rx ry rz sx sy w h), 'RLVC' vertex colour/alpha
       (16 = pane alpha), 'RLTS' texture SRT, 'RLMC' material colour, 'RLTP' texture pattern,
       'RLVI' visibility.
Run as a script on a .brlyt/.brlan to print it.
"""
import struct
import sys


def _str(b):
    return b.split(b"\0", 1)[0].decode("latin1")


def _sections(data, magic):
    if data[:4] != magic:
        raise ValueError(f"not {magic!r}")
    hsize, n = struct.unpack(">HH", data[12:16])
    p = hsize
    for _ in range(n):
        tag, size = struct.unpack(">4sI", data[p:p + 8])
        yield tag.decode(), p, data[p:p + size]
        p += size


def parse_brlyt(data):
    lay = {"textures": [], "materials": [], "panes": []}
    stack, last = [], None
    for tag, _, s in _sections(data, b"RLYT"):
        if tag == "lyt1":
            lay["width"], lay["height"] = struct.unpack(">ff", s[12:20])
        elif tag == "txl1":
            n = struct.unpack(">H", s[8:10])[0]
            base = 12
            for i in range(n):
                off = struct.unpack(">I", s[base + i * 8:base + i * 8 + 4])[0]
                lay["textures"].append(_str(s[base + off:]))
        elif tag == "mat1":
            n = struct.unpack(">H", s[8:10])[0]
            for i in range(n):
                off = struct.unpack(">I", s[12 + i * 4:16 + i * 4])[0]
                m = s[off:]
                flags = struct.unpack(">I", m[0x3C:0x40])[0]
                ntex = flags & 15
                texs = [struct.unpack(">H", m[0x40 + j * 4:0x42 + j * 4])[0] for j in range(ntex)]
                lay["materials"].append({"name": _str(m[:20]), "textures": texs,
                                         "color": struct.unpack(">4h", m[0x14:0x1C]),
                                         "tev": [struct.unpack(">4B", m[0x2C + j * 4:0x30 + j * 4]) for j in range(4)]})
        elif tag in ("pan1", "pic1", "txt1", "wnd1", "bnd1"):
            flags, origin, alpha = s[8], s[9], s[10]
            name = _str(s[12:28])
            tx, ty, tz, rx, ry, rz, sx, sy, w, h = struct.unpack(">10f", s[0x24:0x4C])
            pane = {"kind": tag, "name": name, "visible": bool(flags & 1), "alpha": alpha,
                    "origin": origin, "trans": (tx, ty, tz), "rot": (rx, ry, rz),
                    "scale": (sx, sy), "size": (w, h), "children": [],
                    "parent": stack[-1]["name"] if stack else None}
            if tag == "pic1":
                pane["vtx_colors"] = [struct.unpack(">4B", s[0x4C + i * 4:0x50 + i * 4]) for i in range(4)]
                pane["material"], nuv = struct.unpack(">HB", s[0x5C:0x5F])
                pane["uv"] = [struct.unpack(">8f", s[0x60 + i * 32:0x80 + i * 32]) for i in range(nuv)]
            if stack:
                stack[-1]["children"].append(name)
            lay["panes"].append(pane)
            last = pane
        elif tag == "pas1":
            stack.append(last)
        elif tag == "pae1":
            stack.pop()
    return lay


def parse_brlan(data):
    anim = {"panes": {}}
    for tag, _, s in _sections(data, b"RLAN"):
        if tag != "pai1":
            continue
        frames, loop, _, ntex, nentries, eoff = struct.unpack(">HBBHHI", s[8:20])
        anim["frames"], anim["loop"] = frames, bool(loop)
        for i in range(nentries):
            po = struct.unpack(">I", s[eoff + i * 4:eoff + i * 4 + 4])[0]
            e = s[po:]
            name = _str(e[:20])
            ntags, is_mat = e[20], e[21]
            tracks = []
            for t in range(ntags):
                to = struct.unpack(">I", e[24 + t * 4:28 + t * 4])[0]
                tg = e[to:]
                ttype = tg[:4].decode()
                nent = tg[4]
                for k in range(nent):
                    ko = struct.unpack(">I", tg[8 + k * 4:12 + k * 4])[0]
                    kk = tg[ko:]
                    idx, target, dtype, _, nkeys, _, koff = struct.unpack(">BBBBHHI", kk[:12])
                    keys = []
                    for j in range(nkeys):
                        if dtype == 2:  # hermite: frame, value, slope
                            keys.append(struct.unpack(">fff", kk[koff + j * 12:koff + j * 12 + 12]))
                        else:  # step: frame, u16 value, pad
                            f_, v = struct.unpack(">fH", kk[koff + j * 8:koff + j * 8 + 6])
                            keys.append((f_, float(v), 0.0))
                    tracks.append((ttype, target if ttype != "RLTS" else idx * 10 + target, keys))
            anim["panes"][name] = tracks
    return anim


if __name__ == "__main__":
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        if d[:4] == b"RLYT":
            lay = parse_brlyt(d)
            print(path, "size", lay.get("width"), lay.get("height"), "textures", lay["textures"])
            for i, m in enumerate(lay["materials"]):
                print("  mat", i, m["name"], "tex", m["textures"])
            for p in lay["panes"]:
                extra = f" mat={p['material']}" if "material" in p else ""
                print(f"  {p['kind']} {p['name']:<16} parent={p['parent']} vis={p['visible']} a={p['alpha']}"
                      f" t={tuple(round(v, 1) for v in p['trans'])} r={tuple(round(v, 1) for v in p['rot'])}"
                      f" s={p['scale']} size={p['size']}{extra}")
        else:
            an = parse_brlan(d)
            print(path, "frames", an["frames"], "loop", an["loop"])
            for name, tracks in an["panes"].items():
                for ttype, target, keys in tracks:
                    print(f"  {name:<16} {ttype} {target}: " + " ".join(f"{k[0]:.0f}:{k[1]:.2f}" for k in keys))
