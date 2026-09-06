#!/usr/bin/env python3
"""Convert the shared AY-3-8910 VGM soundtrack into the ST's YMS1 streams.

The ST's YM2149 is the same chip family as the MSX PSG, so the Atari port plays
the very same recordings the MSX2 one does (`msx_music/*.vgm`) -- but it plays
them from a flat 50 Hz register-delta stream rather than a VGM interpreter,
because the player runs inside the VBL handler and has to be a few hundred
cycles, not a parser.

Two things happen here that cannot happen on the machine:

* **Retuning.**  The MSX PSG is clocked at 1.7897725 MHz and the ST's YM2149 at
  2 MHz, and the period registers are clock-relative: played back unchanged
  every note would be a tone and a half sharp.  Each period is rescaled by
  2000000/1789772.5 once, offline, instead of costing a divide per note.
* **Loop-point state.**  A delta stream cannot be entered in the middle, so the
  frame the VGM loops back to is emitted as a FULL fourteen-register dump.
  Without that, a loop inherits whatever the last frame before the end happened
  to leave in the registers -- which is silence about as often as not.

    python3 tools/atarist/gen_atarist_audio.py
    python3 tools/atarist/gen_atarist_audio.py --out build/atarist/data/MUS

Output is one `.YMS` per track, which the floppy image carries in `MUS\\` and
`atarist_disk.c` loads per scene.

Stream format (see atarist_audio.h, which is the only consumer):

    'YMS1'   u32   magic
    frames   u16   frame count
    flags    u16   bit 0 = loops
    loop_off u32   byte offset of the loop frame inside `data`
    size     u32   bytes of `data`
    data           per frame: u8 count, then count * (u8 reg, u8 value)
"""

import argparse
import gzip
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCE_DIR = os.path.join(ROOT, "msx_music")
DEFAULT_OUT = os.path.join(ROOT, "build", "atarist", "data", "MUS")

# The tracks the port asks for, in atarist_audio.h's enum order, and the eight
# character DOS name each becomes on the floppy.
TRACKS = [
    ("Title.vgm", "TITLE.YMS"),
    ("Overworld.vgm", "OVERWRLD.YMS"),
    ("Battle.vgm", "BATTLE.YMS"),
    ("Boss.vgm", "BOSS.YMS"),
    ("FinalBoss.vgm", "FINALBOS.YMS"),
    ("Victory.vgm", "VICTORY.YMS"),
    ("Fail.vgm", "FAIL.YMS"),
]

MSX_PSG_CLOCK = 1789772.5
ST_YM_CLOCK = 2000000.0
VGM_RATE = 44100
FRAME_HZ = 50
SAMPLES_PER_FRAME = VGM_RATE // FRAME_HZ      # 882

NUM_REGS = 14

# Which registers are periods, and how many bits each one keeps.  Retuning a
# non-period register (volume, mixer) would be a bug, so the split is explicit.
TONE_PAIRS = {0: 1, 2: 3, 4: 5}               # low byte -> high byte
NOISE_REG = 6
ENV_LOW, ENV_HIGH = 11, 12


def read_vgm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    if raw[:4] != b"Vgm ":
        raise SystemExit("%s: not a VGM file" % path)
    version = struct.unpack_from("<I", raw, 8)[0]
    loop_offset = struct.unpack_from("<I", raw, 0x1C)[0]
    loop_abs = (0x1C + loop_offset) if loop_offset else 0
    if version >= 0x150:
        data_offset = struct.unpack_from("<I", raw, 0x34)[0]
        data_abs = 0x34 + data_offset if data_offset else 0x40
    else:
        data_abs = 0x40
    return raw, data_abs, loop_abs


def retune(period, bits):
    """Rescale a period register from the MSX clock to the ST's."""
    scaled = int(round(period * (ST_YM_CLOCK / MSX_PSG_CLOCK)))
    top = (1 << bits) - 1
    if scaled > top:
        scaled = top
    if period and scaled == 0:
        scaled = 1
    return scaled


def retuned_registers(regs):
    """One frame of register state, with the period registers rescaled.

    Retuning is done on the 12-bit pair, not on each byte: scaling the low and
    high bytes independently is not the same number and drifts a semitone at
    the top of the range.
    """
    out = list(regs)
    for lo, hi in TONE_PAIRS.items():
        period = (regs[lo] | (regs[hi] << 8)) & 0x0FFF
        period = retune(period, 12)
        out[lo] = period & 0xFF
        out[hi] = (period >> 8) & 0x0F
    out[NOISE_REG] = retune(regs[NOISE_REG] & 0x1F, 5)
    env = regs[ENV_LOW] | (regs[ENV_HIGH] << 8)
    env = retune(env, 16)
    out[ENV_LOW] = env & 0xFF
    out[ENV_HIGH] = (env >> 8) & 0xFF
    return out


