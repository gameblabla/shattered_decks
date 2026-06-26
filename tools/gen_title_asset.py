#!/usr/bin/env python3
"""Regenerate title assets.

The normal PC/headless title uses an RGB-optimized indexed image and RGB
palette.  NEC PC-FX cannot display arbitrary RGB888 palette colors; VCE entries
are Y8U4V4.  If the RGB palette is converted after quantization, the title art
keeps indices chosen for colors the hardware cannot represent, producing color
noise and posterized skin/sky.  Therefore this tool also emits a PC-FX-specific
indexed title image/PC-FX-native YUV palette and a KING 16M YUV422 direct-color title.  The gameplay code still sees
one title-screen asset API; the PC-FX CD backend maps the title blob to the
PC-FX-specific external file.
"""
from pathlib import Path
import re
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'assets/source/title/titlescreen_shardsofcards.png'
SRC_ENDING = ROOT / 'assets/source/ending/ending.png'
PAL_HEADER = ROOT / 'src/generated/waifu_assets.h'
OUT = ROOT / 'src/generated/title_asset.h'
W, H = 256, 240
# The PC-FX 16M title layer is scrolled down by four rows at runtime to avoid
# the hardware/mixer sampling wrapped hidden rows at the top.  The full 256-row
# CD/KRAM blob is therefore pre-shifted so visible scanlines still map to the
# original 256x240 source image exactly.
TITLE_PCFX_16M_SCROLL_Y = 4


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
    return common, reserved


def clamp(v, lo=0, hi=255):
    return lo if v < lo else hi if v > hi else v


