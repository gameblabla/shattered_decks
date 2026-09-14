#!/usr/bin/env python3
"""Headless verification for the SNES port.

`SNES/snes-mednafen-1.32.1-accurate-headless-linux-x86_64 script` is the
accurate headless Mednafen build: it takes an input script, runs N frames, and
drops the last frame as a PPM, a WRAM dump and optionally a directory of
frames.  This is the harness that turns that into assertions.

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

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snes_dc
import gen_snes_planar as planar

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# THE DEBUG ROM (make -f Makefile.snes DEBUG=1): the retail one answers none
# of the pad switches the runs below are driven by.
ROM = os.path.join(ROOT, "build", "snes", "waifusnes_debug.sfc")
RETAIL_ROM = os.path.join(ROOT, "build", "snes", "waifusnes.sfc")
MEDNAFEN = os.path.join(
    ROOT, "SNES", "snes-mednafen-1.32.1-accurate-headless-linux-x86_64")
OUT = os.path.join(ROOT, "build", "snes", "verify")
TIMEOUT = 120

# The frame stamp, mirroring src/snes/snes_stamp.h.
STAMP_FIELDS = ["magic", "scene", "frames", "render_lines", "frame_gen",
                "duel_turn", "lp_player", "lp_com", "duel_result", "ui",
                "cursor", "field_cards", "phase", "turn_owner", "deck_slot",
                "deck_count", "deck_head", "storage_count", "save_valid",
                "map_lines", "conv_lines", "nmi_skips", "turn_max_lines", "rest_max_lines", "dropped", "held_max_lines", "occupied", "view", "battle_phase",
                "battle_field", "battle_damage", "checksum"]

# enum SnesDuelUi, mirroring src/snes/snes_duel.c.
UI = ["HAND", "PLACE", "EQUIP_TARGET", "ATTACKER", "DEFENDER", "COM",
      "RESULT", "FUSE_TARGET", "CHECK", "BATTLE_ART", "EFFECT_ART",
      "FUSION_ART"]
STAMP_MAGIC = 0x5744
BATTLE_VERDICT_Y = 94           # snes_battle.c VERDICT_Y

# SNES serial pad bits, the order the headless emulator's script rows use.
PAD = {"B": 0x8000, "Y": 0x4000, "SELECT": 0x2000, "START": 0x1000,
       "UP": 0x0800, "DOWN": 0x0400, "LEFT": 0x0200, "RIGHT": 0x0100,
       "A": 0x0080, "X": 0x0040, "L": 0x0020, "R": 0x0010}

SCENES = ["BOOT", "TITLE", "MENU", "STORY_MAP", "STORY_TALK", "DUEL",
          "BATTLE_ART", "RESULT", "DECK", "ENDING"]

# Where the Mode 3 card presentation puts its cards, mirroring
# src/snes/snes_cardart.h: the PC-FX battle card positions.
BIGCARD_CHECK_X, BIGCARD_CHECK_Y = 4, 22
BIGCARD_BATTLE_X0, BIGCARD_BATTLE_X1 = 4, 132


class Failure(Exception):
    pass


def check_rom_fresh():
    if not os.path.exists(ROM):
        raise Failure("no ROM at %s -- run: make -f Makefile.snes DEBUG=1" % ROM)
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
            if f.endswith((".pyc", ".md")) or "__pycache__" in f or f == __file__:
                continue
            if os.path.getmtime(f) > rom_time:
                newer.append(os.path.relpath(f, ROOT))
    if newer:
        raise Failure("ROM is older than %d source file(s), e.g. %s -- rebuild "
                      "before capturing" % (len(newer), ", ".join(sorted(newer)[:3])))


def rom_header(rom=None):
    """Decode the cartridge shape out of the ROM itself.

    The emulator's own `header` command mislabels $31 as SlowROM, so the map-mode
    byte is decoded here instead of trusted from its output."""
    with open(rom or ROM, "rb") as fh:
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


_RUN_CACHE = {}

def run(name, script, frames, capture=None):
    """One scripted run.  `script` is a list of (start, end, pad_mask) rows."""
    cacheable = name in ("still", "fixture", "nocards", "hold", "moving", "topview")
    identity = (os.path.realpath(ROM), os.path.getsize(ROM),
                os.stat(ROM).st_mtime_ns, os.path.realpath(MEDNAFEN),
                os.path.getsize(MEDNAFEN), os.stat(MEDNAFEN).st_mtime_ns)
    key = (identity, name, tuple(script), frames, capture)
    if cacheable and key in _RUN_CACHE:
        return _RUN_CACHE[key]
    os.makedirs(OUT, exist_ok=True)
    # Keep the regression's battery state separate from the owner's emulator
    # saves.  Multiple runs in this file intentionally share this directory so
    # the deck check can prove persistence across two emulator processes.
    os.makedirs(os.path.join(OUT, "sav"), exist_ok=True)
    in_path = os.path.join(OUT, name + ".in.txt")
    with open(in_path, "w") as fh:
        for start, end, mask in script:
            fh.write("%d %d %d 0\n" % (start, end, mask))
    ppm = os.path.join(OUT, name + ".ppm")
    wram = os.path.join(OUT, name + ".wram.bin")
    argv = [MEDNAFEN, "script", ROM, in_path, str(frames), ppm, wram]
    if capture:
        frame_dir = os.path.join(OUT, name + ".frames")
        # EMPTIED FIRST.  A capture that writes fewer frames than the last one
        # leaves the older run's frames behind, and comparing "the first two
        # frames" then compares two runs -- which is how a flicker that had
        # already been fixed went on being reproduced for an hour.
        shutil.rmtree(frame_dir, ignore_errors=True)
        os.makedirs(frame_dir, exist_ok=True)
        # NO COVERAGE AND NO VRAM TRACE WITH A CAPTURE.  Nothing here reads
        # either, and coverage stops the emulator at a hundred million
        # instructions -- about 8,500 fields -- which silently cut every
        # longer captured run short at whatever turn that was.
        argv += ["-", "0", os.path.join(OUT, name + ".ppu"), "-", frame_dir,
                 str(capture[0]), str(capture[1]), str(capture[2])]
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, cwd=OUT,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired as exc:
        raise Failure("%s: emulator timed out after %ss" % (name, TIMEOUT)) from exc
    if proc.returncode != 0:
        raise Failure("%s: emulator exited %d\n%s" % (name, proc.returncode,
                                                      proc.stderr[-2000:]))
    if cacheable:
        _RUN_CACHE[key] = (ppm, wram)
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
    for off in range(0, len(wram) - n * 2 + 1):
        if wram[off] | (wram[off + 1] << 8) != STAMP_MAGIC:
            continue
        words = struct.unpack_from("<%dH" % n, wram, off)
        if sum(words[:-1]) & 0xFFFF != words[-1]:
            continue
        return dict(zip(STAMP_FIELDS, words))
    raise Failure("no valid frame stamp in %s -- the game never reached its "
                  "main loop, or the dump is torn" % os.path.basename(path))


def read_ppu_regs(prefix):
    """Read the final PPU register snapshot emitted by a capture run."""
    path = prefix + ".regs"
    if not os.path.exists(path):
        raise Failure("capture did not produce PPU registers at %s" %
                      os.path.basename(path))
    regs = {}
    with open(path) as fh:
        for line in fh:
            match = re.match(r"^\s*([A-Za-z0-9_]+)\s*(?:[=:]\s*)?([$0-9A-Fa-fx]+)", line)
            if not match:
                continue
            try:
                raw = match.group(2)
                if raw.startswith("$"):
                    raw = "0x" + raw[1:]
                regs[match.group(1).upper()] = int(raw, 0)
            except ValueError:
                continue
    if "BGMODE" not in regs:
        raise Failure("PPU register dump %s has no BGMODE" %
                      os.path.basename(path))
    return regs


def read_oam(prefix):
    """Read the 512-byte OAM shadow and its 32-byte high table."""
    path = prefix + ".oam"
    if not os.path.exists(path):
        raise Failure("capture did not produce OAM at %s" % os.path.basename(path))
    with open(path, "rb") as fh:
        blob = fh.read()
    if len(blob) != 0x220:
        raise Failure("OAM dump is %d bytes, expected 544" % len(blob))
    return blob[:0x200], blob[0x200:]


def oam_sprite(oam, oamhi, index):
    off = index * 4
    x = oam[off + 0]
    if oamhi[index >> 2] & (1 << ((index & 3) * 2)):
        x |= 0x100
    return x, oam[off + 1], oam[off + 2], oam[off + 3]


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

def check_cartridge(rom=None):
    h = rom_header(rom)
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


def check_retail_rom():
    """The retail ROM has the same shape and none of the harness's switches.

    Every other check here drives the DEBUG ROM.  This one runs the retail
    build once, pressing the title's deck shortcut (Y): the debug ROM leaves
    for the editor on it, the retail one must stay on the title."""
    global ROM
    if not os.path.exists(RETAIL_ROM):
        raise Failure("no retail ROM at %s -- run: make -f Makefile.snes" % RETAIL_ROM)
    if os.path.getmtime(RETAIL_ROM) < os.path.getmtime(ROM) - 600:
        raise Failure("the retail ROM is much older than the debug one -- rebuild both")
    shape = check_cartridge(RETAIL_ROM)
    debug_rom = ROM
    ROM = RETAIL_ROM
    try:
        _, wram = run("retail_title_y", [(100, 160, PAD["Y"])], 600)
    finally:
        ROM = debug_rom
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("TITLE"):
        raise Failure("the retail ROM answered the title's Y shortcut: scene %s"
                      % SCENES[stamp["scene"]])
    return "retail: %s; the title ignores the harness's Y" % shape


def check_title():
    """Boot shows intact painted art and waits for title input."""
    ppm, wram = run("title", [(0, 100, 0)], 100, capture=(99, 99, 1))
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("TITLE"):
        raise Failure("title capture reached scene %s after only %d frames" %
                      (SCENES[stamp["scene"]], stamp["frames"]))
    w, h, px = read_ppm(ppm)
    if (w, h) != (256, 224):
        raise Failure("title is %dx%d, expected the 256x224 visible area" %
                      (w, h))
    visible = sum(1 for i in range(0, len(px), 3)
                  if px[i:i + 3] != b"\x00\x00\x00")
    if visible < w * h * 3 // 4:
        raise Failure("title has only %d/%d non-black pixels -- artwork is not "
                      "visible" % (visible, w * h))
    # A stale scene-text upload used to overwrite a 2 KiB strip of title art.
    # Visible scanline 191 already belongs to the blank prompt band.
    black_rows = [sum(px[(y * w + x) * 3:(y * w + x + 1) * 3] ==
                      b"\x00\x00\x00" for x in range(w)) for y in range(160, 191)]
    if max(black_rows) > w // 2:
        raise Failure("title art contains a %d-pixel black row at y=%d" %
                      (max(black_rows), 160 + black_rows.index(max(black_rows))))
    with open(os.path.join(ROOT, "src/snes/assets/snes_title_tiles.bin"), "rb") as fh:
        expected = fh.read()
    with open(os.path.join(OUT, "title.ppu.vram"), "rb") as fh:
        actual = fh.read(len(expected))
    if actual != expected:
        raise Failure("title tile VRAM differs from the generated artwork")
    return "scene TITLE, painted 256x224 art (%d%% non-black)" % (
        visible * 100 // (w * h))


def title_expected_rows(rows, prompt=False):
    """The title's intended pixels on the given screen rows, decoded from the
    generated map, 8bpp tiles and palette: what the PPU must show when the
    map's row 0 is aligned with visible line 0."""
    with open(os.path.join(ROOT, "src/snes/assets/snes_title_tiles.bin"), "rb") as fh:
        tiles = fh.read()
    name = "snes_title_prompt_map.bin" if prompt else "snes_title_map.bin"
    with open(os.path.join(ROOT, "src/snes/assets", name), "rb") as fh:
        tilemap = fh.read()
    with open(os.path.join(ROOT, "src/snes/assets/snes_title_pal.bin"), "rb") as fh:
        pal = fh.read()
    out = {}
    for y in rows:
        line = []
        for x in range(256):
            cell = (y // 8) * 32 + x // 8
            tile = tilemap[cell * 2] | (tilemap[cell * 2 + 1] << 8)
            idx = planar_pixel(tiles, tile & 0x3FF, x & 7, y & 7)
            word = pal[idx * 2] | (pal[idx * 2 + 1] << 8)
            line.append((word & 31, (word >> 5) & 31, (word >> 10) & 31))
        out[y] = line
    return out


def check_title_scanlines():
    """THE FIRST AND LAST VISIBLE LINES ARE THE PAINTING'S OWN.

    The PPU shows map line VOFS+1 on the first scanline; with VOFS 0 the
    painting's twenty-eight tile rows started one line up and visible line 223
    fell on map row 28, an unused row of tile zero, which drew as a flat blue
    line across the bottom of the screen.  Rows 0, 1, 222 and 223 are compared
    pixel for pixel against the generated artwork, which the old check (tile
    bytes and interior rows) could not see."""
    ppm, wram = run("title", [(0, 100, 0)], 100, capture=(99, 99, 1))
    w, h, px = read_ppm(ppm)
    rows = (0, 1, 222, 223)
    want = title_expected_rows(rows)
    for y in rows:
        bad = 0
        for x in range(w):
            got = screen5(px, w, x, y)
            if got != want[y][x]:
                bad += 1
        if bad:
            raise Failure("title row %d differs from the artwork in %d of 256 "
                          "pixels (row colours: %d)" %
                          (y, bad, len(set(want[y]))))
    if len(set(want[223])) < 8:
        raise Failure("the artwork's own last row is nearly flat (%d colours): "
                      "the check cannot tell it from a stuck line" %
                      len(set(want[223])))
    return "title rows 0, 1, 222 and 223 match the artwork pixel for pixel"


def check_title_input():
    """The visible three-row menu must launch Random Battle."""
    _, wram = run("title_input", [press("START", 150), press("DOWN", 300),
                                   press("A", 450)], 2500)
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("DUEL"):
        raise Failure("A-button title start ended in scene %s" %
                      SCENES[stamp["scene"]])
    return "START, DOWN, A selects Random Battle and enters DUEL"


def check_deck_gallery(w, h, px, cards):
    """The PC-FX gallery on the SNES editor: a navy panel and `cards` icons.

    The previous check only counted non-black pixels, and passed a screen of
    garbage tiles.  This one reads the panel between the counters and the
    first card row, which must be the card check's navy, and every icon cell
    at the PC-FX positions (x 14 + 40 col, y 59 + 40 row, 26x34), which must
    hold a many-coloured picture and not the panel."""
    def at(x, y):
        i = (y * w + x) * 3
        return px[i], px[i + 1], px[i + 2]
    navy = at(128, 53)
    if not (navy[2] > navy[0] and navy[2] > navy[1] and max(navy) < 96):
        raise Failure("deck editor panel is %r, expected the card check's navy" % (navy,))
    for slot in range(18):
        x0 = 14 + (slot % 6) * 40
        y0 = 59 + (slot // 6) * 40
        colours = set(at(x0 + x, y0 + y) for y in range(34) for x in range(26))
        if slot < cards:
            if len(colours) < 8 or navy in colours and len(colours) < 12:
                raise Failure("gallery slot %d shows %d colours, not a card" %
                              (slot, len(colours)))
        elif colours != {navy}:
            raise Failure("empty gallery slot %d is not bare panel" % slot)


def check_deck_editor():
    """The reference DECK/STORAGE editor must move and persist both lists.

    The first run moves the default deck's first card (0) to STORAGE, moves
    the first stored support card back, and saves.  The second run is a fresh
    emulator process; its stamp proves that the complete 40/40 deck and
    four-card STORAGE list survived.  Restore the default afterward so later
    duel checks
    start from the same known collection.
    """
    change = [
        (100, 160, PAD["Y"]),        # title -> deck editor
        (2200, 2260, PAD["R"]),      # deterministic default deck
        (2400, 2460, PAD["A"]),      # DECK -> STORAGE; head becomes 7
        (2600, 2660, PAD["X"]),      # DECK tab -> STORAGE tab
        (2800, 2860, PAD["A"]),      # STORAGE -> DECK
        (3000, 3060, PAD["Y"]),      # write SRAM
    ]
    ppm, wram = run("deck_save", change, 3300)
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("DECK"):
        raise Failure("save run ended in scene %s" % SCENES[stamp["scene"]])
    if (not stamp["save_valid"] or stamp["deck_count"] != 40 or
            stamp["storage_count"] != 4):
        raise Failure("save run reports valid=%d deck=%d storage=%d, expected "
                      "1/40/4" % (stamp["save_valid"], stamp["deck_count"],
                                   stamp["storage_count"]))
    if stamp["deck_head"] != 7:
        raise Failure("save run head is %d, expected changed card 7" %
                      stamp["deck_head"])
    w, h, px = read_ppm(ppm)
    visible = sum(1 for i in range(0, len(px), 3)
                  if px[i:i + 3] != b"\x00\x00\x00")
    if visible < 1000:
        raise Failure("deck editor has only %d non-black pixels" % visible)
    # The save run ends on the STORAGE tab: four cards, fourteen bare cells.
    check_deck_gallery(w, h, px, 4)

    load_ppm, wram = run("deck_load", [(100, 160, PAD["Y"])], 2500)
    loaded = read_stamp(wram)
    w, h, px = read_ppm(load_ppm)
    check_deck_gallery(w, h, px, 18)        # the DECK tab, forty cards
    if loaded["scene"] != SCENES.index("DECK"):
        raise Failure("fresh process ended in scene %s" %
                      SCENES[loaded["scene"]])
    if (loaded["save_valid"] != 1 or loaded["deck_count"] != 40 or
            loaded["storage_count"] != 4 or loaded["deck_head"] != 7):
        raise Failure("fresh process loaded valid=%d deck=%d storage=%d head=%d, "
                      "expected 1/40/4/7" %
                      (loaded["save_valid"], loaded["deck_count"],
                       loaded["storage_count"], loaded["deck_head"]))

    # Exercise the player-facing title menu's third row.  This run only reads
    # SRAM and leaves the saved deck available to later checks.
    _, wram = run("menu_load_save", [press("START", 150),
                                      press("DOWN", 300),
                                      press("DOWN", 450),
                                      press("A", 600)], 2600)
    menu_loaded = read_stamp(wram)
    if menu_loaded["scene"] != SCENES.index("STORY_TALK"):
        raise Failure("LOAD SAVE menu ended in scene %s" %
                      SCENES[menu_loaded["scene"]])
    if menu_loaded["save_valid"] != 1 or menu_loaded["deck_count"] != 40:
        raise Failure("LOAD SAVE menu reports valid=%d deck=%d, expected 1/40" %
                      (menu_loaded["save_valid"], menu_loaded["deck_count"]))

    check_ppm, wram = run("deck_check", [
        (100, 160, PAD["Y"]),
        (2300, 2360, PAD["B"]),
    ], 2600)
    checked = read_stamp(wram)
    if checked["scene"] != SCENES.index("DECK"):
        raise Failure("CARD CHECK left scene %s" % SCENES[checked["scene"]])
    _, _, editor_px = read_ppm(load_ppm)
    _, _, check_px = read_ppm(check_ppm)
    if sum(a != b for a, b in zip(editor_px, check_px)) < 1000:
        raise Failure("CARD CHECK capture is indistinguishable from the gallery")

    _, wram = run("deck_gate", [
        (100, 160, PAD["Y"]),
        (2300, 2360, PAD["A"]),
        (2500, 2560, PAD["START"]),
    ], 2800)
    gated = read_stamp(wram)
    if gated["scene"] != SCENES.index("DECK"):
        raise Failure("incomplete deck escaped to scene %s" % SCENES[gated["scene"]])

    run("deck_restore", [
        (100, 160, PAD["Y"]),
        (2300, 2360, PAD["R"]),
        (2500, 2560, PAD["Y"]),
    ], 2800)
    return "DECK/STORAGE editor + four-slot SRAM round trip survived emulator reset"


def check_story_scene():
    """B opens the story window: sky ramp, ground, two BG1 speakers, typewriter."""
    name = "story_portrait_entry"
    ppm, wram = run(name, [(100, 160, PAD["B"])], 2500,
                    capture=(180, 500, 8))
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("STORY_TALK"):
        raise Failure("story entry ended in scene %s" % SCENES[stamp["scene"]])
    w, h, px = read_ppm(ppm)
    bottom = sum(1 for y in range(144, h) for x in range(w)
                 if px[(y * w + x) * 3:(y * w + x + 1) * 3] != b"\x00\x00\x00")
    if bottom < 40:
        raise Failure("story text window has only %d lit pixels" % bottom)
    # The sky is the backdrop tinted per scanline: a column of the left edge
    # must be lit above the horizon and change colour down the ramp.
    sky = [px[(y * w + 2) * 3:(y * w + 3) * 3] for y in range(0, 120, 8)]
    if any(c == b"\x00\x00\x00" for c in sky):
        raise Failure("the story sky has a black line at x=2: %s" % sky)
    if len(set(sky)) < 4:
        raise Failure("the story sky is flat, not a ramp: %s" % sky)
    # The ground is read off the FIRST capture: once both speakers are in
    # they stand across the whole width of it (Serena is 160 wide).
    frames = sorted(os.listdir(os.path.join(OUT, name + ".frames")))
    if len(frames) < 4:
        raise Failure("captured only %d story frames" % len(frames))
    first = read_ppm(os.path.join(OUT, name + ".frames", frames[0]))[2]
    ground = [first[(y * w + x) * 3:(y * w + x + 1) * 3]
              for y in range(120, 144, 4) for x in range(0, w, 32)]
    if sum(1 for c in ground if c != b"\x00\x00\x00") < len(ground) * 0.9:
        raise Failure("the ground rows 120..143 are not painted")
    # The two speakers are BG1 tile blocks: Serena's 20x17 tiles on the left,
    # the opponent's 16x17 on the right, both fully in by the time this ends;
    # the four columns they share hold the composited tiles (Serena in
    # front), which follow the blank tile after both blocks.
    with open(os.path.join(OUT, name + ".ppu.vram"), "rb") as fh:
        vram = fh.read()
    def cell(col, row):
        i = (0x7000 + row * 32 + col) * 2
        return vram[i] | (vram[i + 1] << 8)
    L_COLS, R_COLS, OVER = 20, 16, 4
    L_TILES, R_TILES = L_COLS * 17, R_COLS * 17
    OVER_TILE = L_TILES + R_TILES + 1
    for row in range(17):
        for col in range(32):
            if col < R_COLS:
                want = row * L_COLS + col
                who = "Serena's"
            elif col < L_COLS:
                want = OVER_TILE + row * OVER + (col - R_COLS)
                who = "the shared"
            else:
                want = L_TILES + row * R_COLS + (col - R_COLS)
                who = "the opponent's"
            if cell(col, 1 + row) != want:
                raise Failure("%s tile at (%d, %d) is %d, expected %d" %
                              (who, col, 1 + row, cell(col, 1 + row), want))
    with open(os.path.join(ROOT, "src/snes/assets/snes_portrait_0.bin"), "rb") as fh:
        serena = fh.read()
    if len(serena) != L_TILES * 64:
        raise Failure("Serena's block is %d bytes, not 20x17 tiles" % len(serena))
    if vram[:len(serena)] != serena:
        raise Failure("Serena's BG1 tiles in VRAM differ from the generated block")
    with open(os.path.join(ROOT, "src/snes/assets/snes_portrait_1_over.bin"), "rb") as fh:
        over = fh.read()
    if vram[OVER_TILE * 64:OVER_TILE * 64 + len(over)] != over:
        raise Failure("the shared columns' tiles in VRAM differ from the generated block")
    # The composite is Serena where she has a pixel and the opponent where
    # she has none: her hair's right edge is what the block exists for.
    with open(os.path.join(ROOT, "src/snes/assets/snes_portrait_1.bin"), "rb") as fh:
        opp = fh.read()
    from_serena = from_opp = 0
    for row in range(17):
        for k in range(OVER):
            o = (row * OVER + k) * 64
            s_tile = serena[(row * L_COLS + R_COLS + k) * 64:(row * L_COLS + R_COLS + k + 1) * 64]
            o_tile = opp[(row * R_COLS + k) * 64:(row * R_COLS + k + 1) * 64]
            for sv, ov, cv in zip(untile8(s_tile, 0), untile8(o_tile, 0),
                                  untile8(over, o)):
                if sv and cv == sv: from_serena += 1
                elif not sv and cv == ov: from_opp += 1
                else: raise Failure("a shared tile pixel is neither speaker's")
    if not from_serena or not from_opp:
        raise Failure("the shared columns show only one speaker (%d/%d)" %
                      (from_serena, from_opp))
    # The entrance is a slide: the early captures show fewer columns, and
    # THE WORDS WAIT FOR IT -- no dialogue text is typed while the speakers
    # are still walking in (rows 21..23 of the window stay blank).
    early = read_ppm(os.path.join(OUT, name + ".frames", frames[1]))[2]
    if early == px:
        raise Failure("the speakers did not move in during the entrance")
    for f in frames[:2]:
        fpx = read_ppm(os.path.join(OUT, name + ".frames", f))[2]
        typed = sum(1 for y in range(170, 194) for x in range(16, 240)
                    if fpx[(y * w + x) * 3:(y * w + x + 1) * 3] not in
                    (b"\x00\x00\x00", fpx[(y * w + 8) * 3:(y * w + 9) * 3]))
        if typed > 40:
            raise Failure("dialogue text (%d lit pixels) was typed before the "
                          "speakers had walked in (%s)" % (typed, f))
    return "STORY_TALK: sky ramp, ground tiles, two 112-colour BG1 speakers, typewriter"


def check_ending_scene():
    """X opens the real ending painting and its narration layer."""
    ppm, wram = run("ending", [(100, 160, PAD["X"])], 2500)
    stamp = read_stamp(wram)
    if stamp["scene"] != SCENES.index("ENDING"):
        raise Failure("ending entry ended in scene %s" % SCENES[stamp["scene"]])
    w, h, px = read_ppm(ppm)
    bottom = sum(1 for y in range(200, h) for x in range(w)
                 if px[(y * w + x) * 3:(y * w + x + 1) * 3] != b"\x00\x00\x00")
    if bottom < w * 20:
        raise Failure("ending narration layer has only %d lit pixels" % bottom)
    return "ENDING, real 256x224 painting with narration"


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
    return 100


# A RUN IS MEASURED IN EMULATOR FIELDS, AND A GAME FRAME IS MANY OF THEM.
# A board with twenty cards on it takes about twenty fields to draw (see the
# render-cost check), so a button has to be HELD for longer than one game frame
# or the poll that reads it never happens while it is down.  Sixty fields is
# comfortably longer than the slowest frame the port produces.
RUN_FRAMES = 2800
HOLD = 60
DUEL_READY = 2200

# The harness's switches in the duel, all of them documented in
# src/snes/snes_duel.c: R fills the board from the decks (the measurement and
# identification fixture), SELECT draws it without cards (the ablation), L hands
# the player's side to the rules (the demo, and the soak run), Y toggles the
# board resolution, and SELECT+X / SELECT+A stage deterministic Thunder and
# fusion presentations.


def press(button, at):
    return (at, at + HOLD, PAD[button])


def press_chord(buttons, at):
    mask = 0
    for button in buttons:
        mask |= PAD[button]
    return (at, at + HOLD, mask)


def random_battle_script():
    """Open the title menu and select its second, Random Battle, row."""
    return [press("START", 150), press("DOWN", 300), press("A", 450)]


def run_still(capture=None):
    return run("still", random_battle_script(), RUN_FRAMES, capture=capture)


def run_fixture(capture=None):
    """The full-board fixture: five monsters and five supports a side, one of
    them set face down.  Every id in it came off the duel's own shuffled deck --
    it is a fixed BOARD, not fixed art."""
    return run("fixture", random_battle_script() + [press("R", DUEL_READY)], RUN_FRAMES, capture=capture)


def run_no_cards(capture=None):
    """The fixture board with the cards NOT DRAWN.

    The port's rule is that a performance claim comes from a measurement or an
    ablation; this is the ablation that says what the cards cost.  It is also
    the only way to measure the slab's own shape, since twenty cards cover
    almost all of it."""
    return run("nocards", random_battle_script() + [press("R", DUEL_READY), press("SELECT", DUEL_READY + 200)],
               RUN_FRAMES, capture=capture)


def run_hold(capture=None):
    """A card picked up and carried: A chooses the first hand card, and the UI
    stays in PLACE with it hovering over the slot until it is put down."""
    return run("hold", random_battle_script() + [press("A", DUEL_READY),
                         press("RIGHT", DUEL_READY + 200),
                         press("RIGHT", DUEL_READY + 300)],
               RUN_FRAMES, capture=capture)


def run_moving(capture=None):
    # Y pins the moving-camera renderer.  The press has to land after the
    # scene machine is running.
    return run("moving", random_battle_script() + [press("Y", DUEL_READY)], RUN_FRAMES, capture=capture)


def run_demo(frames=14000):
    """L hands the player's side to the rules as well, so the duel plays itself
    to a result with no further input -- the port's soak run."""
    # The title scene now owns the first few dozen game frames; start the demo
    # after the normal unattended title timeout, just like the other duel
    # fixtures start after boot has settled.
    return run("demo", random_battle_script() + [press("L", DUEL_READY)], frames)


def run_flow(name, buttons):
    """A scripted turn through the duel UI, one button at a time."""
    script = random_battle_script()
    at = DUEL_READY
    for b in buttons:
        script.append(press(b, at))
        at += HOLD * 2
    # Allow the final input to settle on a complete frame.  Full-detail board
    # renders and their staged uploads can span many emulator fields, and a
    # turn hand-off may also animate the camera before the stamp is committed.
    return run(name, script, at + 2200)


def check_still_resolution():
    """The resting board resolves individual pixels in both axes."""
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    if stamp["frame_gen"] == 0:
        raise Failure("no board generation has been presented")
    w, h, px = read_ppm(ppm)
    horizontal = sum(px[(y*w+x)*3:(y*w+x)*3+3] != px[(y*w+x+1)*3:(y*w+x+1)*3+3]
                     for y in range(60,120) for x in range(70,180,2))
    vertical = sum(px[(y*w+x)*3:(y*w+x)*3+3] != px[((y+1)*w+x)*3:((y+1)*w+x)*3+3]
                   for y in range(60,120,2) for x in range(70,180))
    if min(horizontal, vertical) < 100:
        raise Failure("resting floor appears doubled (%d/%d)" % (horizontal,vertical))
    return "256x144 resting board resolves individual pixels in both axes (generation %d)" % stamp["frame_gen"]


def check_moving_resolution():
    """THE MOVING CAMERA IS 128x72, SHOWN DOUBLED.  Y pins the general
    renderer (the world texture through the inverse-ray walker into the
    motion frame, every occupied cell converted with each texel a 2x2 block
    of pixels) a frame a game frame at the resting camera: every even-aligned
    pixel pair must be identical in both axes and the picture must still
    change between the pairs, the frame generation must be advancing, and the
    map on screen must be complete -- every occupied cell a real tile.  The
    resting board is checked at 1:1 separately (check_still_resolution)."""
    ppm, wram = run_moving(capture=(RUN_FRAMES - 2, RUN_FRAMES - 1, 1))
    stamp = read_stamp(wram)
    if stamp["turn_max_lines"] == 0:
        raise Failure("Y fixture did not render through the moving path")
    w, h, px = read_ppm(ppm)
    def pix(x, y):
        return px[(y * w + x) * 3:(y * w + x) * 3 + 3]
    pair_h = sum(pix(x, y) != pix(x + 1, y) for y in range(60, 120) for x in range(70, 180, 2))
    pair_v = sum(pix(x, y) != pix(x, y + 1) for y in range(60, 120, 2) for x in range(70, 180))
    odd_h = sum(pix(x, y) != pix(x + 1, y) for y in range(60, 120) for x in range(71, 179, 2))
    odd_v = sum(pix(x, y) != pix(x, y + 1) for y in range(61, 119, 2) for x in range(70, 180))
    if pair_h or pair_v:
        raise Failure("the motion frame is not doubled: %d/%d even-aligned pixel "
                      "pairs differ across/down" % (pair_h, pair_v))
    if min(odd_h, odd_v) < 100:
        raise Failure("the motion frame is flat (%d/%d texel edges)" % (odd_h, odd_v))
    prefix = os.path.join(OUT, "moving.ppu")
    regs, data, words, base = published_map(prefix)
    occupied = sum(1 for wd in words[:576] if wd & 1023)
    if occupied != stamp["occupied"]:
        raise Failure("the map on screen names %d tiles but the stamp says %d cells were occupied"
                      % (occupied, stamp["occupied"]))
    if occupied > 351:
        raise Failure("%d occupied cells exceed the sparse budget" % occupied)
    return ("moving camera at 128x72 doubled: %d cells on the published map, generation %d, %d lines a frame"
            % (occupied, stamp["frame_gen"], stamp["turn_max_lines"]))


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
    # ...below the life panels, which are sprites and are meant to be there.
    sky = band_colours(px, w, LP_Y + 8, 40)
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
             if px[(140 * w + x) * 3:(140 * w + x) * 3 + 3] != black]
    if below:
        raise Failure("%d pixels of board below its near edge on line 140 -- the "
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
        # A single dark texel is sandstone noise, not a two-sided groove.
        # Real grooves remain multi-pixel runs even on the farthest band.
        merged = [(a, b) for a, b in merged if b > a]
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
    # The moving board is measured on the same fixture: R fills it, Y pins the
    # fixed motion cadence, and the number is the one an animating frame pays.
    _, wram = run("fixture_moving", random_battle_script() + [press("R", DUEL_READY),
                                      press("Y", DUEL_READY + 200)],
                  RUN_FRAMES)
    moving = read_stamp(wram)["render_lines"]
    if still == 0 or moving == 0:
        raise Failure("no render timing was recorded (still %d, moving %d)"
                      % (still, moving))
    if floor_only >= still:
        raise Failure("the ablated board (%d lines) is not cheaper than the "
                      "full one (%d) -- SELECT did not turn the cards off"
                      % (floor_only, still))
    return ("still %d lines (%.1f fields), moving %d lines (%.1f fields); "
            "ablated: floor %d, so the cards are %d lines (%d%%)"
            % (still, still / 262.0,
               moving, moving / 262.0,
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
STILL_W, STILL_H = planar.BOARD_W, planar.BOARD_H
# Mirroring snes_duel.c: the camera stands 2.75 units in front of the slab's
# near edge, which is what fits its 5-unit width across a viewport whose focal
# length is fixed at half its own width.
CAM_Z, CAM_HEIGHT = planar.CAM_Z, planar.CAM_HEIGHT
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
        blob = b""
        for name in ("snes_card_tex32.bin", "snes_card_tex32b.bin"):
            with open(os.path.join(ROOT, "src/snes/assets", name), "rb") as fh:
                blob += fh.read()
        _FACES = [blob[i * 1024:(i + 1) * 1024] for i in range(len(blob) // 1024)]
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


def slot_samples(px, w, h, row, col, ox, oy, lo=6, hi=26):
    """The interior texels of one slot's card, read out of a still screenshot.

    The frame's outermost texels are left out because half a pixel of rounding
    at a card's edge samples the tile beside it, and the whole grid is offset by
    (ox, oy) so the caller can search for the alignment the renderer's own
    rounding produced."""
    cx, cz = col - 2, ROW_Z[row]
    focal, horizon = planar.FOCAL, planar.HORIZON
    out = []
    for v in range(lo, hi):
        for u in range(lo, hi):
            wx = cx + ((u + 0.5) / 32.0 - 0.5) * 0.75
            wz = cz + 0.5 - ((v + 0.5) / 32.0)
            depth = wz - CAM_Z
            x = int(STILL_W / 2.0 + wx / depth * focal) + ox
            y = int(horizon + CAM_HEIGHT / depth * focal) + oy
            if 0 <= x < w and 0 <= y < h:
                i = (y * w + x) * 3
                out.append((v * 32 + u,
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

# Screen coordinates, mirroring snes_duel.c.  The life panels are at the TOP of
# the screen in both views and the band under the board reads downwards -- the
# hand, then the focused card's name, then its stats.
LP_Y, LP_YOU_X, LP_COM_X = 8, 8, 152
LP_LABEL_DX, LP_BAR_DX, LP_BAR_W, LP_NUM_DX = 2, 28, 32, 64
LP_MAX = 8000
HAND_Y, HAND_X0, HAND_PITCH = 150, 8, 48
NAME_Y, STAT_Y, STAT_ATK_X, STAT_DEF_X, STAT_NUM_DX = 199, 209, 8, 72, 16
TOP_CELL, TOP_X0, TOP_Y0 = 48, 8, 16
TOP_MSG_Y, TOP_STAT_X, TOP_STAT_GAP = 213, 120, 72
OVER_Y = 28
# The plate under the two text rows, from snes_m7fb.c's gradient table.  It is
# not in the bitmap at all: the bitmap's HUD rows are transparent and the plate
# is the backdrop with a fixed colour added to it, HDMA'd into $2132 a scanline
# at a time.  So it is twenty-six SINGLE lines, 197..222, out of the full
# fifteen-bit colour space -- line 197 a bright rule, then a smooth ramp -- and
# both the line above it and line 223 are black because the table says so.
BAND_TOP, BAND_BOTTOM = 197, 222
SPR_CARDS_HI = os.path.join(ROOT, "src", "snes", "assets",
                            "snes_spr_cards_hi.bin")
SPR_FACE_PAL = os.path.join(ROOT, "src", "snes", "assets",
                            "snes_spr_face_pal.bin")
SPR_FACE_PAL_GREY = os.path.join(ROOT, "src", "snes", "assets",
                                 "snes_spr_face_pal_grey.bin")
# The tiles the stat row prints instead of the words ATK and DEF, and where they
# are in the font sheet: the glyphs, the eight cursor corners (gold and red),
# the eighteen bar states, the two plates, then these.
ICON_TILE = GLYPH_COUNT + 8 + 18 + 2
ICON_NAMES = ("sword", "shield")
CARD_NAMES = os.path.join(ROOT, "src", "snes", "assets", "snes_card_names.bin")
NAME_LEN = 16

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


def read_icon(px, w, h, x, y):
    """Which stat icon is at (x, y), matched against the ROM's own tiles.

    The icons carry the same baked shadow the glyphs do and are inked in two
    different colours, so this compares the WHOLE tile -- every pixel, ink,
    shadow and transparent -- rather than looking for lit pixels.  A transparent
    pixel is whatever the band's gradient is under it, which is not a colour
    this can predict, so those pixels are the ones it skips."""
    font = spr_asset(SPR_FONT, "font")
    best = None
    for kind, name in enumerate(ICON_NAMES):
        tile = untile4(font, (ICON_TILE + kind) * 32)
        hit = miss = 0
        for v in range(8):
            for u in range(8):
                index = tile[v * 8 + u]
                if index == 0:
                    continue
                sx, sy = x + u, y + v
                if not (0 <= sx < w and 0 <= sy < h):
                    return None
                if screen5(px, w, sx, sy) == obj_colour(7, index):
                    hit += 1
                else:
                    miss += 1
        if best is None or hit - miss > best[1]:
            best = (name, hit - miss, hit, hit + miss)
    if best[2] < best[3] * 0.9:
        return None
    return best[0]


def face_colour(face, index, grey=False):
    """An entry of the palette snes_spr_cards_hi's `face` was cut against."""
    blob = spr_asset(SPR_FACE_PAL_GREY if grey else SPR_FACE_PAL,
                     "facepal_grey" if grey else "facepal")
    w = blob[(face * 16 + index) * 2] | (blob[(face * 16 + index) * 2 + 1] << 8)
    return (w & 31, (w >> 5) & 31, (w >> 10) & 31)


def card_sprite_pixels(face, hi=False, grey=False):
    """One card sprite as 32x32 five-bit colours, straight out of the ROM.

    `hi` reads the per-face sheet instead of the clustered one: that is what
    the five hand slots draw from, with the face's own fifteen colours uploaded
    into the slot's OBJ palette beside its tiles."""
    if hi:
        cards = spr_asset(SPR_CARDS_HI, "cardshi")
        colour = lambda i: face_colour(face, i, grey)
    else:
        cards = spr_asset(SPR_CARDS, "cards")
        pal = spr_asset(SPR_GROUP, "group")[face]
        colour = lambda i: obj_colour(pal, i)
    out = [None] * (32 * 32)
    for t in range(16):
        tile = untile4(cards, face * 512 + t * 32)
        tx, ty = (t % 4) * 8, (t // 4) * 8
        for y in range(8):
            for x in range(8):
                out[(ty + y) * 32 + tx + x] = colour(tile[y * 8 + x])
    return out


BIGCARDS = os.path.join(ROOT, "src", "snes", "assets", "snes_bigcards_%d.bin")
BIGCARD_FIRST_BANK, BIGCARD_PER_BANK = 30, 4
BIGCARD_TILES, BIGCARD_BYTES, BIGCARD_PAL_BYTES = 225, 14400, 160
BIGCARD_FIRST_COLOUR = 32
BIG_ART = 112


def untile8(blob, off):
    """One 8bpp tile's 64 indices."""
    out = [0] * 64
    for plane in range(8):
        for y in range(8):
            byte = blob[off + (plane // 2) * 16 + y * 2 + (plane & 1)]
            for x in range(8):
                if byte & (0x80 >> x):
                    out[y * 8 + x] |= 1 << plane
    return out


def bigcard_art_pixels(face):
    """The 112x112 painting of one Mode 3 card, as the PPU shows it: the
    generator's own tiles through the generator's own eighty-colour palette,
    five bits a channel.  The frame round it is shared and not compared."""
    bank = BIGCARD_FIRST_BANK + face // BIGCARD_PER_BANK
    blob = spr_asset(BIGCARDS % bank, "bigcards%d" % bank)
    base = (face % BIGCARD_PER_BANK) * (BIGCARD_BYTES + BIGCARD_PAL_BYTES)
    pal = blob[base + BIGCARD_BYTES:base + BIGCARD_BYTES + BIGCARD_PAL_BYTES]
    colours = {}
    for i in range(BIGCARD_PAL_BYTES // 2):
        w = pal[i * 2] | (pal[i * 2 + 1] << 8)
        colours[BIGCARD_FIRST_COLOUR + i] = (w & 31, (w >> 5) & 31, (w >> 10) & 31)
    out = [None] * (BIG_ART * BIG_ART)
    # The painting sits at (4, 6) in the 120x120 block: tile columns 0..14
    # and rows 0..14 cover it with a 4/6-pixel offset.
    for t in range(BIGCARD_TILES):
        tile = untile8(blob, base + t * 64)
        tx, ty = (t % 15) * 8 - 4, (t // 15) * 8 - 6
        for y in range(8):
            yy = ty + y
            if not 0 <= yy < BIG_ART:
                continue
            for x in range(8):
                xx = tx + x
                if 0 <= xx < BIG_ART:
                    out[yy * BIG_ART + xx] = colours.get(tile[y * 8 + x])
    return out


def identify_bigcard(px, w, h, x0, y0):
    """Which card's painting fills the 112x112 window at (x0, y0), and how
    many of its pixels match exactly.  (face, matches) for the best face."""
    if x0 < 0 or y0 < 0 or x0 + BIG_ART > w or y0 + BIG_ART > h:
        return None
    obs = [screen5(px, w, x0 + x, y0 + y) for y in range(BIG_ART) for x in range(BIG_ART)]
    best = None
    for face in range(CARD_BACK + 1):
        want = bigcard_art_pixels(face)
        n = sum(a == b for a, b in zip(obs, want))
        if best is None or n > best[1]:
            best = (face, n)
    return best


def identify_card_sprite(px, w, h, x0, y0, hi=False, vflip=False, grey=False):
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
            obs.append(screen5(px, w, x0 + x, y0 + (31 - y if vflip else y)))
    best = None
    for face in range(len(group)):
        want = card_sprite_pixels(face, hi, grey)
        n = sum(1 for a, b in zip(obs, want) if a == b)
        if best is None or n > best[1]:
            best = (face, n)
    return best


def card_name(face):
    """What the ROM says a face is called, which is what the HUD must print."""
    blob = spr_asset(CARD_NAMES, "names")
    return blob[face * NAME_LEN:(face + 1) * NAME_LEN].split(b"\0")[0].decode()


def read_life_panel(px, w, h, x, side):
    """One life panel off the screen: its label, its number, and how much of
    its gauge is filled.

    The gauge is read as a COUNT OF LIT PIXELS in the side's own colour, which
    is the only thing a bar can be checked against -- not "there is something
    there" but a length that has to agree with the life points the rules hold.
    A tile of the bar carries its fill in colour 4 (the player) or 6 (the
    opponent) against colour 9, so the two sides cannot be confused for each
    other either."""
    label = read_sprite_line(px, w, h, x + LP_LABEL_DX, LP_Y, 3)
    number = read_sprite_line(px, w, h, x + LP_NUM_DX, LP_Y, 4)
    ink = obj_colour(7, 6 if side else 4)
    filled = 0
    for u in range(LP_BAR_W):
        if screen5(px, w, x + LP_BAR_DX + u, LP_Y + 2) == ink:
            filled += 1
    return label, number, filled


def check_hud_text():
    """The band says what the rules say.

    The life panels are decoded off the screenshot -- label, digits AND the
    length of the gauge -- and checked against the frame stamp, so a HUD that
    draws stale or wrong numbers fails here rather than looking plausible.  The
    name row is checked against the ROM's own name table: it either IS a card
    the game could be showing, or the message the last action left."""
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    w, h, px = read_ppm(ppm)

    for x, side, lp, who in ((LP_YOU_X, 0, stamp["lp_player"], "YOU"),
                             (LP_COM_X, 1, stamp["lp_com"], "COM")):
        label, number, filled = read_life_panel(px, w, h, x, side)
        if label != who:
            raise Failure("the %s panel is labelled %r" % (who, label))
        if number != "%04d" % lp:
            raise Failure("the %s panel reads %r, but the rules say %d"
                          % (who, number, lp))
        want = min(LP_BAR_W, lp * LP_BAR_W // LP_MAX)
        if abs(filled - want) > 1:
            raise Failure("the %s gauge is %d pixels long for %d life points, "
                          "where %d is the length that means %d"
                          % (who, filled, lp, want, lp))

    name = read_sprite_line(px, w, h, 8, NAME_Y, 15)
    stat = read_sprite_line(px, w, h, STAT_ATK_X, STAT_Y, 20)
    names = set(card_name(f) for f in range(CARD_BACK + 1))
    icons = (read_icon(px, w, h, STAT_ATK_X, STAT_Y),
             read_icon(px, w, h, STAT_DEF_X, STAT_Y))
    if UI[stamp["ui"]] == "HAND":
        if name not in names:
            raise Failure("the name row reads %r, which is not a card in the "
                          "ROM's name table" % name)
        # THE STAT ROW IS AN ICON AND A NUMBER, NOT A WORD.  The row either
        # carries the sword and the shield with a four-digit number after each,
        # or it is the button legend, which is what it falls back to when the
        # focused card has no stats to show.
        if icons == ("sword", "shield"):
            for kind, x in (("attack", STAT_ATK_X), ("defence", STAT_DEF_X)):
                digits = read_sprite_line(px, w, h, x + STAT_NUM_DX, STAT_Y, 4)
                if not digits.isdigit() or len(digits) != 4:
                    raise Failure("the %s icon is followed by %r, not four "
                                  "digits" % (kind, digits))
        elif not stat.startswith("A:PLAY"):
            raise Failure("the stat row shows icons %r and reads %r while the "
                          "UI is in HAND" % (icons, stat))

    # The plate the two rows sit on.  It is checked as a RAMP and not as "there
    # is blue there" -- every line at least as blue as the one below it, the top
    # brighter than the bottom, and blue the dominant channel throughout --
    # because a plate with the ramp upside down or in the wrong place still
    # passes any check that only counts colours.
    lines = [screen5(px, w, w - 3, y) for y in range(BAND_TOP, BAND_BOTTOM + 1)]
    for y, (r, g, b) in zip(range(BAND_TOP, BAND_BOTTOM + 1), lines):
        if b <= r or b <= g:
            raise Failure("line %d of the HUD plate is (%d,%d,%d), which is not "
                          "blue" % (y, r, g, b))
    for i in range(1, len(lines)):
        if sum(lines[i]) > sum(lines[i - 1]):
            raise Failure("the HUD plate brightens from line %d to %d -- the "
                          "gradient is not a ramp"
                          % (BAND_TOP + i - 1, BAND_TOP + i))
    if sum(lines[0]) <= sum(lines[-1]):
        raise Failure("the HUD plate is flat: %s to %s"
                      % (lines[0], lines[-1]))
    if screen5(px, w, w - 3, BAND_TOP - 1) != (0, 0, 0):
        raise Failure("line %d, above the plate, is not black -- the gradient "
                      "has grown into the hand" % (BAND_TOP - 1))
    if screen5(px, w, w - 3, 223) != (0, 0, 0):
        raise Failure("line 223, under the plate, is not black -- the HDMA "
                      "table stops before the last line and the register keeps "
                      "the ramp's final colour")

    # AND IT IS SMOOTH, which is the only thing that separates the backdrop
    # gradient from the painted one it replaced.  The bitmap plate could hold
    # three blues in two-line steps; a per-scanline fixed colour holds
    # twenty-odd in single lines.  Both of these fail on a plate that has gone
    # back into the bitmap even though it is still a correct ramp: the step
    # bound catches the four-level blue axis, the level count catches the
    # 2x2 sampling that doubles every step.
    ramp = lines[1:]
    steps = [ramp[i - 1][2] - ramp[i][2] for i in range(1, len(ramp))]
    if max(steps) > 2:
        raise Failure("the HUD plate steps %d levels of blue in one line -- "
                      "that is a banded gradient, not a smooth one" % max(steps))
    levels = len(set(b for _, _, b in ramp))
    if levels < 16:
        raise Failure("the HUD plate's ramp shows %d distinct blues over %d "
                      "lines -- it is quantised, not smooth"
                      % (levels, len(ramp)))
    return "%r / %r, name %r, icons %s, stats %r, plate %s..%s, %d blues, "\
           "max step %d" % (
        "YOU %d" % stamp["lp_player"], "COM %d" % stamp["lp_com"], name,
        "/".join(str(i) for i in icons), stat, lines[0], lines[-1],
        levels, max(steps))


def check_hand_is_per_face():
    """THE HAND DRAWS FROM THE PER-FACE SHEET AND THE TOP VIEW DOES NOT.

    Every hand card is identified twice -- once against the clustered sheet the
    top view uses, once against the sheet whose faces were each fitted a palette
    of their own -- and the per-face one has to win.  That is the only assertion
    that can tell the two apart: they are the same paintings, so a card matched
    loosely matches both, and the whole point of the change is which fifteen
    colours it was quantised through.

    It also catches the failure the change could actually produce on hardware --
    tiles from one sheet on screen against the other's palette -- because a
    mismatched pair matches NEITHER sheet exactly."""
    ppm, wram = run_still()
    stamp = read_stamp(wram)
    w, h, px = read_ppm(ppm)
    if UI[stamp["ui"]] != "HAND":
        raise Failure("the UI is in %s, so there is no hand to read"
                      % UI[stamp["ui"]])

    wins, seen = 0, []
    for i in range(5):
        x = HAND_X0 + i * HAND_PITCH
        dimmed = i != stamp["cursor"]
        # The focused hand card bobs by up to four pixels once per field.
        # Search that bounded animation offset, retaining the exact 1024-pixel
        # art requirement after locating the sprite.
        hi_candidates = [identify_card_sprite(px, w, h, x, HAND_Y + dy,
                                              hi=True, grey=dimmed)
                         for dy in range(-4, 5)]
        hi = max((v for v in hi_candidates if v is not None),
                 key=lambda v: v[1], default=None)
        lo_candidates = [identify_card_sprite(px, w, h, x, HAND_Y + dy,
                                              hi=False)
                         for dy in range(-4, 5)]
        lo = max((v for v in lo_candidates if v is not None),
                 key=lambda v: v[1], default=None)
        if hi is None or lo is None:
            continue
        if hi[1] < 1024 * 0.98:
            mode = "greyscale" if dimmed else "per-face"
            raise Failure("hand card %d matches its best %s sprite in only "
                          "%d of 1024 pixels -- the sheet and palette on "
                          "screen do not agree" % (i, mode, hi[1]))
        if not dimmed and hi[1] <= lo[1]:
            raise Failure("hand card %d matches the CLUSTERED sheet at least as "
                          "well (%d) as the per-face one (%d) -- the hand is "
                          "not using snes_spr_cards_hi" % (i, lo[1], hi[1]))
        wins += 1
        seen.append(card_name(hi[0]))
    if wins < 5:
        raise Failure("only %d of the five hand cards could be identified" % wins)
    return "5 hand cards exact against the per-face sheet: %s" % ", ".join(seen)


def hand_sprite_present(px, w, h, slot):
    """Whether ANY card sprite is drawn at hand slot `slot`: the best per-face
    match over the bob range, against the sheet, has to be nearly exact."""
    x = HAND_X0 + slot * HAND_PITCH
    best = 0
    for dy in range(-4, 5):
        for grey in (False, True):
            got = identify_card_sprite(px, w, h, x, HAND_Y + dy, hi=True, grey=grey)
            if got is not None and got[1] > best:
                best = got[1]
    return best >= 1024 * 0.9


def check_chosen_card_leaves_the_hand():
    """THE CARD BEING PLAYED IS NOT DRAWN IN THE HAND WHILE ITS SLOT IS CHOSEN.

    A on the first hand card enters PLACE: the card hovers over the board as
    the held card and its hand sprite must be gone.  B cancels: the same card
    is back in its slot, selected.  Both are read from the screen, not the
    stamp, and the other four hand cards must be present throughout."""
    ppm, wram = run_hold()
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "PLACE":
        raise Failure("A did not enter PLACE (ui %s)" % UI[stamp["ui"]])
    w, h, px = read_ppm(ppm)
    if hand_sprite_present(px, w, h, 0):
        raise Failure("the chosen hand card is still drawn in the hand while its "
                      "slot is being chosen")
    others = [hand_sprite_present(px, w, h, i) for i in range(1, 5)]
    if not all(others):
        raise Failure("the other hand cards are not all drawn during PLACE: %s" % others)
    ppm, wram = run("hold_cancel", random_battle_script() + [press("A", DUEL_READY),
                    press("RIGHT", DUEL_READY + 200), press("B", DUEL_READY + 400)],
                    RUN_FRAMES)
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "HAND" or stamp["cursor"] != 0:
        raise Failure("B did not put the card back (ui %s, cursor %d)"
                      % (UI[stamp["ui"]], stamp["cursor"]))
    w, h, px = read_ppm(ppm)
    present = [hand_sprite_present(px, w, h, i) for i in range(5)]
    if not all(present):
        raise Failure("after cancelling, hand cards present: %s" % present)
    return "chosen card hidden in PLACE, all five back after B"


def back_sprite_match(px, w, h, x0, y0, want=None):
    """How many of a card BACK's 1024 pixels are on screen at (x0, y0).

    Matched against the one face rather than the whole sheet, so a frame's
    worth of positions can be scanned: the first row rejects most of them."""
    if want is None:
        want = card_sprite_pixels(CARD_BACK, hi=True)
    if not (0 <= x0 and x0 + 32 <= w and 0 <= y0 and y0 + 32 <= h):
        return 0
    row0 = sum(1 for x in range(32) if screen5(px, w, x0 + x, y0) == want[x])
    if row0 < 20:
        return row0
    return sum(1 for y in range(32) for x in range(32)
               if screen5(px, w, x0 + x, y0 + y) == want[y * 32 + x])


def hand_backs(px, w, h, want):
    """Which hand slots hold a card back this frame (any bob or slide)."""
    out = set()
    for slot in range(5):
        x = HAND_X0 + slot * HAND_PITCH
        for dy in range(-4, 5):
            if back_sprite_match(px, w, h, x, HAND_Y + dy, want) >= 1024 * 0.9:
                out.add(slot)
                break
    return out


def hand_cursor_slot(px, w, h):
    """The hand slot the red selection bracket is around, if any."""
    red = obj_colour(7, 4)
    for slot in range(5):
        x = HAND_X0 + slot * HAND_PITCH
        for dy in range(-4, 5):
            y = HAND_Y + dy
            n = sum(1 for yy in range(y - 4, y + 4) for xx in range(x - 4, x + 4)
                    if 0 <= xx < w and 0 <= yy < h and screen5(px, w, xx, yy) == red)
            if n >= 6:
                return slot
    return None


def check_com_turn_presentation():
    """THE OPPONENT'S TURN IS PRESENTED, NOT JUST APPLIED.

    START hands the first turn over with nothing played.  Across the COM turn
    the lower edge shows the opponent's hand as CARD BACKS (never a face), the
    red cursor visits more than one of them before settling, and the chosen
    back LEAVES the row and is seen on its way to the board while the others
    stay.  THE CARD FLIES FACE UP (COM_FLY_SLOT in snes_duel.c) and the name
    row names it as it goes, so the flight is matched against the face the
    HUD announces.  Read from a capture of every other field, then from the
    stamp of a run that stops in the middle of the turn."""
    name = "com_turn"
    start, end, step = DUEL_READY + 100, DUEL_READY + 1500, 2
    run(name, random_battle_script() + [press("START", DUEL_READY)],
        DUEL_READY + 1600, capture=(start, end, step))
    frames = sorted(os.listdir(os.path.join(OUT, name + ".frames")))
    if len(frames) < 100:
        raise Failure("captured only %d fields of the COM turn" % len(frames))
    want = card_sprite_pixels(CARD_BACK, hi=True)
    group = spr_asset(SPR_GROUP, "group")
    names = dict((card_name(f), f) for f in range(CARD_BACK))
    seen_backs, cursor_slots, departures, flights = [], set(), 0, 0
    flown_name = ""
    departure_field = 0
    faces_shown = 0
    prev = set()
    most = 0
    for fi, fname in enumerate(frames):
        w, h, px = read_ppm(os.path.join(OUT, name + ".frames", fname))
        backs = hand_backs(px, w, h, want)
        most = max(most, len(backs))
        seen_backs.append(backs)
        if backs:
            slot = hand_cursor_slot(px, w, h)
            if slot is not None:
                cursor_slots.add(slot)
            # No face of any card in the opponent's hand row: a back is a
            # back, and the rest of the row is a back or nothing.
            for slot in range(5):
                if slot in backs:
                    continue
                got = identify_card_sprite(px, w, h, HAND_X0 + slot * HAND_PITCH,
                                           HAND_Y, hi=True)
                if got is not None and got[0] != CARD_BACK and got[1] >= 1024 * 0.9:
                    faces_shown += 1
        # A slot's back gone while the rest stayed is a departure; the card
        # is then somewhere between the row and the board.
        if prev and len(backs) == len(prev) - 1 and backs < prev:
            departures += 1
            departure_field = int(fname[1:7])
            gone = (prev - backs).pop()
            x0 = HAND_X0 + gone * HAND_PITCH
            found = False
            flown_name = read_sprite_line(px, w, h, 8, NAME_Y, NAME_LEN).strip()
            wants = [want]
            if flown_name in names:
                wants.insert(0, card_sprite_pixels(names[flown_name], hi=True))
            # The face replaces the back in the row on the departure field
            # itself; the flight is in the fields after it.
            for later in frames[fi:fi + 8]:
                _, _, lpx = read_ppm(os.path.join(OUT, name + ".frames", later))
                for y in range(40, HAND_Y - 2, 2):
                    for x in range(max(0, x0 - 160), min(w - 32, x0 + 160)):
                        if any(back_sprite_match(lpx, w, h, x, y, wt) >= 1024 * 0.8
                               for wt in wants):
                            found = True
                            break
                    if found:
                        break
                if found:
                    break
            if found:
                flights += 1
        prev = backs
    if most < 3:
        raise Failure("at most %d card backs were ever drawn in the opponent's "
                      "hand row" % most)
    if faces_shown:
        raise Failure("a card FACE was drawn in the opponent's hand row in %d "
                      "captured fields" % faces_shown)
    if len(cursor_slots) < 2:
        raise Failure("the cursor sat on hand slots %s during the opponent's turn "
                      "-- it does not sweep" % sorted(cursor_slots))
    if not departures:
        raise Failure("no card back ever left the opponent's hand row")
    if not flights:
        raise Failure("a back left the row but was never seen on its way to the "
                      "board (the name row said %r)" % flown_name)
    # The rules' side of it, read mid-flight: it IS the opponent's turn.
    _, wram = run(name + "_mid", random_battle_script() + [press("START", DUEL_READY)],
                  departure_field + 6)
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "COM" or stamp["turn_owner"] != 1:
        raise Failure("mid-turn the UI is %s with turn owner %d, expected the "
                      "opponent's turn" % (UI[stamp["ui"]], stamp["turn_owner"]))
    return "%d backs in the row, cursor on slots %s, %d departure(s), %d in flight (%s), no face in the row" % (
        most, sorted(cursor_slots), departures, flights, flown_name or "a back")


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
                      "you %d, com %d" % (14000, stamp["duel_turn"],
                                          stamp["lp_player"], stamp["lp_com"]))
    won = stamp["duel_result"] == 1
    loser_lp = stamp["lp_com"] if won else stamp["lp_player"]
    if loser_lp != 0:
        raise Failure("the duel is decided but the loser has %d life points"
                      % loser_lp)
    if stamp["duel_turn"] < 4:
        raise Failure("the duel ended on turn %d, which is not a duel"
                      % stamp["duel_turn"])
    # THE VERDICT IS GIVEN OVER THE BATTLE'S CARDS (snes_battle.c): a blow
    # that empties a life bar ends on the Mode 4 scene, its cards still up,
    # with the big banner slid in across their middle, and the duel waits
    # there for a press.  Only a deck-out (no blow) uses the board's banner.
    w, h, px = read_ppm(ppm)
    if UI[stamp["ui"]] == "BATTLE_ART":
        band_y = BATTLE_VERDICT_Y
        # The card art is still on the screen: the lanes are not black.
        lane = [screen5(px, w, x, y) for y in range(40, 80, 4)
                for x in list(range(8, 128, 8)) + list(range(136, 256, 8))]
        if sum(c != (0, 0, 0) for c in lane) < len(lane) // 3:
            raise Failure("the verdict is up but the battle's cards are gone")
    elif UI[stamp["ui"]] == "RESULT":
        band_y = OVER_Y
    else:
        raise Failure("the duel is decided but the screen is in %s"
                      % UI[stamp["ui"]])
    # The result prompt is a large animated OBJ banner: check the banner's
    # lit area and its colour instead of decoding the normal 8x8 font.
    banner = [screen5(px, w, x, y) for y in range(band_y, band_y + 16)
              for x in range(0, w)]
    want = obj_colour(7, 3 if won else 4)
    if sum(c == want for c in banner) < 20:
        raise Failure("the result banner has no %s ink at y=%d" %
                      ("gold" if won else "red", band_y))
    return "%s on turn %d, %d fields, large banner over the %s" % (
        "won" if won else "lost", stamp["duel_turn"], 14000,
        "battle" if UI[stamp["ui"]] == "BATTLE_ART" else "board")


def run_top(capture=None):
    """The fixture board, then UP into the tactical top view."""
    return run("topview", random_battle_script() + [press("R", DUEL_READY),
                            press("UP", DUEL_READY + 400)], 3800,
               capture=capture)


# The overhead camera, mirroring snes_duel.c's lift_camera at its top: over
# the middle of the board at LIFT_HEIGHT (4.0 units), looking straight down,
# the frame centred on the camera's foot at LIFT_HORIZON.  A world point
# (wx, wz) in the bitmap projects around y=72.  On entry row 2 is selected,
# so the PPU applies the -24-pixel tracking offset and that bitmap centre is
# displayed at y=96.  Moving between rows changes only BG1VOFS by 32 pixels.
TOP_HEIGHT = 1024 / 256.0
TOP_K = 128.0 / TOP_HEIGHT
TOP_CX, TOP_CY = 128.0, 96.0
# The moving camera draws the cards out of the world texture, where each is a
# 24x32-texel stamp of its 32x32 face over a 0.75 x 1.0 unit footprint.
TOP_CARD_W, TOP_CARD_H = 0.75, 1.0


def top_slot_samples(px, w, h, row, col, ox, oy, lo=4, hi=28):
    """A slot's card texels read out of the overhead picture.

    The overhead cards come off the same 32x32 sheets as the resting board
    (stamped into the world texture at 24x32 texels, three quarters of a unit
    by one), so the identification is against those sheets too."""
    cx, cz = col - 2, ROW_Z[row]
    flip = row < 2
    out = []
    for v in range(lo, hi):
        for u in range(lo, hi):
            vv = 31 - v if flip else v
            wx = cx - TOP_CARD_W / 2 + ((u + 0.5) / 32.0) * TOP_CARD_W
            wz = cz + TOP_CARD_H / 2 - ((vv + 0.5) / 32.0) * TOP_CARD_H
            x = int(TOP_CX + wx * TOP_K) + ox
            y = int(TOP_CY - wz * TOP_K) + oy
            if 0 <= x < w and 0 <= y < h:
                i = (y * w + x) * 3
                out.append((v * 32 + u,
                            direct_colour_byte((px[i], px[i + 1], px[i + 2]))))
    return out


def identify_top_slot(px, w, h, row, col):
    faces, weight = card_faces()
    best = None
    for ox in (-2, -1, 0, 1, 2):
        for oy in (-2, -1, 0, 1, 2):
            s = top_slot_samples(px, w, h, row, col, ox, oy)
            if len(s) < 60:
                continue
            scored = sorted(((sum(weight.get(b, 0.0) for i, b in s if f[i] == b),
                              sum(1 for i, b in s if f[i] == b), fi)
                             for fi, f in enumerate(faces)), reverse=True)
            if best is None or scored[0][0] > best[0]:
                best = (scored[0][0], scored[0][2], scored[1][0], scored[0][1], len(s))
    if best is None:
        return None
    return best[1], best[0], best[2], best[3], best[4]


def check_top_view():
    """UP walks up into the overhead view, and it is THE SAME BOARD.

    There is no resident table any more: the overhead view is the duel's
    renderer at the top of the camera lift, looking straight down, in Mode 3
    direct colour through the same character base and map publication as the
    seat.  The twenty slots are identified by projecting each card's texels
    through that camera and matching the 32x32 sheets the world texture is
    stamped from; the cursor is the red sprite bracket over the inspected
    slot; and a different field (the empty board without the fixture) must
    give a different picture, which a leftover static table could not."""
    ppm, wram = run_top(capture=(3798, 3799, 1))
    stamp = read_stamp(wram)
    if stamp["view"] != 1:
        raise Failure("UP did not settle in the overhead view (view %d)" % stamp["view"])
    regs = read_ppu_regs(os.path.join(OUT, "topview.ppu"))
    if regs["BGMODE"] != 3 or regs["BG12NBA"] != 0 or regs["BG1SC"] not in (0x58, 0x5c):
        raise Failure("the overhead view is not the seat's Mode 3 board: %s" % regs)
    if regs.get("TM") != 0x11 or regs.get("BG1VOFS") != 999:
        raise Failure("the overhead view is not full-height/tracked (TM=%s VOFS=%s)" %
                      (regs.get("TM"), regs.get("BG1VOFS")))
    w, h, px = read_ppm(ppm)
    found = []
    for row in range(4):
        for col in range(5):
            got = identify_top_slot(px, w, h, row, col)
            if got and decisive(got):
                found.append((row, col, got[0]))
    if len(found) < 14:
        raise Failure("only %d of the twenty slots are identified from above -- "
                      "the overhead view is not showing the field (%s)"
                      % (len(found), found))
    if len(set(r for r, _, _ in found)) != 4 or len(set(c for _, c, _ in found)) != 5:
        raise Failure("the identified cards cover rows %s and columns %s"
                      % (sorted(set(r for r, _, _ in found)),
                         sorted(set(c for _, c, _ in found))))
    if len(set(f for _, _, f in found)) < 4:
        raise Failure("the overhead view shows %d distinct faces" %
                      len(set(f for _, _, f in found)))
    below_old_clip = sum(px[(y * w + x) * 3:(y * w + x + 1) * 3] != b"\x00\x00\x00"
                         for y in range(145, 161) for x in range(48, 208))
    if below_old_clip < 300:
        raise Failure("the full-height overhead board has only %d lit pixels below "
                      "the chair view's old line-144 clip" % below_old_clip)
    # The cursor: four red corner brackets around the inspected slot, which on
    # entry is the player's monster row, column 0.  Assert the displayed ink,
    # not the post-field OAM dump: a dump can already contain the next shadow
    # while the PPM is the field that was just presented.
    cx, cz = -2, ROW_Z[2]
    x0, x1 = int(TOP_CX + (cx - 0.5) * TOP_K), int(TOP_CX + (cx + 0.5) * TOP_K)
    y0, y1 = int(TOP_CY - (cz + 0.5) * TOP_K), int(TOP_CY - (cz - 0.5) * TOP_K)
    red = obj_colour(7, 4)
    brackets = []
    for x, y in ((x0, y0), (x1 - 8, y0),
                 (x0, y1 - 8), (x1 - 8, y1 - 8)):
        brackets.append(sum(screen5(px, w, x + dx, y + dy) == red
                            for dy in range(8) for dx in range(8)))
    if min(brackets) < 6:
        raise Failure("cursor corners at %d,%d..%d,%d have red-pixel counts %s"
                      % (x0, y0, x1, y1, brackets))
    # The same view over an EMPTY board is a different picture.
    empty_ppm, empty_wram = run("topview_empty", random_battle_script() +
                                [press("UP", DUEL_READY + 400)], 3800)
    if read_stamp(empty_wram)["view"] != 1:
        raise Failure("UP on the empty board did not reach the overhead view")
    _, _, epx = read_ppm(empty_ppm)
    differ = sum(1 for i in range(0, 144 * w * 3, 3) if px[i:i + 3] != epx[i:i + 3])
    if differ < 2000:
        raise Failure("the overhead picture of a full board differs from an empty one in "
                      "only %d pixels" % differ)
    # The life panels do not move when the player walks up.
    label, number, filled = read_life_panel(px, w, h, LP_YOU_X, 0)
    if label != "YOU" or number != "%04d" % stamp["lp_player"]:
        raise Failure("the overhead view's life panel reads %r %r" % (label, number))
    return "%d of 20 slots identified through the overhead camera, %d faces, cursor bracketed, %d pixels differ from the empty board" % (
        len(found), len(set(f for _, _, f in found)), differ)


def check_top_view_switch_is_seamless():
    """THE LIFT IS RENDERED, COMPLETE FRAMES ONLY, AND NEVER BLANK.

    Every second field across the UP press is captured.  None may be blank
    (a mode change that blanked the screen, or a map switched before its
    tiles were up, would put a black or half-drawn field here), the pose must
    visibly change through several distinct pictures, and the last field must
    be the overhead view."""
    run_top(capture=(DUEL_READY + 300, DUEL_READY + 1300, 2))
    frames = sorted(os.listdir(os.path.join(OUT, "topview.frames")))
    if len(frames) < 40:
        raise Failure("captured %d fields across the switch, need at least 40"
                      % len(frames))
    lit, unique = [], set()
    for name in frames:
        w, h, px = read_ppm(os.path.join(OUT, "topview.frames", name))
        n = sum(1 for i in range(0, 144 * w * 3, 3 * 8)
                if px[i:i + 3] != b"\x00\x00\x00")
        lit.append(n)
        unique.add(px[:144 * w * 3])
    worst = min(lit)
    typical = sorted(lit)[len(lit) // 2]
    if worst < typical // 3:
        raise Failure("a field across the switch has %d lit samples against a "
                      "typical %d -- the lift blanked the board" % (worst, typical))
    if len(unique) < 5:
        raise Failure("the captured UP interval has only %d distinct board pictures -- "
                      "the camera lift is not animating" % len(unique))
    _, wram = run_top()
    stamp = read_stamp(wram)
    if stamp["view"] != 1:
        raise Failure("the lift did not end in the overhead view")
    # EVERY RENDERED POSE WAS SHOWN.  A pose whose span is over the converter's
    # 351-cell budget is refused and never reaches the screen; the lift's
    # trajectory is chosen so that none is, and the overhead pose fills the
    # frame as far as that budget allows.
    if stamp["dropped"]:
        raise Failure("the converter refused %d frames during the lift: a pose is "
                      "over the 351-cell budget" % stamp["dropped"])
    if stamp["occupied"] < 300:
        raise Failure("the overhead board occupies only %d cells -- it is not "
                      "filling the view" % stamp["occupied"])
    return "%d fields across the lift, %d distinct pictures, quietest %d lit samples against %d, no frame dropped, overhead occupies %d cells" % (
        len(frames), len(unique), worst, typical, stamp["occupied"])


def check_camera_round_trip():
    before, _ = run_fixture()
    _, _, reference = read_ppm(before)
    after, wram = run("camera_roundtrip", random_battle_script() + [press("R", DUEL_READY),
        press("UP", DUEL_READY + 400), press("DOWN", DUEL_READY + 1700),
        press("DOWN", DUEL_READY + 1900)], 5000)
    stamp = read_stamp(wram)
    _, _, pixels = read_ppm(after)
    # The top cursor starts on the player's monster row: one DOWN to the
    # support row, a second off the bottom is the way back down (B is the
    # card check up there, as in the hand).
    if stamp["view"] != 0 or UI[stamp["ui"]] != "HAND":
        raise Failure("UP/DOWN/DOWN did not return to the resting hand view (view %d, ui %s)"
                      % (stamp["view"], UI[stamp["ui"]]))
    # The slab ends above line 140.  The selected hand card's corner bracket
    # bobs as high as line 140, so including it compares unrelated phases.
    if pixels[24 * 256 * 3:140 * 256 * 3] != reference[24 * 256 * 3:140 * 256 * 3]:
        raise Failure("the board changed geometry or lost cards during UP/DOWN/DOWN")
    return "UP/DOWN/DOWN restores the original board pixels exactly"


def check_top_cursor_and_card_check():
    """The top view owns its cursor, and A/B enter and leave card check."""
    top_ppm, _ = run("top_cursor", random_battle_script() + [
        press("R", DUEL_READY), press("UP", DUEL_READY + 400),
        press("RIGHT", DUEL_READY + 1800),
        press("UP", DUEL_READY + 2000),
    ], 6000, capture=(5999, 5999, 1))
    tracked = read_ppu_regs(os.path.join(OUT, "top_cursor.ppu"))
    if tracked.get("BG1VOFS") != 967:
        raise Failure("moving the top cursor up did not tile-scroll the board "
                      "by one row (BG1VOFS=%s, expected 967)" %
                      tracked.get("BG1VOFS"))
    # B IS THE CARD CHECK ON EITHER SIDE OF THE TABLE (A on the player's own
    # monster picks it as the attacker, the PC-FX gesture); the cursor is
    # walked up to the opponent's row first, so the check is of their card.
    check_name = "top_check_mode3"
    check_ppm, top_wram = run(check_name, random_battle_script() + [
        press("R", DUEL_READY), press("UP", DUEL_READY + 400),
        press("UP", DUEL_READY + 1500), press("B", DUEL_READY + 1800),
    ], 6000, capture=(5600, 5999, 4))
    checked = read_stamp(top_wram)
    if UI[checked["ui"]] != "CHECK":
        raise Failure("B on the top cursor ended in %s, expected CHECK" %
                      UI[checked["ui"]])
    w, h, px = read_ppm(check_ppm)
    regs = read_ppu_regs(os.path.join(OUT, check_name + ".ppu"))
    if regs["BGMODE"] != 3:
        raise Failure("card check PPU BGMODE is %d, expected Mode 3" %
                      regs["BGMODE"])
    # Card check is the PC-FX battle card on Mode 3's BG1: its 112x112
    # painting at (8, 28).  Match it against every generated card; a
    # non-black count or a wrong card can pass otherwise.
    art = identify_bigcard(px, w, h, BIGCARD_CHECK_X + 4, BIGCARD_CHECK_Y + 6)
    if art is None or art[1] < BIG_ART * BIG_ART * 0.98:
        raise Failure("card check painting matches its card in only %d of %d "
                      "pixels" % (art[1] if art else 0, BIG_ART * BIG_ART))
    # ...and the PC-FX text column beside it says what it is.
    column = sum(1 for y in range(20, 60) for x in range(132, 252)
                 if px[(y * w + x) * 3:(y * w + x + 1) * 3] not in
                 (b"\x00\x00\x00", px[(10 * w + 200) * 3:(10 * w + 201) * 3]))
    if column < 200:
        raise Failure("the card check's text column has only %d lit pixels"
                      % column)
    check_ppm, check_wram = run("top_check_close", random_battle_script() + [
        press("R", DUEL_READY), press("UP", DUEL_READY + 400),
        press("UP", DUEL_READY + 1500), press("B", DUEL_READY + 1800),
        press("B", DUEL_READY + 2100),
    ], 7000, capture=(6999, 6999, 1))
    closed = read_stamp(check_wram)
    if UI[closed["ui"]] == "CHECK":
        raise Failure("B did not close the card check")
    regs = read_ppu_regs(os.path.join(OUT, "top_check_close.ppu"))
    if regs["BGMODE"] != 3:
        raise Failure("closing top-view inspection did not restore Mode 3")
    _, _, a = read_ppm(top_ppm)
    _, _, b = read_ppm(check_ppm)
    if a == b:
        raise Failure("moving the top cursor did not change the rendered cursor")
    return "top cursor moves independently; B opens CHECK and B closes it"


# The card-art screen shares the check layout: BG1 map $7400 and text map
# $7800, whose font starts at tile 192 for ASCII 32.
CARDART_BG2_MAP = 0x7800
CARDART_FONT_TILE = 192


def decode_cardart_text(vram, row):
    line = ""
    for col in range(32):
        word = struct.unpack_from("<H", vram,
                                  (CARDART_BG2_MAP + row * 32 + col) * 2)[0]
        tile = word & 0x3FF
        if CARDART_FONT_TILE <= tile < CARDART_FONT_TILE + 64:
            line += chr(32 + tile - CARDART_FONT_TILE)
        else:
            line += " "
    return line.rstrip()


def check_support_cutin():
    """Thunder names itself, then shows every snapshotted victim."""
    start = DUEL_READY + 400
    ppm, wram = run("support_cutin", random_battle_script() +
                    [press_chord(("SELECT", "X"), start)], start + 173,
                    capture=(start + 60, start + 172, 112))
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "EFFECT_ART":
        raise Failure("Thunder fixture ended in %s, expected EFFECT_ART" %
                      UI[stamp["ui"]])
    intro = os.path.join(OUT, "support_cutin.frames", "f%06d.ppm" %
                         (start + 60))
    iw, ih, ipx = read_ppm(intro)
    art = identify_bigcard(ipx, iw, ih, BIGCARD_CHECK_X + 4,
                           BIGCARD_CHECK_Y + 6)
    if not art or art[0] != SUPPORT_FIRST + 4 or art[1] < BIG_ART * BIG_ART * 0.98:
        raise Failure("support cut-in did not show Thunder exactly: %s" %
                      (art,))
    w, h, px = read_ppm(ppm)
    victim = identify_bigcard(px, w, h, BIGCARD_CHECK_X + 4,
                              BIGCARD_CHECK_Y + 6)
    if not victim or victim[0] >= SUPPORT_FIRST or victim[1] < BIG_ART * BIG_ART * 0.98:
        raise Failure("Thunder victim screen is not a full-resolution monster: %s" %
                      (victim,))
    vram = open(os.path.join(OUT, "support_cutin.ppu.vram"), "rb").read()
    if "THUNDER" not in decode_cardart_text(vram, 0) or \
       "DESTROYED" not in decode_cardart_text(vram, 8):
        raise Failure("Thunder victim text is missing: %r / %r" %
                      (decode_cardart_text(vram, 0),
                       decode_cardart_text(vram, 8)))
    return "Thunder support art, effect text, and victim %d shown at 120x160" % victim[0]


def check_fusion_cutin():
    """A legal two-card recipe merges through a flash into its large result."""
    start = DUEL_READY + 400
    ppm, wram = run("fusion_cutin", random_battle_script() +
                    [press_chord(("SELECT", "A"), start)], start + 121,
                    capture=(start + 120, start + 120, 1))
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "FUSION_ART":
        raise Failure("Fusion fixture ended in %s, expected FUSION_ART" %
                      UI[stamp["ui"]])
    w, h, px = read_ppm(ppm)
    result = identify_bigcard(px, w, h, BIGCARD_CHECK_X + 4,
                              BIGCARD_CHECK_Y + 6)
    if not result or result[0] >= SUPPORT_FIRST or result[1] < BIG_ART * BIG_ART * 0.98:
        raise Failure("Fusion result is not a full-resolution monster: %s" %
                      (result,))
    vram = open(os.path.join(OUT, "fusion_cutin.ppu.vram"), "rb").read()
    if "FUSION RESULT" not in decode_cardart_text(vram, 0) or \
       "FUSION SUMMON" not in decode_cardart_text(vram, 8):
        raise Failure("Fusion result text is missing: %r / %r" %
                      (decode_cardart_text(vram, 0),
                       decode_cardart_text(vram, 8)))
    return "two materials flash into full-resolution fusion result %d" % result[0]


# ── The Mode 4 battle ────────────────────────────────────────────────────────
#
# Mirroring src/snes/snes_battle.c: the player's lane at x 8..127, the
# opponent's at 136..255, cards 120x160 resting with their top on line 22,
# the gutter column 0..7 and the gap 128..135 always black.
LANE_X = (8, 136)
LANE_W, CARD_H, LANE_REST_Y = 120, 160, 22
BATTLE_FONT_FIRST = 32
BATTLE_BG2_MAP, BATTLE_LP_ROW, BATTLE_TEXT_ROW = 0x6800, 25, 23
FX_DIGIT_TILES = None


def battle_like(px, w, h):
    """Is this field the Mode 4 battle screen?  The board and the title both
    paint the bottom rows (the HUD plate, the menu); the battle leaves them
    black, and shows something above them."""
    for y in range(213, 223, 2):
        for x in (2, 5, 131, 133):
            # Black, or the whiteout's white: the flash adds the fixed colour
            # to the backdrop too.
            if screen5(px, w, x, y) not in ((0, 0, 0), (31, 31, 31)):
                return False
    return any(px[(y * w + x) * 3:(y * w + x + 1) * 3] != b"\x00\x00\x00"
               for y in range(4, 212, 4) for x in range(8, 256, 8))


def gold_at(px, w, x, y):
    r, g, bl = px[(y * w + x) * 3:(y * w + x + 1) * 3]
    return r > 150 and g > 100 and bl < 120


def lane_card_top(px, w, h, lane):
    """The first row of the card's gold rim in a lane, or None: both side
    rims gold for twelve rows, which no ring arc or ray can fake."""
    xl, xr = LANE_X[lane], LANE_X[lane] + LANE_W - 1
    run = 0
    for y in range(0, 224):
        if gold_at(px, w, xl, y) and gold_at(px, w, xr, y):
            run += 1
            if run == 12:
                return y - 11
        else:
            run = 0
    return None


def heat_pixels(px, w, h, x0, x1, y0, y1):
    """Bright warm pixels (the effects' whites, yellows and oranges) in a box."""
    n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            r, g, bl = px[(y * w + x) * 3:(y * w + x + 1) * 3]
            if r > 200 and g > 100 and bl < 120:
                n += 1
    return n


def battle_frames(name, script, first, last, step=1):
    """Run, capture the window, and return [(field, pixels)] for the fields
    that are the battle screen."""
    run(name, script, last + 1, capture=(first, last, step))
    frame_dir = os.path.join(OUT, name + ".frames")
    seq = []
    for f in sorted(os.listdir(frame_dir)):
        if not f.startswith("f"):
            continue
        w, h, px = read_ppm(os.path.join(frame_dir, f))
        seq.append((int(f[1:7]), px, battle_like(px, w, h)))
    # The field the board is force-blanked on is a torn one (HUD sprites over
    # black) that looks like the battle screen; only a run of battle fields
    # is the battle.
    out = []
    for i, (n, px, ok) in enumerate(seq):
        if ok and (i + 2 < len(seq) and seq[i + 1][2] and seq[i + 2][2] or
                   i >= 2 and seq[i - 1][2] and seq[i - 2][2]):
            out.append((n, px))
    return out


def decode_battle_text(vram, row):
    """One row of the battle's BG2 map as text (the 2bpp font's tile n is
    glyph n + 32)."""
    line = ""
    for col in range(32):
        word = struct.unpack_from("<H", vram, (BATTLE_BG2_MAP + row * 32 + col) * 2)[0]
        tile = word & 0x3FF
        line += chr(BATTLE_FONT_FIRST + tile) if 0 < tile < 64 else " "
    return line


def readout_digits(prefix):
    """The damage readout's digits, read from OAM: 8x8 sprites in the atlas's
    digit rows, ordered by x, one per 16-pixel column."""
    data = open(os.path.join(ROOT, "src/snes/snes_battle_data.h")).read()
    d0 = int(re.search(r"SNES_FX_DIGIT0\s+(\d+)", data)[1])
    d8 = int(re.search(r"SNES_FX_DIGIT_ROW2\s+(\d+)", data)[1])
    oam, oamhi = read_oam(prefix)
    cols = {}
    for i in range(128):
        x, y, t, a = oam_sprite(oam, oamhi, i)
        if y >= 224:
            continue
        tile = t | ((a & 1) << 8)
        for base, first in ((d0, 0), (d8, 8)):
            if base <= tile < base + 16 and (tile - base) % 2 == 0 and (tile - base) // 2 < 8:
                d = first + (tile - base) // 2
                if d < 10:
                    cols.setdefault(x, d)
    if not cols:
        return None
    return int("".join(str(cols[x]) for x in sorted(cols)))


_FX_DIGITS = None
_FX_DIGIT_PAL = None


def fx_digit_pixels(digit):
    """One 16x16 damage digit and its five-bit display palette."""
    global _FX_DIGITS, _FX_DIGIT_PAL
    if _FX_DIGITS is None:
        with open(os.path.join(ROOT, "src/snes/assets/snes_fx_tiles.bin"), "rb") as fh:
            tiles = fh.read()
        with open(os.path.join(ROOT, "src/snes/assets/snes_fx_pal.bin"), "rb") as fh:
            palette = fh.read()[32:64]  # second generated OBJ palette: digits
        _FX_DIGIT_PAL = []
        for i in range(16):
            word = palette[i * 2] | (palette[i * 2 + 1] << 8)
            _FX_DIGIT_PAL.append((word & 31, (word >> 5) & 31,
                                  (word >> 10) & 31))
        _FX_DIGITS = []
        for d in range(10):
            base = 272 + d * 2 if d < 8 else 304 + (d - 8) * 2
            image = [0] * 256
            for tile, tx, ty in ((base, 0, 0), (base + 1, 8, 0),
                                 (base + 16, 0, 8), (base + 17, 8, 8)):
                block = untile4(tiles, tile * 32)
                for y in range(8):
                    for x in range(8):
                        image[(ty + y) * 16 + tx + x] = block[y * 8 + x]
            _FX_DIGITS.append(image)
    return _FX_DIGITS[digit], _FX_DIGIT_PAL


def displayed_damage(ppm, value, cx, cy):
    """Read the expected damage number from the pixels actually presented."""
    w, h, px = read_ppm(ppm)
    text = str(min(value, 9999))
    x0, y0 = cx - len(text) * 8, cy - 8
    for column, ch in enumerate(text):
        digit, palette = fx_digit_pixels(int(ch))
        for y in range(16):
            for x in range(16):
                index = digit[y * 16 + x]
                if index and screen5(px, w, x0 + column * 16 + x,
                                     y0 + y) != palette[index]:
                    return None
    return value


def battle_script():
    return random_battle_script() + [
        press("R", DUEL_READY),
        # R leaves the duel on the first player turn, where attacks are
        # locked.  Hand the turn to COM and wait for it to return before
        # entering battle selection.
        press("START", DUEL_READY + 200),
        press("X", DUEL_READY + 2800),
        # The fixture can have an empty first slot after drawing real
        # monsters from the shuffled deck; select the next slot explicitly.
        press("RIGHT", DUEL_READY + 3200),
        press("A", DUEL_READY + 3600),
        press("A", DUEL_READY + 4200),
    ]


def check_battle_mode4():
    """AN ATTACK IS TWO CARDS IN VERTICAL LANES, A PIXEL A FIELD.

    Every field of the player's attack is captured.  The battle screen must
    be Mode 4 with the 32x64 BG1 map, the BG3 offset map and direct colour
    off; the player's card must enter DOWNWARD and the opponent's UPWARD in
    steps no larger than three pixels with single-pixel steps among them (an
    8-pixel horizontal offset-per-tile would jump); both paintings must be
    identified against the generated card sheet once settled; and the damage
    the sequencer shows must be the rules' own."""
    frames = battle_frames("battle_mode4", battle_script(), DUEL_READY + 4200,
                           DUEL_READY + 4460)
    if len(frames) < 60:
        raise Failure("only %d battle fields captured after the attack" % len(frames))
    first = frames[0][0]
    tops = [(f, lane_card_top(px, 256, 224, 0), lane_card_top(px, 256, 224, 1))
            for f, px in frames]
    # THE ENTRY.  The opponent's card comes up from below, so its top rim is
    # the first gold row for the whole climb: that row must fall
    # monotonically, in eased steps of at most sixteen pixels, through
    # positions that are not multiples of eight (offset-per-tile with a
    # HORIZONTAL entry would move in eight-pixel jumps), and rest on line 22.
    # The player's card comes down from above: what shows first is its
    # bottom rim descending, then its top rim settling on the same line.
    right = [t for _, _, t in tops if t is not None]
    if LANE_REST_Y not in right:
        raise Failure("the opponent's card never rested on line %d: %s" % (LANE_REST_Y, right[:30]))
    right = right[:right.index(LANE_REST_Y) + 1]
    if len(right) < 8:
        raise Failure("the opponent's card is visible in only %d entry fields: %s" % (len(right), right))
    r_steps = [b - a for a, b in zip(right, right[1:])]
    if any(d > 0 for d in r_steps):
        raise Failure("the opponent's card moved back down during its entry: %s" % right)
    if min(r_steps) < -20:
        raise Failure("the opponent's card jumped %d pixels in one field" % -min(r_steps))
    if sum(1 for y in right if (y - LANE_REST_Y) % 8) < 5:
        raise Failure("the opponent's entry touches only tile-aligned lines: %s" % right)
    if right[0] < 60:
        raise Failure("the opponent's card did not climb from below to line %d: %s"
                      % (LANE_REST_Y, right))
    left = [t for _, t, _ in tops if t is not None]
    if LANE_REST_Y not in left or 0 not in left or left.index(0) > left.index(LANE_REST_Y):
        raise Failure("the player's card did not descend from above to line %d: %s"
                      % (LANE_REST_Y, left[:30]))
    # The settled paintings.
    settled = [px for (f, t0, t1), (_, px) in zip(tops, frames)
               if t0 == LANE_REST_Y and t1 == LANE_REST_Y]
    if not settled:
        raise Failure("no field shows both cards at rest")
    px = settled[len(settled) // 4]
    ids = []
    for lane in (0, 1):
        card = identify_bigcard(px, 256, 224, LANE_X[lane] + 4, LANE_REST_Y + 6)
        if card is None or card[1] < BIG_ART * BIG_ART * 0.90:
            raise Failure("lane %d painting matches no generated card (%s)" % (lane, card))
        ids.append(card[0])
    # The registers and the stamp, from a run ending on a settled field.
    at = first + 26
    _, wram = run("battle_regs", battle_script(), at + 1, capture=(at, at, 1))
    regs = read_ppu_regs(os.path.join(OUT, "battle_regs.ppu"))
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "BATTLE_ART":
        raise Failure("field %d is in UI %s, not the battle" % (at, UI[stamp["ui"]]))
    if regs["BGMODE"] != 4 or regs["BG1SC"] != 0x62 or regs["BG3SC"] != 0x6c or \
       regs["BG2SC"] != 0x68 or regs["BG12NBA"] != 0x70:
        raise Failure("battle PPU registers are wrong: %s" % regs)
    if regs.get("CGWSEL", 0) & 1:
        raise Failure("direct colour is on over the indexed card art")
    vram = open(os.path.join(OUT, "battle_regs.ppu.vram"), "rb").read()
    text = decode_battle_text(vram, BATTLE_TEXT_ROW)
    if not re.search(r"A\d{4} D\d{4}", text):
        raise Failure("the figures row reads %r" % text)
    # The damage shown is the rules' damage, once: the life points at the end
    # of the sequence are the ones before it less exactly that.
    _, end_wram = run("battle_end", battle_script(), first + 200)
    end = read_stamp(end_wram)
    if UI[end["ui"]] == "BATTLE_ART":
        raise Failure("the battle had not ended %d fields after it began" % 200)
    if stamp["battle_damage"]:
        # The rules resolve the attack before the presentation begins, so
        # "before" is before the A press that declared it.
        before = read_stamp(run("battle_before", battle_script(), DUEL_READY + 4199)[1])
        lost = (before["lp_player"] - end["lp_player"]) + (before["lp_com"] - end["lp_com"])
        if lost != stamp["battle_damage"]:
            raise Failure("the battle showed %d damage but the life points moved by %d"
                          % (stamp["battle_damage"], lost))
    return ("Mode 4, BG1 32x64 at $6000, offsets at $6C00; %d fields, cards %s enter down/up in 1-3 px steps, "
            "figures %r, damage %d" % (len(frames), ids, text.strip(), stamp["battle_damage"]))


def direct_script():
    """Play nothing: the opponent's second turn attacks the empty field."""
    return random_battle_script() + [press("START", DUEL_READY),
                                     press("START", DUEL_READY + 1400),
                                     press("START", DUEL_READY + 2800)]


def check_direct_attack():
    """THE DIRECT ATTACK IS THE PC'S BEAT: blade, whiteout, burst, readout.

    The opponent's direct attack on an empty field is captured field by
    field.  Only the attacker's lane holds a card; the blade's bright sweep
    must appear before the whiteout, the burst (heat-coloured pixels over the
    empty lane) after it, and the damage readout later still; the readout's
    digits, read from OAM, must be the rules' damage, and the life points
    printed under it must count to the rules' value."""
    frames = battle_frames("direct", direct_script(), DUEL_READY + 1400,
                           DUEL_READY + 2200)
    if len(frames) < 80:
        raise Failure("only %d battle fields captured for the direct attack" % len(frames))
    first = frames[0][0]
    # Only the attacker: one lane has a card, the other never does.
    lanes = set()
    for f, px in frames:
        for lane in (0, 1):
            if lane_card_top(px, 256, 224, lane) is not None:
                lanes.add(lane)
    if len(lanes) != 1:
        raise Failure("the direct attack shows cards in lanes %s" % sorted(lanes))
    attacker = lanes.pop()
    target = attacker ^ 1
    tx0, tx1 = LANE_X[target], LANE_X[target] + LANE_W
    gap0, gap1 = (128, 136) if attacker == 0 else (128, 136)
    blade, white, burst, readout = None, None, None, None
    for f, px in frames:
        t = f - first
        heat_gap = heat_pixels(px, 256, 224, 128, 136, 30, 190)
        heat_lane = heat_pixels(px, 256, 224, tx0, tx1, 20, 200)
        whites = sum(1 for y in range(40, 180, 8)
                     if screen5(px, 256, LANE_X[attacker] + 60, y) == (31, 31, 31))
        digits = heat_pixels(px, 256, 224, tx0, tx1, 92, 110)
        # The blade is the bright sweep through the gap before the flash.
        if blade is None and white is None and heat_gap > 6:
            blade = t
        if white is None and whites >= 12:
            white = t
        if burst is None and white is not None and heat_lane > 120 and t > white:
            burst = t
        if readout is None and burst is not None and t > burst + 12 and digits > 8:
            readout = t
    if None in (blade, white, burst, readout):
        raise Failure("beats missing: blade %s, whiteout %s, burst %s, readout %s"
                      % (blade, white, burst, readout))
    if not (blade < white < burst < readout):
        raise Failure("beats out of order: blade %d, whiteout %d, burst %d, readout %d"
                      % (blade, white, burst, readout))
    # The readout's digits and the life points, at a field inside the beat.
    at = first + readout + 40
    reg_ppm, wram = run("direct_regs", direct_script(), at + 1,
                        capture=(at, at, 1))
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "BATTLE_ART" or not stamp["battle_damage"]:
        raise Failure("field %d is not inside the direct attack's readout (ui %s, damage %d)"
                      % (at, UI[stamp["ui"]], stamp["battle_damage"]))
    shown = readout_digits(os.path.join(OUT, "direct_regs.ppu"))
    if shown != stamp["battle_damage"]:
        shown = displayed_damage(reg_ppm, stamp["battle_damage"],
                                 LANE_X[target] + LANE_W // 2,
                                 LANE_REST_Y + 78)
    if shown != stamp["battle_damage"]:
        raise Failure("the readout shows %s, the rules' damage is %d" %
                      (shown, stamp["battle_damage"]))
    vram = open(os.path.join(OUT, "direct_regs.ppu.vram"), "rb").read()
    lp_text = decode_battle_text(vram, BATTLE_LP_ROW)
    m = re.search(r"LP\s*(\d{4})", lp_text)
    if not m:
        raise Failure("no life points printed under the readout: %r" % lp_text)
    lp_after = stamp["lp_player"] if target == 0 else stamp["lp_com"]
    if int(m[1]) != lp_after:
        raise Failure("the life points settle on %s, the rules say %d" % (m[1], lp_after))
    regs = read_ppu_regs(os.path.join(OUT, "direct_regs.ppu"))
    if regs["BGMODE"] != 4:
        raise Failure("the direct attack is not in Mode 4")
    return ("attacker lane %d only; blade at +%d, whiteout +%d, burst +%d, readout +%d; "
            "readout %d = damage, LP settles on %d" %
            (attacker, blade, white, burst, readout, shown, lp_after))


def check_fusion_target():
    """DOWN queues hand cards and A opens the field fusion target."""
    _, wram = run("fusion_target", random_battle_script() + [
        press("DOWN", DUEL_READY + 300),
        press("RIGHT", DUEL_READY + 420), press("DOWN", DUEL_READY + 540),
        press("A", DUEL_READY + 660), press("A", DUEL_READY + 960),
    ], 6000)
    stamp = read_stamp(wram)
    if UI[stamp["ui"]] != "HAND" or stamp["field_cards"] < 1:
        raise Failure("fusion did not complete (UI=%s cards=%d)" %
                      (UI[stamp["ui"]], stamp["field_cards"]))
    return "two hand cards queue with DOWN and A completes fusion"


def check_placement_lowering():
    """A placement lowers the held card onto its slot IN THE BITMAP: several
    distinct board pictures before the rules place it, every one of them
    changing only inside the board -- a sprite flying up from the hand row
    would change pixels below the board's 144 lines."""
    start = DUEL_READY
    _, wram = run("placement_lowering", random_battle_script() + [
        press("A", start + 300),
        press("A", start + 480),
    ], 5000, capture=(start + 480, start + 1100, 2))
    stamp = read_stamp(wram)
    if stamp["field_cards"] != 1 or UI[stamp["ui"]] == "PLACE":
        raise Failure("placement did not land (UI=%s cards=%d)" %
                      (UI[stamp["ui"]], stamp["field_cards"]))
    frames = sorted(os.listdir(os.path.join(OUT, "placement_lowering.frames")))
    if len(frames) < 4:
        raise Failure("captured only %d placement frames" % len(frames))
    pics = [read_ppm(os.path.join(OUT, "placement_lowering.frames", f))
            for f in frames]
    images = [p[2] for p in pics]
    if len(set(images)) < 3:
        raise Failure("placement capture has no visible movement")
    w = pics[0][0]
    below = 0
    for a, b in zip(images, images[1:]):
        if a == b:
            continue
        # The hand row and the text under it: 144..224.  A HUD change there
        # (the name line, the hand card leaving) is at most a few hundred
        # pixels; a 32x32 card sprite in flight is a thousand a frame.
        below += sum(1 for y in range(144, 224) for x in range(0, w, 2)
                     if a[(y * w + x) * 3:(y * w + x) * 3 + 3]
                     != b[(y * w + x) * 3:(y * w + x) * 3 + 3])
    if below > 600:
        raise Failure("%d pixels change under the board during the placement "
                      "-- a sprite is flying, not the 3D card lowering" % below)
    return ("%d distinct board pictures across %d fields, %d pixels changed "
            "under the board" % (len(set(images)), len(frames), below))


def planar_pixel(vram, tile, x, y):
    base = tile * 64
    return sum(((vram[base + (p // 2)*16 + y*2 + (p & 1)] >> (7-x)) & 1) << p
               for p in range(8))


def published_map(prefix):
    """The board map BG1SC names, as 1024 words, plus which map it is."""
    regs = read_ppu_regs(prefix)
    data = open(prefix + ".vram", "rb").read()
    base = (regs["BG1SC"] >> 2) << 10
    if base not in (0x5800, 0x5c00):
        raise Failure("BG1SC names map at word $%04x, not one of the two board maps" % base)
    words = struct.unpack_from("<1024H", data, base * 2)
    return regs, data, words, base


def check_converter_patterns():
    """Exercise both pair-LUT halves through the real CPU, converter and DMA.

    R+SELECT fills the frame with (x + y) & 255 over a 192x112 window: every
    byte value, every pixel position and both halves of the pair tables.  The
    tiles are read back through the map the PPU is actually showing, so this
    also proves the publication: every visible cell names a tile whose bytes
    are exactly the pattern's, and every cell outside the window is the blank
    tile 0."""
    combo = PAD["R"] | PAD["SELECT"]
    script = random_battle_script() + [(2200, 2260, combo)]
    name = "pattern_rest"
    ppm, wram = run(name, script, 3200, capture=(3198, 3199, 1))
    prefix = os.path.join(OUT, name + ".ppu")
    regs, data, words, base = published_map(prefix)
    if regs["BGMODE"] != 3 or regs["BG12NBA"] != 0:
        raise Failure("pattern has wrong Mode 3 board registers: %s" % regs)
    stamp = read_stamp(wram)
    if stamp["occupied"] != 24 * 14:
        raise Failure("the pattern frame occupies %d cells, expected %d" %
                      (stamp["occupied"], 24 * 14))
    for y in range(144):
        for x in range(256):
            cell = (y // 8) * 32 + x // 8
            word = words[cell]
            inside = 16 <= y < 128 and 32 <= x < 224
            if not inside:
                if word & 1023:
                    raise Failure("cell %d outside the pattern names tile %d, not the blank" %
                                  (cell, word & 1023))
                continue
            if word & 0x1c00 != 0x1c00:
                raise Failure("direct-colour attributes missing at %d,%d" % (x, y))
            got = planar_pixel(data, word & 1023, x & 7, y & 7)
            want = (x + y) & 255
            if got != want:
                raise Failure("converter at %d,%d: %d != %d (tile %d)" %
                              (x, y, got, want, word & 1023))
    w, h, px = read_ppm(ppm)
    # And on screen: the pattern at 1:1, no doubling in either axis.
    for y in (20, 63, 100):
        for x in range(40, 200):
            a = screen5(px, w, x, y)
            b = screen5(px, w, x + 1, y)
            if a == b:
                raise Failure("pattern pixels %d and %d on row %d are equal: not 1:1" %
                              (x, x + 1, y))
    return "336 pattern cells exact through the published map; both pair halves, all positions, 1:1 on screen"


def check_video_budget():
    """The sparse presenter's layout: 704 tiles, two maps, the OBJ sheet,
    exactly 64 KB; the frame and the allocator where the design puts them;
    and no trace of the retired 128x72 path in the production sources."""
    root = os.path.join(ROOT, "src/snes")
    video = open(os.path.join(root, "snes_video.h")).read()
    expected = {"BOARD_CHARS": 0, "BOARD_MAP_A": 0x5800, "BOARD_MAP_B": 0x5c00,
                "OBJ": 0x6000}
    for key, value in expected.items():
        m = re.search(r"#define SNES_VRAM_" + key + r"\s+(0x[0-9A-Fa-f]+)u?", video)
        if not m or int(m[1], 16) != value:
            raise Failure("unexpected VRAM layout for " + key)
    if 704 * 32 > 0x5800 or 0x5c00 + 1024 > 0x6000 or 0x6000 + 8192 > 0x8000:
        raise Failure("VRAM regions overlap")
    m = re.search(r"#define SNES_SPARSE_TILES\s+(\d+)", video)
    if not m or int(m[1]) != 351 or 2 * 351 + 1 > 704:
        raise Failure("the sparse tile budget is not two disjoint 351-cell frames")
    # Mode 7 multiplier registers are intentionally used by foreground math;
    # the ISR must never change them, and DMA scratch channels must stay idle.
    fb = open(os.path.join(root, "snes_fb.asm")).read()
    nmi = fb[fb.index("snesFbNmi:"):]
    if re.search(r"\$211[B-Cb-c]", nmi):
        raise Failure("NMI interferes with the foreground PPU multiplier")
    text = open(os.path.join(root, "snes_video.c")).read()
    if re.search(r"0x43[123][0-9A-Fa-f]", text) or "BG_MODE7" in text:
        raise Failure("video uses reserved renderer DMA channels or Mode 7")
    for name in ("snes_video.h", "snes_video.c", "snes_duel.c", "snes_fb.asm",
                 "snes_conv_drivers.inc", "snes_board3d.c"):
        src = open(os.path.join(root, name)).read()
        for word in ("SNES_MOVING_W", "snes_motion_fb", "snes_fb_doubler",
                     "snesConvMotion", "SNES_VRAM_TOP_MAP", "snesVideoSetView"):
            if word in src:
                raise Failure("%s still names the retired motion path: %s" % (name, word))
    sym = open(os.path.join(ROOT, "build/snes/waifusnes.sym")).read()
    for label, addr in (("snes_board_texture", "007f0000"), ("snes_frame_fb", "007e7000"),
                        ("snes_map_shadow_a", "007f8000"), ("snes_map_shadow_b", "007f8800"),
                        ("snes_ring", "00000400")):
        if not re.search(r"^%s %s$" % (addr, label), sym, re.M):
            raise Failure("%s is not at $%s in the link map" % (label, addr))
    return ("VRAM fits 64 KB (704 tiles + 2 maps + OBJ); frame at $7E7000, maps and "
            "allocator at $7F8000; no 128x72 path left; NMI multiplier ownership checked")


CHECKS = [
    ("cartridge", check_cartridge),
    ("retail rom", check_retail_rom),
    ("video budgets", check_video_budget),
    ("converter exactness", check_converter_patterns),
    ("title art", check_title),
    ("title scanlines", check_title_scanlines),
    ("title input", check_title_input),
    ("deck editor + SRAM", check_deck_editor),
    ("story scene", check_story_scene),
    ("ending scene", check_ending_scene),
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
    ("hand per-face art", check_hand_is_per_face),
    ("chosen card leaves hand", check_chosen_card_leaves_the_hand),
    ("top view", check_top_view),
    ("top view switch", check_top_view_switch_is_seamless),
    ("camera round trip", check_camera_round_trip),
    ("top cursor + card check", check_top_cursor_and_card_check),
    ("support cut-in", check_support_cutin),
    ("fusion cut-in", check_fusion_cutin),
    ("battle Mode 4", check_battle_mode4),
    ("direct attack", check_direct_attack),
    ("fusion target", check_fusion_target),
    ("placement lowering", check_placement_lowering),
    ("duel flow", check_duel_flow),
    ("COM turn presentation", check_com_turn_presentation),
    ("duel plays out", check_duel_plays_out),
    ("render cost", check_render_cost),
]


def main():
    global ROM, MEDNAFEN, OUT, TIMEOUT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", default=ROM, help="candidate ROM")
    parser.add_argument("--emulator", default=MEDNAFEN, help="headless emulator")
    parser.add_argument("--output", default=OUT, help="verification output directory")
    parser.add_argument("--timeout", type=int, default=TIMEOUT,
                        help="per-emulator-run timeout in seconds")
    parser.add_argument("--checks", help="comma-separated check names")
    args = parser.parse_args()
    ROM = os.path.abspath(args.rom)
    MEDNAFEN = os.path.abspath(args.emulator)
    OUT = os.path.abspath(args.output)
    TIMEOUT = args.timeout
    selected = None
    if args.checks:
        selected = {name.strip() for name in args.checks.split(",") if name.strip()}
        known = {name for name, _fn in CHECKS}
        unknown = selected - known
        if not selected or unknown:
            parser.error("unknown or empty --checks selection: %s" %
                         ", ".join(sorted(unknown or {"<empty>"})))
    try:
        check_rom_fresh()
    except Failure as exc:
        print("FAIL  freshness: %s" % exc)
        return 1
    failures = 0
    for name, fn in CHECKS:
        if selected is not None and name not in selected:
            continue
        try:
            print("ok    %-20s %s" % (name, fn()), flush=True)
        except Failure as exc:
            print("FAIL  %-20s %s" % (name, exc), flush=True)
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
