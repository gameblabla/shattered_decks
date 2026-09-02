#pragma once

#include "msxgl.h"

#define MSX2_BATTLE_BURN_STEPS 6

void Msx2_BattleFxImpact(u8 x, bool trap);
void Msx2_BattleFxResult(bool trap);
void Msx2_BattleFxBurnCard(u8 x, u8 h, u8 step);
