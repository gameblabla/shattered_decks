#!/usr/bin/env python3
"""Check every shipped portrait fringe table against what the baker intended.

The edge byte used to be chosen on the Z80, out of a six-byte record, so this
tool host-compiled that selector and re-ran it.  The choice is now made in
gen_msx_plus.bust_edge_bytes and a record is (x, ink), so what there is to
check is that the bytes in `portraits.bin` ARE that decision -- recomputed here
from the source art and the shipped backdrops, not read back out of the same
table -- and that they still fit, still cover each column once, and still leave
the chroma bits alone.  The emulator separately checks SDCC, banking and VRAM
writes; pass a capture directory to compare against real openMSX frames.

Run after `make plus-assets`.
"""
from pathlib import Path
import sys

import numpy as np

import gen_msx_plus as plus
import gen_msx_scenes as scenes
import msx2_yjk as yjk


def variant_art(index):
    """(bust image, dimmed, x, y) for one of the twelve portrait variants.

    bake_portraits walks Serena and every opponent, lit then dimmed, and lays
    them out in that order; this has to agree with it.
    """
    names = ["serena"] + ["opponent_%d" % d for d in range(scenes.STORY_DUELS)]
    who, dim = divmod(index, 2)
    x0 = plus.MSX2_PORTRAIT_LEFT_X if who == 0 else plus.MSX2_PORTRAIT_RIGHT_X
    y0 = scenes.PORTRAIT_LEFT_Y if who == 0 else scenes.PORTRAIT_RIGHT_Y
    size = (scenes.PORTRAIT_W, scenes.PORTRAIT_H)
    return scenes.portrait(names[who], size, premul=True), bool(dim), x0, y0


def main():
    root = Path(__file__).resolve().parents[2]
    portraits = (root / "src/msx2/assets/plus/portraits.bin").read_bytes()
    capture = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    # WHICH FIGURE IS IN A CAPTURED FRAME IS NOT SCHEDULED, IT IS DISCOVERED.
    # This used to name the frame numbers each variant was expected in, and a
    # redraw that got faster moved the scene along and broke the test without
    # anything being wrong.  Instead every stage-0 variant is compared against
    # every frame, a variant "is" in a frame when not one of its edge bytes
    # differs, and what is asserted is that each of the four was found in at
    # least one frame and that no frame matched a variant only in part.
    frames = sorted(capture.glob("*.vram")) if capture else []
    captures = {f: f.read_bytes() for f in frames}
    hits = {(f, v): [0, 0] for f in frames for v in range(4)}
    yae = total = old_error = new_error = worst_table = 0

    art = [variant_art(v) for v in range(12)]
    for stage in range(plus.STAGE_COUNT):
        bg = (root / ("src/msx2/assets/plus/talk_%d.bin" % stage)).read_bytes()
        pal3 = np.array(yjk.palette_from_bytes(bg[256 * 212:]), dtype=int)
        pal5 = (pal3 << 2) | (pal3 >> 1)
        backdrop = plus.stage_backdrop(bg)
        bg5, jj, kk, _ = backdrop
        for variant, (bust, dim, x0, y0) in enumerate(art):
            # The baker's own answer, from the art rather than from the table.
            ink = plus.bust_edge_bytes(bust, dim, x0, y0, backdrop)
            # ... and the two errors it chose between, for the report.
            a = (np.asarray(bust.getchannel("A"), dtype=float) / 255.0)[..., None]
            fig = np.asarray(bust.convert("RGB"), dtype=float)
            if dim:
                fig = fig * plus.PLUS_PORTRAIT_DIM
            win = (slice(y0, y0 + bust.size[1]), slice(x0, x0 + bust.size[0]))
            comp = a * yjk.to5(fig) + (1.0 - a) * bg5[win]
            ybits = yjk._best_y(comp, jj[win], kk[win], even=True)
            target = np.clip(np.rint(comp), 0, 31).astype(int)
            decoded = np.floor(yjk.yjk_to_rgb(ybits, jj[win], kk[win])).astype(int)
            yjk_error = (np.abs(target - decoded) * [1, 2, 1]).sum(axis=-1)
            pal_error = (np.abs(target[..., None, :] - pal5) * [1, 2, 1]).sum(axis=-1)

            body = portraits[variant * scenes.PORTRAIT_STRIDE:
                             (variant + 1) * scenes.PORTRAIT_STRIDE]
            start = offset = plus.FRINGE_OFF + stage * plus.FRINGE_STRIDE
            covered = set()
            for row in range(scenes.PORTRAIT_H):
                n = body[offset]
                assert n <= plus.FRINGE_MAX
                offset += 1
                for _ in range(n):
                    x, shipped = body[offset:offset + plus.FRINGE_BYTES]
                    offset += plus.FRINGE_BYTES
                    assert (row, x) not in covered
                    covered.add((row, x))
                    # A fringe pixel owns bits 7..3 and nothing else: the low
                    # three are its group's chroma, which Msx2_MergeRow keeps.
                    assert shipped & 7 == 0
                    assert shipped == ink[row, x], (stage, variant, row, x)
                    assert row + y0 < scenes.TALK_BOX_Y
                    if shipped & 8:
                        entry = shipped >> 4
                        score = int(pal_error[row, x, entry])
                        assert score == int(pal_error[row, x].min())
                        assert score < int(yjk_error[row, x])
                        yae += 1
                    else:
                        assert shipped == (int(ybits[row, x]) << 3)
                        score = int(yjk_error[row, x])
                        assert int(pal_error[row, x].min()) >= score
                    if stage == 0 and variant < 4:
                        address = (row + y0) * 256 + x + x0
                        expected = (bg[address] & 7) | shipped
                        for frame in frames:
                            actual = captures[frame][(address >> 1)
                                                     | ((address & 1) << 16)]
                            hits[(frame, variant)][actual != expected] += 1
                    total += 1
                    old_error += int(yjk_error[row, x])
                    new_error += score
            worst_table = max(worst_table, offset - start)
            assert offset <= start + plus.FRINGE_STRIDE
            pixels = sum(body[y * scenes.PORTRAIT_ROW_STRIDE + 2 + k * 2]
                         for y in range(scenes.PORTRAIT_H)
                         for k in range(body[y * scenes.PORTRAIT_ROW_STRIDE]))
            assert plus.PIXELS_OFF + pixels <= scenes.PORTRAIT_STRIDE
    assert yae and new_error < old_error
    print("PASS: %d edges, %d YAE improvements, weighted error %d -> %d (%.1f%% less)"
          % (total, yae, old_error, new_error, 100 * (1 - new_error / old_error)))
    print("All 48 variant/stage combinations fit; largest edge table %d/%d bytes."
          % (worst_table, plus.FRINGE_STRIDE))
    if capture:
        found = {v: [] for v in range(4)}
        verified = 0
        for frame in frames:
            # A figure is EITHER lit or dimmed, never both and never half of
            # one: a frame it stands in must match exactly one of its two
            # variants down to the last edge byte.  A frame that is not a talk
            # screen at all matches neither, which is not a failure -- what
            # would be is a frame matching both, or one and then not quite.
            for side in ((0, 1), (2, 3)):
                clean = [v for v in side if hits[(frame, v)][1] == 0]
                assert len(clean) <= 1, (frame.name, side)
                for v in clean:
                    found[v].append(frame.name)
                    verified += hits[(frame, v)][0]
        for variant in range(4):
            assert found[variant], ("variant %d appears in no captured frame"
                                    % variant)
        print("Matched %d edge bytes across %d openMSX frames; every lit and "
              "dimmed figure found." % (verified, len(frames)))


if __name__ == "__main__":
    main()
