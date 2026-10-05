#!/usr/bin/env python3
"""Find PowerPC code that builds a given address with lis + addi/ori/lwz-family
pairs, and dump the containing function (bounds from .pdata)."""
import bisect
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

BASE = 0x82000000


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        pdata_va, pdata_size = 0x8232A800, 0x41BA8
        o = pdata_va - BASE
        self.funcs = []
        for i in range(pdata_size // 8):
            begin, info = struct.unpack_from(">II", self.data, o + 8 * i)
            if begin == 0:
                continue
            length = ((info >> 8) & 0x3FFFFF) * 4
            self.funcs.append((begin, length))
        self.funcs.sort()
        self.starts = [f[0] for f in self.funcs]
        self.md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)

    def word(self, va):
        return struct.unpack_from(">I", self.data, va - BASE)[0]

    def function_at(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and self.funcs[i][0] <= va < self.funcs[i][0] + self.funcs[i][1]:
            return self.funcs[i]
        return None

    def refs_to(self, target, text=(0x82370000, 0x82F734D4)):
        """lis rA, hi ; then within 16 instrs an addi/ori/ld/st with rA and lo."""
        hi = (target + 0x8000) >> 16 & 0xFFFF
        lo = target & 0xFFFF
        hits = []
        for va in range(text[0], text[1], 4):
            w = self.word(va)
            if w >> 26 == 15 and (w >> 16) & 0x1F == 0 and w & 0xFFFF == hi:  # lis rD, hi
                rd = (w >> 21) & 0x1F
                for k in range(1, 24):
                    w2 = self.word(va + 4 * k)
                    op = w2 >> 26
                    ra = (w2 >> 16) & 0x1F
                    if ra == rd and w2 & 0xFFFF == lo and op in (14, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54, 58, 62):
                        hits.append(va + 4 * k)
                        break
        return hits

    def disasm(self, start, length):
        code = self.data[start - BASE : start - BASE + length]
        return list(self.md.disasm(code, start))


if __name__ == "__main__":
    img = Image(sys.argv[1])
    for target in sys.argv[2:]:
        t = int(target, 16)
        for va in img.refs_to(t):
            f = img.function_at(va)
            print(f"{t:#x} referenced at {va:#x} in function {f[0]:#x}+{f[1]:#x}" if f else f"{t:#x} at {va:#x}")
