// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe_bank.c — regression-only diagnostic state owner
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_probe.h"
#include "msx2_bank.h"
#include "msx2_audio.h"
#include "msx2_entropy.h"

#ifdef MSX2_DEBUG_REGRESSION

Msx2RegressionDiag g_msx2_regression_diag;

void Msx2_ProbeRegressionCopy_In(Msx2Probe* probe)
{
	u8* dst;
	u8* src;
	u8 i;

	if(g_stat_scene == MSX2_SCENE_DUEL)
		Msx2_BoardRegressionStamp();
	else if(g_stat_scene == MSX2_SCENE_STORY)
		Msx2_StoryRegressionStamp();
	Msx2_AudioRegressionStamp();
	g_msx2_regression_diag.initial_seed = g_msx2_initial_seed;
	g_msx2_regression_diag.duel_seed = g_msx2_duel_seed;
	g_msx2_regression_diag.entropy_sources = g_msx2_entropy_flags;

	dst = (u8*)&probe->board_mode;
	src = (u8*)&g_msx2_regression_diag;
	for(i = 0; i < sizeof(Msx2RegressionDiag); ++i)
		dst[i] = src[i];
}

#endif
