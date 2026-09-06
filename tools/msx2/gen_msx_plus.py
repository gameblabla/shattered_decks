#!/usr/bin/env python3
"""Bake the MSX2+ asset set: the 2-D screens in SCREEN 10 (YJK + YAE).

WHAT THIS IS FOR
----------------
The MSX2 build draws everything in GRAPHIC 7, whose 256 colours are three bits
of green, three of red and TWO OF BLUE -- which is why every painting in this
game arrives as a poster and every sky bands.  A V9958 has the same 256-byte
line and the same two pages, but it can read those bytes as YJK: a brightness
per pixel and a hue shared by each group of four, which on artwork of this kind
is worth about fifteen thousand colours.

So the MSX2+ cartridge is the same game with the same code, and only the
PICTURE SCREENS re-encoded: the title, the story -- narration, the talks, the
sanctum road, the continue-code screens, the deck editor -- and the ending.
The duel itself stays in GRAPHIC 7, cards and all: the board is rasterised live
by the Z80 and its card textures are shared with the hand, so YJK would buy it
nothing and cost it a chroma group on every card edge.

Nothing here changes the layout of a byte, a segment or a screen.  Every asset
this writes is the same size and lands at the same segment as the MSX2 one it
stands in for, so the plus ROM is packed by the same tool from the same
manifest; only `cards_yjk` is new, and it is emitted into a plus-only manifest
and header so the MSX2 cartridge never carries it.

    python3 tools/msx2/gen_msx_plus.py [--no-dither] [--quiet]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np                                             # noqa: E402
from PIL import Image                                          # noqa: E402

import gen_msx_scenes as scenes                                # noqa: E402
import msx2_yjk as yjk                                         # noqa: E402

ROOT = scenes.ROOT
ASSET_DIR = scenes.ASSET_DIR
PLUS_DIR = os.path.join(ASSET_DIR, "plus")
MANIFEST = os.path.join(PLUS_DIR, "manifest_plus.txt")
HEADER = os.path.join(ROOT, "src", "generated", "msx2_plus_scenes.h")

WIDTH, HEIGHT = scenes.WIDTH, scenes.HEIGHT
SEGMENT_BYTES = scenes.SEGMENT_BYTES

# The screens this build re-encodes, and the SCREEN 8 ones it deliberately does
# not.  BATTLE is the attack cut-in: it is drawn by the duel's own translation
# unit out of the duel's card textures, so it stays where the board is.
# Two of them have their own source art: the MSX2 title and ending are
# pre-dithered GRAPHIC 7 dumps (a hand-tuned SCREEN 8 conversion of a
# photographic sky), and there is nothing to re-encode in a picture that has
# already been through a 256-colour quantiser.  The plus build goes back to the
# paintings.
PLUS_SOURCE = {
    "TITLE": "assets/source/msx2/title256_msx2plus.png",
    "ENDING": "assets/source/msx2/ending256x212_msx2plus.png",
}

YJK_SCENES = ["TITLE", "ENDING"] + \
             ["MAP_%d" % s for s in range(4)] + \
             ["TALK_%d" % s for s in range(4)]

# Floyd-Steinberg, at the same strength the story busts are dithered at in the
# MSX2 build.  YJK's error is structured -- a whole group of four pixels shares
# the hue it could not reach -- so diffusing it is worth more here than in a
# paletted mode, and it is what keeps a sky from banding into ribbons.
DITHER = 0.9

# See bust_blob: the MSX2 build's 0.45 is too dark for YJK.
PLUS_PORTRAIT_DIM = 0.62

# Where the busts stand in the plus build -- msx2_plus.h moves them onto chroma
# group boundaries, and the edge composite has to look at the same pixels the
# runtime will write.
MSX2_PORTRAIT_LEFT_X = 0
MSX2_PORTRAIT_RIGHT_X = 128

# Eight palette entries are the interface's (msx2_yjk.UI_PALETTE) and eight are
# fitted to each picture, because YJK's weak end is the dark end and a scene's
# own shadows are the colours worth spending them on.
FIT = 8

# The interface's ink, as SCREEN 10 pixel bytes: entry in the high nibble, the
# YAE bit set.  The low three bits are the group's chroma and belong to the
# picture, so a stamp keeps them (see stamp_yae).
YAE_BLACK = 0x08
YAE_GOLD = (2 << 4) | 8
YAE_WHITE = (7 << 4) | 8


def yae(index):
    return (index << 4) | 8


def stamp_yae(buf, width, x, y, w, h, index):
    """Paint a rectangle in a palette colour, keeping each pixel's chroma bits.

    A YAE pixel's low three bits are still read as part of its group's J or K,
    so a box that zeroed them would drag the colour out of up to three picture
    pixels at each of its vertical edges.  Keeping them costs one OR."""
    ink = yae(index)
    for row in range(y, min(y + h, HEIGHT)):
        base = row * width
        for col in range(x, min(x + w, width)):
            buf[base + col] = ink | (buf[base + col] & 7)


def encode_scene(img, dither):
    data, palette = yjk.encode(img, dither=dither, fit_yae=FIT)
    return bytearray(data), palette


def scene_blob(data, palette):
    """The picture, then its palette in the tail of the last segment.

    A scene is 54,272 bytes inside four 16 KB segments, so there are eleven
    kilobytes of slack after it that the packer already writes as padding.  The
    palette goes at the front of that slack: the runtime reads 32 bytes from
    (scene segment, MSX2_SCENE_BYTES) after streaming, and no asset, segment
    number or header symbol had to be added to carry it."""
    blob = bytearray(data)
    blob += yjk.palette_bytes(palette)
    return bytes(blob)


def bake_scenes(dither, quiet):
    """Every full-screen picture, re-encoded, plus the title's prompt strip."""
    out = {}
    raw = {}
    strip = None
    for name, build in scenes.SCENES:
        if name not in YJK_SCENES:
            continue
        if name in PLUS_SOURCE:
            built = Image.open(os.path.join(ROOT, PLUS_SOURCE[name]))
        else:
            built = build()
        if isinstance(built, (bytes, bytearray)):
            raise SystemExit("scene %s is pre-quantised and has no plus source"
                             % name)
        img = built.convert("RGB")
        if img.size != (WIDTH, HEIGHT):
            img = img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        data, palette = encode_scene(img, dither)

        if name.startswith("TALK"):
            # The text box is interface, not painting: it is stamped flat in the
            # panel colour so the words that land in it keep their own edges.
            stamp_yae(data, WIDTH, 0, scenes.TALK_BOX_Y, WIDTH,
                      HEIGHT - scenes.TALK_BOX_Y, 1)
        if name.startswith("MAP"):
            stamp_yae(data, WIDTH, scenes.MAP_PANEL_X, scenes.MAP_PANEL_Y,
                      scenes.MAP_PANEL_W, scenes.MAP_PANEL_H, 1)
        if name == "TITLE":
            # The words are stamped into the picture exactly as the MSX2 build
            # stamps them, but in palette ink -- and the prompt strip is cut out
            # of the result the same way, so the blink is still one command.
            #
            # keep_chroma, on every one of them.  These words sit straight on
            # the painting, so most of their glyphs share a group of four with
            # sky: overwriting the whole byte took the low three bits with it
            # and dragged the hue out of the picture pixels beside every letter.
            # The letter itself is a palette pixel and never wanted them.
            scenes.stamp_big(data, WIDTH, scenes.TITLE_LOGO_Y1, "SHATTERED",
                             YAE_GOLD, YAE_BLACK, keep_chroma=True)
            scenes.stamp_big(data, WIDTH, scenes.TITLE_LOGO_Y2, "DECKS",
                             YAE_WHITE, YAE_BLACK, keep_chroma=True)
            scenes.stamp_outline(data, WIDTH, scenes.TITLE_COPY_Y,
                                 "(C) 2026 GAMEBLABLA", YAE_WHITE, YAE_BLACK,
                                 keep_chroma=True)
            strip = bytearray(data[scenes.TITLE_STRIP_Y * WIDTH:
                                   (scenes.TITLE_STRIP_Y + scenes.TITLE_STRIP_H)
                                   * WIDTH])
            scenes.stamp_outline(strip, WIDTH,
                                 scenes.TITLE_PROMPT_Y - scenes.TITLE_STRIP_Y,
                                 scenes.TITLE_PROMPT, YAE_WHITE, YAE_BLACK,
                                 keep_chroma=True)

        out[name] = scene_blob(data, palette)
        raw[name] = bytes(data)
        if not quiet:
            print("%-13s %d bytes YJK+YAE" % (name, len(out[name])))
    if strip is None:
        raise SystemExit("no TITLE scene: the prompt strip has nowhere to come from")
    out["title_prompt"] = bytes(strip)
    return out, raw


