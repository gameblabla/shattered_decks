#pragma once

#include "msxgl.h"

#define MSX2_BATTLE_BURN_STEPS 6

// How many poses the blade sweep grows over, and how many the damage readout
// counts up over (the sweep plus the sprite burst that follows it).
#define MSX2_BATTLE_SLASH_STEPS 8
#define MSX2_BATTLE_COUNT_STEPS 16

void Msx2_BattleFxImpact(u8 x, bool trap);

// One pose of the blade sweep centred on (x, 83).  It only ever GROWS, so a
// page that has been levelled from the other one can simply have the next pose
// drawn into it -- no part of it is ever erased.
void Msx2_BattleFxSlash(u8 x, u8 step);

// The running damage readout under the struck lane: -0 climbing to -<damage>.
// Its own black plate, so the digits can shrink again without leaving a tail.
void Msx2_BattleFxDamageCount(u8 x, i16 value);

// One frame of the sprite explosion at (x, y).  `step` past the last frame
// takes it off the screen.
void Msx2_BattleFxBurst(u8 x, u8 y, u8 step);
void Msx2_BattleFxResult(bool trap);
void Msx2_BattleFxBurnCard(u8 x, u8 h, u8 step);
