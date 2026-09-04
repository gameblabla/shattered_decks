#!/usr/bin/env python3
"""Bake the MSX2 duel board out of the game's own renderer.

MSX2_PORT_PLAN.md §0.3.1 and §4.3 settle what this file is for: the duel is
played on the same 3D arena the other five targets render, captured pose by
pose, and the cards are drawn into their *true projected quads* rather than into
a grid of upright rectangles.  Nothing here draws an arena.  The capture comes
from `--dump-msx2-views` in `src/main.c`, which renders `render_board()` at the
authored MSX2 poses and prints the projected corner list for every field slot.

Three things come out:

  * `board_view_<stage>.bin` -- the 256x212 resting picture: the captured board,
    the same black surround as PC-FX, and the three baked UI panels.
    One per story stage.
  * `board_move_<stage>_<move>.bin` -- §4.6's baked camera move: a strip of whole
    pictures of the board band, played back in order by the ordinary streamer.
    No codec, no reconstruction; that is the entire technique.
  * `card_spans.bin` -- §8.4's Tier A span programs.  For each (pose, slot) an
    offline rasterisation of the card texture into the projected quad, expressed
    as run lengths so the Z80 replays it with block I/O and no arithmetic.

`gen_msx_scenes.py` imports this module so the cartridge segment map stays
owned by one tool; running this file directly bakes the same outputs and prints
what they cost.
"""

import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msx2_grb332 as grb  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSET_DIR = os.path.join(ROOT, "src", "msx2", "assets")
CAPTURE_DIR = os.path.join(ROOT, "build", "msx2_capture")
DUMP_BIN = os.path.join(CAPTURE_DIR, "waifu_msx2_dump")

WIDTH = 256
HEIGHT = 212
CAPTURE_H = 240

# ── The 212 rows, divided ────────────────────────────────────────────────────
#
# The capture is framed for a 240-row screen with nothing over it; the MSX2
# window is 212 rows with a HUD strip, a hand row and an info panel taking a
# hundred of them.  CROP_Y is what reconciles the two: it is chosen so the two
# projected card rows land immediately under the HUD, which is why the pose in
# `msx2_top_camera()` and this number have to move together.
CROP_Y = 75

HUD_H = 13                  # rows 0..12      -- LP
BAND_Y = 14                 # rows 14..127    -- the board itself
BAND_H = 114
HAND_BAND_Y = BAND_Y + BAND_H   # rows 128..181 -- the player's hand
HAND_BAND_H = 54
INFO_Y = HAND_BAND_Y + HAND_BAND_H   # rows 182..211 -- card name, ATK/DEF, prompt

CARD_W, CARD_H = 40, 48     # the one master texture, §4.4
HAND_X0 = 12
HAND_PITCH = 47
HAND_Y = HAND_BAND_Y + 3

# Twenty, not ten: the two monster rows keep slots 0..9, and the two SUPPORT
# rows -- where an equip or a set trap lives on every other target -- are
# appended as 10..19.  Appending rather than interleaving is what keeps the
# monster slots at the numbers the board and its baked tables already use.
FIELD_SLOTS = 20
HAND_SLOTS = 5

# The margin a slot's restore tile keeps around the quad.  It used to be a flat
# brown ring drawn just outside every slot, back when the cursor was a rectangle
# in the bitmap and had to be erased by redrawing a known colour.  The cursor is
# a sprite now, so the ring was a box of paint over the arena that said nothing;
# the margin it needed stays, because a span program can round a texel past the
# quad and the restore has to cover it.
SLOT_PAD = 4.0
PANEL_RGB = (40, 34, 48)
GOLD_RGB = (198, 152, 54)

# One tile per slot in the SLOTS blob, at a stride that divides a 16 KB segment
# so a tile can never straddle a bank switch.
SLOT_STRIDE = 4096

# The same rule for a span program: the interpreter reads it through the 0x8000
# window, so a program that straddled a segment boundary would have to be read
# in two bank-switched halves.  2,048 divides 16,384 exactly, so it never can.
SPAN_STRIDE = 2048

SEGMENT_BYTES = 16 * 1024

# Span-program opcodes (§8.4).  The count lives in the low five bits, so a run
# is 1..31 pixels and anything longer is split -- which costs one byte per 31
# pixels and keeps the interpreter's dispatch to a single AND.
OP_COPY = 0x00   # n texels, source advances
OP_DUP = 0x20    # repeat the last texel n times (magnification)
OP_SKIP = 0x40   # reserved: the generator splits a broken row into two records
OP_ADV = 0x60    # advance the source n texels, write nothing (minification)
OP_ENDROW = 0x80
OP_END = 0xA0
# The fifth byte of a row record: which copy of the texture the run reads.
SRC_NORMAL = 0
SRC_MIRROR = 1
OP_MAX_RUN = 31

STAGE_NAMES = ("DESERT", "STONE", "EMBER", "SKY")
BOARD_STAGES = len(STAGE_NAMES)

# ── The overhead view ────────────────────────────────────────────────────────
#
# The two captured views are the duellists' own chairs: the arena in
# perspective, which is what the game looks like, and which squeezes a field
# slot down to about 36x15 pixels.  That is a picture, not a board you can
# read, and every other target answers the same way -- PC-FX and the PC build
# put the camera overhead when the player walks up out of the hand, so the ten
# slots become ten legible rectangles and the bottom panel keeps naming
# whatever the cursor is on.
#
# This one is not captured.  It is the board drawn flat, supplied as artwork
# (assets/source/msx2/msx2_3d_top_view.png) -- a five-by-four table of tiles --
# and it takes the hand's rows as well as the board band, because in this view
# there is no hand on the screen.  The two middle rows of tiles are the two
# players' fields; the outer two are the table around them.
OVER_ART = os.path.join(ROOT, "assets", "source", "msx2", "msx2_3d_top_view.png")
OVER_Y = BAND_Y                      # the picture starts under the HUD
OVER_H = HAND_BAND_Y + HAND_BAND_H - BAND_Y     # ... and runs to the panel

# The artwork's own grid, measured off it.  The live overhead renderer fills
# each complete 48x42 pitch with one of the board's two beige face colours: no
# bevel, outline, or wall-coloured gutter is visible from straight above.  The
# narrower interior dimensions remain the card-placement reference.
OVER_TILE_X0, OVER_TILE_PITCH_X, OVER_TILE_W = 12, 48, 43
OVER_TILE_Y0, OVER_TILE_PITCH_Y, OVER_TILE_H = 0, 42, 38
# Which tile row each of the board's four rows plays on.  The artwork is a
# five-by-four grid and the outer two rows used to be "the table around them";
# they are the two support rows, which is the same four-row board the perspective
# views project.  The order is the slot order: monsters first, supports after.
OVER_SLOT_ROWS = (1, 2, 0, 3)

