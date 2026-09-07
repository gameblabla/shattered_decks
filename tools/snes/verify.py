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
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROM = os.path.join(ROOT, "build", "snes", "waifusnes.sfc")
FAUST = os.path.join(ROOT, "SNES", "snesfaust", "snesfaust-mednafen")
OUT = os.path.join(ROOT, "build", "snes", "verify")

# The frame stamp, mirroring src/snes/snes_stamp.h.
STAMP_FIELDS = ["magic", "scene", "frames", "render_lines", "board_res",
                "duel_turn", "lp_player", "lp_com", "duel_result", "checksum"]
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
    ppm, wram = run("boot", [(0, 600, 0)], 400)
    stamp = read_stamp(wram)
    if stamp["frames"] < 60:
        raise Failure("only %d game frames in 400 -- the main loop is stalled"
                      % stamp["frames"])
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


def run_still():
    return run("still", [(0, 600, 0)], 400)


def run_moving(capture=None):
    # Y toggles the board resolution.  The press has to land after the scene
    # machine is running, and the frames after it are what the camera sways
    # through.
    return run("moving", [(0, 120, 0), (121, 124, PAD["Y"]), (125, 600, 0)],
               400, capture=capture)


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
    # The HUD band must stay at 2x2 while the board halves, which is what the
    # per-band HDMA table buys.
    ppm, wram = run_moving()
    stamp = read_stamp(wram)
    if stamp["board_res"] != 0:
        raise Failure("Y did not switch the board to the moving resolution")
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
    accumulators, which turned the near half of the board back into backdrop."""
    ppm, _ = run_still()
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


def check_camera_moves():
    """The moving resolution exists for a camera that moves, so prove it does:
    two frames a few apart must differ."""
    ppm, wram = run_moving(capture=(200, 240, 20))
    frames = sorted(os.listdir(os.path.join(OUT, "moving.frames")))
    if len(frames) < 2:
        raise Failure("captured %d frames, need at least 2" % len(frames))
    a = read_ppm(os.path.join(OUT, "moving.frames", frames[0]))
    b = read_ppm(os.path.join(OUT, "moving.frames", frames[-1]))
    diff = sum(1 for i in range(0, len(a[2]), 3) if a[2][i:i+3] != b[2][i:i+3])
    if diff < 1000:
        raise Failure("only %d pixels differ between two frames -- the camera is "
                      "not moving" % diff)
    return "%d pixels differ across %d frames of sway" % (diff, len(frames))


def check_render_cost():
    """The measurement that replaces section 4.4's estimates.

    render_lines is scanlines of wall clock for one board, taken from the V
    counter and the vblank count (src/snes/snes_duel.c), so 262 of them is one
    NTSC field."""
    _, wram = run_still()
    still = read_stamp(wram)["render_lines"]
    _, wram = run_moving()
    moving = read_stamp(wram)["render_lines"]
    if still == 0 or moving == 0:
        raise Failure("no render timing was recorded (still %d, moving %d)"
                      % (still, moving))
    if moving >= still:
        raise Failure("the moving board (%d lines) is not cheaper than the still "
                      "one (%d) -- the resolution switch is not doing anything"
                      % (moving, still))
    return ("still %d lines (%.1f fields, %.1f fps), moving %d lines "
            "(%.1f fields, %.1f fps)"
            % (still, still / 262.0, 60.0 / (still / 262.0),
               moving, moving / 262.0, 60.0 / (moving / 262.0)))


CHECKS = [
    ("cartridge", check_cartridge),
    ("boot", check_boot),
    ("still resolution", check_still_resolution),
    ("moving resolution", check_moving_resolution),
    ("floor texture", check_floor_is_textured),
    ("board shape", check_board_is_a_slab),
    ("camera motion", check_camera_moves),
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