# ── The story busts ──────────────────────────────────────────────────────────

# TWO THRESHOLDS, BECAUSE THE TWO QUESTIONS ARE DIFFERENT.
#
# "Is this pixel part of the figure?" is the MSX2 build's question and its
# answer is the MSX2 build's: anything past 96/255 is drawn.  Raising that is
# what put HOLES in the busts -- the art's antialiased hair is a long way from
# opaque, and every strand of it below the bar stopped being blitted at all.
#
# "Should this pixel have a say in the colour of the four it shares a chroma
# group with?" is the question YJK adds, and there the bar has to be high: the
# paintings are cut out against black, so a pixel at 40% alpha carries almost
# no colour, and letting it into the group's mean blackened its three
# neighbours as well.  That is the fringe the first plus cartridge drew.
BUST_SOLID = 96          # ... is drawn
BUST_CHROMA = 176        # ... and gets a vote on the hue


def opaque_runs(alpha, y, w):
    """scenes.portrait_runs, said again here so the threshold is this file's."""
    apx = alpha.load()
    runs = []
    x = 0
    while x < w:
        while x < w and apx[x, y] < BUST_SOLID:
            x += 1
        if x >= w:
            break
        start = x
        while x < w and apx[x, y] >= BUST_SOLID:
            x += 1
        runs.append([start, x - start])
    while len(runs) > scenes.PORTRAIT_MAX_RUNS:
        gaps = [(runs[i + 1][0] - (runs[i][0] + runs[i][1]), i)
                for i in range(len(runs) - 1)]
        _gap, i = min(gaps)
        runs[i][1] = runs[i + 1][0] + runs[i + 1][1] - runs[i][0]
        del runs[i + 1]
    return [(a, min(b, 255)) for a, b in runs]


