"""Wii TPL texture decoder (formats 0-6: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8).

decode_tpl(data) -> [(width, height, rgba bytearray)]   (straight alpha, row-major)
Run as a script to dump every image of the given TPLs to PNG next to them.
"""
import os
import struct
import sys

# format: (block width, block height, bits per pixel)
BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32)}


def _rgb5a3(v):
    if v & 0x8000:
        r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255)
    a, r, g, b = (v >> 12) & 7, (v >> 8) & 15, (v >> 4) & 15, v & 15
    return (r * 17, g * 17, b * 17, a << 5 | a << 2 | a >> 1)


def decode_image(fmt, w, h, d):
    bw, bh, bpp = BLOCK[fmt]
    out = bytearray(w * h * 4)
    p = 0

    def put(x, y, c):
        if x < w and y < h:
            i = (y * w + x) * 4
            out[i:i + 4] = bytes(c)

    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 6:  # RGBA8: AR block then GB block
                ar, gb = d[p:p + 32], d[p + 32:p + 64]
                p += 64
                for i in range(16):
                    put(bx + i % 4, by + i // 4, (ar[i * 2 + 1], gb[i * 2], gb[i * 2 + 1], ar[i * 2]))
                continue
            for y in range(bh):
                for x in range(bw):
                    if fmt == 0:
                        v = d[p + (y * bw + x) // 2]
                        v = (v >> 4) if x % 2 == 0 else (v & 15)
                        c = (v * 17,) * 3 + (255,)
                    elif fmt == 1:
                        v = d[p + y * bw + x]
                        c = (v, v, v, 255)
                    elif fmt == 2:
                        v = d[p + y * bw + x]
                        i_, a = (v & 15) * 17, (v >> 4) * 17
                        c = (i_, i_, i_, a)
                    else:
                        v = struct.unpack_from(">H", d, p + (y * bw + x) * 2)[0]
                        if fmt == 3:
                            c = (v & 255,) * 3 + (v >> 8,)
                        elif fmt == 4:
                            r, g, b = v >> 11, (v >> 5) & 63, v & 31
                            c = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)
                        else:
                            c = _rgb5a3(v)
                    put(bx + x, by + y, c)
            p += bw * bh * bpp // 8
    return out


def decode_tpl(data):
    if data[:4] != b"\x00\x20\xaf\x30":
        raise ValueError("not a TPL")
    n, toff = struct.unpack(">II", data[4:12])
    images = []
    for i in range(n):
        io, _ = struct.unpack(">II", data[toff + i * 8:toff + i * 8 + 8])
        h, w, fmt, doff = struct.unpack(">HHII", data[io:io + 12])
        if fmt not in BLOCK:
            raise ValueError(f"TPL format {fmt} not supported")
        images.append((w, h, decode_image(fmt, w, h, data[doff:])))
    return images


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import pngio
    for path in sys.argv[1:]:
        for i, (w, h, px) in enumerate(decode_tpl(open(path, "rb").read())):
            pngio.write_png(os.path.splitext(path)[0] + (f"_{i}" if i else "") + ".png", w, h, px)
            print(path, w, h)
