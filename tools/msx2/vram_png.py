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

    rows = []
    for y in range(HEIGHT):
        logical = base + y * WIDTH
        line = bytes(vram[(logical + x) >> 1 | ((logical + x) & 1) * PAGE_BYTES]
                     for x in range(WIDTH))
        row = bytearray()
        for byte in line:
            r, g, b = grb332_rgb(byte)
            row += bytes((r, g, b)) * scale
        for _ in range(scale):
            rows.append(row)

    write_png(args[1], rows, WIDTH * scale, HEIGHT * scale)
    print("wrote %s (page %d, %dx%d)" % (args[1], page, WIDTH * scale, HEIGHT * scale))


if __name__ == "__main__":
    main()
