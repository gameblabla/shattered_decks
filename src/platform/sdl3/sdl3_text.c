/* FreeType glyph atlas for the SDL3/PC frontend. See sdl3_text.h.

   A monospace TTF is rasterized once at a high pixel size into a persistent
   RGBA atlas (white, coverage in alpha). Per glyph we record a draw rect in
   game-pixel units, scaled so the font's ascender maps to CELL_ASCENDER inside
   the game's 8 px cell, and the glyph's advance so the caller can centre it.

   Two things make this read like a game font rather than a terminal:
   the face is BOLD (the 8x8 bitmap font it replaces is effectively bold, and
   thin strokes disappear against card art), and glyphs are stretched
   horizontally until their advance reaches ADVANCE_FILL of the cell. A text
   face's natural advance is only ~0.6 em against a near-square cell, so at
   natural width every character was followed by a ragged gap. */

#include "sdl3_text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <string.h>

#define GLYPH_FIRST 32
#define GLYPH_LAST  126
#define GLYPH_COUNT (GLYPH_LAST - GLYPH_FIRST + 1)

#define ATLAS_W 512
#define ATLAS_H 512
#define RASTER_PX 48          /* rasterization size; downscaled crisp at draw */
#define CELL_ASCENDER 7.4f    /* game px the font ascender maps to (8px cell) */
#define CELL_W 8.0f           /* the cell these metrics are baked for */
#define ADVANCE_FILL 0.90f    /* fraction of the cell the advance is stretched to */
#define ATLAS_PAD 2

static int g_state = 0;       /* 0 = untried, 1 = ready, -1 = failed */
static uint32_t g_atlas[ATLAS_W * ATLAS_H];
static WaifuGlyphInfo g_glyphs[GLYPH_COUNT];

/* Bold first: it is the shipped face. The regular weights are only fallbacks
   for a tree without the bundled fonts. */
static const char *const g_font_paths[] = {
    "assets/fonts/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "assets/fonts/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    0
};

static int build_atlas(void)
{
    FT_Library lib = 0;
    FT_Face face = 0;
    int i;
    float scale_y, scale_x, ascender_px, advance_px;
    int pen_x = ATLAS_PAD, pen_y = ATLAS_PAD, row_h = 0;

    if (FT_Init_FreeType(&lib) != 0) return 0;
    for (i = 0; g_font_paths[i]; ++i) {
        if (FT_New_Face(lib, g_font_paths[i], 0, &face) == 0) break;
        face = 0;
    }
    if (!face) { FT_Done_FreeType(lib); return 0; }
    if (FT_Set_Pixel_Sizes(face, 0, RASTER_PX) != 0) {
        FT_Done_Face(face); FT_Done_FreeType(lib); return 0;
    }

    ascender_px = (float)(face->size->metrics.ascender >> 6);
    if (ascender_px < 1.0f) ascender_px = (float)RASTER_PX * 0.75f;
    scale_y = CELL_ASCENDER / ascender_px;

    /* Horizontal scale is set by the ADVANCE, not the ascender: stretch until
       one character step fills ADVANCE_FILL of the cell. On a monospace face
       every advance is the same, so measuring 'M' measures them all. */
    advance_px = (float)(face->size->metrics.max_advance >> 6);
    if (FT_Load_Char(face, 'M', FT_LOAD_DEFAULT) == 0 && face->glyph->advance.x > 0)
        advance_px = (float)(face->glyph->advance.x >> 6);
    if (advance_px < 1.0f) advance_px = (float)RASTER_PX * 0.6f;
    scale_x = (CELL_W * ADVANCE_FILL) / advance_px;

    memset(g_atlas, 0, sizeof(g_atlas));
    memset(g_glyphs, 0, sizeof(g_glyphs));

    for (i = 0; i < GLYPH_COUNT; ++i) {
        unsigned int ch = (unsigned int)(GLYPH_FIRST + i);
        FT_GlyphSlot g;
        int bw, bh, bl, bt, r, c;
        WaifuGlyphInfo *gi = &g_glyphs[i];
        if (FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) continue;
        g = face->glyph;
        bw = (int)g->bitmap.width;
        bh = (int)g->bitmap.rows;
        bl = g->bitmap_left;
        bt = g->bitmap_top;
        if (bw <= 0 || bh <= 0) {
            gi->drawable = 0;   /* space and the like: advance only */
            gi->adv = (float)(g->advance.x >> 6) * scale_x;
            continue;
        }
        if (pen_x + bw + ATLAS_PAD > ATLAS_W) {
            pen_x = ATLAS_PAD;
            pen_y += row_h + ATLAS_PAD;
            row_h = 0;
        }
        if (pen_y + bh + ATLAS_PAD > ATLAS_H) break;   /* atlas full: stop */
        for (r = 0; r < bh; ++r) {
            const unsigned char *src = g->bitmap.buffer + (size_t)r * g->bitmap.pitch;
            uint32_t *dst = &g_atlas[(size_t)(pen_y + r) * ATLAS_W + pen_x];
            for (c = 0; c < bw; ++c)
                dst[c] = 0x00FFFFFFu | ((uint32_t)src[c] << 24);   /* white, alpha=coverage */
        }
        gi->u0 = (float)pen_x / (float)ATLAS_W;
        gi->v0 = (float)pen_y / (float)ATLAS_H;
        gi->u1 = (float)(pen_x + bw) / (float)ATLAS_W;
        gi->v1 = (float)(pen_y + bh) / (float)ATLAS_H;
        gi->dx = (float)bl * scale_x;
        gi->dy = CELL_ASCENDER - (float)bt * scale_y;
        gi->dw = (float)bw * scale_x;
        gi->dh = (float)bh * scale_y;
        gi->adv = (float)(g->advance.x >> 6) * scale_x;
        gi->drawable = 1;
        pen_x += bw + ATLAS_PAD;
        if (bh > row_h) row_h = bh;
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return 1;
}

int waifu_sdl3_text_ready(void)
{
    if (g_state == 0)
        g_state = build_atlas() ? 1 : -1;
    return g_state == 1;
}

int waifu_sdl3_glyph_info(unsigned char ch, WaifuGlyphInfo *out)
{
    const WaifuGlyphInfo *gi;
    if (g_state != 1) return 0;
    if (ch < GLYPH_FIRST || ch > GLYPH_LAST) return 0;
    gi = &g_glyphs[ch - GLYPH_FIRST];
    if (!gi->drawable) return 0;
    if (out) *out = *gi;
    return 1;
}

float waifu_sdl3_glyph_advance(void)
{
    if (g_state != 1) return 0.0f;
    return CELL_W * ADVANCE_FILL;
}

const uint32_t *waifu_sdl3_glyph_atlas(int *w, int *h)
{
    if (g_state != 1) return 0;
    if (w) *w = ATLAS_W;
    if (h) *h = ATLAS_H;
    return g_atlas;
}
