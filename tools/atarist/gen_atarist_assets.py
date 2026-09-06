#!/usr/bin/env python3
"""Bake the Atari ST/STE port's art out of assets/source/.

Everything the ST shows comes from the same paintings every other target
draws.  The problem this tool solves is that an ST low-resolution screen holds
sixteen colours, the duel screen splits those into two sets of sixteen, and the
card art has to survive inside whatever is left after the structural colours --
sky, sand, panel, HUD text -- have taken their entries.

So the palettes are not written by hand here: the structural colours are
sampled from the real textures, and the entries that remain are FITTED to the
card paintings (k-means with the structural colours pinned as fixed centres).
Eight entries a half go to structure -- the two tile photographs and the card
frame on the board side, the panel and the HUD inks on the card side -- two to
black and white, and the remaining SIX are fitted.  The art is then
Floyd-Steinberg dithered against the whole sixteen, so a painting has the
structural ten behind its own six, which is what lets six free entries carry
seventy-eight monsters.

Outputs (--out, default build/atarist/data):

  DAT/ARENA.TEX   128x128 chunky board texture: the sandstone checkerboard
  DAT/FIELD.CRD   79 card faces, 32x32 chunky, ARENA palette (the 3D board)
  DAT/HAND.CRD    79 card faces, 32x24 planar 4bpp, CARD palette (the hand)
  DAT/TITLE.SCR   the title painting: 320x200 planar + one palette per band

  src/generated/atarist_art.h   the two palettes and the sizes above

Chunky bytes are stored ALREADY MULTIPLIED BY FOUR, which is the convention
the C2P and the rasteriser share (see atarist_c2p.S); the loader is then a
plain read with no fix-up pass over 80 KB on an 8 MHz machine.

Previews are written to --preview (build/atarist/preview) by decoding the
binaries back through the same palettes, so the art can be judged without an
emulator and without trusting the code above.

Usage:
    tools/atarist/gen_atarist_assets.py [--out build/atarist/data] [--quiet]
"""

import argparse
import os
import re
import struct
import sys

import numpy as np
from PIL import (Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter,
                 ImageOps)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CARD_DIR = os.path.join(ROOT, "assets", "source", "cards")
CARD_DATA = os.path.join(CARD_DIR, "card_data.txt")
TEXTURE_DIR = os.path.join(ROOT, "assets", "source", "textures")
BG_DIR = os.path.join(ROOT, "assets", "source", "bg")
TITLE_SRC = os.path.join(ROOT, "assets", "source", "title", "title_320x200_atarist.png")
CARD_BACK_SRC = os.path.join(TEXTURE_DIR, "card_texture.png")
# The two tile photographs.  They are not one texture shaded twice any more:
# sandstone_1 IS the light tile and sandstone_2 IS the dark one, each a square
# slab with its own bevelled border, so the checkerboard is two real stones.
SAND_LIGHT_SRC = os.path.join(TEXTURE_DIR, "sandstone_1.png")
SAND_DARK_SRC = os.path.join(TEXTURE_DIR, "sandstone_2.png")
# The card FRONT frames, the same three the PC build uses, and the reference
# file that says where the art window sits inside them (in the template's own
# pixels, which this tool scales to whatever size the ST shows a card at).
FRAME_SRC = [os.path.join(TEXTURE_DIR, "monster_card_front_template.png"),
             os.path.join(TEXTURE_DIR, "spell_card_front_template.png"),
             os.path.join(TEXTURE_DIR, "trap_card_front_template.png")]
FRAME_WINDOWS = os.path.join(TEXTURE_DIR,
                             "card_front_template_pixel_monster_original_res.txt")
HEADER = os.path.join(ROOT, "src", "generated", "atarist_art.h")

# ── The two palettes ─────────────────────────────────────────────────────────
#
# Index assignments are a contract with src/atarist/atarist_assets.h; the names
# below are the ones the C macros carry.  "None" marks an entry this tool is
# free to choose, and those are the entries the card art gets fitted into.

# The board is a FINITE SLAB over black, not an infinite plane: a checkerboard
# of tiles with a groove between them and a thick front rim, which is what the
# MSX2 and PC-FX boards are and what the ST one now matches.  Nothing is spent
# on a sky any more -- the surround is entry 0 -- and the three entries that
# used to hold one went to the card art.
ARENA_SLOTS = ["BLACK", "GROOVE", "TILE_DARK", "TILE_LIGHT", "TILE_LIGHT2",
               "RIM_SIDE", "RIM_TOP", "SLOT",
               None, None, None, None, None, None,
               "WHITE", "HILIGHT"]
CARD_SLOTS = ["BLACK", "PANEL_DARK", "PANEL_MID", "PANEL_LIGHT",
              "GOLD", "RED", "GREEN", "FRAME_STONE",
              None, None, None, None, None, None,
              "WHITE", "YELLOW"]

# HOW THE SIXTEEN ARE SPENT, in both halves and in this order:
#
#   * EIGHT to structure -- the tiles and the card frame in the ARENA half, the
#     panel, the HUD inks and the frame's stone in the CARD half.  These are
#     sampled from the real textures, never invented.
#   * TWO to black and white, which every half needs and neither can fit.
#   * SIX FITTED TO THE PAINTINGS, by k-means with the other ten pinned as
#     fixed centres, so a free entry is only spent where the structural ten
#     serve a painting worst.
#
# and then a card is dithered against ALL SIXTEEN, not against the six.  That
# is the whole strategy: the six carry the hues the sand and the panel cannot
# (skin, hair, the bright accents), and the dither borrows the browns, the
# gold, the panel blues, black and white for everything else -- so a monster
# has ten colours of support behind its own six rather than six alone.
#
# Two earlier cuts of this are recorded because they are worth not repeating:
# an eight-step GREY RAMP at 8..13 (even on the hardware, and every card came
# back a grey blob because it spent no entry on hue at all), and a per-scanline
# palette split five ways across the hand row (three private colours per card
# per line, which is noise plus row-to-row streaking).

