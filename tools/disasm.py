#!/usr/bin/env python3
"""Per-function Thumb-2 disassembly of the iOS armv7 slice + softfp signature inference.

Writes decomp/raw/asm/<addr>.s (literal-pool loads and PC-relative address formation resolved
in comments) and decomp/signatures.tsv:

    addr  name  nparams  float_params  float_return  callers

Signature heuristics (softfp: floats travel in r0-r3 / r0):
  * a parameter register rN is "float" if its first read in the function is a VMOV into a VFP/
    NEON register (vmov sX, rN / vmov dX, rN, rM) and it is not read as an integer before that;
  * a function returns float if a VMOV r0, sX (or vmov r0, r1, dX) is the last write to r0 before
    a return, and the caller side agrees: call sites read r0 with vmov after the bl;
  * nparams = highest of r0-r3 read before being written (stack params flagged as '+').
These are hints for the translators, not ground truth -- check against the Wii decomp.

Usage: python tools/disasm.py
"""
import csv
import os
import re
import struct

import capstone
from capstone import arm as carm

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build", "bin", "3d_pixel_racing.armv7")


def load_sections(d):
    ncmds = struct.unpack_from("<I", d, 16)[0]
    off, secs = 28, {}
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", d, off)
        if cmd == 1:
            nsects = struct.unpack_from("<I", d, off + 48)[0]
            for j in range(nsects):
                s = off + 56 + j * 68
                sn = d[s:s + 16].rstrip(b"\0").decode()
                addr, size, foff = struct.unpack_from("<III", d, s + 32)
                secs[sn] = (addr, size, foff)
                RESERVED1[sn] = struct.unpack_from("<I", d, s + 60)[0]
        elif cmd == 2:  # LC_SYMTAB
            SYMTAB[:] = struct.unpack_from("<IIII", d, off + 8)
        elif cmd == 0xB:  # LC_DYSYMTAB
            DYSYM[:] = struct.unpack_from("<II", d, off + 8 + 4 * 12)
        off += sz
    return secs


RESERVED1, SYMTAB, DYSYM = {}, [], []


