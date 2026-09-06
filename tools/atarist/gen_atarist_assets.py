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
The art is then Floyd-Steinberg dithered against the whole sixteen, which is
what lets five or seven free entries carry seventy-eight monsters.

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
SAND_SRC = os.path.join(TEXTURE_DIR, "sandstone_1.png")
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
               "ART_K1", "ART_K2", "ART_K3", "ART_K4", "ART_K5", "ART_K6",
               "WHITE", "HILIGHT"]
CARD_SLOTS = ["BLACK", "PANEL_DARK", "PANEL_MID", "PANEL_LIGHT",
              "GOLD", "RED", "GREEN", None,
              "ART_K1", "ART_K2", "ART_K3", "ART_K4", "ART_K5", "ART_K6",
              "WHITE", "YELLOW"]

# THE CARD PAINTINGS ARE DRAWN IN EIGHT GREYS, in both palettes and at both
# sizes: black, six greys, white.  Colour was tried twice and lost twice.
# Fitting five to seven free entries across seventy-eight paintings gives every
# card the same washed blue-grey, because that is the average of seventy-eight
# paintings; and fitting three entries PER SLOT PER SCANLINE -- a palette
# reloaded on every line of the hand row, which is what the shifter can
# actually do -- buys colour at the price of a three-colour picture per line,
# and a monster in three colours a line is noise.  A grey ramp spends no entry
# on hue and every entry on tone, and the ramp is the same on every card, so
# the hand reads as a row of cards rather than a colour chart.  It leaves the
# other eight entries whole for the board, which is the half of the screen
# that does need colour.
#
# THE SIX ARE THE ST'S OWN LEVELS.  Three bits a channel is eight greys --
# 0, 36, 73, 109, 145, 182, 218, 255 -- so black, these six and white are an
# exactly even eight-step ramp on the hardware, with no two steps landing in
# the same bucket and none of the banding a hand-picked ramp gets.
ART_GREYS = [(36, 36, 36), (73, 73, 73), (109, 109, 109),
             (145, 145, 145), (182, 182, 182), (218, 218, 218)]
ART_GREY_FIXED = dict(("ART_K%d" % (i + 1), c) for i, c in enumerate(ART_GREYS))

ARENA_FIXED = {
    "BLACK": (0, 0, 0),
    "SLOT": (224, 176, 56),
    "WHITE": (248, 248, 248),
    "HILIGHT": (248, 232, 96),
}
ARENA_FIXED.update(ART_GREY_FIXED)
CARD_FIXED = {
    "BLACK": (0, 0, 0),
    "PANEL_DARK": (24, 24, 40),
    "PANEL_MID": (72, 72, 104),
    "PANEL_LIGHT": (152, 152, 176),
    "GOLD": (224, 176, 56),
    "RED": (208, 64, 64),
    "GREEN": (72, 176, 88),
    "WHITE": (248, 248, 248),
    "YELLOW": (248, 232, 96),
}
CARD_FIXED.update(ART_GREY_FIXED)

