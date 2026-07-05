#!/usr/bin/env python3
"""Split the card-face atlas (card_faces.bin) into fixed 32 KiB chunks.

The CD32X resident Sega CD supervisor serves each asset blob as one bounded
CPY_TO_32X transfer (max ~65535 words).  The full 72-card face atlas is ~144 KiB
which overflows a single transfer, so the SH-2 reads it as CARD_FACE_0..4.BIN
chunks.  This tool produces those chunk files from the platform-agnostic atlas.
"""
import sys
from pathlib import Path

CHUNK = 32768
PADDED_BYTES = 149504
COUNT = 5


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} CARD_FACES.BIN OUTDIR", file=sys.stderr)
        return 2
    src = Path(argv[1])
    outdir = Path(argv[2])
    data = src.read_bytes()
    if len(data) > PADDED_BYTES:
        raise SystemExit(f"card face blob too large: {len(data)} > {PADDED_BYTES}")
    data += bytes(PADDED_BYTES - len(data))
    outdir.mkdir(parents=True, exist_ok=True)
    for i in range(COUNT):
        chunk = data[i * CHUNK:(i + 1) * CHUNK]
        (outdir / f"CARD_FACE_{i}.BIN").write_bytes(chunk)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
