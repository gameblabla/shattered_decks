// ─────────────────────────────────────────────────────────────────────────────
//  Shattered Decks — MSX2 port
//  msx2_main.c — boot and frame loop
//
//  The loop shape is the one the plan specifies: HALT to vblank, take input,
//  advance the game by a bounded amount of work, then present.
//
//  Two scenes exist so far.  TITLE is drawn (msx2_title.c) and driven by the
//  player.  DUEL still has no renderer -- it is milestone 1's blind autoplay,
//  where both sides are played by the AI and the state is stamped into RAM
//  every frame (msx2_probe.c) for tools/msx2/read_probe.py to read back.  It
//  draws only a status line, which is enough to prove the scene changed.
// ─────────────────────────────────────────────────────────────────────────────

#include "msxgl.h"

#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_probe.h"
#include "msx2_audio.h"
#include "msx2_video.h"
#include "msx2_input.h"
#include "msx2_title.h"

// A duel that has not resolved within this many rules steps is not a long duel,
// it is a bug.  The watchdog turns what would be a silent hang into a status
// byte the headless reader can see.
#define MSX2_DUEL_STEP_WATCHDOG   4000

// Rules steps per frame.  One discrete action per frame is what the presented
// game will want (each step is a beat the player watches); the blind build runs
// hotter so a headless run covers whole duels in a few hundred frames.
#ifndef MSX2_STEPS_PER_FRAME
	#define MSX2_STEPS_PER_FRAME  16
#endif

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

static void Msx2_StartDuel(void)
{
	// Cycle the five story opponents, then a free battle, so the scripted decks
	// and the water-field duel are all covered by a single blind run.
	u8 story = (u8)(g_stat_duels % (MSX2_STORY_MAX_DUELS + 1));
	if(story == MSX2_STORY_MAX_DUELS)
		story = MSX2_STORY_NONE;

	Msx2_DuelInit(Msx2_NextSeed(), story);
	Msx2_MusicPlay((story == MSX2_STORY_FINAL_DUEL) ? MSX2_MUSIC_FINAL_BOSS : MSX2_MUSIC_BATTLE);
	g_stat_steps = 0;
}

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

// The duel screen is milestone 2 work.  Until it exists, the scene paints a
// black page and a single line of state, repainted a few times a second: enough
// that a screenshot shows the game left the title and is playing, and cheap
// enough that it cannot be what makes a frame late.
static void Msx2_DuelEnter(void)
{
	// Both pages, so the status line below can repaint one of them without the
	// other being blank underneath it.
	Msx2_VideoDrawPage(MSX2_PAGE_0);
	Msx2_ClearPage(MSX2_BLACK);
	Msx2_TextColor(MSX2_GOLD, MSX2_BLACK);
	Msx2_TextCenter(90, "DUEL IN PROGRESS");
	Msx2_VideoCopyPage(MSX2_PAGE_0, MSX2_PAGE_1);
	Msx2_VideoShowPage(MSX2_PAGE_0);
}

// Erase-then-redraw on the visible page is exactly what flickers, so the status
// line is written on the hidden page and shown by a flip.  It is redrawn from
// the live state every time, so the page that comes up is always the fresh one.
static void Msx2_DuelDrawStatus(void)
{
	Msx2_Fill(0, 106, MSX2_SCREEN_W, 8, MSX2_BLACK);
	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);
	Msx2_TextAt(72, 106, "LP");
	Msx2_NumAt(90, 106, g_duel.side[MSX2_OWNER_PLAYER].lp);
	Msx2_TextAt(140, 106, "COM");
	Msx2_NumAt(166, 106, g_duel.side[MSX2_OWNER_COM].lp);
	Msx2_VideoFlipRequest();
}

static void Msx2_SceneTitle(void)
{
	u8 choice = Msx2_TitleStep();
	g_stat_menu_cursor = Msx2_TitleCursor();
	if(choice == MSX2_TITLE_BUSY)
		return;

	// STORY MODE has no name entry and no story flow on this target yet, so
	// both playable rows enter the same blind duel; they diverge the moment
	// the story scenes land, and the seam is already here.
	Msx2_StartDuel();
	MSX2_STAGE(MSX2_STAGE_DUEL);
	Msx2_DuelEnter();
	g_stat_scene = MSX2_SCENE_DUEL;
	g_stat_menu_cursor = 0xFF;
}

void main(void)
{
	u8 i;

	g_seed = 0x1234ABCDu;
	Msx2_AudioInit();
	Msx2_ProbeInit();
	Msx2_VideoInit();
	Msx2_InputInit();
	MSX2_STAGE(MSX2_STAGE_BOOT);

	Msx2_TitleEnter();
	g_stat_scene = MSX2_SCENE_TITLE;
	MSX2_STAGE(MSX2_STAGE_TITLE);

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

		if(g_stat_status == MSX2_PROBE_OK)
		{
			for(i = 0; i < MSX2_STEPS_PER_FRAME; ++i)
			{
				if(g_duel.result != 0)
					break;
				Msx2_DuelStep();
				++g_stat_steps;

				if(!Msx2_StateIsSane())
				{
					g_stat_status = MSX2_PROBE_BADSTATE;
					break;
				}
				if(g_stat_steps > MSX2_DUEL_STEP_WATCHDOG)
				{
					g_stat_status = MSX2_PROBE_STUCK;
					break;
				}
			}

			if((g_stat_status == MSX2_PROBE_OK) && (g_duel.result != 0))
			{
				if(g_duel.result > 0)
					++g_stat_wins_player;
				else
					++g_stat_wins_com;
				++g_stat_duels;
				Msx2_StartDuel();
			}

			if((g_stat_frame & 7) == 0)
				Msx2_DuelDrawStatus();
		}

		Msx2_ProbeUpdate();
	}
}
