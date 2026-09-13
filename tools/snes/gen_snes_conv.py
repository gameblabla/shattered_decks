#!/usr/bin/env python3
"""The 1:1 chunky -> planar tile converter for the sparse Mode 3 presenter.

The converter turns one 8x8 cell of a chunky direct-colour frame into one
64-byte 8bpp planar tile in the upload ring, straight-line code with every
address baked in, because the loop control is where a generic converter spends
a third of its time.  They are generated rather than written because there are
four copies of each: the ring slot the tile lands in is addressed through the
direct page register (a four-cycle store instead of a six-cycle indexed one),
and a direct-page access pays an extra cycle whenever D's low byte is not zero,
so D is pointed at the 256-byte PAGE the slot is in and the slot's phase inside
that page (0..3, 64 bytes each) picks one of four copies with its offsets
pre-added.

Each tile row is four texel pairs, each looked up once per plane pair in the
512 KB pair LUT (tools/snes/gen_snes_planar.py) and shifted into place.  The
texel-at-a-time form through the 16 KB texel LUT that preceded it cost 60
cycles a texel, and the converter was the largest single cost of every frame.

Register contract for every generated tile routine (called with jsr from the
driver in snes_conv.asm, which sets everything up):
    A/X/Y 16-bit, D = ring slot page, Y = the cell's chunky byte offset,
    DB = the chunky frame's bank.  X is clobbered.  Returns with rts.
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "src", "snes", "snes_conv_gen.asm")

FRAME_STRIDE = 256
# The moving camera's 128x72 frame (the top of the same buffer at half the
# stride), shown doubled: each texel is a 2x2 block of screen pixels, so a
# cell of it is four rows of four texels.
HALF_STRIDE = 128


def frame_tile(phase):
    """One 8-row 1:1 tile for ring-slot phase `phase`, from the frame.

    A tile row is four texel PAIRS.  Each pair loads as one word (t1 << 8 |
    t0); `asl` turns it into a word offset and moves t1's top bit into C,
    which picks the 64 KB half of each plane-pair table (the 17-bit index
    split).  The table word has t0 in column 0 and t1 in column 1 of both
    bytes, so pair p is that word shifted right 2p -- the low six bits of
    each byte are zero, so nothing crosses between the planes.  Four lookups
    a pair instead of four a texel: about 34 cycles a texel."""
    base = phase * 64
    o = ["snesConvFrameTile%d:" % phase]
    for r in range(8):
        off = [base + k * 16 + r * 2 for k in range(4)]
        for pair in range(4):
            src = "snes_frame_fb + %d" % (r * FRAME_STRIDE + pair * 2)
            lab = "_cf%d_r%d_p%d" % (phase, r, pair)
            o.append("    lda.w %s,y" % src)
            o.append("    asl a")
            o.append("    tax")
            # Both halves inline: a texel with blue >= 2 has its top bit
            # set, so the halves are about equally likely and a branch
            # around costs less than an out-of-line jump.
            o.append("    bcs %s_h1" % lab)
            for h in (0, 1):
                if h == 1:
                    o.append("%s_h1:" % lab)
                for k in range(4):
                    o.append("    lda.l snes_pairlut_p%d_h%d,x" % (k, h))
                    o += ["    lsr a"] * (2 * pair)
                    if pair:
                        o.append("    ora.b $%02X" % off[k])
                    o.append("    sta.b $%02X" % off[k])
                if h == 0:
                    o.append("    bra %s_back" % lab)
            o.append("%s_back:" % lab)
    o.append("    rts")
    return o


def half_tile(phase):
    """One tile of the DOUBLED motion frame for ring-slot phase `phase`.

    The motion frame is 128x72 and every texel of it is a 2x2 block of
    screen pixels, so a tile is four source rows of four texels: a texel's
    bit lands in two adjacent columns of the plane byte, and each plane row
    is stored twice.  The texel byte alone is the index (`and #$FF`, then a
    word offset) into a 256-entry table that already has the pair at the
    right columns for that texel position -- one table per plane pair and
    position, 8 KB in all -- so there is nothing to shift.  Four lookups a
    texel, about 68 cycles a texel or 17 a screen pixel: half the 1:1
    converter's, for a frame with the same number of cells."""
    base = phase * 64
    o = ["snesConvHalfTile%d:" % phase]
    for r in range(4):
        # Tile rows 2r and 2r + 1, both from source row r.
        off = [base + k * 16 + r * 4 for k in range(4)]
        for pos in range(4):
            src = "snes_frame_fb + %d" % (r * HALF_STRIDE + pos)
            o.append("    lda.w %s,y" % src)
            o.append("    and #$00FF")
            o.append("    asl a")
            o.append("    tax")
            for k in range(4):
                o.append("    lda.l snes_dbllut_p%d_c%d,x" % (k, pos))
                if pos:
                    o.append("    ora.b $%02X" % off[k])
                o.append("    sta.b $%02X" % off[k])
                if pos == 3:
                    o.append("    sta.b $%02X" % (off[k] + 2))
    o.append("    rts")
    return o


def dbl_luts():
    """[plane pair][texel position][texel] words: the texel's two planes at
    columns 2*pos and 2*pos+1 (bits 7-2*pos and 6-2*pos), low byte the even
    plane, high byte the odd one; the other six bits are zero."""
    o = []
    for k in range(4):
        for pos in range(4):
            o.append("snes_dbllut_p%d_c%d:" % (k, pos))
            words = []
            for t in range(256):
                lo = (((t >> (2 * k)) & 1) * 0xC0) >> (2 * pos)
                hi = (((t >> (2 * k + 1)) & 1) * 0xC0) >> (2 * pos)
                words.append(lo | (hi << 8))
            for i in range(0, 256, 16):
                o.append("    .dw " + ", ".join("$%04X" % w for w in words[i:i + 16]))
    return o


def main():
    lines = [
        "; Generated by tools/snes/gen_snes_conv.py; do not edit.",
        "; The unrolled chunky -> planar tile converters.  See the generator for",
        "; the register contract and why there are four copies of each.",
        '.include "hdr.asm"',
        '.include "snes_fb.inc"',
        "",
        ".ACCU 16",
        ".INDEX 16",
        ".16BIT",
        "",
        ".BASE $C0",
        '.SECTION "snes_conv_gen" SUPERFREE',
        "",
        "; jsr targets, indexed by (slot phase << 1).  Same bank as the code:",
        "; jmp (abs,x) reads its table from the program bank.",
        "snesConvFrameTileTab:",
        "    .dw snesConvFrameTile0, snesConvFrameTile1",
        "    .dw snesConvFrameTile2, snesConvFrameTile3",
        "snesConvHalfTileTab:",
        "    .dw snesConvHalfTile0, snesConvHalfTile1",
        "    .dw snesConvHalfTile2, snesConvHalfTile3",
        "",
    ]
    for p in range(4):
        lines += frame_tile(p) + [""]
    for p in range(4):
        lines += half_tile(p) + [""]
    lines += ['.INCLUDE "snes_conv_drivers.inc"', "", ".ENDS", ""]
    lines += ["", ".BASE $C0", '.SECTION "snes_dbllut" SUPERFREE', ""]
    lines += dbl_luts()
    lines += ["", ".ENDS", ""]
    with open(OUT, "w") as fh:
        fh.write("\n".join(lines))
    print("%s: %d lines" % (os.path.relpath(OUT, ROOT), len(lines)))


if __name__ == "__main__":
    main()
