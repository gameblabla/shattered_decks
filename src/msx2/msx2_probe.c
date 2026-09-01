// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe.c — stamp the duel state into RAM for headless verification.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_probe.h"
#include "msx2_duel.h"

Msx2Probe g_probe[MSX2_PROBE_SLOTS];

// Which slot the next update writes.  The other one always holds a complete,
// self-consistent stamp for the reader to find.
static u8 g_probe_slot;

u16 g_stat_frame;
u16 g_stat_steps;
u16 g_stat_duels;
u16 g_stat_wins_player;
u16 g_stat_wins_com;
u8  g_stat_status;
u8  g_stat_scene;
u8  g_stat_menu_cursor;

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

void Msx2_ProbeUpdate(void)
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

	// Everything up to (not including) the checksum field.  `stage` sits after
	// it and is excluded on purpose: it is written asynchronously.
	for(i = 0; i < (u16)((const u8*)&probe->checksum - (const u8*)probe); ++i)
		sum += p[i];
	probe->checksum = sum;

	// Only now is this slot complete, so the next update goes to the other one.
	g_probe_slot ^= 1;
}