def _rgb565_roundtrip(r: int, g: int, b: int):
    # pcfxemu is built with WANT_16BPP + FRONTEND_SUPPORTS_RGB565.
    # The renderer first packs with simple truncation, then screenshots/Y4M
    # unpack RGB565 with the standard integer scale.  Optimize the PC-FX
    # title palette against that displayed value, not abstract RGB888.
    r5 = clamp(r) >> 3
    g6 = clamp(g) >> 2
    b5 = clamp(b) >> 3
    return ((r5 * 255 + 15) // 31,
            (g6 * 255 + 31) // 63,
            (b5 * 255 + 15) // 31)


def yuv_to_rgb(y: int, u4: int, v4: int):
    # Match pcfxemu/mednafen pcfx/king.cpp: the 4-bit U/V nibbles are
    # expanded by shifting in zero low bits, then the YUV->RGB coefficients are
    # truncated to int, not rounded.  This matters most on skin tones because
    # a one-nibble chroma error can push them pink/red.
    u = (u4 << 4) - 128
    v = (v4 << 4) - 128
    ro = int(-0.000039457070707 * u + 1.139827967171717 * v)
    go = int(-0.394610164141414 * u - 0.580500315656566 * v)
    bo = int( 2.031999684343434 * u - 0.000481376262626 * v)
    return _rgb565_roundtrip(y + ro, y + go, y + bo)


def rgb888_to_pcfx_yuv(rgb):
    r, g, b = rgb
    best_err = 10**18
    best = (0, 8, 8)
    for u4 in range(16):
        u = (u4 << 4) - 128
        for v4 in range(16):
            v = (v4 << 4) - 128
            ro = int(-0.000039457070707 * u + 1.139827967171717 * v)
            go = int(-0.394610164141414 * u - 0.580500315656566 * v)
            bo = int( 2.031999684343434 * u - 0.000481376262626 * v)
            y0 = int(round((2 * (r - ro) + 4 * (g - go) + (b - bo)) / 7.0))
            for y in range(max(0, y0 - 4), min(255, y0 + 4) + 1):
                rr, gg, bb = yuv_to_rgb(y, u4, v4)
                dr = rr - r; dg = gg - g; db = bb - b
                err = 2 * dr * dr + 4 * dg * dg + db * db
                if err < best_err:
                    best_err = err
                    best = (y, u4, v4)
    y, u4, v4 = best
    return (y << 8) | (u4 << 4) | v4


def is_probable_skin(rgb):
    r, g, b = rgb
    return (r > 70 and g > 40 and b > 35 and
            r * 100 >= g * 95 and r * 100 >= b * 105 and
            (r - b) > 8)


def pcfx_title_error(src, cand, skin=False):
    r, g, b = src
    pr, pg, pb = cand
    dr = pr - r; dg = pg - g; db = pb - b
    err = 2 * dr * dr + 4 * dg * dg + db * db
    if skin:
        # Skin in the source has green slightly above blue.  With only 4-bit
        # chroma, the nearest plain RGB solution often lands with B >= G,
        # which reads as sunburnt/magenta on the PC-FX YUV pipeline.  Preserve
        # channel deltas for likely skin pixels and penalize blue-over-green.
        err += 3 * ((pg - pb) - (g - b)) * ((pg - pb) - (g - b))
        err += 2 * ((pr - pg) - (r - g)) * ((pr - pg) - (r - g))
        if pb > pg:
            err += 10 * (pb - pg) * (pb - pg)
    return err


def nearest_idx(rgb, pal, candidates):
    r, g, b = rgb
    best, bd = 0, 10**12
    for i in candidates:
        pr, pg, pb = pal[i]
        d = (pr - r) * (pr - r) + (pg - g) * (pg - g) + (pb - b) * (pb - b)
        if d < bd:
            best, bd = i, d
    return best


def quantize_title_palette(img: Image.Image, color_count: int):
    # Pillow's MEDIANCUT path gives a compact source-specific palette while
    # keeping generation deterministic for regression builds.
    q = img.quantize(colors=max(1, min(256, color_count)), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    raw = q.getpalette()[:color_count * 3]
    colors = []
    for i in range(0, len(raw), 3):
        if i + 2 < len(raw):
            colors.append((raw[i], raw[i + 1], raw[i + 2]))
    if not colors:
        colors = [(0, 0, 0)]
    return colors


def assign_pixels_to_palette(img: Image.Image, pal, candidates):
    return [nearest_idx(px, pal, candidates) for px in img.getdata()]


def build_pcfx_title(img: Image.Image, common, reserved, free_slots):
    # Initial centers from the RGB title palette, then Lloyd refine in displayed
    # pcfxemu RGB565 space while snapping every center to a PC-FX Y8U4V4 color.
    centers = quantize_title_palette(img, len(free_slots))
    centers = (centers + [centers[-1]])[:len(free_slots)]
    pixels = list(img.getdata())
    skin_flags = [is_probable_skin(px) for px in pixels]

    def snap(rgb):
        w = rgb888_to_pcfx_yuv(rgb)
        return w, yuv_to_rgb((w >> 8) & 255, (w >> 4) & 15, w & 15)

    yuv_centers = []
    rgb_centers = []
    for c in centers:
        w, rr = snap(c)
        yuv_centers.append(w)
        rgb_centers.append(rr)

    # A small fixed iteration count is deterministic and cheap for 61k pixels.
    for _ in range(6):
        sums = [[0, 0, 0, 0] for _ in rgb_centers]
        for px_i, (r, g, b) in enumerate(pixels):
            skin = skin_flags[px_i]
            best = 0; bd = 10**12
            for i, cand in enumerate(rgb_centers):
                d = pcfx_title_error((r, g, b), cand, skin)
                if d < bd:
                    best = i; bd = d
            weight = 3 if skin else 1
            s = sums[best]
            s[0] += r * weight; s[1] += g * weight; s[2] += b * weight; s[3] += weight
        changed = False
        for i, s in enumerate(sums):
            if s[3]:
                mean = (s[0] // s[3], s[1] // s[3], s[2] // s[3])
                w, rr = snap(mean)
                if w != yuv_centers[i]:
                    changed = True
                yuv_centers[i] = w
                rgb_centers[i] = rr
        if not changed:
            break

    pcfx_yuv = [rgb888_to_pcfx_yuv(common[i]) for i in range(256)]
    pcfx_rgb = [yuv_to_rgb((pcfx_yuv[i] >> 8) & 255, (pcfx_yuv[i] >> 4) & 15, pcfx_yuv[i] & 15) for i in range(256)]
    for n, slot in enumerate(free_slots):
        pcfx_yuv[slot] = yuv_centers[n]
        pcfx_rgb[slot] = rgb_centers[n]

    pcfx_data = []
    for px_i, rgb in enumerate(img.getdata()):
        skin = skin_flags[px_i]
        best = 0; bd = 10**12
        for i in free_slots:
            d = pcfx_title_error(rgb, pcfx_rgb[i], skin)
            if d < bd:
                best, bd = i, d
        pcfx_data.append(best)
    return pcfx_yuv, pcfx_data




def rgb888_to_pcfx_yuv_fast64(rgb):
    """Fast per-pixel RGB888 -> PC-FX Y8U4V4 for 64K BG mode.

    This is not constrained to 256 palette entries, so direct chroma quantizing
    plus a small local refinement gives much better skin color than indexed
    palette mode without the extremely expensive full 256-candidate palette
    search per pixel.
    """
    r, g, b = rgb
    # Invert pcfxemu's approximate YUV->RGB path to a continuous estimate.
    y_est = 0.299 * r + 0.587 * g + 0.114 * b
    u_est = (b - y_est) / 2.031999684343434 + 128.0
    v_est = (r - y_est) / 1.139827967171717 + 128.0
    u0 = int(round(u_est / 16.0))
    v0 = int(round(v_est / 16.0))
    best_err = 10**12
    best = 0x0188
    # Search a small neighbourhood; 5x5 chroma points, exact weighted luma.
    for u4 in range(max(0, u0 - 2), min(15, u0 + 2) + 1):
        u = (u4 << 4) - 128
        for v4 in range(max(0, v0 - 2), min(15, v0 + 2) + 1):
            v = (v4 << 4) - 128
            ro = int(-0.000039457070707 * u + 1.139827967171717 * v)
            go = int(-0.394610164141414 * u - 0.580500315656566 * v)
            bo = int( 2.031999684343434 * u - 0.000481376262626 * v)
            y = int(round((2 * (r - ro) + 4 * (g - go) + (b - bo)) / 7.0))
            y = clamp(y, 1, 255)
            rr, gg, bb = yuv_to_rgb(y, u4, v4)
            dr = rr - r; dg = gg - g; db = bb - b
            err = 2 * dr * dr + 4 * dg * dg + db * db
            # For likely skin, avoid a magenta/sunburn bias.
            if is_probable_skin(rgb):
                err += 1 * ((gg - bb) - (g - b)) * ((gg - bb) - (g - b))
                if bb > gg:
                    err += 2 * (bb - gg) * (bb - gg)
            if err < best_err:
                best_err = err
                best = (y << 8) | (u4 << 4) | v4
    return best

def build_pcfx_title_yuv16(img: Image.Image):
    """Return per-pixel KING_BGMODE_64K Y8U4V4 words.

    PC-FX 64K BG mode is still YUV, not RGB565: upper 8 bits are Y,
    bits 7..4 are U, bits 3..0 are V.  pcfxemu treats Y==0 as transparent,
    so clamp black pixels to neutral near-black instead of literal zero.
    A cache is required because the source is RGB888 after scaling and can
    contain many repeated colors.
    """
    out = []
    cache = {}
    for rgb in img.getdata():
        w = cache.get(rgb)
        if w is None:
            w = rgb888_to_pcfx_yuv_fast64(rgb)
            if (w & 0xff00) == 0:
                # Avoid BG transparency; keep chroma neutral for black/near-black.
                w = 0x0188
            cache[rgb] = w
        out.append(w)
    return out


def yuv888_to_rgb565_rt(y: int, u8: int, v8: int):
    """Match pcfxemu KING 16M display path for one YUV888 pixel."""
    u = u8 - 128
    v = v8 - 128
    ro = int(-0.000039457070707 * u + 1.139827967171717 * v)
    go = int(-0.394610164141414 * u - 0.580500315656566 * v)
    bo = int( 2.031999684343434 * u - 0.000481376262626 * v)
    return _rgb565_roundtrip(y + ro, y + go, y + bo)


def _rgb_to_yuv_est(rgb):
    r, g, b = rgb
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = (b - y) / 2.031999684343434 + 128.0
    v = (r - y) / 1.139827967171717 + 128.0
    return y, u, v


def rgb_pair_to_pcfx_16m_words(rgb0, rgb1):
    """Return two 16-bit words for KING_BGMODE_16M.

    pcfxemu documents/implements KING 16M as four bytes for two pixels:
      word0 high byte = Y0, word0 low byte = Y1,
      word1 high byte = shared U, word1 low byte = shared V.
    Y==0 is transparent, so generated luma is clamped to 1.
    """
    r0, g0, b0 = rgb0
    r1, g1, b1 = rgb1
    _, u0, v0 = _rgb_to_yuv_est(rgb0)
    _, u1, v1 = _rgb_to_yuv_est(rgb1)
    ue = int(round((u0 + u1) * 0.5))
    ve = int(round((v0 + v1) * 0.5))
    best = (1, 1, 128, 128)
    best_err = 10**18
    # Shared chroma is the only lossy component.  Search a small neighbourhood
    # around the continuous estimate; solve luma directly for each candidate.
    for u8 in range(max(0, ue - 5), min(255, ue + 5) + 1):
        u = u8 - 128
        for v8 in range(max(0, ve - 5), min(255, ve + 5) + 1):
            v = v8 - 128
            ro = int(-0.000039457070707 * u + 1.139827967171717 * v)
            go = int(-0.394610164141414 * u - 0.580500315656566 * v)
            bo = int( 2.031999684343434 * u - 0.000481376262626 * v)
            y0 = clamp(int(round((2 * (r0 - ro) + 4 * (g0 - go) + (b0 - bo)) / 7.0)), 1, 255)
            y1 = clamp(int(round((2 * (r1 - ro) + 4 * (g1 - go) + (b1 - bo)) / 7.0)), 1, 255)
            rr0, gg0, bb0 = yuv888_to_rgb565_rt(y0, u8, v8)
            rr1, gg1, bb1 = yuv888_to_rgb565_rt(y1, u8, v8)
            dr0 = rr0 - r0; dg0 = gg0 - g0; db0 = bb0 - b0
            dr1 = rr1 - r1; dg1 = gg1 - g1; db1 = bb1 - b1
            err = 2 * dr0 * dr0 + 4 * dg0 * dg0 + db0 * db0 + 2 * dr1 * dr1 + 4 * dg1 * dg1 + db1 * db1
            if err < best_err:
                best_err = err
                best = (y0, y1, u8, v8)
    y0, y1, u8, v8 = best
    return ((y0 << 8) | y1, (u8 << 8) | v8)

def build_pcfx_title_yuv422_16m(img: Image.Image):
    """Return KING_BGMODE_16M words: two 16-bit words per two pixels."""
    pix = list(img.getdata())
    out = []
    cache = {}
    for y in range(H):
        row = y * W
        for x in range(0, W, 2):
            rgb0 = pix[row + x]
            rgb1 = pix[row + x + 1]
            key = (rgb0, rgb1)
            words = cache.get(key)
            if words is None:
                words = rgb_pair_to_pcfx_16m_words(rgb0, rgb1)
                cache[key] = words
            out.extend(words)
    return out



def bake_pcfx_title_logo(img: Image.Image, title_pal, idx_black: int, idx_white: int, idx_gold_hi: int):
    """Bake the static title logo into the PC-FX direct-KRAM 16M asset.

    Runtime PC-FX title/menu changes are VDC-only; the KING title surface is
    uploaded once from CD to KRAM and is never rebuilt in CPU RAM.  The logo
    therefore belongs in the disc image, not in a transient framebuffer.
    """
    font_header = ROOT / 'src/engine/font_menudata.h'
    text = font_header.read_text()
    font = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', text)]
    pix = img.load()

    def rect_fill(x, y, w, h, rgb):
        x0 = max(0, x); y0 = max(0, y)
        x1 = min(W, x + w); y1 = min(H, y + h)
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                pix[xx, yy] = rgb

    def draw_text_scaled(x, y, msg, scale, fg_idx, shadow_idx):
        fg = title_pal[fg_idx]
        shadow = title_pal[shadow_idx]
        cx = x
        for ch in msg:
            code = ord(ch) & 0x7f
            for yy in range(8):
                row = font[code * 8 + yy]
                for xx in range(8):
                    if row & (1 << (7 - xx)):
                        rect_fill(cx + xx * scale + 2, y + yy * scale + 2, scale, scale, shadow)
                        rect_fill(cx + xx * scale, y + yy * scale, scale, scale, fg)
            cx += 8 * scale

    def draw_centered(y, msg, scale, fg_idx, shadow_idx):
        draw_text_scaled((W - len(msg) * 8 * scale) // 2, y, msg, scale, fg_idx, shadow_idx)

    draw_centered(20, 'SHATTERED', 2, idx_gold_hi, idx_black)
    draw_centered(38, 'DECKS', 2, idx_white, idx_black)


def pad_pcfx_16m_page(words):
    """Pad/shift visible 256x240 16M words to a full 256x256 KING page.

    Runtime scrolls the KING 16M title layer down by TITLE_PCFX_16M_SCROLL_Y
    scanlines.  Store source row 0 at KRAM row scroll_y, so the 240 visible
    screen rows sample source rows 0..239 rather than source rows 4..239 plus
    repeated padding at the bottom.
    """
    if len(words) != W * H:
        raise RuntimeError(f'expected {W*H} visible 16M words, got {len(words)}')
    out = []
    for dst_y in range(256):
        src_y = dst_y - TITLE_PCFX_16M_SCROLL_Y
        if src_y < 0:
            src_y = 0
        elif src_y >= H:
            src_y = H - 1
        out.extend(words[src_y * W:(src_y + 1) * W])
    return out

def main():
    if not SRC.exists():
        raise FileNotFoundError(SRC)
    if not SRC_ENDING.exists():
        raise FileNotFoundError(SRC_ENDING)
    if not PAL_HEADER.exists():
        raise FileNotFoundError(PAL_HEADER)

    common, reserved = parse_common_assets(PAL_HEADER)
    idx_defs = {name: int(value) for name, value in re.findall(r'^#define\s+(IDX_[A-Z0-9_]+)\s+(\d+)\s*$', PAL_HEADER.read_text(), re.M)}
    free_slots = [i for i in range(256) if i not in reserved]
    if not free_slots:
        raise RuntimeError('No non-IDX palette slots left for title art')

    img = ImageOps.fit(Image.open(SRC).convert('RGB'), (W, H), method=Image.Resampling.BILINEAR, centering=(0.5, 0.5))
    title_colors = quantize_title_palette(img, len(free_slots))

    # Start with the common palette so every reserved text/UI slot is identical.
    # Non-reserved slots are then replaced with title-art colors.
    title_pal = list(common)
    for n, slot in enumerate(free_slots):
        title_pal[slot] = title_colors[n % len(title_colors)]

    # Do not allow the normal title bitmap to use reserved IDX_* slots; they are
    # stable for runtime text/panel drawing only.
    data = assign_pixels_to_palette(img, title_pal, free_slots)

    # PC-FX title is now normally presented through KING_BGMODE_64K, so the
    # indexed PC-FX title is only a fallback.  Keep it cheap and deterministic.
    pcfx_yuv = [rgb888_to_pcfx_yuv(c) for c in title_pal]
    pcfx_data = list(data)
    pcfx_yuv16 = build_pcfx_title_yuv16(img)
    pcfx_16m_img = img.copy()
    bake_pcfx_title_logo(pcfx_16m_img, title_pal, idx_defs['IDX_BLACK'], idx_defs['IDX_WHITE'], idx_defs['IDX_GOLD_HI'])
    pcfx_yuv422 = build_pcfx_title_yuv422_16m(pcfx_16m_img)
    pcfx_yuv422_kram = pad_pcfx_16m_page(pcfx_yuv422)
    ending_img = ImageOps.fit(Image.open(SRC_ENDING).convert('RGB'), (W, H), method=Image.Resampling.BILINEAR, centering=(0.5, 0.5))
    ending_yuv422 = build_pcfx_title_yuv422_16m(ending_img)
    ending_yuv422_kram = pad_pcfx_16m_page(ending_yuv422)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('w') as f:
        f.write('#ifndef TITLE_ASSET_H\n#define TITLE_ASSET_H\n\n')
        f.write('#include <stdint.h>\n\n')
        f.write('#define TITLE_SCREEN_W 256\n#define TITLE_SCREEN_H 240\n')
        f.write('static const uint8_t title_screen_palette_rgb[256 * 3] = {\n')
        flat_pal = [v for rgb in title_pal for v in rgb]
        for i in range(0, len(flat_pal), 18):
            f.write('    ' + ','.join(str(v) for v in flat_pal[i:i + 18]) + ',\n')
        f.write('};\n')
        f.write('static const uint16_t title_screen_palette_pcfx_yuv[256] = {\n')
        for i in range(0, len(pcfx_yuv), 12):
            f.write('    ' + ','.join(f'0x{v:04x}' for v in pcfx_yuv[i:i + 12]) + ',\n')
        f.write('};\n')
        f.write('#ifndef WAIFU_ASSET_EXTERNAL_TITLE_IMAGE\n')
        f.write('static const uint8_t title_screen_img[TITLE_SCREEN_W * TITLE_SCREEN_H] = {\n')
        for i in range(0, len(data), 32):
            f.write('    ' + ','.join(str(v) for v in data[i:i + 32]) + ',\n')
        f.write('};\n')
        f.write('static const uint16_t title_screen_pcfx_yuv16[TITLE_SCREEN_W * TITLE_SCREEN_H] = {\n')
        for i in range(0, len(pcfx_yuv16), 12):
            f.write('    ' + ','.join(f'0x{v:04x}' for v in pcfx_yuv16[i:i + 12]) + ',\n')
        f.write('};\n')
        f.write('static const uint16_t title_screen_pcfx_yuv422[TITLE_SCREEN_W * TITLE_SCREEN_H] = {\n')
        for i in range(0, len(pcfx_yuv422), 12):
            f.write('    ' + ','.join(f'0x{v:04x}' for v in pcfx_yuv422[i:i + 12]) + ',\n')
        f.write('};\n')
        f.write('#endif /* WAIFU_ASSET_EXTERNAL_TITLE_IMAGE */\n\n')
        f.write('#ifndef WAIFU_ASSET_EXTERNAL_ENDING_IMAGE\n')
        f.write('static const uint16_t ending_screen_pcfx_yuv422[TITLE_SCREEN_W * TITLE_SCREEN_H] = {\n')
        for i in range(0, len(ending_yuv422), 12):
            f.write('    ' + ','.join(f'0x{v:04x}' for v in ending_yuv422[i:i + 12]) + ',\n')
        f.write('};\n')
        f.write('#endif /* WAIFU_ASSET_EXTERNAL_ENDING_IMAGE */\n\n#endif\n')
    bin_out = ROOT / 'assets/generated'
    bin_out.mkdir(parents=True, exist_ok=True)
    (bin_out / 'title_screen_img.bin').write_bytes(bytes(data))
    (bin_out / 'title_screen_pcfx_img.bin').write_bytes(bytes(pcfx_data))
    yuv16_bytes = bytearray()
    for w in pcfx_yuv16:
        yuv16_bytes.append(w & 0xff)
        yuv16_bytes.append((w >> 8) & 0xff)
    (bin_out / 'title_screen_pcfx_yuv16.bin').write_bytes(bytes(yuv16_bytes))
    yuv422_bytes = bytearray()
    for w in pcfx_yuv422_kram:
        yuv422_bytes.append(w & 0xff)
        yuv422_bytes.append((w >> 8) & 0xff)
    (bin_out / 'title_screen_pcfx_yuv422.bin').write_bytes(bytes(yuv422_bytes))
    ending_yuv422_bytes = bytearray()
    for w in ending_yuv422_kram:
        ending_yuv422_bytes.append(w & 0xff)
        ending_yuv422_bytes.append((w >> 8) & 0xff)
    (bin_out / 'ending_screen_pcfx_yuv422.bin').write_bytes(bytes(ending_yuv422_bytes))
    print(f'wrote {OUT} with {len(free_slots)} title-art palette slots and {len(reserved)} preserved IDX slots')
    print('wrote assets/generated/title_screen_img.bin, title_screen_pcfx_img.bin, title_screen_pcfx_yuv16.bin, title_screen_pcfx_yuv422.bin, and ending_screen_pcfx_yuv422.bin')


if __name__ == '__main__':
    main()
