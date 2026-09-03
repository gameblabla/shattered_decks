#pragma once

#include "msxgl.h"

#define MSX2_BATTLE_BURN_STEPS 6

// How many poses the blade sweep grows over, and how many the damage readout
// counts up over (the sweep plus the sprite burst that follows it).
#define MSX2_BATTLE_SLASH_STEPS 8
#define MSX2_BATTLE_COUNT_STEPS 16

// One pose of the blade sweep centred on (x, 83).  It is SPRITES: it touches no
// part of the picture under it, so there is nothing to erase and no page to
// level between poses, which is the whole reason an attack is quick.
void Msx2_BattleFxSlash(u8 x, u8 step);

// The running damage readout under the struck lane: -0 climbing to -<damage>.
// Its own black plate, so the digits can shrink again without leaving a tail --
// and it is the one thing in the beat that IS bitmap, so the caller must give
// the same figure to two consecutive frames or the two pages disagree and it
// flickers between two numbers.
void Msx2_BattleFxDamageCount(u8 x, i16 value);

// One frame of the sprite explosion at (x, y).  `step` past the last frame
// takes it off the screen.
void Msx2_BattleFxBurst(u8 x, u8 y, u8 step);
void Msx2_BattleFxResult(bool trap);
void Msx2_BattleFxBurnCard(u8 x, u8 h, u8 step);
