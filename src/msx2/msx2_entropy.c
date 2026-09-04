// ─────────────────────────────────────────────────────────────────────────────
//  msx2_entropy.c — resident state and page-0 entropy trampolines
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_entropy.h"
#include "msx2_bank.h"

u32 g_msx2_initial_seed;
u32 g_msx2_duel_seed;

void Msx2_EntropyInit(void)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_EntropyInit_In();
	Msx2_Bank0Leave(back);
}

void Msx2_EntropyMixInput(u8 held, u8 pressed, c8 typed)
{
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	Msx2_EntropyMixInput_In(held, pressed, typed);
	Msx2_Bank0Leave(back);
}

u32 Msx2_EntropyNextSeed(void)
{
	u32 seed;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	seed = Msx2_EntropyNextSeed_In();
	Msx2_Bank0Leave(back);
	return seed;
}

u8 Msx2_EntropyFlags(void)
{
	return g_msx2_entropy_flags;
}
