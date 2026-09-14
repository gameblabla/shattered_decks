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
pair LUT (tools/snes/gen_snes_planar.py) and shifted into place.  The
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
    t0); `asl` turns it into a word offset and moves t1's top bit into C.
    That bit is plane 7 of the odd texel and nothing else reads it, so it
    picks the 64 KB half of plane pair 3's table only (the 17-bit index
    split) and the other three pairs read a single table each.  The table
    word has t0 in column 0 and t1 in column 1 of both bytes, so pair p is
    that word shifted right 2p -- the low six bits of each byte are zero,
    so nothing crosses between the planes.  Four lookups a pair instead of
    four a texel: about 34 cycles a texel.

    THE PAIRS ARE FOLDED IN REVERSE.  The row word is
        T0 | (T1 >> 2) | (T2 >> 4) | (T3 >> 6)
    and shifting each lookup into place costs 0 + 2 + 4 + 6 = 12 `lsr` a
    plane pair.  Built from pair 3 down as
        acc = T3; acc = (acc >> 2) | T2; acc = (acc >> 2) | T1; acc = (acc >> 2) | T0
    it is the same word (each byte's low six bits are zero, so a shifted
    partial never reaches the other byte) for 2 + 2 + 2 = 6.  The accumulator
    stays in the ring word; a later pair is `lda dp; lsr; lsr; ora long,x;
    sta dp`, the old `lda long,x; lsr...; ora dp; sta dp` with the shifts
    halved.  Pair 3's half-table branch must be taken BEFORE the shifts, which
    clobber C, so both arms shift."""
    base = phase * 64
    o = ["snesConvFrameTile%d:" % phase]
    for r in range(8):
        off = [base + k * 16 + r * 2 for k in range(4)]
        for pair in (3, 2, 1, 0):
            src = "snes_frame_fb + %d" % (r * FRAME_STRIDE + pair * 2)
            lab = "_cf%d_r%d_p%d" % (phase, r, pair)
            first = pair == 3
            o.append("    lda.w %s,y" % src)
            o.append("    asl a")
            o.append("    tax")
            # Plane pair 3 first, while C still says which half.  A texel
            # with blue >= 2 has its top bit set, so the halves are about
            # equally likely and the branch costs the same either way.
            o.append("    bcc %s_h0" % lab)
            if first:
                o.append("    lda.l snes_pairlut_p3_h1,x")
            else:
                o.append("    lda.b $%02X" % off[3])
                o.append("    lsr a")
                o.append("    lsr a")
                o.append("    ora.l snes_pairlut_p3_h1,x")
            o.append("    bra %s_j" % lab)
            o.append("%s_h0:" % lab)
            if first:
                o.append("    lda.l snes_pairlut_p3_h0,x")
            else:
                o.append("    lda.b $%02X" % off[3])
                o.append("    lsr a")
                o.append("    lsr a")
                o.append("    ora.l snes_pairlut_p3_h0,x")
            o.append("%s_j:" % lab)
            o.append("    sta.b $%02X" % off[3])
            for k in range(3):
                if first:
                    o.append("    lda.l snes_pairlut_p%d_h0,x" % k)
                else:
                    o.append("    lda.b $%02X" % off[k])
                    o.append("    lsr a")
                    o.append("    lsr a")
                    o.append("    ora.l snes_pairlut_p%d_h0,x" % k)
                o.append("    sta.b $%02X" % off[k])
    o.append("    rts")
    return o


def half_tile(phase):
    """One tile of the DOUBLED motion frame for ring-slot phase `phase`.

    The motion frame is 128x72 and every texel of it is a 2x2 block of
    screen pixels, so a tile is four source rows of four texels, each row
    stored twice.  A row is two texel PAIRS, loaded and indexed exactly as
    the 1:1 tile's (asl, C = the odd texel's plane-7 bit), through the
    doubled pair LUT (tools/snes/gen_snes_planar.py): t0 in columns 0-1,
    t1 in 2-3.  The second pair is the same word shifted down a nibble --
    the nibbles do not touch, both bytes' low four bits being zero -- and
    t1's plane-7 bit, which the 15-bit index cannot carry, is ORed in as
    $3000 before the shift.  Two lookups a plane a row instead of four:
    about 40 cycles a texel, against 68 a texel at a time."""
    base = phase * 64
    o = ["snesConvHalfTile%d:" % phase]
    for r in range(4):
        # Tile rows 2r and 2r + 1, both from source row r.
        off = [base + k * 16 + r * 4 for k in range(4)]
        for pos in range(2):
            src = "snes_frame_fb + %d" % (r * HALF_STRIDE + pos * 2)
            lab = "_ch%d_r%d_p%d" % (phase, r, pos)
            o.append("    lda.w %s,y" % src)
            o.append("    asl a")
            o.append("    tax")
            o.append("    bcc %s_h0" % lab)
            o.append("    lda.l snes_dbl2lut_p3,x")
            o.append("    ora #$3000")
            o.append("    bra %s_j" % lab)
            o.append("%s_h0:" % lab)
            o.append("    lda.l snes_dbl2lut_p3,x")
            o.append("%s_j:" % lab)
            for k in (3, 0, 1, 2):
                if k != 3:
                    o.append("    lda.l snes_dbl2lut_p%d,x" % k)
                if pos:
                    o += ["    lsr a"] * 4
                    o.append("    ora.b $%02X" % off[k])
                o.append("    sta.b $%02X" % off[k])
                # The duplicate row is written ONCE, from the finished
                # word: only the first row is read back to fold the second
                # pair in, so the first pair's copy was a wasted store
                # (four a row, sixteen a tile).  Safe because the slot is
                # published to the NMI only after the whole tile is built.
                if pos:
                    o.append("    sta.b $%02X" % (off[k] + 2))
    o.append("    rts")
    return o


def main():
    lines = [
        "; Generated by tools/snes/gen_snes_conv.py; do not edit.",
        "; The unrolled chunky -> planar tile converters.  See the generator for",
        "; the register contract and why there are four copies of each.",
        '.include "hdr.asm"',
        '.include "snes_fb.inc"',
        '.include "snes_fastdp.inc"',
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
    with open(OUT, "w") as fh:
        fh.write("\n".join(lines))
    print("%s: %d lines" % (os.path.relpath(OUT, ROOT), len(lines)))


if __name__ == "__main__":
    main()
