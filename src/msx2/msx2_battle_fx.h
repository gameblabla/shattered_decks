#pragma once

#include "msxgl.h"

#define MSX2_BATTLE_BURN_STEPS 6

void Msx2_BattleFxImpact(u8 x, bool trap);

// One frame of the sprite explosion at (x, y).  `step` past the last frame
// takes it off the screen.
void Msx2_BattleFxBurst(u8 x, u8 y, u8 step);
void Msx2_BattleFxResult(bool trap);
void Msx2_BattleFxBurnCard(u8 x, u8 h, u8 step);
