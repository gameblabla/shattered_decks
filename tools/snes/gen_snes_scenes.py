#!/usr/bin/env python3
"""The Mode 3 scenes: title, story dialogue, ending.

The title is the whole 256x224 painting as an 8bpp BG1 -- no strip is given
up to a black prompt bar.  "PRESS START" is baked into the painting as a
second set of tiles for the two rows it covers, and the attract blink is a
swap between the two 32x32 maps; the menu that replaces it is BG2 text over a
colour-math window, so it needs no tiles at all.

The story dialogue used to be a 32 KB picture of the desert.  It is now three
things that cost a fraction of that:

  * the SKY is CGRAM entry 0 rewritten by HDMA every scanline -- a ramp of
    real 15-bit colours from the top of the painting to its horizon, the same
    way the duel's HUD plate is a tinted backdrop rather than pixels;
  * the GROUND is the painting's three tile rows under the horizon, as 4bpp
    BG2 tiles in BG2 palette 1 (the starry sky is six star tiles scattered by
    a seeded shuffle instead);
  * the two SPEAKERS are 8bpp BG1 tiles, a hundred and twelve colours each,
    where the OBJ portraits they replace had fifteen.

Which painting a dialogue gets follows src/main.c's story_scene_kind():
progress 0 and 1 stand in the desert, 2 in the temple, 3 at the volcano and
4 in the void.
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
TITLE_ART_ROWS = TILES_Y
TITLE_PROMPT_MAX_TILES = 28      # what the bank has left after the painting
PALETTE_ENTRIES = 256
BANK = 9                         # $C9; $C8 cards, $CA OBJ assets
STORY_BANK = 7                   # $C7; the four backgrounds and their skies
ENDING_BANK = 13                 # $CD; full-screen ending art
SCENE_FONT_BANK = 14             # $CE; outlined dialogue font and frame tiles
PORTRAIT_BANK = 24               # $D8..$DD; one 8bpp BG1 sheet per portrait
PORTRAIT_W, PORTRAIT_H = 128, 136   # sixteen by seventeen tiles: lines 8..143
PORTRAIT_COLOURS = 112
PORTRAIT_FIRST = (32, 144)       # Serena's entries, then the opponent's
GROUND_FIRST = 16                # BG2 palette 1
GROUND_COLOURS = 15
SKY_LINES = 120                  # the HDMA ramp; the ground starts on line 120
GROUND_ROWS = 3                  # tile rows 15..17, lines 120..143
SCENE_ROWS = 18                  # the picture above the dialogue window
STORY_KINDS = ("desert", "stone", "ember", "sky")
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

    # A restrained drop shadow keeps the lettering readable without putting a
    # flat opaque panel over the painting.
    logo_font = font(20)
    logo_box = draw.textbbox((0, 0), logo, font=logo_font, stroke_width=2)
    logo_x = (WIDTH - (logo_box[2] - logo_box[0])) // 2
    draw.text((logo_x + 2, 9), logo, font=logo_font, fill=(18, 11, 8),
              stroke_width=2, stroke_fill=(18, 11, 8))
    draw.text((logo_x, 7), logo, font=logo_font, fill=(255, 224, 136),
              stroke_width=1, stroke_fill=(78, 34, 18))
    return image


PROMPT_ROW = 24                  # the prompt's two tile rows: lines 192..207


def title_prompt(image):
    """The same painting with PRESS START lettered over its lower rows.

    Only the tiles that differ from the plain painting are kept, so the text
    is drawn in the same lettering as the logo yet costs a couple of dozen
    tiles rather than a second picture."""
    out = image.copy()
    draw = ImageDraw.Draw(out)
    text = "PRESS START"
    f = font(11)
    box = draw.textbbox((0, 0), text, font=f, stroke_width=1)
    x = (WIDTH - (box[2] - box[0])) // 2
    y = PROMPT_ROW * 8 + 3
    draw.text((x + 1, y + 1), text, font=f, fill=(18, 11, 8),
              stroke_width=1, stroke_fill=(18, 11, 8))
    draw.text((x, y), text, font=f, fill=(255, 240, 200),
              stroke_width=1, stroke_fill=(78, 34, 18))
    return out


# ── The story backgrounds ────────────────────────────────────────────────────

def bg_source(kind):
    return Image.open(os.path.join(ROOT, "assets", "source", "bg",
                                   kind + ".png")).convert("RGB")


# Each sky's ramp, top to horizon.  The paintings themselves are a flat
# field of one colour dithered into the ground over the last thirty lines, so
# these are chosen from them rather than measured: the desert deepens the
# painting's blue at the zenith and runs to cyan at the horizon, the volcano's
# violet lightens towards the glow, the temple's night blue lifts to the grey
# above the stone, and the void is black.
SKY_RAMPS = {
    "desert": ((0, 72, 224), (88, 196, 255)),
    "ember": ((112, 0, 208), (208, 120, 236)),
    "stone": ((8, 20, 64), (96, 104, 136)),
    "sky": ((0, 0, 0), (0, 0, 0)),
}


def sky_ramp(kind):
    """One BGR555 word per scanline of sky, lines 0..119."""
    top, bottom = SKY_RAMPS[kind]
    ramp = []
    for y in range(SKY_LINES):
        t = y / float(SKY_LINES - 1)
        c = tuple(int(round(top[i] + (bottom[i] - top[i]) * t)) for i in range(3))
        ramp.append(snes_colour(c))
    return ramp


def ground_tiles(kind):
    """The tile rows under the horizon as 4bpp BG2 tiles: (tiles, map, pal).

    Tile 0 is always blank.  The map is SCENE_ROWS rows of 32 cells, BG2
    palette 1, and for the void it is star tiles scattered over every row."""
    tiles = [tile4([0] * 64)]
    cells = [0] * (SCENE_ROWS * 32)
    if kind == "sky":
        img = bg_source(kind)
        # Six star tiles: the brightest 8x8 blocks of the painting, each a
        # star at a different offset in its cell.
        blocks = []
        for ty in range(img.height // 8):
            for tx in range(TILES_X):
                b = img.crop((tx * 8, ty * 8, tx * 8 + 8, ty * 8 + 8))
                lum = sum(sum(p) for p in b.getdata())
                if lum:
                    blocks.append((lum, tx, ty, b))
        blocks.sort(key=lambda b: -b[0])
        colours = [(0, 0, 0), (255, 255, 255), (176, 176, 208), (96, 96, 128)]
        picked = []
        for lum, tx, ty, b in blocks:
            key = tuple(1 if sum(p) > 96 else 0 for p in b.getdata())
            if key in [k for k, _ in picked]:
                continue
            px = []
            for p in b.getdata():
                v = sum(p) // 3
                px.append(0 if v < 32 else 3 if v < 96 else 2 if v < 192 else 1)
            picked.append((key, px))
            if len(picked) == 6:
                break
        for _, px in picked:
            tiles.append(tile4(px))
        import random
        rng = random.Random(0x5744)
        for i in range(SCENE_ROWS * 32):
            if rng.random() < 0.22:
                cells[i] = rng.randrange(1, len(tiles))
        pal = colours + [(0, 0, 0)] * (16 - len(colours))
        return b"".join(tiles), cells, pal
    img = bg_source(kind).crop((0, SKY_LINES, WIDTH, SKY_LINES + GROUND_ROWS * 8))
    indexed = img.quantize(colors=GROUND_COLOURS, method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.FLOYDSTEINBERG)
    raw = indexed.getpalette()[:GROUND_COLOURS * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours += [(0, 0, 0)] * (GROUND_COLOURS - len(colours))
    data = [v + 1 for v in indexed.getdata()]
    for ty in range(GROUND_ROWS):
        for tx in range(TILES_X):
            block = []
            for y in range(8):
                row = (ty * 8 + y) * WIDTH + tx * 8
                block += data[row:row + 8]
            t = tile4(block)
            if t not in tiles:
                tiles.append(t)
            cells[(SCENE_ROWS - GROUND_ROWS + ty) * 32 + tx] = tiles.index(t)
    return b"".join(tiles), cells, [(0, 0, 0)] + colours


def write_story_backgrounds():
    """Bank $C7: for each kind, the sky ramp, the ground tiles, its map and
    palette, behind a small directory the runtime indexes by kind."""
    blobs = []
    directory = bytearray()
    sizes = {}
    for kind in STORY_KINDS:
        ramp = sky_ramp(kind)
        sky = bytearray()
        for word in ramp:
            sky += bytes((word & 0xFF, word >> 8))
        tiles, cells, pal = ground_tiles(kind)
        cell_map = bytearray()
        for c in cells:
            cell_map += bytes((c, 0x04))     # BG2 palette 1, low priority
        blobs.append(("snes_story_%s_sky" % kind, bytes(sky)))
        blobs.append(("snes_story_%s_tiles" % kind, tiles))
        blobs.append(("snes_story_%s_map" % kind, bytes(cell_map)))
        blobs.append(("snes_story_%s_pal" % kind, palette_bytes(pal)))
        sizes[kind] = len(tiles)
    asm = ['; Generated by tools/snes/gen_snes_scenes.py.',
           '.include "hdr.asm"', '.BASE $C0',
           '.SECTION "snes_story" BANK %d SLOT 0 ORG $0000 FORCE' % STORY_BANK]
    total = 0
    for label, blob in blobs:
        with open(os.path.join(ASSETS, label + ".bin"), "wb") as fh:
            fh.write(blob)
        asm += ['%s:' % label, '    .INCBIN "%s.bin"' % label]
        total += len(blob)
    asm += ['.ENDS', '']
    assert total <= 0x10000, "story backgrounds are %d bytes" % total
    with open(os.path.join(ASSETS, "snes_story.asm"), "w") as fh:
        fh.write("\n".join(asm))
    return sizes


# SNES uses the hand-pixelled character set in story order: Serena, the first
# opponent, then opponents 1 through 4.  The current files are numbered in
# that presentation order.  Keep these explicit so adding a newer
# PC portrait at the top level cannot silently change the cartridge artwork.
PORTRAIT_CROPS = [
    ("pixelart/serna_portrait_160px_pixelart.png", 16, 0),
    ("pixelart/opponent_1_pixelart.png", 0, 0),
    ("pixelart/opponent_2_pixelart.png", 0, 0),
    ("pixelart/opponent_3_pixelart.png", 0, 0),
    ("pixelart/opponent_4_pixelart.png", 0, 0),
    ("pixelart/opponent_5_pixelart.png", 0, 0),
]


def portrait_asset(filename, source_x, source_y, first):
    """One speaker as a 16x17 block of 8bpp BG1 tiles and its 112 colours.

    Crop the pixel-art source into the 128x136 story window at exactly 1:1;
    source_x/source_y choose the visible native-pixel region for each figure.
    Index 0 is transparent: the sky ramp and the ground show through round the
    figure.  `first` is the CGRAM entry the block's colours start at, baked into
    the tiles: Serena only ever stands on the left and reads entries 32..143,
    an opponent on the right reads 144..255."""
    path = os.path.join(ROOT, "assets", "source", "story_portraits", filename)
    with Image.open(path) as source:
        canvas = source.convert("RGBA").crop(
            (source_x, source_y, source_x + PORTRAIT_W,
             source_y + PORTRAIT_H))
    rgb = Image.new("RGB", canvas.size, (0, 0, 0))
    rgb.paste(canvas.convert("RGB"), mask=canvas.getchannel("A"))
    indexed = rgb.quantize(colors=PORTRAIT_COLOURS - 1,
                           method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.FLOYDSTEINBERG)
    raw = indexed.getpalette()[:(PORTRAIT_COLOURS - 1) * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours += [(0, 0, 0)] * (PORTRAIT_COLOURS - 1 - len(colours))
    alpha = list(canvas.getchannel("A").getdata())
    values = [0 if a < 48 else (v + first + 1)
              for v, a in zip(indexed.getdata(), alpha)]
    tiles = bytearray()
    for ty in range(PORTRAIT_H // 8):
        for tx in range(PORTRAIT_W // 8):
            block = []
            for y in range(8):
                row = (ty * 8 + y) * PORTRAIT_W + tx * 8
                block += values[row:row + 8]
            tiles += tile8(block)
    # Entry `first` itself is the figure's black outline colour, so nothing
    # in the block ever reads as the transparent index.
    return bytes(tiles), palette_bytes([(0, 0, 0)] + colours)


def write_portraits():
    asm = ['; Generated by tools/snes/gen_snes_scenes.py.',
           '.include "hdr.asm"', '.BASE $C0']
    for i, (filename, source_x, source_y) in enumerate(PORTRAIT_CROPS):
        tiles, pal = portrait_asset(filename, source_x, source_y,
                                    PORTRAIT_FIRST[0 if i == 0 else 1])
        with open(os.path.join(ASSETS, "snes_portrait_%d.bin" % i), "wb") as fh:
            fh.write(tiles)
        with open(os.path.join(ASSETS, "snes_portrait_%d_pal.bin" % i), "wb") as fh:
            fh.write(pal)
        asm += ['.SECTION "snes_portrait_%d" BANK %d SLOT 0 ORG $0000 FORCE' %
                (i, PORTRAIT_BANK + i),
                'snes_portrait_%d:' % i,
                '    .INCBIN "snes_portrait_%d.bin"' % i,
                'snes_portrait_%d_pal:' % i,
                '    .INCBIN "snes_portrait_%d_pal.bin"' % i,
                '.ENDS']
    with open(os.path.join(ASSETS, "snes_portraits.asm"), "w") as fh:
        fh.write("\n".join(asm) + "\n")


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
    with open(os.path.join(ASSETS, "snes_scene_text_pal.bin"), "wb") as fh:
        fh.write(palette_bytes(TEXT_PALETTE))
    with open(os.path.join(ASSETS, "snes_sceneassets.asm"), "w") as fh:
        fh.write("""; Generated by tools/snes/gen_snes_scenes.py.
