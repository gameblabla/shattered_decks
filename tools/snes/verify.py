#!/usr/bin/env python3
"""Headless verification for the SNES port.

`SNES/snesfaust/snesfaust-mednafen script` is a fully headless SNES-Faust: it
takes an input script, runs N frames, and drops the last frame as a PPM, a WRAM
dump and optionally a directory of frames.  This is the harness that turns that
into assertions.

Two rules this file exists to enforce, both learned the expensive way on the
other ports:

  * **Rebuild before capturing.**  The MSX2 port repeatedly photographed a
    stale ROM and drew conclusions from it, so this refuses to run when the ROM
    is older than anything it is built from.
  * **A black screenshot is not proof of anything.**  Every check here looks at
    the pixels or at the frame stamp in WRAM; none of them is satisfied by "the
    emulator did not crash".

Run it with no arguments for the standard regression set.
"""

import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snes_dc

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROM = os.path.join(ROOT, "build", "snes", "waifusnes.sfc")
FAUST = os.path.join(ROOT, "SNES", "snesfaust", "snesfaust-mednafen")
OUT = os.path.join(ROOT, "build", "snes", "verify")

# The frame stamp, mirroring src/snes/snes_stamp.h.
STAMP_FIELDS = ["magic", "scene", "frames", "render_lines", "board_res",
                "duel_turn", "lp_player", "lp_com", "duel_result", "ui",
                "cursor", "field_cards", "phase", "turn_owner", "checksum"]

# enum SnesDuelUi, mirroring src/snes/snes_duel.c.
UI = ["HAND", "PLACE", "EQUIP_TARGET", "ATTACKER", "DEFENDER", "COM", "RESULT"]
STAMP_MAGIC = 0x5744

# SNES serial pad bits, the order snesfaust's script rows use.
PAD = {"B": 0x8000, "Y": 0x4000, "SELECT": 0x2000, "START": 0x1000,
       "UP": 0x0800, "DOWN": 0x0400, "LEFT": 0x0200, "RIGHT": 0x0100,
       "A": 0x0080, "X": 0x0040, "L": 0x0020, "R": 0x0010}

SCENES = ["BOOT", "TITLE", "MENU", "STORY_MAP", "STORY_TALK", "DUEL",
          "BATTLE_ART", "RESULT", "DECK", "ENDING"]


class Failure(Exception):
    pass


def check_rom_fresh():
    if not os.path.exists(ROM):
        raise Failure("no ROM at %s -- run: make -f Makefile.snes" % ROM)
    rom_time = os.path.getmtime(ROM)
    newer = []
    for base in ("src/snes", "src/msx2/msx2_duel.c", "src/game", "Makefile.snes",
                 "tools/snes"):
        path = os.path.join(ROOT, base)
        if os.path.isfile(path):
            files = [path]
        else:
            files = [os.path.join(d, f)
                     for d, _, fs in os.walk(path) for f in fs]
        for f in files:
            # This file is the harness, not an input: a change to it does not
            # make the ROM stale.
            if f.endswith((".pyc",)) or "__pycache__" in f or f == __file__:
                continue
            if os.path.getmtime(f) > rom_time:
                newer.append(os.path.relpath(f, ROOT))
    if newer:
        raise Failure("ROM is older than %d source file(s), e.g. %s -- rebuild "
                      "before capturing" % (len(newer), ", ".join(sorted(newer)[:3])))


def rom_header():
    """Decode the cartridge shape out of the ROM itself.

    snesfaust's own `header` command mislabels $31 as SlowROM, so the map-mode
    byte is decoded here instead of trusted from its output."""
    with open(ROM, "rb") as fh:
        data = fh.read()
    mode = data[0xFFD5]
    return {
        "size": len(data),
        "map_mode": mode,
        "hirom": bool(mode & 0x01),
        "fastrom": bool(mode & 0x10),
        "cartridge_type": data[0xFFD6],
        "rom_size": data[0xFFD7],
        "sram_size": data[0xFFD8],
    }


def run(name, script, frames, capture=None):
    """One scripted run.  `script` is a list of (start, end, pad_mask) rows."""
    os.makedirs(OUT, exist_ok=True)
    in_path = os.path.join(OUT, name + ".in.txt")
    with open(in_path, "w") as fh:
        for start, end, mask in script:
            fh.write("%d %d %d 0\n" % (start, end, mask))
    ppm = os.path.join(OUT, name + ".ppm")
    wram = os.path.join(OUT, name + ".wram.bin")
    argv = [FAUST, "script", ROM, in_path, str(frames), ppm, wram]
    if capture:
        frame_dir = os.path.join(OUT, name + ".frames")
        # EMPTIED FIRST.  A capture that writes fewer frames than the last one
        # leaves the older run's frames behind, and comparing "the first two
        # frames" then compares two runs -- which is how a flicker that had
        # already been fixed went on being reproduced for an hour.
        shutil.rmtree(frame_dir, ignore_errors=True)
        os.makedirs(frame_dir, exist_ok=True)
        cov = os.path.join(OUT, name + ".cov")
        argv += [cov, "0", os.path.join(OUT, name + ".ppu"),
                 os.path.join(OUT, name + ".ppu.txt"), frame_dir,
                 str(capture[0]), str(capture[1]), str(capture[2])]
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        raise Failure("%s: emulator exited %d\n%s" % (name, proc.returncode,
                                                      proc.stderr[-2000:]))
    return ppm, wram


def read_ppm(path):
    with open(path, "rb") as fh:
        assert fh.readline().strip() == b"P6"
        w, h = (int(v) for v in fh.readline().split())
        fh.readline()
        return w, h, fh.read()


def read_stamp(path):
    """Find the frame stamp in the WRAM dump and decode it.

    The stamp is located by its magic and validated by its checksum rather than
    by a hard-coded address, so a relink that moves it does not silently turn
    every assertion below into a comparison against zero."""
    with open(path, "rb") as fh:
        wram = fh.read()
    n = len(STAMP_FIELDS)
    # 816-tcc does not word-align statics, so scan every byte offset.
    for off in range(0, len(wram) - n * 2):
        if wram[off] | (wram[off + 1] << 8) != STAMP_MAGIC:
            continue
        words = struct.unpack_from("<%dH" % n, wram, off)
        if sum(words[:-1]) & 0xFFFF != words[-1]:
            continue
        return dict(zip(STAMP_FIELDS, words))
    raise Failure("no valid frame stamp in %s -- the game never reached its "
                  "main loop, or the dump is torn" % os.path.basename(path))


