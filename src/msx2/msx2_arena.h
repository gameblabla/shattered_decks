// MSX2 arena: compact visible spans, runtime sandstone sampling, flat walls.
// Geometry/card quads retain the shared renderer's authored poses.
#pragma once

#include "msxgl.h"

// Read one pose's mesh and card quads out of the cartridge.  Poses are named
// by the MSX2_MESH_POSE_* macros in the generated header.
void Msx2_ArenaPose(u8 pose);

// Which pose is loaded, so a caller can avoid re-reading the resting one.
u8 Msx2_ArenaCurrentPose(void);

// THE FIRST SCREEN ROW THE BOARD IS ACTUALLY PAINTED ON.
//
// A camera-move frame draws one row in every two or three and has the command
// engine repeat it down the group (see msx2_floor.c), so the group that
// straddles the top of the arena leads on a row the board does not reach yet
// and comes out empty: at MOVE_TURN_2 the board's own top row is band 10 but
// nothing is painted above band 12.  A card is mapped row by row and does not
// skip, so a card in a far row would stick out above the board it stands on --
// and worse, the erase bookkeeping (g_ext_*) only records the rows the FLOOR
// covered, so those rows are never blacked again and the card's top edge stays
// on the page for the rest of the orbit.  That is the green line off the
// board's rim beside an equip.
//
// The rasterizer clips to this rather than the arena's true top, so every
// pixel a card puts down is inside a row the silhouette knows about.  A
// resting pose draws every row, so there it is the board's top row and clips
// nothing.
extern u8 g_msx2_arena_top;

// The projected texture corners belonging to the currently loaded pose.
// `defense` selects the inscribed quarter-turned footprint.
const u8* Msx2_ArenaCardQuad(u8 slot, u8 defense);

// Paint the whole board band on the draw page: the two camera-facing slab
// walls, the twenty floor tiles, and black everywhere the arena is not.
void Msx2_ArenaDraw(void);

// The same band, drawn as one step of a camera move: black is repainted only
// where this page's previous pose had something else, which is most of what
// makes a moving frame affordable.  Msx2_ArenaMoveStart must open the move.
void Msx2_ArenaDrawStep(void);
void Msx2_ArenaMoveStart(void);

// The same board, but only inside one rectangle -- what an emptied slot needs.
void Msx2_ArenaDrawBox(u8 x, u8 y, u8 w, u8 h);

// The overhead table: a flat five-by-four grid, drawn straight rather than
// projected, for the view the player walks up into.
void Msx2_ArenaDrawOver(void);
void Msx2_ArenaDrawOverBox(u8 x, u8 y, u8 w, u8 h);
