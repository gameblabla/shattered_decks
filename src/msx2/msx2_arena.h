// ─────────────────────────────────────────────────────────────────────────────
//  msx2_arena.h — the duel board, drawn rather than streamed
//
//  The cartridge used to carry a 54,272-byte picture of this arena for every
//  camera pose: four stages x three views, plus sixteen opening poses and five
//  turn poses per stage.  3.9 MB, and every one of them arrived through the
//  Z80's own `outi` loop at 32 T-states a byte -- a quarter of a second for one
//  band.
//
//  What it carries now is the same arena's PROJECTED MESH, 157 bytes a pose
//  (`gen_msx_views.py`, `MSX2_MESH_SEGMENT`), and the board is filled by the
//  V9938's command engine one HMMV per scanline.  Same corners, same
//  perspective, same checker: the mesh comes out of `render_board()`'s own
//  projection, so MSX2_PORT_PLAN.md §4.3's rule that the MSX2 board IS the
//  shared board survives the change intact -- the pixels stopped being baked,
//  the geometry did not.
//
//  A pose is about a tenth of a second to draw against the quarter-second the
//  stream cost, which is why the camera moves are smoother now and not merely
//  smaller.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Read one pose's mesh and card quads out of the cartridge.  Poses are named
// by the MSX2_MESH_POSE_* macros in the generated header.
void Msx2_ArenaPose(u8 pose);

// Which pose is loaded, so a caller can avoid re-reading the resting one.
u8 Msx2_ArenaCurrentPose(void);

// The projected texture corners belonging to the currently loaded pose.
// `defense` selects the inscribed quarter-turned footprint.
const u8* Msx2_ArenaCardQuad(u8 slot, u8 defense);

// Paint the whole board band on the draw page: the two camera-facing slab
// walls, the twenty floor tiles, and black everywhere the arena is not.
void Msx2_ArenaDraw(void);

// The same board, but only inside one rectangle -- what an emptied slot needs.
void Msx2_ArenaDrawBox(u8 x, u8 y, u8 w, u8 h);

// The overhead table: a flat five-by-four grid, drawn straight rather than
// projected, for the view the player walks up into.
void Msx2_ArenaDrawOver(void);
void Msx2_ArenaDrawOverBox(u8 x, u8 y, u8 w, u8 h);
