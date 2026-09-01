// ─────────────────────────────────────────────────────────────────────────────
//  Shattered Decks — MSX2 port
//  msx2_main.c — boot and frame loop
//
//  The loop shape is the one the plan specifies: HALT to vblank, take input,
//  advance the game by a bounded amount of work, then present.
//
//  Three scenes exist: TITLE (msx2_title.c), STORY (msx2_story.c) and DUEL
//  (msx2_board.c), all driven by the player through the same latched input.
//  Story mode owns the run between duels: it asks for one and is handed the
//  result back, which is what keeps the board itself ignorant of the story.
//
//  Building with -DMSX2_DEBUG_AUTOPLAY hands the player's turn to the same AI
//  the COM uses, which is how the blind soak still works on a build that draws
//  a board: the ROM plays itself for hundreds of duels and stamps its state
//  into RAM every frame (msx2_probe.c) for tools/msx2/read_probe.py.
// ─────────────────────────────────────────────────────────────────────────────

#include "msxgl.h"

#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_probe.h"
#include "msx2_audio.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_title.h"
#include "msx2_board.h"
#include "msx2_story.h"

// A duel the SOAK has not resolved within this many frames is not a long duel,
// it is a bug, and the watchdog turns what would be a silent hang into a status
// byte the headless reader can see.  It applies to the soak only: a person is
// perfectly entitled to spend two minutes on one turn.
#define MSX2_DUEL_FRAME_WATCHDOG  4000

static u32 g_seed;

// Frames counted by the vblank ISR.  MSXgl's crt0 installs its own handler in
// page 0 for a mapped ROM, which means the BIOS interrupt routine -- and its
// JIFFY counter -- is not running: this is the port's only clock.
volatile u16 g_msx2_ticks;

// The vblank ISR.  crt0_rom_neo.asm calls this on every V-blank once
// CustomISR = "VBLANK" is set (it is, in project_config.js).  Keep it short:
// per the port plan it does music, SFX and input latching only, and every byte
// of VRAM work stays in the main loop so VDP register state is never touched
// from two contexts.
void VDP_InterruptHandler(void)
{
	++g_msx2_ticks;
	Msx2_AudioTick();
}

// xorshift on the frame counter: the blind run must play *different* duels, or
// it only ever exercises one path through the rules.
static u32 Msx2_NextSeed(void)
{
	g_seed ^= g_seed << 13;
	g_seed ^= g_seed >> 17;
	g_seed ^= g_seed << 5;
	return g_seed;
}

// Deal a duel against `story` (MSX2_STORY_NONE for a free battle) and compose
// the board for it.
static void Msx2_DealDuel(u8 story)
{
	Msx2_DuelInit(Msx2_NextSeed(), story);
	Msx2_MusicPlay((story == MSX2_STORY_FINAL_DUEL) ? MSX2_MUSIC_FINAL_BOSS : MSX2_MUSIC_BATTLE);
	g_stat_steps = 0;
	MSX2_STAGE(MSX2_STAGE_DUEL);
	Msx2_BoardEnter(Msx2_BoardStageForStory(story));
	g_stat_scene = MSX2_SCENE_DUEL;
	g_stat_menu_cursor = 0xFF;
}

// The soak's duel: cycle the five story opponents, then a free battle, so the
// scripted decks and the water-field duel are all covered by a single blind run.
static void Msx2_StartDuel(void)
{
	u8 story = (u8)(g_stat_duels % (MSX2_STORY_MAX_DUELS + 1));
	if(story == MSX2_STORY_MAX_DUELS)
		story = MSX2_STORY_NONE;
	Msx2_DealDuel(story);
}

// Whether the duel on screen belongs to a story run, and therefore whether its
// result goes back to the story scene or straight to the title.
static bool g_in_story;

