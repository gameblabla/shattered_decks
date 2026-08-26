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
