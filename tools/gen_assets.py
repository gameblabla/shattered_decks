#!/usr/bin/env python3
from PIL import Image, ImageDraw, ImageOps, ImageFilter
from pathlib import Path
import math, os, re

ROOT = Path(__file__).resolve().parents[1]
CARD_DIR = ROOT/'assets/source/cards'
if not CARD_DIR.exists():
    # Development fallback used by this ChatGPT build environment. The generated
    # header is committed, so the distributed package does not require these PNGs.
    alt = Path('/mnt/data/card_game_unzip/cards')
    if alt.exists(): CARD_DIR = alt
OUT = ROOT/'src/generated/waifu_assets.h'
CARD_W, CARD_H = 38, 54
BIG_W, BIG_H = 112, 112
TILE = 32

CARD_META = [
  ('Abstract_beast','Nyxara, Abstract Chimera','Fiend','Dark',1900,1600),
  ('Bee_woman','Melora, Honeyblade Muse','Insect','Wind',1450,1200),
  ('Beetle','Carmina, Scarab Knight','Insect','Earth',1650,1900),
  ('Cabaret','Vivienne, Velvet Stage','Spellcaster','Dark',1600,1300),
  ('Carnivore_plant','Rosaria, Thorn Diva','Plant','Earth',1700,1500),
  ('Cobra','Serphea, Cobra Enchantress','Reptile','Dark',1800,1200),
  ('Cocinelle','Luminelle, Ladybird Page','Insect','Wind',1200,1800),
  ('Crow','Ravenna, Nightwing Idol','Winged Beast','Dark',1500,1250),
  ('Dragon','Dracona, Crimson Wyrm','Dragon','Fire',2400,2000),
  ('Electric','Voltara, Storm Siren','Thunder','Light',1900,1400),
  ('Falcon','Arienne, Falcon Herald','Winged Beast','Wind',1750,1400),
  ('Fox_Egypt','Nefra, Desert Fox Oracle','Beast','Light',1800,1650),
  ('Golem_Idol','Galatea, Golem Idol','Rock','Earth',1600,2300),
  ('Insect_Soldier','Vespara, Insect Lancer','Insect','Earth',1700,1600),
  ('Insect_queen','Aurelia, Hive Empress','Insect','Earth',2200,1900),
  ('Jester','Mirelle, Moonlit Jester','Fiend','Dark',1500,1700),
  ('JetFighter','Celeste, Jet Valkyrie','Machine','Wind',1850,1500),
  ('Mage_skeleton','Ossaria, Bone Mage','Spellcaster','Dark',1700,2100),
  ('Mermaid_machine','Marielle, Chrome Mermaid','Machine','Water',1800,1800),
  ('Parrot','Lorie, Parrot Minstrel','Winged Beast','Wind',1150,900),
  ('Penguin','Pina, Frost Penguin','Aqua','Water',950,1400),
  ('Pretty_snake','Saphira, Serpent Belle','Reptile','Water',1550,1350),
  ('Priestess','Elenia, Sun Priestess','Spellcaster','Light',1600,2000),
  ('Rat','Rattina, Alley Scout','Beast','Earth',900,700),
  ('Reptile','Vespera, Scale Duelist','Reptile','Earth',1450,1700),
  ('Scarab','Khepri, Jewel Scarab','Insect','Earth',1300,1600),
  ('Scorpion','Scorpia, Venom Dancer','Insect','Dark',1600,1500),
  ('Sea_Serpent','Thalassa, Sea Serpent','Sea Serpent','Water',2000,1700),
  ('Skull_Queen','Mortessa, Skull Queen','Zombie','Dark',2300,2100),
  ('Slime','Lumia, Slime Oracle','Aqua','Water',800,2000),
  ('Snake','Nagae, Coil Familiar','Reptile','Earth',1200,1000),
  ('Sphinx','Sakhmet, Sphinx Guardian','Beast','Light',2100,2500),
  ('Squid_tentacles','Calamaria, Tentacle Siren','Aqua','Water',1400,1800),
  ('Stone_Dragon','Petra, Stone Dragon','Dragon','Earth',2200,2600),
  ('Stone_Tablet_Woman','Menat, Tablet Keeper','Rock','Light',1100,2200),
  ('Turtle','Chelonia, Tide Shell','Aqua','Water',1000,2200),
  ('Tyranno','Tyranna, Raptor Queen','Dinosaur','Earth',2300,1800),
  ('Ultimate_Gold_Dragon','Aurumelia, Ultimate Gold Dragon','Dragon','Light',3000,2500),
  ('Warrior','Brienne, Blade Duelist','Warrior','Earth',1800,1600),
  ('Water_Element','Ondina, Water Element','Aqua','Water',1800,2200),
  ('White_dragon','Albathia, White Dragon','Dragon','Light',2600,2100),
  ('Witch','Morganna, Night Witch','Spellcaster','Dark',1900,1700),
  ('Yokai','Yuzuki, Yokai Shade','Fiend','Dark',2000,1800),
  ('insect_bomb','Bombella, Hive Grenadier','Insect','Fire',1000,1000),
]

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

