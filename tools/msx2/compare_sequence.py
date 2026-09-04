#!/usr/bin/env python3
"""Machine-check a short MSX2 VRAM/SAT capture sequence.

The trace command writes a JSON manifest whose frame entries contain decoded
PNG paths and optional raw SAT paths.  Assertions are deliberately small:
they describe the transient properties the final screenshot cannot prove.
"""

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops


def frame(manifest, index):
    try:
        return manifest["frames"][index]
    except IndexError:
        raise SystemExit("frame index %d is outside the capture" % index)


def region(image, spec):
    x, y, w, h = (int(part) for part in spec.split(","))
    return image.crop((x, y, x + w, y + h))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--unchanged", metavar="X,Y,W,H")
    parser.add_argument("--from", dest="start", type=int, default=0)
    parser.add_argument("--to", dest="end", type=int)
    parser.add_argument("--hidden-id", type=int, action="append", default=[])
    parser.add_argument("--allow-black", action="store_true")
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    frames = manifest.get("frames", [])
    if not frames:
        raise SystemExit("capture manifest contains no frames")
    end = len(frames) - 1 if args.end is None else args.end
    if not 0 <= args.start <= end < len(frames):
        raise SystemExit("invalid frame interval %d..%d" % (args.start, end))

    if args.unchanged:
        first = Image.open(frame(manifest, args.start)["png"])
        expected = region(first, args.unchanged)
        for index in range(args.start + 1, end + 1):
            current = region(Image.open(frame(manifest, index)["png"]), args.unchanged)
            if ImageChops.difference(expected, current).getbbox() is not None:
                raise SystemExit("FAIL unchanged region at frame %d" % index)

    for index in range(args.start, end + 1):
        item = frame(manifest, index)
        if not args.allow_black and Image.open(item["png"]).getbbox() is None:
            raise SystemExit("FAIL all-black frame %d" % index)
        if args.hidden_id:
            sat = item.get("sat")
            if not sat:
                raise SystemExit("frame %d has no SAT dump" % index)
            data = Path(sat).read_bytes()
            for sprite_id in args.hidden_id:
                at = sprite_id * 4
                if at >= len(data) or data[at] < 217:
                    raise SystemExit("FAIL sprite %d visible at frame %d" %
                                     (sprite_id, index))

    print("SEQUENCE OK: frames %d..%d" % (args.start, end))


if __name__ == "__main__":
    main()
