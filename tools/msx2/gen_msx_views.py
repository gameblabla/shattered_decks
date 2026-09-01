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
    the same black surround as PC-FX, a flat ring around every
    slot, and the three baked UI panels.  One per story stage.
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

HUD_H = 13                  # rows 0..12      -- LP, turn counter
BAND_Y = 14                 # rows 14..127    -- the board itself
BAND_H = 114
HAND_BAND_Y = BAND_Y + BAND_H   # rows 128..181 -- the player's hand
HAND_BAND_H = 54
INFO_Y = HAND_BAND_Y + HAND_BAND_H   # rows 182..211 -- card name, ATK/DEF, prompt

CARD_W, CARD_H = 40, 48     # the one master texture, §4.4
HAND_X0 = 12
HAND_PITCH = 47
HAND_Y = HAND_BAND_Y + 3

FIELD_SLOTS = 10
HAND_SLOTS = 5

# The ring baked around every slot.  §8.5's transparency key is colour 0, and
# this is its counterpart: the cursor is drawn INTO the ring, so erasing it is a
# redraw in this exact colour and no artwork underneath is ever repaired.
RING = 2
RING_RGB = (72, 40, 8)
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


def draw_slot_rings(img, quads):
    """A flat ring just outside every slot's quad.

    §21.1 keeps `msx2_board.c`'s ring trick and notes that the captured arena
    carries no highlight of its own, so the generator draws one -- which is the
    fallback that section names.  It is drawn OUTSIDE the quad so a card never
    covers it, and it is flat so the runtime can erase a cursor by redrawing
    the ring colour rather than by repairing artwork."""
    mask = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(mask)
    for quad in quads:
        d.polygon(expand_quad(quad, RING + 1.5), fill=255)
    for quad in quads:
        d.polygon(expand_quad(quad, 0.5), fill=0)
    img.paste(Image.new("RGB", img.size, RING_RGB), (0, 0), mask)
    return img


def paint_panels(img):
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
    d.rectangle([0, HAND_BAND_Y, WIDTH - 1, HAND_BAND_Y + HAND_BAND_H - 1],
                fill=(0, 0, 0))
    # A gold hairline under the hand row's slot pitch, so the five hand
    # positions read as positions before a card is in them.
    for i in range(HAND_SLOTS):
        x = HAND_X0 + i * HAND_PITCH
        d.rectangle([x - 1, HAND_Y - 1, x + CARD_W, HAND_Y + CARD_H],
                    outline=GOLD_RGB)
    return img


def build_view(cap, stage, tag):
    quads = cap.poses[tag]
    img = composite_arena(cap, tag, stage)
    draw_slot_rings(img, quads)
    paint_panels(img)
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
        # The turn strip is also the empty COM-side resting view at its last
        # pose.  Carry the slot rings through the strip so the destination can
        # be populated and the cursor can be erased without repairing a full
        # board image.  (The opening is deliberately ring-free until it lands
        # on TOP, where the normal view already contains the rings.)
        if move == "TURN":
            draw_slot_rings(img, cap.poses[tag])
        elif pose == poses - 1:
            # The shared opening lands exactly on TOP.  Carry its rings on the
            # last pose so the animation truly ends on the retained base frame
            # rather than flashing them in one presentation later.
            draw_slot_rings(img, cap.poses[tag])
        band = img.crop((0, BAND_Y, WIDTH, BAND_Y + BAND_H))
        frames.append(pad_segments(grb.quantize(band, (WIDTH, BAND_H))))
    return b"".join(frames)


def cut_slot_tiles(scene, quads):
    """The empty-slot rectangle of one arena at each slot, in the arena's own
    quantised bytes -- so putting a destroyed monster back is the picture, not
    something close to it."""
    blob = bytearray()
    boxes = []
    for quad in quads:
        x0, y0, x1, y1 = quad_box(quad)
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


def quad_box(quad):
    """The pixel box a quad's ring and card occupy, clamped to the band."""
    outer = expand_quad(quad, RING + 2.0)
    x0 = max(0, int(math.floor(min(p[0] for p in outer))))
    x1 = min(WIDTH, int(math.ceil(max(p[0] for p in outer))) + 1)
    y0 = max(BAND_Y, int(math.floor(min(p[1] for p in outer))))
    y1 = min(BAND_Y + BAND_H, int(math.ceil(max(p[1] for p in outer))) + 1)
    return x0, y0, x1, y1


