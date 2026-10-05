#!/usr/bin/env python3
"""Recover AttribSys field/class names by hashing candidate strings (lookup8,
same as skate-engine/crates/skate-data/src/attrib_hash.rs)."""
import re
M = (1 << 64) - 1


def _mix(a, b, c):
    for x, y, z in ((43, 9, 8), (38, 23, 5), (35, 49, 11), (12, 18, 22)):
        a = ((a - b - c) & M) ^ (c >> x)
        b = ((b - c - a) & M) ^ ((a << y) & M)
        c = ((c - a - b) & M) ^ (b >> z)
    return a, b, c


def attrib_hash(text: str) -> int:
    data = text.encode()
    if not data:
        return 0
    a = b = 0xABCDEF0011223344
    c = 0x9E3779B97F4A7C13
    full = len(data) - len(data) % 24
    for i in range(0, full, 24):
        x, y, z = (int.from_bytes(data[i + k : i + k + 8], "little") for k in (0, 8, 16))
        a, b, c = _mix((a + x) & M, (b + y) & M, (c + z) & M)
    c = (c + len(data)) & M
    for i, byte in enumerate(data[full:]):
        if i < 8:
            a = (a + (byte << (i * 8))) & M
        elif i < 16:
            b = (b + (byte << ((i - 8) * 8))) & M
        else:
            c = (c + (byte << ((i - 15) * 8))) & M
    return _mix(a, b, c)[2]


def candidates_from_binary(data: bytes, minimum=3):
    for m in re.finditer(rb"[A-Za-z_][A-Za-z0-9_:. \-]{%d,}" % (minimum - 1), data):
        yield m.group().decode()
