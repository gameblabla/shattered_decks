#!/usr/bin/env python3
"""Regenerate src/generated/title_asset.h from assets/source/title/titlescreen_shardsofcards.png.
Uses the palette already generated in waifu_assets.h so the whole game stays in one 8-bpp palette.
"""
from pathlib import Path
import re
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'assets/source/title/titlescreen_shardsofcards.png'
PAL_HEADER = ROOT / 'src/generated/waifu_assets.h'
OUT = ROOT / 'src/generated/title_asset.h'
W, H = 256, 240

def parse_palette(path: Path):
    text = path.read_text()
    m = re.search(r'static const uint8_t waifu_palette_rgb\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        raise RuntimeError('Could not find waifu_palette_rgb[] in waifu_assets.h. Run tools/gen_assets.py first.')
    nums = [int(x) for x in re.findall(r'\d+', m.group(1))]
    if len(nums) < 768:
        raise RuntimeError(f'Palette has {len(nums)} values, expected 768')
    return [(nums[i], nums[i+1], nums[i+2]) for i in range(0, 768, 3)]

def nearest_idx(rgb, pal):
    r,g,b = rgb
    best, bd = 0, 10**12
    for i,(pr,pg,pb) in enumerate(pal):
        d = (pr-r)*(pr-r)+(pg-g)*(pg-g)+(pb-b)*(pb-b)
        if d < bd:
            best, bd = i, d
    return best

def main():
    if not SRC.exists():
        raise FileNotFoundError(SRC)
    if not PAL_HEADER.exists():
        raise FileNotFoundError(PAL_HEADER)
    pal = parse_palette(PAL_HEADER)
    img = ImageOps.fit(Image.open(SRC).convert('RGB'), (W, H), method=Image.Resampling.BILINEAR, centering=(0.5,0.5))
    data = [nearest_idx(px, pal) for px in img.getdata()]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('w') as f:
        f.write('#ifndef TITLE_ASSET_H\n#define TITLE_ASSET_H\n\n')
        f.write('#define TITLE_SCREEN_W 256\n#define TITLE_SCREEN_H 240\n')
        f.write('static const uint8_t title_screen_img[TITLE_SCREEN_W * TITLE_SCREEN_H] = {\n')
        for i in range(0, len(data), 32):
            f.write('    ' + ','.join(str(v) for v in data[i:i+32]) + ',\n')
        f.write('};\n\n#endif\n')
    print(f'wrote {OUT}')

if __name__ == '__main__':
    main()
