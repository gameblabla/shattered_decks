/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_obj.h — the sprite layer: the HUD, the hand, and the top view's cards.
 *
 *  THE HUD IS SPRITES AND NOT BITMAP, and the reason is resolution.  The duel
 *  board is a Mode 7 chunky bitmap sampled at 2x2 screen pixels a texel in
 *  every 3D state, so a letter drawn into it is a letter at half the
 *  console's resolution and a card in hand is sixteen texels stretched across
 *  thirty-two pixels.  The OBJ layer is drawn by the PPU at the screen's own
 *  resolution, over whichever background mode is up, and this machine allows
 *  thirty-two sprites and thirty-four 8x8 slivers on a scanline -- five 32x32
 *  cards across a row is twenty of those slivers, so the budget is not close.
 *
 *  VRAM, and why nothing here is ever rewritten in bulk:
 *
 *      words $0000-$3FFF   the Mode 7 bitmap (tilemap in the low bytes,
 *                          characters in the high ones -- see snes_fb.asm)
 *      words $4000-$5FFF   OBJ characters: 20 card sprites of 32x32, then the
 *                          HUD font and the cursor's corner brackets
 *      words $6000-$6FFF   the top view's 8bpp background characters
 *      words $7000-$73FF   the top view's tilemap
 *
 *  CGRAM 0..127 is the top view's background palette and 128..255 the eight
 *  OBJ palettes; Mode 7 direct colour reads no CGRAM at all, so both views
 *  share one CGRAM and A MODE CHANGE TOUCHES NO COLOUR AND NO VRAM.  That is
 *  what lets the duel walk up into the top view in three register writes,
 *  inside one vblank, with no force blank and therefore no black frame.
 *
 *  A card sprite's tiles are FOUR ROWS OF FOUR, because a 32x32 sprite at name
 *  n covers n..n+3 on each of four rows of the sixteen-wide name table.  Four
 *  cards therefore share a 64-name group, and one card is uploaded as four
 *  128-byte chunks 512 bytes apart rather than as one 512-byte block.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_OBJ_H
#define WAIFU_SNES_OBJ_H

#include "snes_types.h"
#include "snes_obj_data.h"

/* Twenty card sprites: the whole field in the top view, or the five in hand
 * in the board view, which reuses the first five slots. */
#define SNES_OBJ_CARDS       20
#define SNES_OBJ_NO_FACE     0xFFu

void snesObjInit(void);

/* One frame's sprite list.  It is rebuilt from scratch every frame rather than
 * patched: at a few dozen sprites that is a rounding error next to a board
 * render, and a list that is never patched cannot hold a sprite from a screen
 * the player left. */
void snesObjBegin(void);
void snesObjSprite(s16 x, s16 y, u16 tile, u8 pal, u8 big);
void snesObjText(s16 x, s16 y, const char *s);
void snesObjNum(s16 x, s16 y, u16 value, u8 digits);
/* SNES_SPR_ICON_ATK or SNES_SPR_ICON_DEF: the sword and the shield the stat
 * row prints where it used to spell ATK and DEF. */
void snesObjIcon(s16 x, s16 y, u8 kind);
/* The four corner brackets of a w x h box -- the cursor, in either view. */
void snesObjBox(s16 x, s16 y, u8 w, u8 h);
/* One side's life panel: a coloured label plate, a gauge and the number, all
 * of it sprites.  `side` is 0 for the player (red) and 1 for the opponent
 * (blue), which is the pairing the PC and PC-FX builds' LP panels use.  The
 * panel is SNES_OBJ_LIFE_W wide and one 8-pixel row tall. */
#define SNES_OBJ_LIFE_W   96
void snesObjLifePanel(s16 x, s16 y, u8 side, u16 lp, u16 lp_max);
/* A 32x32 card face in OBJ card slot `slot`.  The face's tiles are uploaded by
 * snesObjVblank over the following vblanks; until they are there the sprite is
 * not emitted, because a card drawn out of tiles that still hold the previous
 * face is worse than a card that arrives a frame late. */
void snesObjCard(s16 x, s16 y, u8 slot, u8 face);
void snesObjCardFlip(s16 x, s16 y, u8 slot, u8 face, u8 flip);
void snesObjQueueCard(u8 slot, u8 face, u8 hi);
/* WHICH SHEET THE NEXT snesObjCard CALLS DRAW FROM, and it is a palette
 * decision as much as a tile one.
 *
 * Off (the default) is the clustered sheet: a face shares its fifteen colours
 * with the ten or so nearest it in colour, which is the only thing that works
 * when the top view puts twenty cards on screen against seven OBJ palettes.
 *
 * On is the per-face sheet: the slot's OBJ palette is loaded with THAT FACE'S
 * OWN fifteen colours as its tiles go up, so the card is quantised against
 * nothing but itself.  A slot may only ask for it if its index is also a valid
 * card palette (slot < SNES_SPR_CARD_PALS), because the slot IS the palette --
 * which the five hand slots are and the top view's twenty are not. */
void snesObjCardHiRes(u8 on);
void snesObjEnd(void);

/* Called once a vblank, before the framebuffer's own upload asks for what is
 * left: pushes OAM and at most a couple of card faces. */
void snesObjVblank(void);
/* Whether every card asked for is actually in VRAM. */
u8   snesObjCardsReady(void);

#endif /* WAIFU_SNES_OBJ_H */
