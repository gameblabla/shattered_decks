#ifndef WAIFU_SDL3_TEXT_H
#define WAIFU_SDL3_TEXT_H

/* FreeType text for the SDL3/PC frontend: a monospace TTF is rasterized once
   into a persistent glyph atlas so the game's fixed 8px character cells render
   crisp, anti-aliased glyphs (linear + mipmap) instead of the upscaled 8x8
   bitmap font. Console targets (PC-FX/CD32X/headless) keep the bitmap font.

   The glyphs are drawn monospaced-in-cell: each glyph keeps its natural shape,
   bearing and size but the caller advances by the same fixed cell width the
   bitmap font used, so every existing text layout (wrapping, centering,
   right-align, the typewriter reveal counts) stays valid. */

#include <stdint.h>

typedef struct WaifuGlyphInfo {
    float u0, v0, u1, v1;   /* atlas UV rect, normalized */
    float dx, dy, dw, dh;   /* draw rect relative to the cell origin, in game px */
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

/* The CPU glyph atlas (RGBA8, white with coverage in alpha) for the video
   layer to upload once. Returns NULL until the font is ready. */
const uint32_t *waifu_sdl3_glyph_atlas(int *w, int *h);

#endif /* WAIFU_SDL3_TEXT_H */
