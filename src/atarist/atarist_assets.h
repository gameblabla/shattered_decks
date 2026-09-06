/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_assets.h — the art, and the two palettes it is held in.
 *
 *  Everything here comes off the floppy, converted from assets/source/ by
 *  tools/atarist/gen_atarist_assets.py.  Nothing is drawn procedurally any
 *  more: the ground is real sandstone, a card face is the card's own painting,
 *  and the two sixteen-colour sets are FITTED to those pictures rather than
 *  written by hand (src/generated/atarist_art.h).
 *
 *  Two palettes, one per half of the raster split.  The ARENA set serves the
 *  3D board -- sky, sand, the board markings -- and the CARD set serves the
 *  hand and the HUD.  In both, the entries the structure does not need are
 *  given to the card art, which is why the same painting is converted twice:
 *  once for the board texture and once for the hand.
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
#define ARENA_BLACK      0
#define ARENA_SKY_DARK   1
#define ARENA_SKY_MID    2
#define ARENA_SKY_LIGHT  3
#define ARENA_SAND_DARK  4
#define ARENA_SAND_MID   5
#define ARENA_SAND_LIGHT 6
#define ARENA_GRID       7
#define ARENA_SLOT       8
/* 9..13 are the fitted art entries and have no fixed meaning. */
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
/* 7..13 are the fitted art entries. */
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
#define ATARIST_TEXELS_PER_UNIT 8
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
