#!/usr/bin/env python3
"""Split sector-padded CD32X big-card art into small chunk files.

The supervisor can load a named file through the Sega CD BIOS reliably while
raw sector slices are more fragile around active audio/data transitions.  The
input is assets/generated/card_big_art_cd.bin: records padded to seven
2048-byte sectors with the first 112*112 bytes containing the card art.  Chunks
are kept below 256 KiB so one chunk fits in the Word-RAM staging window.
"""
import sys
from pathlib import Path

CARD_BYTES = 112 * 112
SECTOR = 2048
SLOT_BYTES = ((CARD_BYTES + SECTOR - 1) // SECTOR) * SECTOR
CARDS_PER_CHUNK = 16


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} CARD_BIG_ART_CD.BIN OUTDIR", file=sys.stderr)
        return 2
    src = Path(argv[1])
    outdir = Path(argv[2])
    data = src.read_bytes()
    if len(data) == 0 or len(data) % SLOT_BYTES != 0:
        raise SystemExit(f"unexpected big-art blob size: {len(data)}")
    card_count = len(data) // SLOT_BYTES
    outdir.mkdir(parents=True, exist_ok=True)
    for chunk_id, start_card in enumerate(range(0, card_count, CARDS_PER_CHUNK)):
        start = start_card * SLOT_BYTES
        end = min(card_count, start_card + CARDS_PER_CHUNK) * SLOT_BYTES
        (outdir / f"CARD_BG{chunk_id}.BIN").write_bytes(data[start:end])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
