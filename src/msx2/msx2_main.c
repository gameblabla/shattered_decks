// ─────────────────────────────────────────────────────────────────────────────
//  Shattered Decks — MSX2 port
//  msx2_main.c — boot and frame loop
//
//  Milestone 1 of MSX2_PORT_PLAN.md: prove the rules fit in RAM on a Z80 and
//  that a duel can be played through to a result completely blind.  There is no
//  renderer here yet; the game state is stamped into RAM every frame
//  (msx2_probe.c) and read back out of an `openmsx snap` dump.
//
//  The loop shape is already the one the plan specifies: HALT to vblank, take
//  input, advance the game by a bounded amount of work, then present.  Only the
//  present step is missing.
// ─────────────────────────────────────────────────────────────────────────────

#include "msxgl.h"

#include "msx2_duel.h"
#include "msx2_cards.h"
#include "msx2_probe.h"
#include "msx2_audio.h"

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

void main(void)
{
	u8 i;

	VDP_SetColor(COLOR_BLACK);
	VDP_EnableVBlank(TRUE);

	g_seed = 0x1234ABCDu;
	Msx2_AudioInit();
	Msx2_ProbeInit();
	MSX2_STAGE(MSX2_STAGE_BOOT);
	Msx2_StartDuel();

	MSX2_STAGE(MSX2_STAGE_LOOP);
	for(;;)
	{
		Halt();                      // sync to vblank
		++g_stat_frame;

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
		}

		Msx2_ProbeUpdate();
	}
}
