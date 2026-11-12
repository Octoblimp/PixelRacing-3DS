"""Minimal pure-Python PNG reader/writer with Apple CgBI ("crushed" iOS PNG) support.

CgBI files (every PNG in the iOS 3D Pixel Racing bundle) differ from standard PNG:
  * a 'CgBI' chunk comes before IHDR,
  * IDAT is raw deflate (no zlib header/Adler-32),
  * pixels are BGRA with the colour premultiplied by alpha.
read_png() undoes all three and returns straight-alpha RGBA.
"""
import struct
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter(raw, w, h, bpp):
    stride = w * bpp
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                ul = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, prev[i], ul)) & 255
        elif ft != 0:
            raise ValueError("bad PNG filter %d" % ft)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def read_png(data):
    """Return (width, height, rgba bytearray, was_cgbi)."""
    if data[:8] != SIG:
        raise ValueError("not a PNG")
    p = 8
    cgbi = False
    idat = bytearray()
    w = h = bd = ct = None
    plte = trns = None
    while p < len(data):
        n, t = struct.unpack(">I4s", data[p:p + 8])
        body = data[p + 8:p + 8 + n]
        p += 12 + n
        if t == b"CgBI":
            cgbi = True
        elif t == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            if bd != 8 or il:
                raise ValueError("only 8-bit non-interlaced PNGs are handled")
        elif t == b"PLTE":
            plte = body
        elif t == b"tRNS":
            trns = body
        elif t == b"IDAT":
            idat += body
        elif t == b"IEND":
            break
    raw = zlib.decompress(bytes(idat), -15 if cgbi else 15)
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    px = _unfilter(raw, w, h, bpp)
    rgba = bytearray(w * h * 4)
    if ct == 6:
        rgba[:] = px
    elif ct == 2:
        rgba[0::4], rgba[1::4], rgba[2::4] = px[0::3], px[1::3], px[2::3]
        rgba[3::4] = b"\xff" * (w * h)
    elif ct == 0:
        rgba[0::4] = rgba[1::4] = rgba[2::4] = px
        rgba[3::4] = b"\xff" * (w * h)
    elif ct == 4:
        rgba[0::4] = rgba[1::4] = rgba[2::4] = px[0::2]
        rgba[3::4] = px[1::2]
    elif ct == 3:
        for i, idx in enumerate(px):
            rgba[i * 4:i * 4 + 3] = plte[idx * 3:idx * 3 + 3]
            rgba[i * 4 + 3] = trns[idx] if trns and idx < len(trns) else 255
    if cgbi:
        if ct in (2, 6):  # BGR(A) -> RGB(A)
            rgba[0::4], rgba[2::4] = rgba[2::4], rgba[0::4]
        if ct == 6:  # un-premultiply
            for i in range(0, len(rgba), 4):
                a = rgba[i + 3]
                if 0 < a < 255:
                    rgba[i] = min(255, (rgba[i] * 255 + a // 2) // a)
                    rgba[i + 1] = min(255, (rgba[i + 1] * 255 + a // 2) // a)
                    rgba[i + 2] = min(255, (rgba[i + 2] * 255 + a // 2) // a)
    return w, h, rgba, cgbi


def write_png(path, w, h, rgba):
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    stride = w * 4
    raw = b"".join(b"\0" + bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h))
    with open(path, "wb") as f:
        f.write(SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pad_pot(w, h, rgba):
    """Pad to power-of-two (min 8) by extending into transparent black; returns (pw, ph, rgba)."""
    def pot(n):
        p = 8
        while p < n:
            p <<= 1
        return p
    pw, ph = pot(w), pot(h)
    if (pw, ph) == (w, h):
        return w, h, rgba
    out = bytearray(pw * ph * 4)
    for y in range(h):
        out[y * pw * 4:y * pw * 4 + w * 4] = rgba[y * w * 4:(y + 1) * w * 4]
    return pw, ph, out
