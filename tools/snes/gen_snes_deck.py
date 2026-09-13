#!/usr/bin/env python3
"""The SNES deck editor's picture: the PC-FX editor on Mode 3 BG1 Direct Colour.

The PC-FX draws its editor into a 256-wide indexed framebuffer: a navy panel
with a triple border, two tabs, a six-by-three gallery of the 38x54 hand-card
faces scaled to 26x34 with a hard black drop shadow, a red double outline round
the cursor, an info box and a hint line (draw_deck_editor() in src/main.c).
This generator bakes that picture for the SNES, where BG1 is an 8bpp
direct-colour background, so the gallery shows eighteen different cards at
once with no palette to share:

  * The STATIC PICTURE -- panel, borders, both tab states, the info box, the
    striped margin outside the panel -- as a deduplicated pool of 8bpp tiles
    and a 32x28 map, plus a two-row strip per tab state that the runtime drops
    over map rows 3..4.

  * Every card's ICON BLOCK: a 40x40 (five by five tile) cell holding the
    26x34 mini card at (6, 3), its shadow, and enough navy round it for the
    cursor outline (x 3..34, y 0..39), which the runtime paints into a WRAM
    copy of the block before it goes to VRAM.  Cells are the same size as the
    gallery pitch (40), so the blocks tile the gallery band edge to edge.

DIRECT COLOUR, AND WHAT THE NAVY COSTS.  A texel byte is BBGGGRRR and the
tile's palette bits add one low bit to each channel (R5 = rrr<<2 | p0<<1,
G5 = ggg<<2 | p1<<1, B5 = bb<<3 | p2<<2).  Every cell here uses palette 4,
so blue's four levels are 33/99/165/231 instead of 0/66/132/198 and white is
(231,231,231) rather than yellowish.  The panel's navy (7,10,43) quantises to
byte 0 -- and byte 0 is TRANSPARENT on any background, so it cannot be
painted.  It is the backdrop instead: CGRAM 0 is $1420, the same navy the
card check uses, and every navy pixel of the picture is left at 0.  Pure
black is the other casualty: it becomes 0x01, (33,0,33), a purple-black that
reads as black against the navy and keeps the drop shadows opaque.

Text is BG2, the outlined scene font at priority 1 over the opaque BG1; its
four ink palettes (white, gold, dim, red) are emitted here too.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "atarist"))

from PIL import Image, ImageDraw  # noqa: E402
import gen_atarist_assets as ga  # noqa: E402
import gen_snes_bigcards as gb  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(ROOT, "src", "snes", "assets")
GENERATED = os.path.join(ROOT, "assets", "generated")
HEADER = os.path.join(ROOT, "src", "snes", "snes_deck_data.h")

WIDTH, HEIGHT = 256, 224
MAP_ROWS = HEIGHT // 8

# PC-FX hand card face and the icon it is scaled to.
FACE_W, FACE_H = 38, 54
ICON_W, ICON_H = 26, 34
# One icon block: the cell, the icon's offset inside it, the drop shadow.
BLOCK = 40
BLOCK_TILES = (BLOCK // 8) * (BLOCK // 8)
BLOCK_BYTES = BLOCK_TILES * 64
ICON_X, ICON_Y = 6, 3
CURSOR_X0, CURSOR_X1 = ICON_X - 3, ICON_X + ICON_W + 2    # 3..34 inclusive
# The gallery: six columns by three rows of blocks, block (0, 0) at (8, 56).
GRID_COLS, GRID_ROWS = 6, 3
GRID_X0, GRID_Y0 = 8, 56
# Tabs (map rows 3..4) and the two-row strip that swaps their state.
TAB_Y, TAB_H = 24, 16
TAB_ROW0, TAB_ROWS = TAB_Y // 8, TAB_H // 8
# Where the icon blocks start in the BG1 tile pool; the static picture must
# fit below it.
ICON_BASE_TILE = 128
ICONS_PER_BANK = 40
ICON_BANKS = (4, 5)                  # $C4, $C5
UI_BANK = 17                         # $D1
MONSTERS = 72
SUPPORTS = ga.SUPPORT_VARIANTS
ICONS = MONSTERS + SUPPORTS

NAVY_BGR555 = 0x1420                 # (7,10,43): R 0, G 1, B 5
BG1_PAL = 4                          # blue low bit on every cell

# The direct-colour levels a palette-4 cell can show, in 8-bit terms.
R_LEVELS = [((v << 2) * 255 + 15) // 31 for v in range(8)]
G_LEVELS = R_LEVELS
B_LEVELS = [(((v << 3) | 4) * 255 + 15) // 31 for v in range(4)]


def nearest(levels, v):
    best = 0
    for i, lv in enumerate(levels):
        if abs(lv - v) < abs(levels[best] - v):
            best = i
    return best


R_LUT = [nearest(R_LEVELS, v) for v in range(256)]
B_LUT = [nearest(B_LEVELS, v) for v in range(256)]


def pack(rgb):
    """RGB -> the texel byte; 0 is transparent (the navy backdrop)."""
    r, g, b = rgb
    v = (B_LUT[b] << 6) | (R_LUT[g] << 3) | R_LUT[r]
    if v == 0 and rgb != NAVY:
        v = 0x01
    return v


def unpack(v):
    if v == 0:
        return NAVY
    return (R_LEVELS[v & 7], G_LEVELS[(v >> 3) & 7], B_LEVELS[v >> 6])


def tile8(px):
    """One 8x8 block of texel bytes as an 8bpp planar tile (64 bytes)."""
    out = bytearray()
    for lo in (0, 2, 4, 6):
        for y in range(8):
            p0 = p1 = 0
            for x in range(8):
                v = (px[y * 8 + x] >> lo) & 3
                p0 |= (v & 1) << (7 - x)
                p1 |= ((v >> 1) & 1) << (7 - x)
            out += bytes((p0, p1))
    return bytes(out)


def cut(px, stride, x0, y0, tw, th):
    for ty in range(th):
        for tx in range(tw):
            yield [px[(y0 + ty * 8 + y) * stride + x0 + tx * 8 + x]
                   for y in range(8) for x in range(8)]


# ── The shared palette ───────────────────────────────────────────────────────

TEXT = gb.shared_header()
PAL = gb.shared_palette(TEXT)
IDX = dict(gb.LOCAL_IDX)
IDX["IDX_TRAP_FRAME_DK"] = 145        # src/main.c's third local trap colour
for _name, _value in re.findall(r"#define\s+(IDX_[A-Z0-9_]+)\s+(\d+)", TEXT):
    IDX.setdefault(_name, int(_value))


def colour(name):
    return PAL[IDX[name]]


NAVY = colour("IDX_UI_DARK")
BLACK = (0, 0, 0)


# ── The static picture ───────────────────────────────────────────────────────

def rect_fill(d, x, y, w, h, c):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)


def rect_outline(d, x, y, w, h, c):
    d.rectangle([x, y, x + w - 1, y + h - 1], outline=c)


def draw_tab(d, x, active):
    """One tab, the PC-FX way: gold when active, black and dim when not."""
    rect_fill(d, x, TAB_Y, 102, TAB_H,
              colour("IDX_GOLD_DARK") if active else BLACK)
    rect_outline(d, x, TAB_Y, 102, TAB_H,
                 colour("IDX_GOLD_HI") if active else colour("IDX_DIM"))


def static_picture(tab):
    """draw_deck_editor()'s frame on 224 lines, with `tab` (0 DECK, 1
    STORAGE) the active one."""
    img = Image.new("RGB", (WIDTH, HEIGHT), BLACK)
    d = ImageDraw.Draw(img)
    # The margin outside the panel: a brown band, then navy stripes.
    rect_fill(d, 0, 0, WIDTH, 28, colour("IDX_DARK_BROWN"))
    for y in range(24, HEIGHT, 16):
        rect_fill(d, 0, max(y, 28), WIDTH, y + 8 - max(y, 28), NAVY)
    # draw_panel_rect(4, 4, W-8, H-8, IDX_UI_DARK).
    rect_fill(d, 4, 4, WIDTH - 8, HEIGHT - 8, NAVY)
    rect_outline(d, 4, 4, WIDTH - 8, HEIGHT - 8, colour("IDX_WHITE"))
    rect_outline(d, 5, 5, WIDTH - 10, HEIGHT - 10, colour("IDX_UI_LIGHT"))
    rect_outline(d, 6, 6, WIDTH - 12, HEIGHT - 12, colour("IDX_DIM"))
    draw_tab(d, 14, tab == 0)
    draw_tab(d, 140, tab == 1)
    # The info box: the PC-FX box is 36 lines for a 7-line font; two rows of
    # the 8-line scene font with four lines above and below is 24.
    rect_fill(d, 9, 176, WIDTH - 18, 24, BLACK)
    rect_outline(d, 9, 176, WIDTH - 18, 24, colour("IDX_UI_LIGHT"))
    return img


def picture_texels(img):
    return [pack(tuple(p)) for p in list(img.getdata())]


class TilePool:
    def __init__(self):
        self.tiles = [bytes(64)]         # 0: the blank (all transparent)
        self.index = {self.tiles[0]: 0}

    def add(self, px):
        t = tile8(px)
        if t not in self.index:
            self.index[t] = len(self.tiles)
            self.tiles.append(t)
        return self.index[t]


def build_static():
    pool = TilePool()
    maps = []
    for tab in range(2):
        tex = picture_texels(static_picture(tab))
        cells = []
        for ty in range(MAP_ROWS):
            for tx in range(32):
                block = [tex[(ty * 8 + y) * WIDTH + tx * 8 + x]
                         for y in range(8) for x in range(8)]
                cells.append(pool.add(block))
        maps.append(cells)
    assert len(pool.tiles) <= ICON_BASE_TILE, \
        "%d static tiles, past the icon base %d" % (len(pool.tiles), ICON_BASE_TILE)
    # Only the tab rows differ between the two maps.
    for i in range(MAP_ROWS * 32):
        if maps[0][i] != maps[1][i]:
            assert TAB_ROW0 <= i // 32 < TAB_ROW0 + TAB_ROWS, \
                "tab states differ outside the tab rows at cell %d" % i
    return pool, maps


def map_words(cells):
    out = bytearray()
    for c in cells:
        w = (BG1_PAL << 10) | c
        out += bytes((w & 0xFF, w >> 8))
    return bytes(out)


# ── The icons ────────────────────────────────────────────────────────────────

def face_pixels(raw):
    """A 38x54 indexed hand-card face as RGB triples."""
    return [PAL[v] for v in raw]


def trap_overlay(px):
    """draw_trap_frame_overlay() at the face's own size: the baked blue
    support frame recoloured violet, the sigil window left alone."""
    hi, dk, fr = (colour("IDX_TRAP_FRAME_HI"), colour("IDX_TRAP_FRAME_DK"),
                  colour("IDX_TRAP_FRAME"))
    img = Image.new("RGB", (FACE_W, FACE_H))
    img.putdata(px)
    d = ImageDraw.Draw(img)
    rect_outline(d, 1, 0, 36, 54, hi)
    rect_outline(d, 2, 2, 34, 50, dk)
    rect_outline(d, 3, 3, 32, 48, fr)
    rect_fill(d, 4, 4, 30, 4, fr)
    rect_fill(d, 4, 40, 30, 10, fr)
    rect_fill(d, 5, 41, 28, 8, dk)
    return list(img.getdata())


def scale_face(px):
    """draw_card_raw()'s nearest scaler: source = floor(dst * src / dst)."""
    out = []
    for y in range(ICON_H):
        sy = y * FACE_H // ICON_H
        for x in range(ICON_W):
            sx = x * FACE_W // ICON_W
            out.append(px[sy * FACE_W + sx])
    return out


