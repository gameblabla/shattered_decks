#!/usr/bin/env python3
"""Extract the baked waifu_texture_atlas[] from src/generated/waifu_assets.h
into a raw .bin.

The 32X CD boot ROM uploads the SH2 image through a single 128 KiB
framebuffer pass, so the boot image must stay under 131072 bytes.  The 9 KiB
3D texture atlas is pure data: the CD32X build streams it from the ISO
(ASSETS/TEX_ATLAS.BIN) into BSS at startup instead of baking it into .text.
"""
import re
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src).read()
    m = re.search(r'waifu_texture_atlas\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        sys.exit("waifu_texture_atlas[] not found in " + src)
    data = bytes(int(v) for v in re.findall(r'\d+', m.group(1)))
    tile = int(re.search(r'#define\s+WAIFU_TEX_TILE_SIZE\s+(\d+)', text).group(1))
    count = int(re.search(r'#define\s+WAIFU_TEX_TILE_COUNT\s+(\d+)', text).group(1))
    expect = tile * tile * count
    if len(data) != expect:
        sys.exit("atlas is %d bytes, expected %d" % (len(data), expect))
    with open(dst, 'wb') as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (dst, len(data)))


if __name__ == '__main__':
    main()