# The entries that are pinned rather than sampled: the two the whole screen
# needs, the gold that is both a card frame's rule and the cursor tint, and the
# CARD half's panel and HUD inks.  Everything brown is filled in from the tile
# photographs in `main`.
ARENA_FIXED = {
    "BLACK": (0, 0, 0),
    "SLOT": (224, 176, 56),
    "WHITE": (248, 248, 248),
    "HILIGHT": (248, 232, 96),
}
CARD_FIXED = {
    "BLACK": (0, 0, 0),
    "PANEL_DARK": (24, 24, 40),
    "PANEL_MID": (72, 72, 104),
    "PANEL_LIGHT": (152, 152, 176),
    "GOLD": (224, 176, 56),
    "RED": (208, 64, 64),
    "GREEN": (72, 176, 88),
    # The card frame's stone, so a hand card's frame is the same object the
    # board's cards wear.  Filled in from the tile photographs in `main`.
    "FRAME_STONE": (117, 84, 46),
    "WHITE": (248, 248, 248),
    "YELLOW": (248, 232, 96),
}

ARENA_TEX_W = 128
TEXELS_PER_UNIT = 16         # must equal ATARIST_TEXELS_PER_UNIT
TILE_INSET = 0.14            # how much of a slab photograph is its border
BOARD_COLS, BOARD_ROWS = 5, 4   # must equal ATARIST_COLS / ATARIST_ROWS
FIELD_W = FIELD_H = 32       # 3D board card texture
HAND_W, HAND_H = 32, 24      # hand card art window
TITLE_W, TITLE_H = 320, 200
TITLE_BAND_ROWS = 8          # 25 bands, one palette each
SUPPORT_VARIANTS = 6
# How much of a card's dither error is carried into its neighbours.  Not the
# 1.0 the grey ramp wanted (a ramp's neighbour can always absorb the error) and
# not the ground's damped 0.35 either: sixteen entries fitted to a painting are
# dense in luminance and sparse in hue, so full diffusion overshoots into the
# saturated entries -- the HUD's red and green -- and speckles a grey robe with
# confetti.  0.75 keeps the shading and drops the confetti.
ART_DIFFUSION = 0.75


