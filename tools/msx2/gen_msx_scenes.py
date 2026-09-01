#!/usr/bin/env python3
"""Bake every MSX2 cartridge asset: full-screen scenes, card art, and text.

The MSX2 port shows the same pictures the other targets do -- they come from
`assets/source/`, not from anything drawn at runtime (MSX2_PORT_PLAN.md §14).
A GRAPHIC 7 screen is 256x212 bytes of GRB332, so one scene is 54,272 bytes:
far too much to link into the 32 KB the Z80 can address, which is why every
asset here is packed into whole 16 KB NEO cartridge segments and streamed (§5).

This is deliberately ONE generator rather than three.  Scenes, card textures
and the string table share a single cartridge segment map, and two tools each
advancing their own cursor is exactly how a ROM ends up with its card art
written over its title screen.

Outputs:
  src/msx2/assets/<scene>.bin   raw GRB332, row-major, no header
  src/msx2/assets/<scene>.png   a preview decoded back from that binary
  src/msx2/assets/cards.bin     79 card textures at a 2 KB stride
  src/msx2/assets/cards.png     a contact sheet of them, same decode path
  src/msx2/assets/text.bin      the string table (card names)
  src/generated/msx2_scenes.h   where all of it lives in the cartridge

tools/msx2/pack_msx_rom.py writes the binaries into the ROM image at exactly
those segments.

Usage:
    tools/msx2/gen_msx_scenes.py [--quiet]
"""

import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msx2_grb332 as grb  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(ROOT, "src", "msx2", "assets")
HEADER = os.path.join(ROOT, "src", "generated", "msx2_scenes.h")
CARD_DIR = os.path.join(ROOT, "assets", "source", "cards")
CARD_DATA = os.path.join(CARD_DIR, "card_data.txt")
BG_DIR = os.path.join(ROOT, "assets", "source", "bg")

WIDTH = 256
HEIGHT = 212
SEGMENT_BYTES = 16 * 1024

# Segments 0 and 1 are the resident code the cartridge boots into, and segment 2
# is the page-0 code bank (the ISR plus the scene code that must stay mapped
# while the 0x8000 window holds picture data).  Asset data starts clear of all
# of them; the packer asserts it never lands on top of code.
FIRST_ASSET_SEGMENT = 4

# ── The duel board ───────────────────────────────────────────────────────────
#
# These numbers are the contract between the baked backdrop and msx2_board.c:
# the plinths below are drawn at exactly the coordinates the renderer blits
# cards to, so a card lands in its recess and not next to it.  They are emitted
# into the generated header so there is one definition, not two.
CARD_W, CARD_H = 40, 48
SLOTS = 5
SLOT_X0 = 12
SLOT_PITCH = 48
ROW_COM_Y = 16
ROW_PLAYER_Y = 70
ROW_HAND_Y = 126
HUD_H = 13
INFO_Y = 178

# A flat ring of a known colour is baked around every slot, and the selection
# cursor is drawn *into that ring* rather than over the card.  That is what
# makes moving the cursor four VDP fills instead of two card re-blits: erasing
# it is a fill of RING_RGB, and no artwork underneath ever has to be restored.
RING = 2
RING_RGB = (72, 40, 8)
PANEL_RGB = (10, 8, 14)

# 72 monsters + 6 support variants + 1 card back.
SUPPORT_VARIANTS = 6
NAME_STRIDE = 24
CARD_STRIDE = 2048   # 8 cards per segment, so no card ever straddles one


def card_ident(asset_id):
    """The C identifier suffix gen_assets.py's card_macro() produces."""
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", asset_id).upper()
    return re.sub(r"[^A-Z0-9]+", "_", s).strip("_")


def parse_cards():
    """(asset_id, display_name) per monster, in card-id order."""
    cards = []
    with open(CARD_DATA) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if parts[0] == "card":
                cards.append((parts[1], parts[2]))
    return cards


SUPPORT_NAMES = [
    "BRONZE EQUIP",
    "DESERT GUARD",
    "ANCIENT DRAW",
    "OASIS LIGHT",
    "THUNDER",
    "MIRROR VEIL",
]

