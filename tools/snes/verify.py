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
    hud = texel_size(px, w, 200, 2)
    if hud != 2:
        raise Failure("HUD band texel is %d screen pixels wide, expected 2" % hud)
    return "still band is exactly 2x2"


def check_moving_resolution():
    """The board halves itself while something is moving, and the HUD does not.

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
    hud = texel_size(px, w, 200, 2)
    if board != 4:
        raise Failure("moving band texel is %d screen pixels wide, expected 4" % board)
    if hud != 2:
        raise Failure("HUD band texel is %d screen pixels wide, expected 2 -- the "
                      "per-band HDMA scale is not being applied" % hud)
    return "moving band 4x4 over a 2x2 HUD band"


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
    cols = band_colours(px, w, 100, 155)
    if len(cols) < 12:
        raise Failure("the board band shows %d colours -- the floor is not "
                      "textured" % len(cols))
    sky = band_colours(px, w, 0, 30)
    if len(sky) < 3:
        raise Failure("the sky band shows %d colours -- the backdrop picture is "
                      "not being drawn" % len(sky))
    return "%d colours on the floor, %d in the backdrop" % (len(cols), len(sky))


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
    # The surround is one flat colour, and the left edge of line 60 is outside
    # the slab: the far end of the board is the narrow one, and the backdrop
    # picture stops at the painted horizon well above it.
    bg = px[(60 * w + 2) * 3:(60 * w + 2) * 3 + 3]
    # Its EXTENT, not the count of non-surround pixels: the grooves between
    # tiles quantise into the same dark brown as the surround, so counting
    # would measure the texture rather than the slab.
    widths = []
    for y in (50, 58, 66):
        xs = [x for x in range(w)
              if px[(y * w + x) * 3:(y * w + x) * 3 + 3] != bg]
        widths.append(xs[-1] - xs[0] + 1 if xs else 0)
    if not (widths[0] < widths[1] <= widths[2]):
        raise Failure("board widths down the screen are %s -- the slab is not "
                      "in perspective" % widths)
    if widths[2] < w // 2:
        raise Failure("the board is only %d pixels wide by line 66" % widths[2])
    # Below the slab's near edge there is no board at all: this is the half of
    # the bounds check the widening test cannot see.
    near = [x for x in range(w)
            if px[(156 * w + x) * 3:(156 * w + x) * 3 + 3] != bg]
    if near:
        raise Failure("%d pixels of board below its near edge on line 156 -- the "
                      "slab is not bounded" % len(near))
    return "slab widens %d -> %d -> %d pixels, and ends at its near edge" % tuple(widths)


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
CAM_Z, CAM_HEIGHT = -3.0, 1.0

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
    focal, horizon = STILL_W / 2.0, STILL_H / 8.0
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

FONT = os.path.join(ROOT, "src", "snes", "assets", "snes_font.bin")
HUD_LP_ROW = 80                 # framebuffer rows, mirroring snes_duel.c
HUD_MSG_ROW = 89


def read_hud_line(px, w, fb_row):
    """Read a line of HUD text back off the screen, glyph by glyph.

    The HUD band is framebuffer rows 80..111 shown at 2x2 from line 160, and
    the text is the shared 1bpp font drawn in white over its own shadow -- so a
    glyph cell can be sampled into eight bytes and matched against the same font
    table the game draws from.  This is how the harness reads life points: not
    "there are bright pixels in the band" but the actual number."""
    if not os.path.exists(FONT):
        raise Failure("no font at %s -- run: python3 tools/snes/gen_snes_font.py"
                      % os.path.relpath(FONT, ROOT))
    with open(FONT, "rb") as fh:
        font = fh.read()
    out = ""
    for col in range(16):
        bits = []
        for v in range(8):
            row = 0
            for u in range(8):
                x = 2 * (col * 8 + u)
                y = 160 + 2 * (fb_row - HUD_LP_ROW + v)
                i = (y * w + x) * 3
                if px[i] > 200 and px[i + 1] > 200 and px[i + 2] > 150:
                    row |= 0x80 >> u
            bits.append(row)
        best = min((sum(bin(font[ch * 8 + i] ^ bits[i]).count("1")
                        for i in range(8)), ch) for ch in range(32, 127))
        out += chr(best[1]) if best[0] <= 6 else "?"
    return out.rstrip()


def check_hud_text():
    """The band says what the rules say.

    Both lines are decoded off the screenshot and checked against the frame
    stamp, so a HUD that draws stale or wrong numbers fails here rather than
    looking plausible."""
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    w, h, px = read_ppm(ppm)
    lp = read_hud_line(px, w, HUD_LP_ROW)
    prompt = read_hud_line(px, w, HUD_MSG_ROW)
    expect = "YOU%04d COM%04d" % (stamp["lp_player"], stamp["lp_com"])
    if lp != expect:
        raise Failure("the life-point line reads %r, but the rules say %r"
                      % (lp, expect))
    if UI[stamp["ui"]] == "HAND" and not prompt.startswith("A:PLAY"):
        raise Failure("the prompt reads %r while the UI is in HAND" % prompt)
    return "%r / %r" % (lp, prompt)


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
    msg = read_hud_line(px, w, HUD_MSG_ROW)
    expect = "YOU WIN  A:AGAIN" if won else "YOU LOSE A:AGAIN"
    if msg != expect:
        raise Failure("the duel was %s but the band reads %r"
                      % ("won" if won else "lost", msg))
    return "%s on turn %d, %d fields, band reads %r" % (
        "won" if won else "lost", stamp["duel_turn"], 9000, msg)


CHECKS = [
    ("cartridge", check_cartridge),
    ("boot", check_boot),
    ("still resolution", check_still_resolution),
    ("moving resolution", check_moving_resolution),
    ("floor texture", check_floor_is_textured),
    ("board shape", check_board_is_a_slab),
    ("cards on board", check_cards_on_board),
    ("face-down card", check_face_down_card),
    ("empty slot", check_empty_slot_is_floor),
    ("quad card", check_quad_card),
    ("hud text", check_hud_text),
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
