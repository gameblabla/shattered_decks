#!/usr/bin/env python3
"""Read the debug state stamp out of a Tsugaru screenshot.

FMTOWNS_DEBUG_INPUT builds stamp state into the top-left 58 pixels of the
256x240 framebuffer as little-endian bit patterns (see fmtowns_main.c's
fmtowns_stamp_debug_state):

    pixels  0-15  game frame counter
    pixels 16-20  CD-DA track number currently started
    pixel     21  set while the drive last reported PLAYING
    pixels 22-33  average whole-frame time, in 128us units
    pixels 34-45  average waifu_fm_step() time, in 128us units
    pixels 46-57  average present time, in 128us units

The three timing fields come from the machine's own free-running 1us counter,
so the frame rate they give is the emulated Marty's real speed and does not
depend on how fast the host is emulating -- a run under -NOWAIT reports the
same fps as a throttled one.  Tsugaru's SS command saves a
640x480 frame, in which that framebuffer is drawn 2x-scaled and centred, so
framebuffer pixel (i, 0) is screenshot pixel (64 + 2i, 0).

Pass --summary to print the min/median/max of a whole capture instead of a
line per shot; see summarize() for why that is the only sound way to compare
two builds.

Lit ("1") is palette index 3 and clear ("0") is index 255, whose actual RGB
depends on which palette is loaded, so this classifies by relative
brightness: the two values used across the 16 cells are separated and the
brighter one taken as 1.  A frame where both cells happen to be the same
colour (e.g. a fully faded-to-black palette) reads as ambiguous and is
reported as such rather than guessed at.

Usage: read_frame_stamp.py shot.png [shot.png ...]
"""
import sys

from PIL import Image

ORIGIN_X = 64
SCALE = 2


STAMP_BITS = 58

# Timing fields are stored in 128us units (fmtowns_main.c).
US_PER_UNIT = 128


def read_stamp(path):
    img = Image.open(path).convert("RGB")
    cells = [img.getpixel((ORIGIN_X + SCALE * i, 0)) for i in range(STAMP_BITS)]
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in cells]
    lo, hi = min(lum), max(lum)
    if hi - lo < 24:
        return None
    mid = (lo + hi) / 2
    bits = 0
    for i, v in enumerate(lum):
        if v > mid:
            bits |= 1 << i
    total = ((bits >> 22) & 0xFFF) * US_PER_UNIT
    return {
        "frame": bits & 0xFFFF,
        "track": (bits >> 16) & 0x1F,
        "playing": bool(bits & (1 << 21)),
        "total_us": total,
        "step_us": ((bits >> 34) & 0xFFF) * US_PER_UNIT,
        "present_us": ((bits >> 46) & 0xFFF) * US_PER_UNIT,
        "fps": (1e6 / total) if total else 0.0,
    }


def summarize(stats):
    """Print the distribution of frame times across a whole capture.

    A single screenshot's timing only describes whatever scene it happened to
    land on, and two builds never reach the same scene at the same wall-clock
    second -- CD load times are wall-clock, so a faster build arrives
    everywhere earlier and the runs desynchronise.  Comparing one shot from
    each build therefore compares two different scenes and says nothing.
    Sampling a whole run and comparing the distributions does work: both runs
    walk the same duel through the same script, so the cheapest and dearest
    frames are the same scenes even though they occur at different moments.
    """
    if not stats:
        return
    def field(key):
        return sorted(s[key] / 1000.0 for s in stats)
    def show(name, vals):
        mid = vals[len(vals) // 2]
        print(f"  {name:8s} min {vals[0]:6.1f}  median {mid:6.1f}"
              f"  max {vals[-1]:6.1f} ms")
    print(f"{len(stats)} timed shots:")
    show("frame", field("total_us"))
    show("step", field("step_us"))
    show("present", field("present_us"))
    fps = sorted(s["fps"] for s in stats)
    print(f"  fps      min {fps[0]:6.1f}  median {fps[len(fps) // 2]:6.1f}"
          f"  max {fps[-1]:6.1f}")


def main():
    args = [a for a in sys.argv[1:] if a != "--summary"]
    want_summary = len(args) != len(sys.argv[1:])
    if not args:
        sys.exit(__doc__)
    timed = []
    for path in args:
        st = read_stamp(path)
        if st is None:
            print(f"{path}: ambiguous (stamp cells are the same colour)")
            continue
        if st["total_us"]:
            timed.append(st)
        if want_summary:
            continue
        music = (f"cd track {st['track']}"
                 + (" playing" if st["playing"] else " NOT playing")) \
            if st["track"] else "silence"
        # A zero total means the first 16-frame profile window has not closed
        # yet (or this is a pre-timing build), not a frame that took no time.
        timing = (f"{st['fps']:.1f} fps "
                  f"[frame {st['total_us'] / 1000:.1f}ms = "
                  f"step {st['step_us'] / 1000:.1f} + "
                  f"present {st['present_us'] / 1000:.1f}]"
                  if st["total_us"] else "timing not yet sampled")
        print(f"{path}: frame {st['frame']}, {timing}, {music}")
    if want_summary:
        summarize(timed)


if __name__ == "__main__":
    main()
