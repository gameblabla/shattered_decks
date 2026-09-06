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
#include "msx2_duel_bank.h"
#include "msx2_cards.h"

#ifdef MSX2_ASCII16X
#include "msx2_scenes.h"
#include "../generated/msx2_scene_geometry.h"
#endif

#ifndef MSX2_ASCII16X
// NEO-16's page-0 bank register.  Writing a 16-bit segment number to it maps
// that segment at 0x0000; the address itself is ROM, so the write only ever
// reaches the mapper.
#define MSX2_NEO_BANK0_REG  0x5000
#define MSX2_NEO_BANK2_REG  0x7000
#endif
#define MSX2_BANK2_CODE     1

// The mapper is write-only, so the current segment is tracked here.  It starts
// at segment 2 because that is what crt0 maps at boot.
#ifndef MSX2_ASCII16X
static u16 g_bank0 = MSX2_BANK0_DUEL;
#endif
// The streamer restores bank 2 in inline assembly while interrupts are still
// disabled; it updates this shadow directly before re-enabling them.
u16 g_bank2 = MSX2_BANK2_CODE;

#ifdef MSX2_ASCII16X

static u16 g_bank0 = MSX2_MAPPER_CODE_SEGMENT;

u16 Msx2_Bank0Enter(u16 segment)
{
	u16 previous = g_bank0;
	if(segment > MSX2_MAPPER_MAX_SEGMENT)
		return previous;
	__asm di __endasm;
	g_bank0 = segment;
	g_bank2 = segment;
	Msx2_MapperSetPage2(segment);
	__asm ei __endasm;
	return previous;
}

void Msx2_Bank0Leave(u16 segment)
{
	if(segment > MSX2_MAPPER_MAX_SEGMENT)
		return;
	__asm di __endasm;
	g_bank0 = segment;
	g_bank2 = segment;
	Msx2_MapperSetPage2(segment);
	__asm ei __endasm;
}

u16 Msx2_Bank0Current(void)
{
	return g_bank0;
}

#else

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

#endif

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
	#ifdef MSX2_ASCII16X
	if(segment > MSX2_MAPPER_MAX_SEGMENT)
		return previous;
	#endif
	g_bank2 = segment;
	#ifdef MSX2_ASCII16X
	Msx2_MapperSetPage2(segment);
	#else
	*(u16*)MSX2_NEO_BANK2_REG = segment;
	#endif
	return previous;
}

void Msx2_Bank2Leave(u16 segment)
{
	#ifdef MSX2_ASCII16X
	if(segment > MSX2_MAPPER_MAX_SEGMENT)
		return;
	#endif
	g_bank2 = segment;
	#ifdef MSX2_ASCII16X
	Msx2_MapperSetPage2(segment);
	#else
	*(u16*)MSX2_NEO_BANK2_REG = segment;
	#endif
}

#ifdef MSX2_ASCII16X

// The ASCII16-X rules image is a separate page-2 bank.  These wrappers map it
// for one call and restore the scene that called them.  No scene code retains
// a pointer into this image while page 2 is borrowed.
#define MSX2_DUEL_BACK() Msx2_Bank2Enter(MSX2_MAPPER_RULES_SEGMENT)
#define MSX2_DUEL_RESTORE(back) Msx2_Bank2Leave(back)

// The story picker and probe run in other page-2 banks but use these three
// deck operations.  Their implementations stay with the rules bank so the
// rules path has no cross-bank calls; these small entries preserve the same
// caller-bank contract for the two cold callers.
void waifu_deck_rng_seed(WaifuDeckRng* rng, u32 seed)
{
	u16 back = MSX2_DUEL_BACK();
	waifu_deck_rng_seed_In(rng, seed);
	MSX2_DUEL_RESTORE(back);
}

int waifu_deck_remaining(const WaifuDeck* deck)
{
	int value;
	u16 back = MSX2_DUEL_BACK();
	value = waifu_deck_remaining_In(deck);
	MSX2_DUEL_RESTORE(back);
	return value;
}

void waifu_deck_build_random(WaifuDeck* deck, WaifuDeckRng* rng,
                             int strength_bias)
{
	u16 back = MSX2_DUEL_BACK();
	waifu_deck_build_random_In(deck, rng, strength_bias);
	MSX2_DUEL_RESTORE(back);
}

// These queries touch only the RAM duel state and card-id ranges.  Keeping
// them resident avoids a mapper turn for every cursor test in the board scene;
// queries that read the card tables or execute rules remain trampolines below.
void Msx2_ClearActionEvent(void)
{
	g_duel.last_action = MSX2_ACTION_NONE;
}

bool Msx2_IsMonster(u8 card)
{
	return card < MSX2_CARD_COUNT;
}

bool Msx2_IsSupport(u8 card)
{
	return (card >= MSX2_CARD_COUNT) && (card < MSX2_TOTAL_CARDS);
}

