#!/usr/bin/env python3
"""Split sector-padded CD32X big-card art into small chunk files.

The supervisor can load a named file through the Sega CD BIOS reliably while
raw sector slices are more fragile around active audio/data transitions.  The
input is assets/generated/card_big_art_cd.bin: records padded to seven
2048-byte sectors with the first 112*112 bytes containing the card art.

Chunks are small (a few cards each) so a single B-button card check streams only
~56 KiB instead of a whole 224 KiB 16-card chunk: card checks no longer stall for
a second-plus of CD access, and re-checks served from the SH-2 big-art LRU stay
free.  CARDS_PER_CHUNK must match CD32X_CARD_BIG_CHUNK_CARDS in
src/platform/cd32x/cd32x_boot_main.c.  The file count is kept low enough that the
/ASSETS ISO directory stays inside one sector (this disc's Sega CD boot loader is
only exercised single-sector), and the "CBG%02d.BIN" names stay within ISO9660
8.3.
"""
import sys
from pathlib import Path

CARD_BYTES = 112 * 112
SECTOR = 2048
SLOT_BYTES = ((CARD_BYTES + SECTOR - 1) // SECTOR) * SECTOR
CARDS_PER_CHUNK = 4


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
        (outdir / f"CBG{chunk_id:02d}.BIN").write_bytes(data[start:end])
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
