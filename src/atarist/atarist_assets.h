/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_assets.h — the art, and the two palettes it is held in.
 *
 *  Everything here comes off the floppy, converted from assets/source/ by
 *  tools/atarist/gen_atarist_assets.py.  Nothing is drawn procedurally any
 *  more: the board is a real sandstone checkerboard and a card face is the
 *  card's own painting (src/generated/atarist_art.h).
 *
 *  Two palettes, one per half of the raster split, and both are cut the same
 *  way.  Entries 0..7 are the structure of that half -- for ARENA the
 *  checkerboard tiles, the groove between them, the slab's rim; for CARD the
 *  panel, the HUD inks.  Entries 8..13 plus black and white are an EIGHT-STEP
 *  GREY RAMP, at the same indices in both, and the card paintings are dithered
 *  into nothing else.  That is why a card reads identically in the hand and on
 *  the 3D board even though the two halves share no palette, and why the board
 *  half still has all eight of its colours for the board.
 *
 *  Textures are stored with every byte already multiplied by four -- the chunky
 *  buffer's convention, see atarist_c2p.S -- so the rasteriser's inner loop is
 *  a plain byte copy and the loader is a plain read.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_ASSETS_H
#define WAIFU_ATARIST_ASSETS_H

#include <stdint.h>
#include "atarist_board3d.h"
#include "atarist_draw.h"
#include "atarist_art.h"

/* ARENA palette indices — the contract with ARENA_SLOTS in the converter. */
#define ARENA_BLACK      0    /* the surround: the slab floats over it */
#define ARENA_GROOVE     1    /* the line between two tiles */
#define ARENA_TILE_DARK  2
#define ARENA_TILE_LIGHT 3
#define ARENA_TILE_LIT2  4    /* the light tile's second tone, its grain */
#define ARENA_RIM_SIDE   5    /* the slab's front face */
#define ARENA_RIM_TOP    6    /* the lit edge along the top of that face */
#define ARENA_SLOT       7    /* a card's rim, and the cursor tint */
/* 8..13 are the six greys of the card ramp; with ARENA_BLACK and ARENA_WHITE
 * they are the eight levels a painting is dithered into.  Nothing but the card
 * art uses them, and the CARD palette holds the same ramp at the same
 * indices. */
#define ARENA_WHITE     14
#define ARENA_HILIGHT   15

/* CARD palette indices — the contract with CARD_SLOTS in the converter. */
#define CARD_BLACK       0
#define CARD_PANEL_DARK  1
#define CARD_PANEL_MID   2
#define CARD_PANEL_LIGHT 3
#define CARD_GOLD        4
#define CARD_RED         5
#define CARD_GREEN       6
/* 7 is spare; 8..13 are the six greys of the card ramp, which with CARD_BLACK
 * and CARD_WHITE are the eight levels a painting is dithered into -- the same
 * indices the ARENA palette puts them at. */
#define CARD_WHITE      14
#define CARD_YELLOW     15
/* The two the HUD used to spend its own entries on.  A panel tone is what they
 * always meant, and the entries they cost are worth more to the paintings. */
#define CARD_GREY       CARD_PANEL_MID
#define CARD_SILVER     CARD_PANEL_LIGHT

#define ATARIST_ARENA_TEX_LOG2 7      /* 128 x 128, ATARIST_ART_ARENA_W */
/* Texels per world unit in the arena texture.  The duel camera has to scale
 * world coordinates to texels with the same number the texture was baked with,
 * so it is published here rather than duplicated. */
#define ATARIST_TEXELS_PER_UNIT 16
#define ATARIST_CARD_TEX_LOG2  5      /* 32 x 32 */

/* A face is a card id: monsters 0..71, supports 72..77, and the back, which is
 * also what a face-down card shows. */
#define ATARIST_CARD_FACE_BACK ATARIST_ART_FACE_BACK

/* Loads the art off the floppy.  0 when the machine had no room for it or the
 * files are missing; the game still runs, with flat colours where the art
 * would have been. */
int  Atarist_AssetsInit(void);
/* Non-zero once the real art is resident. */
int  Atarist_AssetsHaveArt(void);

void Atarist_ApplyArenaPalette(void);
void Atarist_ApplyCardPalette(void);
/* Both halves get the CARD set, for the screens that have no split. */
void Atarist_ApplyCardPaletteEverywhere(void);

const AtaristTexture *Atarist_ArenaTexture(void);
/* The 32x32 board texture for a face, in the ARENA palette. */
const AtaristTexture *Atarist_CardFace(int face);
/* The same card as a planar image for the hand, in the CARD palette.  Null
 * when the art is not resident. */
const AtaristImage   *Atarist_CardHandImage(int face);
/* Which face a card id shows. */
int  Atarist_CardFaceForCard(int card_id, int face_up);

#endif /* WAIFU_ATARIST_ASSETS_H */
