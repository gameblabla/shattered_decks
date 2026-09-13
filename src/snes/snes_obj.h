/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_obj.h — the sprite layer: the HUD, the hand, and the overhead cursor.
 *
 *  THE HUD IS SPRITES AND NOT BOARD, and the reason is resolution.  The duel
 *  board is a software-rendered picture, and a letter drawn into it would
 *  cost a render every time it changed; the OBJ layer is drawn by the PPU at
 *  the screen's own resolution over whichever background is up, and this
 *  machine allows thirty-two sprites and thirty-four 8x8 slivers on a
 *  scanline -- five 32x32 cards across a row is twenty of those slivers, so
 *  the budget is not close.
 *
 *  VRAM, and why nothing here is ever rewritten in bulk (snes_video.h has
 *  the whole map):
 *
 *      words $0000-$5FFF   the Mode 3 direct-colour board's tiles and maps
 *      words $6000-$7FFF   OBJ characters: 20 card sprite slots of 32x32,
 *                          then the HUD font and the cursor's corner brackets
 *
 *  CGRAM 128..255 is the eight OBJ palettes; the board's direct colour reads
 *  no CGRAM at all.  The battle (snes_battle.c) takes VRAM and CGRAM for its
 *  own layout and snesObjInit puts all of this back afterwards.
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

/* Twenty card sprite slots; the hand uses the first five, and those five
 * are also the ones with an OBJ palette of their own. */
#define SNES_OBJ_CARDS       20
#define SNES_OBJ_NO_FACE     0xFFu

void snesObjInit(void);

/* One frame's sprite list.  It is rebuilt from scratch every frame rather than
 * patched: at a few dozen sprites that is a rounding error next to a board
 * render, and a list that is never patched cannot hold a sprite from a screen
 * the player left. */
void snesObjBegin(void);
void snesObjSprite(s16 x, s16 y, u16 tile, u8 pal, u8 big);
void snesObjSpriteFlip(s16 x, s16 y, u16 tile, u8 pal, u8 big, u8 flip);
void snesObjText(s16 x, s16 y, const char *s);
void snesObjNum(s16 x, s16 y, u16 value, u8 digits);
/* SNES_SPR_ICON_ATK or SNES_SPR_ICON_DEF: the sword and the shield the stat
 * row prints where it used to spell ATK and DEF. */
void snesObjIcon(s16 x, s16 y, u8 kind);
/* The four corner brackets of a w x h box -- the cursor, in either view.
 * snesObjBox is the gold one the hand uses; snesObjBoxRed is the board cursor
 * the top view puts round the slot being inspected, which is the colour the
 * PC-FX and FM TOWNS builds draw that same marker in. */
void snesObjBox(s16 x, s16 y, u8 w, u8 h);
/* The index the next sprite will take, and a sprite's y rewritten in place
 * after snesObjEnd: what lets the hand slide every field while a board frame
 * is on its way without rebuilding the whole list (a rebuild is more than a
 * field of 816-tcc). */
u8   snesObjCount(void);
void snesObjPatchY(u8 index, s16 y, u8 big);
/* Mark the list for upload.  pvsneslib's NMI copies ITS OWN (empty) OAM
 * buffer over the hardware whenever the main loop is waiting on it, so a
 * game frame that does not rebuild the list must still re-upload it. */
void snesObjTouch(void);
void snesObjBoxRed(s16 x, s16 y, u8 w, u8 h);
/* The result banner: 16x16 letters, four 8x8 sprites each, at a 16-pixel
 * pitch.  `set` is SNES_SPR_BIG_SET_GOLD or SNES_SPR_BIG_SET_RED.  Only the
 * characters the two banners use exist in the sheet; anything else is a gap. */
void snesObjBigText(s16 x, s16 y, const char *s, u8 set);
#define SNES_OBJ_BIG_PITCH  16
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
/* GREY IS A PALETTE, NOT A SECOND SHEET.  A hand card that is not the one
 * under the cursor is drawn through the same tiles with `snes_spr_face_pal`
 * swapped for `snes_spr_face_pal_grey` -- the face's own fifteen colours taken
 * to luma and darkened -- so the hand reads as one lit card among four dimmed
 * ones for thirty-two bytes of CGRAM instead of five hundred and twelve bytes
 * of VRAM a card.  It only applies on top of snesObjCardHiRes: a slot sharing
 * a clustered palette has no palette of its own to grey. */
void snesObjCardGrey(u8 on);
void snesObjEnd(void);

/* Called once a vblank, before the framebuffer's own upload asks for what is
 * left: pushes OAM and at most a couple of card faces. */
void snesObjVblank(void);
u16  snesObjVblankBytes(void);
/* Whether every card asked for is actually in VRAM. */
u8   snesObjCardsReady(void);

#endif /* WAIFU_SNES_OBJ_H */
