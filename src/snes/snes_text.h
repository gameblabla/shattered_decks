/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_text.h — the HUD's letters.
 *
 *  Eight texels a glyph and no proportional spacing: the HUD band is 128 texels
 *  wide, so a line is exactly sixteen characters, and every string in the duel
 *  is written to fit that rather than to be clipped by it.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_TEXT_H
#define WAIFU_SNES_TEXT_H

#include "snes_types.h"

#define SNES_TEXT_W    8
#define SNES_TEXT_H    8
#define SNES_TEXT_COLS 16

/* snes_raster.asm: one 8x8 glyph, transparent where the font bit is clear. */
void snesTextGlyph(u16 fb_index, u16 ch, u16 ink, u16 stride);

void snesTextAt(u8 x, u8 y, const char *s, u8 ink, u8 shadow);
void snesTextNum(u8 x, u8 y, u16 v, u8 digits, u8 ink, u8 shadow);

#endif /* WAIFU_SNES_TEXT_H */