def quantize_levels(img, n, crop=None, contrast=True):
    """`n` representative colours of an image, dark to light.

    Terciles of luminance rather than k-means: a photographic texture is one
    cluster with a long tail, and k-means on it returns three colours a few
    units apart -- three identical-looking sand tones, and a ground that
    dithers to a flat wash."""
    img = img.convert("RGB")
    if crop:
        img = img.crop(crop)
    # Per-channel autocontrast stretches a texture's tonal range, but it also
    # neutralises a single-hue source (a blue sky comes back grey), so it is
    # off for the calls that only want a colour.
    if contrast:
        img = ImageOps.autocontrast(img, cutoff=3)
    px = np.asarray(img.resize((96, 96)), dtype=np.float64).reshape(-1, 3)
    lum = px @ np.array([0.299, 0.587, 0.114])
    order = np.argsort(lum)
    out = []
    for i in range(n):
        band = order[len(order) * i // n:len(order) * (i + 1) // n]
        out.append(tuple(int(round(v)) for v in px[band].mean(axis=0)))
    return out


def kmeans(px, k, fixed, iters=24):
    """k free centres over `px`, with `fixed` centres competing but never moving.

    This is the whole reason the ST card art reads at all.  Fitting the free
    entries WITHOUT the fixed ones in the assignment spends two of five on
    colours the sand and the panel already provide; with them pinned, the free
    entries go where the paintings actually need help -- skin, hair, the bright
    accents -- and the dither borrows the structural colours for the rest.
    """
    if k <= 0:
        return np.zeros((0, 3))
    rng = np.random.default_rng(12345)
    # k-means++ style seeding on the residual: start from the pixels the fixed
    # palette serves worst, so the first free centre is never a near-duplicate.
    cen = np.zeros((k, 3))
    if len(fixed):
        d = ((px[:, None, :] - fixed[None, :, :]) ** 2).sum(axis=2).min(axis=1)
    else:
        d = np.full(len(px), 1.0)
    for i in range(k):
        p = d / d.sum() if d.sum() > 0 else None
        cen[i] = px[rng.choice(len(px), p=p)]
        nd = ((px - cen[i]) ** 2).sum(axis=1)
        d = np.minimum(d, nd)
    for _ in range(iters):
        all_cen = np.concatenate([cen, fixed]) if len(fixed) else cen
        lab = ((px[:, None, :] - all_cen[None, :, :]) ** 2).sum(axis=2).argmin(axis=1)
        for i in range(k):
            sel = px[lab == i]
            if len(sel):
                cen[i] = sel.mean(axis=0)
    return cen


def fit_palette(slots, fixed_map, sample):
    """Fill the None entries of `slots` from `sample` pixels."""
    fixed = np.array([fixed_map[s] for s in slots if s is not None], dtype=np.float64)
    free_idx = [i for i, s in enumerate(slots) if s is None]
    cen = kmeans(sample, len(free_idx), fixed)
    order = np.argsort(cen.sum(axis=1))
    pal = [None] * 16
    for i, s in enumerate(slots):
        if s is not None:
            pal[i] = fixed_map[s]
    for n, i in enumerate(free_idx):
        pal[i] = tuple(int(max(0, min(255, round(v)))) for v in cen[order[n]])
    return pal


# ── Dither ───────────────────────────────────────────────────────────────────

def dither(img, palette, allow=None, strength=0.30):
    """Floyd-Steinberg an RGB image into palette indices.

    `allow` restricts the usable entries (the ground uses only its sand tones;
    letting it reach for the sky blues put blue speckle in the sand).

    `strength` scales the diffused error.  It is DAMPED for a palette of
    scattered colours -- the ground's sand tones -- because a large error
    carried into a neighbour that has no near colour to absorb it reads as
    noise rather than as shading.  A card takes ART_DIFFUSION, between the two:
    its sixteen entries are dense enough in luminance to carry an error along
    but sparse enough in hue that full diffusion overshoots into the saturated
    ones and speckles the picture."""
    pal = np.array(palette, dtype=np.float64)
    idx = list(range(16)) if allow is None else list(allow)
    sub = pal[idx]
    src = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w = src.shape[:2]
    out = np.zeros((h, w), dtype=np.uint8)
    # SERPENTINE, alternate rows right to left.  A one-directional scan carries
    # its error the same way on every row, and on a thirty-two pixel card that
    # shows as a diagonal drift across the picture; reversing every other row
    # cancels it.
    for y in range(h):
        row = src[y]
        xs = range(w) if not (y & 1) else range(w - 1, -1, -1)
        step = 1 if not (y & 1) else -1
        for x in xs:
            old = row[x]
            k = int(((sub - old) ** 2).sum(axis=1).argmin())
            out[y, x] = idx[k]
            err = (old - sub[k]) * strength
            nx = x + step
            if 0 <= nx < w:
                row[nx] += err * (7 / 16.0)
            if y + 1 < h:
                if 0 <= x - step < w:
                    src[y + 1][x - step] += err * (3 / 16.0)
                src[y + 1][x] += err * (5 / 16.0)
                if 0 <= nx < w:
                    src[y + 1][nx] += err * (1 / 16.0)
    return out


def to_planar(idx):
    """Palette indices -> ST interleaved bitplanes, 4 words per 16 pixels."""
    h, w = idx.shape
    assert w % 16 == 0
    words = bytearray()
    for y in range(h):
        for g in range(w // 16):
            block = idx[y, g * 16:(g + 1) * 16]
            for p in range(4):
                word = 0
                for i, v in enumerate(block):
                    if (int(v) >> p) & 1:
                        word |= 0x8000 >> i
                words += struct.pack(">H", word)
    return bytes(words)


def st3(c):
    """What a plain ST actually shows: three bits a channel.

    Every structural colour is checked against this, because the board's browns
    do not survive the quantisation by accident.  RIM_SIDE was (125,102,55) and
    came back OLIVE: 125 and 102 land in the same three-bit bucket, red stops
    leading green, and the slab's front face turned into a green plank on the
    hardware while looking correct in every 24-bit preview this tool writes."""
    return tuple((int(max(0, min(255, round(v)))) >> 5) * 255 // 7 for v in c)


def art_prep(img, flat=False):
    """A painting made ready for a sixteen-colour dither.

    Three steps, each measured against its alternatives on a sheet of eight
    cards.  A stretch rather than an equalisation: with six fitted colours plus
    ten structural ones the dark end of a painting has somewhere to go, and
    equalising -- which the grey-ramp version of this file needed -- flattens
    the tonal massing that tells a monster from its background.  A small lift
    afterwards, because these paintings are dark.  Then BLUR, which is the
    counter-intuitive one and the one that makes cards readable: a painting
    carries far more detail than thirty-two pixels can hold, and detail finer
    than the grid becomes dither noise that reads as static.  Low-passing
    first leaves flat regions the palette can actually hold, so the monster
    keeps its silhouette."""
    if flat:
        # A support sigil and the card back are DRAWN, not photographed: a few
        # flat tones on a flat field.  Blurring one rounds its shape off and
        # all six supports came back as the same blob.
        return ImageOps.autocontrast(img.convert("RGB"), cutoff=1)
    out = ImageOps.autocontrast(img.convert("RGB"), cutoff=1)
    # Saturation UP and contrast slightly DOWN, which sounds backwards and is
    # not.  Six of the sixteen entries are fitted to these paintings, so hue is
    # the thing the palette can actually reproduce and it is worth pushing;
    # contrast is the thing a sixteen-colour dither exaggerates by itself, and
    # a straight autocontrast plus a brightness lift bleached every card to
    # white-on-blue -- the whole sheet came back looking sun-struck.
    out = ImageEnhance.Color(out).enhance(1.25)
    out = ImageEnhance.Contrast(out).enhance(0.92)
    return out.filter(ImageFilter.GaussianBlur(0.5))


def preview(idx, palette):
    pal = np.array(palette, dtype=np.uint8)
    return Image.fromarray(pal[idx])


# ── Card sources ─────────────────────────────────────────────────────────────
#
# The crop rules are the ones tools/gen_assets.py and the MSX2 generator use,
# so an ST card frames its monster the same way every other target does.

def parse_cards():
    cards = []
    with open(CARD_DATA) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if parts[0] == "card":
                cards.append(parts[1])
    return cards


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
    """The monster, cropped to its content and framed FACE-high.

    The content box is then pulled in again -- a fifth off each side and a
    quarter off the bottom -- before the fit.  A full-body painting scaled
    into thirty-two pixels spends most of them on a torso the dither cannot
    hold anyway, and every card came back as the same dark column; cropping
    in on the head first is what makes one monster tell from another at this
    size.  The trim is deliberately not symmetric: the legs are worth less
    than the shoulders, so the bottom loses more than the sides.

    Contrast is pushed before the dither, not after: a 32-pixel thumbnail that
    keeps the original's midtone range dithers into an even mush of the two
    nearest palette entries, and the monster stops being a shape."""
    l, t, r, b = card_content_bbox(img)
    cw, ch = r - l, b - t
    l += int(cw * 0.24)
    r -= int(cw * 0.24)
    b -= int(ch * 0.33)
    src = img.crop((l, t, max(l + 1, r), max(t + 1, b))).convert("RGB")
    out = ImageOps.fit(src, size, method=Image.Resampling.LANCZOS,
                       centering=(0.5, 0.06))
    out = ImageOps.autocontrast(out, cutoff=2)
    # Desaturate a little, lift the midtones, and then BLUR before the dither.
    # The blur is the counter-intuitive one and it is what makes the cards
    # readable: these paintings carry far more detail than 32 pixels can hold,
    # and sharp detail at this scale becomes high-frequency dither noise that
    # reads as static.  Low-passing first leaves flat regions the palette can
    # actually hold, so the monster keeps its silhouette.
    out = ImageEnhance.Color(out).enhance(0.8)
    out = ImageEnhance.Brightness(out).enhance(1.1)
    return out.filter(ImageFilter.GaussianBlur(0.7))


SUPPORT_TINTS = [
    ((60, 150, 208), (14, 40, 66), (38, 108, 164)),    # equip  - arcane blue
    ((88, 176, 120), (12, 46, 30), (44, 118, 76)),     # guard  - jade
    ((150, 140, 216), (28, 24, 60), (96, 88, 160)),    # draw   - violet
    ((236, 196, 96), (58, 40, 12), (176, 138, 48)),    # heal   - amber
    ((236, 128, 72), (58, 20, 10), (176, 76, 34)),     # storm  - ember
    ((196, 84, 108), (52, 14, 26), (140, 46, 68)),     # trap   - garnet
]


def support_art(kind, size):
    """A sigil per support kind.  The six supports are the only cards with no
    painting of their own, so this is the same emblem the MSX2 fork draws."""
    w, h = size
    em = Image.new("RGB", (64, 64), SUPPORT_TINTS[kind][1])
    d = ImageDraw.Draw(em)
    bright, _, mid = SUPPORT_TINTS[kind]
    cx = cy = 31.5
    if kind in (0, 1):                                  # equip / guard: shield
        d.polygon([(cx, 4), (58, 14), (54, 54), (cx, 60), (10, 54), (6, 14)],
                  fill=mid, outline=bright)
        # The two shields must differ by SHAPE, not by tint.  They were told
        # apart by arcane blue against jade, and the grey-ramp cut of the card
        # art threw the tint away: equip and guard came back as the same
        # emblem.  The palette carries hue again, but a shape costs nothing.
        if kind == 0:
            d.line([cx, 12, cx, 52], fill=bright, width=4)      # equip: a bar
        else:
            d.line([14, cy - 2, 50, cy - 2], fill=bright, width=4)
            d.line([cx, 12, cx, 24], fill=bright, width=4)      # guard: a cross
    elif kind == 2:                                     # draw: stacked cards
        for k, off in enumerate((12, 6, 0)):
            d.rectangle([10 + off, 12 + off, 44 + off, 52 + off],
                        fill=mid if k < 2 else bright, outline=bright)
    elif kind == 3:                                     # heal: a cross
        d.rectangle([cx - 6, 10, cx + 6, 54], fill=bright)
        d.rectangle([10, cy - 6, 54, cy + 6], fill=bright)
    elif kind == 4:                                     # storm: a bolt
        d.polygon([(cx + 8, 6), (cx - 10, cy + 4), (cx - 2, cy + 4),
                   (cx - 8, 58), (cx + 12, cy - 6), (cx + 2, cy - 6)],
                  fill=bright)
    else:                                               # trap: an eye
        d.ellipse([6, cy - 14, 58, cy + 14], fill=mid, outline=bright)
        d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=bright)
        d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=(12, 12, 16))
    return em.resize((w, h), Image.Resampling.LANCZOS)


def card_back_art(size):
    """The cover painting, fitted to the whole rect the way every other target
    maps this file over a card back."""
    img = Image.open(CARD_BACK_SRC).convert("RGB").resize(
        size, Image.Resampling.LANCZOS)
    # WARMED INTO THE BOARD'S BROWNS.  The painting is a navy field with a gold
    # compass on it, and a face-down card is shown lying among the tiles: navy
    # among sandstone reads as a hole in the board.  The channel scale turns
    # the field brown and leaves the gold gold, which is the same trick the
    # ground uses to keep sandstone from reading as concrete.
    # No autocontrast anywhere on this path.  Per-channel stretching is what
    # NEUTRALISES a single-hue picture, so running the warmed back through the
    # flat-art preparation put the navy straight back.
    img = ImageEnhance.Brightness(img).enhance(1.25)
    r, g, b = img.split()
    img = Image.merge("RGB", (r.point(lambda v: min(255, int(v * 1.35))),
                              g.point(lambda v: min(255, int(v * 1.05))),
                              b.point(lambda v: int(v * 0.5))))
    return img.filter(ImageFilter.SHARPEN)


def frame_windows():
    """The art, stat and level windows inside a card front, as fractions.

    Read out of the template's own reference file, exactly the way the PC
    build reads it: every integer in the file in order, as four corners each
    for the art window, the ATK/DEF band and the level-ankh band.  Fractions,
    not pixels, because the ST shows a card at thirty-two pixels and the
    template is 1122 x 1402."""
    with open(FRAME_WINDOWS) as f:
        nums = [int(n) for n in re.findall(r"\d+", f.read())]
    w, h = Image.open(FRAME_SRC[0]).size
    out = []
    for i in range(0, 12, 4):
        x0, y0, x1, y1 = nums[i:i + 4]
        out.append((x0 / w, y0 / h, x1 / w, y1 / h))
    return out                                  # art, stats, stars


def framed_card(art, size, kind=0, flat=False):
    """A card front: the template's frame with the painting in its window.

    The frame is squashed to `size` rather than letterboxed, and that is
    correct rather than sloppy: on the board a card is a 0.70 x 0.90 quad, so a
    square texture stretched over it comes back at the template's own 0.80
    aspect.  The level-ankh band is left as bare stone -- at thirty-two pixels
    a card the band is two pixels tall and an ankh row in it would be one grey
    smear -- and the ATK/DEF band likewise, since the hand draws those numbers
    in the HUD font underneath.

    THE ART IS PREPARED AT THE SIZE IT LANDS AT, and the frame is not prepared
    at all.  Blurring and stretching the composite instead -- which is what
    this did first -- softens the frame's gold rules into brown smears, and
    those rules are the only thing that says "card" at twenty-four pixels
    across."""
    base = Image.open(FRAME_SRC[kind]).convert("RGB").resize(
        size, Image.Resampling.LANCZOS)
    fx0, fy0, fx1, fy1 = frame_windows()[0]
    x0, y0 = int(round(fx0 * size[0])), int(round(fy0 * size[1]))
    x1, y1 = int(round(fx1 * size[0])), int(round(fy1 * size[1]))
    x1, y1 = max(x0 + 1, x1), max(y0 + 1, y1)
    win = ImageOps.fit(art.convert("RGB"), (x1 - x0, y1 - y0),
                       method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    base.paste(art_prep(win, flat), (x0, y0))
    return base


# The support kinds, in SUPPORT_TINTS order, mapped onto the PC's three card
# classes: everything is a spell except the trap.
SUPPORT_FRAME = [1, 1, 1, 1, 1, 2]


def card_faces(cards):
    """Every face, in card-id order: monsters, the six supports, then the back
    (which is also what a face-down card shows)."""
    faces = []
    for asset_id in cards:
        faces.append(("m:" + asset_id, Image.open(find_card_image(asset_id))))
    for kind in range(SUPPORT_VARIANTS):
        faces.append(("s:%d" % kind, None))
    faces.append(("back", None))
    return faces


def face_art(tag, src, size):
    """What goes INSIDE a card's frame: the painting, or a drawn sigil."""
    if tag.startswith("m:"):
        return card_thumb(src, size)
    if tag.startswith("s:"):
        return support_art(int(tag[2:]), size)
    return card_back_art(size)


def face_image(tag, src, size):
    """A whole card at `size`: frame plus art.  The back has no frame -- it is
    the frame, on its other side -- so it is the one face prepared here."""
    if tag == "back":
        return card_back_art(size)
    kind = 0 if tag.startswith("m:") else SUPPORT_FRAME[int(tag[2:])]
    # The art is drawn at four times the window it lands in and resampled down
    # by the frame: fitting straight into a twenty-four pixel window throws
    # away the detail the LANCZOS kernel needs to keep an edge.
    return framed_card(face_art(tag, src, (size[0] * 4, size[1] * 4)),
                       size, kind, flat=not tag.startswith("m:"))


def hand_image(tag, src, size):
    """The hand's window: the same card, cropped to its art window plus the
    frame's inner rule.  The hand slot is landscape and only sixty-four pixels
    across, with the ATK/DEF numbers drawn under it in the HUD font, so it
    cannot hold a whole portrait card -- what it shows is the picture inside
    its gold rule, which is the part of the frame that reads at this size."""
    if tag == "back":
        return card_back_art(size)
    tall = (size[0], int(round(size[0] * 1.25)))     # the card's own aspect
    card = face_image(tag, src, (tall[0] * 4, tall[1] * 4))
    fx0, fy0, fx1, fy1 = frame_windows()[0]
    m = 0.045                                        # keep the gold rule
    box = (0, max(0, int((fy0 - m) * card.height)),
           card.width, min(card.height, int((fy1 + m) * card.height)))
    return card.crop(box).resize(size, Image.Resampling.LANCZOS)


# ── The ground ───────────────────────────────────────────────────────────────

def tile_cell(path, pal, allow, t):
    """One board tile: the photograph itself, dithered into its own tones.

    The stone is REAL now.  The drawn tile this replaces -- a flat face, a
    baked bevel and a hash grain -- existed because the first attempt tiled a
    128x128 crop of one photograph across the board and picked up its
    large-scale luminance drift, so some light tiles dithered almost entirely
    to the darker of their two tones and the board came out blotchy.  A whole
    slab resampled to sixteen texels has no drift left to pick up: what
    survives is the bevelled border and the cracks, which is exactly the
    structure a tile needs, and the two photographs differ in tone by
    themselves so nothing has to be shaded to make a checkerboard.

    Resampled with BOX, then the contrast pushed: a mean over eighty source
    pixels a texel is a smooth stone, and the dither has to be given back the
    tonal range that averaging took away or the tile is a flat wash."""
    img = Image.open(path).convert("RGB")
    # Crop to the slab's INNER FIELD.  Both photographs are a stone inside a
    # bevelled border, and at sixteen texels that border is two texels of the
    # tile on every side: the board came out as a wall of picture frames.  The
    # groove the checkerboard draws between tiles is the border it needs.
    w, h = img.size
    img = img.crop((int(w * TILE_INSET), int(h * TILE_INSET),
                    int(w * (1 - TILE_INSET)), int(h * (1 - TILE_INSET))))
    img = ImageOps.autocontrast(img, cutoff=3)
    # Tone from a BOX average, GRAIN from a point sample, mixed.  A mean over
    # eighty source pixels a texel is a perfectly smooth stone -- the first
    # version of this was that mean alone and the tiles came back as flat
    # brown squares.  A point sample of the same photograph at the same grid
    # is a real stone pixel, so the two together are the slab's tone with the
    # slab's own speckle on it, and neither is invented noise.
    tone = img.resize((t, t), Image.Resampling.BOX)
    grain = img.resize((t, t), Image.Resampling.NEAREST)
    img = Image.blend(tone, grain, 0.30)
    img = ImageEnhance.Contrast(img).enhance(1.35)
    # DAMPED diffusion, as everywhere else that a scattered palette is the
    # target: four tile tones are not a ramp, and a large error carried into a
    # neighbour with no near colour to absorb it reads as noise, not shading.
    return dither(img, pal, allow=allow, strength=0.35)


def build_arena_texture(pal, light, dark):
    """The board slab's top surface: a checkerboard of sandstone tiles.

    THE BOARD IS CENTRED ON TEXEL (0,0), NOT ON THE TEXTURE'S MIDDLE: the plane
    rasteriser samples u = world_x * texels with no origin offset.  Building
    the markings around the centre instead puts the playfield eight world units
    away, out by the horizon.

    Tiles are one world unit square, so a tile is TEXELS_PER_UNIT across.  The
    five columns are centred on the origin and the four rows straddle it, which
    puts the column edges at texel 8 (mod 16) and the row edges at 0 (mod 16).
    A one-texel groove on each edge is what separates the tiles; at the near
    end of the board that groove is three screen pixels, which is the dark line
    the MSX2 and PC-FX boards read by."""
    t = TEXELS_PER_UNIT
    n = ARENA_TEX_W // t
    light = np.tile(light, (n, n))
    dark = np.tile(dark, (n, n))
    idx = np.zeros((ARENA_TEX_W, ARENA_TEX_W), dtype=np.uint8)
    for y in range(ARENA_TEX_W):
        cz = y if y < ARENA_TEX_W // 2 else y - ARENA_TEX_W
        gy = cz % t
        row = cz // t
        for x in range(ARENA_TEX_W):
            cx = x if x < ARENA_TEX_W // 2 else x - ARENA_TEX_W
            gx = (cx - t // 2) % t
            col = (cx - t // 2) // t
            if gx == 0 or gy == 0:
                idx[y, x] = 1                              # GROOVE
            elif (col + row) & 1:
                idx[y, x] = dark[y, x]
            else:
                idx[y, x] = light[y, x]
    return idx


# ── Title ────────────────────────────────────────────────────────────────────

def build_title(quiet):
    """The title painting, one palette per 8-row band.

    A band is its own 16-colour picture, so the screen carries 25 palettes and
    several hundred colours.  Three entries are pinned in every band -- black,
    the menu highlight and white -- because the text is drawn over the picture
    and cannot ask which band a glyph landed in."""
    src = Image.open(TITLE_SRC).convert("RGB")
    if src.size != (TITLE_W, TITLE_H):
        src = src.resize((TITLE_W, TITLE_H), Image.Resampling.LANCZOS)
    bands = TITLE_H // TITLE_BAND_ROWS
    idx = np.zeros((TITLE_H, TITLE_W), dtype=np.uint8)
    pals = []
    for b in range(bands):
        y0 = b * TITLE_BAND_ROWS
        strip = src.crop((0, y0, TITLE_W, y0 + TITLE_BAND_ROWS))
        px = np.asarray(strip, dtype=np.float64).reshape(-1, 3)
        fixed = np.array([(0, 0, 0), (248, 232, 96), (248, 248, 248)],
                         dtype=np.float64)
        cen = kmeans(px, 13, fixed)
        order = np.argsort(cen.sum(axis=1))
        pal = [(0, 0, 0)]
        pal += [tuple(int(max(0, min(255, round(v)))) for v in cen[o]) for o in order]
        pal.append((248, 232, 96))
        pal.append((248, 248, 248))
        idx[y0:y0 + TITLE_BAND_ROWS] = dither(strip, pal)
        pals.append(pal)
    if not quiet:
        print("TITLE    %d bands x 16 colours" % bands)
    return idx, pals


# ── Emit ─────────────────────────────────────────────────────────────────────

def write(path, data, quiet, what):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    if not quiet:
        print("%-8s %-28s %6d bytes" % (what, os.path.relpath(path, ROOT), len(data)))


def pal_c(name, pal, slots):
    out = ["static const uint8_t %s[16][3] = {" % name]
    for i, c in enumerate(pal):
        label = slots[i] if slots[i] else "art"
        out.append("    {%3d, %3d, %3d},   /* %2d %s */" % (c[0], c[1], c[2], i, label))
    out.append("};")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "atarist", "data"))
    ap.add_argument("--preview",
                    default=os.path.join(ROOT, "build", "atarist", "preview"),
                    help="where the decoded previews go; kept OUT of --out, "
                         "which is copied onto the floppy verbatim")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    quiet = args.quiet
    dat = os.path.join(args.out, "DAT")
    prev = args.preview
    os.makedirs(prev, exist_ok=True)

    cards = parse_cards()
    faces = card_faces(cards)

    # The sample the six free entries are fitted to is the PAINTINGS ONLY, at
    # the size the window inside a card frame shows them.  Fitting on the
    # composited card instead spends free entries on the frame's stone and
    # gold, which the structural ten already hold exactly.
    sample = []
    thumbs = []
    win = frame_windows()[0]
    wx = int(round((win[2] - win[0]) * FIELD_W))
    wy = int(round((win[3] - win[1]) * FIELD_H))
    for tag, src in faces:
        thumbs.append((tag, face_image(tag, src, (FIELD_W, FIELD_H))))
        art = face_art(tag, src, (wx * 4, wy * 4))
        art = ImageOps.fit(art.convert("RGB"), (wx, wy),
                           method=Image.Resampling.LANCZOS)
        sample.append(np.asarray(art_prep(art, not tag.startswith("m:")),
                                 dtype=np.float64).reshape(-1, 3))
    sample = np.concatenate(sample)

    arena_fixed = dict(ARENA_FIXED)
    card_fixed = dict(CARD_FIXED)

    def keep_hue(c):
        """Nudge a tone until it is still a BROWN after `st3`.

        Three bits a channel is a coarse grid and sandstone sits right on the
        edge of it: RIM_SIDE was (125,102,55) once and came back OLIVE, because
        125 and 102 land in the same bucket and red stops leading green.  The
        same happens at the blue end -- a stone a few units off neutral
        quantises to a pink -- so both orderings are enforced here, r > g > b in
        the three-bit grid, and the picture on the hardware is a brown."""
        r, g, b = (int(v) for v in c)
        while (r >> 5) <= (g >> 5) and g > 0:
            g -= 8
        while (g >> 5) <= (b >> 5) and b > 0:
            b -= 8
        return (r, max(0, g), max(0, b))

    def shade(c, k):
        return tuple(int(max(0, min(255, round(v * k)))) for v in c)

    # THE TILES ARE TWO PHOTOGRAPHS, not one photograph shaded twice.
    # sandstone_1 is the light slab and sandstone_2 the dark one, each already
    # a square stone with its own bevelled border, so the checkerboard's
    # contrast is the stones' own and no tone has to be invented for it.  Three
    # luminance terciles of each, spread from their own mean, because a
    # low-contrast photograph's terciles land within a few units of each other
    # and three near-identical entries make a tile a flat wash.
    def terciles(path, spread):
        # contrast=False: per-channel autocontrast stretches a texture's range
        # but NEUTRALISES a single-hue source, and both slabs are one hue.  It
        # came back as a pink stone the first time this was run.
        img = Image.open(path)
        w, h = img.size
        lv = quantize_levels(img, 3, contrast=False,
                             crop=(int(w * TILE_INSET), int(h * TILE_INSET),
                                   int(w * (1 - TILE_INSET)),
                                   int(h * (1 - TILE_INSET))))
        mid = tuple(sum(c[i] for c in lv) / 3.0 for i in range(3))
        return [keep_hue(tuple(int(max(0, min(255, round(
                    mid[i] + (c[i] - mid[i]) * spread)))) for i in range(3)))
                for c in lv]

    def separate(tones):
        """Push a stone's three tones apart until the HARDWARE can tell them.

        Three bits a channel is 512 colours, and two tones of one photograph
        forty units apart routinely land in the same bucket -- TILE_LIGHT and
        TILE_LIGHT2 came back identical, which costs a palette entry and makes
        the tile flat.  Walking up from the darkest and lifting each tone until
        `st3` differs from the one below is cheap and keeps the stone's hue."""
        out = [tones[0]]
        for c in tones[1:]:
            c = list(c)
            for _ in range(24):
                if st3(tuple(c)) != st3(out[-1]):
                    break
                c = [min(255, v + 10) for v in c]
            out.append(keep_hue(tuple(c)))
        return out

    lo = separate(terciles(SAND_LIGHT_SRC, 2.0))    # dark, mid, bright
    # The dark slab is darkened again on top of being the darker photograph.
    # The two stones as shot are only about fifty units apart in luminance, and
    # a checkerboard that close reads as one texture with a seam in it.
    hi = separate([shade(c, 0.72) for c in terciles(SAND_DARK_SRC, 2.0)])

    # Six of the eight structural entries are the two stones; the other two are
    # the gold (a card frame's rule and the cursor tint) and the highlight.
    # RIM_SIDE and RIM_TOP double as the slab's front face and its lit top
    # edge, which is what stops the board reading as a rug.
    arena_fixed["TILE_LIGHT"] = lo[1]
    arena_fixed["TILE_LIGHT2"] = lo[0]
    arena_fixed["RIM_TOP"] = lo[2]
    arena_fixed["TILE_DARK"] = hi[1]
    arena_fixed["RIM_SIDE"] = hi[2]
    arena_fixed["GROOVE"] = keep_hue(shade(hi[0], 0.55))
    # The hand's frame stone: the dark slab's mid tone, so a card in hand wears
    # the same stone the board's cards do.
    card_fixed["FRAME_STONE"] = hi[1]
    if not quiet:
        for name in ("TILE_LIGHT", "TILE_LIGHT2", "TILE_DARK", "GROOVE",
                     "RIM_SIDE", "RIM_TOP"):
            print("ARENA    %-11s %-16s -> ST %s"
                  % (name, arena_fixed[name], st3(arena_fixed[name])))

    arena_pal = fit_palette(ARENA_SLOTS, arena_fixed, sample)
    card_pal = fit_palette(CARD_SLOTS, card_fixed, sample)

    # Ground.  Each tile keeps to its own stone's tones plus the groove, so a
    # light tile cannot speckle with the dark tile's brown and vice versa.
    # A TILE MAY ONLY REACH FOR ITS OWN STONE.  Letting the dark tile borrow
    # the light stone's tones (they are the brighter half of the palette, so
    # the dither wants them) turned the two tiles into the same tile and the
    # checkerboard disappeared.
    tile_light = tile_cell(SAND_LIGHT_SRC, arena_pal,
                           [ARENA_SLOTS.index(n) for n in
                            ("TILE_LIGHT2", "TILE_LIGHT", "RIM_TOP")],
                           TEXELS_PER_UNIT)
    tile_dark = tile_cell(SAND_DARK_SRC, arena_pal,
                          [ARENA_SLOTS.index(n) for n in
                           ("GROOVE", "TILE_DARK", "RIM_SIDE")],
                          TEXELS_PER_UNIT)
    ground = build_arena_texture(arena_pal, tile_light, tile_dark)
    write(os.path.join(dat, "ARENA.TEX"), (ground << 2).astype(np.uint8).tobytes(),
          quiet, "GROUND")
    preview(ground, arena_pal).resize((256, 256), Image.Resampling.NEAREST).save(
        os.path.join(prev, "arena_tex.png"))

    # Card faces: chunky for the 3D board, planar for the hand.
    field = bytearray()
    hand = bytearray()
    sheet_f = Image.new("RGB", (FIELD_W * 10, FIELD_H * ((len(faces) + 9) // 10)))
    sheet_h = Image.new("RGB", (HAND_W * 10, HAND_H * ((len(faces) + 9) // 10)))
    # ALL SIXTEEN ENTRIES ARE ALLOWED TO A CARD.  The six fitted ones carry
    # the hues nothing else in the palette has, and the dither reaches for the
    # structural ten -- the tile browns, the frame's gold, the panel blues,
    # black and white -- for everything they already serve.  The alternative,
    # penning the art into its own entries, is what the grey-ramp version did,
    # and a card then had six colours instead of sixteen.
    #
    # FULL-strength error diffusion, unlike the ground's damped 0.35: sixteen
    # entries fitted to these paintings are dense enough that the error one
    # pixel cannot hold is genuinely what its neighbour can, and damping it
    # only posterises the result.
    for i, (tag, img) in enumerate(thumbs):
        fi = dither(img, arena_pal, strength=ART_DIFFUSION)
        field += (fi << 2).astype(np.uint8).tobytes()
        sheet_f.paste(preview(fi, arena_pal), ((i % 10) * FIELD_W, (i // 10) * FIELD_H))

        tag2, src = faces[i]
        hi = dither(hand_image(tag2, src, (HAND_W, HAND_H)), card_pal,
                    strength=ART_DIFFUSION)
        hand += to_planar(hi)
        sheet_h.paste(preview(hi, card_pal), ((i % 10) * HAND_W, (i // 10) * HAND_H))
    write(os.path.join(dat, "FIELD.CRD"), bytes(field), quiet, "FIELD")
    write(os.path.join(dat, "HAND.CRD"), bytes(hand), quiet, "HAND")
    sheet_f.save(os.path.join(prev, "field_cards.png"))
    sheet_h.save(os.path.join(prev, "hand_cards.png"))

    # Title.
    tidx, tpals = build_title(quiet)
    blob = bytearray()
    blob += struct.pack(">4sHHHH", b"STSC", TITLE_W, TITLE_H,
                        len(tpals), TITLE_BAND_ROWS)
    for b, pal in enumerate(tpals):
        blob += struct.pack(">H", b * TITLE_BAND_ROWS)
        for c in pal:
            blob += bytes(c)
    blob += to_planar(tidx)
    write(os.path.join(dat, "TITLE.SCR"), bytes(blob), quiet, "TITLE")
    tprev = Image.new("RGB", (TITLE_W, TITLE_H))
    for b, pal in enumerate(tpals):
        y0 = b * TITLE_BAND_ROWS
        tprev.paste(preview(tidx[y0:y0 + TITLE_BAND_ROWS], pal), (0, y0))
    tprev.save(os.path.join(prev, "title.png"))

    # The header.  Palettes travel as 8-bit RGB and the running machine packs
    # them, because one asset set has to serve the 3-bit ST and the 4-bit STE.
    with open(HEADER, "w") as f:
        f.write("""/* Generated by tools/atarist/gen_atarist_assets.py -- do not edit.
 *
 * The two sixteen-colour sets the duel screen's raster split shows, and the
 * shapes of the art files on the floppy.  Each set is EIGHT structural entries
 * (the tile stones and the card frame; the panel and the HUD inks) plus black
 * and white, and the SIX marked `art` were FITTED to the card paintings with
 * those ten pinned.  A card is then dithered against all sixteen, so it has
 * the structural ten behind its own six.
 */
#ifndef WAIFU_ATARIST_ART_H
#define WAIFU_ATARIST_ART_H

""")
        f.write("#define ATARIST_ART_FACES      %d\n" % len(faces))
        f.write("#define ATARIST_ART_MONSTERS   %d\n" % len(cards))
        f.write("#define ATARIST_ART_SUPPORTS   %d\n" % SUPPORT_VARIANTS)
        f.write("#define ATARIST_ART_FACE_BACK  %d\n" % (len(faces) - 1))
        f.write("#define ATARIST_ART_FIELD_W    %d\n" % FIELD_W)
        f.write("#define ATARIST_ART_FIELD_H    %d\n" % FIELD_H)
        f.write("#define ATARIST_ART_HAND_W     %d\n" % HAND_W)
        f.write("#define ATARIST_ART_HAND_H     %d\n" % HAND_H)
        f.write("#define ATARIST_ART_ARENA_W    %d\n" % ARENA_TEX_W)
        f.write("#define ATARIST_ART_TITLE_BAND %d\n\n" % TITLE_BAND_ROWS)
        f.write(pal_c("g_atarist_arena_rgb", arena_pal, ARENA_SLOTS))
        f.write("\n\n")
        f.write(pal_c("g_atarist_card_rgb", card_pal, CARD_SLOTS))
        f.write("\n\n#endif /* WAIFU_ATARIST_ART_H */\n")
    if not quiet:
        print("HEADER   %-28s" % os.path.relpath(HEADER, ROOT))
        print("total on floppy: %d bytes" %
              (len(field) + len(hand) + len(blob) + ARENA_TEX_W * ARENA_TEX_W))


if __name__ == "__main__":
    main()
