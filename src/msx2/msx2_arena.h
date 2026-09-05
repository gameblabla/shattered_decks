// MSX2 arena: compact visible spans, runtime sandstone sampling, flat walls.
// Geometry/card quads retain the shared renderer's authored poses.
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