.include "hdr.asm"
.BASE $C0
.SECTION "snes_sceneassets" BANK %d SLOT 0 ORG $0000 FORCE
snes_scene_font:
    .INCBIN "snes_scene_font.bin"
snes_scene_text_pal:
    .INCBIN "snes_scene_text_pal.bin"
.ENDS
""" % SCENE_FONT_BANK)
    return len(data)


def save_scene_asset(name, image, bank, blank_tile=False, art_colors=240):
    """Quantise and write one BG1 8bpp scene plus its 32x32 map."""
    width, height = image.size
    assert width == 256 and height % 8 == 0
    indexed = image.quantize(colors=art_colors, method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    raw = indexed.getpalette()[:art_colors * 3]
    art_colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    art_colours += [(0, 0, 0)] * (art_colors - len(art_colours))
    colours = TEXT_PALETTE + art_colours
    colours += [(0, 0, 0)] * (256 - len(colours))
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
#define SNES_SCENE_GLYPH_BYTES %d
#define SNES_SCENE_BORDER_TILE %d
#define SNES_SCENE_BORDER_COUNT %d
#define SNES_ENDING_TILE_BYTES %d
#define SNES_SCENE_PAL_BYTES 512
#define SNES_SCENE_MAP_BYTES 2048

/* The story dialogue's picture: see the generator's docstring. */
#define SNES_STORY_KINDS        %d
#define SNES_STORY_SKY_LINES    %d
#define SNES_STORY_SKY_BYTES    %d
#define SNES_STORY_SCENE_ROWS   %d
#define SNES_STORY_MAP_BYTES    %d
#define SNES_STORY_GROUND_PAL   %d
#define SNES_STORY_GROUND_TILES_MAX %d
#define SNES_PORTRAIT_W         %d
#define SNES_PORTRAIT_H         %d
#define SNES_PORTRAIT_COLS      %d
#define SNES_PORTRAIT_ROWS      %d
#define SNES_PORTRAIT_TILES     %d
#define SNES_PORTRAIT_BYTES     %d
#define SNES_PORTRAIT_COLOURS   %d
#define SNES_PORTRAIT_PAL_BYTES %d
#define SNES_PORTRAIT_FIRST_L   %d
#define SNES_PORTRAIT_FIRST_R   %d

extern const u8 snes_scene_font[];
extern const u8 snes_scene_text_pal[];      /* BG2 palette 0: 16 entries */
extern const u8 snes_ending_tiles[];
extern const u8 snes_ending_pal[];
extern const u8 snes_ending_map[];
""" % (scene_font_bytes, SCENE_FONT_GLYPH_COUNT * 32, SCENE_BORDER_TILE,
       SCENE_BORDER_COUNT, ending_sizes[0],
       len(STORY_KINDS), SKY_LINES, SKY_LINES * 2, SCENE_ROWS, SCENE_ROWS * 64,
       GROUND_FIRST, max(story_sizes.values()) // 32,
       PORTRAIT_W, PORTRAIT_H, PORTRAIT_W // 8, PORTRAIT_H // 8,
       (PORTRAIT_W // 8) * (PORTRAIT_H // 8),
       (PORTRAIT_W // 8) * (PORTRAIT_H // 8) * 64,
       PORTRAIT_COLOURS, PORTRAIT_COLOURS * 2,
       PORTRAIT_FIRST[0], PORTRAIT_FIRST[1]))
        for kind in STORY_KINDS:
            for part in ("sky", "tiles", "map", "pal"):
                fh.write("extern const u8 snes_story_%s_%s[];\n" % (kind, part))
        for i in range(len(PORTRAIT_CROPS)):
            fh.write("extern const u8 snes_portrait_%d[];\n"
                     "extern const u8 snes_portrait_%d_pal[];\n" % (i, i))
        fh.write("""
/* The ground tile blobs differ in length; the runtime uploads this many. */
""")
        for kind in STORY_KINDS:
            fh.write("#define SNES_STORY_%s_TILE_BYTES %d\n"
                     % (kind.upper(), story_sizes[kind]))
        fh.write("\n#endif /* WAIFU_SNES_SCENE_DATA_H */\n")


def build():
    os.makedirs(ASSETS, exist_ok=True)
    scene_font_bytes = write_scene_font_asset()
    image = title_art()
    prompt = title_prompt(image)
    # Mode 3 BG2 owns palette 0 for the menu.  Keep those sixteen entries
    # stable and fit the painting into 16..255, just like the story scenes do,
    # so runtime text never inherits arbitrary colours from the art.  Both
    # versions of the painting are quantised against ONE palette, fitted to
    # the plain one: the prompt's tiles must share it.
    indexed = image.quantize(colors=PALETTE_ENTRIES - 16,
                             method=Image.Quantize.MEDIANCUT,
                             dither=Image.Dither.NONE)
    raw = indexed.getpalette()[:(PALETTE_ENTRIES - 16) * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours = TEXT_PALETTE + colours
    colours += [(0, 0, 0)] * (PALETTE_ENTRIES - len(colours))
    # Both pictures through the SAME nearest-colour mapping, or every cell of
    # the prompt rows differs from the plain painting by a stray index.
    indices = [v + 16 for v in
               image.quantize(palette=indexed, dither=Image.Dither.NONE).getdata()]
    prompt_indices = [v + 16 for v in
                      prompt.quantize(palette=indexed, dither=Image.Dither.NONE).getdata()]

    def art_tile(src, tx, ty):
        block = []
        for y in range(8):
            row = (ty * 8 + y) * WIDTH + tx * 8
            block += src[row:row + 8]
        return tile8(block)

    tiles = bytearray()
    for ty in range(TITLE_ART_ROWS):
        for tx in range(TILES_X):
            tiles += art_tile(indices, tx, ty)
    tilemap = bytearray()
    for ty in range(32):
        for tx in range(32):
            tile = ty * TILES_X + tx if ty < TITLE_ART_ROWS else 0
            tilemap += bytes((tile & 0xFF, tile >> 8))

    # The prompt: only the cells whose tile changed get a new tile, appended
    # after the painting; the second map points those cells at them.
    prompt_map = bytearray(tilemap)
    extra = 0
    for ty in range(PROMPT_ROW, PROMPT_ROW + 2):
        for tx in range(TILES_X):
            t = art_tile(prompt_indices, tx, ty)
            if t == tiles[(ty * TILES_X + tx) * 64:(ty * TILES_X + tx + 1) * 64]:
                continue
            index = TITLE_ART_ROWS * TILES_X + extra
            tiles += t
            extra += 1
            cell = (ty * 32 + tx) * 2
            prompt_map[cell] = index & 0xFF
            prompt_map[cell + 1] = index >> 8
    assert extra <= TITLE_PROMPT_MAX_TILES, "PRESS START needs %d tiles" % extra

    with open(os.path.join(ASSETS, "snes_title_tiles.bin"), "wb") as fh:
        fh.write(tiles)
    with open(os.path.join(ASSETS, "snes_title_pal.bin"), "wb") as fh:
        fh.write(palette_bytes(colours))
    with open(os.path.join(ASSETS, "snes_title_map.bin"), "wb") as fh:
        fh.write(tilemap)
    with open(os.path.join(ASSETS, "snes_title_prompt_map.bin"), "wb") as fh:
        fh.write(prompt_map)

    # Preview with the same 5-bit colour precision the PPU will display.
    ppu_palette = []
    for colour in colours:
        word = snes_colour(colour)
        r, g, b = word & 31, (word >> 5) & 31, (word >> 10) & 31
        ppu_palette += [(r << 3) | (r >> 2), (g << 3) | (g >> 2),
                        (b << 3) | (b >> 2)]
    preview = Image.frombytes("P", (WIDTH, HEIGHT), bytes(prompt_indices))
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
snes_title_prompt_map:
    .INCBIN "snes_title_prompt_map.bin"
.ENDS
""" % BANK)
    assert len(tiles) + 512 + 4096 <= 0x10000, "the title bank overflows"

    with open(HEADER, "w") as fh:
        fh.write("""/* Generated by tools/snes/gen_snes_scenes.py. */
#ifndef WAIFU_SNES_TITLE_H
#define WAIFU_SNES_TITLE_H

#include "snes_types.h"

#define SNES_TITLE_TILE_BYTES %d
#define SNES_TITLE_PAL_BYTES 512
#define SNES_TITLE_MAP_BYTES 2048

extern const u8 snes_title_tiles[];
extern const u8 snes_title_pal[];
extern const u8 snes_title_map[];
/* The same map with PRESS START's cells pointing at their lettered tiles. */
extern const u8 snes_title_prompt_map[];

#endif /* WAIFU_SNES_TITLE_H */
""" % len(tiles))

    write_portraits()
    story_sizes = write_story_backgrounds()
    with Image.open(os.path.join(ROOT, "assets", "source", "ending",
                                 "ending256x240.png")) as source:
        # Like the story backdrop, the ending gives the dialogue window the
        # lower eighty lines.  This keeps the HDMA ramp on the transparent
        # backdrop instead of relying on a per-line layer mask.
        ending = source.convert("RGB").crop((0, 8, WIDTH, 152))
    ending_sizes = save_scene_asset("ending", ending, ENDING_BANK,
                                   blank_tile=True)
    write_scene_header(scene_font_bytes, story_sizes, ending_sizes)

    print("title: %d tiles (%d for the prompt), bank %d" %
          (len(tiles) // 64, extra, BANK))
    print("story grounds: %s; ending: %d bytes" %
          (", ".join("%s %d" % (k, v // 32) for k, v in story_sizes.items()),
           ending_sizes[0]))


if __name__ == "__main__":
    build()