# Support cards are one frame in six colourways, so the kind is readable at a
# glance on a 40-pixel card where the name never fits.
SUPPORT_TINTS = [
    ((60, 150, 208), (14, 40, 66), (38, 108, 164)),    # equip  - arcane blue
    ((88, 176, 120), (12, 46, 30), (44, 118, 76)),     # guard  - jade
    ((150, 140, 216), (28, 24, 60), (96, 88, 160)),    # draw   - violet
    ((236, 196, 96), (58, 40, 12), (176, 138, 48)),    # heal   - amber
    ((236, 128, 72), (58, 20, 10), (176, 76, 34)),     # storm  - ember
    ((196, 84, 108), (52, 14, 26), (140, 46, 68)),     # trap   - garnet
]


# ── Card art ─────────────────────────────────────────────────────────────────
#
# The crop rules below are lifted from tools/gen_assets.py (card_content_bbox /
# prepared_card_source / card_thumb_crop) so an MSX2 card frames its monster the
# same way every other target does.  They are duplicated rather than imported
# because gen_assets.py does its work at module scope: importing it would
# regenerate the 5.3 MB shared asset header as a side effect.

def find_card_image(asset_id):
    for ext in (".png", ".webp", ".jpg", ".jpeg"):
        path = os.path.join(CARD_DIR, asset_id + ext)
        if os.path.exists(path):
            return path
    raise FileNotFoundError(asset_id)