u8 Msx2_SupportKind(u8 card)
{
	u8 kind;
	if(card < MSX2_CARD_COUNT)
		return MSX2_SUP_EQUIP;
	kind = (u8)(card - MSX2_CARD_COUNT);
	while(kind >= MSX2_SUPPORT_VARIANTS)
		kind = (u8)(kind - MSX2_SUPPORT_VARIANTS);
	return kind;
}

u16 Msx2_CardAtk(u8 card)
{
	u16 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_CardAtk_In(card);
	MSX2_DUEL_RESTORE(back);
	return value;
}

u16 Msx2_CardDef(u8 card)
{
	u16 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_CardDef_In(card);
	MSX2_DUEL_RESTORE(back);
	return value;
}

u8 Msx2_FusionResult(u8 a, u8 b)
{
	u8 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FusionResult_In(a, b);
	MSX2_DUEL_RESTORE(back);
	return value;
}

u8 Msx2_FusionChainResult(const u8* cards, u8 count)
{
	u8 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FusionChainResult_In(cards, count);
	MSX2_DUEL_RESTORE(back);
	return value;
}

i16 Msx2_FieldAtk(u8 owner, u8 slot)
{
	i16 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FieldAtk_In(owner, slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

i16 Msx2_FieldDef(u8 owner, u8 slot)
{
	i16 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FieldDef_In(owner, slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

u8 Msx2_LiveMonsterCount(u8 owner)
{
	u8 i, count = 0;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(Msx2_IsMonster(g_duel.side[owner].field[i]))
			++count;
	return count;
}

u8 Msx2_FirstLiveSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(Msx2_IsMonster(g_duel.side[owner].field[i]))
			return i;
	return MSX2_SLOT_NONE;
}

u8 Msx2_FirstFreeSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(!Msx2_IsMonster(g_duel.side[owner].field[i]))
			return i;
	return MSX2_SLOT_NONE;
}

u8 Msx2_FirstFreeEquipSlot(u8 owner)
{
	u8 i;
	for(i = 0; i < MSX2_FIELD; ++i)
		if(g_duel.side[owner].equip_field[i] == MSX2_CARD_NONE)
			return i;
	return MSX2_SLOT_NONE;
}

bool Msx2_FirstTurnAttackLocked(void)
{
	return (g_duel.turn_owner == MSX2_OWNER_PLAYER) && (g_duel.turns <= 1);
}

bool Msx2_Attack(u8 owner, u8 attacker_slot, u8 defender_slot)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_Attack_In(owner, attacker_slot, defender_slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

bool Msx2_PlaceMonster(u8 owner, u8 hand_slot, u8 field_slot, bool defense)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_PlaceMonster_In(owner, hand_slot, field_slot, defense);
	MSX2_DUEL_RESTORE(back);
	return value;
}

u8 Msx2_FusionPreview(u8 owner, const u8* hand_slots, u8 count, u8 field_slot)
{
	u8 value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FusionPreview_In(owner, hand_slots, count, field_slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

bool Msx2_PlaceFusion(u8 owner, const u8* hand_slots, u8 count, u8 field_slot,
                      bool defense)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_PlaceFusion_In(owner, hand_slots, count, field_slot, defense);
	MSX2_DUEL_RESTORE(back);
	return value;
}

bool Msx2_FusionSucceeded(void)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_FusionSucceeded_In();
	MSX2_DUEL_RESTORE(back);
	return value;
}

bool Msx2_PlaySupport(u8 owner, u8 hand_slot, u8 target_slot)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_PlaySupport_In(owner, hand_slot, target_slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

bool Msx2_ChangePosition(u8 owner, u8 field_slot)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_ChangePosition_In(owner, field_slot);
	MSX2_DUEL_RESTORE(back);
	return value;
}

void Msx2_DuelInit(u32 seed, u8 story_duel_index)
{
	u16 back = MSX2_DUEL_BACK();
	Msx2_DuelInit_In(seed, story_duel_index);
	MSX2_DUEL_RESTORE(back);
}

void Msx2_DuelSetPlayerDeck(const u8* cards, u8 count)
{
	u16 back = MSX2_DUEL_BACK();
	Msx2_DuelSetPlayerDeck_In(cards, count);
	MSX2_DUEL_RESTORE(back);
}

void Msx2_EndTurn(void)
{
	u16 back = MSX2_DUEL_BACK();
	Msx2_EndTurn_In();
	MSX2_DUEL_RESTORE(back);
}

bool Msx2_DuelStep(void)
{
	bool value;
	u16 back = MSX2_DUEL_BACK();
	value = Msx2_DuelStep_In();
	MSX2_DUEL_RESTORE(back);
	return value;
}

#undef MSX2_DUEL_RESTORE
#undef MSX2_DUEL_BACK

#ifdef MSX2_PLUS
// The SCREEN 10 fill has a larger command path than the resident GRAPHIC 7
// fill.  Keep that cold path beside the modal screens, where the ASCII16-X
// bank has room, while preserving one public entry for every caller.
void Msx2_Fill(u8 x, u8 y, u16 w, u8 h, u8 color)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_Fill_In(x, y, w, h, color);
	Msx2_Bank0Leave(back);
}
#endif

#endif

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
