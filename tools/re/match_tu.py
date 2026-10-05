#!/usr/bin/env python3
"""Map functions between two Skate 3 builds (e.g. TU0 -> TU3) by an
address-independent signature: opcodes and register fields only."""
import hashlib
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    osz = struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(n):
        s = pe + 24 + osz + 40 * i
        name, vs, va = struct.unpack_from("<8sII", d, s)
        if name.startswith(b".pdata"):
            pva, pvs = va, vs
    funcs = {}
    for i in range(pvs // 8):
        b, info = struct.unpack_from(">II", d, pva + 8 * i)
        if b:
            funcs[b] = ((info >> 8) & 0x3FFFFF) * 4
    return d, funcs


def signature(d, start, length):
    words = []
    for o in range(start - 0x82000000, start - 0x82000000 + length, 4):
        w = struct.unpack_from(">I", d, o)[0]
        op = w >> 26
        if op in (18, 16):          # branches: keep opcode + link/aa bits
            w &= 0xFC000003 if op == 18 else 0xFFFF0003
        elif op not in (19, 31, 30, 59, 63, 4):  # D-form: drop the immediate
            w &= 0xFFFF0000
        words.append(w)
    return hashlib.sha1(struct.pack(">%dI" % len(words), *words)).hexdigest()


def build_index(d, funcs):
    index = {}
    for a, l in funcs.items():
        index.setdefault((l, signature(d, a, l)), []).append(a)
    return index


if __name__ == "__main__":
    d0, f0 = load(sys.argv[1])
    d3, f3 = load(sys.argv[2])
    idx = build_index(d3, f3)
    for arg in sys.argv[3:]:
        a = int(arg, 16)
        fa = max(x for x in f0 if x <= a)
        m = idx.get((f0[fa], signature(d0, fa, f0[fa])), [])
        print(f"{a:#x} (func {fa:#x}, {f0[fa]} bytes) -> {[hex(x) for x in m]}")
