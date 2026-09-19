// ─────────────────────────────────────────────────────────────────────────────
//  msx2_entropy.h — human duel seed sources and deterministic test override
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

enum Msx2EntropySource
{
	MSX2_ENTROPY_RTC = 0x01,
	MSX2_ENTROPY_Z80_R = 0x02,
	MSX2_ENTROPY_TICKS = 0x04,
	MSX2_ENTROPY_INPUT = 0x08,
	MSX2_ENTROPY_FIXED = 0x80,
};

void Msx2_EntropyInit(void);
void Msx2_EntropyMixInput(u8 held, u8 pressed, c8 typed);
u32  Msx2_EntropyNextSeed(void);

// Implementations execute from the modal page-0 bank so RTC collection does
// not consume the fixed 32 KB streaming/code window.
void Msx2_EntropyInit_In(void);
void Msx2_EntropyMixInput_In(u8 held, u8 pressed, c8 typed);
u32  Msx2_EntropyNextSeed_In(void);

extern u32 g_msx2_initial_seed;
extern u32 g_msx2_duel_seed;
extern u8  g_msx2_entropy_flags;
