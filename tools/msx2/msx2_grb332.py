#!/usr/bin/env python3
"""GRB332 conversion shared by every MSX2 asset generator.

GRAPHIC 7 has no palette: a byte *is* a colour, (G<<5)|(R<<2)|B, with three
bits of green, three of red and only two of blue.

**Nothing here is dithered.**  An error-diffused or ordered pattern buys back
some of the blue channel's four levels in theory, and on a 256x212 screen of
flat-lit 3D artwork it reads as crawling grain over every surface -- which is
worse than the banding it removes, and worse still on the duel board, where the
same picture is on screen for the whole duel and the grain never resolves.  So
a pixel takes its nearest representable colour and nothing else.  Where a
gradient would band visibly, the fix is in the art (a flatter grade, a tint the
palette can actually hold), not in the quantiser.

Everything in this module works on 8-bit GRB332 byte strings, row-major, no
header: that is exactly the layout the VDP wants, so a blob written here is
pushed at the data port unchanged.
"""

import struct
import zlib

from PIL import Image, ImageChops

# The exact levels the VDP shows, in 0..255.
R_LEVELS = [i * 255 // 7 for i in range(8)]
G_LEVELS = [i * 255 // 7 for i in range(8)]
B_LEVELS = [i * 255 // 3 for i in range(4)]


def _level_lut(levels):
    """value 0..255 -> index of the nearest representable level."""
    lut = []
    for value in range(256):
        best = 0
        for i, level in enumerate(levels):
            if abs(level - value) < abs(levels[best] - value):
                best = i
        lut.append(best)
    return lut


R_LUT = _level_lut(R_LEVELS)
G_LUT = _level_lut(G_LEVELS)
B_LUT = _level_lut(B_LEVELS)


def clamp8(value):
    return 0 if value < 0 else (255 if value > 255 else int(value))


def pack(r, g, b):
    """RGB 0..255 -> the GRB332 byte, no dithering.  For flat UI colours."""
    return (G_LUT[clamp8(g)] << 5) | (R_LUT[clamp8(r)] << 2) | B_LUT[clamp8(b)]


def unpack(byte):
    return (R_LEVELS[(byte >> 2) & 7], G_LEVELS[(byte >> 5) & 7], B_LEVELS[byte & 3])


def quantize(img, size):
    """Nearest-colour GRB332, no dithering (see the module docstring).

    Done with three channel LUTs through `Image.point`, so the whole picture is
    converted inside PIL rather than a pixel at a time in Python -- which is
    what makes re-baking every view, every camera-move pose and 78 cards a
    few seconds instead of a few minutes."""
    width, height = size
    src = img.convert("RGB")
    if src.size != size:
        src = src.resize(size, Image.LANCZOS)
    r, g, b = src.split()
    # Each channel is mapped straight to its packed field, so the three bands
    # can then simply be added together.
    r = r.point([R_LUT[v] << 2 for v in range(256)])
    g = g.point([G_LUT[v] << 5 for v in range(256)])
    b = b.point([B_LUT[v] for v in range(256)])
    packed = ImageChops.add(ImageChops.add(r, g), b)
    return packed.tobytes()


def write_preview(path, data, size):
    """Decode a GRB332 blob back to a PNG, so a conversion can be eyeballed
    without an emulator.  Written by hand rather than through PIL so the
    preview is provably the bytes the VDP will see."""
    width, height = size
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(unpack(data[y * width + x]))

    def chunk(tag, payload):
        head = struct.pack(">I", len(payload)) + tag + payload
        return head + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        f.write(chunk(b"IEND", b""))
