/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_draw.c — direct planar drawing.
 *
 *  EVERYTHING HERE IS ROW-POINTER BASED, AND THAT IS THE WHOLE PERFORMANCE
 *  STORY OF THIS FILE.  The first version addressed the screen per 16-pixel
 *  group: `Atarist_BackBuffer() + y * 160` inside the innermost helper.  On a
 *  68000 that is a cross-module call plus __mulsi3 -- there is no 32x32
 *  multiply in the instruction set -- for every sixteen pixels, and it cost the
 *  duel HUD twenty-four vblanks a frame, twenty-four times the entire 3D board.
 *  So a row pointer is computed once per shape and advanced by ATARIST_SCREEN_
 *  WORDS per scanline, and no routine below multiplies inside a loop.
 *
 *  The second rule is that interior groups are STORED, not read-modify-written.
 *  A solid fill only has to preserve pixels in its two edge groups; the middle
 *  is four `move.w` per sixteen pixels with nothing read back.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stddef.h>

#include "atarist_draw.h"
#include "atarist_video.h"

#define WAIFU_FONT_MENUDATA_DEFINE 1
#include "font_menudata.h"

/* Words per scanline: four interleaved plane words per 16 pixels. */
#define SCREEN_WORDS  (ATARIST_SCREEN_STRIDE / 2)
#define SCREEN_GROUPS (ATARIST_SCREEN_W / 16)

/* Plane-word masks for a solid colour: plane p is all-ones where bit p of the
 * colour is set. */
static void colour_words(uint8_t colour, uint16_t *w)
{
    w[0] = (colour & 1) ? 0xffffu : 0u;
    w[1] = (colour & 2) ? 0xffffu : 0u;
    w[2] = (colour & 4) ? 0xffffu : 0u;
    w[3] = (colour & 8) ? 0xffffu : 0u;
}

/* The one place a scanline address is formed.  The multiply is 16x16, which the
 * 68000 does have, so it compiles to a single MULU. */
static uint16_t *row_ptr(int y)
{
    return (uint16_t *)Atarist_BackBuffer() +
           (unsigned int)(unsigned short)y * (unsigned int)SCREEN_WORDS;
}

static void group_masked(uint16_t *p, uint16_t mask, const uint16_t *cw)
{
    uint16_t keep = (uint16_t)~mask;
    p[0] = (uint16_t)((p[0] & keep) | (cw[0] & mask));
    p[1] = (uint16_t)((p[1] & keep) | (cw[1] & mask));
    p[2] = (uint16_t)((p[2] & keep) | (cw[2] & mask));
    p[3] = (uint16_t)((p[3] & keep) | (cw[3] & mask));
}

/* A run resolved into "leading masked group, N whole groups, trailing masked
 * group".  Solving it ONCE and then walking scanlines is the difference
 * between a rectangle costing about 550 cycles a row and about 220: a masked
 * group is four read-modify-writes at roughly 144 cycles, a whole group is
 * four stores at roughly 48, and the per-call prologue is another 150 on top.
 * That arithmetic is why the duel HUD is laid out on 16-pixel boundaries --
 * an aligned rectangle has no masked groups at all. */
typedef struct Run {
    int      g0;          /* first group touched */
    uint16_t lead;        /* mask, or 0 when the leading group is whole */
    int      full;        /* whole groups after the leading one */
    uint16_t trail;       /* mask, or 0 when the trailing group is whole */
} Run;

static int run_solve(Run *r, int x, int w)
{
    int x0 = x, x1 = x + w, g1;

    if (w <= 0) return 0;
    if (x0 < 0) x0 = 0;
    if (x1 > ATARIST_SCREEN_W) x1 = ATARIST_SCREEN_W;
    if (x0 >= x1) return 0;

    r->g0 = x0 >> 4;
    g1 = (x1 - 1) >> 4;
    r->lead = r->trail = 0;
    r->full = 0;

    if (r->g0 == g1) {
        r->lead = (uint16_t)((0xffffu >> (x0 & 15)) &
                             (0xffffu << (15 - ((x1 - 1) & 15))));
        return 1;
    }
    if (x0 & 15) {
        r->lead = (uint16_t)(0xffffu >> (x0 & 15));
        r->full = g1 - r->g0 - 1;
    } else {
        r->full = g1 - r->g0;
    }
    if ((x1 & 15) == 0) ++r->full;
    else r->trail = (uint16_t)(0xffffu << (15 - ((x1 - 1) & 15)));
    return 1;
}