# A card on the overhead board is drawn at its own size out of the cartridge
# (gen_msx_scenes.py's overhead set), not rasterised into a quad, so this is a
# rectangle and not a projection.  32x42 centred on a 43x38 tile interior:
# two rows of overhang each way, into the gutter, touching no neighbour.
OVER_CARD_W, OVER_CARD_H = 32, 42

# A card in defence position is turned a quarter turn, the way it is on a real
# table, so overhead it occupies OVER_CARD_H x OVER_CARD_W.  It is drawn at full
# size -- the tile pitch is 48 and there are sixteen pixels of gutter between two
# cards, so a 42-wide card still touches no neighbour -- and this is how much
# wider than the upright card the slot's restore tile therefore has to be.
OVER_DEF_PADX = (OVER_CARD_H - OVER_CARD_W) // 2

# The same card turned in a CHAIR view cannot have that room: the slots there are
# a projected 40x48 and they sit shoulder to shoulder.  So a turned card is
# scaled to fit the slot it is in -- 48 units of card across 40 units of slot --
# and its footprint inside the slot's own (u, v) is this tall, centred.
DEF_FIT = (CARD_W / float(CARD_H)) ** 2


# ── The capture ──────────────────────────────────────────────────────────────

def build_dump_tool(quiet=False):
    """Compile the headless dump if it is missing or older than the game.

    The MSX2 board is a build artifact of `src/main.c`, exactly as the FM TOWNS
    turn cache is, so the capture tool is built here rather than being a step a
    person has to remember."""
    os.makedirs(CAPTURE_DIR, exist_ok=True)
    sources = [os.path.join(ROOT, "src", "main.c")]
    newest = max(os.path.getmtime(p) for p in sources)
    if os.path.exists(DUMP_BIN) and os.path.getmtime(DUMP_BIN) >= newest:
        return
    cmd = [os.environ.get("CC", "cc"), "-O1", "-std=c99", "-w",
           "-DPLATFORM=5", "-DBY16=1", "-DHARDWARE_DIV=1",
           "-DWAIFU_FM_HEADLESS_TESTS", "-DWAIFU_MSX2_VIEW_DUMP",
           "-I" + os.path.join(ROOT, "src", "engine"),
           "-I" + os.path.join(ROOT, "src", "generated"),
           "-I" + os.path.join(ROOT, "src", "record"),
           "-I" + os.path.join(ROOT, "src", "game")]
    for rel in ("src/main.c", "src/game/ai.c", "src/game/deck.c",
                "src/game/palette.c", "src/game/sounds.c", "src/game/assets.c",
                "src/engine/renderer3d.c", "src/engine/renderer3d_generic.c",
                "src/engine/common.c", "src/engine/bmp_writer.c",
                "src/platform/host_storage.c", "src/platform/host_assets.c",
                "src/platform/host_video.c", "src/record/zmbv_mkv.c"):
        cmd.append(os.path.join(ROOT, rel))
    cmd += ["-lm", "-lz", "-o", DUMP_BIN]
    if not quiet:
        print("building the MSX2 board capture tool")
    subprocess.run(cmd, check=True, cwd=ROOT)


def run_capture(quiet=False):
    build_dump_tool(quiet)
    subprocess.run([DUMP_BIN, "--dump-msx2-views", CAPTURE_DIR], check=True,
                   cwd=ROOT, stdout=subprocess.DEVNULL if quiet else None)
    return read_capture()


class Capture(object):
    """What `--dump-msx2-views` wrote: a palette, poses, and per-pose quads."""

    def __init__(self):
        self.palette = [(0, 0, 0)] * 256
        self.poses = {}      # tag -> [quad0..quad9], each 8 ints in window space
        self.moves = []      # (name, poses)
        self.bg_index = 0    # the index render_board() clears to
        self.card_tex = (CARD_W, CARD_H)
        # tag -> the projected arena mesh, in the same window space as the
        # quads.  This is what the cartridge carries INSTEAD of a picture of the
        # board; see MESH_POINTS below for the point order.
        self.mesh = {}

    def frame(self, tag):
        """One captured pose as a 256x240 RGB image."""
        raw = open(os.path.join(CAPTURE_DIR, tag + ".raw"), "rb").read()
        img = Image.new("RGB", (WIDTH, CAPTURE_H))
        img.putdata([self.palette[b] for b in raw[:WIDTH * CAPTURE_H]])
        return img


def read_capture():
    cap = Capture()
    tag = None
    with open(os.path.join(CAPTURE_DIR, "views.txt")) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "PAL":
                cap.palette[int(parts[1])] = (int(parts[2]), int(parts[3]),
                                              int(parts[4]))
            elif parts[0] == "CARD_TEX":
                cap.card_tex = (int(parts[1]), int(parts[2]))
            elif parts[0] == "BG_INDEX":
                cap.bg_index = int(parts[1])
            elif parts[0] == "MOVE":
                cap.moves.append((parts[1], int(parts[2])))
            elif parts[0] == "POSE":
                tag = parts[1]
                cap.poses[tag] = [None] * FIELD_SLOTS
            elif parts[0] == "MESH":
                kind = parts[1]
                i, j = int(parts[2]), int(parts[3])
                x, y = int(parts[4]), int(parts[5]) - CROP_Y
                m = cap.mesh.setdefault(tag, {})
                if kind == "SIDE":
                    # x and y are the two "which wall faces us" flags, not a
                    # point; they are carried in the same lines so the pose
                    # record stays one contiguous block.
                    m["SIDE"] = (int(parts[4]), int(parts[5]))
                elif kind == "TOP":
                    m[("TOP", i, j)] = (x, y)
                else:
                    m[(kind, i)] = (x, y)
            elif parts[0] == "QUAD":
                # Into window space here and nowhere else, so every consumer
                # below is talking about the same 212 rows the VDP shows.
                pts = [int(v) for v in parts[2:]]
                cap.poses[tag][int(parts[1])] = [
                    (pts[i * 2], pts[i * 2 + 1] - CROP_Y) for i in range(4)]
    if not cap.poses:
        raise SystemExit("gen_msx_views: the capture is empty")
    return cap


# ── The resting picture ──────────────────────────────────────────────────────

def backdrop(stage):
    """The PC-FX/headless battle surround: ungraded, literal black."""
    del stage
    return Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))


_ARENA_CACHE = {}


def arena_layer(cap, tag):
    """A captured pose as (board pixels, where the arena is NOT).

    `render_board()` clears to index 0 and draws nothing behind the arena --
    every console target paints its own backdrop there -- so index 0 is exactly
    'this pixel is not the arena', and the PC-FX-black surround shows through. The
    mask depends only on the capture, so it is cut once and reused by all four
    stages."""
    if tag in _ARENA_CACHE:
        return _ARENA_CACHE[tag]
    raw = open(os.path.join(CAPTURE_DIR, tag + ".raw"), "rb").read()
    board = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    mask = Image.new("L", (WIDTH, HEIGHT), 255)
    bpx, mpx = board.load(), mask.load()
    for wy in range(HEIGHT):
        cy = wy + CROP_Y
        if cy >= CAPTURE_H:
            break
        row = cy * WIDTH
        for x in range(WIDTH):
            index = raw[row + x]
            if index != cap.bg_index:
                bpx[x, wy] = cap.palette[index]
                mpx[x, wy] = 0
    _ARENA_CACHE[tag] = (board, mask)
    return board, mask