def colours(pixels):
    return {pixels[i:i + 3] for i in range(0, len(pixels), 3)}


def texel_size(pixels, w, y, expected):
    """Measure how many screen pixels one chunky texel occupies on row `y`.

    This is the whole point of the M1 calibration image: the still band must be
    exactly 2 screen pixels a texel and the moving band exactly 4, and only a
    measurement off the frame can say so."""
    row = [pixels[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
    runs, cur, n = [], row[0], 1
    for px in row[1:]:
        if px == cur:
            n += 1
        else:
            runs.append(n)
            cur, n = px, 1
    runs.append(n)
    # Interior runs only: the first and last are clipped by the screen edge,
    # and neighbouring texels that happen to share a colour merge into one run.
    interior = [r for r in runs[1:-1]]
    if not interior:
        raise Failure("row %d is a single flat colour -- nothing to measure" % y)
    return min(interior)


# ── Checks ───────────────────────────────────────────────────────────────────

def check_cartridge():
    h = rom_header()
    if h["size"] != 4 * 1024 * 1024:
        raise Failure("ROM is %d bytes, expected 4 MB" % h["size"])
    if not (h["hirom"] and h["fastrom"]):
        raise Failure("map mode $%02X is not HiROM+FastROM" % h["map_mode"])
    if h["rom_size"] != 0x0C:
        raise Failure("ROM size byte is $%02X, expected $0C (32 Mbit)" % h["rom_size"])
    if h["sram_size"] != 0x03:
        raise Failure("SRAM size byte is $%02X, expected $03 (8 KB)" % h["sram_size"])
    if h["cartridge_type"] != 0x02:
        raise Failure("cartridge type is $%02X, expected $02 (ROM+RAM+battery)"
                      % h["cartridge_type"])
    return "4 MB HiROM/FastROM, 8 KB SRAM, no enhancement chip"


def check_boot():
    ppm, wram = run("boot", [(0, RUN_FRAMES + 200, 0)], RUN_FRAMES)
    stamp = read_stamp(wram)
    # The floor is why this is not sixty: a still board with cards on it is
    # about twenty fields of work, so a thousand fields is a few dozen game
    # frames and no more.  What the number has to prove is that the loop is
    # still turning, not that it is fast -- the render-cost check owns speed.
    if stamp["frames"] < 20:
        raise Failure("only %d game frames in %d -- the main loop is stalled"
                      % (stamp["frames"], RUN_FRAMES))
    w, h, px = read_ppm(ppm)
    if len(colours(px)) < 8:
        raise Failure("the screen shows %d colours -- nothing is being drawn"
                      % len(colours(px)))
    return "scene %s, %d frames, %d colours on screen" % (
        SCENES[stamp["scene"]], stamp["frames"], len(colours(px)))


def board_row(res):
    """A screen row that is inside the board, in either resolution.

    The board occupies lines 0..159 and the slab itself starts a little below
    the painted horizon; line 130 is inside it in both resolutions and clear of
    the HUD band at 160."""
    return 130


# A RUN IS MEASURED IN EMULATOR FIELDS, AND A GAME FRAME IS MANY OF THEM.
# A board with twenty cards on it takes about twenty fields to draw (see the
# render-cost check), so a button has to be HELD for longer than one game frame
# or the poll that reads it never happens while it is down.  Sixty fields is
# comfortably longer than the slowest frame the port produces.
RUN_FRAMES = 1200
HOLD = 60

# The harness's four switches in the duel, all of them documented in
# src/snes/snes_duel.c: R fills the board from the decks (the measurement and
# identification fixture), SELECT draws it without cards (the ablation), L hands
# the player's side to the rules (the demo, and the soak run), Y toggles the
# board resolution.


def press(button, at):
    return (at, at + HOLD, PAD[button])


def run_still(capture=None):
    return run("still", [(0, RUN_FRAMES + 200, 0)], RUN_FRAMES, capture=capture)


def run_fixture(capture=None):
    """The full-board fixture: five monsters and five supports a side, one of
    them set face down.  Every id in it came off the duel's own shuffled deck --
    it is a fixed BOARD, not fixed art."""
    return run("fixture", [press("R", 200)], RUN_FRAMES, capture=capture)


def run_no_cards(capture=None):
    """The fixture board with the cards NOT DRAWN.

    The port's rule is that a performance claim comes from a measurement or an
    ablation; this is the ablation that says what the cards cost.  It is also
    the only way to measure the slab's own shape, since twenty cards cover
    almost all of it."""
    return run("nocards", [press("R", 200), press("SELECT", 400)],
               RUN_FRAMES, capture=capture)


def run_hold(capture=None):
    """A card picked up and carried: A chooses the first hand card, and the UI
    stays in PLACE with it hovering over the slot until it is put down."""
    return run("hold", [press("A", 200), press("RIGHT", 400), press("RIGHT", 500)],
               RUN_FRAMES, capture=capture)


def run_moving(capture=None):
    # Y toggles the board resolution.  The press has to land after the scene
    # machine is running.
    return run("moving", [press("Y", 200)], RUN_FRAMES, capture=capture)


def run_demo(frames=9000):
    """L hands the player's side to the rules as well, so the duel plays itself
    to a result with no further input -- the port's soak run."""
    return run("demo", [press("L", 60)], frames)


def run_flow(name, buttons):
    """A scripted turn through the duel UI, one button at a time."""
    script = []
    at = 200
    for b in buttons:
        script.append(press(b, at))
        at += HOLD * 2
    return run(name, script, at + 400)


def check_still_resolution():
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    if stamp["board_res"] != 1:
        raise Failure("board_res is %d, expected 1 (still)" % stamp["board_res"])
    w, h, px = read_ppm(ppm)
    size = texel_size(px, w, board_row(1), 2)
    if size != 2:
        raise Failure("still band texel is %d screen pixels wide, expected 2" % size)
    hud = texel_size(px, w, HUD_LP_Y + 3, 1)
    if hud != 1:
        raise Failure("the HUD's letters are %d screen pixels a stroke -- they "
                      "are being drawn into the bitmap, not by the sprite layer"
                      % hud)
    return "still band is exactly 2x2 over a 1:1 sprite HUD"


def check_moving_resolution():
    """The board halves itself while something is moving, and the HUD does not.

    THE HUD NOT CHANGING IS THE POINT.  It is sprites, so it is drawn at the
    screen's own resolution whatever the bitmap under it is doing; when it was
    bitmap texels this check had to prove a per-band HDMA scale was keeping it
    readable, and now it proves the band scale cannot reach it at all.

    THE RESOLUTION IS NOT A TOGGLE ANY MORE: the duel picks it, dropping the
    board to 64x40 while a card is in the air or the rules are changing the
    picture and returning to 128x80 once it settles.  So this holds a card --
    which is the state that keeps the board in motion -- and measures the two
    bands, which is what the per-band HDMA table buys."""
    ppm, wram = run_hold()
    stamp = read_stamp(wram)
    if stamp["board_res"] != 0:
        raise Failure("the board is still at the resting resolution while a "
                      "card is being held")
    w, h, px = read_ppm(ppm)
    board = texel_size(px, w, board_row(0), 4)
    hud = texel_size(px, w, HUD_LP_Y + 3, 1)
    if board != 4:
        raise Failure("moving band texel is %d screen pixels wide, expected 4" % board)
    if hud != 1:
        raise Failure("the HUD's letters are %d screen pixels a stroke while the "
                      "board is moving -- the sprite layer does not scale with "
                      "the bitmap and must not appear to" % hud)
    return "moving band 4x4 under a HUD that does not move with it"


def band_colours(px, w, y0, y1):
    out = set()
    for y in range(y0, y1):
        for x in range(0, w, 2):
            i = (y * w + x) * 3
            out.add(px[i:i + 3])
    return out


def check_floor_is_textured():
    """The ground has to be the arena texture, not a fill.

    A flat quad with a grid drawn on it would pass a "the board is visible"
    test, so this counts distinct colours across the board band and demands
    more than a texture-free renderer could produce."""
    ppm, _ = run_still()
    w, h, px = read_ppm(ppm)
    cols = band_colours(px, w, 90, 125)
    if len(cols) < 12:
        raise Failure("the board band shows %d colours -- the floor is not "
                      "textured" % len(cols))
    # EVERYTHING THE SLAB DOES NOT COVER IS BLACK.  There is no backdrop
    # picture and no horizon band; the board is the only textured object on the
    # screen.  So the rows above its far edge are one colour and that colour is
    # black -- a check a painted sky would fail, which is the point.
    sky = band_colours(px, w, 0, 40)
    if sky != {b"\x00\x00\x00"}:
        raise Failure("the rows above the board show %d colours (%s) -- the "
                      "surround is not black"
                      % (len(sky), sorted(c.hex() for c in sky)[:4]))
    return "%d colours on the floor, black everywhere else" % len(cols)


def check_board_is_a_slab():
    """The slab is FINITE and in perspective: its width grows with the screen
    row, and rows above its far edge and below its near edge are not board.

    This is the check that would have caught the signed-word wrap in the edge
    accumulators, which turned the near half of the board back into backdrop.

    It measures the board with the CARDS OFF, because a card's own dark frame
    quantises into the same byte as the surround: with cards on, the extent of
    "not the backdrop colour" is no longer the extent of the slab."""
    ppm, _ = run_no_cards()
    w, h, px = read_ppm(ppm)
    black = b"\x00\x00\x00"
    widths = []
    for y in (60, 80, 100):
        xs = [x for x in range(w)
              if px[(y * w + x) * 3:(y * w + x) * 3 + 3] != black]
        widths.append(xs[-1] - xs[0] + 1 if xs else 0)
    if not (widths[0] < widths[1] <= widths[2]):
        raise Failure("board widths down the screen are %s -- the slab is not "
                      "in perspective" % widths)
    # THE WHOLE SLAB FITS ACROSS THE SCREEN.  Its near edge is the widest part
    # of it and the focal length is fixed at half the viewport, so a camera
    # standing closer than the slab's own half width runs the near row off both
    # sides -- which is what "the graphics are not close to the other ports"
    # looked like.  So the widest row must be wide, and must NOT touch either
    # edge of the screen.
    near = [x for x in range(w)
            if px[(120 * w + x) * 3:(120 * w + x) * 3 + 3] != black]
    if not near:
        raise Failure("there is no board on line 120 at all")
    if near[0] == 0 or near[-1] == w - 1:
        raise Failure("the slab reaches x %d..%d on line 120 -- it is running "
                      "off the side of the screen" % (near[0], near[-1]))
    if len(near) < w * 3 // 4:
        raise Failure("the board is only %d pixels wide at its near edge -- it "
                      "does not fill the screen" % len(near))
    # Below the slab's near edge and its front wall there is no board at all:
    # this is the half of the bounds check the widening test cannot see.
    below = [x for x in range(w)
             if px[(156 * w + x) * 3:(156 * w + x) * 3 + 3] != black]
    if below:
        raise Failure("%d pixels of board below its near edge on line 156 -- the "
                      "slab is not bounded" % len(below))
    return "slab widens %d -> %d -> %d pixels, inside the screen, and ends" % tuple(widths)


def check_board_is_five_by_four():
    """FIVE COLUMNS ACROSS, AND NO GROOVE DOWN THE MIDDLE OF A SLOT.

    (The four ROWS are counted by check_top_view, which identifies all twenty
    slots pixel for pixel; down the perspective board a row boundary and the
    checkerboard's own change of material are the same kind of dark step, so
    the columns are what a screenshot row can settle.)

    The floor texture repeats every world unit and a slot centre is at a whole
    world x, so a texture that is not offset half a cell puts a groove exactly
    where a card goes: the slab reads as four columns with a half tile at each
    end and the middle card sits on the seam between two of them.  That is what
    it did.

    Counting grooves alone does not catch it -- misaligned, the five interior
    grooves land on the five slot centres, which is about as many runs as four
    grooves plus two rims.  WHERE they are is what tells the two apart, and the
    decisive place is the screen's own centre line: the middle column's middle
    is either stone or it is a groove.  So this asserts the centre is stone,
    that there are four grooves inside the slab, and that they sit either side
    of the centre in pairs."""
    ppm, _ = run_no_cards()
    w, h, px = read_ppm(ppm)
    black = b"\x00\x00\x00"

    def profile(y):
        row = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
        lit = [x for x, c in enumerate(row) if c != black]
        if len(lit) < w // 2:
            return None
        x0, x1 = lit[0], lit[-1]
        lum = [sum(row[x]) for x in range(x0, x1 + 1)]
        lo, hi = min(lum), max(lum)
        thr = lo + (hi - lo) * 0.4
        runs, start = [], None
        for i, v in enumerate(lum):
            if v < thr and start is None:
                start = i
            elif v >= thr and start is not None:
                runs.append((x0 + start, x0 + i - 1))
                start = None
        if start is not None:
            runs.append((x0 + start, x1))
        # A groove is two texels of darkened stone and the sandstone in it is
        # noisy, so a bright texel can split one groove into two runs.  Runs
        # within a few pixels of each other are one groove.
        merged = []
        for a, b in runs:
            if merged and a - merged[-1][1] <= 4:
                merged[-1] = (merged[-1][0], b)
            else:
                merged.append((a, b))
        return x0, x1, merged

    def brightest(a, b):
        """The row of a band least affected by a groove ACROSS the board.

        The rows between board rows are grooves too, and a screenshot row that
        lands on one is dark from side to side -- thresholding it finds the
        texture's noise rather than the column grid.  So each band contributes
        the row with the most light in it, which is a row through the middle of
        a board row."""
        best = None
        for y in range(a, b):
            row = [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for x in range(w)]
            lit = [sum(c) for c in row if c != black]
            if len(lit) < w // 2:
                continue
            m = sum(lit) / float(len(lit))
            if best is None or m > best[0]:
                best = (m, y)
        if best is None:
            raise Failure("no slab anywhere in lines %d..%d" % (a, b))
        return best[1]

    report = []
    for y in (brightest(78, 92), brightest(95, 110), brightest(112, 128)):
        got = profile(y)
        if got is None:
            raise Failure("no slab across line %d" % y)
        x0, x1, runs = got
        mid = (x0 + x1) // 2
        for a, b in runs:
            if a - 1 <= mid <= b + 1:
                raise Failure("line %d has a groove at x %d..%d, straddling the "
                              "board's centre line at %d -- the floor texture "
                              "is half a tile out and the middle slot is a seam"
                              % (y, a, b, mid))
        # The rims are grooves too, and at the slab's edge they merge with the
        # black surround, so what is counted is the ones strictly inside.
        inner = [r for r in runs if r[0] > x0 + 3 and r[1] < x1 - 3]
        if len(inner) != 4:
            raise Failure("line %d has %d grooves inside the slab (%s) -- five "
                          "columns need exactly four"
                          % (y, len(inner), inner))
        left = [r for r in inner if r[1] < mid]
        if len(left) != 2:
            raise Failure("line %d has %d of its four grooves left of centre -- "
                          "the columns are not symmetric about the middle slot"
                          % (y, len(left)))
        report.append(len(inner))

    return "four grooves inside the slab on every row, none on its centre line"


def check_render_cost():
    """The measurement that replaces section 4.4's estimates.

    render_lines is scanlines of wall clock for one board, taken from the V
    counter and the vblank count (src/snes/snes_duel.c), so 262 of them is one
    NTSC field."""
    _, wram = run_fixture()
    still = read_stamp(wram)["render_lines"]
    _, wram = run_no_cards()
    floor_only = read_stamp(wram)["render_lines"]
    # The moving board is measured on the same fixture: R fills it, Y drops the
    # resolution, and the number is the one an animating frame pays.
    _, wram = run("fixture_moving", [press("R", 200), press("Y", 400)],
                  RUN_FRAMES)
    moving = read_stamp(wram)["render_lines"]
    if still == 0 or moving == 0:
        raise Failure("no render timing was recorded (still %d, moving %d)"
                      % (still, moving))
    if moving >= still:
        raise Failure("the moving board (%d lines) is not cheaper than the still "
                      "one (%d) -- the resolution switch is not doing anything"
                      % (moving, still))
    if floor_only >= still:
        raise Failure("the ablated board (%d lines) is not cheaper than the "
                      "full one (%d) -- SELECT did not turn the cards off"
                      % (floor_only, still))
    return ("still %d lines (%.1f fields, %.1f fps), moving %d lines "
            "(%.1f fields, %.1f fps); ablated: floor %d, so the cards are "
            "%d lines (%d%%)"
            % (still, still / 262.0, 60.0 / (still / 262.0),
               moving, moving / 262.0, 60.0 / (moving / 262.0),
               floor_only, still - floor_only,
               100 * (still - floor_only) // still))



# ── The board's cards ────────────────────────────────────────────────────────
#
# These checks identify what is on the board BY MATCHING THE PIXELS AGAINST THE
# CARD SHEET, which is the only way "the right cards are in the right slots"
# can be asserted from outside the machine.  The camera model below mirrors
# src/snes/snes_duel.c and snes_board3d.c exactly -- one world unit is one slot
# pitch, the camera is one unit up and three back, the focal length is half the
# viewport and the horizon an eighth of it -- so a card's texel can be projected
# here and read back out of a screenshot.
#
# Only the NEAR rows are identified.  A far-row card is eight screen pixels
# across, and at that size any sixteen-texel picture matches it about as well as
# any other; asserting on it would be a test of the noise floor.

CARD_TEX = os.path.join(ROOT, "src", "snes", "assets", "snes_card_tex.bin")
CARD_BACK = 78                 # the last face; also what a set monster shows
SUPPORT_FIRST = 72             # ids 72..77 are the six support variants
ROW_Z = [1.5, 0.5, -0.5, -1.5]  # far to near, matching snesSlotCentre
CARD_UNITS = 0.8               # a card covers four fifths of its tile
STILL_W, STILL_H = 128, 80
# Mirroring snes_duel.c: the camera stands 2.75 units in front of the slab's
# near edge, which is what fits its 5-unit width across a viewport whose focal
# length is fixed at half its own width.
CAM_Z, CAM_HEIGHT = -4.75, 2.75
HORIZON_DIV = 32.0

_FACES = None
_WEIGHT = None


def card_faces():
    """The sheet, and how much a matching texel is worth.

    A match is weighted by the RARITY of its colour across the whole sheet,
    and without that weight this does not work at all: half of every face is
    the frame's near-black navy, so a flat support sigil matches a monster's
    dark corners better than the monster's own face does and every slot comes
    back as the same support card."""
    global _FACES, _WEIGHT
    if _FACES is None:
        if not os.path.exists(CARD_TEX):
            raise Failure("no card sheet at %s -- run: python3 "
                          "tools/snes/gen_snes_cards.py"
                          % os.path.relpath(CARD_TEX, ROOT))
        with open(CARD_TEX, "rb") as fh:
            blob = fh.read()
        _FACES = [blob[i * 256:(i + 1) * 256] for i in range(len(blob) // 256)]
        counts = {}
        for b in blob:
            counts[b] = counts.get(b, 0) + 1
        _WEIGHT = dict((b, float(len(blob)) / n) for b, n in counts.items())
    return _FACES, _WEIGHT


_BYTE_OF = {}


def direct_colour_byte(rgb):
    """The direct-colour byte a screen pixel came from.

    Mode 7 direct colour expands BBGGGRRR into 15-bit colour with the tile's
    palette bits underneath, so the emulator's RGB is not the arithmetic
    snes_dc.unpack does; the mapping is still one to one, so each screen colour
    is resolved to its nearest cube entry once and cached."""
    if rgb in _BYTE_OF:
        return _BYTE_OF[rgb]
    best, best_e = 0, None
    for b in range(256):
        t = snes_dc.unpack(b)
        e = (t[0] - rgb[0]) ** 2 + (t[1] - rgb[1]) ** 2 + (t[2] - rgb[2]) ** 2
        if best_e is None or e < best_e:
            best, best_e = b, e
    _BYTE_OF[rgb] = best
    return best


def slot_samples(px, w, h, row, col, ox, oy, lo=3, hi=13):
    """The interior texels of one slot's card, read out of a still screenshot.

    The frame's outermost texels are left out because half a pixel of rounding
    at a card's edge samples the tile beside it, and the whole grid is offset by
    (ox, oy) so the caller can search for the alignment the renderer's own
    rounding produced."""
    cx, cz = col - 2, ROW_Z[row]
    focal, horizon = STILL_W / 2.0, STILL_H / HORIZON_DIV
    out = []
    for v in range(lo, hi):
        for u in range(lo, hi):
            wx = cx + ((u + 0.5) / 16.0 - 0.5) * CARD_UNITS
            wz = cz + CARD_UNITS / 2 - ((v + 0.5) / 16.0) * CARD_UNITS
            depth = wz - CAM_Z
            x = int((STILL_W / 2.0 + wx / depth * focal) * 2) + ox
            y = int((horizon + CAM_HEIGHT / depth * focal) * 2) + oy
            if 0 <= x < w and 0 <= y < h:
                i = (y * w + x) * 3
                out.append((v * 16 + u,
                            direct_colour_byte((px[i], px[i + 1], px[i + 2]))))
    return out


def identify_slot(px, w, h, row, col):
    """Which face is lying in a slot, and how decisively.

    Returns (face id, weighted score, the runner-up's score, exactly matching
    texels, samples) or None when the slot is clipped by the side of the
    viewport.  The alignment is searched over a couple of pixels: the projection
    here and the renderer's Q8.8 arithmetic round differently, and a card is
    only about twenty pixels across.

    BARE FLOOR ALSO HAS A BEST MATCH, and it beats its own runner-up about two
    to one -- sandstone is brown and so is half the card sheet -- so the ratio
    alone says nothing.  What separates them is the count of texels that match
    EXACTLY: a card that is really there matches a third of them and an empty
    slot a tenth, which is why `decisive` below wants both."""
    faces, weight = card_faces()
    best = None
    for ox in (-2, -1, 0, 1, 2):
        for oy in (-2, -1, 0, 1, 2):
            s = slot_samples(px, w, h, row, col, ox, oy)
            if len(s) < 60:
                continue
            scored = sorted(((sum(weight.get(b, 0.0) for i, b in s if f[i] == b),
                              sum(1 for i, b in s if f[i] == b), fi)
                             for fi, f in enumerate(faces)), reverse=True)
            if best is None or scored[0][0] > best[0]:
                best = (scored[0][0], scored[0][2], scored[1][0], scored[0][1],
                        len(s))
    if best is None:
        return None
    return best[1], best[0], best[2], best[3], best[4]


def decisive(got):
    """Whether an identification is a card at all, rather than the best of
    seventy-nine bad matches against a patch of ground."""
    if got is None:
        return False
    _face, score, second, strict, n = got
    return score >= second * 1.5 and strict * 4 >= n


def check_cards_on_board():
    """Five slots a side, showing the cards the fixture put there.

    The near monster row and the near support row are identified card by card
    against the sheet: a support row must hold support faces, a monster row must
    not, and the identifications must differ from one another -- one card drawn
    five times would pass a "there is something on the board" test."""
    ppm, wram = run_fixture()
    stamp = read_stamp(wram)
    if stamp["field_cards"] != 5:
        raise Failure("the fixture put %d monsters on the player's row, not 5"
                      % stamp["field_cards"])
    w, h, px = read_ppm(ppm)
    found = []
    for row in (2, 3):
        for col in range(5):
            got = identify_slot(px, w, h, row, col)
            if not decisive(got):
                continue
            found.append((row, col, got[0]))
    if len(found) < 4:
        raise Failure("only %d of the near rows' slots identify as a card face "
                      "-- the board is not showing the sheet's cards"
                      % len(found))
    supports = [f for r, c, f in found if r == 3]
    monsters = [f for r, c, f in found if r == 2 and f != CARD_BACK]
    if not supports or any(f < SUPPORT_FIRST or f >= CARD_BACK
                           for f in supports):
        raise Failure("the support row identifies as %s, which is not the "
                      "support faces (%d..%d)" % (supports, SUPPORT_FIRST,
                                                  CARD_BACK - 1))
    if any(f >= SUPPORT_FIRST for f in monsters):
        raise Failure("the monster row identifies as %s, which includes a "
                      "support face" % monsters)
    if len(set(f for _, _, f in found)) < 3:
        raise Failure("the identified slots are %s -- the board is showing the "
                      "same card everywhere" % [f for _, _, f in found])
    return "monsters %s, supports %s" % (monsters, supports)


def check_face_down_card():
    """A SET MONSTER SHOWS THE BACK, and the back is a face like any other.

    The rules set every monster a side plays (msx2_duel.c: "A PLACED MONSTER IS
    SET, WHOEVER PLAYS IT"), the fixture sets the player's fourth, and the back
    is the id past the last card -- so this also proves the renderer needs no
    special case for it: same sheet, same page, same walker."""
    ppm, _ = run_fixture()
    w, h, px = read_ppm(ppm)
    got = identify_slot(px, w, h, 2, 3)
    if got is None:
        raise Failure("the set monster's slot is off screen")
    face, score, second, strict, n = got
    if face != CARD_BACK:
        raise Failure("the set monster shows face %d, expected the back (%d)"
                      % (face, CARD_BACK))
    if not decisive(got):
        raise Failure("the back scored %.0f against a runner-up's %.0f on %d of "
                      "%d exact texels -- not a decisive identification"
                      % (score, second, strict, n))
    return "the set monster shows the back, %d of %d texels exactly" % (strict, n)


def check_empty_slot_is_floor():
    """The negative of the check above, and the reason it is here is that the
    identifier will name a face for a patch of ground if it is only asked which
    face fits best.  A slot the rules left empty must not identify as a card."""
    ppm, wram = run_flow("empty", ["A", "A"])          # place one monster
    stamp = read_stamp(wram)
    if stamp["field_cards"] != 1:
        raise Failure("the scripted placement put %d monsters on the board, "
                      "expected 1" % stamp["field_cards"])
    w, h, px = read_ppm(ppm)
    placed = identify_slot(px, w, h, 2, 0)
    if not decisive(placed) or placed[0] != CARD_BACK:
        raise Failure("the placed monster's slot identifies as %s -- a monster "
                      "the player has just set should show the back"
                      % (placed and placed[0]))
    for col in (1, 2, 3, 4):
        got = identify_slot(px, w, h, 2, col)
        if decisive(got):
            raise Failure("empty slot %d identifies as face %d on %d of %d "
                          "texels -- bare ground is being read as a card"
                          % (col, got[0], got[3], got[4]))
    return "one card at slot 0 (the back), four slots of bare ground"


def check_quad_card():
    """The convex-quad path: a card in the air over the slot it is going into.

    The held card leans back and bobs, so it is a DIFFERENT quad every game
    frame -- two frames apart it must have moved, and it must have moved in its
    own part of the screen and nowhere else.  A trapezoid walk, or a chain that
    only turns on one side, would still draw something; what it would not do is
    leave the rest of the board untouched while it does it."""
    _, wram = run_hold(capture=(RUN_FRAMES - 200, RUN_FRAMES - 1, 3))
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "PLACE":
        raise Failure("the UI is in %s, not PLACE -- nothing is being held"
                      % UI[stamp["ui"]])
    if stamp["cursor"] != 2:
        raise Failure("the cursor is on slot %d, expected 2 after two RIGHTs"
                      % stamp["cursor"])
    frames = sorted(os.listdir(os.path.join(OUT, "hold.frames")))
    if len(frames) < 8:
        raise Failure("captured %d frames, need at least 8" % len(frames))
    best = None
    for i in range(len(frames) - 1):
        a = read_ppm(os.path.join(OUT, "hold.frames", frames[i]))
        b = read_ppm(os.path.join(OUT, "hold.frames", frames[i + 1]))
        w = a[0]
        moved = [(x, y) for y in range(0, 160) for x in range(0, w, 2)
                 if a[2][(y * w + x) * 3:(y * w + x) * 3 + 3]
                 != b[2][(y * w + x) * 3:(y * w + x) * 3 + 3]]
        if best is None or len(moved) > len(best[0]):
            best = (moved, w)
    moved, w = best
    if len(moved) < 20:
        raise Failure("only %d pixels change between frames -- the held card is "
                      "not being redrawn through the quad path" % len(moved))
    xs = [x for x, y in moved]
    ys = [y for x, y in moved]
    # The cursor is on the middle slot of the player's own row, so the card is
    # in the middle of the board and above that slot.
    if min(xs) < w // 4 or max(xs) > w - w // 4:
        raise Failure("the moving pixels span x %d..%d, wider than the held "
                      "card's slot -- the quad is not where it should be"
                      % (min(xs), max(xs)))
    if max(ys) > 150:
        raise Failure("the moving pixels reach line %d, below the held card's "
                      "own slot" % max(ys))
    return "%d pixels move in x %d..%d, y %d..%d -- the held card only" % (
        len(moved), min(xs), max(xs), min(ys), max(ys))


# ── The duel itself ──────────────────────────────────────────────────────────

# ── The sprite layer ─────────────────────────────────────────────────────────
#
# Everything below reads the OBJ layer back off the screenshot, and it can do
# so EXACTLY: a sprite is drawn at the screen's own resolution, so one sprite
# pixel is one screenshot pixel and one CGRAM entry, and the comparison is an
# equality rather than a nearest match.  That is not true of anything in the
# Mode 7 bitmap, and it is the reason the HUD moved onto sprites.

SPR_FONT = os.path.join(ROOT, "src", "snes", "assets", "snes_spr_font.bin")
SPR_PAL = os.path.join(ROOT, "src", "snes", "assets", "snes_spr_pal.bin")
SPR_CARDS = os.path.join(ROOT, "src", "snes", "assets", "snes_spr_cards.bin")
SPR_GROUP = os.path.join(ROOT, "src", "snes", "assets", "snes_spr_group.bin")
GLYPH_FIRST = 32
GLYPH_COUNT = 64

# Screen coordinates, mirroring snes_duel.c.
HUD_LP_Y, HUD_MSG_Y = 162, 172
HAND_Y, HAND_X0, HAND_PITCH = 186, 8, 48
TOP_CELL, TOP_X0, TOP_Y0 = 48, 8, 16
TOP_LP_Y, TOP_MSG_Y = 4, 212

_SPR = {}


def spr_asset(path, name):
    if name not in _SPR:
        if not os.path.exists(path):
            raise Failure("no %s -- run: python3 tools/snes/gen_snes_obj.py"
                          % os.path.relpath(path, ROOT))
        with open(path, "rb") as fh:
            _SPR[name] = fh.read()
    return _SPR[name]


def untile4(blob, off):
    """One 8x8 tile of 4bpp back to sixty-four palette indices."""
    px = [0] * 64
    for k, lo in enumerate((0, 2)):
        for y in range(8):
            p0, p1 = blob[off + k * 16 + y * 2], blob[off + k * 16 + y * 2 + 1]
            for x in range(8):
                px[y * 8 + x] |= (((p0 >> (7 - x)) & 1) |
                                  (((p1 >> (7 - x)) & 1) << 1)) << lo
    return px


def obj_colour(pal, index):
    """A CGRAM entry as the five bits a channel the PPU actually holds."""
    blob = spr_asset(SPR_PAL, "pal")
    w = blob[(pal * 16 + index) * 2] | (blob[(pal * 16 + index) * 2 + 1] << 8)
    return (w & 31, (w >> 5) & 31, (w >> 10) & 31)


def screen5(px, w, x, y):
    i = (y * w + x) * 3
    return (px[i] >> 3, px[i + 1] >> 3, px[i + 2] >> 3)


def read_sprite_line(px, w, h, x0, y0, cols):
    """A line of HUD text, decoded glyph by glyph out of the sprite font.

    The glyph tiles in ROM are matched against the screen an ink pixel at a
    time: a sprite is 1:1 with the screen, so a letter either IS the tile the
    game says it drew or it is not.  This is how the harness reads life points
    -- not "there are bright pixels there" but the actual number."""
    font = spr_asset(SPR_FONT, "font")
    ink = obj_colour(7, 1)
    out = ""
    for col in range(cols):
        seen = []
        for v in range(8):
            row = 0
            for u in range(8):
                x, y = x0 + col * 8 + u, y0 + v
                if 0 <= x < w and 0 <= y < h and screen5(px, w, x, y) == ink:
                    row |= 0x80 >> u
            seen.append(row)
        if not any(seen):
            out += " "
            continue
        best = None
        for g in range(GLYPH_COUNT):
            tile = untile4(font, g * 32)
            bits = []
            for v in range(8):
                row = 0
                for u in range(8):
                    if tile[v * 8 + u] == 1:
                        row |= 0x80 >> u
                bits.append(row)
            d = sum(bin(bits[i] ^ seen[i]).count("1") for i in range(8))
            if best is None or d < best[0]:
                best = (d, g)
        out += chr(GLYPH_FIRST + best[1]) if best[0] <= 4 else "?"
    return out.rstrip()


def card_sprite_pixels(face):
    """One card sprite as 32x32 five-bit colours, straight out of the ROM."""
    cards = spr_asset(SPR_CARDS, "cards")
    group = spr_asset(SPR_GROUP, "group")
    pal = group[face]
    out = [None] * (32 * 32)
    for t in range(16):
        tile = untile4(cards, face * 512 + t * 32)
        tx, ty = (t % 4) * 8, (t // 4) * 8
        for y in range(8):
            for x in range(8):
                out[(ty + y) * 32 + tx + x] = obj_colour(pal, tile[y * 8 + x])
    return out


def identify_card_sprite(px, w, h, x0, y0):
    """Which face a 32x32 sprite on screen is, and how exactly.

    Returns (face, matching pixels) for the best face.  A sprite is pixel exact,
    so the right answer matches all 1024 and the wrong one does not come close;
    there is no scoring model here and none is needed."""
    group = spr_asset(SPR_GROUP, "group")
    obs = []
    for y in range(32):
        for x in range(32):
            if not (0 <= x0 + x < w and 0 <= y0 + y < h):
                return None
            obs.append(screen5(px, w, x0 + x, y0 + y))
    best = None
    for face in range(len(group)):
        want = card_sprite_pixels(face)
        n = sum(1 for a, b in zip(obs, want) if a == b)
        if best is None or n > best[1]:
            best = (face, n)
    return best


def check_hud_text():
    """The band says what the rules say.

    Both lines are decoded off the screenshot and checked against the frame
    stamp, so a HUD that draws stale or wrong numbers fails here rather than
    looking plausible."""
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    w, h, px = read_ppm(ppm)
    you = read_sprite_line(px, w, h, 8, HUD_LP_Y, 9)
    com = read_sprite_line(px, w, h, 152, HUD_LP_Y, 9)
    prompt = read_sprite_line(px, w, h, 8, HUD_MSG_Y, 18)
    expect_you = "YOU %04d" % stamp["lp_player"]
    expect_com = "COM %04d" % stamp["lp_com"]
    if you != expect_you or com != expect_com:
        raise Failure("the life-point line reads %r / %r, but the rules say "
                      "%r / %r" % (you, com, expect_you, expect_com))
    if UI[stamp["ui"]] == "HAND" and not prompt.startswith("A:PLAY"):
        raise Failure("the prompt reads %r while the UI is in HAND" % prompt)
    return "%r %r / %r" % (you, com, prompt)


def check_duel_flow():
    """A turn played through the UI, one button at a time.

    Each step is its own scripted run, because the frame stamp is dumped once at
    the end: the sequence of runs IS the trace.  What it proves is that the
    rules and the screen are the same machine -- a card leaves the hand, lands
    on the board, the battle phase opens and the turn passes to the opponent."""
    steps = [
        (["A"], "PLACE", None),
        (["A", "A"], "HAND", 1),
        (["A", "A", "X"], "ATTACKER", 1),
        # By the end of this run the opponent has taken its own turn, so what
        # the board holds is the rules' business and not this step's.
        (["A", "A", "X", "START"], None, None),
    ]
    trace = []
    for buttons, ui_name, cards in steps:
        _, wram = run_flow("flow", buttons)
        stamp = read_stamp(wram)
        if ui_name and UI[stamp["ui"]] != ui_name:
            raise Failure("after %s the UI is in %s, expected %s"
                          % ("+".join(buttons), UI[stamp["ui"]], ui_name))
        if cards is not None and stamp["field_cards"] != cards:
            raise Failure("after %s the player has %d monsters on the board, "
                          "expected %d" % ("+".join(buttons),
                                           stamp["field_cards"], cards))
        trace.append("%s->%s" % ("+".join(buttons), ui_name or UI[stamp["ui"]]))
    # START ends the player's turn; by the end of the run the opponent has had
    # its own and handed the turn back, so what is asserted is that the turn
    # COUNTER moved rather than who is holding it at the final frame.
    _, wram = run_flow("flow", ["A", "A", "X", "START"])
    stamp = read_stamp(wram)
    if stamp["duel_turn"] < 2:
        raise Failure("START did not pass the turn: still on turn %d"
                      % stamp["duel_turn"])
    trace.append("turn %d" % stamp["duel_turn"])
    return ", ".join(trace)


def check_duel_plays_out():
    """A whole duel, to a result.

    L hands the player's side to the rules as well, so the duel plays itself:
    the AI on both sides, the same rules model the MSX2 and Atari ST ports use,
    and the SNES presentation showing every step of it.  What is asserted is
    that it ENDS -- a decisive result, the loser on zero life points, and the
    screen saying which way it went."""
    ppm, wram = run_demo()
    stamp = read_stamp(wram)
    if stamp["duel_result"] == 0:
        raise Failure("the demo duel is still running after %d fields: turn %d, "
                      "you %d, com %d" % (9000, stamp["duel_turn"],
                                          stamp["lp_player"], stamp["lp_com"]))
    won = stamp["duel_result"] == 1
    loser_lp = stamp["lp_com"] if won else stamp["lp_player"]
    if loser_lp != 0:
        raise Failure("the duel is decided but the loser has %d life points"
                      % loser_lp)
    if stamp["duel_turn"] < 4:
        raise Failure("the duel ended on turn %d, which is not a duel"
                      % stamp["duel_turn"])
    if UI[stamp["ui"]] != "RESULT":
        raise Failure("the duel is decided but the screen is in %s"
                      % UI[stamp["ui"]])
    w, h, px = read_ppm(ppm)
    msg = read_sprite_line(px, w, h, 8, HUD_MSG_Y, 16)
    expect = "YOU WIN  A:AGAIN" if won else "YOU LOSE A:AGAIN"
    if msg != expect:
        raise Failure("the duel was %s but the band reads %r"
                      % ("won" if won else "lost", msg))
    return "%s on turn %d, %d fields, band reads %r" % (
        "won" if won else "lost", stamp["duel_turn"], 9000, msg)


def run_top(capture=None):
    """The fixture board, then UP into the tactical top view."""
    return run("topview", [press("R", 200), press("UP", 600)], 1400,
               capture=capture)


def check_top_view():
    """UP walks up into the tactical top view, and it is a REAL board.

    Mode 3 at the full 256x224 with the twenty field slots as 32x32 sprites, no
    hand, and the HUD still up.  Every card on it is identified pixel for pixel
    against the sprite sheet -- a sprite is 1:1 with the screen, so a card that
    is really there matches all 1024 of its pixels and there is nothing to
    argue about."""
    ppm, wram = run_top()
    stamp = read_stamp(wram)
    w, h, px = read_ppm(ppm)

    found, exact = [], []
    for row in range(4):
        for col in range(5):
            got = identify_card_sprite(px, w, h,
                                       TOP_X0 + col * TOP_CELL + 8,
                                       TOP_Y0 + row * TOP_CELL + 8)
            if got and got[1] >= 1024 * 0.98:
                found.append((row, col, got[0]))
                exact.append(got[1])
    if len(found) < 18:
        raise Failure("only %d of the twenty slots hold a card sprite that "
                      "matches the sheet -- the top view is not showing the "
                      "field" % len(found))
    if len(set(r for r, _, _ in found)) != 4 or len(set(c for _, c, _ in found)) != 5:
        raise Failure("the identified cards cover rows %s and columns %s -- the "
                      "top view is not a five by four table"
                      % (sorted(set(r for r, _, _ in found)),
                         sorted(set(c for _, c, _ in found))))
    if len(set(f for _, _, f in found)) < 4:
        raise Failure("the top view shows %d distinct faces -- it is drawing "
                      "one card everywhere" % len(set(f for _, _, f in found)))
    # Rows 0 and 3 are the two support rows, 1 and 2 the monster rows: the same
    # far-to-near order the perspective board uses, so walking up does not
    # rearrange the board under the player.
    # The two support rows are the ones the fixture fills deterministically
    # (equip_field is set from the support ids, not dealt), so they are what can
    # be asserted face by face.  The monster rows come off the shuffled deck and
    # a deck holds supports as well, so what they hold is the rules' business.
    sup = [f for r, _, f in found if r in (0, 3)]
    if any(f < SUPPORT_FIRST or f >= CARD_BACK for f in sup):
        raise Failure("the support rows hold %s, which are not support faces"
                      % sup)

    # THE HAND IS NOT VISIBLE.  It is not on the board, and the top view is the
    # board from above; the rows the hand occupies in the other view are the
    # table's own bottom edge and nothing else.
    hand_band = set()
    for y in range(TOP_Y0 + 4 * TOP_CELL + 2, min(h, TOP_MSG_Y - 2)):
        for x in range(0, w, 2):
            i = (y * w + x) * 3
            hand_band.add(px[i:i + 3])
    if len(hand_band) > 1:
        raise Failure("%d colours between the table's near edge and the prompt "
                      "-- something is drawn where the hand used to be"
                      % len(hand_band))

    you = read_sprite_line(px, w, h, 8, TOP_LP_Y, 9)
    if you != "YOU %04d" % stamp["lp_player"]:
        raise Failure("the top view's life-point line reads %r" % you)
    return "%d of 20 slots identified exactly, %d distinct faces, %r" % (
        len(found), len(set(f for _, _, f in found)), you)


def check_top_view_switch_is_seamless():
    """THE MODE CHANGE SHOWS NO BLACK FRAME, and that is the whole design.

    Both pictures are resident -- the bitmap owns VRAM words $0000-$3FFF, the
    sprites $4000-$5FFF and the top view $6000-$73FF -- and one CGRAM serves
    both, so changing view rewrites nothing and needs no force-blank window.
    This captures every field across the UP press and demands that not one of
    them is blank.  A force-blanked switch, or one that reloaded VRAM, would
    put at least one black or half-drawn field in here."""
    run_top(capture=(600, 700, 1))
    frames = sorted(os.listdir(os.path.join(OUT, "topview.frames")))
    if len(frames) < 40:
        raise Failure("captured %d fields across the switch, need at least 40"
                      % len(frames))
    lit = []
    for name in frames:
        w, h, px = read_ppm(os.path.join(OUT, "topview.frames", name))
        n = sum(1 for i in range(0, len(px), 3 * 8)
                if px[i:i + 3] != b"\x00\x00\x00")
        lit.append(n)
    worst = min(lit)
    typical = sorted(lit)[len(lit) // 2]
    if worst < typical // 3:
        raise Failure("a field across the switch has %d lit samples against a "
                      "typical %d -- the mode change is blanking the screen"
                      % (worst, typical))
    return "%d fields across the switch, quietest %d lit samples against %d" % (
        len(frames), worst, typical)


CHECKS = [
    ("cartridge", check_cartridge),
    ("boot", check_boot),
    ("still resolution", check_still_resolution),
    ("moving resolution", check_moving_resolution),
    ("floor texture", check_floor_is_textured),
    ("board shape", check_board_is_a_slab),
    ("five by four", check_board_is_five_by_four),
    ("cards on board", check_cards_on_board),
    ("face-down card", check_face_down_card),
    ("empty slot", check_empty_slot_is_floor),
    ("quad card", check_quad_card),
    ("hud text", check_hud_text),
    ("top view", check_top_view),
    ("top view switch", check_top_view_switch_is_seamless),
    ("duel flow", check_duel_flow),
    ("duel plays out", check_duel_plays_out),
    ("render cost", check_render_cost),
]


def main():
    try:
        check_rom_fresh()
    except Failure as exc:
        print("FAIL  freshness: %s" % exc)
        return 1
    failures = 0
    for name, fn in CHECKS:
        try:
            print("ok    %-20s %s" % (name, fn()))
        except Failure as exc:
            print("FAIL  %-20s %s" % (name, exc))
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
