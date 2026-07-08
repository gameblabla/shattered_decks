#!/usr/bin/env sh
set -eu

PCFX_HEADLESS=${PCFX_HEADLESS:-./pcfxemu/pcfx-headless}
PCFX_BIOS_DIR=${PCFX_BIOS_DIR:-.}
PCFX_CUE=${PCFX_CUE:-waifupcfx.cue}
SEED_SRM=${SEED_SRM:-waifupcfx.cue.srm}
OUT=${OUT:-pcfx_sanctum_rainbow_hscroll_out}
COMMANDS=${COMMANDS:-scripts/pcfx/story_sanctum_rainbow_hscroll.commands}

mkdir -p "$OUT/sram"

for scene in desert:0 stone:2 ember:3 sky:4; do
  name=${scene%%:*}
  duel=${scene##*:}
  scene_out="$OUT/$name"
  mkdir -p "$scene_out/sram"

  python3 - "$SEED_SRM" "$scene_out/sram/$(basename "$PCFX_CUE").srm" "$duel" <<'PY'
import sys
from pathlib import Path
src, dst, duel = sys.argv[1], sys.argv[2], int(sys.argv[3])
b = bytearray(Path(src).read_bytes())
off = b.find(b"WAIF\x01")
if off < 0:
    raise SystemExit("no WAIF save blob in seed SRM")
b[off + 11] = duel
b[off + 12] = 0
b[off + 13] = 0
ck = sum(b[off:off + 121]) & 0xffff
b[off + 121] = ck & 0xff
b[off + 122] = ck >> 8
Path(dst).write_bytes(b)
PY

  "$PCFX_HEADLESS" --bios-dir "$PCFX_BIOS_DIR" --save-dir "$scene_out" --pcfx --fast-video \
    --frames 5200 --commands "$COMMANDS" \
    --screenshot "$scene_out/sanctum_a.ppm" \
    --state-out "$scene_out/sanctum.state" \
    "$PCFX_CUE" >/dev/null

  hold_frames=60
  "$PCFX_HEADLESS" --bios-dir "$PCFX_BIOS_DIR" --save-dir "$scene_out" --pcfx --fast-video \
    --state-in "$scene_out/sanctum.state" --frames "$hold_frames" \
    --screenshot "$scene_out/sanctum_b.ppm" \
    "$PCFX_CUE" >/dev/null

  python3 - "$scene_out/sanctum_a.ppm" "$scene_out/sanctum_b.ppm" "$name" <<'PY'
import sys
from pathlib import Path

def read_ppm(path):
    data = Path(path).read_bytes()
    i = 0
    def token():
        nonlocal i
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if i < len(data) and data[i] == ord("#"):
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            return token()
        j = i
        while i < len(data) and data[i] not in b" \t\r\n":
            i += 1
        return data[j:i]
    magic = token()
    if magic != b"P6":
        raise SystemExit(f"{path}: expected P6 PPM")
    w = int(token())
    h = int(token())
    maxv = int(token())
    if maxv != 255:
        raise SystemExit(f"{path}: expected max value 255")
    if i < len(data) and data[i] in b" \t\r\n":
        i += 1
    pixels = data[i:]
    if len(pixels) != w * h * 3:
        raise SystemExit(f"{path}: bad pixel payload size")
    return w, h, pixels

def rgb_at(pixels, w, x, y):
    o = (y * w + x) * 3
    return pixels[o], pixels[o + 1], pixels[o + 2]

def sad(a, b, w, x0, y0, x1, y1, shift):
    total = 0
    n = 0
    min_n = ((x1 - x0) * (y1 - y0)) // 2
    for y in range(y0, y1):
        for x in range(x0, x1):
            sx = x + shift
            if sx < x0 or sx >= x1:
                continue
            ar, ag, ab = rgb_at(a, w, sx, y)
            br, bg, bb = rgb_at(b, w, x, y)
            total += abs(ar - br) + abs(ag - bg) + abs(ab - bb)
            n += 1
    if n < min_n:
        return float("inf")
    return total / max(n, 1)

wa, ha, pa = read_ppm(sys.argv[1])
wb, hb, pb = read_ppm(sys.argv[2])
scene_name = sys.argv[3]
if (wa, ha) != (wb, hb):
    raise SystemExit("screenshot dimensions differ")

# Upper-left RAINBOW area. This stays above most 3D geometry and left of the
# VDC menu panel, while including enough texture/stars for every backdrop.
x0, y0, x1, y1 = 0, 24, 128, 96
min_changed = 100
identity = sad(pa, pb, wa, x0, y0, x1, y1, 0)
best_shift = 0
best_score = identity
for shift in range(-64, 65):
    if shift == 0:
        continue
    score = sad(pa, pb, wa, x0, y0, x1, y1, shift)
    if score < best_score:
        best_score = score
        best_shift = shift

changed = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        if rgb_at(pa, wa, x, y) != rgb_at(pb, wa, x, y):
            changed += 1

if changed < min_changed:
    raise SystemExit(f"{scene_name}: sanctum background did not visibly change: changed_pixels={changed}")
if best_shift == 0 or best_score >= identity * 0.85:
    raise SystemExit(
        f"{scene_name}: sanctum background changed but no horizontal-scroll signature was found: "
        f"identity_sad={identity:.2f} best_shift={best_shift} best_sad={best_score:.2f}"
    )

print(
    f"PCFX sanctum RAINBOW hscroll OK {scene_name}: changed_pixels={changed} "
    f"best_shift={best_shift} identity_sad={identity:.2f} best_sad={best_score:.2f}"
)
PY
done