def import_names(d, secs):
    """address -> symbol for __symbolstub1 stubs and __nl_symbol_ptr / __lazy_symbol slots."""
    symoff, nsyms, stroff, _ = SYMTAB
    indoff, nind = DYSYM
    out = {}
    for sec, esz in (("__symbolstub1", 4), ("__nl_symbol_ptr", 4), ("__lazy_symbol", 4)):
        if sec not in secs:
            continue
        addr, size, _ = secs[sec]
        first = RESERVED1[sec]
        for i in range(size // esz):
            idx = struct.unpack_from("<I", d, indoff + 4 * (first + i))[0]
            if idx & 0xC0000000:
                continue
            strx = struct.unpack_from("<I", d, symoff + 12 * idx)[0]
            o = stroff + strx
            out[addr + i * esz] = d[o:d.index(bytes(1), o)].decode()
    return out


def main():
    d = open(BIN, "rb").read()
    secs = load_sections(d)
    taddr, tsize, toff = secs["__text"]
    cs_addr, cs_size, cs_off = secs["__cstring"]

    def rd32(a):
        for name, (sa, ss, so) in secs.items():
            if sa <= a < sa + ss and so:
                return struct.unpack_from("<I", d, so + a - sa)[0]
        return None

    def cstr(a):
        if cs_addr <= a < cs_addr + cs_size:
            o = cs_off + a - cs_addr
            return d[o:d.index(b"\0", o)].decode("latin1")
        return None

    # stub names: __symbolstub1 entries map to the imports via Ghidra's names in functions.tsv
    stub_names = import_names(d, secs)
    ftsv = os.path.join(ROOT, "decomp", "raw", "functions.tsv")
    for line in open(ftsv, encoding="utf-8"):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 2 and re.fullmatch(r"[0-9a-fA-F]{8}", p[0]):
            stub_names[int(p[0], 16)] = p[1]

    rows = list(csv.DictReader(open(os.path.join(ROOT, "decomp", "symbols.csv"))))
    addrs = sorted(int(r["addr"], 16) for r in rows)
    names = {int(r["addr"], 16): r["name"] for r in rows}
    ends = {a: (addrs[i + 1] if i + 1 < len(addrs) else taddr + tsize) for i, a in enumerate(addrs)}

    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    md.detail = True
    outdir = os.path.join(ROOT, "decomp", "raw", "asm")
    os.makedirs(outdir, exist_ok=True)

    sig = {}
    callsites = {}  # callee -> list of (caller, float_r0_after)
    for a in addrs:
        end = ends[a]
        code = d[toff + a - taddr: toff + end - taddr]
        lines = []
        insns = []
        off = 0
        while off < len(code):
            got = False
            for ins in md.disasm(code[off:], a + off):
                insns.append(ins)
                off = ins.address + ins.size - a
                got = True
            if off < len(code):  # data in code (literal pool / jump table): skip a halfword
                lines.append(f"{a + off:08x}  .hword 0x{struct.unpack_from('<H', code, off)[0]:04x}")
                off += 2
            if not got and off >= len(code):
                break
        # analysis
        written, read_first = set(), {}
        float_param = set()
        last_r0_float = False
        movpend = {}
        returns_float_votes = []
        for idx, ins in enumerate(insns):
            regs_read, regs_write = [], []
            try:
                rr, ww = ins.regs_access()
                regs_read = [ins.reg_name(r) for r in rr]
                regs_write = [ins.reg_name(r) for r in ww]
            except capstone.CsError:
                pass
            m = ins.mnemonic
            for r in regs_read:
                if r in ("r0", "r1", "r2", "r3") and r not in written and r not in read_first:
                    isf = m.startswith("vmov") and ins.op_str.split(",")[0].strip()[0] in "sd"
                    if isf:  # vmov sX, rN followed by vcvt.f32.[us]32 of sX/dX is an int param
                        dst = ins.op_str.split(",")[0].strip()
                        dreg = dst if dst[0] == "d" else "d%d" % (int(dst[1:]) // 2)
                        for nx in insns[idx + 1: idx + 24]:
                            ops = re.findall(r"\b[sd]\d+\b", nx.op_str)
                            if dst in ops or dreg in ops:
                                if nx.mnemonic.startswith("vcvt.f32.u32") or nx.mnemonic.startswith("vcvt.f32.s32"):
                                    if ops and ops[-1] in (dst, dreg):
                                        isf = False
                                if ops and ops[0] in (dst, dreg) and not nx.mnemonic.startswith("vcvt"):
                                    pass
                                if ops and ops[-1] in (dst, dreg) or (ops and ops[0] in (dst, dreg)):
                                    break
                    read_first[r] = isf
            for r in regs_write:
                written.add(r)
            if "r0" in regs_write:
                last_r0_float = m.startswith("vmov")
            is_ret = (m in ("bx",) and ins.op_str == "lr") or (m.startswith("pop") and "pc" in ins.op_str)
            if is_ret:
                returns_float_votes.append(last_r0_float)
            # comments
            cmt = ""
            if (m.startswith("ldr") or m.startswith("vldr")) and "[pc" in ins.op_str:
                mm = re.search(r"#(-?0x[0-9a-f]+|-?\d+)\]", ins.op_str)
                imm = int(mm.group(1), 0) if mm else 0
                lit = ((ins.address + 4) & ~3) + imm
                v = rd32(lit)
                if v is not None:
                    cmt = f"  ; [{lit:08x}] = 0x{v:08x}"
                    f = struct.unpack("<f", struct.pack("<I", v))[0]
                    if 1e-6 < abs(f) < 1e7:
                        cmt += f" ({f:g}f)"
                    s = cstr(v)
                    if s is not None:
                        cmt += f' "{s[:60]}"'
                    elif 0x3d000 <= v < 0x8c000:
                        pv = rd32(v)
                        cmt += f" (data)" + (f" -> 0x{pv:08x}" if pv is not None and 0x3d000 <= v < 0x3f800 else "")
            # movw/movt rX + add rX, pc  ->  PC-relative address of a string/global
            mm = re.fullmatch(r"(r\d+|r1[0-2]), #(0x[0-9a-f]+|\d+)", ins.op_str)
            if m in ("movw", "movt") and mm:
                v = int(mm.group(2), 0)
                r = mm.group(1)
                movpend[r] = (v & 0xFFFF) if m == "movw" else ((movpend.get(r, 0) & 0xFFFF) | (v << 16))
            elif m == "add" and re.fullmatch(r"(r\d+), pc", ins.op_str) and ins.op_str.split(",")[0] in movpend:
                tgt = (movpend.pop(ins.op_str.split(",")[0]) + ins.address + 4) & 0xFFFFFFFF
                cmt = f"  ; =0x{tgt:08x}"
                s = cstr(tgt)
                if s is not None:
                    cmt += f' "{s[:60]}"'
                elif 0x3e92c <= tgt < 0x3f3a0:
                    pv = rd32(tgt)
                    cmt += f" (ptr slot -> 0x{pv:08x})" if pv is not None else ""
            if m in ("bl", "blx", "b.w", "b") and ins.op_str.startswith("#"):
                t = int(ins.op_str[1:], 0)
                nm = names.get(t) or stub_names.get(t) or stub_names.get(t & ~1)
                if nm:
                    cmt = f"  ; {nm}"
                if m in ("bl", "blx") and t in names:
                    # does the caller treat r0 as float right after?
                    nxt = insns[idx + 1] if idx + 1 < len(insns) else None
                    fl = bool(nxt and nxt.mnemonic.startswith("vmov") and re.search(r"\br0\b", nxt.op_str)
                              and nxt.op_str.split(",")[0].strip()[0] in "sd")
                    callsites.setdefault(t, []).append((a, fl))
            lines.append(f"{ins.address:08x}  {m:8s} {ins.op_str}{cmt}")
        lines.sort(key=lambda s: int(s[:8], 16))
        with open(os.path.join(outdir, f"{a:08x}.s"), "w", newline="\n") as f:
            f.write(f"; {names[a]}  [{a:08x}, {end:08x})\n")
            f.write("\n".join(lines) + "\n")
        nparams = 0
        for i, r in enumerate(("r0", "r1", "r2", "r3")):
            if r in read_first:
                nparams = i + 1
        float_param = [r for r in ("r0", "r1", "r2", "r3") if read_first.get(r)]
        sig[a] = (nparams, float_param, returns_float_votes)

    with open(os.path.join(ROOT, "decomp", "signatures.tsv"), "w", newline="\n") as f:
        f.write("addr\tname\tnparams\tfloat_params\tret_float_callee\tret_float_callers\tncallers\n")
        for a in addrs:
            n, fp, votes = sig[a]
            cs = callsites.get(a, [])
            f.write(f"{a:08x}\t{names[a]}\t{n}\t{','.join(fp)}\t"
                    f"{int(bool(votes) and all(votes))}\t{sum(1 for _, x in cs if x)}\t{len(cs)}\n")
    print("wrote", len(addrs), "functions")


if __name__ == "__main__":
    main()
