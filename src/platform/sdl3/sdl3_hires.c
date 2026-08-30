/* See sdl3_hires.h. Decode + cover-crop; no CPU resample — the GPU mip chain
 * handles downscaling, so we only select the framing sub-rectangle. */

#include "sdl3_hires.h"

#include <stdlib.h>
#include <string.h>

#include "sdl3_image_load.h"
#include "sdl3_card_paths.h"

/* Copy the largest sub-rectangle of (src, sw x sh) with aspect ar = tw/th,
 * horizontally centered and vertically placed by anchor (0 = top .. 1 = bottom),
 * into a new tight RGBA buffer. */
static uint8_t *cover_crop(const uint8_t *src, int sw, int sh, float ar,
                           float anchor_y, int *out_w, int *out_h)
{
    int cw, ch, cx, cy, y;
    uint8_t *out;
    if (sw <= 0 || sh <= 0 || ar <= 0.0f) return NULL;
    if ((float)sw / (float)sh > ar) {
        ch = sh;
        cw = (int)((float)sh * ar + 0.5f);
    } else {
        cw = sw;
        ch = (int)((float)sw / ar + 0.5f);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (cw > sw) cw = sw;
    if (ch > sh) ch = sh;
    cx = (sw - cw) / 2;
    cy = (int)((float)(sh - ch) * anchor_y + 0.5f);
    if (cy < 0) cy = 0;
    if (cy > sh - ch) cy = sh - ch;
    out = (uint8_t *)malloc((size_t)cw * (size_t)ch * 4);
    if (!out) return NULL;
    for (y = 0; y < ch; ++y) {
        memcpy(out + (size_t)y * cw * 4,
               src + ((size_t)(cy + y) * sw + cx) * 4,
               (size_t)cw * 4);
    }
    *out_w = cw;
    *out_h = ch;
    return out;
}

int waifu_sdl3_hires_card_count(void)
{
    return WAIFU_SDL3_CARD_SRC_COUNT;
}

int waifu_sdl3_hires_has_card(int card_id)
{
    if (card_id < 0 || card_id >= WAIFU_SDL3_CARD_SRC_COUNT) return 0;
    return waifu_sdl3_card_src[card_id] && waifu_sdl3_card_src[card_id][0];
}

uint8_t *waifu_sdl3_hires_card_decode(int card_id, int kind, int *w, int *h)
{
    const char *path;
    uint8_t *src;
    uint8_t *crop;
    int sw = 0, sh = 0;
    float ar, anchor;
    if (!waifu_sdl3_hires_has_card(card_id)) return NULL;
    path = waifu_sdl3_card_src[card_id];
    src = waifu_sdl3_image_load_rgba(path, &sw, &sh);
    if (!src) return NULL;
    if (kind == WAIFU_HIRES_BIG) {
        ar = 1.0f;          /* square, matching the 112x112 big-art cover */
        anchor = 0.40f;
    } else {
        ar = 1.0f;          /* the card-face art window is a 30x30 square */
        anchor = 0.12f;     /* bias toward the top (face / upper torso) */
    }
    crop = cover_crop(src, sw, sh, ar, anchor, w, h);
    free(src);
    return crop;
}

/* ---- story portraits ------------------------------------------------------
 * The decoded portrait always comes out exactly PORTRAIT_BOX_W x PORTRAIT_BOX_H,
 * an integer multiple of the game's 124x200 portrait cell, so the frontend can
 * drop it straight onto the rect the 8bpp blit would have covered with no
 * placement maths at the call site. */

#define PORTRAIT_BOX_W (124 * WAIFU_SDL3_PORTRAIT_SCALE)
#define PORTRAIT_BOX_H (200 * WAIFU_SDL3_PORTRAIT_SCALE)
/* Air above the hair, in box units (6 game pixels), the same for every
   portrait -- it is what makes them line up with each other. */
#define PORTRAIT_HEADROOM (6 * WAIFU_SDL3_PORTRAIT_SCALE)

/* Alpha bounding box of an RGBA image; returns 0 if fully transparent. */
static int alpha_bbox(const uint8_t *px, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
    int x, y, minx = w, miny = h, maxx = -1, maxy = -1;
    for (y = 0; y < h; ++y) {
        const uint8_t *row = px + (size_t)y * w * 4;
        for (x = 0; x < w; ++x) {
            if (row[x * 4 + 3] == 0) continue;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (maxx < 0) return 0;
    *x0 = minx; *y0 = miny; *x1 = maxx + 1; *y1 = maxy + 1;
    return 1;
}

/* Area-average resample of a sub-rectangle into dst_w x dst_h. Alpha-weighted,
 * so the transparent border of a cut-out figure does not bleed its (black)
 * colour into the silhouette edge. */
static void resample_box(const uint8_t *src, int sw, int srx, int sry, int srw, int srh,
                         uint8_t *dst, int dst_w, int dst_h)
{
    int dx, dy;
    /* The destination can be LARGER than the source rect (a portrait whose art
       is smaller than the portrait cell), in which case a destination pixel
       covers less than one source pixel and the span collapses. Widening it by
       one must not walk off the end of the rectangle -- that read past the last
       row is what corrupted the heap the first time round. */
    for (dy = 0; dy < dst_h; ++dy) {
        int sy0 = sry + (int)((int64_t)dy * srh / dst_h);
        int sy1 = sry + (int)((int64_t)(dy + 1) * srh / dst_h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > sry + srh) { sy1 = sry + srh; sy0 = sy1 - 1; }
        for (dx = 0; dx < dst_w; ++dx) {
            int sx0 = srx + (int)((int64_t)dx * srw / dst_w);
            int sx1 = srx + (int)((int64_t)(dx + 1) * srw / dst_w);
            uint32_t ar = 0, ag = 0, ab = 0, aa = 0;
            int n = 0, x, y;
            uint8_t *o;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > srx + srw) { sx1 = srx + srw; sx0 = sx1 - 1; }
            for (y = sy0; y < sy1; ++y) {
                const uint8_t *row = src + ((size_t)y * sw + sx0) * 4;
                for (x = sx0; x < sx1; ++x, row += 4) {
                    uint32_t a = row[3];
                    ar += row[0] * a; ag += row[1] * a; ab += row[2] * a; aa += a;
                    ++n;
                }
            }
            o = dst + ((size_t)dy * dst_w + dx) * 4;
            if (aa == 0 || n == 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            o[0] = (uint8_t)(ar / aa);
            o[1] = (uint8_t)(ag / aa);
            o[2] = (uint8_t)(ab / aa);
            o[3] = (uint8_t)(aa / (uint32_t)n);
        }
    }
}

uint8_t *waifu_sdl3_hires_portrait_decode(int portrait_id, int *w, int *h)
{
    const char *path;
    uint8_t *src, *box, *art;
    int sw = 0, sh = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int tw, th, ch, dst_h, oy, y;

    if (portrait_id < 0 || portrait_id >= WAIFU_SDL3_PORTRAIT_SRC_COUNT) return NULL;
    path = waifu_sdl3_portrait_src[portrait_id];
    if (!path || !path[0]) return NULL;
    src = waifu_sdl3_image_load_rgba(path, &sw, &sh);
    if (!src) return NULL;
    if (!alpha_bbox(src, sw, sh, &bx0, &by0, &bx1, &by1)) {
        bx0 = by0 = 0; bx1 = sw; by1 = sh;
    }
    tw = bx1 - bx0;
    th = by1 - by0;

    /* NEVER crop the sides. The figure's full width is kept and the cell's
       aspect decides how much of its HEIGHT fits; taking that from the top of
       the alpha box keeps the head in frame. A figure taller than the cell
       therefore shows its upper body (the framing this screen wants), and one
       shorter than the cell shows all of it, standing on the bottom edge with
       transparency above -- which is the console portrait's own framing. */
    ch = (int)(((long long)tw * PORTRAIT_BOX_H + PORTRAIT_BOX_W / 2) / PORTRAIT_BOX_W);
    if (ch > th) ch = th;
    if (ch < 1) ch = 1;

    dst_h = (int)(((long long)ch * PORTRAIT_BOX_W + tw / 2) / tw);
    if (dst_h > PORTRAIT_BOX_H) dst_h = PORTRAIT_BOX_H;
    if (dst_h < 1) dst_h = 1;

    box = (uint8_t *)calloc((size_t)PORTRAIT_BOX_W * PORTRAIT_BOX_H, 4);
    art = (uint8_t *)malloc((size_t)PORTRAIT_BOX_W * dst_h * 4);
    if (!box || !art) { free(box); free(art); free(src); return NULL; }
    resample_box(src, sw, bx0, by0, tw, ch, art, PORTRAIT_BOX_W, dst_h);
    free(src);

    /* Every portrait hangs from the SAME headroom, never from the bottom of the
       cell. The crop starts at the top of the alpha box, so a fixed top offset
       puts every character's hair at the same height -- bottom-anchoring instead
       made a figure that filled the cell (a narrow, tall source like Serena)
       sit a good 27 game pixels above one that did not (the wide 1086x1448
       sources), which is exactly how it read on screen. Anything that runs past
       the bottom of the cell is behind the dialogue box, which covers the cell's
       last 48 game pixels. */
    oy = PORTRAIT_HEADROOM;
    for (y = 0; y < dst_h && oy + y < PORTRAIT_BOX_H; ++y) {
        memcpy(box + ((size_t)(oy + y) * PORTRAIT_BOX_W) * 4,
               art + ((size_t)y * PORTRAIT_BOX_W) * 4, (size_t)PORTRAIT_BOX_W * 4);
    }
    free(art);
    *w = PORTRAIT_BOX_W;
    *h = PORTRAIT_BOX_H;
    return box;
}

uint8_t *waifu_sdl3_hires_card_back_decode(int *w, int *h)
{
    if (!waifu_sdl3_card_back_src || !waifu_sdl3_card_back_src[0]) return NULL;
    return waifu_sdl3_image_load_rgba(waifu_sdl3_card_back_src, w, h);
}

uint8_t *waifu_sdl3_hires_board_tile_decode(int index, int *w, int *h)
{
    if (index < 0 || index >= WAIFU_SDL3_BOARD_TILE_SRC_COUNT) return NULL;
    if (!waifu_sdl3_board_tile_src[index][0]) return NULL;
    return waifu_sdl3_image_load_rgba(waifu_sdl3_board_tile_src[index], w, h);
}

uint8_t *waifu_sdl3_hires_title_decode(int *w, int *h)
{
    if (!waifu_sdl3_title_src || !waifu_sdl3_title_src[0]) return NULL;
    return waifu_sdl3_image_load_rgba(waifu_sdl3_title_src, w, h);
}

uint8_t *waifu_sdl3_hires_ending_decode(int *w, int *h)
{
    if (!waifu_sdl3_ending_src || !waifu_sdl3_ending_src[0]) return NULL;
    return waifu_sdl3_image_load_rgba(waifu_sdl3_ending_src, w, h);
}