/* Paint one solved run into one scanline.  Whole groups go out as two longs
 * rather than four words: the plane pattern of a solid colour is constant, so
 * the 68000 moves 32 bits at a time and the fill halves. */
static void run_paint(const Run *r, uint16_t *row, const uint16_t *cw,
                      uint32_t l0, uint32_t l1)
{
    uint16_t *p = row + r->g0 * 4;
    int n = r->full;

    if (r->lead) { group_masked(p, r->lead, cw); p += 4; }
    while (n-- > 0) {
        *(uint32_t *)p = l0;
        *(uint32_t *)(p + 2) = l1;
        p += 4;
    }
    if (r->trail) group_masked(p, r->trail, cw);
}

static void span_row(uint16_t *row, int x, int w, const uint16_t *cw)
{
    Run r;
    if (!run_solve(&r, x, w)) return;
    run_paint(&r, row, cw,
              ((uint32_t)cw[0] << 16) | cw[1], ((uint32_t)cw[2] << 16) | cw[3]);
}

static int clip_rows(int *y, int *h)
{
    if (*h <= 0) return 0;
    if (*y < 0) { *h += *y; *y = 0; }
    if (*y + *h > ATARIST_SCREEN_H) *h = ATARIST_SCREEN_H - *y;
    return *h > 0;
}

void Atarist_HLine(int x, int y, int w, uint8_t colour)
{
    uint16_t cw[4];
    if (y < 0 || y >= ATARIST_SCREEN_H) return;
    colour_words(colour, cw);
    span_row(row_ptr(y), x, w, cw);
}

void Atarist_VLine(int x, int y, int h, uint8_t colour)
{
    uint16_t cw[4];
    uint16_t *p;
    uint16_t mask;
    if (!clip_rows(&y, &h)) return;
    if (x < 0 || x >= ATARIST_SCREEN_W) return;
    colour_words(colour, cw);
    mask = (uint16_t)(0x8000u >> (x & 15));
    p = row_ptr(y) + (x >> 4) * 4;
    while (h--) {
        group_masked(p, mask, cw);
        p += SCREEN_WORDS;
    }
}

void Atarist_FillRect(int x, int y, int w, int h, uint8_t colour)
{
    uint16_t cw[4];
    uint16_t *row;
    Run r;
    uint32_t l0, l1;
    if (!clip_rows(&y, &h)) return;
    if (!run_solve(&r, x, w)) return;
    colour_words(colour, cw);
    l0 = ((uint32_t)cw[0] << 16) | cw[1];
    l1 = ((uint32_t)cw[2] << 16) | cw[3];
    row = row_ptr(y);
    while (h--) {
        run_paint(&r, row, cw, l0, l1);
        row += SCREEN_WORDS;
    }
}

void Atarist_FrameRect(int x, int y, int w, int h, uint8_t colour)
{
    if (w <= 0 || h <= 0) return;
    Atarist_HLine(x, y, w, colour);
    Atarist_HLine(x, y + h - 1, w, colour);
    Atarist_VLine(x, y + 1, h - 2, colour);
    Atarist_VLine(x + w - 1, y + 1, h - 2, colour);
}

/* ── Text ────────────────────────────────────────────────────────────────── */

/* A glyph straddles at most two groups, so its two destination pointers are
 * formed once and both stepped a scanline at a time. */
static void draw_glyph(int x, int y, unsigned char ch, const uint16_t *cw)
{
    const uint8_t *g = &n2DLib_font[(ch & 0x7f) * 8];
    int shift = x & 15;
    int gx = x >> 4;
    int rows = 8;
    int y0 = y;
    uint16_t *p;
    int second = (gx + 1) < SCREEN_GROUPS;

    if (gx < 0 || gx >= SCREEN_GROUPS) return;
    if (y0 < 0) { g -= y0; rows += y0; y0 = 0; }
    if (y0 + rows > ATARIST_SCREEN_H) rows = ATARIST_SCREEN_H - y0;
    if (rows <= 0) return;

    p = row_ptr(y0) + gx * 4;
    while (rows--) {
        uint32_t wide = ((uint32_t)*g++ << 8) << (16 - shift);
        uint16_t hi = (uint16_t)(wide >> 16);
        uint16_t lo = (uint16_t)wide;
        if (hi) group_masked(p, hi, cw);
        if (lo && second) group_masked(p + 4, lo, cw);
        p += SCREEN_WORDS;
    }
}