def card_content_bbox(img):
    rgba = img.convert("RGBA")
    alpha = rgba.getchannel("A")
    if alpha.getextrema()[0] < 255:
        bbox = alpha.point(lambda p: 255 if p > 8 else 0).getbbox()
        if bbox:
            return bbox
    rgb = img.convert("RGB")
    bg = Image.new("RGB", rgb.size, rgb.getpixel((0, 0)))
    diff = ImageChops.difference(rgb, bg).convert("L")
    bbox = diff.point(lambda p: 255 if p > 8 else 0).getbbox()
    if bbox:
        l, t, r, b = bbox
        mx = max(1, (r - l) // 30)
        my = max(1, (b - t) // 30)
        return (max(0, l - mx), max(0, t - my),
                min(img.width, r + mx), min(img.height, b + my))
    return (0, 0, img.width, img.height)


def card_thumb(img, size):
    src = img.crop(card_content_bbox(img)).convert("RGB")
    return ImageOps.fit(src, size, method=Image.Resampling.BILINEAR,
                        centering=(0.5, 0.36))


ART_X, ART_Y, ART_W, ART_H = 4, 9, 32, 30


def card_frame(outer, inner, mid, strip):
    """The bevelled frame every card shares, at MSX2 board size."""
    img = Image.new("RGB", (CARD_W, CARD_H), inner)
    d = ImageDraw.Draw(img)
    d.rectangle([1, 0, CARD_W - 2, CARD_H - 1], fill=outer)
    d.rectangle([2, 2, CARD_W - 3, CARD_H - 3], fill=inner)
    d.rectangle([3, 3, CARD_W - 4, CARD_H - 4], fill=mid)
    d.rectangle([4, 4, CARD_W - 5, 7], fill=strip)          # type strip
    return img


def card_footer(img, band, well, marks, pips, pip_color):
    """The stat band and the level pips.  The real numbers live in the info
    panel at the bottom of the duel screen -- at 40 pixels wide a legible ATK
    figure would eat the art window, and the panel is where a player reads it
    anyway."""
    d = ImageDraw.Draw(img)
    d.rectangle([4, 40, CARD_W - 5, 44], fill=band)
    d.rectangle([5, 41, CARD_W - 6, 43], fill=well)
    d.line([7, 42, 16, 42], fill=marks)
    d.line([22, 42, 32, 42], fill=marks)
    for s in range(pips):
        x = 5 + s * 4
        d.ellipse([x, 5, x + 1, 6], fill=pip_color)


def draw_monster_card(asset_id, atk, deff):
    img = card_frame((213, 156, 48), (72, 42, 16), (192, 132, 39), (228, 181, 63))
    art = card_thumb(Image.open(find_card_image(asset_id)), (ART_W, ART_H))
    art = art.filter(ImageFilter.SHARPEN)
    img.paste(art, (ART_X, ART_Y))
    ImageDraw.Draw(img).rectangle(
        [ART_X, ART_Y, ART_X + ART_W - 1, ART_Y + ART_H - 1], outline=(32, 19, 8))
    stars = max(1, min(8, (atk + deff) // 700))
    card_footer(img, (230, 190, 96), (54, 42, 28), (236, 220, 150),
                stars, (170, 24, 18))
    return img


def draw_support_emblem(kind, size):
    """A sigil per support kind, so the six colourways are not the only cue."""
    em = Image.new("RGB", (size, size), SUPPORT_TINTS[kind][1])
    d = ImageDraw.Draw(em)
    cx, cy = (size - 1) / 2.0, (size - 1) / 2.0
    bright, _, mid = SUPPORT_TINTS[kind]
    if kind in (0, 1):                                  # equip / guard: a shield
        d.polygon([(cx, 2), (size - 4, 7), (size - 6, size - 6), (cx, size - 3),
                   (5, size - 6), (3, 7)], fill=mid, outline=bright)
        d.line([cx, 6, cx, size - 7], fill=bright, width=2)
    elif kind == 2:                                     # draw: stacked cards
        for k, off in enumerate((6, 3, 0)):
            d.rectangle([5 + off, 6 + off, size - 9 + off, size - 6 + off],
                        fill=mid if k < 2 else bright, outline=bright)
    elif kind == 3:                                     # heal: a cross
        d.rectangle([cx - 3, 5, cx + 3, size - 6], fill=bright)
        d.rectangle([5, cy - 3, size - 6, cy + 3], fill=bright)
    elif kind == 4:                                     # storm: a bolt
        d.polygon([(cx + 4, 3), (cx - 5, cy + 2), (cx - 1, cy + 2),
                   (cx - 4, size - 3), (cx + 6, cy - 3), (cx + 1, cy - 3)],
                  fill=bright)
    else:                                               # trap: an eye
        d.ellipse([3, cy - 7, size - 4, cy + 7], fill=mid, outline=bright)
        d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=bright)
        d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=(12, 12, 16))
    return em


def draw_support_card(kind):
    bright, dark, mid = SUPPORT_TINTS[kind]
    img = card_frame(bright, dark, mid, tuple(min(255, c + 40) for c in bright))
    img.paste(draw_support_emblem(kind, ART_W).resize((ART_W, ART_H)),
              (ART_X, ART_Y))
    ImageDraw.Draw(img).rectangle(
        [ART_X, ART_Y, ART_X + ART_W - 1, ART_Y + ART_H - 1], outline=dark)
    card_footer(img, tuple(min(255, c + 40) for c in bright), dark,
                tuple(min(255, c + 60) for c in bright), 4, (248, 236, 140))
    return img


def draw_card_back():
    img = Image.new("RGB", (CARD_W, CARD_H), (46, 24, 8))
    d = ImageDraw.Draw(img)
    d.rectangle([1, 0, CARD_W - 2, CARD_H - 1], fill=(205, 132, 35))
    d.rectangle([3, 3, CARD_W - 4, CARD_H - 4], fill=(15, 8, 4))
    cx, cy = CARD_W // 2, CARD_H // 2
    colors = [(230, 136, 18), (140, 70, 8), (250, 187, 34)]
    for k in range(14):
        r = 3 + k * 2
        d.arc([cx - r, cy - r, cx + r, cy + r], k * 22, k * 22 + 230,
              fill=colors[k % 3], width=2)
    d.rectangle([1, 0, CARD_W - 2, CARD_H - 1], outline=(35, 19, 7))
    d.rectangle([2, 2, CARD_W - 3, CARD_H - 3], outline=(240, 169, 45))
    return img


# ── The duel board backdrop ──────────────────────────────────────────────────

TEXTURE_DIR = os.path.join(ROOT, "assets", "source", "textures")


def sky_band(name, height):
    """The upper part of a stage's environment gradient, which is what the 3D
    targets put above their horizon too."""
    img = Image.open(os.path.join(BG_DIR, name)).convert("RGB")
    band = img.crop((0, 0, img.width, max(1, img.height // 3)))
    return band.resize((WIDTH, height), Image.Resampling.LANCZOS)


def perspective_floor(texture, height, dim):
    """A receding stone floor, built by warping a tiled material.

    The arena the other targets draw is 3D; this one is a picture of the same
    material seen from the same place.  The source quad is wide at the top and
    narrow at the bottom, so the top of the destination rectangle samples a
    span far wider than the screen -- which is what recession is.  The tile is
    scaled down first: at its native 1254 pixels one tile would cover the whole
    plane and the floor would have no grain to recede with."""
    tile = Image.open(os.path.join(TEXTURE_DIR, texture)).convert("RGB")
    tile = tile.resize((128, 128), Image.Resampling.LANCZOS)
    plane = Image.new("RGB", (512, 512))
    for y in range(0, 512, 128):
        for x in range(0, 512, 128):
            plane.paste(tile, (x, y))
    floor = plane.transform((WIDTH, height), Image.Transform.QUAD,
                            data=(0, 0, 160, 512, 352, 512, 512, 0),
                            resample=Image.Resampling.BILINEAR)
    # Haze toward the horizon: distance is the only depth cue a flat image has,
    # and the far end of the floor also has to stop competing with the cards
    # standing on it.
    dark = Image.new("RGB", (WIDTH, height), (0, 0, 0))
    mask = Image.new("L", (1, height))
    for y in range(height):
        far = 1.0 - (y / float(height - 1))
        mask.putpixel((0, y), int(255 * (dim + (0.78 - dim) * far * far)))
    return Image.composite(dark, floor, mask.resize((WIDTH, height)))


def draw_plinth(img, x, y, edge):
    """One empty card recess, drawn at exactly the rect msx2_board.c blits a
    card into.

    The well is allowed to be textured -- the floor material shows through it,
    which matters because ten of the fifteen slots are empty when a duel opens.
    Putting a destroyed monster's slot back is therefore not a fill but a blit
    of the SLOTS blob, which is cut out of this very image after it has been
    quantised, so the restore is the backdrop's own bytes.

    The RING around it is flat, and that part is load-bearing: the selection
    cursor is drawn into the ring and erased with a fill of MSX2_RING_COLOR."""
    d = ImageDraw.Draw(img)
    d.rectangle([x - RING, y - RING, x + CARD_W - 1 + RING, y + CARD_H - 1 + RING],
                fill=RING_RGB)
    well = Image.new("RGBA", (CARD_W, CARD_H), (0, 0, 0, 56))
    ImageDraw.Draw(well).rectangle([0, 0, CARD_W - 1, CARD_H - 1],
                                   outline=edge + (190,))
    img.alpha_composite(well, (x, y))


def draw_panel(d, x, y, w, h, fill, edge):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=fill)
    d.rectangle([x, y, x + w - 1, y + h - 1], outline=edge)


def duel_board(sky_src, texture, dim, tint, edge):
    # The horizon sits exactly under the HUD.  There is no room for a sky on
    # this screen -- three rows of 48-pixel cards and two panels use all 212
    # lines -- and a three-pixel band of it peeking out above the top row read
    # as blue rubbish rather than as distance.
    horizon = HUD_H
    img = Image.new("RGB", (WIDTH, HEIGHT))
    img.paste(sky_band(sky_src, horizon), (0, 0))
    img.paste(perspective_floor(texture, HEIGHT - horizon, dim), (0, horizon))
    # A stage tint, so the four arenas read as four places and not as one
    # sandstone floor under four skies.
    img = Image.blend(img, Image.new("RGB", (WIDTH, HEIGHT), tint), 0.16)

    img = img.convert("RGBA")
    for row_y in (ROW_COM_Y, ROW_PLAYER_Y, ROW_HAND_Y):
        for i in range(SLOTS):
            draw_plinth(img, SLOT_X0 + i * SLOT_PITCH, row_y, edge)
    img = img.convert("RGB")

    d = ImageDraw.Draw(img)
    gold = (198, 152, 54)
    # HUD strip and info panel: flat dark ground, because live text is written
    # over them every turn and GRAPHIC 7 has no way to erase back to artwork
    # except by putting the artwork there again.
    draw_panel(d, 0, 0, WIDTH, HUD_H, PANEL_RGB, gold)
    draw_panel(d, 0, INFO_Y, WIDTH, HEIGHT - INFO_Y, PANEL_RGB, gold)
    d.rectangle([8, ROW_HAND_Y - 5, WIDTH - 9, ROW_HAND_Y - 4], fill=gold)
    return img


SCENES = [
    ("TITLE", lambda: Image.open(
        os.path.join(ROOT, "assets/source/title/title256_msx2.png"))),
    ("ENDING", lambda: Image.open(
        os.path.join(ROOT, "assets/source/ending/ending256x212.png"))),
    ("BOARD_DESERT", lambda: duel_board("desert.png", "sandstone_1.png", 0.08,
                                        (28, 18, 6), (198, 152, 54))),
    ("BOARD_STONE", lambda: duel_board("stone.png", "sandstone_2.png", 0.06,
                                       (16, 18, 22), (176, 178, 168))),
    ("BOARD_EMBER", lambda: duel_board("ember.png", "sandstone_1.png", 0.12,
                                       (40, 8, 4), (232, 120, 52))),
    ("BOARD_SKY", lambda: duel_board("sky.png", "pyramid_beige.png", 0.10,
                                     (8, 10, 30), (156, 186, 232))),
]


def build_card_blob(cards, quiet):
    """Every card texture at a fixed 2 KB stride.

    The stride is padding, not waste: a card that straddled a 16 KB segment
    boundary would have to be streamed in two bank-switched halves, and 2,048
    divides 16,384 exactly, so it never can."""
    blob = bytearray()
    faces = []
    for asset_id, _name in cards:
        faces.append(draw_monster_card(asset_id, *CARD_STATS[asset_id]))
    for kind in range(SUPPORT_VARIANTS):
        faces.append(draw_support_card(kind))
    faces.append(draw_card_back())

    for face in faces:
        data = grb.quantize(face, (CARD_W, CARD_H))
        blob += data
        blob += bytes(CARD_STRIDE - len(data))

    # A contact sheet, decoded straight back out of the blob, so the art can be
    # checked without an emulator and without trusting the drawing code.
    columns = 10
    rows = (len(faces) + columns - 1) // columns
    sheet = bytearray(columns * CARD_W * rows * CARD_H)
    for i in range(len(faces)):
        cx, cy = (i % columns) * CARD_W, (i // columns) * CARD_H
        for y in range(CARD_H):
            src = i * CARD_STRIDE + y * CARD_W
            dst = (cy + y) * columns * CARD_W + cx
            sheet[dst:dst + CARD_W] = blob[src:src + CARD_W]
    grb.write_preview(os.path.join(ASSET_DIR, "cards.png"), bytes(sheet),
                      (columns * CARD_W, rows * CARD_H))
    if not quiet:
        print("CARDS    %d textures -> %d bytes" % (len(faces), len(blob)))
    return bytes(blob), len(faces)


def cut_slot_tiles(scene):
    """The fifteen empty-slot rectangles of one backdrop, at the card stride.

    Cut out of the quantised scene rather than re-rendered, so a tile is byte
    for byte what the streamed backdrop put on that part of the screen."""
    blob = bytearray()
    for row_y in (ROW_COM_Y, ROW_PLAYER_Y, ROW_HAND_Y):
        for i in range(SLOTS):
            x = SLOT_X0 + i * SLOT_PITCH
            tile = bytearray()
            for y in range(CARD_H):
                start = (row_y + y) * WIDTH + x
                tile += scene[start:start + CARD_W]
            blob += tile + bytes(CARD_STRIDE - len(tile))
    return bytes(blob)


def build_text_blob(cards):
    """Fixed-stride, NUL-padded strings.  Fixed stride so the Z80 indexes them
    with a shift instead of walking the table, and out of line in a cartridge
    segment because 1.8 KB of names would be 1.8 KB the 32 KB code budget does
    not have."""
    blob = bytearray()
    names = [name for _id, name in cards] + SUPPORT_NAMES
    for name in names:
        text = name.upper()[:NAME_STRIDE - 1].encode("ascii", "replace")
        blob += text + bytes(NAME_STRIDE - len(text))
    return bytes(blob), len(names)


def main():
    quiet = "--quiet" in sys.argv
    os.makedirs(ASSET_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(HEADER), exist_ok=True)

    cards = parse_cards()

    segment = FIRST_ASSET_SEGMENT
    entries = []
    slot_blob = bytearray()
    for name, build in SCENES:
        data = grb.quantize(build(), (WIDTH, HEIGHT))
        if name.startswith("BOARD_"):
            slot_blob += cut_slot_tiles(data)
        binpath = os.path.join(ASSET_DIR, name.lower() + ".bin")
        with open(binpath, "wb") as f:
            f.write(data)
        grb.write_preview(os.path.join(ASSET_DIR, name.lower() + ".png"), data,
                          (WIDTH, HEIGHT))
        span = (len(data) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
        entries.append((name, segment, span))
        if not quiet:
            print("%-13s %d bytes, segments %d..%d"
                  % (name, len(data), segment, segment + span - 1))
        segment += span

    card_blob, card_count = build_card_blob(cards, quiet)
    with open(os.path.join(ASSET_DIR, "cards.bin"), "wb") as f:
        f.write(card_blob)
    card_segment = segment
    segment += (len(card_blob) + SEGMENT_BYTES - 1) // SEGMENT_BYTES

    with open(os.path.join(ASSET_DIR, "slots.bin"), "wb") as f:
        f.write(slot_blob)
    slot_segment = segment
    segment += (len(slot_blob) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
    if not quiet:
        print("SLOTS    %d tiles -> %d bytes, segments %d..%d"
              % (len(slot_blob) // CARD_STRIDE, len(slot_blob), slot_segment,
                 segment - 1))

    text_blob, name_count = build_text_blob(cards)
    with open(os.path.join(ASSET_DIR, "text.bin"), "wb") as f:
        f.write(text_blob)
    text_segment = segment
    segment += (len(text_blob) + SEGMENT_BYTES - 1) // SEGMENT_BYTES

    # The packer works from this manifest rather than by scraping the header:
    # "where does each blob go" is data, and re-deriving it from C macros with a
    # regular expression is how the two drift apart.
    with open(os.path.join(ASSET_DIR, "manifest.txt"), "w") as f:
        f.write("# name  first-segment  file   (written by gen_msx_scenes.py)\n")
        for name, seg, _span in entries:
            f.write("%s %d %s.bin\n" % (name, seg, name.lower()))
        f.write("CARDS %d cards.bin\n" % card_segment)
        f.write("SLOTS %d slots.bin\n" % slot_segment)
        f.write("TEXT %d text.bin\n" % text_segment)

    with open(HEADER, "w") as f:
        f.write("// Generated by tools/msx2/gen_msx_scenes.py -- do not edit.\n")
        f.write("//\n")
        f.write("// Where every cartridge asset lives.  None of it is linked into the 32 KB\n")
        f.write("// the Z80 sees: the streamer maps the segment into the 0x8000 window and\n")
        f.write("// pushes it straight at the VDP (or, for text, into a RAM buffer).\n")
        f.write("#pragma once\n\n")
        f.write("#define MSX2_SCENE_BYTES        %d   // 256 x 212 GRB332\n" % (WIDTH * HEIGHT))
        f.write("#define MSX2_SCENE_SEG_SPAN     %d   // 16 KB segments per scene\n\n"
                % ((WIDTH * HEIGHT + SEGMENT_BYTES - 1) // SEGMENT_BYTES))
        for name, seg, _span in entries:
            f.write("#define MSX2_SCENE_%-14s %d\n" % (name + "_SEGMENT", seg))
        f.write("\n// The four duel backdrops, in story-stage order.\n")
        f.write("#define MSX2_BOARD_SEGMENT(stage)  "
                "(MSX2_SCENE_BOARD_DESERT_SEGMENT + (stage) * MSX2_SCENE_SEG_SPAN)\n")
        f.write("#define MSX2_BOARD_STAGES       4\n\n")

        f.write("// ── Card textures ───────────────────────────────────────────────────────\n")
        f.write("#define MSX2_CARD_ART_SEGMENT   %d\n" % card_segment)
        f.write("#define MSX2_CARD_ART_STRIDE    %d\n" % CARD_STRIDE)
        f.write("#define MSX2_CARD_ART_PER_SEG   %d\n" % (SEGMENT_BYTES // CARD_STRIDE))
        f.write("#define MSX2_CARD_ART_COUNT     %d\n" % card_count)
        f.write("#define MSX2_CARD_BACK_INDEX    %d\n" % (card_count - 1))
        f.write("#define MSX2_CARD_W             %d\n" % CARD_W)
        f.write("#define MSX2_CARD_H             %d\n\n" % CARD_H)

        f.write("// ── Board geometry, shared with the baked backdrop ──────────────────────\n")
        f.write("#define MSX2_SLOTS              %d\n" % SLOTS)
        f.write("#define MSX2_SLOT_X0            %d\n" % SLOT_X0)
        f.write("#define MSX2_SLOT_PITCH         %d\n" % SLOT_PITCH)
        f.write("#define MSX2_ROW_COM_Y          %d\n" % ROW_COM_Y)
        f.write("#define MSX2_ROW_PLAYER_Y       %d\n" % ROW_PLAYER_Y)
        f.write("#define MSX2_ROW_HAND_Y         %d\n" % ROW_HAND_Y)
        f.write("#define MSX2_HUD_H              %d\n" % HUD_H)
        f.write("#define MSX2_INFO_Y             %d\n" % INFO_Y)
        f.write("#define MSX2_SLOT_RING          %d\n" % RING)
        f.write("// The exact GRB332 bytes the backdrop was baked with, so a fill\n")
        f.write("// erases back to the picture instead of to something close to it.\n")
        f.write("#define MSX2_RING_COLOR         0x%02X\n" % grb.pack(*RING_RGB))
        f.write("#define MSX2_PANEL_COLOR        0x%02X\n\n" % grb.pack(*PANEL_RGB))

        f.write("// ── Empty-slot tiles ───────────────────────────────────────────────────\n")
        f.write("// One 40x48 cut-out of each backdrop at each of the fifteen slots, in the\n")
        f.write("// backdrop's own quantised bytes.  Clearing a destroyed monster blits the\n")
        f.write("// tile for (stage, slot); that is the only way to put a textured board\n")
        f.write("// back exactly without re-streaming the whole picture.\n")
        f.write("#define MSX2_SLOT_ART_SEGMENT   %d\n" % slot_segment)
        f.write("#define MSX2_SLOT_ART_STRIDE    %d\n" % CARD_STRIDE)
        f.write("#define MSX2_SLOT_ART_PER_SEG   %d\n" % (SEGMENT_BYTES // CARD_STRIDE))
        f.write("#define MSX2_SLOT_ART_PER_STAGE %d\n\n" % (3 * SLOTS))

        f.write("// ── String table ───────────────────────────────────────────────────────\n")
        f.write("#define MSX2_TEXT_SEGMENT       %d\n" % text_segment)
        f.write("#define MSX2_NAME_STRIDE        %d\n" % NAME_STRIDE)
        f.write("#define MSX2_NAME_COUNT         %d\n\n" % name_count)

        f.write("#define MSX2_SCENE_SEGMENT_FIRST  %d\n" % FIRST_ASSET_SEGMENT)
        f.write("#define MSX2_SCENE_SEGMENT_LAST   %d\n" % (segment - 1))
        f.write("#define MSX2_ASSET_ROM_KB         %d\n" % (segment * SEGMENT_BYTES // 1024))

    if not quiet:
        print("TEXT     %d names -> %d bytes, segment %d"
              % (name_count, len(text_blob), text_segment))
        print("cartridge assets end at segment %d (%d KB)"
              % (segment - 1, segment * SEGMENT_BYTES // 1024))
        print("wrote %s" % os.path.relpath(HEADER, ROOT))


def load_card_stats():
    """ATK/DEF per asset id, for the level pips on the card face."""
    stats = {}
    with open(CARD_DATA) as f:
        for raw in f:
            parts = [p.strip() for p in raw.strip().split("|")]
            if parts and parts[0] == "card":
                stats[parts[1]] = (int(parts[5]), int(parts[6]))
    return stats


CARD_STATS = load_card_stats()


if __name__ == "__main__":
    main()
