#!/usr/bin/env python3
from PIL import Image, ImageDraw, ImageOps, ImageFilter, ImageChops
from pathlib import Path
import math, os, re

ROOT = Path(__file__).resolve().parents[1]
CARD_DIR = ROOT/'assets/source/cards'
CARD_DATA = CARD_DIR/'card_data.txt'
if not CARD_DIR.exists():
    # Development fallback used by this ChatGPT build environment. The generated
    # header is committed, so the distributed package does not require these PNGs.
    alt = Path('/mnt/data/card_game_unzip/cards')
    if alt.exists(): CARD_DIR = alt
OUT = ROOT/'src/generated/waifu_assets.h'
CARD_IDS_OUT = ROOT/'src/game/card_ids.h'
DECK_POOLS_OUT = ROOT/'src/generated/deck_pools.h'
CARD_W, CARD_H = 38, 54
BIG_W, BIG_H = 112, 112
TILE = 32
PORTRAIT_W, PORTRAIT_H = 124, 200
CD_SECTOR = 2048
PORTRAIT_BYTES = PORTRAIT_W * PORTRAIT_H
PORTRAIT_CD_STRIDE = ((PORTRAIT_BYTES + CD_SECTOR - 1) // CD_SECTOR) * CD_SECTOR
PORTRAIT_DIR = ROOT/'assets/source/story_portraits'
STORY_PORTRAITS = ['serena.png','opponent_0.png','opponent_1.png','opponent_2.png','opponent_3.png','opponent_4.png']

def card_macro(asset_id):
    s = re.sub(r'[^A-Za-z0-9]+', '_', asset_id).upper()
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', asset_id).upper()
    s = re.sub(r'[^A-Z0-9]+', '_', s).strip('_')
    return 'WAIFU_CARD_ID_' + s

def c_string(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

def parse_card_data(path):
    cards = []
    pools = {}
    seen_cards = set()
    if not path.exists():
        raise FileNotFoundError(path)
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split('|')]
        kind = parts[0]
        if kind == 'card':
            if len(parts) != 8:
                raise SystemExit(f'{path}:{lineno}: card rows need 8 fields')
            asset_id, name, tribe, attr, atk, deff, desc = parts[1:]
            if asset_id in seen_cards:
                raise SystemExit(f'{path}:{lineno}: duplicate card {asset_id}')
            seen_cards.add(asset_id)
            cards.append((asset_id, name, tribe, attr, int(atk), int(deff), desc))
        elif kind == 'pool':
            if len(parts) != 3:
                raise SystemExit(f'{path}:{lineno}: pool rows need 3 fields')
            pool_name = parts[1]
            if pool_name in pools:
                raise SystemExit(f'{path}:{lineno}: duplicate pool {pool_name}')
            pools[pool_name] = [p.strip() for p in parts[2].split(',') if p.strip()]
        else:
            raise SystemExit(f'{path}:{lineno}: unknown row kind {kind}')
    card_ids = {card[0]: i for i, card in enumerate(cards)}
    for pool_name, entries in pools.items():
        for entry in entries:
            if entry not in card_ids:
                raise SystemExit(f'{path}: pool {pool_name} references unknown card {entry}')
    return cards, pools, card_ids

CARD_META, CARD_POOLS, CARD_ID_BY_ASSET = parse_card_data(CARD_DATA)

BASE_COLORS = {
    'BLACK': (0,0,0), 'WHITE': (232,232,224), 'DIM': (48,48,55),
    'UI_DARK': (7,10,43), 'UI_BLUE': (18,34,93), 'UI_LIGHT': (144,164,219),
    'UI_RED': (176,20,22), 'UI_TEAL': (5,42,46), 'UI_TEAL2': (9,65,69),
    'GOLD': (222,164,42), 'GOLD_HI': (248,220,83), 'GOLD_DARK': (113,54,18),
    'STONE': (84,80,70), 'STONE_HI': (152,143,117), 'BROWN': (68,25,8),
    'DARK_BROWN': (24,8,4), 'GREEN': (21,82,46), 'RED': (220,14,14),
    'BLUE_WHITE': (145,208,255), 'FLAME1': (255,221,72), 'FLAME2': (246,112,12),
    'FLAME3': (128,28,8), 'CARD_GOLD': (188,128,38), 'CARD_RIM': (59,35,13),
    'MAGIC_GREEN': (23,126,62), 'TRAP_MAGENTA': (114,35,115),
}

def find_image(card_id):
    for ext in ['.png','.webp','.jpg','.jpeg']:
        p = CARD_DIR/(card_id+ext)
        if p.exists(): return p
    raise FileNotFoundError(card_id)

def card_content_bbox(img):
    # Source images are not guaranteed to be 1:1. Some also carry transparent
    # or flat-color margins. Trim only those safe borders before fitting so the
    # card conversion never produces black bars or empty letterbox space.
    rgba = img.convert('RGBA')
    alpha = rgba.getchannel('A')
    if alpha.getextrema()[0] < 255:
        bbox = alpha.point(lambda p: 255 if p > 8 else 0).getbbox()
        if bbox:
            return bbox
    rgb = img.convert('RGB')
    bg = Image.new('RGB', rgb.size, rgb.getpixel((0, 0)))
    diff = ImageChops.difference(rgb, bg).convert('L')
    bbox = diff.point(lambda p: 255 if p > 8 else 0).getbbox()
    if bbox:
        l, t, r, b = bbox
        mx = max(1, (r - l) // 30)
        my = max(1, (b - t) // 30)
        return (max(0, l - mx), max(0, t - my), min(img.width, r + mx), min(img.height, b + my))
    return (0, 0, img.width, img.height)

def prepared_card_source(img):
    return img.crop(card_content_bbox(img)).convert('RGB')

def cover(img, size):
    # Fill the square battle-art target by cropping rather than padding. This
    # deliberately avoids black bars on portrait/tall monster sources.
    return ImageOps.fit(prepared_card_source(img), size, method=Image.Resampling.BILINEAR, centering=(0.5,0.45))

def card_thumb_crop(img, size):
    # Hand/field thumbnails should emphasize the face/upper torso region.
    # Fit from the trimmed source and crop to fill the thumbnail, never pad.
    return ImageOps.fit(prepared_card_source(img), size, method=Image.Resampling.BILINEAR, centering=(0.5,0.36))


def fit_story_portrait(img, size=(PORTRAIT_W, PORTRAIT_H)):
    img = img.convert('RGBA')
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    w, h = img.size
    # For dialogue scenes, emphasize the upper body and hands instead of the
    # full standing figure so the visible portion above the bottom text box is
    # expressive once the portrait is pinned to the screen bottom.
    top = int(h * 0.01)
    bottom = max(top + 1, int(h * 0.80))
    left = int(w * 0.05)
    right = max(left + 1, int(w * 0.95))
    img = img.crop((left, top, right, bottom))
    bg = Image.new('RGBA', size, (0,0,0,0))
    art = ImageOps.contain(img, (int(size[0] * 1.02), int(size[1] * 1.02)), method=Image.Resampling.BILINEAR)
    x = (size[0] - art.width) // 2
    y = size[1] - art.height
    bg.alpha_composite(art, (x, y))
    return bg

def load_story_portraits_rgba():
    portraits = []
    for fn in STORY_PORTRAITS:
        portraits.append(fit_story_portrait(Image.open(PORTRAIT_DIR / fn)))
    return portraits

def draw_card_face(card_id, name, tribe, attr, atk, deff):
    img = Image.new('RGB', (CARD_W,CARD_H), (51,32,14))
    d = ImageDraw.Draw(img)
    # bevelled card frame, deliberately chunky for 256x240
    d.rectangle([1,0,CARD_W-2,CARD_H-1], fill=(213,156,48))
    d.rectangle([2,2,CARD_W-3,CARD_H-3], fill=(72,42,16))
    d.rectangle([3,3,CARD_W-4,CARD_H-4], fill=(192,132,39))
    # type strip
    d.rectangle([4,4,CARD_W-5,7], fill=(228,181,63))
    # art area
    art = card_thumb_crop(Image.open(find_image(card_id)), (30,30))
    # PS1-like pre-crunch: lower color detail, mild sharpness
    art = art.filter(ImageFilter.SHARPEN)
    img.paste(art, (4,9))
    d.rectangle([4,9,33,38], outline=(32,19,8))
    # text/stat region
    d.rectangle([4,40,33,50], fill=(230,190,96))
    d.rectangle([5,41,32,49], fill=(54,42,28))
    # tiny fake stat marks
    d.line([7,43,14,43], fill=(236,220,150))
    d.line([7,46,15,46], fill=(236,220,150))
    d.line([21,43,30,43], fill=(236,220,150))
    d.line([20,46,30,46], fill=(236,220,150))
    # stars based on level-ish power
    stars = max(1, min(8, (atk+deff)//700))
    for s in range(stars):
        x = 5 + s*3
        d.ellipse([x,5,x+1,6], fill=(170,24,18))
    return img

def draw_support_emblem(size):
    """Colorful arcane sigil used as the inner 'art' of support/trap cards."""
    em = Image.new('RGB', (size, size), (16, 26, 48))
    d = ImageDraw.Draw(em)
    cx = (size - 1) / 2.0
    cy = (size - 1) / 2.0
    spoke_cols = [(96,224,255),(255,214,96),(120,210,255),
                  (255,170,90),(150,255,170),(210,240,255)]
    rmax = size * 0.46
    lw = max(1, size // 18)
    n = 12
    for k in range(n):
        ang = math.pi * 2 * k / n
        x2 = cx + math.cos(ang) * rmax
        y2 = cy + math.sin(ang) * rmax
        d.line([cx, cy, x2, y2], fill=spoke_cols[k % len(spoke_cols)], width=lw)
    ring_cols = [(255,214,96),(96,224,255),(255,170,90)]
    for i, rr in enumerate([0.22, 0.34, 0.45]):
        r = size * rr
        for t in range(max(1, size // 40)):
            d.ellipse([cx-r+t, cy-r+t, cx+r-t, cy+r-t], outline=ring_cols[i % len(ring_cols)])
    gr = max(2, int(size * 0.14))
    d.ellipse([cx-gr-1, cy-gr-1, cx+gr+1, cy+gr+1], fill=(20,30,60))
    d.ellipse([cx-gr, cy-gr, cx+gr, cy+gr], fill=(120,210,255))
    hl = max(1, gr // 2)
    d.ellipse([cx-hl, cy-hl, cx+hl, cy+hl], fill=(255,255,255))
    return em

def draw_support_face():
    # Support/trap cards use the same bevelled card frame as monsters, but in a
    # distinct arcane-blue colour with a colourful sigil in the art window.
    img = Image.new('RGB', (CARD_W,CARD_H), (12,26,44))
    d = ImageDraw.Draw(img)
    d.rectangle([1,0,CARD_W-2,CARD_H-1], fill=(60,150,208))
    d.rectangle([2,2,CARD_W-3,CARD_H-3], fill=(14,40,66))
    d.rectangle([3,3,CARD_W-4,CARD_H-4], fill=(38,108,164))
    # type strip
    d.rectangle([4,4,CARD_W-5,7], fill=(120,196,232))
    # art window with the colourful sigil
    img.paste(draw_support_emblem(30), (4,9))
    d.rectangle([4,9,33,38], outline=(10,28,46))
    # stat/text strip
    d.rectangle([4,40,33,50], fill=(120,190,222))
    d.rectangle([5,41,32,49], fill=(22,46,70))
    d.line([7,43,14,43], fill=(150,220,245))
    d.line([7,46,15,46], fill=(150,220,245))
    d.line([21,43,30,43], fill=(150,220,245))
    d.line([20,46,30,46], fill=(150,220,245))
    # gem dots along the type strip
    for s in range(4):
        x = 5 + s*3
        d.ellipse([x,5,x+1,6], fill=(248,236,140))
    return img

def draw_card_back():
    img = Image.new('RGB', (CARD_W,CARD_H), (46,24,8))
    d = ImageDraw.Draw(img)
    d.rectangle([1,0,CARD_W-2,CARD_H-1], fill=(205,132,35))
    d.rectangle([3,3,CARD_W-4,CARD_H-4], fill=(15,8,4))
    cx, cy = CARD_W//2, CARD_H//2
    colors=[(230,136,18),(140,70,8),(250,187,34)]
    for k in range(16):
        r = 3+k*2
        box=[cx-r,cy-r,cx+r,cy+r]
        d.arc(box, k*22, k*22+230, fill=colors[k%3], width=2)
    d.rectangle([1,0,CARD_W-2,CARD_H-1], outline=(35,19,7))
    d.rectangle([2,2,CARD_W-3,CARD_H-3], outline=(240,169,45))
    return img

def tile_gold(variant=0):
    img=Image.new('RGB',(TILE,TILE),(190,111,18))
    for y in range(TILE):
        for x in range(TILE):
            # Keep the gold board texture free of baked-in borders or hard
            # diagonals. Those strokes were being projected with the normal
            # per-cell two-triangle renderer and read as texture seams,
            # especially on the bright yellow tile.  Use only soft periodic
            # variation here; the board grid is responsible for tile borders.
            diag=(x+y+variant*9)&31
            wave=16-abs(diag-16)
            grain=(x*5+y*3+variant*17)&15
            r=194+wave*2+grain
            g=124+wave+grain//2
            b=22+wave//4
            if ((x*2+y+variant*7)&31) < 9:
                r += 16; g += 18; b += 7
            base=(r,g,b)
            img.putpixel((x,y), tuple(min(255,c) for c in base))
    return img

def tile_sand():
    img=Image.new('RGB',(TILE,TILE),(198,162,96))
    for y in range(TILE):
        for x in range(TILE):
            # Desert ground needs more visible texture than the board tiles:
            # layered dune ripples, granular noise, and a few pebble specks,
            # while still remaining seamless when tiled over the full 3D map.
            grain = ((x*13 + y*9) ^ (x*7 + y*5) ^ (x*y*3)) & 15
            ripple_a = 16 - abs(((x + y*2) & 31) - 16)
            ripple_b = 16 - abs((((x*3) - y*2) & 31) - 16)
            ripple_c = 16 - abs((((x*5) + y) & 31) - 16)
            dune = (ripple_a*3 + ripple_b*2 + ripple_c*2) // 7
            shade = dune - 8

            # fine wind streaks and granular breakup
            streak = 0
            streak_phase = (x*3 + y*5) & 31
            if streak_phase < 4:
                streak = 8 - streak_phase*2
            elif streak_phase > 27:
                streak = -(streak_phase - 27) * 2

            micro = grain - 7
            r = 198 + shade*3 + streak + micro
            g = 162 + shade*2 + streak//2 + micro//2
            b = 96 + shade + streak//3

            # sparse pebbles / darker flecks to stop the floor reading as flat.
            pebble = ((x*11 + y*17 + x*y) & 63)
            if pebble == 0:
                r -= 26; g -= 22; b -= 14
            elif pebble in (1, 2):
                r += 14; g += 10; b += 4

            # small lighter sand clusters.
            patch = ((x*5 - y*3) & 31)
            if 9 <= patch <= 12:
                r += 8; g += 6

            r=max(0, min(255, r))
            g=max(0, min(255, g))
            b=max(0, min(255, b))
            img.putpixel((x,y), (r,g,b))
    return img

def tile_stone():
    img=Image.new('RGB',(TILE,TILE),(76,70,61)); d=ImageDraw.Draw(img)
    for y in range(TILE):
        for x in range(TILE):
            v=(x*7+y*11)%43
            img.putpixel((x,y),(64+v,58+v,50+v//2))
    for y in [8,16,24]: d.line([0,y,31,y], fill=(32,26,22))
    for x in [6,17,26]: d.line([x,0,x,31], fill=(32,26,22))
    return img

def tile_side_wall():
    # Vertical edge material for the 3D board slab.  Keep it low-contrast and
    # mostly horizontal so it reads as the side of the field, not as another
    # playable checker tile.
    img=Image.new('RGB',(TILE,TILE),(70,37,13)); d=ImageDraw.Draw(img)
    for y in range(TILE):
        t = y / float(TILE - 1)
        base = int(102 - 50 * t)
        img_line = (base, max(22, int(60 - 34 * t)), max(7, int(18 - 9 * t)))
        d.line([0,y,31,y], fill=img_line)
    # top bevel and dark lower lip
    d.line([0,0,31,0], fill=(214,146,45))
    d.line([0,1,31,1], fill=(154,83,23))
    d.line([0,30,31,30], fill=(28,10,4))
    d.line([0,31,31,31], fill=(12,4,2))
    # subtle block seams; these must be straight and local when the wall is
    # drawn in cell-sized segments.
    for x in [7,16,25]:
        d.line([x,3,x,28], fill=(44,18,6))
        if x + 1 < TILE:
            d.line([x+1,3,x+1,28], fill=(118,62,17))
    for y in [10,20]:
        d.line([1,y,30,y], fill=(49,20,6))
        d.line([1,y+1,30,y+1], fill=(111,58,17))
    return img

def tile_dark(): return Image.new('RGB',(TILE,TILE),(0,0,0))

def tile_brown():
    img=Image.new('RGB',(TILE,TILE),(52,19,7)); d=ImageDraw.Draw(img)
    for y in range(0,TILE,4): d.line([0,y,31,y], fill=(25,8,3))
    for x in range(0,TILE,8): d.line([x,0,x,31], fill=(100,42,10))
    return img

def tile_volcanic_ground():
    img = Image.new('RGB', (TILE, TILE), (74, 39, 22))
    for y in range(TILE):
        for x in range(TILE):
            grain = ((x*9 + y*11) ^ (x*3 + y*5) ^ (x*y)) & 15
            crack = ((x*5 - y*3) & 31)
            ridge = 16 - abs((((x*2) + y*3) & 31) - 16)
            shade = ridge - 8 + grain//2
            r = 82 + shade*2 + grain
            g = 46 + shade + grain//2
            b = 27 + shade//2
            if crack in (0,1):
                r -= 26; g -= 18; b -= 12
            elif crack in (14,15,16):
                r += 10; g += 5
            ember = ((x*13 + y*7 + x*y*2) & 63)
            if ember == 0:
                r += 26; g += 9; b += 2
            img.putpixel((x,y), (max(0,min(255,r)), max(0,min(255,g)), max(0,min(255,b))))
    return img

def tile_volcanic_slope():
    img = Image.new('RGB', (TILE, TILE), (92, 47, 28))
    for y in range(TILE):
        for x in range(TILE):
            band = 16 - abs((((x*3) + (y*5)) & 31) - 16)
            grain = ((x*7 + y*13) ^ (x*y*5)) & 15
            r = 96 + band*2 + grain
            g = 52 + band + grain//2
            b = 30 + band//2
            # Jagged obsidian-like seams.
            seam = ((x*4 - y*3) & 31)
            if seam in (0,1,2):
                r -= 28; g -= 20; b -= 16
            elif seam in (15,16):
                r += 12; g += 6; b += 1
            # Sparse glowing fissures.
            fissure = ((x*11 + y*17 + x*y) & 127)
            if fissure == 0:
                r += 34; g += 14; b += 3
            elif fissure == 1:
                r += 18; g += 8
            img.putpixel((x,y), (max(0,min(255,r)), max(0,min(255,g)), max(0,min(255,b))))
    return img

# Make master palette image.
card_faces_rgb=[draw_card_face(*m[:6]) for m in CARD_META]
# Full-size 112x112 battle/check art.  This is separate from the 38x54
# gameplay card face so detail screens never upscale the tiny thumbnail.
big_card_rgb=[cover(Image.open(find_image(m[0])), (BIG_W, BIG_H)).filter(ImageFilter.SHARPEN) for m in CARD_META]
support_rgb=draw_support_face()
# Big art window is the inner sigil only (the card frame is drawn by the C code),
# rendered crisply at full size rather than upscaled from the 38x54 face.
support_big_rgb=draw_support_emblem(BIG_W)
back_rgb=draw_card_back()
tex_rgb=[tile_dark(),tile_gold(0),tile_sand(),tile_stone(),tile_side_wall(),tile_brown(),tile_volcanic_ground(),tile_volcanic_slope(),back_rgb.resize((TILE,TILE), Image.Resampling.NEAREST)]
story_portraits_rgba=load_story_portraits_rgba()

def build_palette_image(images):
    # Master swatches are heavily weighted so UI colors survive quantization.
    swatches=[]
    for c in BASE_COLORS.values():
        swatches += [c]*64
    for im in images:
        small=im.resize((max(1,im.width//2), max(1,im.height//2)), Image.Resampling.BILINEAR)
        swatches.extend(list(small.getdata()))
    master=Image.new('RGB',(256, max(1, math.ceil(len(swatches)/256))))
    master.putdata(swatches + [(0,0,0)]*(master.width*master.height-len(swatches)))
    pal=master.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette=pal.getpalette()[:768]
    palette += [0] * (768 - len(palette))
    return pal, palette

def nearest_idx_in_palette(palette, rgb):
    best=0; bd=10**9
    for i in range(256):
        pr,pg,pb=palette[i*3:i*3+3]
        d=(pr-rgb[0])**2+(pg-rgb[1])**2+(pb-rgb[2])**2
        if d<bd:
            best=i; bd=d
    return best

def nearest_nonzero_idx_in_palette(palette, rgb):
    best=1; bd=10**9
    for i in range(1, 256):
        pr,pg,pb=palette[i*3:i*3+3]
        d=(pr-rgb[0])**2+(pg-rgb[1])**2+(pb-rgb[2])**2
        if d<bd:
            best=i; bd=d
    return best

def apply_base_colors(pal_img, palette, idx_map):
    for name,rgb in BASE_COLORS.items():
        i = idx_map[name] * 3
        palette[i:i+3] = list(rgb)
    pal_img.putpalette(palette)

common_palette_images = card_faces_rgb + big_card_rgb + [support_rgb,support_big_rgb,back_rgb] + tex_rgb
dialogue_palette_images = [p.convert('RGB') for p in story_portraits_rgba]
pal_img,palette=build_palette_image(common_palette_images)
dialogue_pal_img,dialogue_palette=build_palette_image(dialogue_palette_images)

# Constants for named palette slots. Keep one shared set of indexes and stamp
# those exact RGB values into every runtime palette so UI/text colors are stable.
idx = {name:nearest_idx_in_palette(palette, rgb) for name,rgb in BASE_COLORS.items()}
apply_base_colors(pal_img, palette, idx)
apply_base_colors(dialogue_pal_img, dialogue_palette, idx)

def qbytes_with_palette(im, quant_palette):
    return bytes(im.convert('RGB').quantize(palette=quant_palette, dither=Image.Dither.NONE).tobytes())

def qbytes(im):
    return qbytes_with_palette(im, pal_img)

def qbytes_card_art(im):
    rgb_im = im.convert('RGB')
    data = bytearray(qbytes_with_palette(rgb_im, pal_img))
    if 0 in data:
        rgb = list(rgb_im.getdata())
        for i, px in enumerate(data):
            if px == 0:
                data[i] = nearest_nonzero_idx_in_palette(palette, rgb[i])
    return bytes(data)

def qbytes_dialogue(im):
    return qbytes_with_palette(im, dialogue_pal_img)

q_cards=[qbytes_card_art(im) for im in card_faces_rgb]
q_big_cards=[qbytes_card_art(im) for im in big_card_rgb]
q_support=qbytes_card_art(support_rgb)
q_support_big=qbytes_card_art(support_big_rgb)
q_back=qbytes_card_art(back_rgb)
q_tex=[qbytes(im) for im in tex_rgb]

card_art_blobs = q_cards + q_big_cards + [q_support, q_support_big, q_back]
if any(0 in blob for blob in card_art_blobs):
    raise RuntimeError('card art blobs must not use palette index 0')

# Keep the 3D texture atlas identical between the common and dialogue palettes.
# The story-dialogue (plaza) scene renders the same pyramid/sky geometry as the
# map and sanctum, but switches to the dialogue palette so the portraits look
# right.  The texture atlas is quantized against the COMMON palette, so under
# the dialogue palette those same indices map to portrait colors and the pyramid
# texture visibly shifts hue between the map and the dialogue.  Reserve exactly
# the palette indices the texture atlas uses to their common-palette RGB, then
# re-quantize the portraits around the reserved slots so the pyramid is stable.
tex_reserved_indices = sorted(set(b''.join(q_tex)))
for _i in tex_reserved_indices:
    dialogue_palette[_i*3:_i*3+3] = palette[_i*3:_i*3+3]
dialogue_pal_img.putpalette(dialogue_palette)

q_story_portraits=[qbytes_dialogue(im.convert('RGB')) for im in story_portraits_rgba]
q_story_portrait_masks=[bytes([255 if px[3] >= 16 else 0 for px in im.getdata()]) for im in story_portraits_rgba]

# write header
OUT.parent.mkdir(parents=True, exist_ok=True)
with open(OUT,'w') as f:
    f.write('/* Generated by tools/gen_assets.py. 256x240 indexed assets. */\n')
    f.write('#ifndef WAIFU_ASSETS_H\n#define WAIFU_ASSETS_H\n#include <stdint.h>\n')
    f.write(f'#define WAIFU_CARD_COUNT {len(CARD_META)}\n#define WAIFU_CARD_W {CARD_W}\n#define WAIFU_CARD_H {CARD_H}\n#define WAIFU_BIG_W {BIG_W}\n#define WAIFU_BIG_H {BIG_H}\n#define WAIFU_TEX_TILE_SIZE {TILE}\n#define WAIFU_TEX_TILE_COUNT {len(tex_rgb)}\n#define WAIFU_STORY_PORTRAIT_COUNT {len(STORY_PORTRAITS)}\n#define WAIFU_STORY_PORTRAIT_W {PORTRAIT_W}\n#define WAIFU_STORY_PORTRAIT_H {PORTRAIT_H}\n#define WAIFU_STORY_PORTRAIT_CD_STRIDE {PORTRAIT_CD_STRIDE}\n')
    for name,val in idx.items():
        f.write(f'#define IDX_{name} {val}\n')
    def array(name, data, width=16):
        f.write(f'static const uint8_t {name}[] = {{\n')
        for i in range(0,len(data),width):
            f.write('    '+','.join(str(b) for b in data[i:i+width])+',\n')
        f.write('};\n')
    array('waifu_palette_rgb', bytes(palette), 18)
    array('waifu_dialogue_palette_rgb', bytes(dialogue_palette), 18)
    # texture atlas concatenated
    array('waifu_texture_atlas', b''.join(q_tex), 16)
    f.write('#ifndef WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS\n')
    array('waifu_story_portraits', b''.join(q_story_portraits), 16)
    array('waifu_story_portrait_mask', b''.join(q_story_portrait_masks), 16)
    f.write('#endif /* WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS */\n')
    # cards as flat array. CD-ROM/low-RAM builds externalize these blobs and
    # stage them only when entering deck editor/battle.
    f.write('#ifndef WAIFU_ASSET_EXTERNAL_CARD_IMAGES\n')
    array('waifu_card_faces', b''.join(q_cards), 16)
    array('waifu_big_card_art', b''.join(q_big_cards), 16)
    array('waifu_card_back', q_back, 16)
    array('waifu_support_face', q_support, 16)
    array('waifu_support_big_art', q_support_big, 16)
    f.write('#endif /* WAIFU_ASSET_EXTERNAL_CARD_IMAGES */\n')
    f.write('static const char *waifu_card_names[WAIFU_CARD_COUNT] = {\n')
    for _,name,_,_,_,_,_ in CARD_META:
        f.write('    "'+name.replace('"','\\"')+'",\n')
    f.write('};\n')
    f.write('static const char *waifu_card_attr[WAIFU_CARD_COUNT] = {\n')
    for _,_,_,attr,_,_,_ in CARD_META:
        f.write('    "'+attr+'",\n')
    f.write('};\n')
    f.write('static const char *waifu_card_tribe[WAIFU_CARD_COUNT] = {\n')
    for _,_,tribe,_,_,_,_ in CARD_META:
        f.write('    "'+tribe+'",\n')
    f.write('};\n')
    f.write('static const char *waifu_card_desc[WAIFU_CARD_COUNT] = {\n')
    for *_,desc in CARD_META:
        f.write('    "'+c_string(desc)+'",\n')
    f.write('};\n')
    f.write('static const uint16_t waifu_card_atk[WAIFU_CARD_COUNT] = {')
    f.write(','.join(str(card[4]) for card in CARD_META)); f.write('};\n')
    f.write('static const uint16_t waifu_card_def[WAIFU_CARD_COUNT] = {')
    f.write(','.join(str(card[5]) for card in CARD_META)); f.write('};\n')
    f.write('#endif\n')

CARD_IDS_OUT.parent.mkdir(parents=True, exist_ok=True)
with CARD_IDS_OUT.open('w', newline='\n') as f:
    f.write('/* Generated by tools/gen_assets.py from assets/source/cards/card_data.txt. */\n')
    f.write('#ifndef WAIFU_FM_CARD_IDS_H\n#define WAIFU_FM_CARD_IDS_H\n\n')
    for i, card in enumerate(CARD_META):
        f.write(f'#define {card_macro(card[0])} {i}\n')
    f.write('\n#endif /* WAIFU_FM_CARD_IDS_H */\n')

def emit_pool_header_array(f, pool_name):
    symbol = 'waifu_' + pool_name + '_pool'
    entries = CARD_POOLS[pool_name]
    f.write(f'static const int {symbol}[] = {{\n')
    for i, asset_id in enumerate(entries):
        sep = ',' if i + 1 < len(entries) else ''
        f.write(f'    {card_macro(asset_id)}{sep}\n')
    f.write('};\n')
    f.write(f'#define {symbol.upper()}_COUNT ((int)(sizeof({symbol}) / sizeof({symbol}[0])))\n\n')

DECK_POOLS_OUT.parent.mkdir(parents=True, exist_ok=True)
with DECK_POOLS_OUT.open('w', newline='\n') as f:
    f.write('/* Generated by tools/gen_assets.py from assets/source/cards/card_data.txt. */\n')
    f.write('#ifndef WAIFU_FM_DECK_POOLS_H\n#define WAIFU_FM_DECK_POOLS_H\n\n')
    f.write('#include "card_ids.h"\n\n')
    for pool_name in [
        'random_strong', 'random_mid', 'random_weak',
        'story_starter_strong', 'story_starter_weak', 'story_reward_strong',
        'opponent_dream', 'opponent_plaza', 'opponent_adept', 'opponent_reaver',
        'opponent_burning', 'opponent_void', 'opponent_sphinx', 'opponent_demon',
    ]:
        if pool_name not in CARD_POOLS:
            raise SystemExit(f'missing pool {pool_name} in {CARD_DATA}')
        emit_pool_header_array(f, pool_name)
    f.write('static const int *waifu_opponent_story_pools[] = {\n')
    for pool_name in [
        'opponent_dream', 'opponent_plaza', 'opponent_adept', 'opponent_reaver',
        'opponent_burning', 'opponent_void', 'opponent_sphinx', 'opponent_demon',
    ]:
        f.write(f'    waifu_{pool_name}_pool,\n')
    f.write('};\n')
    f.write('static const int waifu_opponent_story_pool_counts[] = {\n')
    for pool_name in [
        'opponent_dream', 'opponent_plaza', 'opponent_adept', 'opponent_reaver',
        'opponent_burning', 'opponent_void', 'opponent_sphinx', 'opponent_demon',
    ]:
        f.write(f'    WAIFU_{pool_name.upper()}_POOL_COUNT,\n')
    f.write('};\n\n')
    f.write('#endif /* WAIFU_FM_DECK_POOLS_H */\n')

def pad_records(records, stride):
    out = bytearray()
    for rec in records:
        out.extend(rec)
        out.extend(b'\0' * (stride - len(rec)))
    return bytes(out)

BIN_OUT = ROOT/'assets/generated'
BIN_OUT.mkdir(parents=True, exist_ok=True)
(BIN_OUT/'story_portraits.bin').write_bytes(pad_records(q_story_portraits, PORTRAIT_CD_STRIDE))
(BIN_OUT/'story_portrait_mask.bin').write_bytes(pad_records(q_story_portrait_masks, PORTRAIT_CD_STRIDE))
(BIN_OUT/'card_faces.bin').write_bytes(b''.join(q_cards))
big_blob = b''.join(q_big_cards)
(BIN_OUT/'card_big_art.bin').write_bytes(big_blob)
# PC-FX CD path: each 112x112 art entry is padded to whole 2048-byte
# sectors so the CD reader can load one entry with a single sector read into
# a small staging slot. The first 112*112 bytes are the art; the tail is unused.
_sector = 2048
_big_one = BIG_W * BIG_H
_big_slot = ((_big_one + _sector - 1) // _sector) * _sector
padded = bytearray()
for art in q_big_cards:
    padded.extend(art)
    padded.extend(bytes(_big_slot - len(art)))
(BIN_OUT/'card_big_art_cd.bin').write_bytes(bytes(padded))
(BIN_OUT/'card_back.bin').write_bytes(q_back)
(BIN_OUT/'support_face.bin').write_bytes(q_support)
(BIN_OUT/'support_big_art.bin').write_bytes(q_support_big)
(BIN_OUT/'support_big_art_cd.bin').write_bytes(q_support_big + bytes(_big_slot - len(q_support_big)))
print('generated', OUT, 'cards', len(CARD_META), 'palette idx black', idx['BLACK'])