// Cheap invariant check.  A duel state that has gone out of range is worth
// catching in the blind run, where nothing is drawn and nothing else would
// notice.
static bool Msx2_StateIsSane(void)
{
	u8 owner, i;
	for(owner = 0; owner < 2; ++owner)
	{
		if((g_duel.side[owner].lp < 0) || (g_duel.side[owner].lp > 30000))
			return FALSE;
		for(i = 0; i < MSX2_FIELD; ++i)
		{
			u8 card = g_duel.side[owner].field[i];
			if((card != MSX2_CARD_NONE) && (card >= MSX2_TOTAL_CARDS))
				return FALSE;
		}
		for(i = 0; i < MSX2_HAND; ++i)
		{
			u8 card = g_duel.side[owner].hand[i];
			if((card != MSX2_CARD_NONE) && (card >= MSX2_TOTAL_CARDS))
				return FALSE;
		}
	}
	return TRUE;
}

static void Msx2_SceneTitle(void)
{
	u8 choice = Msx2_TitleStep();
	g_stat_menu_cursor = Msx2_TitleCursor();
	if(choice == MSX2_TITLE_BUSY)
		return;

	if(choice == MSX2_TITLE_STORY)
	{
		g_in_story = TRUE;
		Msx2_StoryBegin();
		g_stat_scene = MSX2_SCENE_STORY;
		g_stat_menu_cursor = 0xFF;
		return;
	}

	g_in_story = FALSE;
	Msx2_DealDuel(MSX2_STORY_NONE);
}

static void Msx2_SceneStory(void)
{
	u8 want = Msx2_StoryStep();

	if(want == MSX2_STORY_FIGHT)
	{
		Msx2_DealDuel(Msx2_StoryDuelIndex());
	}
	else if(want == MSX2_STORY_QUIT)
	{
		g_in_story = FALSE;
		Msx2_TitleEnter();
		g_stat_scene = MSX2_SCENE_TITLE;
		MSX2_STAGE(MSX2_STAGE_TITLE);
	}
}

void main(void)
{
	g_seed = 0x1234ABCDu;
	Msx2_AudioInit();
	Msx2_ProbeInit();
	Msx2_VideoInit();
	Msx2_InputInit();
	MSX2_STAGE(MSX2_STAGE_BOOT);

#ifdef MSX2_DEBUG_AUTOPLAY
	// The soak has no hands, so it does not walk the menu either: it deals
	// straight into a duel and plays both sides.
	Msx2_StartDuel();
#else
	Msx2_TitleEnter();
	g_stat_scene = MSX2_SCENE_TITLE;
	MSX2_STAGE(MSX2_STAGE_TITLE);
#endif

	MSX2_STAGE(MSX2_STAGE_LOOP);
	for(;;)
	{
		Halt();                      // sync to vblank
		Msx2_VideoPresent();         // ... and swap pages inside the blanking
		++g_stat_frame;
		Msx2_InputUpdate();

		if(g_stat_scene == MSX2_SCENE_TITLE)
		{
			Msx2_SceneTitle();
			Msx2_ProbeUpdate();
			continue;
		}

		if(g_stat_scene == MSX2_SCENE_STORY)
		{
			Msx2_SceneStory();
			Msx2_ProbeUpdate();
			continue;
		}

		if(g_stat_status == MSX2_PROBE_OK)
		{
			u8 outcome = Msx2_BoardStep();
			++g_stat_steps;

			if(!Msx2_StateIsSane())
				g_stat_status = MSX2_PROBE_BADSTATE;
#ifdef MSX2_DEBUG_AUTOPLAY
			else if(g_stat_steps > MSX2_DUEL_FRAME_WATCHDOG)
				g_stat_status = MSX2_PROBE_STUCK;
#endif
			else if(outcome != MSX2_BOARD_BUSY)
			{
				if(outcome == MSX2_BOARD_WIN)
					++g_stat_wins_player;
				else
					++g_stat_wins_com;
				++g_stat_duels;
#ifdef MSX2_DEBUG_AUTOPLAY
				Msx2_StartDuel();
#else
				if(g_in_story)
				{
					Msx2_StoryDuelDone(outcome == MSX2_BOARD_WIN);
					g_stat_scene = MSX2_SCENE_STORY;
					MSX2_STAGE(MSX2_STAGE_STORY);
				}
				else
				{
					// A free battle answers to nothing, so it goes back where
					// it was dealt from.
					Msx2_TitleEnter();
					g_stat_scene = MSX2_SCENE_TITLE;
					MSX2_STAGE(MSX2_STAGE_TITLE);
				}
#endif
			}
		}

		Msx2_ProbeUpdate();
	}
}
