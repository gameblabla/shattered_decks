#!/usr/bin/env python3
"""Everything the SPRITE layer and the Mode 3 top view show.

The Mode 7 board is a chunky bitmap in direct colour and has exactly one layer,
so for a long time the HUD and the hand were texels in that bitmap.  They are
not any more, and the reason is resolution: the board band samples one texel on
2x2 screen pixels (4x4 while it moves), so a letter drawn into the bitmap is a
letter at half the console's resolution, and a hand card is sixteen texels
stretched over thirty-two pixels.  Sprites are drawn by the PPU at the screen's
own resolution over whichever background is up, in any mode, and this machine
has 512 OBJ tiles and thirty-two sprites a scanline going spare.

So this generator produces the three things the OBJ layer needs and the one
thing the top view needs:

  * `snes_spr_cards`  -- every card face as a 32x32 4bpp sprite.  Four faces
    share a 64-tile group of the OBJ name table, because a 32x32 sprite covers
    four names on each of four rows of a sixteen-wide grid; a face is therefore
    stored as four rows of four tiles and uploaded as four 128-byte chunks.
  * `snes_spr_font`   -- the same shared 1bpp glyph table the other ports use,
    expanded to 4bpp tiles with their own drop shadow baked in, so HUD text is
    legible over the sandstone and over black without a second draw.
  * `snes_spr_pal`    -- eight OBJ palettes.  Seven are card palettes, fitted
    by clustering the faces so that near-neighbours in colour share one; the
    eighth is the HUD's.  Fifteen colours a card is what an OBJ palette is.
  * `snes_top_*`      -- the Mode 3 top view: a 5x4 table of board tiles as an
    8bpp background, deduplicated to tiles and a tilemap.  It is preloaded into
    the VRAM the Mode 7 bitmap does not use, which is what makes entering the
    top view three register writes and NOT a force-blanked rewrite.

CGRAM is split down the middle and never rewritten: 0..127 is the top view's
background palette, 128..255 is the eight OBJ palettes.  Mode 7 direct colour
does not read CGRAM at all, so the same CGRAM serves both views and a mode
change touches no colour.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "atarist"))

from PIL import Image, ImageEnhance  # noqa: E402
import gen_atarist_assets as ga  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "assets", "source")
ASSETS = os.path.join(ROOT, "src", "snes", "assets")
FONT_SRC = os.path.join(ROOT, "src", "engine", "font_menudata.h")

BANK = 10                       # $CA; the card sheet is $C8 and the font $C9

# ── Sprite cards ─────────────────────────────────────────────────────────────
SPR = 32                        # a card sprite is 32x32, the largest OBJ size
CARD_PALETTES = 7               # OBJ palettes 0..6; 7 is the HUD's

# ── The top view ─────────────────────────────────────────────────────────────
TOP_W, TOP_H = 256, 224
TOP_CELL = 48                   # a 5x4 table of 48-pixel cells...
TOP_X0, TOP_Y0 = 8, 16          # ...at 8+5*48 = 248 and 16+4*48 = 208
TOP_COLS, TOP_ROWS = 5, 4
TOP_GROOVE = 3
TOP_GROOVE_SCALE = 0.45
TOP_BG_COLOURS = 120            # CGRAM 0..127, and index 0 is reserved black

# The glyphs the HUD can say: ASCII 32..95, which is everything the other
# ports' menu font draws once lower case is folded away.
GLYPH_FIRST = 32
GLYPH_COUNT = 64
# ...and four more tiles after them: the cursor's corner brackets.
CORNER_COUNT = 4
# ...and then the life-bar furniture: nine fill states of the bar in each of the
# two sides' colours, then the two label plates.  See `bar_tiles`.
BAR_STEPS = 9                   # 0..8 columns of the tile filled
BAR_COUNT = BAR_STEPS * 2
PLATE_COUNT = 2
SNES_SPR_GOLD = 3


# ── SNES pixel formats ───────────────────────────────────────────────────────

def snes_colour(rgb):
    """15-bit BGR, five bits a channel -- what a CGRAM word holds."""
    r, g, b = (v >> 3 for v in rgb)
    return (b << 10) | (g << 5) | r


def palette_bytes(colours, n):
    out = bytearray()
    for i in range(n):
        c = snes_colour(colours[i]) if i < len(colours) else 0
        out += bytes((c & 0xFF, c >> 8))
    return bytes(out)


def tile4(indices):
    """One 8x8 tile of 4bpp: rows of planes 0/1 interleaved, then 2/3."""
    out = bytearray()
    for lo in (0, 2):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                v = (indices[y * 8 + x] >> lo) & 3
                p0 |= (v & 1) << (7 - x)
                p1 |= ((v >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
    return bytes(out)


def tile8(indices):
    """One 8x8 tile of 8bpp: four plane pairs, the same interleave."""
    out = bytearray()
    for lo in (0, 2, 4, 6):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                v = (indices[y * 8 + x] >> lo) & 3
                p0 |= (v & 1) << (7 - x)
                p1 |= ((v >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
    return bytes(out)


def cut_tiles(indices, w, x0, y0, tw, th):
    """The 8x8 index blocks of a rectangle, row-major."""
    for ty in range(th):
        for tx in range(tw):
            block = []
            for y in range(8):
                row = (y0 + ty * 8 + y) * w + x0 + tx * 8
                block += indices[row:row + 8]
            yield block


# ── Card sprites ─────────────────────────────────────────────────────────────

def face_list():
    """Every face in card-id order -- the same order gen_snes_cards.py uses, so
    a card id indexes both the board sheet and the sprite sheet."""
    out = []
    for asset_id in ga.parse_cards():
        out.append(("m:" + asset_id, ga.find_card_image(asset_id)))
    for kind in range(ga.SUPPORT_VARIANTS):
        out.append(("s:%d" % kind, None))
    out.append(("back", None))
    return out


ART_FIRST = 5                   # palette entries 5..15 are fitted to the painting
FRAME_FIRST = 1                 # ...and 1..4 are the card frame, in every palette
FRAME_COLOURS = 4
ART_COLOURS = 16 - ART_FIRST


def frame_rgb(tag):
    """The card template a face is framed in, at sprite size.  The back has
    none: it is a picture edge to edge."""
    if tag == "back":
        return None
    kind = ga.face_kind(tag)
    with Image.open(ga.FRAME_SRC[kind]) as im:
        return im.convert("RGB").resize((SPR, SPR), Image.LANCZOS)


def art_rgb(tag, path):
    """The painting that goes inside the frame, at the size of its window."""
    from PIL import ImageOps
    # The back has no frame, so its picture IS the whole card.
    x0, y0, x1, y1 = ((0, 0, SPR, SPR) if tag == "back"
                      else ga.art_window((SPR, SPR)))
    src = Image.open(path) if path else None
    art = ga.face_art(tag, src, ((x1 - x0) * 4, (y1 - y0) * 4))
    if src is not None:
        src.close()
    win = ImageOps.fit(art.convert("RGB"), (x1 - x0, y1 - y0),
                       method=Image.LANCZOS, centering=(0.5, 0.5))
    img = ga.art_prep(win, flat=not tag.startswith("m:"))
    # THE SAME LIFT THE BOARD SHEET GETS, and for the same reason: these are
    # gallery-lit paintings seen on a sunlit board, so their own midtones
    # quantise into the bottom of any fitted palette and the card reads as a
    # hole cut in the table.  gen_snes_cards.py pushes harder because the
    # direct-colour cube has two bits of blue; a fitted palette does not.
    img = ImageEnhance.Brightness(img.convert("RGB")).enhance(1.22)
    img = ImageEnhance.Contrast(img).enhance(1.15)
    return ImageEnhance.Color(img).enhance(1.25)


def ref_palette(colours):
    """A PIL palette image, for quantizing against a fixed set of colours."""
    ref = Image.new("P", (1, 1))
    flat = [v for c in colours for v in c]
    ref.putpalette(flat + [0] * (768 - len(flat)))
    return ref


def nearest(colours):
    def f(px):
        best, bd = 0, None
        for i, c in enumerate(colours):
            d = ((px[0] - c[0]) ** 2 + (px[1] - c[1]) ** 2 +
                 (px[2] - c[2]) ** 2)
            if bd is None or d < bd:
                best, bd = i, d
        return best
    return f


def frame_palette():
    """Four entries every card palette spends on the frame, fitted over all
    three templates at once so a spell and a monster share them."""
    srcs = [frame_rgb("m:x"), frame_rgb("s:0"), frame_rgb("s:5")]
    stack = Image.new("RGB", (SPR, SPR * len(srcs)))
    for j, im in enumerate(srcs):
        stack.paste(im, (0, j * SPR))
    pal = stack.quantize(colors=FRAME_COLOURS, method=Image.Quantize.MAXCOVERAGE)
    raw = pal.getpalette()[:FRAME_COLOURS * 3]
    return [tuple(raw[i * 3:i * 3 + 3]) for i in range(FRAME_COLOURS)]


def cluster(feats, k):
    """Group faces so that faces sharing a palette are near-neighbours in
    colour.  A plain k-means over a coarse colour signature: the point is not
    an optimal partition, it is that a blue dragon and a sand-coloured golem
    never end up quantised through each other's eleven entries."""
    dim = len(feats[0])
    # Seeded far apart rather than at random, so the build is reproducible.
    centres = [feats[i * len(feats) // k] for i in range(k)]
    assign = [0] * len(feats)
    for _ in range(24):
        moved = False
        for i, f in enumerate(feats):
            best, bd = 0, None
            for c, centre in enumerate(centres):
                d = sum((a - b) * (a - b) for a, b in zip(f, centre))
                if bd is None or d < bd:
                    best, bd = c, d
            if assign[i] != best:
                assign[i] = best
                moved = True
        for c in range(k):
            members = [feats[i] for i in range(len(feats)) if assign[i] == c]
            if members:
                centres[c] = [sum(m[d] for m in members) / len(members)
                              for d in range(dim)]
        if not moved:
            break
    return assign


def build_cards():
    """Sprite tiles for every face, plus the seven palettes they share.

    THE FRAME IS QUANTISED AND THE PAINTING IS DITHERED, which is the same
    split tools/atarist/gen_atarist_assets.py makes and for the same reason: a
    frame is flat tones, two gold rules and a black keyline, and Floyd-
    Steinberg over it turns each rule into a dotted line.  So four entries of
    every palette are the frame, fixed and shared, and the other eleven are
    fitted to that cluster's paintings.  Fit all fifteen to the whole card
    instead and the frame -- one or two pixels wide at this size -- loses every
    entry to the painting and the card stops having an edge at all.

    A face is emitted as FOUR ROWS OF FOUR TILES and that shape is the OBJ name
    table's, not a choice: a 32x32 sprite at name n covers n..n+3 on each of
    four consecutive rows of a sixteen-wide grid, so the uploader pushes four
    128-byte chunks 512 bytes apart rather than one 512-byte block."""
    fs = face_list()
    arts = [art_rgb(tag, path) for tag, path in fs]
    frames = [frame_rgb(tag) for tag, _ in fs]
    fpal = frame_palette()
    fmap = nearest(fpal)

    feats = []
    for img in arts:
        small = img.resize((4, 4), Image.BOX)
        feats.append([v for px in small.getdata() for v in px])
    groups = cluster(feats, CARD_PALETTES)

    art_pals = []
    for c in range(CARD_PALETTES):
        members = [arts[i] for i in range(len(arts)) if groups[i] == c]
        if not members:
            art_pals.append([(0, 0, 0)] * ART_COLOURS)
            continue
        w, h = members[0].size
        stack = Image.new("RGB", (w, h * len(members)))
        for j, img in enumerate(members):
            stack.paste(img, (0, j * h))
        # MAXCOVERAGE, not the default median cut: a painting's entries should
        # span its colours rather than crowd wherever its pixels happen to be.
        pal = stack.quantize(colors=ART_COLOURS,
                             method=Image.Quantize.MAXCOVERAGE)
        raw = pal.getpalette()[:ART_COLOURS * 3]
        art_pals.append([tuple(raw[i * 3:i * 3 + 3]) for i in range(ART_COLOURS)])

    x0, y0, x1, y1 = ga.art_window((SPR, SPR))
    blob = bytearray()
    for i, (tag, _) in enumerate(fs):
        ref = ref_palette(art_pals[groups[i]])
        art = [v + ART_FIRST
               for v in arts[i].quantize(palette=ref,
                                         dither=Image.FLOYDSTEINBERG).getdata()]
        if frames[i] is None:
            data = art
        else:
            data = [fmap(px) + FRAME_FIRST for px in frames[i].getdata()]
            for y in range(y1 - y0):
                for x in range(x1 - x0):
                    data[(y0 + y) * SPR + x0 + x] = art[y * (x1 - x0) + x]
        for block in cut_tiles(data, SPR, 0, 0, 4, 4):
            blob += tile4(block)
    return bytes(blob), bytes(groups), [fpal + p for p in art_pals]


# ── The HUD font ─────────────────────────────────────────────────────────────

def font_rows():
    import re
    text = open(FONT_SRC).read()
    body = text[text.index("n2DLib_font[128 * 8] ="):]
    values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    return values[:128 * 8]


def build_font():
    """Each glyph as a 4bpp tile with its shadow already in it.

    Colour 1 is the ink and colour 2 the shadow, offset one pixel down and
    right.  Baking the shadow in is what lets HUD text sit over the board's
    sandstone AND over black without the caller drawing anything twice, and it
    costs nothing: the tile has three spare bits per pixel either way."""
    rows = font_rows()
    blob = bytearray()
    for g in range(GLYPH_COUNT):
        src = rows[(GLYPH_FIRST + g) * 8:(GLYPH_FIRST + g) * 8 + 8]
        px = [0] * 64
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    if y + 1 < 8 and x + 1 < 8:
                        px[(y + 1) * 8 + x + 1] = 2
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    px[y * 8 + x] = 1
        blob += tile4(px)
    blob += corner_tiles()
    blob += bar_tiles()
    blob += plate_tiles()
    return bytes(blob)


def corner_tiles():
    """Four L-brackets, one per corner, in the HUD palette's gold.

    THE CURSOR IN THE TOP VIEW IS FOUR SPRITES, not a rectangle drawn into
    anything: the top view's background is a preloaded tilemap that is never
    rewritten, which is exactly what makes entering it cost three register
    writes, so nothing may draw into it.  Brackets at the corners of a cell
    also leave the card inside it entirely visible, which a filled marker or a
    full outline does not."""
    out = bytearray()
    for corner in range(4):
        right, bottom = corner & 1, corner >> 1
        px = [0] * 64
        for i in range(5):
            x = 7 - i if right else i
            y = 7 - i if bottom else i
            for t in range(2):
                yy = (7 - t) if bottom else t
                xx = (7 - t) if right else t
                px[yy * 8 + x] = SNES_SPR_GOLD
                px[y * 8 + xx] = SNES_SPR_GOLD
        out += tile4(px)
    return bytes(out)


def bar_tiles():
    """The life bar, as nine fill states per side.

    A LIFE BAR ON THIS MACHINE IS A ROW OF SPRITES AND NOTHING ELSE -- there is
    no rectangle to fill, because the board is a Mode 7 bitmap the HUD is not
    allowed to touch and the top view is a preloaded tilemap that is never
    rewritten.  So the bar is four 8x8 sprites and its length is chosen a tile
    at a time: whole tiles up to the fill, one partial tile at the boundary,
    empty tiles after it.  Nine states a tile is one screen pixel of
    granularity, which over a 32-pixel bar is 250 life points -- finer than the
    digits beside it change.

    The bar carries its own frame (top and bottom rules in the shadow colour)
    so it reads as a gauge over the board's sandstone as well as over black."""
    out = bytearray()
    for fill_colour in (PLATE_RED_INK, PLATE_BLUE_INK):
        for w in range(BAR_STEPS):
            px = [0] * 64
            for y in range(6):
                for x in range(8):
                    if y == 0 or y == 5:
                        px[y * 8 + x] = 2            # the frame's rule
                    else:
                        px[y * 8 + x] = fill_colour if x < w else BAR_EMPTY
            out += tile4(px)
    return bytes(out)


def plate_tiles():
    """The two label plates: a solid cell the side's name is drawn over.

    Red is the player and blue the opponent, which is the colour pairing every
    other port's LP panel uses (src/main.c's draw_lp_label), so a player who
    has seen the PC or PC-FX build reads this one without being taught."""
    out = bytearray()
    for colour in (PLATE_RED, PLATE_BLUE):
        px = [colour] * 64
        for x in range(8):
            px[x] = 2                                # a dark rule top...
            px[56 + x] = 2                           # ...and bottom
        out += tile4(px)
    return bytes(out)


# The HUD palette.  Index 1 is the ink every line is drawn in, 2 the shadow the
# glyphs carry, and 3..5 are what a line is recoloured to when it means
# something: the player's own numbers, the opponent's, and a warning.
HUD_PALETTE = [
    (255, 255, 255),   # 1 ink
    (0, 0, 0),         # 2 shadow
    (255, 214, 96),    # 3 gold -- the player's side
    (255, 128, 112),   # 4 red  -- the opponent's side
    (144, 232, 144),   # 5 green
    (168, 200, 255),   # 6 blue
    (176, 32, 40),     # 7 the player's plate and the ink of their bar
    (40, 64, 184),     # 8 the opponent's
    (28, 28, 44),      # 9 the empty part of a bar
] + [(0, 0, 0)] * 6

# ...named, because the bar and plate tiles are generated against them.
PLATE_RED, PLATE_BLUE = 7, 8
PLATE_RED_INK, PLATE_BLUE_INK = 4, 6
BAR_EMPTY = 9


# ── The top view ─────────────────────────────────────────────────────────────

def top_image():
    """The overhead table: a flat 5x4 grid of board tiles on black.

    It is drawn straight rather than projected, which is what the top view IS
    on the other ports -- the camera walks up until the board is a table -- and
    at full 256x224 with no chunky doubling anywhere in it."""
    a = Image.open(os.path.join(SRC, "textures", "sandstone_1.png"))
    b = Image.open(os.path.join(SRC, "textures", "sandstone_2.png"))
    mats = [im.convert("RGB").resize((TOP_CELL, TOP_CELL), Image.LANCZOS)
            for im in (a, b)]
    a.close()
    b.close()
    for m in mats:
        px = m.load()
        for y in range(TOP_CELL):
            for x in range(TOP_CELL):
                if (x < TOP_GROOVE or y < TOP_GROOVE or
                        x >= TOP_CELL - TOP_GROOVE or y >= TOP_CELL - TOP_GROOVE):
                    r, g, bl = px[x, y]
                    px[x, y] = (int(r * TOP_GROOVE_SCALE),
                                int(g * TOP_GROOVE_SCALE),
                                int(bl * TOP_GROOVE_SCALE))

    out = Image.new("RGB", (TOP_W, TOP_H), (0, 0, 0))
    for r in range(TOP_ROWS):
        for c in range(TOP_COLS):
            out.paste(mats[(r + c) & 1],
                      (TOP_X0 + c * TOP_CELL, TOP_Y0 + r * TOP_CELL))
    return out


def build_top():
    """Tiles, tilemap and the 128-entry background palette.

    Every cell of the table is one of two 48x48 materials at a tile-aligned
    origin, so the whole 32x28 screen deduplicates to two cells' worth of tiles
    plus black -- which is why an 8bpp background fits in the 4096 words of
    VRAM the Mode 7 bitmap and the OBJ tiles leave over."""
    img = top_image()
    pal_img = img.convert("P", palette=Image.ADAPTIVE, colors=TOP_BG_COLOURS)
    raw = pal_img.getpalette()[:TOP_BG_COLOURS * 3]
    colours = [tuple(raw[i * 3:i * 3 + 3]) for i in range(TOP_BG_COLOURS)]
    data = list(pal_img.getdata())

    # Index 0 must be black: it is the transparent entry the PPU shows outside
    # every tile, and CGRAM[0] is the screen's backdrop colour as well.
    black = min(range(TOP_BG_COLOURS), key=lambda i: sum(colours[i]))
    if black != 0:
        colours[0], colours[black] = colours[black], colours[0]
        swap = {0: black, black: 0}
        data = [swap.get(v, v) for v in data]
    colours[0] = (0, 0, 0)

    tiles, index, tilemap = bytearray(), {}, []
    for ty in range(TOP_H // 8):
        for tx in range(TOP_W // 8):
            block = []
            for y in range(8):
                row = (ty * 8 + y) * TOP_W + tx * 8
                block += data[row:row + 8]
            key = bytes(block)
            if key not in index:
                index[key] = len(index)
                tiles += tile8(block)
            tilemap.append(index[key])
    assert len(index) <= 128, "top view needs %d tiles, VRAM holds 128" % len(index)

    # A 32x32 entry map; the four rows past the screen show tile 0.
    blank = tilemap[0] if False else index[bytes([0] * 64)]
    words = bytearray()
    for ty in range(32):
        for tx in range(32):
            t = tilemap[ty * 32 + tx] if ty < 28 else blank
            words += bytes((t & 0xFF, (t >> 8) & 0x03))
    return bytes(tiles), bytes(words), colours


# ── Emit ─────────────────────────────────────────────────────────────────────

def emit(name, bank, blobs):
    path = os.path.join(ASSETS, name + ".asm")
    total = sum(len(b[1]) for b in blobs)
    assert total <= 0x10000, "%s is %d bytes, past one bank" % (name, total)
    lines = [
        "; Generated by tools/snes/gen_snes_obj.py; do not edit.",
        '.include "hdr.asm"',
        ".BASE $C0",
        '.SECTION "%s" BANK %d SLOT 0 ORG $0000 FORCE' % (name, bank),
        "",
    ]
    for label, blob in blobs:
        binname = label + ".bin"
        with open(os.path.join(ASSETS, binname), "wb") as fh:
            fh.write(blob)
        lines += ["%s:" % label, '    .INCBIN "%s"' % binname, ""]
    lines += [".ENDS", ""]
    with open(path, "w") as fh:
        fh.write("\n".join(lines))
    print("%s: bank $%02X, %d bytes"
          % (os.path.relpath(path, ROOT), 0xC0 + bank, total))


def main():
    os.makedirs(ASSETS, exist_ok=True)

    cards, groups, card_pals = build_cards()
    font = build_font()
    top_tiles, top_map, top_pal = build_top()

    obj_pal = bytearray()
    for c in range(CARD_PALETTES):
        obj_pal += palette_bytes([(0, 0, 0)] + card_pals[c], 16)
    obj_pal += palette_bytes([(0, 0, 0)] + HUD_PALETTE, 16)
    bg_pal = palette_bytes(top_pal, 128)

    # NOT "snes_obj": the Makefile names an object after its source's basename
    # and src/snes/snes_obj.c already owns that one, so the two would land on
    # the same .obj and the link would lose whichever came first.
    emit("snes_objassets", BANK, [
        ("snes_spr_font", font),
        ("snes_spr_pal", bytes(obj_pal)),
        ("snes_spr_group", groups),
        ("snes_bg_pal", bg_pal),
        ("snes_top_tiles", top_tiles),
        ("snes_top_map", top_map),
    ])
    emit("snes_sprcards", BANK + 1, [("snes_spr_cards", cards)])

    header = os.path.join(ROOT, "src", "snes", "snes_obj_data.h")
    with open(header, "w") as fh:
        fh.write("""/* Generated by tools/snes/gen_snes_obj.py; do not edit. */
#ifndef WAIFU_SNES_OBJ_DATA_H
#define WAIFU_SNES_OBJ_DATA_H

#include "snes_types.h"

/* A card sprite is 32x32 and lives as four rows of four tiles, which is the
 * shape of the OBJ name table a 32x32 sprite reads. */
#define SNES_SPR_CARD_PX      %d
#define SNES_SPR_CARD_BYTES   %d
#define SNES_SPR_CARD_ROW     %d     /* one of its four rows: 4 tiles of 32 */

/* The HUD glyphs: ASCII 32..95, shadowed, OBJ palette 7. */
#define SNES_SPR_GLYPH_FIRST  %d
#define SNES_SPR_GLYPH_COUNT  %d
#define SNES_SPR_CORNER_COUNT %d
/* The life bar's tiles follow the corners: BAR_STEPS fill states in the
 * player's colour, the same again in the opponent's, then the two plates. */
#define SNES_SPR_BAR_STEPS    %d
#define SNES_SPR_BAR_COUNT    %d
#define SNES_SPR_PLATE_COUNT  %d
#define SNES_SPR_HUD_PAL      7
#define SNES_SPR_INK          1
#define SNES_SPR_GOLD         3
#define SNES_SPR_RED          4

/* The Mode 3 top view. */
#define SNES_TOP_CELL         %d
#define SNES_TOP_X0           %d
#define SNES_TOP_Y0           %d
#define SNES_TOP_TILE_BYTES   %d

extern const u8 snes_spr_cards[];
extern const u8 snes_spr_font[];
extern const u8 snes_spr_pal[];
extern const u8 snes_spr_group[];     /* the OBJ palette each face was fitted to */
extern const u8 snes_bg_pal[];
extern const u8 snes_top_tiles[];
extern const u8 snes_top_map[];

#endif
""" % (SPR, SPR * SPR // 2, (SPR // 8) * 32, GLYPH_FIRST, GLYPH_COUNT,
       CORNER_COUNT, BAR_STEPS, BAR_COUNT, PLATE_COUNT,
       TOP_CELL, TOP_X0, TOP_Y0, len(top_tiles)))
    print("%s written" % os.path.relpath(header, ROOT))
    print("top view: %d tiles, %d bytes; %d card sprites"
          % (len(top_tiles) // 64, len(top_tiles), len(groups)))


if __name__ == "__main__":
    main()
