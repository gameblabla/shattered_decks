#ifndef WAIFU_SDL3_TEXT_H
#define WAIFU_SDL3_TEXT_H

/* FreeType text for the SDL3/PC frontend: a monospace TTF is rasterized once
   into a persistent glyph atlas so the game's fixed 8px character cells render
   crisp, anti-aliased glyphs (linear + mipmap) instead of the upscaled 8x8
   bitmap font. Console targets (PC-FX/CD32X/headless) keep the bitmap font.

   The glyphs are drawn monospaced-in-cell: the caller advances by the same
   fixed cell width the bitmap font used, so every existing text layout
   (wrapping, centering, right-align, the typewriter reveal counts) stays valid.
   Inside its cell each glyph is horizontally expanded until its advance nearly
   fills the cell and then centred on it. A text face's natural advance is only
   about two thirds of the game's near-square cell, so drawing at natural width
   left a ragged gap after every character — the "spaced out" look. Expanding
   and centring gives the even rhythm the 8x8 bitmap font had, with real
   letterforms. */

#include <stdint.h>

typedef struct WaifuGlyphInfo {
    float u0, v0, u1, v1;   /* atlas UV rect, normalized */
    float dx, dy, dw, dh;   /* draw rect relative to the glyph's own origin, game px */
    float adv;              /* the glyph's advance width in game px */
    int drawable;           /* 0 for blank glyphs (space) */
} WaifuGlyphInfo;

/* Lazily loads the monospace TTF and builds the ASCII glyph atlas. Returns 1
   when the glyph system is usable, 0 if the font could not be loaded (the
   caller then falls back to the bitmap font). Safe to call every frame. */
int waifu_sdl3_text_ready(void);

/* Fill `out` with the atlas placement + cell-relative draw rect for an ASCII
   character. Returns 1 if the glyph is drawable (not a blank/space or unknown
   code point), 0 otherwise. */
int waifu_sdl3_glyph_info(unsigned char ch, WaifuGlyphInfo *out);

/* The baked monospace advance, in game px for the 8 px cell (0 when the font
   is not ready). Callers that lay text out themselves — the frontend overlay —
   step by this instead of guessing the face's natural ratio. */
float waifu_sdl3_glyph_advance(void);

/* The CPU glyph atlas (RGBA8, white with coverage in alpha) for the video
   layer to upload once. Returns NULL until the font is ready. */
const uint32_t *waifu_sdl3_glyph_atlas(int *w, int *h);

/* ---- CPU string rasterizer -------------------------------------------------
   The card front frames carry their ATK/DEF numbers and their EQUIP/SUPPORT/
   TRAP label baked into a small per-card strip texture (sdl3_hires.c), because
   the same strip has to land on a 2D hand card AND on a perspective board card
   -- the GPU glyph pipeline only draws screen-space quads. These two helpers
   rasterize the same TTF glyph atlas straight into an RGBA8 buffer.

   `cell_px` is the destination height of the game's 8 px text cell, so a string
   drawn at cell_px = 40 has capitals 6.6/8 * 40 = 33 px tall. (x, y) is the
   top-left of that cell box. Pixels are alpha-blended over what is there. */
float waifu_sdl3_text_measure(const char *s, float cell_px);
void waifu_sdl3_text_draw_rgba(uint32_t *dst, int dst_w, int dst_h,
                               float x, float y, float cell_px, const char *s,
                               uint8_t r, uint8_t g, uint8_t b);
/* The same string with a `px`-wide outline in (or, og, ob) laid down first --
   what keeps a light label readable over the frame's dark marbling. */
void waifu_sdl3_text_draw_rgba_outlined(uint32_t *dst, int dst_w, int dst_h,
                                        float x, float y, float cell_px, const char *s,
                                        uint8_t r, uint8_t g, uint8_t b,
                                        uint8_t orr, uint8_t og, uint8_t ob, float px);

#endif /* WAIFU_SDL3_TEXT_H */