ARENA_TEX_W = 128
TEXELS_PER_UNIT = 16         # must equal ATARIST_TEXELS_PER_UNIT
BOARD_COLS, BOARD_ROWS = 5, 4   # must equal ATARIST_COLS / ATARIST_ROWS
FIELD_W = FIELD_H = 32       # 3D board card texture
HAND_W, HAND_H = 32, 24      # hand card art window
TITLE_W, TITLE_H = 320, 200
TITLE_BAND_ROWS = 8          # 25 bands, one palette each
SUPPORT_VARIANTS = 6


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
    noise rather than as shading.  The card art is the opposite case and takes
    it at full strength: four evenly spaced greys are a ramp, so the error a
    pixel cannot hold is exactly what the next one can, and full diffusion is
    what turns four levels into a continuous tone."""
    pal = np.array(palette, dtype=np.float64)
    idx = list(range(16)) if allow is None else list(allow)
    sub = pal[idx]
    src = np.asarray(img.convert("RGB"), dtype=np.float64)
    h, w = src.shape[:2]
    out = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        row = src[y]
        for x in range(w):
            old = row[x]
            k = int(((sub - old) ** 2).sum(axis=1).argmin())
            out[y, x] = idx[k]
            err = (old - sub[k]) * strength
            if x + 1 < w:
                row[x + 1] += err * (7 / 16.0)
            if y + 1 < h:
                if x:
                    src[y + 1][x - 1] += err * (3 / 16.0)
                src[y + 1][x] += err * (5 / 16.0)
                if x + 1 < w:
                    src[y + 1][x + 1] += err * (1 / 16.0)
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


def art_grey(img, flat=False):
    """A painting as luminance, EQUALISED and then low-passed.

    Two steps, both measured against the alternatives on a sheet of eight
    cards.  Equalise rather than autocontrast: these paintings are dark, and a
    plain contrast stretch leaves three quarters of the pixels inside the
    bottom step of a four-level ramp, so the card comes back a black rectangle
    with a white face in it.  Equalisation spends all four steps.

    Then blur, HARDER than the colour path's 0.7, because four levels cannot
    hold texture: what survives a four-level dither is the broad tonal
    massing, and any detail finer than that only turns into speckle that hides
    the massing.  1.0 keeps the silhouette and drops the noise."""
    g = img.convert("L")
    if flat:
        # A support sigil and the card back are DRAWN, not photographed: they
        # are already three or four flat tones on a flat field.  Equalising
        # one spreads those few tones across the whole ramp and blurring it
        # rounds the shape off, and all six supports came back as the same
        # grey blob.  A plain stretch keeps the emblem's edges.
        return ImageOps.autocontrast(g, cutoff=1).convert("RGB")
    g = ImageOps.equalize(g)
    return g.filter(ImageFilter.GaussianBlur(1.0)).convert("RGB")


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
    """The monster, cropped to its content and framed head-high.

    Contrast is pushed before the dither, not after: a 32-pixel thumbnail that
    keeps the original's midtone range dithers into an even mush of the two
    nearest palette entries, and the monster stops being a shape."""
    src = img.crop(card_content_bbox(img)).convert("RGB")
    out = ImageOps.fit(src, size, method=Image.Resampling.LANCZOS,
                       centering=(0.5, 0.30))
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
        # apart by arcane blue against jade, and the eight-grey card art threw
        # the tint away: equip and guard came back as the same emblem.
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
    return ImageOps.autocontrast(img, cutoff=1).filter(ImageFilter.SHARPEN)


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


def face_image(tag, src, size):
    if tag.startswith("m:"):
        return card_thumb(src, size)
    if tag.startswith("s:"):
        return support_art(int(tag[2:]), size)
    return card_back_art(size)


# ── The ground ───────────────────────────────────────────────────────────────

def build_arena_texture(pal):
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
    # Two dithers of the same photograph, one per tile colour, so both tiles
    # carry the same grain and only their tone differs.  Restricting each to
    # its own pair is what stops a light tile speckling with the dark tone.
    # ONE TILE, DRAWN, REPEATED -- not a 128x128 photograph cut into tiles.
    # Two earlier versions failed for opposite reasons.  Tiling the photograph
    # directly picked up its large-scale luminance drift, so some light tiles
    # dithered almost entirely to the darker of their two tones and others to
    # the lighter, and the board came out blotchy.  Dithering one 16x16 crop
    # of it and repeating that gave every tile the same stone, but the crop's
    # own structure became a camouflage blob repeated twenty times.
    #
    # What a tile actually needs is a flat face, a shaded inner edge along the
    # two sides facing away from the light, and grain fine enough to stay
    # grain when a near tile magnifies one texel to three screen pixels.  So it
    # is drawn: the bevel is what makes a tile read as a raised slab, and the
    # grain is a fixed hash rather than noise so it is the same on every run.
    t = TEXELS_PER_UNIT

    def tile_cell(base, shade_idx, grain_idx):
        # THE GRAIN HASH MUST MIX, NOT ADD.  The first version of this was
        # `(xx*7 + yy*13 + xx*yy) % 7`, which is linear in xx for every fixed
        # yy: whole columns of a tile satisfied it at once and the board came
        # out as plaid -- two bright vertical stripes through every tile,
        # which is exactly what a checkerboard must not have.  An integer
        # bit-mix has no such structure and is still deterministic.
        cell = np.full((t, t), base, dtype=np.uint8)
        for yy in range(t):
            for xx in range(t):
                h = (xx * 0x2545F491) ^ (yy * 0x9E3779B1)
                h ^= (h >> 13)
                h = (h * 0x27220A95) & 0xFFFFFFFF
                h ^= (h >> 15)
                if (h % 5) == 0:              # about one texel in five
                    cell[yy, xx] = grain_idx
        # One shaded edge, on the two sides away from the light.  Two texels
        # of bevel at sixteen texels per unit is a third of a screen pixel at
        # the far end and three at the near one, which is the whole point of
        # baking the bevel rather than drawing tile borders in the rasteriser.
        cell[t - 1, :] = shade_idx
        cell[:, t - 1] = shade_idx
        return cell

    # The dark tile's grain is a LIGHTER fleck (RIM_SIDE), not the groove: a
    # near-black speckle on the dark tile reads as dirt, and at three screen
    # pixels a texel the near row of tiles looked mouldy.  Its bevel is still
    # the groove, because a bevel is a shadow.
    light = np.tile(tile_cell(3, 4, 4), (ARENA_TEX_W // t, ARENA_TEX_W // t))
    dark = np.tile(tile_cell(2, 1, 5), (ARENA_TEX_W // t, ARENA_TEX_W // t))
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

    # The sample the free palette entries are fitted to: every card face at the
    # size it is actually shown, so the fit is weighted the way the screen is.
    sample = []
    thumbs = []
    for tag, src in faces:
        img = face_image(tag, src, (FIELD_W, FIELD_H))
        thumbs.append((tag, img))
        sample.append(np.asarray(img, dtype=np.float64).reshape(-1, 3))
    sample = np.concatenate(sample)

    arena_fixed = dict(ARENA_FIXED)
    # Sandstone is a low-contrast photograph: its three terciles come back
    # within a few units of each other, and three near-identical entries make
    # the board a flat wash with no texture in it at all.  Pushing them apart
    # from their own mean is what puts grain back into the tiles.
    sand = quantize_levels(Image.open(SAND_SRC), 3)
    sand_mid = tuple(sum(c[i] for c in sand) / 3.0 for i in range(3))
    sand = [tuple(int(max(0, min(255, round(sand_mid[i] +
                                           (c[i] - sand_mid[i]) * 2.2))))
                  for i in range(3)) for c in sand]

    def shade(c, k):
        return tuple(int(max(0, min(255, round(v * k)))) for v in c)

    def warm(c, k):
        """Pull a sandstone tone towards the board's gold.

        The photograph is a neutral cream and a neutral cream checkerboard
        reads as a chessboard, not as this game's arena.  Mixing every tile
        tone towards ARENA_SLOT's gold is what gives the board the same
        gold-on-brown identity the MSX2 and PC-FX ones have."""
        g = ARENA_FIXED["SLOT"]
        return tuple(int(max(0, min(255, round(c[i] * (1 - k) + g[i] * k))))
                     for i in range(3))

    def keep_hue(c):
        """Nudge a brown until red still leads green after `st3`."""
        r, g, b = c
        while (r >> 5) <= (g >> 5) and g > 0:
            g -= 8
        return (r, max(0, g), b)

    # The checkerboard's contrast is what makes the board read as a board: two
    # tones of warmed sandstone carry the light tile, a darkened one the dark
    # tile, and the groove between them is darker again.
    arena_fixed["TILE_LIGHT"] = keep_hue(warm(sand[2], 0.45))
    arena_fixed["TILE_LIGHT2"] = keep_hue(shade(warm(sand[1], 0.55), 0.88))
    arena_fixed["TILE_DARK"] = keep_hue(shade(warm(sand[0], 0.35), 0.80))
    arena_fixed["GROOVE"] = keep_hue(shade(arena_fixed["TILE_DARK"], 0.55))
    # The slab's front face and the lit edge along the top of it.  Without the
    # two the board is a rug rather than a solid object.
    arena_fixed["RIM_SIDE"] = keep_hue(shade(arena_fixed["TILE_DARK"], 1.35))
    arena_fixed["RIM_TOP"] = keep_hue(shade(arena_fixed["TILE_LIGHT"], 1.12))
    if not quiet:
        for name in ("TILE_LIGHT", "TILE_LIGHT2", "TILE_DARK", "GROOVE",
                     "RIM_SIDE", "RIM_TOP"):
            print("ARENA    %-11s %-16s -> ST %s"
                  % (name, arena_fixed[name], st3(arena_fixed[name])))

    arena_pal = fit_palette(ARENA_SLOTS, arena_fixed, sample)
    card_pal = fit_palette(CARD_SLOTS, CARD_FIXED, sample)

    # Ground.
    ground = build_arena_texture(arena_pal)
    write(os.path.join(dat, "ARENA.TEX"), (ground << 2).astype(np.uint8).tobytes(),
          quiet, "GROUND")
    preview(ground, arena_pal).resize((256, 256), Image.Resampling.NEAREST).save(
        os.path.join(prev, "arena_tex.png"))

    # Card faces: chunky for the 3D board, planar for the hand.
    field = bytearray()
    hand = bytearray()
    sheet_f = Image.new("RGB", (FIELD_W * 10, FIELD_H * ((len(faces) + 9) // 10)))
    sheet_h = Image.new("RGB", (HAND_W * 10, HAND_H * ((len(faces) + 9) // 10)))
    # The eight-step ramp, by index, in each palette.  Passed to the dither as
    # its whole allowed set: letting a card reach for the board's sandstone or
    # the panel's blues is what made the old cards blotchy.  The two ramps sit
    # at the SAME indices in both palettes, so one dithered picture serves the
    # hand and the 3D board and a card reads identically in each.
    def ramp(slots):
        return ([slots.index("BLACK")] +
                [slots.index("ART_K%d" % k) for k in range(1, 7)] +
                [slots.index("WHITE")])

    arena_k = ramp(ARENA_SLOTS)
    card_k = ramp(CARD_SLOTS)

    for i, (tag, img) in enumerate(thumbs):
        flat = not tag.startswith("m:")
        fi = dither(art_grey(img, flat), arena_pal, allow=arena_k, strength=1.0)
        fi[0, :] = fi[-1, :] = 7                      # SLOT: the card's rim
        fi[:, 0] = fi[:, -1] = 7
        field += (fi << 2).astype(np.uint8).tobytes()
        sheet_f.paste(preview(fi, arena_pal), ((i % 10) * FIELD_W, (i // 10) * FIELD_H))

        tag2, src = faces[i]
        hi = dither(art_grey(face_image(tag2, src, (HAND_W, HAND_H)), flat),
                    card_pal, allow=card_k, strength=1.0)
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
 * shapes of the art files on the floppy.  The structural entries are sampled
 * from the real textures; the entries marked `art` were FITTED to the card
 * paintings, which is what lets seventy-eight monsters share five (arena) or
 * seven (card) free palette entries.
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
