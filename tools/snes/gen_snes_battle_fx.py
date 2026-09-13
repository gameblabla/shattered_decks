#!/usr/bin/env python3
"""The battle presentation's own assets (SUPERNES_ISSUES_PLAN.md S5/S6).

Everything the Mode 4 battle screen needs beyond the big card sheets, in one
bank ($CF), deterministic and previewable:

  * the OBJ effects atlas -- 4bpp sprite tiles laid out sixteen wide the way
    OBJ name tables are read, so a 32x32 sprite is a 4x4 block: the impact's
    hot core in three sizes, the blade's head and tail, eight ring arcs (four
    orientations, the rest by flips), the rays, the sparks and the 16x16
    damage digits.  The impact effect on the PC/SDL3 build is a shader
    (src/platform/sdl3/shaders/impact.frag) that draws discs, a ring and 32
    spokes every frame; this machine cannot scale a sprite, so each size that
    matters is a resident variant and the "expansion" is pieces moving apart.
  * two OBJ palettes: the burst's heat bands, which snes_battle.c cools by
    rewriting the palette as the beat decays, and the digits' gold and white.
  * the 2bpp battle font: the dialogue font's glyphs (white ink, black
    outline) cut for Mode 4's 2bpp BG2, whose four-colour palette groups
    happen to match the big card frame palette's white (CGRAM 1..2) and gold
    (17..18) entries, so the text needs no CGRAM of its own.

Run by Makefile.snes; writes src/snes/assets/snes_battlefx.asm, its blobs, a
preview PNG and src/snes/snes_battle_data.h.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image  # noqa: E402
import gen_snes_scenes as scenes  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ASSETS = os.path.join(ROOT, "src", "snes", "assets")
BANK = 15                                  # $CF: free

ATLAS_W = 16                               # tiles across, as OBJ names are read
ATLAS_ROWS = 24
ATLAS_TILES = ATLAS_W * ATLAS_ROWS

# ── Palettes ─────────────────────────────────────────────────────────────────
# OBJ palette 1 (CGRAM 144..159) is the burst; 7 (240..255) the readout.  The
# card presentation leaves exactly those two free (snes_cardart.h).
BURST_PAL = 1
DIGIT_PAL = 7
# The burst's heat bands, hot to cold, as the indices 3..8 read them at full
# intensity.  Cooling shifts the window down the ramp; the ramp is long enough
# for two steps of cooling past the coldest band.
HEAT_RAMP = [
    (31, 31, 31),   # white
    (31, 31, 22),   # pale yellow
    (31, 28, 8),    # yellow
    (31, 20, 2),    # orange
    (30, 11, 1),    # red-orange
    (24, 4, 1),     # red
    (16, 2, 2),     # dark red
    (9, 1, 3),      # ember
    (4, 0, 2),      # dying
    (0, 0, 0),
]
HEAT_BANDS = 6                             # indices 3..8
BURST_PALETTE = [(0, 0, 0), (31, 31, 31), (0, 0, 0)] + HEAT_RAMP[:HEAT_BANDS] + \
    [(31, 27, 9), (20, 20, 24), (12, 12, 14), (31, 31, 31), (31, 31, 31), (31, 31, 31), (31, 31, 31)]
DIGIT_PALETTE = [(0, 0, 0), (31, 27, 9), (0, 0, 0), (31, 31, 31)] + [(0, 0, 0)] * 12


def palette_bytes(colours):
    out = bytearray()
    for r, g, b in colours:
        w = (r & 31) | ((g & 31) << 5) | ((b & 31) << 10)
        out += bytes((w & 0xFF, w >> 8))
    return bytes(out)


# ── Drawing ──────────────────────────────────────────────────────────────────

class Canvas:
    """A palette-indexed sprite canvas; 0 is transparent."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [0] * (w * h)

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = c

    def disc(self, cx, cy, r, c):
        for y in range(self.h):
            for x in range(self.w):
                dx, dy = x + 0.5 - cx, y + 0.5 - cy
                if dx * dx + dy * dy <= r * r:
                    self.put(x, y, c)

    def ring_arc(self, cx, cy, r_in, r_out, a0, a1, colour_of):
        """The band r_in..r_out between angles a0..a1 (turns), colour by
        radius fraction."""
        for y in range(self.h):
            for x in range(self.w):
                dx, dy = x + 0.5 - cx, y + 0.5 - cy
                d = math.hypot(dx, dy)
                if not (r_in <= d < r_out):
                    continue
                a = (math.atan2(-dy, dx) / (2 * math.pi)) % 1.0
                if a0 <= a1:
                    inside = a0 <= a < a1
                else:
                    inside = a >= a0 or a < a1
                if inside:
                    self.put(x, y, colour_of((d - r_in) / max(1e-6, r_out - r_in)))

    def line(self, x0, y0, x1, y1, c, thick=1):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / max(1, n)
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            for dy in range(-thick // 2, thick // 2 + 1):
                for dx in range(-thick // 2, thick // 2 + 1):
                    self.put(int(x + dx), int(y + dy), c)


def core_sprite(radius):
    """A hot core: white centre, then the hottest bands outward."""
    c = Canvas(32, 32)
    bands = [3 + i for i in range(HEAT_BANDS)]
    steps = 4
    for i in range(steps, 0, -1):
        r = radius * i / steps
        colour = bands[min(HEAT_BANDS - 1, steps - i)]
        c.disc(16, 16, r, colour)
    if radius > 6:
        c.disc(16, 16, radius * 0.3, 1)
    return c


def arc_sprite(radius, thickness, orientation, thin=False):
    """One eighth of a ring centred `radius` away from the piece's centre, so
    the piece placed at (cx + radius * cos, cy + radius * sin) shows the arc.
    Orientation 0..3 covers the right, upper-right, top and upper-left
    eighths; the other four are flips."""
    c = Canvas(32, 32)
    a = orientation * 0.125
    cx = 16 - radius * math.cos(a * 2 * math.pi)
    cy = 16 + radius * math.sin(a * 2 * math.pi)
    a0, a1 = (a - 0.0625) % 1.0, (a + 0.0625) % 1.0
    inner = 3 if thin else 3
    outer = 3 + (2 if thin else 4)

    def colour_of(f):
        return inner + (0 if f < 0.35 else (1 if f < 0.7 else 2)) if not thin else outer

    c.ring_arc(cx, cy, radius - thickness / 2, radius + thickness / 2, a0, a1,
               lambda f: 3 + (0 if 0.3 < f < 0.7 else 1) + (1 if thin else 0))
    return c


def ray_sprite(angle_turn, length=30):
    """A spoke from the centre outward, thick at the root, tapering."""
    c = Canvas(32, 32)
    dx, dy = math.cos(angle_turn * 2 * math.pi), -math.sin(angle_turn * 2 * math.pi)
    c.line(16 - dx * length / 2, 16 - dy * length / 2, 16 + dx * length / 2,
           16 + dy * length / 2, 6, 3)
    c.line(16 - dx * length / 2, 16 - dy * length / 2, 16 + dx * length / 6,
           16 + dy * length / 6, 4, 3)
    c.line(16 - dx * length / 2, 16 - dy * length / 2, 16, 16, 3, 1)
    return c


def blade_sprite(head):
    """The blade: a bright vertical sweep with a hot edge (head) or its
    cooling tail."""
    c = Canvas(32, 32)
    for y in range(32):
        w = 10 if head else 6
        f = y / 31.0
        half = int(w * (1.0 - 0.5 * abs(f - 0.5) * 2))
        for x in range(16 - half, 16 + half):
            d = abs(x + 0.5 - 16) / max(1, half)
            if head:
                colour = 1 if d < 0.3 else (3 if d < 0.6 else 4)
            else:
                colour = 4 if d < 0.4 else 6
            c.put(x, y, colour)
    return c


def spark_sprite(variant):
    c = Canvas(8, 8)
    if variant == 0:
        c.disc(4, 4, 2.2, 3); c.put(3, 3, 1); c.put(4, 3, 1)
    elif variant == 1:
        c.disc(4, 4, 1.6, 4)
    elif variant == 2:
        c.put(3, 3, 5); c.put(4, 3, 5); c.put(3, 4, 5); c.put(4, 4, 5); c.put(4, 2, 6)
    else:
        c.line(1, 6, 6, 1, 6, 1); c.put(3, 3, 3); c.put(4, 4, 3)
    return c


def digit_sprite(d):
    """A 16x16 digit: the scene font's glyph doubled, gold ink (1) with a
    black outline (2) -- the readout's settled look.  Index 3 is white for
    the punch-in frames, selected by palette."""
    rows = scenes.scene_font_rows()
    g = ord("0") + d
    src = rows[g * 8:(g + 1) * 8]
    c = Canvas(16, 16)
    for y in range(8):
        for x in range(8):
            if src[y] & (0x80 >> x):
                for yy in (2 * y, 2 * y + 1):
                    for xx in (2 * x, 2 * x + 1):
                        c.put(xx, yy, 1)
    out = Canvas(16, 16)
    out.px = list(c.px)
    for y in range(16):
        for x in range(16):
            if c.px[y * 16 + x] == 1:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < 16 and 0 <= yy < 16 and out.px[yy * 16 + xx] == 0:
                            out.px[yy * 16 + xx] = 2
    return out


# ── Tile packing ─────────────────────────────────────────────────────────────

def tile4(px):
    """One 8x8 block of indices as an SNES 4bpp tile (32 bytes)."""
    out = bytearray(32)
    for y in range(8):
        for plane in range(4):
            byte = 0
            for x in range(8):
                byte |= ((px[y * 8 + x] >> plane) & 1) << (7 - x)
            off = (plane // 2) * 16 + y * 2 + (plane & 1)
            out[off] = byte
    return bytes(out)


def tile2(px):
    """One 8x8 block as a 2bpp tile (16 bytes)."""
    out = bytearray(16)
    for y in range(8):
        for plane in range(2):
            byte = 0
            for x in range(8):
                byte |= ((px[y * 8 + x] >> plane) & 1) << (7 - x)
            out[y * 2 + plane] = byte
    return bytes(out)


class Atlas:
    def __init__(self):
        self.px = [0] * (ATLAS_W * 8 * ATLAS_ROWS * 8)
        self.names = {}

    def blit(self, canvas, col, row, name):
        """Place a canvas at tile (col, row); record its OBJ name."""
        for y in range(canvas.h):
            for x in range(canvas.w):
                self.px[(row * 8 + y) * ATLAS_W * 8 + col * 8 + x] = canvas.px[y * canvas.w + x]
        self.names[name] = row * ATLAS_W + col

    def tiles(self):
        out = bytearray()
        for ty in range(ATLAS_ROWS):
            for tx in range(ATLAS_W):
                block = [self.px[(ty * 8 + y) * ATLAS_W * 8 + tx * 8 + x]
                         for y in range(8) for x in range(8)]
                out += tile4(block)
        return bytes(out)

    def preview(self, path):
        img = Image.new("RGB", (ATLAS_W * 8, ATLAS_ROWS * 8), (40, 40, 60))
        p = img.load()
        for y in range(ATLAS_ROWS * 8):
            for x in range(ATLAS_W * 8):
                c = self.px[y * ATLAS_W * 8 + x]
                if c:
                    r, g, b = BURST_PALETTE[c] if y < 17 * 8 else DIGIT_PALETTE[c]
                    p[x, y] = (r * 8, g * 8, b * 8)
        img.resize((ATLAS_W * 16, ATLAS_ROWS * 16), Image.NEAREST).save(path)


def build_atlas():
    a = Atlas()
    # Rows 0-3: cores and the blade head.
    a.blit(core_sprite(6), 0, 0, "CORE_S")
    a.blit(core_sprite(11), 4, 0, "CORE_M")
    a.blit(core_sprite(15.5), 8, 0, "CORE_L")
    a.blit(blade_sprite(True), 12, 0, "BLADE_HEAD")
    # Rows 4-7: the medium ring's arcs, four orientations.
    for o in range(4):
        a.blit(arc_sprite(40, 7, o), o * 4, 4, "ARC%d" % o)
    # Rows 8-11: rays (0, 1/8, 1/4, 3/8 turn) -- the rest are flips.
    for o in range(4):
        a.blit(ray_sprite(o * 0.125), o * 4, 8, "RAY%d" % o)
    # Rows 12-15: the large, thin ring's arcs, and the blade tail.
    for o in range(3):
        a.blit(arc_sprite(80, 4, o, thin=True), o * 4, 12, "THIN%d" % o)
    a.blit(blade_sprite(False), 12, 12, "BLADE_TAIL")
    # Row 16: sparks and a flash dot.
    for v in range(4):
        a.blit(spark_sprite(v), v, 16, "SPARK%d" % v)
    # Rows 17-20: digits as 2x2 tiles, eight per pair of rows.
    for d in range(10):
        row = 17 + (d // 8) * 2
        col = (d % 8) * 2
        a.blit(digit_sprite(d), col, row, "DIGIT%d" % d)
    return a


def battle_font():
    """The dialogue font's 64 glyphs as 2bpp tiles: 1 ink, 2 outline."""
    rows = scenes.scene_font_rows()
    blob = bytearray()
    for glyph in range(scenes.SCENE_FONT_GLYPH_COUNT):
        src = rows[(scenes.SCENE_FONT_GLYPH_FIRST + glyph) * 8:
                   (scenes.SCENE_FONT_GLYPH_FIRST + glyph) * 8 + 8]
        px = [0] * 64
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            xx, yy = x + dx, y + dy
                            if 0 <= xx < 8 and 0 <= yy < 8 and px[yy * 8 + xx] == 0:
                                px[yy * 8 + xx] = 2
        for y in range(8):
            for x in range(8):
                if src[y] & (0x80 >> x):
                    px[y * 8 + x] = 1
        blob += tile2(px)
    return bytes(blob)


# ── The timeline tables ──────────────────────────────────────────────────────

def ease_out_cubic(u):
    inv = 1.0 - u
    return 1.0 - inv * inv * inv


def smoothstep(u):
    u = max(0.0, min(1.0, u))
    return u * u * (3 - 2 * u)


FX_FIELDS = 72          # the PC's beat length
RING_R0, RING_R1 = 10, 118


def ring_radius(t):
    u = max(0.0, min(1.0, (t - 8) / 56.0))
    r = RING_R0 + (RING_R1 - RING_R0) * ease_out_cubic(u)
    decay = smoothstep((u - 0.72) / 0.28) if u > 0.72 else 0.0
    return int(r - 34 * decay)


def core_size(t):
    u = max(0.0, min(1.0, (t - 8) / 56.0))
    if u < 0.32:
        r = 6 + (44 - 6) * smoothstep(u / 0.32)
    else:
        r = 44 * (1.0 - smoothstep((u - 0.32) / 0.42))
    return int(r)


def spark_table(n=12, seed=7):
    """Deterministic spark launch velocities, Q4.4 pixels a field."""
    out = []
    x = seed
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        a = (x >> 8) % 360
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        speed = 2.0 + ((x >> 8) % 100) / 40.0
        vx = int(round(math.cos(math.radians(a)) * speed * 16))
        vy = int(round(-math.sin(math.radians(a)) * speed * 16)) - 8
        out.append((vx & 0xFFFF, vy & 0xFFFF, i % 4))
    return out


def main():
    os.makedirs(ASSETS, exist_ok=True)
    atlas = build_atlas()
    tiles = atlas.tiles()
    font = battle_font()
    pal = palette_bytes(BURST_PALETTE) + palette_bytes(DIGIT_PALETTE)
    assert len(tiles) == ATLAS_TILES * 32 <= 16384
    assert len(font) == 1024
    with open(os.path.join(ASSETS, "snes_fx_tiles.bin"), "wb") as fh:
        fh.write(tiles)
    with open(os.path.join(ASSETS, "snes_fx_pal.bin"), "wb") as fh:
        fh.write(pal)
    with open(os.path.join(ASSETS, "snes_battle_font.bin"), "wb") as fh:
        fh.write(font)
    atlas.preview(os.path.join(ASSETS, "battlefx_preview.png"))
    with open(os.path.join(ASSETS, "snes_battlefx.asm"), "w") as fh:
        fh.write("""; Generated by tools/snes/gen_snes_battle_fx.py; do not edit.
.include "hdr.asm"
.BASE $C0
.SECTION "snes_battlefx" BANK %d SLOT 0 ORG $0000 FORCE

snes_fx_tiles:
    .INCBIN "snes_fx_tiles.bin"
snes_fx_pal:
    .INCBIN "snes_fx_pal.bin"
snes_battle_font:
    .INCBIN "snes_battle_font.bin"

.ENDS
""" % BANK)

    rings = [ring_radius(t) for t in range(FX_FIELDS)]
    cores = [core_size(t) for t in range(FX_FIELDS)]
    sparks = spark_table()
    names = atlas.names
    with open(os.path.join(ROOT, "src", "snes", "snes_battle_data.h"), "w") as fh:
        fh.write("/* Generated by tools/snes/gen_snes_battle_fx.py; do not edit. */\n")
        fh.write("#ifndef WAIFU_SNES_BATTLE_DATA_H\n#define WAIFU_SNES_BATTLE_DATA_H\n\n")
        fh.write('#include "snes_types.h"\n\n')
        fh.write("extern const u8 snes_fx_tiles[];\nextern const u8 snes_fx_pal[];\n"
                 "extern const u8 snes_battle_font[];\n\n")
        fh.write("#define SNES_FX_TILE_BYTES   %d\n" % len(tiles))
        fh.write("#define SNES_FX_PAL_BYTES    %d\n" % len(pal))
        fh.write("#define SNES_FX_FONT_BYTES   %d\n" % len(font))
        fh.write("#define SNES_FX_BURST_PAL    %d\n" % BURST_PAL)
        fh.write("#define SNES_FX_DIGIT_PAL    %d\n" % DIGIT_PAL)
        fh.write("#define SNES_FX_HEAT_FIRST   3\n#define SNES_FX_HEAT_BANDS   %d\n" % HEAT_BANDS)
        fh.write("#define SNES_FX_FIELDS       %d\n" % FX_FIELDS)
        for name in ("CORE_S", "CORE_M", "CORE_L", "BLADE_HEAD", "BLADE_TAIL",
                     "ARC0", "ARC1", "ARC2", "ARC3", "RAY0", "RAY1", "RAY2", "RAY3",
                     "THIN0", "THIN1", "THIN2", "SPARK0", "SPARK1", "SPARK2", "SPARK3",
                     "DIGIT0"):
            fh.write("#define SNES_FX_%-12s %d\n" % (name, names[name]))
        fh.write("#define SNES_FX_DIGIT_ROW2   %d\n" % names["DIGIT8"])
        fh.write("\n/* The heat ramp the burst palette's six bands are cut from; cooling\n"
                 " * by k writes bands k..k+5 into CGRAM. */\n")
        fh.write("static const u16 snes_fx_heat_ramp[%d] = { %s };\n" % (
            len(HEAT_RAMP), ", ".join("0x%04X" % ((r) | (g << 5) | (b << 10)) for r, g, b in HEAT_RAMP)))
        fh.write("/* Per FX field: the shock ring's radius and the core's radius. */\n")
        fh.write("static const u8 snes_fx_ring_r[%d] = { %s };\n" % (
            FX_FIELDS, ", ".join(str(max(0, min(255, v))) for v in rings)))
        fh.write("static const u8 snes_fx_core_r[%d] = { %s };\n" % (
            FX_FIELDS, ", ".join(str(max(0, min(255, v))) for v in cores)))
        fh.write("/* Spark launch velocities, Q12.4 pixels a field, and the variant. */\n")
        fh.write("#define SNES_FX_SPARKS %d\n" % len(sparks))
        fh.write("static const u16 snes_fx_spark_vx[%d] = { %s };\n" % (
            len(sparks), ", ".join("0x%04X" % s[0] for s in sparks)))
        fh.write("static const u16 snes_fx_spark_vy[%d] = { %s };\n" % (
            len(sparks), ", ".join("0x%04X" % s[1] for s in sparks)))
        fh.write("static const u8 snes_fx_spark_kind[%d] = { %s };\n" % (
            len(sparks), ", ".join(str(s[2]) for s in sparks)))
        fh.write("\n#endif\n")
    print("snes_battlefx: %d tile bytes, %d font bytes, bank $%02X" % (len(tiles), len(font), 0xC0 + BANK))


if __name__ == "__main__":
    main()
