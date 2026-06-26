#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
COMMON = ROOT / 'src/generated/waifu_assets.h'
TITLE = ROOT / 'src/generated/title_asset.h'
OUT = ROOT / 'src/generated/pcfx_palette_assets.h'
LEVELS = 65


def parse_u8_array(path, name, expect=None):
    text = path.read_text()
    m = re.search(r'static const uint8_t\s+' + re.escape(name) + r'[^=]*=\s*\{(.*?)\};', text, re.S)
    if not m:
        raise SystemExit(f'missing {name} in {path}')
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    nums = [int(x,0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
    if expect is not None and len(nums) != expect:
        raise SystemExit(f'{name} length {len(nums)} expected {expect}')
    return nums


def parse_u16_array(path, name, expect=None):
    text = path.read_text()
    m = re.search(r'static const uint16_t\s+' + re.escape(name) + r'[^=]*=\s*\{(.*?)\};', text, re.S)
    if not m:
        return None
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    nums = [int(x,0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
    if expect is not None and len(nums) != expect:
        raise SystemExit(f'{name} length {len(nums)} expected {expect}')
    return nums


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def rgb888_to_pcfx_yuv(r,g,b):
    best_err = 0x7fffffff
    best_y=0; best_u4=8; best_v4=8
    for u4 in range(16):
        u = (u4 << 4) - 128
        for v4 in range(16):
            v = (v4 << 4) - 128
            ro = -0.000039457070707 * u + 1.139827967171717 * v
            go = -0.394610164141414 * u - 0.580500315656566 * v
            bo = 2.031999684343434 * u - 0.000481376262626 * v
            y = int(round((2*(r-ro) + 4*(g-go) + (b-bo)) / 7.0))
            y = clamp(y,0,255)
            rr = clamp(int(round(y+ro)),0,255); gg = clamp(int(round(y+go)),0,255); bb = clamp(int(round(y+bo)),0,255)
            dr=rr-r; dg=gg-g; db=bb-b
            err = 2*dr*dr + 4*dg*dg + db*db
            if err < best_err:
                best_err = err; best_y=y; best_u4=u4; best_v4=v4
    return (best_y << 8) | (best_u4 << 4) | best_v4


def fade_level_to_q8(level):
    return (level * 256 + ((LEVELS - 1)//2)) // (LEVELS - 1)


def dim_yuv_word(word, q8):
    if q8 <= 0:
        return 0x0088
    if q8 >= 256:
        return word
    y = (word >> 8) & 0xff
    u = (word >> 4) & 0x0f
    v = word & 0x0f
    y = (y * q8 + 128) >> 8
    u = 8 + (((u - 8) * q8 + (128 if u >= 8 else -128)) >> 8)
    v = 8 + (((v - 8) * q8 + (128 if v >= 8 else -128)) >> 8)
    u = clamp(u,0,15); v = clamp(v,0,15); y = clamp(y,0,255)
    return (y << 8) | (u << 4) | v


def build_from_rgb(pal):
    words=[]
    for i in range(256):
        words.append(rgb888_to_pcfx_yuv(pal[i*3],pal[i*3+1],pal[i*3+2]))
    return build_from_yuv(words)


def build_from_yuv(words):
    black_idx = 255
    neutral_black = 0x0088
    out=[]
    for level in range(LEVELS):
        q8 = fade_level_to_q8(level)
        row=[]
        for i,w in enumerate(words):
            if i == black_idx:
                yuv = neutral_black
            else:
                yuv = dim_yuv_word(w, q8)
            row.append(yuv)
        row[black_idx]=neutral_black
        out.append(row)
    return out


def emit():
    common=parse_u8_array(COMMON,'waifu_palette_rgb',256*3)
    dialogue=parse_u8_array(COMMON,'waifu_dialogue_palette_rgb',256*3)
    title_yuv=parse_u16_array(TITLE,'title_screen_palette_pcfx_yuv',256)
    if title_yuv is None:
        title=parse_u8_array(TITLE,'title_screen_palette_rgb',256*3)
        title_table=build_from_rgb(title)
    else:
        title_table=build_from_yuv(title_yuv)
    tables=[build_from_rgb(common),title_table,build_from_rgb(dialogue)]
    with OUT.open('w') as f:
        f.write('#ifndef WAIFU_PCFX_PALETTE_ASSETS_H\n#define WAIFU_PCFX_PALETTE_ASSETS_H\n\n#include <stdint.h>\n\n')
        f.write(f'#define WAIFU_PCFX_FADE_LEVELS {LEVELS}\n')
        f.write('static const uint16_t waifu_pcfx_palette_fade_lut[3][WAIFU_PCFX_FADE_LEVELS][256] = {\n')
        for table in tables:
            f.write('  {\n')
            for row in table:
                f.write('    {')
                for i,v in enumerate(row):
                    if i%12==0: f.write('\n      ')
                    f.write(f'0x{v:04x},')
                f.write('\n    },\n')
            f.write('  },\n')
        f.write('};\n\n#endif /* WAIFU_PCFX_PALETTE_ASSETS_H */\n')

if __name__=='__main__':
    emit()