void Atarist_DrawChar(int x, int y, unsigned char ch, uint8_t colour, uint8_t shadow)
{
    uint16_t cw[4];
    if (shadow != colour) {
        colour_words(shadow, cw);
        draw_glyph(x + 1, y + 1, ch, cw);
    }
    colour_words(colour, cw);
    draw_glyph(x, y, ch, cw);
}

void Atarist_DrawText(int x, int y, const char *s, uint8_t colour, uint8_t shadow)
{
    while (*s) {
        Atarist_DrawChar(x, y, (unsigned char)*s++, colour, shadow);
        x += 8;
    }
}

int Atarist_TextWidth(const char *s)
{
    int n = 0;
    while (s[n]) ++n;
    return n * 8;
}

void Atarist_DrawTextCentred(int cx, int y, const char *s, uint8_t colour, uint8_t shadow)
{
    /* Snapped to a multiple of eight, which is not cosmetic: at that alignment
     * a glyph falls inside a single 16-pixel group and costs one masked
     * read-modify-write a row instead of two. */
    Atarist_DrawText((cx - Atarist_TextWidth(s) / 2) & ~7, y, s, colour, shadow);
}

void Atarist_DrawNumber(int x_right, int y, int32_t value, uint8_t colour, uint8_t shadow)
{
    char buf[12];
    int n = 0, i;
    uint32_t v;
    int neg = value < 0;
    v = (uint32_t)(neg ? -value : value);
    /* Least significant digit first, which is also the order it is drawn in:
     * right to left from x_right. */
    do { buf[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < 11);
    if (neg && n < 11) buf[n++] = '-';
    for (i = 0; i < n; ++i) {
        x_right -= 8;
        Atarist_DrawChar(x_right, y, (unsigned char)buf[i], colour, shadow);
    }
}

/* ── Images ──────────────────────────────────────────────────────────────── */

void Atarist_BlitImageRect(const AtaristImage *img, int sx, int sy,
                           int w, int h, int x, int y)
{
    int groups_src = img->w >> 4;
    int gsx = sx >> 4;
    int gw = (w + 15) >> 4;
    int gdx = x >> 4;
    const uint16_t *planes = img->data;
    const uint16_t *mask = img->has_mask
        ? (img->data + (size_t)groups_src * img->h * 4) : 0;
    const uint16_t *sp;
    const uint16_t *mp;
    uint16_t *row;
    int rows = h, y0 = y, row_i = 0;

    if (gdx < 0 || gw <= 0) return;
    if (y0 < 0) { rows += y0; sy -= y0; y0 = 0; }
    if (y0 + rows > ATARIST_SCREEN_H) rows = ATARIST_SCREEN_H - y0;
    if (rows <= 0) return;
    if (gdx + gw > SCREEN_GROUPS) gw = SCREEN_GROUPS - gdx;
    if (gw <= 0) return;

    sp = planes + ((size_t)sy * groups_src + gsx) * 4;
    mp = mask ? (mask + (size_t)sy * groups_src + gsx) : 0;
    row = row_ptr(y0) + gdx * 4;

    for (row_i = 0; row_i < rows; ++row_i) {
        uint16_t *d = row;
        const uint16_t *s = sp;
        int g;
        for (g = 0; g < gw; ++g) {
            uint16_t m = mp ? mp[g] : 0xffffu;
            if (m == 0xffffu) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            } else if (m) {
                uint16_t keep = (uint16_t)~m;
                d[0] = (uint16_t)((d[0] & keep) | (s[0] & m));
                d[1] = (uint16_t)((d[1] & keep) | (s[1] & m));
                d[2] = (uint16_t)((d[2] & keep) | (s[2] & m));
                d[3] = (uint16_t)((d[3] & keep) | (s[3] & m));
            }
            d += 4;
            s += 4;
        }
        row += SCREEN_WORDS;
        sp += (size_t)groups_src * 4;
        if (mp) mp += groups_src;
    }
}

void Atarist_BlitImage(const AtaristImage *img, int x, int y)
{
    Atarist_BlitImageRect(img, 0, 0, img->w, img->h, x, y);
}
