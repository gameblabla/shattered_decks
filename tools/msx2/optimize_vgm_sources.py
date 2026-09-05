#!/usr/bin/env python3
"""Shrink the msx_music/ VGM recordings in place, losslessly.

The recordings are hand-exported VGMs and they carry a lot of writes that do
not change anything: a register set to the value it already holds, a wait
spelled as a four-byte 0x61 when a one-byte 0x7n says the same thing.
``vgm_cmp`` from FMTOWNSCD_EXAMPLE_Cube/vgmtools removes both.

WHAT "LOSSLESS" MEANS HERE, and why it is not a PCM comparison.  What the
chip sees is a list of (sample offset, register, value) writes, and that list
is the thing this script refuses to let the optimizer change: an accepted file
decodes to a byte-identical event stream and an identical total length.  A
rendered-audio comparison is *weaker* than that and, taken literally, wrong --
vgm2wav resamples at its command-chunk boundaries, so re-spelling one wait as
two moves a sample boundary and shifts a handful of samples by one or two LSB
out of 32768 (about -73 dBFS) while every register write stays where it was.
The previous rule here demanded byte-identical PCM, so it rejected every
optimization and the cartridge shipped the unoptimized streams; the rendered
delta is still measured and printed, but as a diagnostic, not as the gate.

    python3 tools/msx2/optimize_vgm_sources.py [--dry-run]
"""

import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "msx_music"
VGMTOOLS = ROOT / "FMTOWNSCD_EXAMPLE_Cube" / "vgmtools"
BUILD = ROOT / "build" / "msx2_vgmtools"

# Header fields that describe the recording rather than its command stream.
# vgm_cmp legitimately rewrites the file size and the GD3 offset; anything
# else moving means the output is not the same song.
HEADER_FIELDS = (
    ("version", 0x08), ("total samples", 0x18), ("loop offset", 0x1C),
    ("loop samples", 0x20), ("rate", 0x24), ("data offset", 0x34),
    ("YM2413 clock", 0x10), ("Y8950 clock", 0x58), ("AY8910 clock", 0x74),
)


def find_vgm_cmp():
    for name in ("vgm_cmp", "vgm_cmp.exe"):
        if (BUILD / name).exists():
            return BUILD / name
    subprocess.run(["cmake", "-S", str(VGMTOOLS), "-B", str(BUILD),
                    "-DCMAKE_BUILD_TYPE=Release"], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--build", str(BUILD), "--target", "vgm_cmp"],
                   check=True, stdout=subprocess.DEVNULL)
    return BUILD / "vgm_cmp"


def events(data, path):
    """The timed chip register writes the file presents, and its length."""
    cursor = 0x34 + struct.unpack_from("<I", data, 0x34)[0]
    ticks = 0
    out = []
    while cursor < len(data):
        op = data[cursor]
        if op == 0x66:
            return out, ticks
        if op == 0x61:
            ticks += struct.unpack_from("<H", data, cursor + 1)[0]
            cursor += 3
        elif op == 0x62:
            ticks += 735
            cursor += 1
        elif op == 0x63:
            ticks += 882
            cursor += 1
        elif 0x70 <= op <= 0x7F:
            ticks += (op & 0x0F) + 1
            cursor += 1
        elif op in (0xA0, 0x51, 0x5C):  # AY-3-8910, YM2413, Y8950
            out.append((ticks, op, data[cursor + 1], data[cursor + 2]))
            cursor += 3
        else:
            raise SystemExit("%s: unsupported VGM opcode 0x%02X at 0x%X"
                             % (path, op, cursor))
    raise SystemExit("%s: command stream has no end marker" % path)


def rendered(path, wav):
    """One deterministic vgm2wav pass, as raw 16-bit samples."""
    subprocess.run(["vgm2wav", str(path), str(wav)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with wave.open(str(wav), "rb") as handle:
        return handle.readframes(handle.getnframes())


def peak_delta(a, b):
    if len(a) != len(b):
        return None
    worst = 0
    for i in range(0, len(a), 2):
        delta = abs(struct.unpack_from("<h", a, i)[0]
                    - struct.unpack_from("<h", b, i)[0])
        if delta > worst:
            worst = delta
    return worst


def main():
    dry_run = "--dry-run" in sys.argv
    optimizer = find_vgm_cmp()
    BUILD.mkdir(parents=True, exist_ok=True)
    candidate = BUILD / "optimize_candidate.vgm"
    before = after = 0
    for source in sorted(SOURCE_DIR.glob("**/*.vgm")):
        original = source.read_bytes()
        before += len(original)
        candidate.unlink(missing_ok=True)
        subprocess.run([str(optimizer), str(source), str(candidate)],
                       check=True, stdout=subprocess.DEVNULL)
        name = source.relative_to(ROOT)
        if not candidate.exists():
            # vgm_cmp writes no second file when it found nothing to remove.
            after += len(original)
            print("%-46s %7d   already minimal" % (name, len(original)))
            continue
        optimized = candidate.read_bytes()

        note = None
        for field, offset in HEADER_FIELDS:
            if (struct.unpack_from("<I", original, offset)[0]
                    != struct.unpack_from("<I", optimized, offset)[0]):
                note = "REJECTED: %s changed" % field
                break
        if note is None and events(original, name) != events(optimized, name):
            note = "REJECTED: timed register writes changed"
        if note is not None:
            after += len(original)
            print("%-46s %7d   %s" % (name, len(original), note))
            continue

        delta = peak_delta(rendered(source, BUILD / "a.wav"),
                           rendered(candidate, BUILD / "b.wav"))
        (BUILD / "a.wav").unlink(missing_ok=True)
        (BUILD / "b.wav").unlink(missing_ok=True)
        if delta is None:
            after += len(original)
            print("%-46s %7d   REJECTED: rendered length changed" % (name, len(original)))
            continue

        if not dry_run:
            shutil.copyfile(candidate, source)
        after += len(optimized)
        print("%-46s %7d -> %7d  %5.1f%%  (rendered delta %d/32768)"
              % (name, len(original), len(optimized),
                 -100.0 * (len(original) - len(optimized)) / len(original),
                 delta))
    candidate.unlink(missing_ok=True)
    print("%-46s %7d -> %7d  %5.1f%%%s"
          % ("TOTAL", before, after, -100.0 * (before - after) / before,
             "   (dry run, nothing written)" if dry_run else ""))


if __name__ == "__main__":
    main()
