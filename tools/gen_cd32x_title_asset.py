#!/usr/bin/env python3
"""Generate CD32X-specific 320x224 indexed title/ending assets.

The normal generated title header remains 256x240 for host/headless/PC-FX
compatibility.  CD32X uses this sidecar asset so its ISO can carry a true
320x224 8bpp title plane while preserving the common IDX_* UI palette slots.
Palette index 0 is deliberately kept out of the image because 32X composition
paths can treat pen/color 0 as transparent depending on priority/CRAM state.
"""
from pathlib import Path
import re
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
PAL_HEADER = ROOT / 'src/generated/waifu_assets.h'
TITLE_ASSET_HEADER = ROOT / 'src/generated/title_asset.h'
TITLE_SRC = ROOT / 'assets/source/title/title_320x224.png'
ENDING_SRC = ROOT / 'assets/source/ending/ending320x240.png'
BIN_OUT = ROOT / 'assets/generated/cd32x_title_screen_img.bin'
ENDING_BIN_OUT = ROOT / 'assets/generated/cd32x_ending_screen_img.bin'
HDR_OUT = ROOT / 'src/generated/cd32x_title_asset.h'
W, H = 320, 224


def parse_common_assets(path: Path):
    text = path.read_text()
    m = re.search(r'static const uint8_t waifu_palette_rgb\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        raise RuntimeError('Could not find waifu_palette_rgb[] in waifu_assets.h. Run tools/gen_assets.py first.')
    nums = [int(x) for x in re.findall(r'\d+', m.group(1))]
    if len(nums) < 768:
        raise RuntimeError(f'Palette has {len(nums)} values, expected 768')
    common = [(nums[i], nums[i + 1], nums[i + 2]) for i in range(0, 768, 3)]
    idx_values = [int(v) for v in re.findall(r'^#define\s+IDX_[A-Z0-9_]+\s+(\d+)\s*$', text, re.M)]
    reserved = {i for i in idx_values if 0 <= i < 256}
    reserved.add(0)  # avoid 32X transparent pen/color 0 on title pixels
    return common, reserved


def parse_u8_rgb_array(path: Path, name: str):
    text = path.read_text()
    m = re.search(r'static const uint8_t\s+' + re.escape(name) + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        raise RuntimeError(f'Could not find {name}[] in {path}. Run tools/gen_title_asset.py first.')
    nums = [int(x) for x in re.findall(r'\d+', m.group(1))]
    if len(nums) < 768:
        raise RuntimeError(f'{name} has {len(nums)} values, expected 768')
    return [(nums[i], nums[i + 1], nums[i + 2]) for i in range(0, 768, 3)]


def quantize_palette(img: Image.Image, count: int):
    q = img.quantize(colors=max(1, min(256, count)), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    raw = q.getpalette()[:count * 3]
    colors = []
    for i in range(0, len(raw), 3):
        if i + 2 < len(raw):
            colors.append((raw[i], raw[i + 1], raw[i + 2]))
    return colors or [(0, 0, 0)]


def nearest_idx(rgb, pal, candidates):
    r, g, b = rgb
    best, best_d = candidates[0], 1 << 62
    for idx in candidates:
        pr, pg, pb = pal[idx]
        d = (pr - r) * (pr - r) + (pg - g) * (pg - g) + (pb - b) * (pb - b)
        if d < best_d:
            best, best_d = idx, d
    return best


def emit_c_array(f, typename, name, values, per_line=18, fmt=str):
    f.write(f'static const {typename} {name}[] = {{\n')
    for i in range(0, len(values), per_line):
        f.write('    ' + ','.join(fmt(v) for v in values[i:i + per_line]) + ',\n')
    f.write('};\n')


def main():
    if not TITLE_SRC.exists():
        raise FileNotFoundError(TITLE_SRC)
    if not ENDING_SRC.exists():
        raise FileNotFoundError(ENDING_SRC)
    common, reserved = parse_common_assets(PAL_HEADER)
    ending_pal = parse_u8_rgb_array(TITLE_ASSET_HEADER, 'ending_screen_palette_rgb')
    free_slots = [i for i in range(256) if i not in reserved]
    if not free_slots:
        raise RuntimeError('No free CD32X title palette slots')

    img = ImageOps.fit(Image.open(TITLE_SRC).convert('RGB'), (W, H), method=Image.Resampling.BILINEAR, centering=(0.5, 0.5))
    colors = quantize_palette(img, len(free_slots))
    pal = list(common)
    pal[0] = (0, 0, 0)
    for n, slot in enumerate(free_slots):
        pal[slot] = colors[n % len(colors)]
    data = bytes(nearest_idx(px, pal, free_slots) for px in img.getdata())
    if len(data) != W * H:
        raise RuntimeError(f'bad image size: {len(data)}')
    if 0 in data:
        raise RuntimeError('CD32X title data unexpectedly uses palette index 0')

    BIN_OUT.parent.mkdir(parents=True, exist_ok=True)
    BIN_OUT.write_bytes(data)

    ending_img = ImageOps.fit(Image.open(ENDING_SRC).convert('RGB'), (W, H), method=Image.Resampling.BILINEAR, centering=(0.5, 0.5))
    ending_slots = list(range(1, 256))
    ending_data = bytes(nearest_idx(px, ending_pal, ending_slots) for px in ending_img.getdata())
    if len(ending_data) != W * H:
        raise RuntimeError(f'bad ending image size: {len(ending_data)}')
    if 0 in ending_data:
        raise RuntimeError('CD32X ending data unexpectedly uses palette index 0')
    ENDING_BIN_OUT.write_bytes(ending_data)

    flat_pal = [v for rgb in pal for v in rgb]
    HDR_OUT.parent.mkdir(parents=True, exist_ok=True)
    with HDR_OUT.open('w') as f:
        f.write('#ifndef CD32X_TITLE_ASSET_H\n#define CD32X_TITLE_ASSET_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#define CD32X_TITLE_SCREEN_W 320\n#define CD32X_TITLE_SCREEN_H 224\n')
        f.write('#define CD32X_TITLE_SCREEN_BYTES (CD32X_TITLE_SCREEN_W * CD32X_TITLE_SCREEN_H)\n')
        emit_c_array(f, 'uint8_t', 'cd32x_title_screen_palette_rgb', flat_pal, per_line=18)
        f.write('\n#endif /* CD32X_TITLE_ASSET_H */\n')

    print(f'wrote {BIN_OUT} ({len(data)} bytes), avoiding palette index 0')
    print(f'wrote {ENDING_BIN_OUT} ({len(ending_data)} bytes), avoiding palette index 0')
    print(f'wrote {HDR_OUT} with {len(free_slots)} art slots and {len(reserved)} reserved slots')


if __name__ == '__main__':
    main()
