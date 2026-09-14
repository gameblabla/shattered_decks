/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_cardart.h — the Mode 3 card presentation: card check and battle.
 *
 *  Both screens are the same picture the PC-FX and FM TOWNS builds draw --
 *  the 120x160 gold-rimmed battle card with its 112x112 painting -- and on
 *  this machine that picture is Mode 3's 8bpp BG1, which is what gives each
 *  card eighty colours of its own instead of an OBJ palette's fifteen.  The
 *  words are BG2 tiles; nothing on either screen is a sprite.
 *
 *  VRAM while one of these screens is up:
 *      $0000-$3FFF  BG1 tiles: card slot 0, card slot 1, the shared frame feet
 *      $4000-$73FF  untouched, so the duel's sprite sheet and top view survive
 *      $7400-$77FF  BG1 map
 *      $7800-$7BFF  BG2 map
 *      $7C00-$7FFF  BG2 font (BG2 character base $7000, tile 192 upwards)
 *  Both maps scroll by (-4, -22) so a card's tile column 0 lands on screen
 *  x = 4 and its row 0 on line 22, exactly where the shared code puts them.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_CARDART_H
#define WAIFU_SNES_CARDART_H

#include "snes_types.h"

#define SNES_CARDART_X0     4       /* screen x of map column 0 */
#define SNES_CARDART_Y0     22      /* screen y of map row 0 */
#define SNES_CARDART_COL_L  0       /* the left card's map column */
#define SNES_CARDART_COL_R  16      /* the right card's */
#define SNES_CARDART_TEXT_COL 16    /* the check screen's text column */
#define SNES_CARDART_TEXT_W   15

#define SNES_CARDART_PAL_WHITE 0
#define SNES_CARDART_PAL_GOLD  1

/* Force blank, then Mode 3 with BG1 + BG2, the font and the frame palettes.
 * `navy` paints the backdrop the card check's panel colour; otherwise black.
 * The caller turns the screen back on. */
void snesCardArtEnter(u8 navy);
void snesCardArtLoadCommon(void);
/* Load one face's tiles and palette into card slot 0 or 1. */
void snesCardArtLoad(u8 slot, u8 face);
/* Put slot `slot` on the BG1 map at tile column `col`, with the frame foot
 * for `face`'s kind under it. */
void snesCardArtPlace(u8 slot, u8 col, u8 face);
void snesCardArtClear(void);
/* The fifteen cells of one tile row (0..19) of a card in `slot`, for the
 * battle's own map. */
void snesCardArtRowCells(u8 slot, u8 face, u8 ty, u16 *cells);
#define SNES_CARDART_BLANK_TILE 511u
/* BG2 text.  Rows and columns are map cells; see the scroll above. */
void snesCardArtText(u8 col, u8 row, const char *s, u8 pal);
void snesCardArtNum(u8 col, u8 row, u16 value, u8 digits, u8 pal);
/* Word-wrapped text; returns the number of rows it used. */
u8   snesCardArtWrap(u8 col, u8 row, u8 width, u8 max_rows, const char *s,
                     u8 pal);
void snesCardArtTextClear(void);
/* The ATK/DEF plate and tribe under a monster's painting. */
void snesCardArtStats(u8 col, u8 face, u16 atk, u16 def);
/* The whole card check screen: the card on the left, the PC-FX text column
 * on the right.  `has_stats` prints the slot's own ATK/DEF, else the card's. */
void snesCardArtCheck(u8 face, u8 has_stats, u16 atk, u16 def);
/* Full-screen duel beats built from the same large-card stage.  Effect shows
 * a support card and its rules text; Destroyed gives one Thunder victim its
 * own reveal; FusionBegin preloads the material/result pair and FusionResult
 * swaps the map to the already-resident result without another art DMA. */
void snesCardArtEffect(u8 face, u8 by_com);
void snesCardArtDestroyed(u8 face, u8 index, u8 total);
void snesCardArtFusionBegin(u8 material, u8 result, u8 count, u8 success);
void snesCardArtFusionResult(u8 result, u8 count, u8 success);
/* The battle reveal: BG1 shown only inside two windows that open from the
 * screen's edges inwards.  `reveal` is 0..120 pixels; 255 turns masking off. */
void snesCardArtReveal(u8 reveal);
/* Add a white fixed-colour wash to both backgrounds.  Zero disables it. */
void snesCardArtFlash(u8 level);
/* Vblank: DMAs whatever map changed and applies the pending window. */
void snesCardArtVblank(void);

#endif /* WAIFU_SNES_CARDART_H */
