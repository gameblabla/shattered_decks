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


def dither(img, size, strength=0.9, alpha=None):
    """Floyd-Steinberg GRB332, for the STORY BUSTS AND NOTHING ELSE.

    The module docstring's rule -- nearest colour, never dithered -- is about
    the things that fill the screen: flat-lit 3D artwork and board furniture,
    where an error-diffused pattern reads as grain crawling over every surface
    for the whole of a duel.  A bust is the opposite case.  It is a painting
    with skin and cloth grades in it, it is 124 pixels wide, it stands still,
    and the two blue bits are nowhere near enough for a face -- so nearest
    colour bands it into poster paint, and diffusing the error is the only way
    the grades survive at all.

    `strength` scales the error carried into the neighbours: at 1.0 the grain
    is as loud as the banding it replaces, so the busts are baked at 0.9, which
    keeps the grades and takes the edge off the pattern in the flat areas.

    `alpha` is the bust's own mask.  Error is neither taken from nor pushed
    into transparent pixels: those are pixels the runtime never blits, and
    letting the cut-out ground bleed into the figure would put a rim of dirt
    around every silhouette.
    """
    width, height = size
    src = img.convert("RGB")
    if src.size != size:
        src = src.resize(size, Image.LANCZOS)
    px = list(src.getdata())
    mask = None
    if alpha is not None:
        if alpha.size != size:
            alpha = alpha.resize(size, Image.LANCZOS)
        mask = list(alpha.getdata())

    # One float error plane per channel, carried a row at a time.
    err = [[0.0, 0.0, 0.0] for _ in range(width * height)]
    out = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            i = y * width + x
            if mask is not None and mask[i] <= 96:
                out[i] = 0
                continue
            r, g, b = px[i]
            e = err[i]
            r = clamp8(r + e[0])
            g = clamp8(g + e[1])
            b = clamp8(b + e[2])
            ri, gi, bi = R_LUT[r], G_LUT[g], B_LUT[b]
            out[i] = (gi << 5) | (ri << 2) | bi
            dr = (r - R_LEVELS[ri]) * strength
            dg = (g - G_LEVELS[gi]) * strength
            db = (b - B_LEVELS[bi]) * strength
            for dx, dy, share in ((1, 0, 7.0 / 16.0), (-1, 1, 3.0 / 16.0),
                                  (0, 1, 5.0 / 16.0), (1, 1, 1.0 / 16.0)):
                nx, ny = x + dx, y + dy
                if nx < 0 or nx >= width or ny >= height:
                    continue
                n = ny * width + nx
                if mask is not None and mask[n] <= 96:
                    continue
                ne = err[n]
                ne[0] += dr * share
                ne[1] += dg * share
                ne[2] += db * share
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