def bust_runs(alpha, y, w):
    """The opaque runs of one row, rounded INWARD to whole chroma groups.

    A YJK pixel cannot be written alone -- three of its neighbours share its
    hue, and those hues live in the low bits of the four bytes -- so a plain
    rectangle blit has to cover whole groups of four.  Which way that is
    rounded used to be the whole question and both answers were wrong: inward
    dropped up to three pixels of silhouette a side, outward WROTE three, and
    what it wrote was this art's own black outline smeared into a four-pixel
    block.  That is the black halo the report is about.

    So a run now covers only the groups the figure fills completely, and the
    pixels left over -- the ones in a group it shares with the backdrop -- are
    not the blitter's problem at all.  They are handed to bust_fringe() and
    written one at a time, keeping the chroma bits already in VRAM, which is
    the only way a cut-out gets a per-pixel edge in this mode.

    Both bust positions on screen are multiples of four (msx2_story.c pins them
    there), so a run aligned inside the rectangle is aligned on the screen too.
    """
    apx = alpha.load()
    groups = w // 4

    def full(g):
        return all(apx[x, y] >= BUST_SOLID
                   for x in range(g * 4, min(g * 4 + 4, w)))

    runs = []
    g = 0
    while g < groups:
        if not full(g):
            g += 1
            continue
        first = g
        while g < groups and full(g):
            g += 1
        runs.append([first * 4, (g - first) * 4])
    # Three runs is what the row record holds; anything past that is folded in
    # by absorbing the narrowest gap, exactly as the MSX2 table does it.
    while len(runs) > scenes.PORTRAIT_MAX_RUNS:
        gaps = [(runs[i + 1][0] - (runs[i][0] + runs[i][1]), i)
                for i in range(len(runs) - 1)]
        _gap, i = min(gaps)
        runs[i][1] = runs[i + 1][0] + runs[i + 1][1] - runs[i][0]
        del runs[i + 1]
    return [(a, min(b, 252)) for a, b in runs]