def flatten(path):
    """VGM -> list of per-frame register states, plus the looping frame index."""
    raw, pos, loop_abs = read_vgm(path)
    regs = [0] * NUM_REGS
    regs[7] = 0x3F                     # everything muted until the music says so
    frames = []
    loop_frame = None
    pending = 0                        # samples waited but not yet a frame

    def emit_frames(n):
        for _ in range(n):
            frames.append(list(regs))

    end = len(raw)
    while pos < end:
        if loop_frame is None and loop_abs and pos >= loop_abs:
            loop_frame = len(frames)
        op = raw[pos]
        pos += 1
        if op == 0xA0:                                  # AY-3-8910 write
            reg, val = raw[pos], raw[pos + 1]
            pos += 2
            if reg < NUM_REGS:
                regs[reg] = val
        elif op == 0x61:
            pending += struct.unpack_from("<H", raw, pos)[0]
            pos += 2
        elif op == 0x62:
            pending += 735
        elif op == 0x63:
            pending += 882
        elif op == 0x66:
            break
        elif 0x70 <= op <= 0x7F:
            pending += (op & 0x0F) + 1
        elif op in (0x4F, 0x50):                        # single-byte operands
            pos += 1
        elif 0x51 <= op <= 0x5F:
            pos += 2
        elif op in (0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
                    0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF):
            pos += 2
        elif 0xC0 <= op <= 0xDF:
            pos += 3
        elif 0xE0 <= op <= 0xFF:
            pos += 4
        elif op == 0x67:                                # data block
            size = struct.unpack_from("<I", raw, pos + 2)[0]
            pos += 6 + size
        elif op in (0x90, 0x91, 0x92, 0x93, 0x94, 0x95):
            pos += {0x90: 4, 0x91: 4, 0x92: 5, 0x93: 10, 0x94: 1, 0x95: 4}[op]
        else:
            # Unknown opcode: stop rather than desynchronise the stream and
            # emit noise that sounds like a broken player.
            break

        while pending >= SAMPLES_PER_FRAME:
            emit_frames(1)
            pending -= SAMPLES_PER_FRAME

    if pending:
        emit_frames(1)
    if loop_frame is None:
        # None of these recordings declares a VGM loop point, and a game track
        # that stops after one pass is a bug, not a feature: loop the whole
        # thing.  Frame 0 is a full register dump anyway, so re-entering there
        # is exact.
        loop_frame = 0
    return frames, loop_frame


def encode(frames, loop_frame):
    """Per-frame register deltas, with a full dump at the loop frame."""
    data = bytearray()
    loop_off = 0
    previous = None
    for i, state in enumerate(frames):
        tuned = retuned_registers(state)
        if loop_frame is not None and i == loop_frame:
            loop_off = len(data)
            writes = [(r, tuned[r]) for r in range(NUM_REGS)]
            previous = None
        elif previous is None:
            writes = [(r, tuned[r]) for r in range(NUM_REGS)]
        else:
            writes = [(r, tuned[r]) for r in range(NUM_REGS)
                      if tuned[r] != previous[r]]
        previous = tuned
        data.append(len(writes))
        for reg, val in writes:
            data.append(reg)
            data.append(val)
    flags = 1 if loop_frame is not None else 0
    head = struct.pack(">4sHHII", b"YMS1", min(len(frames), 0xFFFF), flags,
                       loop_off, len(data))
    return bytes(head) + bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--source", default=SOURCE_DIR)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    total = 0
    for src, name in TRACKS:
        path = os.path.join(a.source, src)
        if not os.path.exists(path):
            print("skip %-14s (no source)" % src)
            continue
        frames, loop_frame = flatten(path)
        if not frames:
            print("skip %-14s (no register writes)" % src)
            continue
        blob = encode(frames, loop_frame)
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(blob)
        total += len(blob)
        writes = sum(1 for i, st in enumerate(frames)
                     if i and st != frames[i - 1])
        print("%-14s -> %-12s %5d frames (%5.1fs) loop@%-5d %6d bytes, "
              "%d frames change something"
              % (src, name, len(frames), len(frames) / float(FRAME_HZ),
                 loop_frame, len(blob), writes))
    print("total %d bytes" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
