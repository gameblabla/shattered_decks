/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_assets.h — palettes and the textures the board rasteriser samples.
 *
 *  Two 16-colour sets, one per half of the raster split.  The ARENA set serves
 *  the 3D board; the CARD set serves the hand panel and the HUD, and is free to
 *  spend all sixteen entries on card art because it never has to hold a sky.
 *
 *  Textures are stored with every byte already multiplied by four -- the chunky
 *  buffer's convention, see atarist_c2p.S -- so the rasteriser's inner loop is
 *  a plain byte copy.  Nothing outside this module and the C2P should ever have
 *  to know that.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_ASSETS_H
#define WAIFU_ATARIST_ASSETS_H

#include <stdint.h>
#include "atarist_board3d.h"

/* ARENA palette indices. */
#define ARENA_BLACK      0
#define ARENA_SKY_DARK   1
#define ARENA_SKY_MID    2
#define ARENA_SKY_LIGHT  3
#define ARENA_SAND_DARK  4
#define ARENA_SAND_MID   5
#define ARENA_SAND_LIGHT 6
#define ARENA_GRID       7
#define ARENA_SLOT       8
#define ARENA_BACK       9
#define ARENA_FACE      10
#define ARENA_ATTR_A    11
#define ARENA_ATTR_B    12
#define ARENA_ATTR_C    13
#define ARENA_WHITE     14
#define ARENA_HILIGHT   15

/* CARD palette indices. */
#define CARD_BLACK       0
#define CARD_PANEL_DARK  1
#define CARD_PANEL_MID   2
#define CARD_PANEL_LIGHT 3
#define CARD_GOLD        4
#define CARD_RED         5
#define CARD_GREEN       6
#define CARD_BLUE        7
#define CARD_PURPLE      8
#define CARD_CYAN        9
#define CARD_ORANGE     10
#define CARD_BROWN      11
#define CARD_GREY       12
#define CARD_SILVER     13
#define CARD_WHITE      14
#define CARD_YELLOW     15

#define ATARIST_ARENA_TEX_LOG2 7      /* 128 x 128 */
/* Texels per world unit in the arena texture.  The duel camera has to scale
 * world coordinates to texels with the same number the texture was baked with,
 * so it is published here rather than duplicated. */
#define ATARIST_TEXELS_PER_UNIT 8
#define ATARIST_CARD_TEX_LOG2  5      /* 32 x 32 */
/* Card faces are generated per attribute, not per card: 78 individual faces
 * would be 80 KB of a 512 KB machine, and the art conversion that will replace
 * them belongs on the floppy, streamed per duel.  Six attributes plus a back
 * and a set (face-down) face is what the board needs to be readable. */
#define ATARIST_CARD_FACES     8
#define ATARIST_CARD_FACE_BACK 6
#define ATARIST_CARD_FACE_SET  7

int  Atarist_AssetsInit(void);       /* 0 when the machine had no room */
void Atarist_ApplyArenaPalette(void);
void Atarist_ApplyCardPalette(void);
/* Both halves get the CARD set, for the screens that have no split. */
void Atarist_ApplyCardPaletteEverywhere(void);

const AtaristTexture *Atarist_ArenaTexture(void);
const AtaristTexture *Atarist_CardFace(int face);
/* Which generated face a card id should show. */
int  Atarist_CardFaceForCard(int card_id, int face_up);

#endif /* WAIFU_ATARIST_ASSETS_H */
