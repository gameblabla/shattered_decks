#!/usr/bin/env python3
"""Split story portrait atlases into one CD32X-safe record per file."""
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def generated_define(name):
    header = ROOT / "src" / "generated" / "waifu_assets.h"
    prefix = f"#define {name} "
    for line in header.read_text().splitlines():
        if line.startswith(prefix):
            return int(line[len(prefix):].split()[0])
    raise SystemExit(f"missing {name} in {header}")


def write_records(src_path, outdir, stem, stride):
    data = Path(src_path).read_bytes()
    if len(data) % stride:
        raise SystemExit(f"{src_path}: size {len(data)} is not a multiple of {stride}")
    count = len(data) // stride
    for i in range(count):
        chunk = data[i * stride:(i + 1) * stride]
        (outdir / f"{stem}{i:02d}.BIN").write_bytes(chunk)
    return count


def main(argv):
    if len(argv) != 4:
        print(f"usage: {argv[0]} STORY_PORTRAITS.BIN STORY_PORTRAIT_MASK.BIN OUTDIR", file=sys.stderr)
        return 2
    outdir = Path(argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    stride = generated_define("WAIFU_STORY_PORTRAIT_CD_STRIDE")
    pixels = write_records(argv[1], outdir, "POR", stride)
    masks = write_records(argv[2], outdir, "PMK", stride)
    if pixels != masks:
        raise SystemExit(f"portrait/mask count mismatch: {pixels} != {masks}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