# THE EDGE IS COMPOSITED AGAINST THE PICTURE IT STANDS ON.
#
# A fringe pixel keeps the chroma bits already in VRAM -- it has to; the other
# three pixels of its group are backdrop and the hue is theirs -- so all the
# YJK fallback gets to choose is its BRIGHTNESS. Writing the figure's brightness
# there is what drew the dark navy rim: the outer ring of this art is a long
# way from opaque, its colour is half the transparent ground, and hanging that
# on the sky's hue is a shadow the drawing never had.
#
# So the pixel is composited here instead: alpha over the backdrop the bust
# will actually stand on, then the best Y for the result GIVEN THE GROUP'S
# CHROMA, which is known because this tool baked that picture a moment ago.
# Keeping hue still cannot reproduce every outline colour, so the scene's
# sixteen palette entries are tried as well and the closer of the two encodings
# wins.  Msx2_MergeRow preserves all three chroma bits either way.
#
# THAT CHOICE IS MADE HERE, NOT ON THE Z80.  It used to be: a record carried
# the YJK fallback, the intended RGB555, the fallback's error and a nominated
# palette index, and Msx2_StoryBlitBust weighed them per pixel.  Every input to
# that comparison is known at bake time -- the palette is the backdrop's, and
# the table is already per backdrop -- so the whole of it collapses into the
# one byte the blitter actually writes.  A record is now (x, ink), which is a
# third of the ROM to read and none of the arithmetic: the fringe went from
# roughly half the cost of a bust redraw to a rounding error on it.
#
# The table is a function of the backdrop, so a bust carries one per talk scene
# (there are four) and the runtime picks by stage.
STAGE_COUNT = 4          # TALK_0..TALK_3; a bust may stand on any of them
FRINGE_STRIDE = 2048     # a count per row, then (x, ink) per fringe pixel
FRINGE_BYTES = 2         # ... which is what one of those records costs
FRINGE_MAX = 26          # the worst row, and what the runtime's RAM record holds
FRINGE_OFF = scenes.PORTRAIT_INDEX_BYTES
PIXELS_OFF = FRINGE_OFF + STAGE_COUNT * FRINGE_STRIDE

# A pixel this opaque is worth compositing.  It is far below BUST_SOLID on
# purpose: these pixels are not "drawn" in the paletted sense, they are the
# skirt of anti-aliasing the alpha channel has, and now that the edge is a
# blend rather than an overwrite there is no reason to throw it away.  Lower
# than this and a bust grows a haze a group wide out of hair that is barely
# there.
EDGE_ALPHA = 40


