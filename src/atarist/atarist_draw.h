/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_draw.h — direct planar drawing.
 *
 *  Everything here writes straight into the four interleaved bitplanes of the
 *  back buffer at the full 320x200, with no chunky staging and no C2P.  That is
 *  deliberate and it is the requirement: the 3D board pays for its downscale,
 *  the card art must not.
 *
 *  Coordinates are screen pixels.  Colours are palette indices 0..15, resolved
 *  against whichever half of the raster split the row falls in.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_DRAW_H
#define WAIFU_ATARIST_DRAW_H

#include <stdint.h>

/* Solid rectangle.  Fast path when x and w are both multiples of 16. */
void Atarist_FillRect(int x, int y, int w, int h, uint8_t colour);
/* One-pixel outline. */
void Atarist_FrameRect(int x, int y, int w, int h, uint8_t colour);
void Atarist_HLine(int x, int y, int w, uint8_t colour);
void Atarist_VLine(int x, int y, int h, uint8_t colour);

/* 8x8 bitmap text.  `shadow` is drawn one pixel down-right first when it is
 * not equal to `colour`, which is what keeps HUD text legible over card art. */
void Atarist_DrawChar(int x, int y, unsigned char ch, uint8_t colour, uint8_t shadow);
void Atarist_DrawText(int x, int y, const char *s, uint8_t colour, uint8_t shadow);
void Atarist_DrawTextCentred(int cx, int y, const char *s, uint8_t colour, uint8_t shadow);
int  Atarist_TextWidth(const char *s);
/* Unsigned decimal, right-aligned at `x_right`.  No printf in this build. */
void Atarist_DrawNumber(int x_right, int y, int32_t value, uint8_t colour, uint8_t shadow);

/* A 4-plane image blitted with its own 1-bit mask.  `src` is
 * (w/16) * h * 4 words of plane data followed by (w/16) * h words of mask,
 * which is the layout tools/atarist/gen_atarist_assets.py emits.  x is rounded
 * down to a 16-pixel boundary unless ATARIST_BLIT_SHIFT is used. */
typedef struct AtaristImage {
    uint16_t w;         /* pixels, multiple of 16 */
    uint16_t h;
    uint16_t has_mask;
    const uint16_t *data;
} AtaristImage;

void Atarist_BlitImage(const AtaristImage *img, int x, int y);
/* Sub-rectangle of an image, for a card front cut out of a sheet. */
void Atarist_BlitImageRect(const AtaristImage *img, int sx, int sy,
                           int w, int h, int x, int y);

#endif /* WAIFU_ATARIST_DRAW_H */
