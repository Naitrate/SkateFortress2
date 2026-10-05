#!/usr/bin/env python3
"""Direct-call graph (bl) for a Skate 3 image, with .pdata function bounds."""
import bisect
import collections
import struct


class Graph:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        n = struct.unpack_from("<H", d, pe + 6)[0]
        osz = struct.unpack_from("<H", d, pe + 20)[0]
        for i in range(n):
            s = pe + 24 + osz + 40 * i
            name, vs, va = struct.unpack_from("<8sII", d, s)
            if name.startswith(b".pdata"):
                pva, pvs = va, vs
            if name.startswith(b".text"):
                self.text = (va, vs)
        self.funcs = sorted(
            (struct.unpack_from(">I", d, pva + 8 * i)[0], ((struct.unpack_from(">I", d, pva + 8 * i + 4)[0] >> 8) & 0x3FFFFF) * 4)
            for i in range(pvs // 8)
        )
        self.starts = [f[0] for f in self.funcs]
        self.callers = collections.defaultdict(set)
        self.callees = collections.defaultdict(set)
        va, vs = self.text
        for o in range(va, va + vs, 4):
            w = struct.unpack_from(">I", d, o)[0]
            if w >> 26 == 18 and w & 3 == 1:
                disp = w & 0x3FFFFFC
                if disp & 0x2000000:
                    disp -= 0x4000000
                src = self.function(0x82000000 + o)
                dst = 0x82000000 + o + disp
                self.callers[dst].add(src)
                self.callees[src].add(dst)

    def function(self, va):
        return self.starts[bisect.bisect_right(self.starts, va) - 1]
