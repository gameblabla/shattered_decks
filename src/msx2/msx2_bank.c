// ─────────────────────────────────────────────────────────────────────────────
//  msx2_bank.c — the page-0 window, and the trampolines through it
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_bank.h"
#include "msx2_screens.h"
#include "msx2_story.h"
#include "msx2_board.h"
#include "msx2_probe.h"
#include "msx2_video.h"
#include "msx2_title.h"
#include "msx2_audio.h"
#include "msx2_battle_fx.h"

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

// The boot bank, through the 0x8000 window.  This trampoline is in _CODE and
// so is every byte of the return path, which is the whole requirement: the
// probe itself, and nothing else, runs with segment 5 in the window.
//
// INTERRUPTS ARE OFF FOR THE WHOLE CALL, and that is not belt-and-braces: the
// V-blank handler runs the audio tick, which reads the resident lVGM decoder
// out of _CODE -- and half of _CODE is above 0x8000, which is exactly what
// segment 5 is standing in.  The slot scan inside takes many milliseconds, so
// leaving them on is not a race that might happen but one that does, every
// boot.  Msx2_AudioSetup_In() therefore never turns them back on either.
u8 Msx2_AudioSetup(struct Msx2MusicAsset* table, Msx2SfxStep* steps, u8* first)
{
	u8 chip;
	u16 back;
	__asm di __endasm;
	back = Msx2_Bank2Enter(MSX2_BANK2_BOOT);
	chip = Msx2_AudioSetup_In(table, steps, first);
	Msx2_Bank2Leave(back);
	__asm ei __endasm;
	return chip;
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

void Msx2_VideoBakeFont(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_VideoBakeFont_In();
	Msx2_Bank0Leave(back);
}

void Msx2_CardCheckCompose(u8 card, i16 atk, i16 def)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_CardCheckCompose_In(card, atk, def);
	Msx2_Bank0Leave(back);
}

void Msx2_FusionBegin(const u8* materials, u8 count, u8 result, u8 fused)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_FusionBegin_In(materials, count, result, fused);
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

// ── The R800 ────────────────────────────────────────────────────────────────
//
// A turbo R boots as a Z80 and stays one until CHGCPU (0x0180 of the main ROM)
// is asked for the other processor: A holds the mode -- 1 is the R800 with
// external memory still read as ROM, which is what a cartridge game wants --
// and bit 7 lights the machine's own front-panel LED.
//
// IT PUTS THE INTERRUPT VECTOR BACK, AND THAT IS THE WHOLE TRICK.
// CHGCPU reinitialises the machine's interrupt system on its way through, and
// this port does not use the machine's: crt0 copies its ISR into RAM at 0xC101
// and runs interrupt mode 2 off a vector table at 0xC000 (INSTALL_RAM_ISR in
// MSXgl's macros.asm), because the ROM's own 0x0038 is this cartridge's duel
// bank.  Coming back in mode 1 therefore sent the very next V-blank into
// whatever byte the duel screen happens to have at 0x0038 -- the machine died
// inside the first cartridge read after the switch, executing at 0x00CC with a
// stack that was not ours.  Reinstating I and IM 2 here is what makes the R800
// usable at all; it cost a day to find, so it is written down.
//
// IT IS CALLED FROM _CODE, NOT FROM THE BOOT BANK BESIDE THE CHIP PROBE.
// Everything in msx2_audio_probe.c runs with segment 5 in the 0x8000 window,
// standing where the resident lVGM decoder the V-blank handler calls normally
// is, and CHGCPU turns interrupts back on.  Here the window holds the code
// segment throughout.
//
// What the faster processor is spent on: the duel's camera moves draw every
// line of the board instead of every second or third one, and walk twice as
// many poses of the turn (msx2_arena.c, msx2_board.c, both asking
// MSX2_TURBO_R()).  Measured against the same soak on the same machine, the
// R800 build was three hundred frames further into its second duel at the 90 s
// mark WITH both of those on.
u8 g_msx2_r800;

void Msx2_CpuFast(void)
{
	if(g_msx2_msxver < 3)
		return;
	__asm
		di
		in		a, (#0xA8)
		ld		b, a
		and		a, #0xFC
		ld		hl, #_g_EXPTBL
		or		a, (hl)
		and		a, #0x03
		ld		c, a
		ld		a, b
		and		a, #0xFC
		or		a, c
		push	bc
		out		(#0xA8), a				// the BIOS, over segment 2
		ld		a, #0x81				// R800/ROM, and light the LED
		call	#0x0180					// CHGCPU
		di
		ld		a, #0xC0				// the RAM vector table crt0 installed
		ld		i, a
		im		2
		pop		bc
		ld		a, b
		out		(#0xA8), a				// segment 2 back
		ei
	__endasm;
	g_msx2_r800 = 1;
}

// The attack cut-in, which moved into the modal bank when segment 2 filled up.
// One trampoline for all five effects (msx2_battle_fx.h says why): every one of
// these is called from the duel bank, several of them every frame of a strike,
// and a bank write is sixteen T-states.
void Msx2_BattleFx(u8 op, u8 x, u8 y, i16 value)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_BattleFx_In(op, x, y, value);
	Msx2_Bank0Leave(back);
}

// ── The duel screen ─────────────────────────────────────────────────────────

void Msx2_TitleEnter(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_TitleEnter_In();
	Msx2_Bank0Leave(back);
}

u8 Msx2_TitleStep(void)
{
	u8 choice;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	choice = Msx2_TitleStep_In();
	Msx2_Bank0Leave(back);
	return choice;
}

u8 Msx2_TitleCursor(void)
{
	u8 cursor;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	cursor = Msx2_TitleCursor_In();
	Msx2_Bank0Leave(back);
	return cursor;
}

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

// The probe stamps.  Both bodies live in the modal bank; these are the only
// way in.  Not regression-only: the shipping ROM stamps the probe too.
void Msx2_ProbeInit(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_ProbeInit_In();
	Msx2_Bank0Leave(back);
}

void Msx2_ProbeUpdate(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_ProbeUpdate_In();
	Msx2_Bank0Leave(back);
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
