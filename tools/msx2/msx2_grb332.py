#!/usr/bin/env python3
"""GRB332 conversion shared by every MSX2 asset generator.

GRAPHIC 7 has no palette: a byte *is* a colour, (G<<5)|(R<<2)|B, with three
bits of green, three of red and only two of blue.  Blue is therefore the
channel that bands, which is why everything here is dithered in the image
rather than quantised per pixel -- a per-pixel nearest-colour pass turns a
sunset into four flat stripes.

Everything in this module works on 8-bit GRB332 byte strings, row-major, no
header: that is exactly the layout the VDP wants, so a blob written here is
pushed at the data port unchanged.
"""

import struct
import zlib

from PIL import Image

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
    """Floyd-Steinberg dither to GRB332, serpentine to avoid a diagonal grain."""
    width, height = size
    src = img.convert("RGB")
    if src.size != size:
        src = src.resize(size, Image.LANCZOS)
    pixels = [[[float(c) for c in src.getpixel((x, y))] for x in range(width)]
              for y in range(height)]

    out = bytearray(width * height)
    for y in range(height):
        columns = range(width) if (y % 2 == 0) else range(width - 1, -1, -1)
        ahead = 1 if (y % 2 == 0) else -1
        for x in columns:
            old = pixels[y][x]
            ri = R_LUT[clamp8(old[0])]
            gi = G_LUT[clamp8(old[1])]
            bi = B_LUT[clamp8(old[2])]
            new = (R_LEVELS[ri], G_LEVELS[gi], B_LEVELS[bi])
            out[y * width + x] = (gi << 5) | (ri << 2) | bi

            err = [old[c] - new[c] for c in range(3)]
            for dx, dy, weight in ((ahead, 0, 7 / 16.0), (-ahead, 1, 3 / 16.0),
                                   (0, 1, 5 / 16.0), (ahead, 1, 1 / 16.0)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < width and 0 <= ny < height:
                    for c in range(3):
                        pixels[ny][nx][c] += err[c] * weight
    return bytes(out)


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
