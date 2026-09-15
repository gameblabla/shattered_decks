#!/usr/bin/env python3
"""Split a sector-padded CD32X story-portrait plane into small chunk files.

The supervisor loads a named file through the Sega CD BIOS into a single 128 KiB
1M-mode Word RAM bank, then slides the requested record to the transfer window.
The whole portrait plane does NOT fit in that
128 KiB bank, so loading it whole overran the bank and hung/crashed the first
opponent's plaza load.  Mirror the big-art atlas: split the plane into small
chunk files that each fit the bank comfortably and read only the chunk holding
the requested portrait.

Input is assets/generated/story_portraits.bin (or story_portrait_mask.bin):
WAIFU_STORY_PORTRAIT_COUNT records, each padded to WAIFU_STORY_PORTRAIT_CD_STRIDE
bytes with the first W*H bytes containing the plane data.

PORTRAITS_PER_CHUNK must match CD32X_STORY_PORTRAIT_CHUNK in
src/platform/cd32x/cd32x_boot_main.c.  The prefix ("SPX"/"SPM") + two-digit chunk
index keeps the names inside ISO9660 8.3.
"""
import re
import sys
from pathlib import Path

SECTOR = 2048
PORTRAITS_PER_CHUNK = 2
ROOT = Path(__file__).resolve().parents[2]


def main(argv):
    if len(argv) != 4:
        print(f"usage: {argv[0]} PLANE.BIN OUTDIR PREFIX", file=sys.stderr)
        return 2
    src = Path(argv[1])
    outdir = Path(argv[2])
    prefix = argv[3]
    data = src.read_bytes()
    if len(data) == 0 or len(data) % SECTOR != 0:
        raise SystemExit(f"unexpected portrait plane size: {len(data)}")
    stride = _stride(len(data))
    count = len(data) // stride
    outdir.mkdir(parents=True, exist_ok=True)
    for chunk_id, start in enumerate(range(0, count, PORTRAITS_PER_CHUNK)):
        lo = start * stride
        hi = min(count, start + PORTRAITS_PER_CHUNK) * stride
        (outdir / f"{prefix}{chunk_id:02d}.BIN").write_bytes(data[lo:hi])
    return 0


def _stride(total):
    # Keep the splitter tied to the generated asset contract.  This avoids a
    # second hard-coded portrait resolution whenever the story cell changes.
    header = (ROOT / "src" / "generated" / "waifu_assets.h").read_text()
    match = re.search(r"#define WAIFU_STORY_PORTRAIT_CD_STRIDE (\d+)", header)
    if not match:
        raise SystemExit("generated portrait stride is missing from waifu_assets.h")
    stride = int(match.group(1))
    if total % stride != 0:
        raise SystemExit(
            f"portrait plane size {total} is not a multiple of stride {stride}; "
            "regenerate the portrait assets and header")
    return stride


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
