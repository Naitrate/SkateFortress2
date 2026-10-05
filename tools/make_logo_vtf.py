#!/usr/bin/env python3
"""Turn an image into the main menu logo (materials/logo/new_tf2_logo.vtf).

    nix-shell shell.nix --run "python3 tools/make_logo_vtf.py art/skate-fortress-2-logo.webp"

TF2's main menu draws logo/new_tf2_logo on a 2048x512 canvas, the logo
spanning x 20..2015 and y 0..471. The image is scaled to that band (aspect kept,
centred) on a transparent canvas and written as an uncompressed RGBA VTF 7.2
with mipmaps. Needs Pillow (in shell.nix).
"""
import struct
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "game/mod_tf/materials/logo/new_tf2_logo.vtf"
CANVAS = (2048, 512)
BAND = (20, 0, 2016, 472)  # where TF2's own logo sits (left, top, right, bottom)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    image = Image.open(sys.argv[1]).convert("RGBA")
    band_w, band_h = BAND[2] - BAND[0], BAND[3] - BAND[1]
    scale = min(band_w / image.width, band_h / image.height)
    size = (round(image.width * scale), round(image.height * scale))
    image = image.resize(size, Image.LANCZOS)
    canvas = Image.new("RGBA", CANVAS, (0, 0, 0, 0))
    canvas.paste(image, (BAND[0] + (band_w - size[0]) // 2, BAND[1] + (band_h - size[1]) // 2))

    mips = [canvas]
    while mips[-1].width > 1 or mips[-1].height > 1:
        w, h = mips[-1].size
        mips.append(mips[-1].resize((max(1, w // 2), max(1, h // 2)), Image.LANCZOS))
    # Flags: clamp S/T, eight-bit alpha (the original also sets 0x40).
    flags = 0x4 | 0x8 | 0x40 | 0x2000
    header = struct.pack("<4s2IIHHIHH4x3f4xfIBIBBH", b"VTF\0", 7, 2, 80, *CANVAS, flags, 1, 0,
                         0.5, 0.5, 0.5, 1.0, 0, len(mips), 0xFFFFFFFF, 0, 0, 1)
    header += b"\0" * (80 - len(header))
    OUT.write_bytes(header + b"".join(m.tobytes() for m in reversed(mips)))  # RGBA8888, smallest first
    print(f"{sys.argv[1]} ({size[0]}x{size[1]} in a {CANVAS[0]}x{CANVAS[1]} canvas) -> {OUT}")


if __name__ == "__main__":
    main()