# ── §8.4: the baked span programs ────────────────────────────────────────────

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
    """Every span program, at the stride the streamer can address it by."""
    blob = bytearray()
    offsets = []
    longest = 0
    for tag in tags:
        for quad in cap.poses[tag]:
            offsets.append(len(blob) // SPAN_STRIDE)
            prog, _rows = bake_span_program(quad, CARD_W, CARD_H)
            longest = max(longest, len(prog))
            if len(prog) > SPAN_STRIDE:
                raise SystemExit("gen_msx_views: a span program is %d bytes, "
                                 "past the %d stride" % (len(prog), SPAN_STRIDE))
            blob += prog + bytes(SPAN_STRIDE - len(prog))
    return bytes(blob), offsets, longest


# ── Driving it all ───────────────────────────────────────────────────────────

def bake(quiet=False, capture=True):
    """Everything this module owns, as blobs plus the numbers the header needs."""
    cap = run_capture(quiet) if capture else read_capture()
    view_tags = ("TOP", "COM")
    views = {tag: [] for tag in view_tags}
    slots = bytearray()
    moves = {name: bytearray() for name, _n in cap.moves}
    # Geometry does not change with the stage tint.  Keep one box list per
    # camera view; the slot blob below still has a stage/view copy because its
    # pixels do change with the captured stage image.
    boxes = {tag: None for tag in view_tags}
    for stage in range(BOARD_STAGES):
        for tag in view_tags:
            quads = cap.poses[tag]
            data = grb.quantize(build_view(cap, stage, tag), (WIDTH, HEIGHT))
            tiles, stage_boxes = cut_slot_tiles(data, quads)
            views[tag].append(pad_segments(data))
            slots += tiles
            if boxes[tag] is None:
                boxes[tag] = stage_boxes
        for name, poses in cap.moves:
            moves[name] += build_move_strip(cap, stage, name, poses)
        if not quiet:
            print("BOARD_%-8s %d views x %d bytes, %d slot tiles each"
                  % (STAGE_NAMES[stage], len(view_tags),
                     WIDTH * HEIGHT, FIELD_SLOTS))

    spans, span_off, span_max = bake_spans(cap, list(view_tags))
    if not quiet:
        print("SPANS    %d programs -> %d bytes (longest %d B)"
              % (len(span_off), len(spans), span_max))
        for name, poses in cap.moves:
            print("MOVE_%-8s %d poses x %d rows -> %d bytes"
                  % (name, poses, BAND_H, len(moves[name])))

    return {
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
        "span_max": span_max,
        "quads": {tag: cap.poses[tag] for tag in view_tags},
    }


def header_lines(baked, view_seg, slot_seg, move_segs, span_seg):
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
    a("#define MSX2_CARD_W             %d" % CARD_W)
    a("#define MSX2_CARD_H             %d" % CARD_H)
    a("#define MSX2_HAND_X0            %d" % HAND_X0)
    a("#define MSX2_HAND_PITCH         %d" % HAND_PITCH)
    a("#define MSX2_HAND_Y             %d" % HAND_Y)
    a("#define MSX2_FIELD_SLOTS        %d" % FIELD_SLOTS)
    a("#define MSX2_HAND_SLOTS         %d" % HAND_SLOTS)
    a("#define MSX2_RING_COLOR         0x%02X" % grb.pack(*RING_RGB))
    a("#define MSX2_PANEL_COLOR        0x%02X" % grb.pack(*PANEL_RGB))
    a("#define MSX2_GOLD_COLOR         0x%02X" % grb.pack(*GOLD_RGB))
    a("")
    a("// The projected corners of every field slot, window pixels, in the corner")
    a("// order the shared renderer hands its rasterizer -- so texture corner 0")
    a("// lands on the same physical corner here as it does on the PC.")
    a("static const unsigned char g_msx2_slot_quad[MSX2_BOARD_VIEWS][MSX2_FIELD_SLOTS][8] = {")
    for tag in baked["view_tags"]:
        a("\t{")
        for quad in baked["quads"][tag]:
            a("\t\t{ %s }," % ", ".join("%d" % max(0, min(255, int(round(v))))
                                           for p in quad for v in p))
        a("\t},")
    a("};")
    a("")
    a("// The box a slot's ring and card occupy: what an empty slot restores, and")
    a("// what a repaint has to cover.")
    a("static const unsigned char g_msx2_slot_box[MSX2_BOARD_VIEWS][MSX2_FIELD_SLOTS][4] = {")
    for tag in baked["view_tags"]:
        a("\t{")
        for box in baked["slot_boxes"][tag]:
            a("\t\t{ %d, %d, %d, %d }," % box)
        a("\t},")
    a("};")
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
    a("// Slot -> which SPAN_STRIDE-sized record in the blob holds its program.")
    a("static const unsigned char g_msx2_span_record[MSX2_BOARD_VIEWS][MSX2_FIELD_SLOTS] = {")
    for view in range(len(baked["view_tags"])):
        start = view * FIELD_SLOTS
        a("\t{ %s }," % ", ".join(str(v) for v in
                                      baked["span_offsets"][start:start + FIELD_SLOTS]))
    a("};")
    a("")
    a("// ── Empty-slot tiles ──────────────────────────────────────────────────")
    a("#define MSX2_SLOT_ART_SEGMENT   %d" % slot_seg)
    a("#define MSX2_SLOT_ART_STRIDE    %d" % SLOT_STRIDE)
    a("#define MSX2_SLOT_ART_PER_SEG   %d" % (16384 // SLOT_STRIDE))
    a("#define MSX2_SLOT_ART_PER_VIEW  %d" % FIELD_SLOTS)
    a("#define MSX2_SLOT_ART_VIEWS     %d" % len(baked["view_tags"]))
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


def main():
    quiet = "--quiet" in sys.argv
    os.makedirs(ASSET_DIR, exist_ok=True)
    baked = bake(quiet)
    total = sum(len(v) for v in baked["views"]) + len(baked["slots"]) + \
        len(baked["spans"]) + sum(len(b) for _n, _p, b in baked["moves"])
    print("board assets: %d bytes (%d KB)" % (total, total // 1024))


if __name__ == "__main__":
    main()
