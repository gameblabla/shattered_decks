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
import gen_msx_views as views  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(ROOT, "src", "msx2", "assets")
HEADER = os.path.join(ROOT, "src", "generated", "msx2_scenes.h")
CARD_DIR = os.path.join(ROOT, "assets", "source", "cards")
CARD_DATA = os.path.join(CARD_DIR, "card_data.txt")
BG_DIR = os.path.join(ROOT, "assets", "source", "bg")
PORTRAIT_DIR = os.path.join(ROOT, "assets", "source", "story_portraits")
MAIN_C = os.path.join(ROOT, "src", "main.c")

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
# The board is NOT drawn here.  MSX2_PORT_PLAN.md §0.3.1 makes the duel's arena
# the game's own arena, captured out of `waifu_fm_headless`, so everything about
# it -- the picture, the projected card quads, the empty-slot tiles and the
# baked camera move -- comes from gen_msx_views.py.  What is left in this file
# is the card art that gets drawn INTO those quads, and the screens that are
# genuinely 2-D.
CARD_W, CARD_H = views.CARD_W, views.CARD_H
PANEL_RGB = views.PANEL_RGB

# 72 monsters + 6 support variants + 1 card back.
SUPPORT_VARIANTS = 6
NAME_STRIDE = 24
CARD_STRIDE = 2048   # 8 cards per segment, so no card ever straddles one

# ── Story dialogue ───────────────────────────────────────────────────────────
#
# One composite per (duel, speaker): backdrop, character and an *empty* text box
# flattened into a single 54,272-byte picture, exactly as MSX2_PORT_PLAN.md
# §14.2 specifies.  A dialogue beat is then one stream and nothing else -- the
# alternative, streaming a backdrop and then blitting a portrait over it, costs
# two copies for a picture that sits perfectly still for ten seconds.
STORY_DUELS = 5
TALK_BOX_Y = 140
TALK_NAME_Y = 144
TALK_LINE_Y = (158, 170, 182)
TALK_PROMPT_Y = 196
TALK_TEXT_X = 8          # 40 columns of the 6-pixel font, with a margin
TALK_NAME_X = 10
TALK_PLATE_RGB = (28, 22, 40)

# ── §14.2: the composited visual-novel scene ─────────────────────────────────
#
# Both speakers stand on the shipped painting at once, the inactive one dimmed,
# and only the two portrait rects change when the speaker does.  The busts are
# blitted at runtime from baked run-length skip lists, so a transparent pixel
# costs one VRAM address re-set and an opaque one costs an OUTI -- which is what
# makes two figures cheaper than the single flattened composite this replaces.
PORTRAIT_W = 124
PORTRAIT_H = TALK_BOX_Y - 16     # everything above the text box
PORTRAIT_LEFT_X = 2
PORTRAIT_RIGHT_X = WIDTH - PORTRAIT_W - 2
PORTRAIT_LEFT_Y = 16
PORTRAIT_RIGHT_Y = 22            # the six-pixel stagger the other targets use
# The inactive speaker is dimmed rather than removed.  Both variants are cut
# from the SAME alpha mask, so they cover byte for byte the same pixels and a
# speaker change is a pure overwrite with no background repair at all.
PORTRAIT_DIM = 0.45
PORTRAIT_STRIDE = 32768          # two whole segments per baked bust
PORTRAIT_CHARS = 1 + STORY_DUELS # Serena, then one opponent per duel

# The sanctum map's list panel, baked empty and filled with rows at runtime.
MAP_PANEL_X = 20
MAP_PANEL_Y = 30
MAP_PANEL_W = WIDTH - 40
MAP_PANEL_H = 156

# Story text records: a speaker byte then a NUL-terminated line.  Fixed stride
# because the Z80 indexes them, and out of line in the cartridge because eight
# kilobytes of prose is eight kilobytes the 32 KB code budget does not have.
LINE_STRIDE = 112
LINES_PER_DUEL = 12
INTRO_LINES = 6
ENDING_LINES = 6
OPP_STRIDE = 32          # name and title, 16 bytes each


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


