// ─────────────────────────────────────────────────────────────────────────────
//  msx2_regression.h — deterministic final-issue fixture hooks
//
//  The production ROM has no synthetic state.  Regression builds use this
//  module only to make a named starting contract visible in the probe; the
//  board, story, input, and audio paths remain the production paths.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

enum Msx2RegressionFixture
{
	MSX2_FIXTURE_NONE = 0,
	MSX2_FIXTURE_COM_TURN4_HAND,
	MSX2_FIXTURE_PLAYER_TOP_PLACE,
	MSX2_FIXTURE_TOP_PASS_TURN,
	MSX2_FIXTURE_STORY_SAVE_ROW,
	MSX2_FIXTURE_DUEL_RESULT_WIN,
	MSX2_FIXTURE_DUEL_RESULT_LOSE,
};

void Msx2_RegressionInit(void);
u8   Msx2_RegressionFixture(void);

#define MSX2_REGRESSION_START_NONE  0
#define MSX2_REGRESSION_START_DUEL  1
#define MSX2_REGRESSION_START_STORY 2
u8 Msx2_RegressionStart(void);
