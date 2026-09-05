// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe_bank.c — filling the verification stamp, and the regression
//  diagnostic block
//
//  The stamp itself is RAM owned by msx2_probe.c; this is the code that writes
//  it, in the modal bank rather than in _CODE.  Both routines are called from
//  the top of the main loop in msx2_main.c through the trampolines in
//  msx2_bank.c, never from a page-0 bank and never from the ISR, so the window
//  they need is always free to take.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_probe.h"
#include "msx2_bank.h"
#include "msx2_audio.h"
#include "msx2_entropy.h"
#include "msx2_duel.h"

// MSXgl's crt0 stores s__HEAP -- the first address above every statically
// allocated byte of RAM -- here, and starts the stack at HIMEM.  The gap
// between the two is the port's real working headroom, so both ends are
// reported rather than trusted from the map file alone.
extern u16 g_HeapStartAddress;

// Read SP.  Routed through a global rather than a __naked return so it does
// not depend on which register SDCC's calling convention returns u16 in.
u16 g_msx2_sp_scratch;

static u16 Msx2_StackPointer(void)
{
	__asm
		ld		hl, #0
		add		hl, sp
		ld		(_g_msx2_sp_scratch), hl
	__endasm;
	return g_msx2_sp_scratch;
}

void Msx2_ProbeInit_In(void)
{
	u8* p = (u8*)&g_probe[0];
	u16 i;
	u8 slot;
	for(i = 0; i < sizeof(g_probe); ++i)
		p[i] = 0;
	for(slot = 0; slot < MSX2_PROBE_SLOTS; ++slot)
	{
		g_probe[slot].magic[0] = MSX2_PROBE_MAGIC0;
		g_probe[slot].magic[1] = MSX2_PROBE_MAGIC1;
		g_probe[slot].magic[2] = MSX2_PROBE_MAGIC2;
		g_probe[slot].magic[3] = MSX2_PROBE_MAGIC3;
		g_probe[slot].version = MSX2_PROBE_VERSION;
		g_probe[slot].data_end = g_HeapStartAddress;
	}
	g_probe_slot = 0;
	g_stat_frame = 0;
	g_stat_steps = 0;
	g_stat_duels = 0;
	g_stat_wins_player = 0;
	g_stat_wins_com = 0;
	g_stat_status = MSX2_PROBE_OK;
	g_stat_scene = MSX2_SCENE_TITLE;
	g_stat_menu_cursor = 0xFF;
}

void Msx2_ProbeUpdate_In(void)
{
	Msx2Probe* probe = &g_probe[g_probe_slot];
	const u8* p = (const u8*)probe;
	u16 sum = 0;
	u16 i;
	u8 s;

	probe->status = g_stat_status;
	probe->frame = g_stat_frame;
	probe->steps = g_stat_steps;
	probe->duels_done = g_stat_duels;
	probe->wins_player = g_stat_wins_player;
	probe->wins_com = g_stat_wins_com;
	probe->turns = g_duel.turns;
	probe->phase = g_duel.phase;
	probe->turn_owner = g_duel.turn_owner;
	probe->result = g_duel.result;
	probe->lp_player = g_duel.side[MSX2_OWNER_PLAYER].lp;
	probe->lp_com = g_duel.side[MSX2_OWNER_COM].lp;
	probe->deck_player = (u8)waifu_deck_remaining(&g_duel.side[MSX2_OWNER_PLAYER].deck);
	probe->deck_com = (u8)waifu_deck_remaining(&g_duel.side[MSX2_OWNER_COM].deck);
	for(s = 0; s < 5; ++s)
	{
		probe->field_player[s] = g_duel.side[MSX2_OWNER_PLAYER].field[s];
		probe->field_com[s] = g_duel.side[MSX2_OWNER_COM].field[s];
		probe->hand_player[s] = g_duel.side[MSX2_OWNER_PLAYER].hand[s];
		probe->hand_com[s] = g_duel.side[MSX2_OWNER_COM].hand[s];
	}
	probe->data_end = g_HeapStartAddress;
	probe->sp = Msx2_StackPointer();
	probe->ram_free = (probe->sp > probe->data_end) ? (probe->sp - probe->data_end) : 0;
	probe->scene = g_stat_scene;
	probe->menu_cursor = g_stat_menu_cursor;

#ifdef MSX2_DEBUG_REGRESSION
	// A direct call: the copy lives in this bank too.
	Msx2_ProbeRegressionCopy_In(probe);
#endif

	// Everything up to (not including) the checksum field.  `stage` sits after
	// it and is excluded on purpose: it is written asynchronously.
	for(i = 0; i < (u16)((const u8*)&probe->checksum - (const u8*)probe); ++i)
		sum += p[i];
	probe->checksum = sum;

	// Only now is this slot complete, so the next update goes to the other one.
	g_probe_slot ^= 1;
}

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
