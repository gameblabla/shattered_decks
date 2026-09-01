// ─────────────────────────────────────────────────────────────────────────────
//  msx2_probe.h — the blind-play observation channel
//
//  The bundled openmsx-headless can inject scripted keyboard input and
//  `openmsx snap` dumps the machine state, all 64 KiB of RAM and all 128 KiB of
//  VRAM.  So the ROM
//  stamps a small, magic-tagged struct into RAM every frame and
//  tools/msx2/read_probe.py finds it in the snapshot.  That is what makes the
//  first milestone -- "does the game fit in RAM, and can it be played
//  completely blind?" -- answerable without a single pixel being drawn.
//
//  Keep this struct small and POD: it is scanned for by magic, so its layout is
//  the wire format read_probe.py parses.  Bump MSX2_PROBE_VERSION on any change.
//
//  TWO SLOTS, alternating.  The emulator dumps RAM at an arbitrary instruction,
//  so a single struct is regularly caught half-written and its checksum fails --
//  which used to make a perfectly good run look like a broken one.  The probe
//  therefore fills one slot completely, checksum included, before the next
//  update moves to the other, so at any instant at least one slot is intact.
//  The reader takes the newest slot that checksums.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_PROBE_MAGIC0   'M'
#define MSX2_PROBE_MAGIC1   '2'
#define MSX2_PROBE_MAGIC2   'P'
#define MSX2_PROBE_MAGIC3   'B'
#define MSX2_PROBE_VERSION  2

// Why the run stopped being interesting, if it did.
#define MSX2_PROBE_OK       0
#define MSX2_PROBE_STUCK    1   // a duel exceeded its step watchdog
#define MSX2_PROBE_BADSTATE 2   // an invariant check failed

typedef struct Msx2Probe
{
	u8  magic[4];          // 'M','2','P','B'
	u8  version;
	u8  status;            // MSX2_PROBE_*
	u16 frame;             // frames since boot
	u16 steps;             // rules steps executed in the current duel
	u16 duels_done;
	u16 wins_player;
	u16 wins_com;
	u16 turns;             // turns in the current duel
	u8  phase;
	u8  turn_owner;
	i8  result;            // current duel result, 0 while running
	i16 lp_player;
	i16 lp_com;
	u8  deck_player;       // cards left
	u8  deck_com;
	u8  field_player[5];
	u8  field_com[5];
	u8  hand_player[5];
	u8  hand_com[5];
	u16 data_end;          // s__HEAP: first byte above all statically allocated RAM
	u16 sp;                // stack pointer at the moment of the stamp
	u16 ram_free;          // sp - data_end: what is left between data and stack
	u8  scene;             // MSX2_SCENE_*: which screen the game is on
	u8  menu_cursor;       // title row under the cursor, 0xFF while on attract
	u16 checksum;          // sum of every preceding byte, so a torn read shows
	// Written directly at any moment (see MSX2_STAGE), therefore deliberately
	// placed after the checksum and excluded from it.
	u8  stage;             // last point main() reached, for bring-up bisection
	u8  pad;
} Msx2Probe;

// The live counters.  Nothing outside Msx2_ProbeUpdate() may touch g_probe's
// checksummed region, or the struct is inconsistent whenever the emulator
// happens to dump RAM -- so the game increments these instead and the probe
// copies them in one pass, immediately before checksumming.
extern u16 g_stat_frame;
extern u16 g_stat_steps;
extern u16 g_stat_duels;
extern u16 g_stat_wins_player;
extern u16 g_stat_wins_com;
extern u8  g_stat_status;
extern u8  g_stat_scene;
extern u8  g_stat_menu_cursor;

#define MSX2_PROBE_SLOTS 2
extern Msx2Probe g_probe[MSX2_PROBE_SLOTS];

void Msx2_ProbeInit(void);
void Msx2_ProbeUpdate(void);

// Bring-up bisection: mark how far execution got, so a crash shows up as a
// stage number in the RAM dump instead of a blank screen.  Sprinkle
// MSX2_STAGE() through any new code that will not boot; the reader prints it.
#define MSX2_STAGE(n)  do { \
		*(volatile u8*)&g_probe[0].stage = (u8)(n); \
		*(volatile u8*)&g_probe[1].stage = (u8)(n); \
	} while(0)

#define MSX2_STAGE_BOOT  1   // past VDP/audio/probe init
#define MSX2_STAGE_LOOP  2   // entering the frame loop
#define MSX2_STAGE_TITLE 3   // title screen composed and shown
#define MSX2_STAGE_DUEL  4   // first duel dealt
#define MSX2_STAGE_STORY 5   // story mode entered

// Which screen the game is on.  The blind soak needs to tell "waiting on the
// title for a button" apart from "playing", or a run that never got past the
// menu would read as a run that never finished a duel.
#define MSX2_SCENE_TITLE 0
#define MSX2_SCENE_DUEL  1
#define MSX2_SCENE_STORY 2
