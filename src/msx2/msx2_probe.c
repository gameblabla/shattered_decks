// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe.c — the RAM the headless verification reads.
//
//  Only the state lives here.  The two routines that fill it are in the modal
//  code bank (msx2_probe_bank.c), reached through the trampolines in
//  msx2_bank.c: they are called three times from the top of the main loop and
//  nowhere else, they are eight hundred bytes, and _CODE is a hard 32 KB that
//  the FM music decoders now share.  The state stays here because it is RAM,
//  which is mapped whatever the banks hold, and half the port writes to it.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_probe.h"

Msx2Probe g_probe[MSX2_PROBE_SLOTS];

// Which slot the next update writes.  The other one always holds a complete,
// self-consistent stamp for the reader to find.  Not static: the bank writes it.
u8 g_probe_slot;

u16 g_stat_frame;
u16 g_stat_steps;
u16 g_stat_duels;
u16 g_stat_wins_player;
u16 g_stat_wins_com;
u8  g_stat_status;
u8  g_stat_scene;
u8  g_stat_menu_cursor;

