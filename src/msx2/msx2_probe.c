// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe.c — stamp the duel state into RAM for headless verification.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_probe.h"
#include "msx2_duel.h"

Msx2Probe g_probe;

u16 g_stat_frame;
u16 g_stat_steps;
u16 g_stat_duels;
u16 g_stat_wins_player;
u16 g_stat_wins_com;
u8  g_stat_status;

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

void Msx2_ProbeInit(void)
{
	u8* p = (u8*)&g_probe;
	u16 i;
	for(i = 0; i < sizeof(Msx2Probe); ++i)
		p[i] = 0;
	g_probe.magic[0] = MSX2_PROBE_MAGIC0;
	g_probe.magic[1] = MSX2_PROBE_MAGIC1;
	g_probe.magic[2] = MSX2_PROBE_MAGIC2;
	g_probe.magic[3] = MSX2_PROBE_MAGIC3;
	g_probe.version = MSX2_PROBE_VERSION;
	g_probe.data_end = g_HeapStartAddress;
	g_stat_frame = 0;
	g_stat_steps = 0;
	g_stat_duels = 0;
	g_stat_wins_player = 0;
	g_stat_wins_com = 0;
	g_stat_status = MSX2_PROBE_OK;
}

void Msx2_ProbeUpdate(void)
{
	const u8* p = (const u8*)&g_probe;
	u16 sum = 0;
	u16 i;
	u8 s;

	g_probe.status = g_stat_status;
	g_probe.frame = g_stat_frame;
	g_probe.steps = g_stat_steps;
	g_probe.duels_done = g_stat_duels;
	g_probe.wins_player = g_stat_wins_player;
	g_probe.wins_com = g_stat_wins_com;
	g_probe.turns = g_duel.turns;
	g_probe.phase = g_duel.phase;
	g_probe.turn_owner = g_duel.turn_owner;
	g_probe.result = g_duel.result;
	g_probe.lp_player = g_duel.side[MSX2_OWNER_PLAYER].lp;
	g_probe.lp_com = g_duel.side[MSX2_OWNER_COM].lp;
	g_probe.deck_player = (u8)waifu_deck_remaining(&g_duel.side[MSX2_OWNER_PLAYER].deck);
	g_probe.deck_com = (u8)waifu_deck_remaining(&g_duel.side[MSX2_OWNER_COM].deck);
	for(s = 0; s < 5; ++s)
	{
		g_probe.field_player[s] = g_duel.side[MSX2_OWNER_PLAYER].field[s];
		g_probe.field_com[s] = g_duel.side[MSX2_OWNER_COM].field[s];
		g_probe.hand_player[s] = g_duel.side[MSX2_OWNER_PLAYER].hand[s];
		g_probe.hand_com[s] = g_duel.side[MSX2_OWNER_COM].hand[s];
	}
	g_probe.data_end = g_HeapStartAddress;
	g_probe.sp = Msx2_StackPointer();
	g_probe.ram_free = (g_probe.sp > g_probe.data_end) ? (g_probe.sp - g_probe.data_end) : 0;

	// Everything up to (not including) the checksum field.  `stage` sits after
	// it and is excluded on purpose: it is written asynchronously.
	for(i = 0; i < (u16)((const u8*)&g_probe.checksum - (const u8*)&g_probe); ++i)
		sum += p[i];
	g_probe.checksum = sum;
}
