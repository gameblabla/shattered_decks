// ─────────────────────────────────────────────────────────────────────────────
//  msx2_mapper.c — ASCII16-X's address-encoded page-2 write
//
//  This file is included by the ASCII16-X page-0 resident wrapper.  NEO-16
//  keeps its existing mapper code and therefore its existing binary.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_mapper.h"

#ifdef MSX2_ASCII16X

static u16 g_mapper_segment;

void Msx2_MapperSetPage2(u16 segment)
{
	// The caller has disabled interrupts when it needs the shadow and hardware
	// update to be one transaction.  This helper deliberately does not change
	// IFF: it is also used from the resident V-blank path.
	g_mapper_segment = segment;
	__asm
		ld		hl, (_g_mapper_segment)
		ld		a, h
		and		#0x0F
		or		#0x70
		ld		h, a
		ld		a, l
		ld		(hl), a
	__endasm;
}

#endif