def stage_backdrop(data):
    """Decode the real backdrop, including palette pixels in the painting.

    The fitted encoder uses YAE throughout the art, not only in UI panels.
    Interpreting a palette index as Y would blend the edge against a fictitious
    background colour. Its low three bits still contribute to the group's J/K.
    """
    a = np.frombuffer(bytes(data[:WIDTH * HEIGHT]),
                      dtype=np.uint8).reshape(HEIGHT, WIDTH).astype(np.int32)
    y = (a >> 3).astype(np.float64)
    lo = (a & 7).reshape(HEIGHT, WIDTH // 4, 4)
    k = (lo[:, :, 0] | (lo[:, :, 1] << 3)).astype(np.int32)
    j = (lo[:, :, 2] | (lo[:, :, 3] << 3)).astype(np.int32)
    k = np.where(k > 31, k - 64, k).astype(np.float64)
    j = np.where(j > 31, j - 64, j).astype(np.float64)
    jj = np.repeat(j, 4, axis=1)
    kk = np.repeat(k, 4, axis=1)
    pal3 = np.array(yjk.palette_from_bytes(data[WIDTH * HEIGHT:]), dtype=np.int32)
    pal5 = (pal3 << 2) | (pal3 >> 1)
    rgb = np.floor(yjk.yjk_to_rgb(y, jj, kk))
    rgb = np.where(((a & 8) != 0)[..., None], pal5[a >> 4], rgb)
    return rgb, jj, kk, pal5


def bust_fringe(alpha, y, w, runs):
    """The columns of one row that no whole-group run covers.

    These are the edge of the figure: pixels sharing a chroma group with the
    backdrop behind them, plus the anti-aliased skirt outside the silhouette
    proper (EDGE_ALPHA).  Which columns they are does not depend on the
    backdrop -- only the byte written into them does -- so every stage's table
    has the same shape and the runtime's cursor does not care which one it is
    walking.
    """
    apx = alpha.load()
    covered = bytearray(w)
    for start, length in runs:
        for x in range(start, min(start + length, w)):
            covered[x] = 1
    return [x for x in range(w)
            if not covered[x] and apx[x, y] >= EDGE_ALPHA]


def bust_edge_bytes(bust, dim, x0, y0, backdrop):
    """The finished edge byte for every pixel of one bust over one backdrop.

    Composite first (alpha over the picture), then ask _best_y what brightness
    comes closest to that colour through the chroma the group already carries,
    and separately which of the scene's sixteen palette entries comes closest
    to it outright.  Whichever is nearer the intended colour is the byte, YJK
    or YAE; the low three bits are left at zero either way, because they are
    the group's and Msx2_MergeRow ORs them back in from VRAM.
    """
    bg5, jj, kk, pal5 = backdrop
    h, w = bust.size[1], bust.size[0]
    a = (np.asarray(bust.getchannel("A"), dtype=np.float64) / 255.0)[..., None]
    fig = np.asarray(bust.convert("RGB"), dtype=np.float64)
    if dim:
        fig = fig * PLUS_PORTRAIT_DIM
    fig5 = yjk.to5(fig)
    win = (slice(y0, y0 + h), slice(x0, x0 + w))
    comp = a * fig5 + (1.0 - a) * bg5[win]
    ybits = yjk._best_y(comp, jj[win], kk[win], even=True)
    target = np.clip(np.rint(comp), 0, 31).astype(np.int32)
    decoded = np.floor(yjk.yjk_to_rgb(ybits, jj[win], kk[win])).astype(np.int32)
    delta = np.abs(target - decoded)
    error = delta[..., 0] + 2 * delta[..., 1] + delta[..., 2]
    palette_error = (np.abs(target[..., None, :] - pal5) * [1, 2, 1]).sum(axis=-1)
    candidate = palette_error.argmin(axis=-1)
    best = np.take_along_axis(palette_error, candidate[..., None], axis=-1)[..., 0]
    yjk_byte = ybits.astype(np.int32) << 3
    yae_byte = (candidate.astype(np.int32) << 4) | 8
    return np.where(best < error, yae_byte, yjk_byte).astype(np.uint8)


def bust_blob(bust, dim, dither, x0, y0, backdrops):
    """One bust, YJK: a run table, a fringe table per backdrop, then the pixels.

    The MSX2 layout with the fringe tables inserted -- the runs and the pixels
    mean exactly what they mean in the paletted build, and the fringe is the
    edge this mode cannot blit (see bust_runs), composited against each picture
    the figure can stand on (see bust_edge_bytes).

    Interiors are pure YJK. Edges can use YAE because the runtime chooses from
    the palette of the actual backdrop, rather than baking a fixed index.

    THE MASK GOES THROUGH THE ENCODER.  It is not an optimisation: the cut-out
    ground is black, a group of four shares one hue, and an encoder that cannot
    see which of the four are real fits that hue -- and its dithering error --
    to the hole.  msx2_yjk.encode takes the mask for exactly this.
    """
    w, h = bust.size
    alpha = bust.getchannel("A")
    rgb = bust.convert("RGB")
    if dim:
        # A HIGHER FLOOR THAN THE MSX2 BUILD'S.
        # YJK's weak end is the dark end -- Y moves in steps of two, and the
        # chroma a group shares is fitted to colours that have almost none --
        # so the 0.45 the paletted build dims a listening speaker to comes out
        # as a black cut-out with the hair lost inside it.  The intent is
        # "not the one speaking", and 0.62 says that in a mode that cannot
        # spend brightness it does not have.
        rgb = Image.blend(Image.new("RGB", bust.size, (0, 0, 0)), rgb,
                          PLUS_PORTRAIT_DIM)
    a = np.asarray(alpha, dtype=np.int32)
    quant, _pal = yjk.encode(rgb, dither=dither, allow_yae=False,
                             mask=a >= BUST_CHROMA, solid=a >= BUST_SOLID)
    edges = [bust_edge_bytes(bust, dim, x0, y0, b) for b in backdrops]

    index = bytearray()
    fringe = [bytearray() for _ in backdrops]
    pixels = bytearray()
    for y in range(h):
        runs = bust_runs(alpha, y, w)[:scenes.PORTRAIT_MAX_RUNS]
        index.append(len(runs))
        for i in range(scenes.PORTRAIT_MAX_RUNS):
            if i < len(runs):
                index += bytes(runs[i])
                pixels += quant[y * w + runs[i][0]:
                                y * w + runs[i][0] + runs[i][1]]
            else:
                index += b"\x00\x00"
        # A row the text box eats is never blitted; the runtime still steps the
        # cursor over its record, so it gets an empty one.
        edge = [] if y0 + y >= scenes.TALK_BOX_Y else bust_fringe(alpha, y, w, runs)
        if len(edge) > FRINGE_MAX:
            sys.exit("row %d has %d fringe pixels, past %d"
                     % (y, len(edge), FRINGE_MAX))
        # The byte to write, already decided (see bust_edge_bytes).
        for t, table in enumerate(fringe):
            table.append(len(edge))
            ink = edges[t]
            for x in edge:
                table += bytes((x, int(ink[y, x])))
    if len(index) > scenes.PORTRAIT_INDEX_BYTES:
        sys.exit("portrait run table is %d bytes, past %d"
                 % (len(index), scenes.PORTRAIT_INDEX_BYTES))
    body = index + bytes(scenes.PORTRAIT_INDEX_BYTES - len(index))
    for table in fringe:
        if len(table) > FRINGE_STRIDE:
            sys.exit("portrait fringe table is %d bytes, past %d"
                     % (len(table), FRINGE_STRIDE))
        body += table + bytes(FRINGE_STRIDE - len(table))
    body += pixels
    if len(body) > scenes.PORTRAIT_STRIDE:
        sys.exit("portrait is %d bytes, past the %d stride"
                 % (len(body), scenes.PORTRAIT_STRIDE))
    return body + bytes(scenes.PORTRAIT_STRIDE - len(body))


def bake_portraits(dither, quiet, raw):
    backdrops = [stage_backdrop(raw["TALK_%d" % s]) for s in range(STAGE_COUNT)]
    blob = bytearray()
    size = (scenes.PORTRAIT_W, scenes.PORTRAIT_H)
    names = ["serena"] + ["opponent_%d" % d for d in range(scenes.STORY_DUELS)]
    for i, name in enumerate(names):
        # Serena stands on the left of every scene and an opponent on the
        # right; the composite has to be done where the figure actually is.
        x0 = MSX2_PORTRAIT_LEFT_X if i == 0 else MSX2_PORTRAIT_RIGHT_X
        y0 = scenes.PORTRAIT_LEFT_Y if i == 0 else scenes.PORTRAIT_RIGHT_Y
        # The premultiplied resample: without it the outer ring of the figure
        # carries the transparent ground's black, which the composite would
        # then dutifully blend into the sky (gen_msx_scenes.portrait_resize).
        bust = scenes.portrait(name, size, premul=True)
        for dim in (False, True):
            blob += bust_blob(bust, dim, dither, x0, y0, backdrops)
    if not quiet:
        print("PORTRAITS     %d bytes YJK" % len(blob))
    return bytes(blob)


# ── The 40x48 card thumbnails, for the story's own screens ───────────────────

def bake_cards(cards, dither, quiet):
    """A YJK copy of the hand thumbnails.

    The deck editor and the story's reward reveal are SCREEN 10 screens that
    show one card, and the MSX2 blob they used is GRAPHIC 7 bytes.  This is the
    only asset the plus cartridge ADDS rather than replaces, which is why it is
    written into a manifest and a header of its own: the MSX2 cartridge is full
    and must not carry it.
    """
    blob = bytearray()
    faces = [scenes.draw_monster_card(asset_id, *scenes.CARD_STATS[asset_id])
             for asset_id, _name, _desc in cards]
    faces += [scenes.draw_support_card(kind)
              for kind in range(scenes.SUPPORT_VARIANTS)]
    faces.append(scenes.draw_card_back())
    for face in faces:
        data, _pal = yjk.encode(face.convert("RGB"), dither=dither,
                                allow_yae=False)
        blob += data + bytes(scenes.CARD_STRIDE - len(data))
    if not quiet:
        print("CARDS_YJK     %d bytes YJK (%d faces)" % (len(blob), len(faces)))
    return bytes(blob)


# ── Output ───────────────────────────────────────────────────────────────────

def base_manifest():
    out = []
    for raw in open(os.path.join(ASSET_DIR, "manifest.txt")):
        line = raw.split("#", 1)[0].strip()
        if line:
            name, seg, binary = line.split()
            out.append((name, int(seg), binary))
    return out


def main():
    quiet = "--quiet" in sys.argv
    dither = 0.0 if "--no-dither" in sys.argv else DITHER
    os.makedirs(PLUS_DIR, exist_ok=True)

    written = {}
    baked, raw = bake_scenes(dither, quiet)
    for name, blob in baked.items():
        path = os.path.join(PLUS_DIR, name.lower() + ".bin")
        with open(path, "wb") as f:
            f.write(blob)
        written[name.lower() + ".bin"] = path
        if name in YJK_SCENES:
            yjk.decode(blob[:WIDTH * HEIGHT],
                       yjk.palette_from_bytes(blob[WIDTH * HEIGHT:]),
                       (WIDTH, HEIGHT)).save(
                os.path.join(PLUS_DIR, name.lower() + ".png"))

    blob = bake_portraits(dither, quiet, baked)
    with open(os.path.join(PLUS_DIR, "portraits.bin"), "wb") as f:
        f.write(blob)
    written["portraits.bin"] = os.path.join(PLUS_DIR, "portraits.bin")

    cards = scenes.parse_cards()
    card_blob = bake_cards(cards, dither, quiet)
    card_path = os.path.join(PLUS_DIR, "cards_yjk.bin")
    with open(card_path, "wb") as f:
        f.write(card_blob)

    # The plus-only asset goes after everything the MSX2 cartridge holds.
    base = base_manifest()
    last = max(seg + (os.path.getsize(os.path.join(ASSET_DIR, b))
                      + SEGMENT_BYTES - 1) // SEGMENT_BYTES
               for _n, seg, b in base)
    for raw in open(os.path.join(ASSET_DIR, "floor_manifest.txt")):
        line = raw.split("#", 1)[0].strip()
        if line:
            _n, seg, b = line.split()
            span = (os.path.getsize(os.path.join(ASSET_DIR, b))
                    + SEGMENT_BYTES - 1) // SEGMENT_BYTES
            last = max(last, int(seg) + span)

    with open(MANIFEST, "w") as f:
        f.write("# MSX2+ (SCREEN 10) asset set -- written by "
                "tools/msx2/gen_msx_plus.py.\n"
                "# Every line replaces the MSX2 binary of the same name at the "
                "same segment,\n# except CARDS_YJK, which the MSX2 cartridge "
                "does not carry at all.\n")
        for name, seg, binary in base:
            if binary in written:
                f.write("%s %d plus/%s\n" % (name, seg, binary))
        f.write("CARDS_YJK %d plus/cards_yjk.bin\n" % last)

    with open(HEADER, "w") as f:
        f.write("// Generated by tools/msx2/gen_msx_plus.py -- do not edit.\n"
                "//\n"
                "// The MSX2+ build's own asset symbols.  Everything else it\n"
                "// uses is at the segment msx2_scenes.h already names: the\n"
                "// plus cartridge REPLACES those binaries with SCREEN 10\n"
                "// encodings of the same pictures, at the same sizes.\n"
                "#ifndef MSX2_PLUS_SCENES_H\n#define MSX2_PLUS_SCENES_H\n\n")
        f.write("#define MSX2_CARD_ART_YJK_SEGMENT  %d\n" % last)
        f.write("#define MSX2_SCENE_PALETTE_OFFSET  %d\n" % (WIDTH * HEIGHT))
        f.write("#define MSX2_SCENE_PALETTE_BYTES   32\n")
        f.write("\n// A YJK bust is blitted as whole chroma groups and then\n"
                "// EDGED one pixel at a time: a pixel that shares its group\n"
                "// with the backdrop keeps the chroma bits already in VRAM,\n"
                "// which is what gives a cut-out a per-pixel silhouette in a\n"
                "// mode whose hue is four pixels wide.  A row is a count and\n"
                "// then that many (x, ink) pairs, and the ink is finished --\n"
                "// YJK brightness or a YAE palette pixel, whichever the baker\n"
                "// found closer to the intended composite colour.  The table\n"
                "// is a function of the backdrop, so there is one per talk\n"
                "// scene, indexed by stage, between the runs and the pixels.\n")
        f.write("#define MSX2_PORTRAIT_FRINGE_OFF   %d\n" % FRINGE_OFF)
        f.write("#define MSX2_PORTRAIT_FRINGE_STRIDE %d\n" % FRINGE_STRIDE)
        f.write("#define MSX2_PORTRAIT_FRINGE_STAGES %d\n" % STAGE_COUNT)
        f.write("#define MSX2_PORTRAIT_FRINGE_MAX   %d\n" % FRINGE_MAX)
        f.write("#define MSX2_PORTRAIT_FRINGE_BYTES %d\n" % FRINGE_BYTES)
        f.write("#define MSX2_PORTRAIT_PIXELS_OFF   %d\n" % PIXELS_OFF)
        f.write("\n#endif\n")

    if not quiet:
        span = (len(card_blob) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
        print("CARDS_YJK     segments %d..%d (plus-only)"
              % (last, last + span - 1))
        print("wrote %s and %s" % (MANIFEST, HEADER))
    return 0


if __name__ == "__main__":
    sys.exit(main())
