// ─────────────────────────────────────────────────────────────────────────────
//  msx2_regression.c — fixed-seed/fixture plumbing for transient regressions
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_regression.h"

#ifdef MSX2_DEBUG_REGRESSION

#include "msx2_bank.h"
#include "msx2_board.h"
#include "msx2_duel.h"
#include "msx2_story.h"
#include "msx2_entropy.h"
#include "msx2_audio.h"

#ifndef MSX2_TEST_FIXTURE
#define MSX2_TEST_FIXTURE MSX2_FIXTURE_NONE
#endif

static u8 g_fixture;

void Msx2_RegressionInit(void)
{
	g_fixture = MSX2_TEST_FIXTURE;
}

u8 Msx2_RegressionFixture(void)
{
	return g_fixture;
}

u8 Msx2_RegressionStart(void)
{
	if(g_fixture == MSX2_FIXTURE_STORY_SAVE_ROW)
	{
		Msx2_StoryBegin();
		Msx2_StoryRegressionFixture(g_fixture);
		return MSX2_REGRESSION_START_STORY;
	}
	if(g_fixture == MSX2_FIXTURE_NONE)
		return MSX2_REGRESSION_START_NONE;

	Msx2_DuelSetPlayerDeck(NULL, 0);
	Msx2_DuelInit(Msx2_EntropyNextSeed(), MSX2_STORY_NONE);
	Msx2_MusicPlay(MSX2_MUSIC_BATTLE);

	if(g_fixture == MSX2_FIXTURE_COM_TURN4_HAND)
	{
		// Construct the observed sparse hand, then run the real turn-start draw
		// before the board takes its presentation snapshot.
		g_duel.side[MSX2_OWNER_COM].hand[0] = MSX2_CARD_NONE;
		g_duel.side[MSX2_OWNER_COM].used[0] = FALSE;
		g_duel.turn_owner = MSX2_OWNER_COM;
		g_duel.turns = 4;
		g_duel.phase = MSX2_PHASE_TURN_START;
		Msx2_DuelStep();
	}
	else
	{
		// A known monster makes the placement and pass fixtures independent of
		// whichever random opening hand the fixed seed happens to produce.
		g_duel.side[MSX2_OWNER_PLAYER].hand[0] = 0;
		g_duel.side[MSX2_OWNER_PLAYER].used[0] = FALSE;
		g_duel.turn_owner = MSX2_OWNER_PLAYER;
		g_duel.turns = 2;
		g_duel.phase = MSX2_PHASE_MAIN;
		g_duel.side[MSX2_OWNER_PLAYER].monster_played = FALSE;
		if(g_fixture == MSX2_FIXTURE_TOP_PASS_TURN)
		{
			// Keep both orientations and both owners on the table while the
			// fixture drives the real player-to-COM camera orbit.  This makes a
			// missing pose-card redraw observable in every captured orbit frame.
			g_duel.side[MSX2_OWNER_COM].field[0] = 1;
			g_duel.side[MSX2_OWNER_COM].faceup[0] = FALSE;
			g_duel.side[MSX2_OWNER_COM].defense[0] = FALSE;
			g_duel.side[MSX2_OWNER_PLAYER].field[0] = 0;
			g_duel.side[MSX2_OWNER_PLAYER].faceup[0] = TRUE;
			g_duel.side[MSX2_OWNER_PLAYER].defense[0] = FALSE;
			g_duel.side[MSX2_OWNER_PLAYER].field[1] = 2;
			g_duel.side[MSX2_OWNER_PLAYER].faceup[1] = TRUE;
			g_duel.side[MSX2_OWNER_PLAYER].defense[1] = TRUE;
		}
	}

	// The board entry is the shipping composition/camera path.  Only after it
	// has established its private presentation state do the fixture hooks enter
	// their named transient; no fixture draws a replacement implementation.
	Msx2_BoardEnter(Msx2_BoardStageForStory(MSX2_STORY_NONE));
	Msx2_BoardRegressionFixture(g_fixture);
	return MSX2_REGRESSION_START_DUEL;
}

#else

void Msx2_RegressionInit(void) {}
u8 Msx2_RegressionFixture(void) { return MSX2_FIXTURE_NONE; }
u8 Msx2_RegressionStart(void) { return MSX2_REGRESSION_START_NONE; }

#endif
