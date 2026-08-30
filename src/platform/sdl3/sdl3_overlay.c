/* Immediate overlay geometry — see sdl3_overlay.h. */

#include "sdl3_overlay.h"
#include "sdl3_text.h"

#include <string.h>

/* Advance per character as a fraction of the text size. The glyph atlas bakes
   its own monospace advance (stretched to nearly fill the game's cell), so the
   overlay steps by that same figure — guessing a different ratio would either
   overlap the menu's text or space it out. The constant is only the fallback
   for a build with no font. */
#define OVERLAY_ADVANCE_FALLBACK 0.62f

static float overlay_advance(void)
{
    float a = waifu_sdl3_text_ready() ? waifu_sdl3_glyph_advance() : 0.0f;
    return a > 0.0f ? a / 8.0f : OVERLAY_ADVANCE_FALLBACK;
}

static Sdl3Overlay g_ov;

Sdl3Overlay *waifu_sdl3_overlay(void) { return &g_ov; }

void waifu_sdl3_overlay_begin(int px_w, int px_h)
{
    float aspect;
    if (px_h < 1) px_h = 1;
    if (px_w < 1) px_w = 1;
    aspect = (float)px_w / (float)px_h;
    g_ov.active = 1;
    g_ov.screen_h = WAIFU_OVERLAY_H;
    g_ov.screen_w = WAIFU_OVERLAY_H * aspect;
    g_ov.vert_count = 0;
    g_ov.glyph_count = 0;
}

void waifu_sdl3_overlay_clear(void)
{
    g_ov.active = 0;
    g_ov.vert_count = 0;
    g_ov.glyph_count = 0;
}

float waifu_sdl3_overlay_width(void) { return g_ov.screen_w; }

static void push_quad(Sdl3UiVertex *dst, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1,
                      const float c0[4], const float c1[4])
{
    static const int cx[6] = { 0, 1, 1, 0, 1, 0 };
    static const int cy[6] = { 0, 0, 1, 0, 1, 1 };
    int i;
    for (i = 0; i < 6; ++i) {
        const float *c = cy[i] ? c1 : c0;
        dst[i].x = cx[i] ? x1 : x0;
        dst[i].y = cy[i] ? y1 : y0;
        dst[i].u = cx[i] ? u1 : u0;
        dst[i].v = cy[i] ? v1 : v0;
        dst[i].r = c[0]; dst[i].g = c[1]; dst[i].b = c[2]; dst[i].a = c[3];
    }
}

void waifu_sdl3_overlay_vgrad(float x, float y, float w, float h,
                              const float top_rgba[4], const float bot_rgba[4])
{
    if (!g_ov.active || w <= 0.0f || h <= 0.0f) return;
    if (g_ov.vert_count + 6 > SDL3_OVERLAY_MAX_VERTS) return;
    push_quad(&g_ov.verts[g_ov.vert_count], x, y, x + w, y + h,
              -1.0f, 0.0f, -1.0f, 0.0f, top_rgba, bot_rgba);
    g_ov.vert_count += 6;
}

void waifu_sdl3_overlay_rect(float x, float y, float w, float h,
                             float r, float g, float b, float a)
{
    float c[4];
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    waifu_sdl3_overlay_vgrad(x, y, w, h, c, c);
}

void waifu_sdl3_overlay_frame(float x, float y, float w, float h, float t,
                              float r, float g, float b, float a)
{
    waifu_sdl3_overlay_rect(x, y, w, t, r, g, b, a);
    waifu_sdl3_overlay_rect(x, y + h - t, w, t, r, g, b, a);
    waifu_sdl3_overlay_rect(x, y + t, t, h - 2.0f * t, r, g, b, a);
    waifu_sdl3_overlay_rect(x + w - t, y + t, t, h - 2.0f * t, r, g, b, a);
}

float waifu_sdl3_overlay_text_width(float size, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    return (float)n * size * overlay_advance();
}

float waifu_sdl3_overlay_text(float x, float y, float size, const char *text,
                              float r, float g, float b, float a)
{
    const float k = size / 8.0f;     /* glyph rects are in 8px-cell units */
    const float adv = size * overlay_advance();
    float pen = x;
    const unsigned char *p;
    float col[4];

    if (!text) return 0.0f;
    if (!g_ov.active || !waifu_sdl3_text_ready())
        return waifu_sdl3_overlay_text_width(size, text);

    col[0] = r; col[1] = g; col[2] = b; col[3] = a;
    for (p = (const unsigned char *)text; *p; ++p, pen += adv) {
        WaifuGlyphInfo gi;
        if (!waifu_sdl3_glyph_info(*p, &gi)) continue;
        if (g_ov.glyph_count + 6 > SDL3_OVERLAY_MAX_GLYPHS) break;
        push_quad(&g_ov.glyphs[g_ov.glyph_count],
                  pen + gi.dx * k, y + gi.dy * k,
                  pen + (gi.dx + gi.dw) * k, y + (gi.dy + gi.dh) * k,
                  gi.u0, gi.v0, gi.u1, gi.v1, col, col);
        g_ov.glyph_count += 6;
    }
    return waifu_sdl3_overlay_text_width(size, text);
}