# ── 2-D screen furniture ─────────────────────────────────────────────────────

def draw_panel(d, x, y, w, h, fill, edge):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=fill)
    d.rectangle([x, y, x + w - 1, y + h - 1], outline=edge)


# ── Story text, lifted out of src/main.c ─────────────────────────────────────
#
# Parsed rather than retyped.  The MSX2 build is a fork of the frontend, not of
# the writing, and a second copy of five thousand words of dialogue is a second
# copy that goes stale.

def c_strings(body):
    """Every "..." literal in a chunk of C, unescaped."""
    out = []
    for raw in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        out.append(raw.replace('\\"', '"').replace("\\\\", "\\"))
    return out


def c_array_body(text, name):
    start = text.index(name)
    start = text.index("{", start)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise SystemExit("unterminated array %s in src/main.c" % name)


SPEAKER = {"STORY_SPK_SERENA": 0, "STORY_SPK_OPPONENT": 1, "STORY_SPK_NARRATOR": 2}


def parse_story():
    text = open(MAIN_C).read()

    dialogue = []
    for duel in range(STORY_DUELS):
        body = c_array_body(text, "g_story_duel%d_dialogue[]" % duel)
        lines = []
        for speaker, line in re.findall(
                r'\{\s*(STORY_SPK_\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', body):
            lines.append((SPEAKER[speaker], line))
        if len(lines) > LINES_PER_DUEL:
            sys.exit("duel %d has %d dialogue lines, more than LINES_PER_DUEL"
                     % (duel, len(lines)))
        dialogue.append(lines)

    intro = c_strings(c_array_body(text, "story_intro_lines[]"))
    ending = c_strings(c_array_body(text, "story_ending_lines[]"))

    opponents = []
    body = c_array_body(text, "g_story_opponents[STORY_MAX_DUELS]")
    for name, title in re.findall(
            r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"', body):
        opponents.append((name, title))
    if len(opponents) != STORY_DUELS:
        sys.exit("expected %d story opponents, parsed %d" % (STORY_DUELS, len(opponents)))

    return dialogue, intro, ending, opponents


def line_record(speaker, text):
    body = text.upper()[:LINE_STRIDE - 2].encode("ascii", "replace")
    return bytes((speaker,)) + body + bytes(LINE_STRIDE - 1 - len(body))


# ── Story screens ────────────────────────────────────────────────────────────
#
# §14.2 and §21.3: the backdrop is one of the paintings the other targets
# already show, whole, centred on the 212-row window -- not a sky band over a
# floor this generator invented.  They are 256x240 and therefore already the
# right width, so the crop is rows 14..225 and nothing else.

BG_CROP_Y = 14

STAGE_BG = ["desert.png", "stone.png", "ember.png", "sky.png"]
STAGE_FOR_DUEL = [0, 0, 1, 2, 3]


def stage_painting(stage):
    img = Image.open(os.path.join(BG_DIR, STAGE_BG[stage])).convert("RGB")
    if img.width != WIDTH:
        img = img.resize((WIDTH, img.height * WIDTH // img.width),
                         Image.Resampling.LANCZOS)
    top = max(0, min(BG_CROP_Y, img.height - HEIGHT))
    return img.crop((0, top, WIDTH, top + HEIGHT))


def portrait(filename, size):
    """A story bust, trimmed and fitted the way gen_assets.py fits them for
    every other target: upper body, pinned to the bottom of its area."""
    img = Image.open(os.path.join(PORTRAIT_DIR, filename)).convert("RGBA")
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    w, h = img.size
    img = img.crop((int(w * 0.05), int(h * 0.01),
                    max(1, int(w * 0.95)), max(2, int(h * 0.80))))
    art = ImageOps.contain(img, size, method=Image.Resampling.LANCZOS)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.alpha_composite(art, ((size[0] - art.width) // 2, size[1] - art.height))
    return out


PORTRAIT_MAX_RUNS = 3            # opaque runs per row the index can hold
PORTRAIT_ROW_STRIDE = 1 + PORTRAIT_MAX_RUNS * 2
PORTRAIT_INDEX_BYTES = 1024      # the run table, ahead of the pixels


def portrait_runs(alpha, y, w):
    """The opaque runs of one row, merged down to PORTRAIT_MAX_RUNS.

    A bust is a single figure, so a row is one run and occasionally two (an arm
    away from the body).  Anything past the third run is folded in by absorbing
    the narrowest transparent gap -- a couple of background pixels drawn over,
    against a fixed-size table the Z80 can index with a shift."""
    apx = alpha.load()
    runs = []
    x = 0
    while x < w:
        while x < w and apx[x, y] <= 96:
            x += 1
        if x >= w:
            break
        start = x
        while x < w and apx[x, y] > 96:
            x += 1
        runs.append([start, x - start])
    while len(runs) > PORTRAIT_MAX_RUNS:
        gaps = [(runs[i + 1][0] - (runs[i][0] + runs[i][1]), i)
                for i in range(len(runs) - 1)]
        _gap, i = min(gaps)
        runs[i][1] = runs[i + 1][0] + runs[i + 1][1] - runs[i][0]
        del runs[i + 1]
    # A run has to be blittable as one rectangle row, so 255 is the cap.
    return [(a, min(b, 255)) for a, b in runs]


def portrait_blob(bust, dim):
    """One bust as a run table plus a packed pixel stream (§1.2, §14.2).

    Splitting the two is what lets the runtime blit a portrait with the ordinary
    rectangle copy it already has: it reads the small table into RAM, then walks
    it issuing one row-blit per opaque run.  A transparent pixel costs nothing
    at all -- not even a VRAM write -- and no new inner loop had to be written
    for it.

    The two brightness variants are cut from the SAME alpha mask, so they have
    identical tables and cover byte for byte the same pixels: swapping which
    speaker is lit is a pure overwrite with no background repair."""
    w, h = bust.size
    alpha = bust.getchannel("A")
    rgb = bust.convert("RGB")
    if dim:
        rgb = Image.blend(Image.new("RGB", bust.size, (0, 0, 0)), rgb,
                          PORTRAIT_DIM)
    quant = grb.quantize(rgb, (w, h))

    index = bytearray()
    pixels = bytearray()
    for y in range(h):
        runs = portrait_runs(alpha, y, w)
        index.append(len(runs))
        for i in range(PORTRAIT_MAX_RUNS):
            if i < len(runs):
                index += bytes(runs[i])
                pixels += quant[y * w + runs[i][0]:y * w + runs[i][0] + runs[i][1]]
            else:
                index += b"\x00\x00"
    if len(index) > PORTRAIT_INDEX_BYTES:
        sys.exit("portrait run table is %d bytes, past %d"
                 % (len(index), PORTRAIT_INDEX_BYTES))
    body = index + bytes(PORTRAIT_INDEX_BYTES - len(index)) + pixels
    if len(body) > PORTRAIT_STRIDE:
        sys.exit("portrait is %d bytes, past the %d stride"
                 % (len(body), PORTRAIT_STRIDE))
    return body + bytes(PORTRAIT_STRIDE - len(body))


def build_portrait_blob(quiet):
    """Serena and the five opponents, lit and dimmed."""
    blob = bytearray()
    files = ["serena.png"] + ["opponent_%d.png" % d for d in range(STORY_DUELS)]
    size = (PORTRAIT_W, PORTRAIT_H)
    for name in files:
        bust = portrait(name, size)
        for dim in (False, True):
            blob += portrait_blob(bust, dim)
    if not quiet:
        print("PORTRAIT %d busts x 2 variants -> %d bytes"
              % (len(files), len(blob)))
    return bytes(blob)


def vn_scene(stage):
    """The dialogue backdrop: the painting, the text box and the name plate.

    The busts are NOT in it -- that is the whole of §14.2.  A speaker change
    touches two rects instead of streaming 54 KB, and both characters are on
    screen at once, which the flattened composite could never manage."""
    img = stage_painting(stage)
    d = ImageDraw.Draw(img)
    gold = (198, 152, 54)
    draw_panel(d, 0, TALK_BOX_Y, WIDTH, HEIGHT - TALK_BOX_Y, PANEL_RGB, gold)
    d.rectangle([6, TALK_NAME_Y - 3, 150, TALK_NAME_Y + 9], fill=TALK_PLATE_RGB,
                outline=gold)
    return img


def map_scene(stage):
    """The sanctum map takes the same backdrop treatment (§14.4)."""
    img = stage_painting(stage)
    d = ImageDraw.Draw(img)
    draw_panel(d, MAP_PANEL_X, MAP_PANEL_Y, MAP_PANEL_W, MAP_PANEL_H,
               PANEL_RGB, (198, 152, 54))
    return img


SCENES = [
    ("TITLE", lambda: Image.open(
        os.path.join(ROOT, "assets/source/title/title256_msx2.png"))),
    ("ENDING", lambda: Image.open(
        os.path.join(ROOT, "assets/source/ending/ending256x212.png"))),
]

# The sanctum map, one per stage, then the dialogue backdrop, one per stage.
# Both are indexed arithmetically at runtime, so they are appended in exactly
# this order and the header emits the base of each run.
for _stage in range(len(STAGE_BG)):
    SCENES.append(("MAP_%d" % _stage, (lambda st: lambda: map_scene(st))(_stage)))
for _stage in range(len(STAGE_BG)):
    SCENES.append(("TALK_%d" % _stage, (lambda st: lambda: vn_scene(st))(_stage)))


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

    mirror = bytearray()
    for face in faces:
        data = grb.quantize(face, (CARD_W, CARD_H))
        blob += data + bytes(CARD_STRIDE - len(data))
        # The mirrored set exists for the COM row.  Its cards are rotated 180
        # degrees on the board plane, so the span programs walk their texels
        # backwards (§8.4); reading a mirrored texture forwards is the same
        # picture and costs a second blob instead of a second inner loop.
        flipped = grb.quantize(face.transpose(Image.Transpose.FLIP_LEFT_RIGHT),
                               (CARD_W, CARD_H))
        mirror += flipped + bytes(CARD_STRIDE - len(flipped))

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
        print("CARDS    %d textures -> %d bytes (+ the mirrored set)"
              % (len(faces), len(blob)))
    return bytes(blob), bytes(mirror), len(faces)


def build_text_blob(cards, story):
    """Every string the game shows, at fixed strides.

    Fixed stride so the Z80 indexes a record with arithmetic instead of walking
    the table, and out of line in a cartridge segment because ten kilobytes of
    card names and dialogue is ten kilobytes the 32 KB code budget does not
    have.  The sections are laid out in a fixed order and their offsets are
    emitted into the generated header."""
    dialogue, intro, ending, opponents = story
    blob = bytearray()
    offsets = {}

    def section(name):
        offsets[name] = len(blob)

    section("NAME")
    names = [name for _id, name in cards] + SUPPORT_NAMES
    for name in names:
        text = name.upper()[:NAME_STRIDE - 1].encode("ascii", "replace")
        blob += text + bytes(NAME_STRIDE - len(text))

    section("OPPONENT")
    for name, title in opponents:
        for field in (name, title):
            text = field.upper()[:OPP_STRIDE // 2 - 1].encode("ascii", "replace")
            blob += text + bytes(OPP_STRIDE // 2 - len(text))

    section("DIALOGUE")
    counts = []
    for lines in dialogue:
        counts.append(len(lines))
        for speaker, text in lines:
            blob += line_record(speaker, text)
        blob += bytes(LINE_STRIDE * (LINES_PER_DUEL - len(lines)))

    section("INTRO")
    for text in intro[:INTRO_LINES]:
        blob += line_record(2, text)
    blob += bytes(LINE_STRIDE * (INTRO_LINES - len(intro[:INTRO_LINES])))

    section("ENDING")
    for text in ending[:ENDING_LINES]:
        blob += line_record(2, text)
    blob += bytes(LINE_STRIDE * (ENDING_LINES - len(ending[:ENDING_LINES])))

    return bytes(blob), len(names), offsets, counts, len(intro), len(ending)


def main():
    quiet = "--quiet" in sys.argv
    os.makedirs(ASSET_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(HEADER), exist_ok=True)

    cards = parse_cards()

    # The board comes out of the game's own renderer before anything else, both
    # because it is the expensive step and because its geometry is emitted into
    # the same header every screen below shares.
    board = views.bake(quiet)

    segment = FIRST_ASSET_SEGMENT
    entries = []
    for name, build in SCENES:
        data = grb.quantize(build(), (WIDTH, HEIGHT))
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

    def place(name, blob):
        """Write a blob out and take the whole segments it needs.

        Every asset is segment-aligned: the streamer maps a segment and pushes
        it at the VDP, so an asset that started mid-segment would have to be
        addressed as a bank plus an offset on every single access."""
        nonlocal segment
        with open(os.path.join(ASSET_DIR, name + ".bin"), "wb") as f:
            f.write(blob)
        first = segment
        segment += (len(blob) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
        extra.append((name.upper(), first, name))
        if not quiet:
            print("%-13s %d bytes, segments %d..%d"
                  % (name.upper(), len(blob), first, segment - 1))
        return first

    extra = []

    card_blob, card_mirror, card_count = build_card_blob(cards, quiet)
    card_segment = place("cards", card_blob)
    card_mirror_segment = place("cards_mirror", card_mirror)

    # ── The captured board (§4.3, §4.6, §8.4) ────────────────────────────────
    view_segment = place("board_views", b"".join(board["views"]))
    slot_segment = place("board_slots", board["slots"])
    span_segment = place("card_spans", board["spans"])
    move_segments = [place("board_move_" + name.lower(), blob)
                     for name, _poses, blob in board["moves"]]

    portrait_blob = build_portrait_blob(quiet)
    portrait_segment = place("portraits", portrait_blob)

    story = parse_story()
    text_blob, name_count, text_off, dialogue_counts, intro_n, ending_n = \
        build_text_blob(cards, story)
    text_segment = place("text", text_blob)

    # The packer works from this manifest rather than by scraping the header:
    # "where does each blob go" is data, and re-deriving it from C macros with a
    # regular expression is how the two drift apart.
    with open(os.path.join(ASSET_DIR, "manifest.txt"), "w") as f:
        f.write("# name  first-segment  file   (written by gen_msx_scenes.py)\n")
        for name, seg, _span in entries:
            f.write("%s %d %s.bin\n" % (name, seg, name.lower()))
        for name, seg, base in extra:
            f.write("%s %d %s.bin\n" % (name, seg, base))

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

        f.write("// ── Card textures ───────────────────────────────────────────────────────\n")
        f.write("#define MSX2_CARD_ART_SEGMENT   %d\n" % card_segment)
        f.write("// The same textures mirrored left to right, for the COM row (§8.4).\n")
        f.write("#define MSX2_CARD_MIRROR_SEGMENT %d\n" % card_mirror_segment)
        f.write("#define MSX2_CARD_ART_STRIDE    %d\n" % CARD_STRIDE)
        f.write("#define MSX2_CARD_ART_PER_SEG   %d\n" % (SEGMENT_BYTES // CARD_STRIDE))
        f.write("#define MSX2_CARD_ART_COUNT     %d\n" % card_count)
        f.write("#define MSX2_CARD_BACK_INDEX    %d\n" % (card_count - 1))
        f.write("// MSX2_CARD_W / MSX2_CARD_H come from the board section below:\n")
        f.write("// the texture size and the quads it is mapped into are one decision.\n")

        f.write("\n".join(views.header_lines(board, view_segment, slot_segment,
                                              move_segments, span_segment)))
        f.write("\n")

        f.write("// ── Story screens ──────────────────────────────────────────────────────\n")
        f.write("#define MSX2_MAP_SEGMENT(stage)   "
                "(MSX2_SCENE_MAP_0_SEGMENT + (stage) * MSX2_SCENE_SEG_SPAN)\n")
        f.write("// §14.2: the dialogue scene is composited at runtime.  The backdrop is\n")
        f.write("// the shipped painting with the text box baked in, one per stage, and it\n")
        f.write("// is streamed ONCE per scene; the two busts are blitted over it from the\n")
        f.write("// skip lists below and a speaker change touches nothing else.\n")
        f.write("#define MSX2_TALK_SEGMENT(stage)  "
                "(MSX2_SCENE_TALK_0_SEGMENT + (stage) * MSX2_SCENE_SEG_SPAN)\n")
        f.write("#define MSX2_STORY_DUELS        %d\n" % STORY_DUELS)
        f.write("static const unsigned char g_msx2_stage_for_duel[MSX2_STORY_DUELS] =\n")
        f.write("\t{ %s };\n" % ", ".join(str(v) for v in STAGE_FOR_DUEL))
        f.write("\n// ── Story busts (§14.2) ────────────────────────────────────────────────\n")
        f.write("// Character 0 is Serena and 1..5 are the opponents; each is baked twice,\n")
        f.write("// lit and dimmed, from the SAME alpha mask -- so the two variants cover\n")
        f.write("// byte for byte the same pixels and swapping which speaker is lit is a\n")
        f.write("// pure overwrite with no background repair at all.\n")
        f.write("#define MSX2_PORTRAIT_SEGMENT   %d\n" % portrait_segment)
        f.write("#define MSX2_PORTRAIT_SEGS      %d\n" % (PORTRAIT_STRIDE // SEGMENT_BYTES))
        f.write("#define MSX2_PORTRAIT_CHARS     %d\n" % PORTRAIT_CHARS)
        f.write("#define MSX2_PORTRAIT_W         %d\n" % PORTRAIT_W)
        f.write("#define MSX2_PORTRAIT_H         %d\n" % PORTRAIT_H)
        f.write("#define MSX2_PORTRAIT_LEFT_X    %d\n" % PORTRAIT_LEFT_X)
        f.write("#define MSX2_PORTRAIT_LEFT_Y    %d\n" % PORTRAIT_LEFT_Y)
        f.write("#define MSX2_PORTRAIT_RIGHT_X   %d\n" % PORTRAIT_RIGHT_X)
        f.write("#define MSX2_PORTRAIT_RIGHT_Y   %d\n" % PORTRAIT_RIGHT_Y)
        f.write("#define MSX2_PORTRAIT_MAX_RUNS  %d\n" % PORTRAIT_MAX_RUNS)
        f.write("#define MSX2_PORTRAIT_ROW_STRIDE %d\n" % PORTRAIT_ROW_STRIDE)
        f.write("#define MSX2_PORTRAIT_INDEX_BYTES %d\n" % PORTRAIT_INDEX_BYTES)
        f.write("// (character, lit) -> the segment its skip list starts in.\n")
        f.write("#define MSX2_PORTRAIT_SEG(chr, lit)  (MSX2_PORTRAIT_SEGMENT +\\\n")
        f.write("     (((chr) * 2 + ((lit) ? 0 : 1)) * MSX2_PORTRAIT_SEGS))\n")
        f.write("#define MSX2_TALK_BOX_Y         %d\n" % TALK_BOX_Y)
        f.write("#define MSX2_TALK_NAME_Y        %d\n" % TALK_NAME_Y)
        f.write("#define MSX2_TALK_LINE0_Y       %d\n" % TALK_LINE_Y[0])
        f.write("#define MSX2_TALK_LINE_STEP     %d\n" % (TALK_LINE_Y[1] - TALK_LINE_Y[0]))
        f.write("#define MSX2_TALK_LINES         %d\n" % len(TALK_LINE_Y))
        f.write("#define MSX2_TALK_PROMPT_Y      %d\n" % TALK_PROMPT_Y)
        f.write("#define MSX2_TALK_TEXT_X        %d\n" % TALK_TEXT_X)
        f.write("#define MSX2_TALK_TEXT_W        %d\n" % (WIDTH - 2 * TALK_TEXT_X))
        f.write("#define MSX2_TALK_COLS          %d\n" % ((WIDTH - 2 * TALK_TEXT_X) // 6))
        f.write("#define MSX2_TALK_NAME_X        %d\n" % TALK_NAME_X)
        # The baked panels are flat colours run through the ditherer, so they
        # are not flat bytes on screen -- anything written over one is written
        # over a fill in the quantised colour first, which is what this is for.
        f.write("#define MSX2_PLATE_COLOR        0x%02X\n" % grb.pack(*TALK_PLATE_RGB))
        f.write("#define MSX2_MAP_PANEL_X        %d\n" % MAP_PANEL_X)
        f.write("#define MSX2_MAP_PANEL_Y        %d\n" % MAP_PANEL_Y)
        f.write("#define MSX2_MAP_PANEL_W        %d\n" % MAP_PANEL_W)
        f.write("#define MSX2_MAP_PANEL_H        %d\n\n" % MAP_PANEL_H)

        f.write("// ── String table ───────────────────────────────────────────────────────\n")
        f.write("#define MSX2_TEXT_SEGMENT       %d\n" % text_segment)
        f.write("#define MSX2_NAME_STRIDE        %d\n" % NAME_STRIDE)
        f.write("#define MSX2_NAME_COUNT         %d\n" % name_count)
        f.write("#define MSX2_NAME_OFFSET        %d\n" % text_off["NAME"])
        f.write("#define MSX2_OPP_OFFSET         %d\n" % text_off["OPPONENT"])
        f.write("#define MSX2_OPP_STRIDE         %d\n" % OPP_STRIDE)
        f.write("#define MSX2_LINE_STRIDE        %d\n" % LINE_STRIDE)
        f.write("#define MSX2_LINES_PER_DUEL     %d\n" % LINES_PER_DUEL)
        f.write("#define MSX2_DIALOGUE_OFFSET    %d\n" % text_off["DIALOGUE"])
        f.write("#define MSX2_INTRO_OFFSET       %d\n" % text_off["INTRO"])
        f.write("#define MSX2_INTRO_COUNT        %d\n" % intro_n)
        f.write("#define MSX2_ENDING_OFFSET      %d\n" % text_off["ENDING"])
        f.write("#define MSX2_ENDING_COUNT       %d\n" % ending_n)
        f.write("static const unsigned char g_msx2_dialogue_count[MSX2_STORY_DUELS] =\n")
        f.write("\t{ %s };\n\n" % ", ".join(str(c) for c in dialogue_counts))

        f.write("#define MSX2_SCENE_SEGMENT_FIRST  %d\n" % FIRST_ASSET_SEGMENT)
        f.write("#define MSX2_SCENE_SEGMENT_LAST   %d\n" % (segment - 1))
        f.write("#define MSX2_ASSET_ROM_KB         %d\n" % (segment * SEGMENT_BYTES // 1024))

    if not quiet:
        print("TEXT     %d names + story -> %d bytes, segment %d"
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
