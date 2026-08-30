#!/usr/bin/env python3
"""Generate a C header listing the high-resolution SOURCE image path for every
card, plus the widescreen title/ending art, for the SDL3 GPU frontend.

The SDL3 target can load the original full-resolution artwork at runtime instead
of the tiny quantized 38x54 / 112x112 blobs baked into waifu_assets.h.  This tool
resolves each card's source image (by asset_id, trying several extensions) and
emits repo-root-relative paths in card_id order, so the C frontend can index the
array by card id.  The header is committed following the repo's
checked-in-generated-file convention.

Usage: python3 tools/gen_sdl3_hires_paths.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CARD_DIR = ROOT / 'assets/source/cards'
CARD_DATA = CARD_DIR / 'card_data.txt'
PORTRAIT_DIR = ROOT / 'assets/source/story_portraits'
# Story portraits, in waifu_assets.h portrait-id order (see gen_assets.py's
# STORY_PORTRAITS). The PC build prefers a high-resolution variant of the same
# artwork; the suffixes are inconsistent in the source tree, so try them all and
# fall back to the small file the console assets were built from.
PORTRAIT_BASES = ['serena', 'opponent_0', 'opponent_1', 'opponent_2',
                  'opponent_3', 'opponent_4']
PORTRAIT_SUFFIXES = ['_hires', '_highres', '']
# The PC card back: a single full-resolution texture that replaces the 38x54
# 8bpp back everywhere it is drawn (board, hand, deck stacks).
CARD_BACK_SRC = ROOT / 'assets/source/textures/card_texture.png'
# The PC card FRONT frames, one per card class, in WAIFU_CARD_FRAME_* order
# (monster, spell, trap).  Like the back, each is a full-resolution texture that
# covers the whole 38x54 card rect; the art window inside it is empty and the
# frontend fills it with the card's own art.
CARD_FRAME_SRC = [ROOT / 'assets/source/textures/monster_card_front_template.png',
                  ROOT / 'assets/source/textures/spell_card_front_template.png',
                  ROOT / 'assets/source/textures/trap_card_front_template.png']
# Where the art goes inside those frames, as pixel coordinates in the template's
# own resolution.  The artist's reference file gives two corner lines; it is
# parsed (rather than transcribed) so re-exporting the templates at another size
# only means updating the .txt beside them.
CARD_FRAME_ART_REF = ROOT / 'assets/source/textures/card_front_template_pixel_monster_original_res.txt'
# The level "star" ankh, stamped into the frame's top band once per level.
ANKH_SRC = ROOT / 'assets/source/textures/ankh.png'
# The two 3D board checker squares.  The console targets bake these down to the
# 32x32 atlas cells (tools/gen_assets.py board_tile()); the PC frontend loads the
# same files here at their original resolution and maps one per board cell.
BOARD_TILE_SRC = [ROOT / 'assets/source/textures/sandstone_1.png',
                  ROOT / 'assets/source/textures/sandstone_2.png']
TITLE_SRC = ROOT / 'assets/source/title/title_16by9.png'
ENDING_SRC = ROOT / 'assets/source/ending/ending_16by9.png'
OUT = ROOT / 'src/generated/sdl3_card_paths.h'

CARD_EXTS = ['.png', '.webp', '.jpg', '.jpeg']


def parse_card_data(path: Path):
    """Parse card_data.txt exactly like tools/gen_assets.py: each non-empty,
    non-'#' line is split on '|'; 'card' rows carry 8 pipe-fields.  The card_id
    is the 0-based index of 'card' rows in file order; return their asset_ids."""
    if not path.exists():
        raise FileNotFoundError(path)
    asset_ids = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split('|')]
        if parts[0] == 'card':
            if len(parts) != 8:
                raise SystemExit(f'{path}:{lineno}: card rows need 8 fields')
            asset_ids.append(parts[1])
    return asset_ids


def resolve_card_src(asset_id: str):
    """First existing source image for asset_id, as a repo-root-relative
    forward-slash path, or '' if none exists."""
    for ext in CARD_EXTS:
        p = CARD_DIR / (asset_id + ext)
        if p.exists():
            return p.relative_to(ROOT).as_posix()
    return ''


def resolve_portrait_src(base: str):
    """Highest-resolution source for one story portrait. '.png.png' is a real
    filename in the tree, so every suffix is tried with both endings."""
    for suffix in PORTRAIT_SUFFIXES:
        for ext in ('.png', '.png.png', '.webp', '.jpg'):
            p = PORTRAIT_DIR / (base + suffix + ext)
            if p.exists():
                return p.relative_to(ROOT).as_posix()
    return ''


def png_size(path: Path):
    """(width, height) from a PNG's IHDR -- avoids a Pillow dependency here."""
    data = path.read_bytes()[:33]
    if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise SystemExit(f'{path}: not a PNG')
    return (int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big'))


def parse_windows(ref: Path, template: Path):
    """The three normalized (u0, v0, u1, v1) windows the artist's reference file
    marks out on the card front template, in file order: the ART window, the
    bottom STAT band (ATK/DEF, or the EQUIP/SUPPORT/TRAP label), and the top
    STAR band (the level ankhs).  Every integer in the file is read in order and
    grouped into corner pairs, so a corner may sit on its own line or share one
    ("137, 1116 to 984,1295") -- the file mixes both."""
    nums = [int(n) for n in re.findall(r'-?\d+', ref.read_text())]
    if len(nums) < 12:
        raise SystemExit(f'{ref}: expected 3 corner-pair windows (12 numbers)')
    tw, th = png_size(template)
    wins = []
    for i in range(3):
        x0, y0, x1, y1 = nums[i * 4:i * 4 + 4]
        wins.append((x0 / tw, y0 / th, x1 / tw, y1 / th))
    return wins


def resolve_fullscreen_src(path: Path):
    return path.relative_to(ROOT).as_posix() if path.exists() else ''


def c_string(s: str):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def main():
    asset_ids = parse_card_data(CARD_DATA)
    card_src = [resolve_card_src(a) for a in asset_ids]
    portrait_src = [resolve_portrait_src(b) for b in PORTRAIT_BASES]
    card_back_src = resolve_fullscreen_src(CARD_BACK_SRC)
    card_frame_src = [resolve_fullscreen_src(p) for p in CARD_FRAME_SRC]
    art_win, stat_win, star_win = parse_windows(CARD_FRAME_ART_REF, CARD_FRAME_SRC[0])
    ankh_src = resolve_fullscreen_src(ANKH_SRC)
    ankh_w, ankh_h = png_size(ANKH_SRC) if ANKH_SRC.exists() else (1, 1)
    tmpl_w, tmpl_h = png_size(CARD_FRAME_SRC[0])
    board_tile_src = [resolve_fullscreen_src(p) for p in BOARD_TILE_SRC]
    title_src = resolve_fullscreen_src(TITLE_SRC)
    ending_src = resolve_fullscreen_src(ENDING_SRC)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('w', newline='\n') as f:
        f.write('/* Auto-generated by tools/gen_sdl3_hires_paths.py — do not edit. */\n')
        f.write('#ifndef WAIFU_SDL3_CARD_PATHS_H\n')
        f.write('#define WAIFU_SDL3_CARD_PATHS_H\n\n')
        f.write(f'#define WAIFU_SDL3_CARD_SRC_COUNT {len(card_src)}\n\n')
        f.write('static const char *const waifu_sdl3_card_src[WAIFU_SDL3_CARD_SRC_COUNT] = {\n')
        for src in card_src:
            f.write(f'    "{c_string(src)}",\n')
        f.write('};\n\n')
        f.write(f'#define WAIFU_SDL3_PORTRAIT_SRC_COUNT {len(portrait_src)}\n\n')
        f.write('static const char *const waifu_sdl3_portrait_src'
                '[WAIFU_SDL3_PORTRAIT_SRC_COUNT] = {\n')
        for src in portrait_src:
            f.write(f'    "{c_string(src)}",\n')
        f.write('};\n\n')
        f.write(f'static const char *const waifu_sdl3_card_back_src = "{c_string(card_back_src)}";\n')
        f.write(f'#define WAIFU_SDL3_CARD_FRAME_SRC_COUNT {len(card_frame_src)}\n')
        f.write('static const char *const waifu_sdl3_card_frame_src'
                '[WAIFU_SDL3_CARD_FRAME_SRC_COUNT] = {\n')
        for src in card_frame_src:
            f.write(f'    "{c_string(src)}",\n')
        f.write('};\n')
        f.write('/* The three windows inside a card front frame, as fractions of the card\n'
                '   rect (from %s): the art window, the bottom\n'
                '   stat band and the top level-ankh band. */\n' % CARD_FRAME_ART_REF.name)
        for prefix, win in (('ART', art_win), ('STAT', stat_win), ('STAR', star_win)):
            for name, val in zip(('U0', 'V0', 'U1', 'V1'), win):
                f.write(f'#define WAIFU_SDL3_CARD_{prefix}_{name} {val:.6f}f\n')
        f.write('/* The template pixel size, so a band pixel aspect (and the strip\n'
                '   textures generated for it) can be recovered from the fractions above. */\n')
        f.write(f'#define WAIFU_SDL3_CARD_TEMPLATE_W {tmpl_w}\n')
        f.write(f'#define WAIFU_SDL3_CARD_TEMPLATE_H {tmpl_h}\n')
        f.write(f'static const char *const waifu_sdl3_ankh_src = "{c_string(ankh_src)}";\n')
        f.write(f'#define WAIFU_SDL3_ANKH_ASPECT {ankh_w / ankh_h:.6f}f\n')
        f.write(f'#define WAIFU_SDL3_BOARD_TILE_SRC_COUNT {len(board_tile_src)}\n')
        f.write('static const char *const waifu_sdl3_board_tile_src'
                '[WAIFU_SDL3_BOARD_TILE_SRC_COUNT] = {\n')
        for src in board_tile_src:
            f.write(f'    "{c_string(src)}",\n')
        f.write('};\n')
        f.write(f'static const char *const waifu_sdl3_title_src = "{c_string(title_src)}";\n')
        f.write(f'static const char *const waifu_sdl3_ending_src = "{c_string(ending_src)}";\n\n')
        f.write('#endif /* WAIFU_SDL3_CARD_PATHS_H */\n')

    resolved = sum(1 for s in card_src if s)
    print(f'wrote {OUT}: {len(card_src)} cards, {resolved} resolved, '
          f'{len(card_src) - resolved} empty')


if __name__ == '__main__':
    main()
