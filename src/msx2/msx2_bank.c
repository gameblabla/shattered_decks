// ─────────────────────────────────────────────────────────────────────────────
//  msx2_bank.c — the page-0 window, and the trampolines through it
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_bank.h"
#include "msx2_screens.h"
#include "msx2_disk.h"

// NEO-16's page-0 bank register.  Writing a 16-bit segment number to it maps
// that segment at 0x0000; the address itself is ROM, so the write only ever
// reaches the mapper.
#define MSX2_NEO_BANK0_REG  0x5000

// The mapper is write-only, so the current segment is tracked here.  It starts
// at segment 2 because that is what crt0 maps at boot.
static u16 g_bank0 = MSX2_BANK0_MAIN;

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

// ── The trampolines ─────────────────────────────────────────────────────────
// One per entry point into segment 3.  They are the public names: nothing
// outside this file knows the window moved.

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

bool Msx2_DiskPresent(void)
{
	bool present;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	present = Msx2_DiskPresent_In();
	Msx2_Bank0Leave(back);
	return present;
}

bool Msx2_DiskSave(const c8* code)
{
	bool ok;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	ok = Msx2_DiskSave_In(code);
	Msx2_Bank0Leave(back);
	return ok;
}

bool Msx2_DiskLoad(c8* code)
{
	bool ok;
	u16 back = Msx2_Bank0Enter(MSX2_BANK0_MODAL);
	ok = Msx2_DiskLoad_In(code);
	Msx2_Bank0Leave(back);
	return ok;
}
