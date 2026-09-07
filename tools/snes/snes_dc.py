#!/usr/bin/env python3
"""Direct-colour conversion shared by every SNES asset generator.

Mode 7 in direct-colour mode has no palette: the 8-bit texel *is* the colour,
BBGGGRRR -- three bits of red, three of green and only two of blue.  That is
the same colour cube the MSX2's GRAPHIC 7 shows, in a different bit order, so
this module reuses tools/msx2/msx2_grb332.py's channel tables rather than
re-deriving them: the two ports quantise the same art into the same cube and
should not disagree about what "nearest" means.

Everything here works on 8-bit direct-colour byte strings, row-major, no
header, because that is exactly what the chunky framebuffer stores and what a
DMA pushes at $2118 unchanged.

Blue is the channel to watch.  Two bits is four levels, and the arena's
horizon and the blue-tinted cards are where that shows first; where a gradient
bands visibly the fix belongs in the art, not in a dither that would crawl
across a board the player stares at for a whole duel.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "msx2"))

from msx2_grb332 import (  # noqa: E402
    R_LEVELS, G_LEVELS, B_LEVELS, R_LUT, G_LUT, B_LUT, clamp8,
)
from PIL import Image, ImageChops  # noqa: E402


def pack(r, g, b):
    """RGB 0..255 -> the direct-colour byte, no dithering."""
    return B_LUT[clamp8(b)] << 6 | G_LUT[clamp8(g)] << 3 | R_LUT[clamp8(r)]


def unpack(byte):
    return (R_LEVELS[byte & 7], G_LEVELS[(byte >> 3) & 7], B_LEVELS[(byte >> 6) & 3])


def quantize(img, size):
    """Nearest-colour direct-colour bytes for a whole picture.

    Three channel LUTs through Image.point, so the conversion happens inside
    PIL: 78 cards times two representations is the bulk of the asset build and
    doing it a pixel at a time in Python turns seconds into minutes."""
    src = img.convert("RGB")
    if src.size != size:
        src = src.resize(size, Image.LANCZOS)
    r, g, b = src.split()
    r = r.point([R_LUT[v] for v in range(256)])
    g = g.point([G_LUT[v] << 3 for v in range(256)])
    b = b.point([B_LUT[v] << 6 for v in range(256)])
    return ImageChops.add(ImageChops.add(r, g), b).tobytes()


def preview(data, size):
    """Decode a direct-colour blob back to a PIL image, so a conversion can be
    eyeballed without a console."""
    out = Image.new("RGB", size)
    out.putdata([unpack(b) for b in data])
    return out


def write_preview(path, data, size):
    preview(data, size).save(path)