def over_card_xy():
    """The top-left of each overhead slot's 32x42 card, in window pixels.

    The card is as tall as the tile pitch, so the four rows tile the picture
    exactly: y is the row's own top, not the tile interior's centre less half a
    card.  Centring is what the two-row board did, and it put the grid two
    pixels ABOVE the picture -- which the interior rows could afford and the
    outer two, now that they are the support rows, cannot: the top one would
    have overhung the HUD and its restore tile would have overlapped the row
    below it."""
    out = []
    for row in OVER_SLOT_ROWS:
        y = OVER_Y + OVER_TILE_Y0 + row * OVER_TILE_PITCH_Y
        for col in range(FIELD_SLOTS // 4):
            cx = OVER_TILE_X0 + col * OVER_TILE_PITCH_X + OVER_TILE_W // 2
            out.append((cx - OVER_CARD_W // 2, y))
    return out


def over_quads():
    """The ten field slots of the overhead view, as the card rectangles.

    Corner order is the capture's: texture top-left, top-right, bottom-right,
    bottom-left.  The COM's row is wound half a turn round so its cards face
    its own chair, exactly as the captured COM row does -- nothing rasterises
    through these quads any more, but the empty-slot tiles and the cursor
    geometry are still cut from them, so the winding has to stay honest."""
    per_row = FIELD_SLOTS // 4
    quads = []
    for i, (x0, y0) in enumerate(over_card_xy()):
        # The LAST pixel of the card, not one past it: quad_box rounds a corner
        # up and adds one, so an exclusive edge here would make every restore
        # box a pixel too wide and a pixel too tall -- and the rows are flush.
        x1, y1 = x0 + OVER_CARD_W - 1, y0 + OVER_CARD_H - 1
        # The opponent's two rows -- its monsters (0..4) and its supports
        # (10..14) -- are wound half a turn round so their cards face its own
        # chair, exactly as the captured COM rows are.
        com = (i // per_row) in (0, 2)
        if com:
            quads.append([(x1, y1), (x0, y1), (x0, y0), (x1, y0)])
        else:
            quads.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
    return quads


_OVER_CACHE = []


def over_scene(stage):
    """The overhead table, on the same 212-row screen as a captured view."""
    del stage                       # the flat board does not take the tint
    if not _OVER_CACHE:
        art = Image.open(OVER_ART).convert("RGB")
        if art.width != WIDTH:
            art = art.resize((WIDTH, art.height), Image.Resampling.LANCZOS)
        _OVER_CACHE.append(art)
    art = _OVER_CACHE[0]
    img = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    img.paste(art.crop((0, 0, WIDTH, min(art.height, OVER_H))), (0, OVER_Y))
    return img


def composite_arena(cap, tag, stage):
    board, mask = arena_layer(cap, tag)
    return Image.composite(backdrop(stage), board, mask)


def pad_segments(data):
    """Round a blob up to whole 16 KB cartridge segments, because the streamer
    addresses an asset by its first segment and nothing else."""
    span = (len(data) + SEGMENT_BYTES - 1) // SEGMENT_BYTES
    return data + bytes(span * SEGMENT_BYTES - len(data))


def quad_center(quad):
    return (sum(p[0] for p in quad) / 4.0, sum(p[1] for p in quad) / 4.0)


def expand_quad(quad, amount):
    cx, cy = quad_center(quad)
    out = []
    for x, y in quad:
        dx, dy = x - cx, y - cy
        d = math.hypot(dx, dy) or 1.0
        out.append((x + dx / d * amount, y + dy / d * amount))
    return out


def paint_panels(img, hand_row=True):
    """The three baked UI grounds.

    They are flat, and that is load-bearing: live text is written over them
    every turn and GRAPHIC 7 has no way to erase back to artwork except by
    putting the artwork there again."""
    d = ImageDraw.Draw(img)
    for y0, h in ((0, HUD_H), (INFO_Y, HEIGHT - INFO_Y)):
        d.rectangle([0, y0, WIDTH - 1, y0 + h - 1], fill=PANEL_RGB,
                    outline=GOLD_RGB)
    # The hand row has no panel of its own.  A slab of flat violet behind the
    # five cards is a dialogue box where the table should be, and it breaks the
    # illusion that the hand is being held over the arena; the cards sit on
    # black instead, which is what PC-FX and FM TOWNS show.
    if not hand_row:
        return img
    # Plain black, with no frame round the slot pitch.  The gold hairline that
    # used to be baked here was the same outline the drawn one was removed for:
    # the card art carries its own border, so the extra ring only read as a
    # brown box around every cover.  The HUD and info panel keep theirs.
    d.rectangle([0, HAND_BAND_Y, WIDTH - 1, HAND_BAND_Y + HAND_BAND_H - 1],
                fill=(0, 0, 0))
    return img


def build_view(cap, stage, tag):
    quads = cap.poses[tag]
    img = over_scene(stage) if tag == "OVER" else composite_arena(cap, tag, stage)
    # The overhead view owns the hand's rows, so it must not be given the five
    # empty hand frames -- there is no hand on this screen.
    paint_panels(img, hand_row=(tag != "OVER"))
    return img


def build_move_strip(cap, stage, move, poses):
    """§4.6: a strip of finished pictures of the board band, in order.

    Only the band moves -- the HUD, the hand row and the info panel are opaque
    and stand still -- so a pose is 114 rows rather than 212, which is the one
    lever §4.6.2 leaves for playback rate."""
    frames = []
    for pose in range(poses):
        tag = "MOVE_%s_%d" % (move, pose)
        img = composite_arena(cap, tag, stage)
        band = img.crop((0, BAND_Y, WIDTH, BAND_Y + BAND_H))
        frames.append(pad_segments(grb.quantize(band, (WIDTH, BAND_H))))
    return b"".join(frames)


def cut_slot_tiles(scene, quads, bottom=BAND_Y + BAND_H, pad=SLOT_PAD,
                   padx=0.0):
    """The empty-slot rectangle of one arena at each slot, in the arena's own
    quantised bytes -- so putting a destroyed monster back is the picture, not
    something close to it."""
    blob = bytearray()
    boxes = []
    for quad in quads:
        x0, y0, x1, y1 = quad_box(quad, bottom, pad, padx)
        tile = bytearray()
        for y in range(y0, y1):
            start = y * WIDTH + x0
            tile += scene[start:start + (x1 - x0)]
        if len(tile) > SLOT_STRIDE:
            raise SystemExit("gen_msx_views: slot tile %dx%d exceeds the stride"
                             % (x1 - x0, y1 - y0))
        blob += tile + bytes(SLOT_STRIDE - len(tile))
        boxes.append((x0, y0, x1 - x0, y1 - y0))
    return bytes(blob), boxes


def quad_box(quad, bottom=BAND_Y + BAND_H, pad=SLOT_PAD, padx=0.0):
    """The pixel box a quad's ring and card occupy, clamped to the view.

    `bottom` is the last row the view's picture reaches: the two captured
    chairs stop at the hand row, the overhead view goes on through it.

    `pad` is the margin around the quad, and the overhead view must NOT have
    one, because its two rows of cards are flush -- a card overhangs its tile by
    two rows into the four-row gutter, from both sides -- so a padded box would
    take the bottom of the opponent's card with it every time the slot below was
    emptied, and the retained painter would never put it back.

    `padx` widens the box in x alone.  That is what a defence card needs
    overhead: turned a quarter turn it is OVER_CARD_H wide rather than
    OVER_CARD_W, and the tile that restores the slot has to cover the widest
    thing that can ever be drawn in it.  There is room for it sideways -- the
    tile pitch is 48 and the card is 42 -- and none of it vertically, which is
    exactly why this is one number and not two."""
    outer = expand_quad(quad, pad)
    x0 = max(0, int(math.floor(min(p[0] for p in outer) - padx)))
    x1 = min(WIDTH, int(math.ceil(max(p[0] for p in outer) + padx)) + 1)
    y0 = max(BAND_Y, int(math.floor(min(p[1] for p in outer))))
    y1 = min(bottom, int(math.ceil(max(p[1] for p in outer))) + 1)
    return x0, y0, x1, y1


# ── §8.4: the baked span programs ────────────────────────────────────────────

def bilinear_point(quad, u, v):
    """The window pixel a quad's own (u, v) lands on -- inverse_bilinear's
    forward direction, which is what places a turned card inside a slot."""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = quad
    return ((1 - u) * (1 - v) * x0 + u * (1 - v) * x1 + u * v * x2 +
            (1 - u) * v * x3,
            (1 - u) * (1 - v) * y0 + u * (1 - v) * y1 + u * v * y2 +
            (1 - u) * v * y3)


def defence_quad(quad):
    """The footprint of a card lying a quarter turn round in this slot.

    It is INSCRIBED in the upright quad -- the full width, DEF_FIT of the height,
    centred -- rather than sticking out sideways the way the overhead board can
    afford to.  Two reasons, and both of them are the retained painter's: the
    chair slots sit shoulder to shoulder, and every erase in this file is the
    slot's own restore tile, which is cut from the upright quad.  A card that
    reached past it would leave a strip of itself behind for the rest of the
    duel.

    The corner order is the upright quad's, so a span program baked through it
    reads the pre-turned texture upright and the turn comes entirely out of the
    art (gen_msx_scenes.py's defence card blob).  Nothing here rotates pixels."""
    v0 = (1.0 - DEF_FIT) / 2.0
    v1 = 1.0 - v0
    return [bilinear_point(quad, u, v)
            for u, v in ((0.0, v0), (1.0, v0), (1.0, v1), (0.0, v1))]


def inverse_bilinear(quad, x, y):
    """Where (x, y) sits in the quad's own (u, v), or None if outside.

    Solved by Newton iteration on the bilinear map rather than in closed form:
    the closed form has a degenerate branch for a quad whose opposite edges are
    parallel, which every top-down board slot very nearly is."""
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = quad
    u = v = 0.5
    for _ in range(24):
        # P(u,v) = (1-u)(1-v)p0 + u(1-v)p1 + uv p2 + (1-u)v p3
        px = ((1 - u) * (1 - v) * x0 + u * (1 - v) * x1 + u * v * x2 +
              (1 - u) * v * x3)
        py = ((1 - u) * (1 - v) * y0 + u * (1 - v) * y1 + u * v * y2 +
              (1 - u) * v * y3)
        ex, ey = px - x, py - y
        if abs(ex) < 1e-6 and abs(ey) < 1e-6:
            break
        dxu = (1 - v) * (x1 - x0) + v * (x2 - x3)
        dyu = (1 - v) * (y1 - y0) + v * (y2 - y3)
        dxv = (1 - u) * (x3 - x0) + u * (x2 - x1)
        dyv = (1 - u) * (y3 - y0) + u * (y2 - y1)
        det = dxu * dyv - dxv * dyu
        if abs(det) < 1e-9:
            return None
        u -= (dyv * ex - dxv * ey) / det
        v -= (dxu * ey - dyu * ex) / det
    if -0.002 <= u <= 1.002 and -0.002 <= v <= 1.002:
        return min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0)
    return None


def emit_runs(ops, opcode, count):
    while count > 0:
        n = min(count, OP_MAX_RUN)
        ops.append(opcode | n)
        count -= n


def bake_span_program(quad, tex_w, tex_h):
    """One slot's quad rasterised offline into run lengths.

    This is turbor's "linear offsetting with a precomputed offset list" (§8.2)
    with the offsets expressed as runs, so the interpreter's inner loop is block
    I/O rather than arithmetic.  A destination row samples ONE texture row --
    the one its own midpoint falls in -- which is what makes a run length
    meaningful at all; the error that buys is under a pixel at these quad sizes,
    and it is what removes every multiply from the Z80's side.

    The program is a list of row records:

        dy, x0, src_lo, src_hi, dir, <ops...>, ENDROW
        ...
        END

    A row is always ONE contiguous destination run, so the interpreter sets the
    VRAM write address once per record and then only pushes bytes.  A row of the
    quad that somehow breaks into two runs becomes two records rather than a
    SKIP, which is why the runtime has no SKIP case to get wrong."""
    x0b, y0b, x1b, y1b = quad_box(quad)
    prog = bytearray()

    for y in range(y0b, y1b):
        inside = []
        for x in range(x0b, x1b):
            uv = inverse_bilinear(quad, x + 0.5, y + 0.5)
            if uv is not None:
                inside.append((x, uv))
        if not inside:
            continue
        # Split into contiguous destination runs; for a convex quad there is
        # exactly one, but nothing here depends on that being true.
        runs = []
        run = [inside[0]]
        for item in inside[1:]:
            if item[0] == run[-1][0] + 1:
                run.append(item)
            else:
                runs.append(run)
                run = [item]
        runs.append(run)

        for run in runs:
            # One texture row for the whole run, taken at its middle.
            mid_v = run[len(run) // 2][1][1]
            trow = min(tex_h - 1, max(0, int(mid_v * tex_h)))
            cols = [min(tex_w - 1, max(0, int(u * tex_w))) for _x, (u, _v) in run]
            step = 1 if cols[-1] >= cols[0] else -1

            ops = bytearray()
            pending_copy = 1      # the first texel is always written
            pending_dup = 0
            prev = cols[0]

            def flush(copy, dup):
                if copy:
                    emit_runs(ops, OP_COPY, copy)
                if dup:
                    emit_runs(ops, OP_DUP, dup)

            for col in cols[1:]:
                delta = (col - prev) * step
                if delta == 0:
                    if pending_copy:
                        emit_runs(ops, OP_COPY, pending_copy)
                        pending_copy = 0
                    pending_dup += 1
                elif delta == 1:
                    if pending_dup:
                        emit_runs(ops, OP_DUP, pending_dup)
                        pending_dup = 0
                    pending_copy += 1
                else:
                    flush(pending_copy, pending_dup)
                    pending_copy = pending_dup = 0
                    emit_runs(ops, OP_ADV, delta - 1)
                    pending_copy = 1
                prev = col
            flush(pending_copy, pending_dup)
            ops.append(OP_ENDROW)

            # A run whose texels walk backwards is read out of the MIRRORED
            # copy of the texture instead, forwards.  That is what lets the
            # whole interpreter advance in one direction and reuse the ordinary
            # forward rectangle blit for a COPY -- a reverse block copy would be
            # a second inner loop earning nothing but the COM row.
            col0 = cols[0] if step > 0 else (tex_w - 1 - cols[0])
            src = trow * tex_w + col0
            prog += bytes((y, run[0][0], src & 0xFF, (src >> 8) & 0xFF,
                           0 if step > 0 else 1))
            prog += ops

    prog.append(OP_END)
    return bytes(prog), 0


def bake_spans(cap, tags):
    """Every span program, at the stride the streamer can address it by.

    Two per slot: the upright card, and the same card lying a quarter turn round
    for defence position.  The turned one samples a texture that is already
    turned -- 48 wide by 40 tall -- so its destination rows still walk one
    texture row forwards and the interpreter is the same three opcodes it always
    was.  Rotating at replay time would have made every texel its own ADV."""
    blob = bytearray()
    offsets = []
    def_offsets = []
    longest = 0

    def emit(quad, tex_w, tex_h, into):
        nonlocal longest
        into.append(len(blob) // SPAN_STRIDE)
        prog, _rows = bake_span_program(quad, tex_w, tex_h)
        longest = max(longest, len(prog))
        if len(prog) > SPAN_STRIDE:
            raise SystemExit("gen_msx_views: a span program is %d bytes, "
                             "past the %d stride" % (len(prog), SPAN_STRIDE))
        return prog + bytes(SPAN_STRIDE - len(prog))

    for tag in tags:
        for quad in cap.poses[tag]:
            blob += emit(quad, CARD_W, CARD_H, offsets)
    for tag in tags:
        for quad in cap.poses[tag]:
            blob += emit(defence_quad(quad), CARD_H, CARD_W, def_offsets)
    return bytes(blob), offsets, def_offsets, longest


# ── Driving it all ───────────────────────────────────────────────────────────

# ── The board as geometry (docs/MSX2_REALTIME_POLYGON_FINDINGS.md) ───────────
#
# The cartridge used to carry a 54,272-byte picture of the arena for every
# camera pose -- 3.9 MB once the opening and the turn orbit were counted -- and
# the Z80 pushed those bytes at the VDP data port at 32 T-states each.  The
# V9938's command engine fills a flat rectangle at 8 T-states a byte and leaves
# the CPU free while it does it, so the arena is now DRAWN: the cartridge
# carries the projected mesh (157 bytes a pose) and the flat colours the capture
# measures, and the Z80 issues one HMMV per scanline of every tile.
#
# The mesh is projected HERE, by the game's own renderer, and not on the Z80.
# That is the same rule as the card quads: MSX2_PORT_PLAN.md §4.3 wants the MSX2
# board to BE the shared board, and a Z80 reimplementation of project_point()
# would drift from it.  Projection is also the one part of the frame that is
# genuinely free to bake -- the camera path is authored -- while the fill is the
# part that is not, which is exactly the split the findings document argues for.

# The point order inside a pose record.  It is the mesh of BoardProjected:
# the (BOARD_ROWS+1) x (BOARD_COLS+1) top surface, then the underside of the
# four slab edges, from which the two camera-facing walls are built.
MESH_ROWS, MESH_COLS = 4, 5
MESH_TOP_PTS = (MESH_ROWS + 1) * (MESH_COLS + 1)          # 30
MESH_BX0 = MESH_TOP_PTS                                    # 30..34
MESH_BX1 = MESH_BX0 + (MESH_ROWS + 1)                      # 35..39
MESH_BZ0 = MESH_BX1 + (MESH_ROWS + 1)                      # 40..45
MESH_BZ1 = MESH_BZ0 + (MESH_COLS + 1)                      # 46..51
MESH_POINTS = MESH_BZ1 + (MESH_COLS + 1)                   # 52

# One pose record: 52 signed 16-bit x, 52 unsigned y, one flags byte, then the
# attack- and defence-position card quads for all twenty field cells.  The card
# corners are already emitted by the shared renderer for every pose; carrying
# them here is what lets a turn orbit redraw the occupied field on the moving
# board instead of flipping five empty arenas.  The 512-byte stride keeps every
# record inside one mapper segment.
MESH_STRIDE = 512


def mesh_index_top(r, c):
    return r * (MESH_COLS + 1) + c


def mesh_points(cap, tag):
    """One pose as (points, flags), in the point order above."""
    m = cap.mesh[tag]
    pts = [None] * MESH_POINTS
    for r in range(MESH_ROWS + 1):
        for c in range(MESH_COLS + 1):
            pts[mesh_index_top(r, c)] = m[("TOP", r, c)]
    for r in range(MESH_ROWS + 1):
        pts[MESH_BX0 + r] = m[("BX0", r)]
        pts[MESH_BX1 + r] = m[("BX1", r)]
    for c in range(MESH_COLS + 1):
        pts[MESH_BZ0 + c] = m[("BZ0", c)]
        pts[MESH_BZ1 + c] = m[("BZ1", c)]
    side_x, side_z = m["SIDE"]
    return pts, (side_x | (side_z << 1))


def mesh_pose_tags(cap):
    """Every pose the cartridge can draw, in the order it indexes them."""
    tags = ["TOP", "COM"]
    for name, poses in cap.moves:
        tags += ["MOVE_%s_%d" % (name, pose) for pose in range(poses)]
    return tags


def board_mesh_blob(cap):
    blob = bytearray()
    for tag in mesh_pose_tags(cap):
        pts, flags = mesh_points(cap, tag)
        rec = bytearray()
        for x, _y in pts:
            # Clamped, not asserted: a pose only has to be drawable, and the
            # filler clips to the screen anyway.  The bound is what fits the
            # Z80 filler's 16-bit Bresenham without overflow.
            x = max(-512, min(511, int(round(x))))
            rec += (x & 0xFFFF).to_bytes(2, "little")
        for _x, y in pts:
            rec.append(max(0, min(255, int(round(y)))))
        rec.append(flags)
        for quads in (cap.poses[tag],
                      [defence_quad(q) for q in cap.poses[tag]]):
            for quad in quads:
                for x, y in quad:
                    rec.append(max(0, min(255, int(round(x)))))
                    rec.append(max(0, min(255, int(round(y)))))
        if len(rec) > MESH_STRIDE:
            raise SystemExit("gen_msx_views: mesh record overflows the stride")
        blob += rec + bytes(MESH_STRIDE - len(rec))
    return bytes(blob)


def region_mean(cap, tag, poly):
    """The captured arena's own mean colour inside one projected polygon.

    Measured rather than chosen: the shared renderer paints the board with
    textures the MSX2 cannot afford, and the honest flat stand-in for a texture
    is its average.  Reading it out of the capture also means a change to the
    arena's materials shows up here without anyone editing a constant."""
    raw = open(os.path.join(CAPTURE_DIR, tag + ".raw"), "rb").read()
    mask = Image.new("L", (WIDTH, HEIGHT), 0)
    ImageDraw.Draw(mask).polygon([(int(round(x)), int(round(y)))
                                  for x, y in poly], fill=255)
    mpx = mask.load()
    rs = gs = bs = n = 0
    for y in range(HEIGHT):
        cy = y + CROP_Y
        if cy >= CAPTURE_H:
            break
        for x in range(WIDTH):
            if not mpx[x, y]:
                continue
            index = raw[cy * WIDTH + x]
            if index == cap.bg_index:
                continue
            r, g_, b = cap.palette[index]
            rs += r
            gs += g_
            bs += b
            n += 1
    if not n:
        return None
    return (rs // n, gs // n, bs // n)


def board_colors(cap):
    """The four flat colours the live board is drawn in, from the capture."""
    pts, flags = mesh_points(cap, "TOP")
    def p(i):
        return pts[i]
    acc = {0: [0, 0, 0, 0], 1: [0, 0, 0, 0]}
    for r in range(MESH_ROWS):
        for c in range(MESH_COLS):
            poly = [p(mesh_index_top(r, c)), p(mesh_index_top(r, c + 1)),
                    p(mesh_index_top(r + 1, c + 1)), p(mesh_index_top(r + 1, c))]
            mean = region_mean(cap, "TOP", poly)
            if mean is None:
                continue
            a = acc[(r + c) & 1]
            a[0] += mean[0]; a[1] += mean[1]; a[2] += mean[2]; a[3] += 1
    tiles = []
    for parity in (0, 1):
        a = acc[parity]
        tiles.append((a[0] // a[3], a[1] // a[3], a[2] // a[3]))
    # The two walls the tactical pose shows.  Which pair faces the camera moves
    # with the pose; the colours do not, so they are measured once here.
    zr = MESH_ROWS if (flags & 2) else 0
    zb = MESH_BZ1 if (flags & 2) else MESH_BZ0
    zwall = region_mean(cap, "TOP",
                        [p(mesh_index_top(zr, 0)), p(mesh_index_top(zr, MESH_COLS)),
                         p(zb + MESH_COLS), p(zb)])
    xc = MESH_COLS if (flags & 1) else 0
    xb = MESH_BX1 if (flags & 1) else MESH_BX0
    xwall = region_mean(cap, "TOP",
                        [p(mesh_index_top(0, xc)), p(mesh_index_top(MESH_ROWS, xc)),
                         p(xb + MESH_ROWS), p(xb)])
    return {
        "tile_a": tiles[0],
        "tile_b": tiles[1],
        "wall_z": zwall or (72, 43, 19),
        "wall_x": xwall or (150, 110, 50),
    }


def bake(quiet=False, capture=True):
    """Everything this module owns, as blobs plus the numbers the header needs."""
    cap = run_capture(quiet) if capture else read_capture()
    # The overhead view is authored, not captured, so its quads are put into
    # the capture here and every consumer below -- rings, empty-slot tiles,
    # span programs, the generated geometry tables -- treats it as one more
    # pose.  That is the point: there is still exactly one definition of where
    # a card goes in a view.
    cap.poses["OVER"] = over_quads()
    view_tags = ("TOP", "COM", "OVER")
    views = {tag: [] for tag in view_tags}
    slots = bytearray()
    moves = {name: bytearray() for name, _n in cap.moves}
    # Geometry does not change with the stage tint.  Keep one box list per
    # camera view.
    #
    # ONE SLOT BLOB FOR ALL FOUR STAGES.  It used to carry a stage/view copy on
    # the grounds that a slot tile is cut from the captured stage IMAGE and so
    # follows the tint.  There is no tint: backdrop() ignores the stage and
    # every stage builds the same ungraded arena on the same black surround, so
    # the four copies were byte-for-byte identical -- 480 KB of cartridge for
    # one picture.  Doubling the slot count to take in the two support rows
    # would have made that 960 KB, which does not fit at all; deduplicating
    # instead makes the whole change SMALLER than what was there before.  If a
    # stage ever really does grade the arena, this loop is where the stage index
    # comes back.
    boxes = {tag: None for tag in view_tags}
    for tag in view_tags:
        quads = cap.poses[tag]
        over = (tag == "OVER")
        bottom = (HAND_BAND_Y + HAND_BAND_H) if over else (BAND_Y + BAND_H)
        data = grb.quantize(build_view(cap, 0, tag), (WIDTH, HEIGHT))
        tiles, stage_boxes = cut_slot_tiles(data, quads, bottom,
                                            0.0 if over else SLOT_PAD,
                                            OVER_DEF_PADX if over else 0.0)
        slots += tiles
        boxes[tag] = stage_boxes
    for stage in range(BOARD_STAGES):
        for tag in view_tags:
            data = grb.quantize(build_view(cap, stage, tag), (WIDTH, HEIGHT))
            views[tag].append(pad_segments(data))
        for name, poses in cap.moves:
            moves[name] += build_move_strip(cap, stage, name, poses)
        if not quiet:
            print("BOARD_%-8s %d views x %d bytes"
                  % (STAGE_NAMES[stage], len(view_tags), WIDTH * HEIGHT))
    if not quiet:
        print("SLOTS    %d views x %d tiles, shared by every stage"
              % (len(view_tags), FIELD_SLOTS))

    spans, span_off, span_def_off, span_max = bake_spans(cap, list(view_tags))
    if not quiet:
        print("SPANS    %d programs -> %d bytes (longest %d B)"
              % (len(span_off), len(spans), span_max))
        for name, poses in cap.moves:
            print("MOVE_%-8s %d poses x %d rows -> %d bytes"
                  % (name, poses, BAND_H, len(moves[name])))

    mesh = board_mesh_blob(cap)
    colors = board_colors(cap)
    if not quiet:
        print("MESH     %d poses x %d bytes -> %d bytes, colours %s"
              % (len(mesh) // MESH_STRIDE, MESH_STRIDE, len(mesh), colors))

    return {
        "mesh": mesh,
        "mesh_tags": mesh_pose_tags(cap),
        "colors": colors,
        # Runtime addressing interleaves camera views inside each stage:
        # stage 0 TOP, stage 0 COM, stage 1 TOP, stage 1 COM, ... .  Keep the
        # flattened blob in that same order; grouping by tag here would make
        # MSX2_VIEW_SEGMENT(stage, view) select the wrong arena tint.
        "views": [views[tag][stage]
                  for stage in range(BOARD_STAGES)
                  for tag in view_tags],
        "view_tags": view_tags,
        "slots": bytes(slots),
        "slot_boxes": boxes,
        "moves": [(name, poses, bytes(moves[name])) for name, poses in cap.moves],
        "spans": spans,
        "span_offsets": span_off,
        "span_def_offsets": span_def_off,
        "span_max": span_max,
        "quads": {tag: cap.poses[tag] for tag in view_tags},
    }


def header_lines(baked, view_seg, slot_seg, move_segs, span_seg, mesh_seg):
    """The generated constants.  Emitted from here so the board's geometry has
    exactly one definition and `msx2_board.c` cannot drift from the capture."""
    out = []
    a = out.append
    a("// ── The duel board, captured from the game's own renderer ──────────────")
    a("// tools/msx2/gen_msx_views.py bakes these out of --dump-msx2-views, so the")
    a("// arena, the perspective and the slot layout are the ones every other")
    a("// target renders (MSX2_PORT_PLAN.md §4.3).  Nothing here is drawn offline.")
    a("#define MSX2_VIEW_TOP          0")
    a("#define MSX2_VIEW_COM          1")
    a("// The overhead view is not captured: it is the flat board of")
    a("// assets/source/msx2/msx2_3d_top_view.png, and it takes the hand's rows")
    a("// as well as the board band, because it is the view with no hand on it.")
    a("#define MSX2_VIEW_OVER         2")
    a("#define MSX2_OVER_Y            %d" % OVER_Y)
    a("#define MSX2_OVER_H            %d" % OVER_H)
    a("// A card on the overhead board is a rectangle copy at its own baked")
    a("// size, not a span program: the slots are axis-aligned there.")
    a("// The table's own grid, so the overhead view can be DRAWN as flat")
    a("// rectangles rather than streamed as a picture of one.")
    a("#define MSX2_OVER_TILE_X0      %d" % OVER_TILE_X0)
    a("#define MSX2_OVER_TILE_PITCH_X %d" % OVER_TILE_PITCH_X)
    a("#define MSX2_OVER_TILE_W       %d" % OVER_TILE_W)
    a("#define MSX2_OVER_TILE_Y0      %d" % OVER_TILE_Y0)
    a("#define MSX2_OVER_TILE_PITCH_Y %d" % OVER_TILE_PITCH_Y)
    a("#define MSX2_OVER_TILE_H       %d" % OVER_TILE_H)
    a("#define MSX2_OVER_TILE_ROWS    %d" % len(OVER_SLOT_ROWS))
    a("#define MSX2_OVER_TILE_COLS    %d" % MESH_COLS)
    a("#define MSX2_OVER_CARD_W       %d" % OVER_CARD_W)
    a("#define MSX2_OVER_CARD_H       %d" % OVER_CARD_H)
    a("#define MSX2_BOARD_VIEWS       %d" % len(baked["view_tags"]))
    a("#define MSX2_VIEW_SEGMENT(stage, view)  (%d + (((stage) * MSX2_BOARD_VIEWS + (view)) * MSX2_SCENE_SEG_SPAN))" % view_seg)
    a("#define MSX2_VIEW_STAGES        %d" %
      (len(baked["views"]) // len(baked["view_tags"])))
    a("#define MSX2_BAND_Y             %d" % BAND_Y)
    a("#define MSX2_BAND_H             %d" % BAND_H)
    a("#define MSX2_HUD_H              %d" % HUD_H)
    a("#define MSX2_HAND_BAND_Y        %d" % HAND_BAND_Y)
    a("#define MSX2_HAND_BAND_H        %d" % HAND_BAND_H)
    a("#define MSX2_INFO_Y             %d" % INFO_Y)
    a("// A card in DEFENCE position is inscribed in its slot: the full width,")
    a("// DEF_FIT of the height, centred.  This is the inset at each end, in")
    a("// 1/256ths of the slot's own height, so the runtime mapper cuts exactly")
    a("// the footprint the baked programs used to.")
    a("#define MSX2_DEF_INSET          %d" % int(round((1.0 - DEF_FIT) / 2.0 * 256)))
    a("#define MSX2_CARD_W             %d" % CARD_W)
    a("#define MSX2_CARD_H             %d" % CARD_H)
    a("#define MSX2_HAND_X0            %d" % HAND_X0)
    a("#define MSX2_HAND_PITCH         %d" % HAND_PITCH)
    a("#define MSX2_HAND_Y             %d" % HAND_Y)
    a("#define MSX2_FIELD_SLOTS        %d" % FIELD_SLOTS)
    a("#define MSX2_HAND_SLOTS         %d" % HAND_SLOTS)
    a("#define MSX2_PANEL_COLOR        0x%02X" % grb.pack(*PANEL_RGB))
    a("#define MSX2_GOLD_COLOR         0x%02X" % grb.pack(*GOLD_RGB))
    a("")
    a("// ONE COPY OF THE GEOMETRY, NOT ONE PER TRANSLATION UNIT.")
    a("// The small retained-view tables live in one translation unit rather than")
    a("// being duplicated by every source that includes this header.")
    a("// It is declared here and DEFINED once, in msx2_cards.c, which is the")
    a("// translation unit that already exists to hold generated tables.")
    a("extern const unsigned char g_msx2_over_card_xy[MSX2_FIELD_SLOTS][2];")
    a("// The box a slot's ring and card occupy: what an empty slot restores, and")
    a("// what a repaint has to cover.")
    a("extern const unsigned char g_msx2_slot_box[MSX2_BOARD_VIEWS][MSX2_FIELD_SLOTS][4];")
    a("")
    a("// ── §8.4 Tier A span programs ─────────────────────────────────────────")
    a("// One offline rasterisation of the card texture into each slot's quad,")
    a("// as run lengths.  The Z80 replays it with block I/O and does no")
    a("// arithmetic at all; that is what makes fifteen perspective cards")
    a("// affordable on a 3.58 MHz machine.")
    a("#define MSX2_SPAN_SEGMENT       %d" % span_seg)
    a("#define MSX2_SPAN_STRIDE        %d" % SPAN_STRIDE)
    a("#define MSX2_SPAN_PER_SEG       %d" % (SEGMENT_BYTES // SPAN_STRIDE))
    a("#define MSX2_SPAN_MAX           %d" % baked["span_max"])
    a("#define MSX2_OP_COPY            0x%02X" % OP_COPY)
    a("#define MSX2_OP_DUP             0x%02X" % OP_DUP)
    a("#define MSX2_OP_SKIP            0x%02X" % OP_SKIP)
    a("#define MSX2_OP_ADV             0x%02X" % OP_ADV)
    a("#define MSX2_OP_ENDROW          0x%02X" % OP_ENDROW)
    a("#define MSX2_OP_END             0x%02X" % OP_END)
    a("#define MSX2_OP_RUN_MASK        0x%02X" % OP_MAX_RUN)
    a("")
    a("// ── Empty-slot tiles ──────────────────────────────────────────────────")
    a("// One set for every stage: the arena is ungraded, so the four stages cut")
    a("// the same pixels and the blob carried four identical copies of them.")
    a("#define MSX2_SLOT_ART_SEGMENT   %d" % slot_seg)
    a("#define MSX2_SLOT_ART_STRIDE    %d" % SLOT_STRIDE)
    a("#define MSX2_SLOT_ART_PER_SEG   %d" % (16384 // SLOT_STRIDE))
    a("#define MSX2_SLOT_ART_PER_VIEW  %d" % FIELD_SLOTS)
    a("#define MSX2_SLOT_ART_VIEWS     %d" % len(baked["view_tags"]))
    a("")
    a("// ── The live board: geometry, not a picture ───────────────────────────")
    a("// docs/MSX2_REALTIME_POLYGON_FINDINGS.md: the V9938 fills a flat")
    a("// rectangle four times faster than the Z80 can push bytes at the data")
    a("// port, so the arena is drawn by the command engine from the projected")
    a("// mesh below rather than streamed as 54 KB of pixels a pose.  One record")
    a("// is 52 corners (x as int16, y as a byte), the facing-wall flags, and")
    a("// attack/defence card quads for all twenty cells.")
    a("#define MSX2_MESH_SEGMENT       %d" % mesh_seg)
    a("#define MSX2_MESH_STRIDE        %d" % MESH_STRIDE)
    a("#define MSX2_MESH_PER_SEG       %d" % (SEGMENT_BYTES // MESH_STRIDE))
    a("#define MSX2_MESH_POINTS        %d" % MESH_POINTS)
    a("#define MSX2_MESH_ROWS          %d" % MESH_ROWS)
    a("#define MSX2_MESH_COLS          %d" % MESH_COLS)
    a("#define MSX2_MESH_TOP(r, c)     ((r) * (MSX2_MESH_COLS + 1) + (c))")
    a("#define MSX2_MESH_BX0           %d" % MESH_BX0)
    a("#define MSX2_MESH_BX1           %d" % MESH_BX1)
    a("#define MSX2_MESH_BZ0           %d" % MESH_BZ0)
    a("#define MSX2_MESH_BZ1           %d" % MESH_BZ1)
    a("// bit 0: the +X wall faces the camera.  bit 1: the +Z wall does.")
    a("#define MSX2_MESH_FLAG_XPOS     0x01")
    a("#define MSX2_MESH_FLAG_ZPOS     0x02")
    a("#define MSX2_MESH_QUAD_OFFSET   %d" % (MESH_POINTS * 3 + 1))
    a("#define MSX2_MESH_QUAD_BYTES    %d" % (FIELD_SLOTS * 8))
    a("// Pose indices inside the blob.")
    for i, tag in enumerate(baked["mesh_tags"]):
        if tag in ("TOP", "COM"):
            a("#define MSX2_MESH_POSE_%-9s %d" % (tag, i))
    for name, poses, _blob in baked["moves"]:
        first = baked["mesh_tags"].index("MOVE_%s_0" % name)
        a("#define MSX2_MESH_POSE_%s(pose)  (%d + (pose))" % (name, first))
    a("// The arena's own colours, measured off the capture: a texture the MSX2")
    a("// cannot afford is honestly stood in for by its average.")
    c = baked["colors"]
    a("#define MSX2_BOARD_TILE_A       0x%02X   // (row + col) even" % grb.pack(*c["tile_a"]))
    a("#define MSX2_BOARD_TILE_B       0x%02X" % grb.pack(*c["tile_b"]))
    a("#define MSX2_BOARD_WALL_Z       0x%02X   // the long facing wall" % grb.pack(*c["wall_z"]))
    a("#define MSX2_BOARD_WALL_X       0x%02X   // the side lip" % grb.pack(*c["wall_x"]))
    a("")
    a("// ── §4.6 baked camera moves ───────────────────────────────────────────")
    a("// A strip of whole pictures of the board band, streamed one after the")
    a("// next by the ordinary §6.2 path.  There is no codec and no decoder: at")
    a("// 29 T-states a byte a band pose is about twelve video frames, so a move")
    a("// is a held cinematic push and the art is authored for that (§4.6.2).")
    a("#define MSX2_MOVE_POSE_BYTES    %d" % (WIDTH * BAND_H))
    a("#define MSX2_MOVE_POSE_SEGS     %d"
      % ((WIDTH * BAND_H + 16383) // 16384))
    for (name, poses, _blob), seg in zip(baked["moves"], move_segs):
        a("#define MSX2_MOVE_%s_SEGMENT(stage)  (%d + (stage) * %d * MSX2_MOVE_POSE_SEGS)"
          % (name, seg, poses))
        a("#define MSX2_MOVE_%s_POSES    %d" % (name, poses))
    a("")
    return out


def data_lines(baked):
    """The definitions of the four geometry tables declared in the header.

    They live in their own generated file so that exactly one translation unit
    (msx2_cards.c) carries them: a `static const` in a header that a dozen files
    include is a dozen copies in the ROM, and the board's geometry is nearly a
    kilobyte of it."""
    out = []
    a = out.append
    a("// Generated by tools/msx2/gen_msx_views.py -- do not edit.")
    a("// Included by exactly one translation unit; see msx2_scenes.h.")
    a("")
    a("const unsigned char g_msx2_over_card_xy[MSX2_FIELD_SLOTS][2] = {")
    for x, y in over_card_xy():
        a("\t{ %d, %d }," % (x, y))
    a("};")
    a("")
    a("const unsigned char g_msx2_slot_box[MSX2_BOARD_VIEWS][MSX2_FIELD_SLOTS][4] = {")
    for tag in baked["view_tags"]:
        a("\t{")
        for box in baked["slot_boxes"][tag]:
            a("\t\t{ %d, %d, %d, %d }," % box)
        a("\t},")
    a("};")
    a("")
    return out


def main():
    quiet = "--quiet" in sys.argv
    os.makedirs(ASSET_DIR, exist_ok=True)
    baked = bake(quiet)
    total = sum(len(v) for v in baked["views"]) + len(baked["slots"]) + \
        len(baked["spans"]) + sum(len(b) for _n, _p, b in baked["moves"])
    print("board assets: %d bytes (%d KB)" % (total, total // 1024))


if __name__ == "__main__":
    main()
