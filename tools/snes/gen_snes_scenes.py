#!/usr/bin/env python3
"""Build the first Mode 3 scene assets for the SNES fork.

The title is a 256x224, 8bpp background: the source painting is cropped to
the SNES visible area, lightly lettered, quantised to one CGRAM palette, and
then encoded as SNES planar tiles plus a 32x32 tile map.  The asset is kept in
its own bank so entering the duel can restore the existing Mode 7 layout
without making the title compete with the board or OBJ assets.
"""

import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCE = os.path.join(ROOT, "assets", "source", "title", "title256.png")
ASSETS = os.path.join(ROOT, "src", "snes", "assets")
HEADER = os.path.join(ROOT, "src", "snes", "snes_title.h")
SCENE_HEADER = os.path.join(ROOT, "src", "snes", "snes_scene_data.h")
FONT_SOURCE = os.path.join(ROOT, "src", "engine", "font_menudata.h")
PREVIEW = os.path.join(ASSETS, "title_preview.png")

WIDTH, HEIGHT = 256, 224
TILES_X, TILES_Y = WIDTH // 8, HEIGHT // 8
PALETTE_ENTRIES = 256
BANK = 9                         # $C9; $C8 cards, $CA OBJ assets
STORY_BANK = 7                   # $C7; 256x144 art leaves font VRAM free
ENDING_BANK = 13                 # $CD; full-screen ending art
SCENE_FONT_BANK = 14             # $CE; outlined dialogue font and frame tiles
SCENE_FONT_GLYPH_FIRST = 32
SCENE_FONT_GLYPH_COUNT = 64
SCENE_BORDER_TILE = SCENE_FONT_GLYPH_COUNT
SCENE_BORDER_COUNT = 8

# Entries 0..15 are reserved for the 4bpp BG2 font.  Keeping those entries
# out of the adaptive painting palette means the typewriter remains readable
# while the 8bpp BG1 art still gets 240 colours of its own.
TEXT_PALETTE = [
    (0, 0, 0), (255, 255, 255), (0, 0, 0), (255, 224, 136),
    (196, 56, 48), (112, 192, 255), (255, 160, 96), (176, 224, 176),
    (64, 48, 40), (255, 240, 192), (88, 64, 48), (208, 208, 208),
    (144, 112, 80), (224, 128, 88), (112, 144, 176), (255, 255, 255),
]


def snes_colour(rgb):
    """Convert an RGB tuple to the SNES little-endian BGR555 word."""
    r, g, b = (value >> 3 for value in rgb)
    return (b << 10) | (g << 5) | r


def palette_bytes(colours):
    out = bytearray()
    for colour in colours:
        value = snes_colour(colour)
        out += bytes((value & 0xFF, value >> 8))
    return bytes(out)


