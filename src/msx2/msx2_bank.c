// ─────────────────────────────────────────────────────────────────────────────
//  msx2_bank.c — the page-0 window, and the trampolines through it
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_bank.h"
#include "msx2_screens.h"
#include "msx2_story.h"
#include "msx2_board.h"
#include "msx2_probe.h"

// NEO-16's page-0 bank register.  Writing a 16-bit segment number to it maps
// that segment at 0x0000; the address itself is ROM, so the write only ever
// reaches the mapper.
#define MSX2_NEO_BANK0_REG  0x5000
#define MSX2_NEO_BANK2_REG  0x7000
#define MSX2_BANK2_CODE     1

// The mapper is write-only, so the current segment is tracked here.  It starts
// at segment 2 because that is what crt0 maps at boot.
static u16 g_bank0 = MSX2_BANK0_DUEL;
// The streamer restores bank 2 in inline assembly while interrupts are still
// disabled; it updates this shadow directly before re-enabling them.
u16 g_bank2 = MSX2_BANK2_CODE;

u16 Msx2_Bank0Enter(u16 segment)
{
	u16 previous = g_bank0;
	if(segment != previous)
	{
		g_bank0 = segment;
		*(u16*)MSX2_NEO_BANK0_REG = segment;
	}
	return previous;
}

void Msx2_Bank0Leave(u16 segment)
{
	if(segment != g_bank0)
	{
		g_bank0 = segment;
		*(u16*)MSX2_NEO_BANK0_REG = segment;
	}
}

u16 Msx2_Bank0Current(void)
{
	return g_bank0;
}

// THE 0x8000 WINDOW IS WRITTEN EVERY TIME, NOT ONLY WHEN THE SHADOW DISAGREES.
// Half of _CODE lives above 0x8000 -- the sprite calls the duel's cursor makes
// every frame are up there -- so the main loop is regularly executing out of
// this window, and the only reason it survives is that whoever borrowed the
// window put the code segment back before interrupts came on again.  The
// register is write-only, so `g_bank2` is a shadow of what was last written,
// and skipping the write when the shadow already matches trusts that shadow
// with the machine: any single divergence -- a write that did not land, a
// borrower that updated one and not the other -- becomes an instruction
// fetched out of a music segment, which is what a crashed Waifu_msx2.oms
// caught (a HALT fetched in the middle of VDP_SetSpriteUniColor).  The write
// costs sixteen T-states and it happens twice a frame, so the guard was never
// worth what it risked.
u16 Msx2_Bank2Enter(u16 segment)
{
	u16 previous = g_bank2;
	g_bank2 = segment;
	*(u16*)MSX2_NEO_BANK2_REG = segment;
	return previous;
}

void Msx2_Bank2Leave(u16 segment)
{
	g_bank2 = segment;
	*(u16*)MSX2_NEO_BANK2_REG = segment;
}

// ── The trampolines ─────────────────────────────────────────────────────────
// One per entry point into a banked screen.  They are the public names:
// nothing outside this file knows the window moved.  Each restores whatever
// bank it displaced rather than assuming one, so a modal screen opened from
// the story lands back in the story and one opened from the duel lands back in
// the duel.

void Msx2_CardCheckCompose(u8 card, i16 atk, i16 def)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_CardCheckCompose_In(card, atk, def);
	Msx2_Bank0Leave(back);
}

void Msx2_FusionBegin(const u8* materials, u8 count, u8 result)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_FusionBegin_In(materials, count, result);
	Msx2_Bank0Leave(back);
}

bool Msx2_FusionStep(void)
{
	bool more;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	more = Msx2_FusionStep_In();
	Msx2_Bank0Leave(back);
	return more;
}

void Msx2_EffectBegin(u8 card, u8 by_com)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_EffectBegin_In(card, by_com);
	Msx2_Bank0Leave(back);
}

bool Msx2_EffectStep(void)
{
	bool more;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	more = Msx2_EffectStep_In();
	Msx2_Bank0Leave(back);
	return more;
}

// ── The duel screen ─────────────────────────────────────────────────────────

u8 Msx2_BoardStageForStory(u8 story_duel_index)
{
	u8 stage;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_DUEL);
	stage = Msx2_BoardStageForStory_In(story_duel_index);
	Msx2_Bank0Leave(back);
	return stage;
}

void Msx2_BoardEnter(u8 stage)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_DUEL);
	Msx2_BoardEnter_In(stage);
	Msx2_Bank0Leave(back);
}

u8 Msx2_BoardStep(void)
{
	u8 what;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_DUEL);
	what = Msx2_BoardStep_In();
	Msx2_Bank0Leave(back);
	return what;
}

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_ProbeRegressionCopy(Msx2Probe* probe)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_ProbeRegressionCopy_In(probe);
	Msx2_Bank0Leave(back);
}

void Msx2_BoardRegressionFixture(u8 fixture)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_DUEL);
	Msx2_BoardRegressionFixture_In(fixture);
	Msx2_Bank0Leave(back);
}

void Msx2_BoardRegressionStamp(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_DUEL);
	Msx2_BoardRegressionStamp_In();
	Msx2_Bank0Leave(back);
}
#endif

// ── The story screens ───────────────────────────────────────────────────────

void Msx2_StoryBegin(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryBegin_In();
	Msx2_Bank0Leave(back);
}

void Msx2_StoryBeginAutoplay(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryBeginAutoplay_In();
	Msx2_Bank0Leave(back);
}

void Msx2_StoryBeginLoad(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryBeginLoad_In();
	Msx2_Bank0Leave(back);
}

void Msx2_StoryPrepareDuelDeck(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryPrepareDuelDeck_In();
	Msx2_Bank0Leave(back);
}

u8 Msx2_StoryDuelIndex(void)
{
	u8 index;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	index = Msx2_StoryDuelIndex_In();
	Msx2_Bank0Leave(back);
	return index;
}

void Msx2_StoryDuelDone(bool won)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryDuelDone_In(won);
	Msx2_Bank0Leave(back);
}

u8 Msx2_StoryStep(void)
{
	u8 what;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	what = Msx2_StoryStep_In();
	Msx2_Bank0Leave(back);
	return what;
}

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_StoryRegressionFixture(u8 fixture)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryRegressionFixture_In(fixture);
	Msx2_Bank0Leave(back);
}

void Msx2_StoryRegressionStamp(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_STORY);
	Msx2_StoryRegressionStamp_In();
	Msx2_Bank0Leave(back);
}
#endif
