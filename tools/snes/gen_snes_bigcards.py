#!/usr/bin/env python3
"""The Mode 3 card presentation: every card as a 120x160 PC-FX battle card.

The duel's card check and its battle cut-in are Mode 3 screens, and Mode 3's
BG1 is an 8bpp background -- so the card shown there is the same object the
PC-FX and FM TOWNS builds draw: the 112x112 painting inside the gold-rimmed
120x160 frame of draw_big_battle_card_stats() in src/main.c, in eighty colours
of its own instead of the fifteen an OBJ palette holds.

What is baked, per card, is the TOP 120x120 of that frame: the rim, the gold
rules, the painting and the rule under it.  The bottom forty rows -- the
ATK/DEF plate and the frame's foot -- are the same for every monster, and the
same again for every spell and every trap, so they are one shared tile set with
three small maps.  Per card that is 225 tiles (14,400 bytes) and a 160-byte
palette; four cards fill a 64 KB HiROM bank and seventy-nine cards take
twenty banks.

TWO CARDS SHARE ONE CGRAM.  The battle shows the attacker and the defender at
once, and an 8bpp background reads all 256 entries, so the palette is split:

    0..15    the text and frame colours (BG2 palette 0)
    16..31   the gold text palette (BG2 palette 1)
    32..111  card slot 0's eighty colours
    128..143 the frame colours again
    144..159 unused
    160..239 card slot 1's eighty colours
    240..255 OBJ palette 7, the HUD font

Every card's tiles are written with slot 0's indices.  Slot 1 is the same
tiles with bitplane 7 set, which the runtime does with eight fixed-source DMAs
into the odd bytes of each tile's last plane pair (see snes_cardart.c), so
nothing is stored twice.  The card colours stop at 111 so that OR 0x80 never
reaches the HUD's sixteen.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "atarist"))

from PIL import Image, ImageDraw, ImageOps  # noqa: E402
import gen_atarist_assets as ga  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(ROOT, "src", "snes", "assets")
SHARED_HEADER = os.path.join(ROOT, "src", "generated", "waifu_assets.h")
HEADER = os.path.join(ROOT, "src", "snes", "snes_bigcard_data.h")

CARD_W, CARD_H = 120, 160
TOP_H = 120
ART = 112
BOTTOM_ROWS = (CARD_H - TOP_H) // 8
TILES_X = CARD_W // 8
TOP_TILES = TILES_X * (TOP_H // 8)
CARD_BYTES = TOP_TILES * 64
CARD_COLOURS = 80
CARD_FIRST = 32
CARD_PAL_BYTES = CARD_COLOURS * 2
CARD_RECORD = CARD_BYTES + CARD_PAL_BYTES
BACK_FEET_BYTES = TILES_X * BOTTOM_ROWS * 64   # the back's own bottom rows
CARDS_PER_BANK = 4
FIRST_BANK = 30                 # $DE..; the OBJ sheets end at $CC
COMMON_BANK = 50                # the frame feet and the card info table

# The sixteen shared entries: what BG2 text and the baked frame draw with.
# Names on the left are the src/main.c palette indices the PC-FX draws the
# same frame from; the RGB comes out of the generated header so the two
# cannot drift apart.
FRAME_NAMES = [
    None,               # 0: transparent
    "IDX_WHITE",        # 1: text ink
    "IDX_BLACK",        # 2: text outline, black
    "IDX_GOLD_HI",      # 3
    "IDX_GOLD_DARK",    # 4
    "IDX_CARD_GOLD",    # 5
    "IDX_CARD_RIM",     # 6
    "IDX_DARK_BROWN",   # 7
    "IDX_UI_BLUE",      # 8
    "IDX_BLUE_WHITE",   # 9
    "IDX_TRAP_FRAME",   # 10
    "IDX_TRAP_FRAME_HI",  # 11
    "IDX_UI_DARK",      # 12: the card check's panel
    "IDX_DIM",          # 13
    "IDX_UI_LIGHT",     # 14
    "IDX_WHITE",        # 15
]
# src/main.c defines the trap frame locally rather than in the header.
LOCAL_IDX = {"IDX_TRAP_FRAME": 137, "IDX_TRAP_FRAME_HI": 47}

KIND_MONSTER, KIND_SPELL, KIND_TRAP = 0, 1, 2

# The card info table: fixed-stride records the card check prints.
INFO_NAME_LEN = 32
INFO_KIND_LEN = 24
INFO_DESC_LEN = 96
INFO_RECORD = INFO_NAME_LEN + INFO_KIND_LEN + INFO_DESC_LEN + 4
SUPPORT_NAMES = ("BRONZE EQUIP", "DESERT GUARD", "ANCIENT DRAW",
                 "OASIS LIGHT", "THUNDER", "MIRROR VEIL")
SUPPORT_TYPES = ("EQUIP", "SPELL", "SPELL", "SPELL", "SPELL", "TRAP")
SUPPORT_EFFECTS = (
    "EQUIP: ONE MONSTER GAINS 500 ATK AND 300 DEF.",
    "EQUIP: ONE MONSTER GAINS 250 ATK AND 800 DEF.",
    "SUPPORT: DRAW 1 CARD FROM YOUR DECK.",
    "SUPPORT: RESTORE 1000 LIFE POINTS.",
    "SUPPORT: DESTROY EVERY MONSTER ON THE OPPONENT FIELD.",
    "TRAP: DESTROY THE NEXT ATTACKER AND CANCEL ITS ATTACK.",
)


def snes_colour(rgb):
    r, g, b = (v >> 3 for v in rgb)
    return (b << 10) | (g << 5) | r


def palette_bytes(colours):
    out = bytearray()
    for c in colours:
        v = snes_colour(c)
        out += bytes((v & 0xFF, v >> 8))
    return bytes(out)


def tile8(indices):
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
    for ty in range(th):
        for tx in range(tw):
            block = []
            for y in range(8):
                row = (y0 + ty * 8 + y) * w + x0 + tx * 8
                block += indices[row:row + 8]
            yield block


# ── The shared palette ───────────────────────────────────────────────────────

def shared_header():
    return open(SHARED_HEADER, encoding="utf-8", errors="replace").read()


def shared_palette(text):
    m = re.search(r"waifu_palette_rgb\[\]\s*=\s*\{(.*?)\};", text, re.S)
    values = [int(v) for v in re.findall(r"\d+", m.group(1))]
    return [tuple(values[i:i + 3]) for i in range(0, 768, 3)]


def frame_colours():
    text = shared_header()
    pal = shared_palette(text)
    idx = dict(LOCAL_IDX)
    for name, value in re.findall(r"#define\s+(IDX_[A-Z0-9_]+)\s+(\d+)", text):
        idx.setdefault(name, int(value))
    out = []
    for name in FRAME_NAMES:
        out.append((0, 0, 0) if name is None else pal[idx[name]])
    return out


FRAME = frame_colours()
TRANSPARENT, WHITE, BLACK, GOLD_HI, GOLD_DARK, CARD_GOLD, CARD_RIM, \
    DARK_BROWN, UI_BLUE, BLUE_WHITE, TRAP_FRAME, TRAP_FRAME_HI, UI_DARK = \
    range(13)
GOLD_TEXT_PALETTE = [(0, 0, 0), FRAME[GOLD_HI], FRAME[BLACK]] + \
    [(0, 0, 0)] * 13


# ── The frame ────────────────────────────────────────────────────────────────

def frame_indices(kind):
    """The whole 120x160 frame as shared-palette indices, art window left 0.

    Line for line the non-HW3D branch of draw_big_battle_card_stats(): rim,
    two gold rules, gold fill, the rule round the painting, and for a monster
    the dark plate the figures are printed on."""
    img = Image.new("P", (CARD_W, CARD_H), TRANSPARENT)
    d = ImageDraw.Draw(img)
    w, h = CARD_W, CARD_H
    d.rectangle([0, 0, w - 1, h - 1], fill=CARD_RIM)
    d.rectangle([0, 0, w - 1, h - 1], outline=GOLD_HI)
    d.rectangle([1, 1, w - 2, h - 2], outline=GOLD_DARK)
    d.rectangle([4, 4, w - 5, h - 5], fill=CARD_GOLD)
    if kind == KIND_MONSTER:
        d.rectangle([3, 5, 3 + 113, 5 + 113], outline=CARD_RIM)
        d.rectangle([5, 123, 5 + 109, 123 + 27], fill=DARK_BROWN,
                    outline=GOLD_DARK)
    else:
        trap = kind == KIND_TRAP
        d.rectangle([0, 0, w - 1, h - 1],
                    outline=TRAP_FRAME_HI if trap else BLUE_WHITE)
        d.rectangle([1, 1, w - 2, h - 2],
                    outline=TRAP_FRAME if trap else UI_BLUE)
        d.rectangle([3, 5, 3 + 113, 5 + 113],
                    outline=TRAP_FRAME if trap else UI_BLUE)
    return list(img.getdata())


# ── The paintings ────────────────────────────────────────────────────────────

def face_list():
    out = []
    for asset_id in ga.parse_cards():
        out.append(("m:" + asset_id, ga.find_card_image(asset_id)))
    for kind in range(ga.SUPPORT_VARIANTS):
        out.append(("s:%d" % kind, None))
    out.append(("back", None))
    return out


def face_kind(tag):
    if tag.startswith("m:") or tag == "back":
        return KIND_MONSTER if tag != "back" else KIND_SPELL
    return ga.face_kind(tag)


def big_art(tag, path):
    """The 112x112 painting, cropped the way gen_assets.py's cover() does for
    the PC-FX: the content box, filled rather than padded, weighted a little
    above centre so a full-length figure keeps her face."""
    if tag.startswith("m:"):
        with Image.open(path) as src:
            box = ga.card_content_bbox(src)
            img = src.crop(box).convert("RGB")
        return ImageOps.fit(img, (ART, ART), method=Image.Resampling.LANCZOS,
                            centering=(0.5, 0.45))
    return ga.face_art(tag, None, (ART, ART)).convert("RGB")


def back_record():
    """THE BACK IS ITS OWN FRAME, so it is not put inside the gold one: the
    cover painting is fitted to the whole 120x160 and quantised as one
    picture.  Its top fifteen tile rows are a record like any other; its
    bottom five -- the seventy-five tiles the shared feet cannot hold in
    its colours -- follow the palette in the same bank slot, and the runtime
    puts them at their own VRAM tiles (snes_cardart.c BACK_FEET_TILE)."""
    art = ga.card_back_art((CARD_W, CARD_H))
    indexed = art.quantize(colors=CARD_COLOURS, method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.FLOYDSTEINBERG)
    raw = indexed.getpalette()[:CARD_COLOURS * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours += [(0, 0, 0)] * (CARD_COLOURS - len(colours))
    frame = [v + CARD_FIRST for v in indexed.getdata()]
    tiles = bytearray()
    for block in cut_tiles(frame, CARD_W, 0, 0, TILES_X, TOP_H // 8):
        tiles += tile8(block)
    assert len(tiles) == CARD_BYTES
    feet = bytearray()
    for block in cut_tiles(frame, CARD_W, 0, TOP_H, TILES_X, BOTTOM_ROWS):
        feet += tile8(block)
    assert len(feet) == BACK_FEET_BYTES
    return bytes(tiles) + palette_bytes(colours) + bytes(feet), frame, colours


def card_record(tag, path):
    if tag == "back":
        return back_record()
    art = big_art(tag, path)
    indexed = art.quantize(colors=CARD_COLOURS, method=Image.Quantize.MEDIANCUT,
                           dither=Image.Dither.FLOYDSTEINBERG)
    raw = indexed.getpalette()[:CARD_COLOURS * 3]
    colours = [tuple(raw[i:i + 3]) for i in range(0, len(raw), 3)]
    colours += [(0, 0, 0)] * (CARD_COLOURS - len(colours))
    art_idx = list(indexed.getdata())

    frame = frame_indices(face_kind(tag))
    for y in range(ART):
        for x in range(ART):
            frame[(6 + y) * CARD_W + 4 + x] = art_idx[y * ART + x] + CARD_FIRST
    tiles = bytearray()
    for block in cut_tiles(frame, CARD_W, 0, 0, TILES_X, TOP_H // 8):
        tiles += tile8(block)
    assert len(tiles) == CARD_BYTES
    return bytes(tiles) + palette_bytes(colours), frame, colours


def feet():
    """The bottom forty rows of the three frame kinds, deduplicated."""
    tiles = []
    maps = []
    for kind in (KIND_MONSTER, KIND_SPELL, KIND_TRAP):
        frame = frame_indices(kind)
        m = bytearray()
        for block in cut_tiles(frame, CARD_W, 0, TOP_H, TILES_X, BOTTOM_ROWS):
            t = tile8(block)
            if t not in tiles:
                tiles.append(t)
            m += bytes((tiles.index(t), 0))
        maps.append(bytes(m))
    return b"".join(tiles), maps


# ── The card info table ──────────────────────────────────────────────────────

def card_info():
    text = shared_header()

    def strings(name):
        m = re.search(r"%s\[WAIFU_CARD_COUNT\]\s*=\s*\{(.*?)\n\};" % name,
                      text, re.S)
        return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))

    def ints(name):
        m = re.search(r"%s\[WAIFU_CARD_COUNT\]\s*=\s*\{(.*?)\};" % name,
                      text, re.S)
        return [int(v) for v in re.findall(r"\d+", m.group(1))]

    names = strings("waifu_card_names")
    attrs = strings("waifu_card_attr")
    tribes = strings("waifu_card_tribe")
    descs = strings("waifu_card_desc")
    atks = ints("waifu_card_atk")
    defs = ints("waifu_card_def")

    def field(s, n):
        s = s.upper().encode("ascii", "replace")[:n - 1]
        return s + bytes(n - len(s))

    blob = bytearray()
    for i in range(len(names)):
        power = (atks[i] + defs[i]) // 2
        stars = max(1, min(8, 2 + power // 500))
        blob += field(names[i], INFO_NAME_LEN)
        blob += field(attrs[i] + " / " + tribes[i], INFO_KIND_LEN)
        blob += field(descs[i], INFO_DESC_LEN)
        blob += bytes((stars, KIND_MONSTER, 0, 0))
    for i in range(ga.SUPPORT_VARIANTS):
        blob += field(SUPPORT_NAMES[i], INFO_NAME_LEN)
        blob += field(SUPPORT_TYPES[i], INFO_KIND_LEN)
        blob += field(SUPPORT_EFFECTS[i], INFO_DESC_LEN)
        blob += bytes((0, ga.face_kind("s:%d" % i), 0, 0))
    blob += field("FACE DOWN", INFO_NAME_LEN)
    blob += field("UNKNOWN", INFO_KIND_LEN)
    blob += field("A SET CARD.  ITS FACE IS HIDDEN UNTIL IT IS REVEALED.",
                  INFO_DESC_LEN)
    blob += bytes((0, KIND_SPELL, 0, 0))
    return bytes(blob)


# ── Emit ─────────────────────────────────────────────────────────────────────

def emit(name, bank, blobs):
    path = os.path.join(ASSETS, name + ".asm")
    total = sum(len(b[1]) for b in blobs)
    assert total <= 0x10000, "%s is %d bytes, past one bank" % (name, total)
    lines = [
        "; Generated by tools/snes/gen_snes_bigcards.py; do not edit.",
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


def preview(records, feet_tiles, feet_maps, path):
    """A contact sheet decoded the way the PPU will show it."""
    cols = 8
    rows = (len(records) + cols - 1) // cols
    out = Image.new("RGB", (cols * (CARD_W + 4), rows * (CARD_H + 4)), (24, 24, 24))
    for i, (_, frame, colours) in enumerate(records):
        pal = FRAME + GOLD_TEXT_PALETTE + colours
        img = Image.new("RGB", (CARD_W, CARD_H))
        px = img.load()
        for y in range(CARD_H):
            for x in range(CARD_W):
                v = frame[y * CARD_W + x]
                c = pal[v] if v < len(pal) else (255, 0, 255)
                px[x, y] = tuple(((ch >> 3) << 3) | (ch >> 5) for ch in c)
        out.paste(img, ((i % cols) * (CARD_W + 4), (i // cols) * (CARD_H + 4)))
    out.save(path)


def main():
    os.makedirs(ASSETS, exist_ok=True)
    faces = face_list()
    assert len(faces) == 79, "expected 79 faces, got %d" % len(faces)
    records = [card_record(tag, path) for tag, path in faces]
    banks = (len(records) + CARDS_PER_BANK - 1) // CARDS_PER_BANK
    for b in range(banks):
        chunk = records[b * CARDS_PER_BANK:(b + 1) * CARDS_PER_BANK]
        emit("snes_bigcards_%d" % (FIRST_BANK + b), FIRST_BANK + b,
             [("snes_bigcards_%d" % (FIRST_BANK + b),
               b"".join(r[0] for r in chunk))])
    feet_tiles, feet_maps = feet()
    info = card_info()
    emit("snes_bigcard_common", COMMON_BANK, [
        ("snes_bigcard_feet", feet_tiles),
        ("snes_bigcard_feet_map", b"".join(feet_maps)),
        ("snes_bigcard_frame_pal",
         palette_bytes(FRAME) + palette_bytes(GOLD_TEXT_PALETTE)),
        ("snes_card_info", info),
    ])
    preview(records, feet_tiles, feet_maps,
            os.path.join(ASSETS, "bigcards_preview.png"))

    with open(HEADER, "w") as fh:
        fh.write("/* Generated by tools/snes/gen_snes_bigcards.py; do not edit. */\n"
                 "#ifndef WAIFU_SNES_BIGCARD_DATA_H\n"
                 "#define WAIFU_SNES_BIGCARD_DATA_H\n\n"
                 '#include "snes_types.h"\n\n'
                 "/* One card: the top 120x120 of the PC-FX battle frame as 225\n"
                 " * 8bpp tiles, then its eighty-colour palette. */\n"
                 "#define SNES_BIGCARD_W          %d\n"
                 "#define SNES_BIGCARD_H          %d\n"
                 "#define SNES_BIGCARD_TILES_X    %d\n"
                 "#define SNES_BIGCARD_TOP_ROWS   %d\n"
                 "#define SNES_BIGCARD_FOOT_ROWS  %d\n"
                 "#define SNES_BIGCARD_TILES      %d\n"
                 "#define SNES_BIGCARD_BYTES      %d\n"
                 "#define SNES_BIGCARD_COLOURS    %d\n"
                 "#define SNES_BIGCARD_FIRST      %d\n"
                 "#define SNES_BIGCARD_PAL_BYTES  %d\n"
                 "#define SNES_BIGCARD_RECORD     %d\n"
                 "#define SNES_BIGCARD_PER_BANK   %d\n"
                 "#define SNES_BIGCARD_FIRST_BANK %d\n"
                 "#define SNES_BIGCARD_BANKS      %d\n"
                 "#define SNES_BIGCARD_FEET_TILES %d\n"
                 "/* The back's own bottom rows, after its record's palette. */\n"
                 "#define SNES_BIGCARD_BACK_FEET_BYTES %d\n"
                 "#define SNES_BIGCARD_KIND_MONSTER 0\n"
                 "#define SNES_BIGCARD_KIND_SPELL   1\n"
                 "#define SNES_BIGCARD_KIND_TRAP    2\n"
                 "/* The card info table the check screen prints. */\n"
                 "#define SNES_CARD_INFO_NAME_LEN %d\n"
                 "#define SNES_CARD_INFO_KIND_LEN %d\n"
                 "#define SNES_CARD_INFO_DESC_LEN %d\n"
                 "#define SNES_CARD_INFO_RECORD   %d\n"
                 "#define snesCardInfoName(f) ((const char *)&snes_card_info[(u16)(f) * SNES_CARD_INFO_RECORD])\n"
                 "#define snesCardInfoKind(f) ((const char *)&snes_card_info[(u16)(f) * SNES_CARD_INFO_RECORD + SNES_CARD_INFO_NAME_LEN])\n"
                 "#define snesCardInfoDesc(f) ((const char *)&snes_card_info[(u16)(f) * SNES_CARD_INFO_RECORD + SNES_CARD_INFO_NAME_LEN + SNES_CARD_INFO_KIND_LEN])\n"
                 "#define snesCardInfoStars(f) (snes_card_info[(u16)(f) * SNES_CARD_INFO_RECORD + SNES_CARD_INFO_NAME_LEN + SNES_CARD_INFO_KIND_LEN + SNES_CARD_INFO_DESC_LEN])\n"
                 "#define snesCardInfoFrame(f) (snes_card_info[(u16)(f) * SNES_CARD_INFO_RECORD + SNES_CARD_INFO_NAME_LEN + SNES_CARD_INFO_KIND_LEN + SNES_CARD_INFO_DESC_LEN + 1])\n\n"
                 % (CARD_W, CARD_H, TILES_X, TOP_H // 8, BOTTOM_ROWS, TOP_TILES,
                    CARD_BYTES, CARD_COLOURS, CARD_FIRST, CARD_PAL_BYTES,
                    CARD_RECORD, CARDS_PER_BANK, FIRST_BANK, banks,
                    len(feet_tiles) // 64, BACK_FEET_BYTES,
                    INFO_NAME_LEN, INFO_KIND_LEN, INFO_DESC_LEN, INFO_RECORD))
        for b in range(banks):
            fh.write("extern const u8 snes_bigcards_%d[];\n" % (FIRST_BANK + b))
        fh.write("extern const u8 snes_bigcard_feet[];\n"
                 "extern const u8 snes_bigcard_feet_map[];\n"
                 "extern const u8 snes_bigcard_frame_pal[];\n"
                 "extern const u8 snes_card_info[];\n\n"
                 "/* One case per bank: a far ROM symbol must stay in the DMA\n"
                 " * expression, or 816-tcc keeps the offset and loses the bank. */\n"
                 "#define SNES_BIGCARD_BANK_SWITCH(bank, offset, EXPR) \\\n"
                 "    switch (bank) { \\\n")
        for b in range(banks):
            fh.write("    case %d: EXPR(snes_bigcards_%d, offset); break; \\\n"
                     % (b, FIRST_BANK + b))
        fh.write("    default: break; }\n\n#endif\n")
    print("bigcards: %d cards in %d banks ($%02X..), %d feet tiles, info %d bytes"
          % (len(records), banks, 0xC0 + FIRST_BANK, len(feet_tiles) // 64,
             len(info)))


if __name__ == "__main__":
    main()