def tile8(indices):
    """Encode one 8x8 tile as the four interleaved 8bpp plane pairs."""
    out = bytearray()
    for low_plane in (0, 2, 4, 6):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                value = (indices[y * 8 + x] >> low_plane) & 3
                p0 |= (value & 1) << (7 - x)
                p1 |= ((value >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
    return bytes(out)


def tile4(indices):
    """Encode one 8x8 tile as the four SNES 4bpp bitplanes."""
    out = bytearray()
    for low_plane in (0, 2):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                value = (indices[y * 8 + x] >> low_plane) & 3
                p0 |= (value & 1) << (7 - x)
                p1 |= ((value >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
    return bytes(out)


def font(size):
    candidates = [
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/NimbusSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def title_art():
    with Image.open(SOURCE) as source:
        # The supplied 256x240 painting has eight overscan-like rows above and
        # below the visible SNES composition.  Keep the central 224 lines.
        image = source.convert("RGB").crop((0, 8, WIDTH, 232))

    draw = ImageDraw.Draw(image)
    logo = "SHATTERED DECKS"
    prompt = "PRESS A TO DUEL"

    # A restrained drop shadow keeps the lettering readable without putting a
    # flat opaque panel over the painting.
    logo_font = font(20)
    prompt_font = font(12)
    draw.text((9, 9), logo, font=logo_font, fill=(18, 11, 8),
              stroke_width=2, stroke_fill=(18, 11, 8))
    draw.text((7, 7), logo, font=logo_font, fill=(255, 224, 136),
              stroke_width=1, stroke_fill=(78, 34, 18))
    draw.text((10, 198), prompt, font=prompt_font, fill=(15, 12, 10),
              stroke_width=2, stroke_fill=(15, 12, 10))
    draw.text((8, 196), prompt, font=prompt_font, fill=(255, 238, 187),
              stroke_width=1, stroke_fill=(78, 34, 18))
    return image


def story_art():
    """A real desert backdrop with Serena's shipped portrait over it."""
    with Image.open(os.path.join(ROOT, "assets", "source", "bg", "desert.png")) as source:
        image = source.convert("RGB").crop((0, 0, 256, 144)).convert("RGBA")
    with Image.open(os.path.join(ROOT, "assets", "source", "story_portraits",
                                 "serena.png")) as source:
        portrait = source.convert("RGBA").resize((115, 144), Image.Resampling.LANCZOS)
    # Serena is the player-side portrait; reserve the right side for opponents.
    image.alpha_composite(portrait, (0, 0))
    return image.convert("RGB")


def scene_font_rows():
    import re
    text = open(FONT_SOURCE).read()
    body = text[text.index("n2DLib_font[128 * 8] ="):]
    values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    return values[:128 * 8]


def scene_border_tiles():
    """The eight tiles of a classic RPG dialogue window frame.

    The outer dark rule keeps the frame readable over the bright end of the
    ramp, while the gold and white inner rules give it the layered, RPG Maker
    window-box look without spending another background layer.
    """
    dark, gold, light = 2, 3, 1
    out = bytearray()
    for kind in range(SCENE_BORDER_COUNT):
        px = [0] * 64
        top = kind in (0, 1, 2)
        bottom = kind in (5, 6, 7)
        left = kind in (0, 3, 5)
        right = kind in (2, 4, 7)
        if top:
            for x in range(8):
                px[0 * 8 + x] = dark
                px[1 * 8 + x] = gold
                px[2 * 8 + x] = light
                px[3 * 8 + x] = dark
        if bottom:
            for x in range(8):
                px[7 * 8 + x] = dark
                px[6 * 8 + x] = gold
                px[5 * 8 + x] = light
                px[4 * 8 + x] = dark
        if left:
            for y in range(8):
                px[y * 8 + 0] = dark
                px[y * 8 + 1] = gold
                px[y * 8 + 2] = light
                px[y * 8 + 3] = dark
        if right:
            for y in range(8):
                px[y * 8 + 7] = dark
                px[y * 8 + 6] = gold
                px[y * 8 + 5] = light
                px[y * 8 + 4] = dark
        out += tile4(px)
    return bytes(out)


def scene_font():
    """The dialogue font: white glyphs with a one-pixel black outline."""
    rows = scene_font_rows()
    blob = bytearray()
    for glyph in range(SCENE_FONT_GLYPH_COUNT):
        src = rows[(SCENE_FONT_GLYPH_FIRST + glyph) * 8:
                   (SCENE_FONT_GLYPH_FIRST + glyph) * 8 + 8]
        px = [0] * 64
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            xx, yy = x + dx, y + dy
                            if 0 <= xx < 8 and 0 <= yy < 8 and px[yy * 8 + xx] == 0:
                                px[yy * 8 + xx] = 2
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    px[y * 8 + x] = 1
        blob += tile4(px)
    blob += scene_border_tiles()
    return bytes(blob)


def write_scene_font_asset():
    data = scene_font()
    with open(os.path.join(ASSETS, "snes_scene_font.bin"), "wb") as fh:
        fh.write(data)
    with open(os.path.join(ASSETS, "snes_sceneassets.asm"), "w") as fh:
        fh.write("""; Generated by tools/snes/gen_snes_scenes.py.
.include "hdr.asm"
.BASE $C0
.SECTION "snes_sceneassets" BANK %d SLOT 0 ORG $0000 FORCE
snes_scene_font:
    .INCBIN "snes_scene_font.bin"
.ENDS
""" % SCENE_FONT_BANK)
    return len(data)


def save_scene_asset(name, image, bank, blank_tile=False):
    """Quantise and write one BG1 8bpp scene plus its 32x32 map."""
    width, height = image.size
    assert width == 256 and height % 8 == 0
    indexed = image.quantize(colors=240, method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    raw = indexed.getpalette()[:240 * 3]
    art_colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    art_colours += [(0, 0, 0)] * (240 - len(art_colours))
    colours = TEXT_PALETTE + art_colours
    indices = [value + 16 for value in indexed.getdata()]

    tiles = bytearray()
    tiles_x, tiles_y = width // 8, height // 8
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            block = []
            for y in range(8):
                row = (ty * 8 + y) * width + tx * 8
                block += indices[row:row + 8]
            tiles += tile8(block)
    blank = tiles_x * tiles_y
    if blank_tile:
        tiles += tile8([0] * 64)

    tilemap = bytearray()
    for ty in range(32):
        for tx in range(32):
            if ty < tiles_y and tx < tiles_x:
                tile = ty * tiles_x + tx
            elif blank_tile:
                tile = blank
            else:
                tile = 0
            tilemap += bytes((tile & 0xFF, tile >> 8))

    def write_blob(suffix, data):
        with open(os.path.join(ASSETS, "snes_%s_%s.bin" % (name, suffix)), "wb") as fh:
            fh.write(data)

    write_blob("tiles", tiles)
    write_blob("pal", palette_bytes(colours))
    write_blob("map", tilemap)

    ppu_palette = []
    for colour in colours:
        word = snes_colour(colour)
        r, g, b = word & 31, (word >> 5) & 31, (word >> 10) & 31
        ppu_palette += [(r << 3) | (r >> 2), (g << 3) | (g >> 2),
                        (b << 3) | (b >> 2)]
    preview = Image.frombytes("P", (width, height), bytes(indices))
    preview.putpalette(ppu_palette + [0] * (768 - len(ppu_palette)))
    preview.convert("RGB").save(os.path.join(ASSETS, "%s_preview.png" % name))

    with open(os.path.join(ASSETS, "snes_%s.asm" % name), "w") as fh:
        fh.write("""; Generated by tools/snes/gen_snes_scenes.py.
.include "hdr.asm"
.BASE $C0
.SECTION "snes_%s" BANK %d SLOT 0 ORG $0000 FORCE
snes_%s_tiles:
    .INCBIN "snes_%s_tiles.bin"
snes_%s_pal:
    .INCBIN "snes_%s_pal.bin"
snes_%s_map:
    .INCBIN "snes_%s_map.bin"
.ENDS
""" % (name, bank, name, name, name, name, name, name))

    return len(tiles), len(tilemap)


def write_scene_header(scene_font_bytes, story_sizes, ending_sizes):
    with open(SCENE_HEADER, "w") as fh:
        fh.write("""/* Generated by tools/snes/gen_snes_scenes.py. */
#ifndef WAIFU_SNES_SCENE_DATA_H
#define WAIFU_SNES_SCENE_DATA_H

#include "snes_types.h"

#define SNES_SCENE_FONT_BYTES %d
#define SNES_SCENE_BORDER_TILE %d
#define SNES_SCENE_BORDER_COUNT %d
#define SNES_STORY_TILE_BYTES %d
#define SNES_ENDING_TILE_BYTES %d
#define SNES_SCENE_PAL_BYTES 512
#define SNES_SCENE_MAP_BYTES 2048

extern const u8 snes_scene_font[];
extern const u8 snes_story_tiles[];
extern const u8 snes_story_pal[];
extern const u8 snes_story_map[];
extern const u8 snes_ending_tiles[];
extern const u8 snes_ending_pal[];
extern const u8 snes_ending_map[];

#endif /* WAIFU_SNES_SCENE_DATA_H */
""" % (scene_font_bytes, SCENE_BORDER_TILE, SCENE_BORDER_COUNT,
       story_sizes[0], ending_sizes[0]))


def build():
    os.makedirs(ASSETS, exist_ok=True)
    scene_font_bytes = write_scene_font_asset()
    image = title_art()
    indexed = image.quantize(colors=PALETTE_ENTRIES,
                             method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    raw = indexed.getpalette()[:PALETTE_ENTRIES * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours += [(0, 0, 0)] * (PALETTE_ENTRIES - len(colours))
    indices = list(indexed.getdata())

    tiles = bytearray()
    for ty in range(TILES_Y):
        for tx in range(TILES_X):
            block = []
            for y in range(8):
                row = (ty * 8 + y) * WIDTH + tx * 8
                block += indices[row:row + 8]
            tiles += tile8(block)

    # The visible area occupies 28 of the 32 rows in a standard 32x32 map.
    # The four off-screen rows are harmless zero tiles and make the map base
    # and addressing identical to the duel's other Mode 3 background.
    tilemap = bytearray()
    for ty in range(32):
        for tx in range(32):
            tile = ty * TILES_X + tx if ty < TILES_Y else 0
            tilemap += bytes((tile & 0xFF, tile >> 8))

    with open(os.path.join(ASSETS, "snes_title_tiles.bin"), "wb") as fh:
        fh.write(tiles)
    with open(os.path.join(ASSETS, "snes_title_pal.bin"), "wb") as fh:
        fh.write(palette_bytes(colours))
    with open(os.path.join(ASSETS, "snes_title_map.bin"), "wb") as fh:
        fh.write(tilemap)

    # Preview with the same 5-bit colour precision the PPU will display.
    ppu_palette = []
    for colour in colours:
        word = snes_colour(colour)
        r, g, b = word & 31, (word >> 5) & 31, (word >> 10) & 31
        ppu_palette += [(r << 3) | (r >> 2), (g << 3) | (g >> 2),
                        (b << 3) | (b >> 2)]
    preview = Image.frombytes("P", (WIDTH, HEIGHT), bytes(indices))
    preview.putpalette(ppu_palette + [0] * (768 - len(ppu_palette)))
    preview.convert("RGB").save(PREVIEW)

    with open(os.path.join(ASSETS, "snes_title.asm"), "w") as fh:
        fh.write("""; Generated by tools/snes/gen_snes_scenes.py.
.include "hdr.asm"
.BASE $C0
.SECTION "snes_title" BANK %d SLOT 0 ORG $0000 FORCE
snes_title_tiles:
    .INCBIN "snes_title_tiles.bin"
snes_title_pal:
    .INCBIN "snes_title_pal.bin"
snes_title_map:
    .INCBIN "snes_title_map.bin"
.ENDS
""" % BANK)

    with open(HEADER, "w") as fh:
        fh.write("""/* Generated by tools/snes/gen_snes_scenes.py. */
#ifndef WAIFU_SNES_TITLE_H
#define WAIFU_SNES_TITLE_H

#include "snes_types.h"

#define SNES_TITLE_TILE_BYTES 57344
#define SNES_TITLE_PAL_BYTES 512
#define SNES_TITLE_MAP_BYTES 2048

extern const u8 snes_title_tiles[];
extern const u8 snes_title_pal[];
extern const u8 snes_title_map[];

#endif /* WAIFU_SNES_TITLE_H */
""")

    story_sizes = save_scene_asset("story", story_art(), STORY_BANK,
                                  blank_tile=True)
    with Image.open(os.path.join(ROOT, "assets", "source", "ending",
                                 "ending256x240.png")) as source:
        # Like the story backdrop, the ending gives the dialogue window the
        # lower eighty lines.  This keeps the HDMA ramp on the transparent
        # backdrop instead of relying on a per-line layer mask.
        ending = source.convert("RGB").crop((0, 8, WIDTH, 152))
    ending_sizes = save_scene_asset("ending", ending, ENDING_BANK,
                                   blank_tile=True)
    write_scene_header(scene_font_bytes, story_sizes, ending_sizes)

    print("title: %d tiles, %d-byte map, bank %d" %
          (TILES_X * TILES_Y, len(tilemap), BANK))
    print("story: %d bytes, ending: %d bytes" %
          (story_sizes[0], ending_sizes[0]))


if __name__ == "__main__":
    build()