def icon_block(face_rgb):
    """The 40x40 cell: navy, the drop shadow, the scaled face."""
    img = Image.new("RGB", (BLOCK, BLOCK), NAVY)
    d = ImageDraw.Draw(img)
    # draw_card_drop_shadow(): two columns right of the card from three lines
    # down, three rows under it from two columns in.
    rect_fill(d, ICON_X + ICON_W, ICON_Y + 3, 2, ICON_H, BLACK)
    rect_fill(d, ICON_X + 2, ICON_Y + ICON_H, ICON_W, 3, BLACK)
    icon = Image.new("RGB", (ICON_W, ICON_H))
    icon.putdata(scale_face(face_rgb))
    img.paste(icon, (ICON_X, ICON_Y))
    tex = picture_texels(img)
    tiles = bytearray()
    for block in cut(tex, BLOCK, 0, 0, BLOCK // 8, BLOCK // 8):
        tiles += tile8(block)
    assert len(tiles) == BLOCK_BYTES
    return bytes(tiles), tex


def icon_faces():
    with open(os.path.join(GENERATED, "card_faces.bin"), "rb") as fh:
        faces = fh.read()
    with open(os.path.join(GENERATED, "support_face.bin"), "rb") as fh:
        support = fh.read()
    assert len(faces) == MONSTERS * FACE_W * FACE_H, len(faces)
    assert len(support) == FACE_W * FACE_H, len(support)
    out = []
    for i in range(MONSTERS):
        out.append(face_pixels(faces[i * FACE_W * FACE_H:(i + 1) * FACE_W * FACE_H]))
    support_rgb = face_pixels(support)
    for kind in range(SUPPORTS):
        px = support_rgb
        if ga.face_kind("s:%d" % kind) == gb.KIND_TRAP:
            px = trap_overlay(px)
        out.append(px)
    return out


# ── Text palettes ────────────────────────────────────────────────────────────

def text_palette(ink):
    return [BLACK, ink, BLACK] + [BLACK] * 13


TEXT_INKS = [colour("IDX_WHITE"), colour("IDX_GOLD_HI"), colour("IDX_DIM"),
             colour("IDX_RED")]


# ── Output ───────────────────────────────────────────────────────────────────

def emit(name, bank, blobs):
    path = os.path.join(ASSETS, name + ".asm")
    total = sum(len(b[1]) for b in blobs)
    assert total <= 0x10000, "%s is %d bytes, past one bank" % (name, total)
    lines = [
        "; Generated by tools/snes/gen_snes_deck.py; do not edit.",
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


def preview(pool, maps, icons, path):
    """The editor as the PPU will show it: the DECK tab, eighteen icons."""
    out = Image.new("RGB", (WIDTH * 2, HEIGHT), NAVY)
    for tab in range(2):
        img = Image.new("RGB", (WIDTH, HEIGHT), NAVY)
        px = img.load()
        for ty in range(MAP_ROWS):
            for tx in range(32):
                tile = pool.tiles[maps[tab][ty * 32 + tx]]
                for y in range(8):
                    for x in range(8):
                        v = 0
                        for p in range(8):
                            byte = tile[(p >> 1) * 16 + y * 2 + (p & 1)]
                            v |= ((byte >> (7 - x)) & 1) << p
                        px[tx * 8 + x, ty * 8 + y] = unpack(v)
        for slot in range(GRID_COLS * GRID_ROWS):
            _, tex = icons[(slot + tab * 18) % len(icons)]
            bx = GRID_X0 + (slot % GRID_COLS) * BLOCK
            by = GRID_Y0 + (slot // GRID_COLS) * BLOCK
            for y in range(BLOCK):
                for x in range(BLOCK):
                    v = tex[y * BLOCK + x]
                    on_ring = (CURSOR_X0 <= x <= CURSOR_X1 and
                               (x < CURSOR_X0 + 2 or x > CURSOR_X1 - 2 or
                                y < 2 or y >= BLOCK - 2))
                    if slot == 0 and on_ring:
                        v = pack(colour("IDX_RED"))
                    px[bx + x, by + y] = unpack(v)
        out.paste(img, (tab * WIDTH, 0))
    out.save(path)


def write_header(static_tiles):
    with open(HEADER, "w") as fh:
        fh.write("/* Generated by tools/snes/gen_snes_deck.py; do not edit. */\n"
                 "#ifndef WAIFU_SNES_DECK_DATA_H\n"
                 "#define WAIFU_SNES_DECK_DATA_H\n\n"
                 '#include "snes_types.h"\n\n'
                 "/* The static picture: a pool of 8bpp direct-colour tiles (0 is the\n"
                 " * blank), a 32x%d map with the DECK tab active, and the two-row\n"
                 " * strip for either tab state. */\n"
                 "#define SNES_DECK_STATIC_TILES   %d\n"
                 "#define SNES_DECK_STATIC_BYTES   %d\n"
                 "#define SNES_DECK_MAP_ROWS       %d\n"
                 "#define SNES_DECK_MAP_BYTES      %d\n"
                 "#define SNES_DECK_TAB_ROW0       %d\n"
                 "#define SNES_DECK_TAB_ROWS       %d\n"
                 "#define SNES_DECK_TAB_STRIP_BYTES %d\n"
                 "#define SNES_DECK_BG1_PAL_BITS   0x%04Xu\n"
                 "#define SNES_DECK_NAVY_BGR555    0x%04Xu\n"
                 "/* The icon blocks: 40x40 cells of 25 tiles, forty a bank. */\n"
                 "#define SNES_DECK_ICON_BASE_TILE %d\n"
                 "#define SNES_DECK_ICON_TILES     %d\n"
                 "#define SNES_DECK_ICON_BYTES     %d\n"
                 "#define SNES_DECK_ICON_SIDE      %d\n"
                 "#define SNES_DECK_ICONS          %d\n"
                 "#define SNES_DECK_ICONS_PER_BANK %d\n"
                 "#define SNES_DECK_ICON_BANK0     0x%02X\n"
                 "#define SNES_DECK_CURSOR_BYTE    0x%02Xu\n"
                 "#define SNES_DECK_CURSOR_X0      %d\n"
                 "#define SNES_DECK_CURSOR_X1      %d\n"
                 "/* The gallery: block (0, 0) at (%d, %d), one block per 40 px. */\n"
                 "#define SNES_DECK_GRID_COLS      %d\n"
                 "#define SNES_DECK_GRID_ROWS      %d\n"
                 "#define SNES_DECK_GRID_COL0      %d\n"
                 "#define SNES_DECK_GRID_ROW0      %d\n"
                 "/* BG2 text palettes 0..3: white, gold, dim, red. */\n"
                 "#define SNES_DECK_TEXT_PALS      4\n"
                 "#define SNES_DECK_TEXT_PAL_BYTES 128\n\n"
                 "extern const u8 snes_deck_bg1_tiles[];\n"
                 "extern const u8 snes_deck_bg1_map[];\n"
                 "extern const u8 snes_deck_bg1_tabs[];\n"
                 "extern const u8 snes_deck_text_pal[];\n"
                 "extern const u8 snes_deckicons_4[];\n"
                 "extern const u8 snes_deckicons_5[];\n\n"
                 "#define SNES_DECK_ICON_BANK_SWITCH(bank, offset, EXPR) \\\n"
                 "    switch (bank) { \\\n"
                 "    case 0: EXPR(snes_deckicons_4, offset); break; \\\n"
                 "    case 1: EXPR(snes_deckicons_5, offset); break; \\\n"
                 "    default: break; }\n\n"
                 "#endif /* WAIFU_SNES_DECK_DATA_H */\n"
                 % (MAP_ROWS, static_tiles, static_tiles * 64, MAP_ROWS,
                    MAP_ROWS * 64, TAB_ROW0, TAB_ROWS, TAB_ROWS * 64,
                    BG1_PAL << 10, NAVY_BGR555,
                    ICON_BASE_TILE, BLOCK_TILES, BLOCK_BYTES, BLOCK, ICONS,
                    ICONS_PER_BANK, 0xC0 + ICON_BANKS[0], pack(colour("IDX_RED")),
                    CURSOR_X0, CURSOR_X1,
                    GRID_X0, GRID_Y0, GRID_COLS, GRID_ROWS,
                    GRID_X0 // 8, GRID_Y0 // 8))


def main():
    os.makedirs(ASSETS, exist_ok=True)
    pool, maps = build_static()
    strips = b"".join(map_words(m[TAB_ROW0 * 32:(TAB_ROW0 + TAB_ROWS) * 32])
                      for m in maps)
    pal = b"".join(gb.palette_bytes(text_palette(ink)) for ink in TEXT_INKS)
    emit("snes_deckui", UI_BANK, [
        ("snes_deck_bg1_tiles", b"".join(pool.tiles)),
        ("snes_deck_bg1_map", map_words(maps[0])),
        ("snes_deck_bg1_tabs", strips),
        ("snes_deck_text_pal", pal),
    ])

    icons = [icon_block(px) for px in icon_faces()]
    assert len(icons) == ICONS
    for b, bank in enumerate(ICON_BANKS):
        chunk = icons[b * ICONS_PER_BANK:(b + 1) * ICONS_PER_BANK]
        emit("snes_deckicons_%d" % bank, bank,
             [("snes_deckicons_%d" % bank, b"".join(t for t, _ in chunk))])
    preview(pool, maps, icons, os.path.join(ASSETS, "deck_preview.png"))
    write_header(len(pool.tiles))
    print("snes deck editor: %d static tiles, %d icons of %d bytes" %
          (len(pool.tiles), len(icons), BLOCK_BYTES))


if __name__ == "__main__":
    main()
