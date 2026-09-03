#!/usr/bin/env python3
"""Render an MSX2 GRAPHIC 7 VRAM dump to a PNG.

Screenshots through the emulator's own renderer need a display, and under Xvfb
openMSX's GL renderer hands back an empty frame -- so this target photographs
itself the way the other console targets do: dump the 128 KiB of VRAM and decode
it here.  That is deterministic, needs no display server, and it can show either
page, including the one that is not currently being scanned out.

GRAPHIC 7 is 256 bytes per line, one byte per pixel, colour GRB332:
    bits 7-5 green, bits 4-2 red, bits 1-0 blue.
Page 0 is logical VRAM 0x00000, page 1 is 0x10000; only the first 212 lines
display.

INTERLEAVING.  In GRAPHIC 6 and 7 the V9938 splits VRAM across two 64 KiB
banks and takes even bytes from one and odd bytes from the other, so a logical
address A lives at physical (A >> 1) in bank (A & 1).  The emulator's
`physical VRAM` debuggable is the raw chip layout, so a naive read shows each
picture twice at half width -- which is the de-interleaved image, not a bug in
the ROM.  This decoder undoes the interleave.

Usage:
    tools/msx2/vram_png.py dump.vram out.png [--page N] [--scale N]
"""

import struct
import sys
import zlib

WIDTH = 256
HEIGHT = 212
PAGE_BYTES = 0x10000


def grb332_rgb(byte):
    g = (byte >> 5) & 0x07
    r = (byte >> 2) & 0x07
    b = byte & 0x03
    return (r * 255 // 7, g * 255 // 7, b * 255 // 3)


def write_png(path, rows, width, height):
    raw = bytearray()
    for row in rows:
        raw.append(0)                      # filter type 0: none
        raw.extend(row)

    def chunk(tag, data):
        head = struct.pack(">I", len(data)) + tag + data
        return head + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def int_option(name, default):
    if name in sys.argv:
        return int(sys.argv[sys.argv.index(name) + 1])
    for arg in sys.argv:
        if arg.startswith(name + "="):
            return int(arg.split("=", 1)[1])
    return default


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    page = int_option("--page", 0)
    scale = int_option("--scale", 1)
    # The option values are positional args too; drop them.
    for name in ("--page", "--scale"):
        if name in sys.argv:
            args.remove(sys.argv[sys.argv.index(name) + 1])
    if len(args) != 2:
        sys.exit(__doc__)

    vram = open(args[0], "rb").read()
    base = page * PAGE_BYTES
    if len(vram) < 2 * PAGE_BYTES:
        sys.exit("dump is %d bytes: expected a full 128 KiB of VRAM" % len(vram))

    # SPRITES.
    # The dump is the bitmap only: the V9938 composites its sprite plane at
    # scan-out, so a decoded page shows none of it.  With --sprites the plane is
    # drawn on top the way the chip would, out of the same tables msx2_sprite.c
    # writes -- patterns at 0xF000, colours at 0xF800, attributes at 0xFA00, all
    # in page 0, 16x16 magnified to 32x32.  Without it an explosion or a result
    # word is invisible in a screenshot and reads as a bug that is not there.
    overlay = {}
    if "--sprites" in sys.argv:
        def vread(addr):
            return vram[addr >> 1 | (addr & 1) * PAGE_BYTES]
        for spr in range(32):
            sy = vread(0xFA00 + spr * 4)
            if sy == 216:
                break
            sx = vread(0xFA00 + spr * 4 + 1)
            pat = vread(0xFA00 + spr * 4 + 2) & 0xFC
            attr = vread(0xF800 + spr * 16)
            col = attr & 0x0F
            if col == 0:
                continue
            # Sprite mode 2 keeps "early clock" in bit 7 of the colour byte, and
            # it shifts the sprite thirty-two pixels LEFT -- which is the only
            # way a sprite can be partly off the left edge, and so the only way
            # a result banner can slide on from outside the screen.  Ignoring it
            # photographed the entering letters stacked on top of the settled
            # ones and made a correct slide look like a corrupt one.
            if attr & 0x80:
                sx -= 32
            top = (sy + 1) & 0xFF
            for py in range(16):
                left = vread(0xF000 + pat * 8 + py)
                right = vread(0xF000 + pat * 8 + 16 + py)
                bits = (left << 8) | right
                for px in range(16):
                    if not (bits & (0x8000 >> px)):
                        continue
                    for dy in range(2):
                        for dx in range(2):
                            overlay[(sx + px * 2 + dx, top + py * 2 + dy)] = col
    # A rough palette for the sprite indices msx2_sprite.c sets.
    SPRITE_RGB = {1: (255, 255, 255), 2: (255, 190, 0), 3: (255, 40, 40),
                  4: (0, 210, 190), 5: (90, 140, 255),
                  # The selector's shadow and highlight either side of 3 and 4:
                  # without these a shaded gem photographs as the unknown-index
                  # magenta and the shot says nothing about the shading.
                  6: (109, 0, 0), 7: (255, 182, 145),
                  8: (0, 72, 36), 9: (145, 218, 255)}

    rows = []
    for y in range(HEIGHT):
        logical = base + y * WIDTH
        line = bytes(vram[(logical + x) >> 1 | ((logical + x) & 1) * PAGE_BYTES]
                     for x in range(WIDTH))
        row = bytearray()
        for x, byte in enumerate(line):
            r, g, b = grb332_rgb(byte)
            if (x, y) in overlay:
                r, g, b = SPRITE_RGB.get(overlay[(x, y)], (255, 0, 255))
            row += bytes((r, g, b)) * scale
        for _ in range(scale):
            rows.append(row)

    write_png(args[1], rows, WIDTH * scale, HEIGHT * scale)
    print("wrote %s (page %d, %dx%d)" % (args[1], page, WIDTH * scale, HEIGHT * scale))


if __name__ == "__main__":
    main()