def cover(img, size):
    return ImageOps.fit(img.convert('RGB'), size, method=Image.Resampling.BILINEAR, centering=(0.5,0.45))

def card_thumb_crop(img, size):
    # Hand/field thumbnails should emphasize the face/upper torso region.
    # Keep battle/full-size art unchanged; only the tiny card-face thumbnail
    # uses this top-middle crop before downscaling.
    img = img.convert('RGB')
    w, h = img.size
    crop_w = int(w * 0.62)
    crop_h = int(h * 0.62)
    left = max(0, (w - crop_w) // 2)
    top = max(0, int(h * 0.06))
    if top + crop_h > h:
        top = max(0, h - crop_h)
    crop = img.crop((left, top, left + crop_w, top + crop_h))
    return ImageOps.fit(crop, size, method=Image.Resampling.BILINEAR, centering=(0.5,0.40))

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

def draw_support_face():
    img = Image.new('RGB', (CARD_W,CARD_H), (9,65,31))
    d = ImageDraw.Draw(img)
    d.rectangle([1,0,CARD_W-2,CARD_H-1], fill=(24,151,75))
    d.rectangle([3,3,CARD_W-4,CARD_H-4], fill=(8,96,48))
    d.rectangle([5,7,CARD_W-6,35], fill=(30,20,50))
    for i in range(5):
        d.arc([7+i,7+i,CARD_W-8-i,35-i], 20+i*8, 310-i*5, fill=(202,188,255), width=1)
    d.rectangle([5,40,CARD_W-6,49], fill=(18,72,38))
    d.text((8,41), 'MAG', fill=(230,240,230))
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
    img=Image.new('RGB',(TILE,TILE),(190,111,18)); d=ImageDraw.Draw(img)
    for y in range(TILE):
        for x in range(TILE):
            v=(x*3+y*5+variant*19)%37
            base=(196+v,126+v//2,22)
            if ((x+y+variant*3)//7)%2==0: base=(220+v//2,160+v//2,42)
            img.putpixel((x,y), tuple(min(255,c) for c in base))
    d.line([0,0,31,0], fill=(250,219,75)); d.line([0,0,0,31], fill=(247,203,63))
    d.line([31,0,31,31], fill=(72,34,12)); d.line([0,31,31,31], fill=(72,34,12))
    d.line([4,0,31,25], fill=(151,74,16))
    d.line([0,27,26,0], fill=(245,197,50))
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

def tile_green_side():
    img=Image.new('RGB',(TILE,TILE),(15,76,40)); d=ImageDraw.Draw(img)
    for y in range(TILE):
        shade=20+y*2
        d.line([0,y,31,y], fill=(9, max(40,100-y*2), 35))
    d.rectangle([3,6,28,25], outline=(160,120,30))
    d.line([6,21,26,8], fill=(26,110,48))
    return img

def tile_dark(): return Image.new('RGB',(TILE,TILE),(0,0,0))

def tile_brown():
    img=Image.new('RGB',(TILE,TILE),(52,19,7)); d=ImageDraw.Draw(img)
    for y in range(0,TILE,4): d.line([0,y,31,y], fill=(25,8,3))
    for x in range(0,TILE,8): d.line([x,0,x,31], fill=(100,42,10))
    return img

# Make master palette image.
card_faces_rgb=[draw_card_face(*m) for m in CARD_META]
support_rgb=draw_support_face()
back_rgb=draw_card_back()
tex_rgb=[tile_dark(),tile_gold(0),tile_gold(1),tile_stone(),tile_green_side(),tile_brown(),back_rgb.resize((TILE,TILE), Image.Resampling.NEAREST)]
# master swatches heavily weighted so UI colors survive
swatches=[]
for c in BASE_COLORS.values():
    swatches += [c]*64
for im in card_faces_rgb + [support_rgb,back_rgb] + tex_rgb:
    small=im.resize((max(1,im.width//2), max(1,im.height//2)), Image.Resampling.BILINEAR)
    swatches.extend(list(small.getdata()))
master=Image.new('RGB',(256, max(1, math.ceil(len(swatches)/256))))
master.putdata(swatches + [(0,0,0)]*(master.width*master.height-len(swatches)))
pal_img=master.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
palette=pal_img.getpalette()[:768]
# force black-ish index discovery via nearest, not necessarily index 0.
def nearest_idx(rgb):
    best=0; bd=10**9
    for i in range(256):
        pr,pg,pb=palette[i*3:i*3+3]
        d=(pr-rgb[0])**2+(pg-rgb[1])**2+(pb-rgb[2])**2
        if d<bd:
            best=i; bd=d
    return best

def qbytes(im):
    return bytes(im.convert('RGB').quantize(palette=pal_img, dither=Image.Dither.NONE).tobytes())

q_cards=[qbytes(im) for im in card_faces_rgb]
# 112x112 battle-window images from the original source art. These are used only
# by the black battle cut-in so the duel cards do not look like tiny field sprites.
big_art_rgb=[]
for card_id, *_ in CARD_META:
    art = cover(Image.open(find_image(card_id)), (BIG_W, BIG_H))
    art = art.filter(ImageFilter.SHARPEN)
    big_art_rgb.append(art)
q_big_art=[qbytes(im) for im in big_art_rgb]
q_support=qbytes(support_rgb)
q_back=qbytes(back_rgb)
q_tex=[qbytes(im) for im in tex_rgb]

# Constants for named palette slots.
idx = {name:nearest_idx(rgb) for name,rgb in BASE_COLORS.items()}

# write header
OUT.parent.mkdir(parents=True, exist_ok=True)
with open(OUT,'w') as f:
    f.write('/* Generated by tools/gen_assets.py. 256x240 indexed assets. */\n')
    f.write('#ifndef WAIFU_ASSETS_H\n#define WAIFU_ASSETS_H\n#include <stdint.h>\n')
    f.write(f'#define WAIFU_CARD_COUNT {len(CARD_META)}\n#define WAIFU_CARD_W {CARD_W}\n#define WAIFU_CARD_H {CARD_H}\n#define WAIFU_BIG_W {BIG_W}\n#define WAIFU_BIG_H {BIG_H}\n#define WAIFU_TEX_TILE_SIZE {TILE}\n#define WAIFU_TEX_TILE_COUNT 7\n')
    for name,val in idx.items():
        f.write(f'#define IDX_{name} {val}\n')
    def array(name, data, width=16):
        f.write(f'static const uint8_t {name}[] = {{\n')
        for i in range(0,len(data),width):
            f.write('    '+','.join(str(b) for b in data[i:i+width])+',\n')
        f.write('};\n')
    array('waifu_palette_rgb', bytes(palette), 18)
    # texture atlas concatenated
    array('waifu_texture_atlas', b''.join(q_tex), 16)
    # cards as flat array
    array('waifu_card_faces', b''.join(q_cards), 16)
    array('waifu_big_art', b''.join(q_big_art), 16)
    array('waifu_card_back', q_back, 16)
    array('waifu_support_face', q_support, 16)
    f.write('static const char *waifu_card_names[WAIFU_CARD_COUNT] = {\n')
    for _,name,_,_,_,_ in CARD_META:
        f.write('    "'+name.replace('"','\\"')+'",\n')
    f.write('};\n')
    f.write('static const char *waifu_card_attr[WAIFU_CARD_COUNT] = {\n')
    for _,_,_,attr,_,_ in CARD_META:
        f.write('    "'+attr+'",\n')
    f.write('};\n')
    f.write('static const char *waifu_card_tribe[WAIFU_CARD_COUNT] = {\n')
    for _,_,tribe,_,_,_ in CARD_META:
        f.write('    "'+tribe+'",\n')
    f.write('};\n')
    f.write('static const uint16_t waifu_card_atk[WAIFU_CARD_COUNT] = {')
    f.write(','.join(str(atk) for *_,atk,deff in CARD_META)); f.write('};\n')
    f.write('static const uint16_t waifu_card_def[WAIFU_CARD_COUNT] = {')
    f.write(','.join(str(deff) for *_,atk,deff in CARD_META)); f.write('};\n')
    f.write('#endif\n')
print('generated', OUT, 'cards', len(CARD_META), 'palette idx black', idx['BLACK'])
