"""Resolve the addresses Ghidra's iOS pseudo-C refers to by name, for hand translation.

Reads decomp/raw/c/*.c and the thinned armv7 slice and writes decomp/raw/consts.txt (untracked,
derived from the binary):
  TEXT  addr  u32  float      -- literal-pool / __const words (DAT_000xxxxx below __DATA)
  PTR   addr  -> target       -- __nl_symbol_ptr / __data pointer slots (PTR_DAT_..., PTR_s_...)
  STR   addr  "text"          -- __cstring entries referenced by address
Usage: python tools/gen_consts.py
"""
import os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'bin', '3d_pixel_racing.armv7')
RAW = os.path.join(ROOT, 'decomp', 'raw')
TEXT_END, DATA_START, DATA_END = 0x3d000, 0x3d000, 0x8c000
FILE_DATA = 0x3c000  # file offset of __DATA (va 0x3d000)

def main():
    b = open(BIN, 'rb').read()
    def word(a):
        off = a - 0x1000 if a < TEXT_END else a - DATA_START + FILE_DATA
        return struct.unpack_from('<I', b, off)[0] if 0 <= off < len(b) - 3 else None
    def cstr(a):
        off = a - 0x1000
        e = b.index(b'\0', off)
        return b[off:e].decode('latin-1')
    names = set()
    for f in os.listdir(os.path.join(RAW, 'c')):
        names |= set(re.findall(r'\b(?:PTR_[A-Za-z0-9_]*?_|DAT_|[a-z]{1,3}Ram)([0-9a-f]{8})\b',
                                open(os.path.join(RAW, 'c', f), encoding='latin-1').read()))
    out = []
    for h in sorted(names):
        a = int(h, 16)
        w = word(a)
        if a < TEXT_END:
            if w is None: continue
            fl = struct.unpack('<f', struct.pack('<I', w))[0]
            out.append('TEXT %08x %08x %g' % (a, w, fl))
        elif a < 0x3e870 or 0x3e870 <= a < 0x3f3f0:  # pointer sections + __data
            if w is None: continue
            tgt = '%08x' % w
            if 0x3b03a <= w < 0x3c9dd: tgt += ' "%s"' % cstr(w)
            out.append(('PTR  %08x -> ' % a) + tgt)
    open(os.path.join(RAW, 'consts.txt'), 'w').write('\n'.join(out) + '\n')
    print(len(out), 'entries')

if __name__ == '__main__':
    main()
