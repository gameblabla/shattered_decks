// ─────────────────────────────────────────────────────────────────────────────
//  msx2_libc.c — the handful of C library entry points the shared game logic
//  expects and a bare MSX cartridge does not have.
//
//  src/game/deck.c seeds its RNG from time() and clock().  Neither exists on a
//  ROM cartridge, and the point of the seed there is entropy, not wall time --
//  so both are answered from the VDP frame counter, which is genuinely
//  unpredictable relative to when the player presses a key.  Defining them here
//  keeps the shared source compiling unmodified, which is the whole contract of
//  this fork.
// ─────────────────────────────────────────────────────────────────────────────

#include "msxgl.h"

// Frames since boot, counted by our own vblank ISR in msx2_main.c.  The BIOS
// handler (and therefore JIFFY) does not run on this target: MSXgl's mapped-ROM
// crt0 puts its own ISR in page 0.
extern volatile u16 g_msx2_ticks;

unsigned long time(unsigned long* t)
{
	unsigned long now = (unsigned long)g_msx2_ticks;
	// deck.c is the only MSX2 caller and passes NULL; there is no hosted
	// caller that needs the optional result pointer in this bare-metal shim.
	(void)t;
	return now;
}

// deck.c calls clock() without a declaration in scope, so it must return int.
int clock(void)
{
	return (int)g_msx2_ticks;
}
