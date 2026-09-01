// ─────────────────────────────────────────────────────────────────────────────
//  msx2_stream.c — cartridge segment → VRAM streaming
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_stream.h"
#include "msx2_video.h"
#include "msx2_scenes.h"

// NEO-16 maps three 16 KB banks; writing a 16-bit segment number to the bank's
// magic address switches it.  Bank 2 is 0x8000-0xBFFF -- the streaming window.
#define MSX2_NEO_BANK2_REG   0x7000
#define MSX2_NEO_WINDOW      0x8000
#define MSX2_NEO_SEGMENT_SZ  0x4000

// The segment the resident code above 0x8000 lives in, restored after every
// chunk.  MSXgl's crt0 maps segment 1 there at boot and nothing else moves it.
#define MSX2_NEO_CODE_SEGMENT 1

// Chunk parameters, passed through globals rather than the stack: the copy loop
// is inline assembly inside a bank switch, and reaching for stack locals there
// would mean trusting SDCC's frame pointer in a window where a mis-step is a
// crash rather than a wrong pixel.
static u16 g_chunk_bytes;
static u16 g_chunk_segment;

// Point the VDP's write pointer at a 16 KB-aligned VRAM address.
// R#14 carries A16-A14, so for an aligned address the other twelve bits are 0.
static void Msx2_StreamSetVramChunk(u8 r14)
{
	r14;   // A
	__asm
		di
		out		(#0x99), a				// R#14 = A16-A14
		ld		a, #(14 | 0x80)
		out		(#0x99), a
		xor		a
		out		(#0x99), a				// A7-A0 = 0
		ld		a, #0x40				// A13-A8 = 0, write-enable
		out		(#0x99), a
		ei
	__endasm;
}

// Copy g_chunk_bytes from the 0x8000 window to the VDP data port, with the
// cartridge segment g_chunk_segment mapped there.  Interrupts stay off for the
// whole chunk because the ISR does not exist while the window is swapped.
//
// The byte count is always a multiple of 256 (segments are, and so is the
// 5,120-byte tail of a 54,272-byte scene), so the loop is whole OTIR pages.
static void Msx2_StreamChunk(void)
{
	__asm
		di
		ld		hl, (_g_chunk_segment)
		ld		(#MSX2_NEO_BANK2_REG), hl	// map the picture in

		ld		hl, #MSX2_NEO_WINDOW
		ld		de, (_g_chunk_bytes)
		ld		c, #0x98					// VDP data port
		ld		a, d						// whole 256-byte pages
	stream_page:
		or		a
		jr		z, stream_done
		ld		b, #0
		otir
		dec		a
		jr		stream_page
	stream_done:

		ld		hl, #MSX2_NEO_CODE_SEGMENT
		ld		(#MSX2_NEO_BANK2_REG), hl	// map the code back
		ei
	__endasm;
}

void Msx2_StreamSceneBlanked(u16 segment, u8 page)
{
	u16 remaining = MSX2_SCENE_BYTES;
	u8 chunk = 0;

	while(remaining != 0)
	{
		g_chunk_bytes = (remaining > MSX2_NEO_SEGMENT_SZ) ? MSX2_NEO_SEGMENT_SZ : remaining;
		g_chunk_segment = (u16)(segment + chunk);

		Msx2_StreamSetVramChunk((u8)((page << 2) | chunk));
		Msx2_StreamChunk();

		remaining = (u16)(remaining - g_chunk_bytes);
		++chunk;
	}

}

void Msx2_StreamScene(u16 segment, u8 page)
{
	// GRAPHIC 7 cannot keep up with OTIR while it is scanning out; blanking is
	// what makes the copy legal, and it is invisible anyway because a streamed
	// scene is always presented by a page flip afterwards.
	VDP_EnableDisplay(FALSE);
	Msx2_StreamSceneBlanked(segment, page);
	VDP_EnableDisplay(TRUE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rectangles, with the display running
//
//  A card is 40x48 = 1,920 bytes, which at the padded 32 T-states per byte the
//  VDP needs while it is scanning out is about one frame.  That is the unit the
//  duel screen is built from: placing a card costs a frame per page, and
//  nothing else on screen has to be touched.
//
//  Parameters travel through globals for the same reason the chunk copy's do --
//  the loop runs inside a bank switch, where trusting SDCC's frame pointer is a
//  crash rather than a wrong pixel.
// ─────────────────────────────────────────────────────────────────────────────

static u16 g_blit_segment;
static u16 g_blit_src;
static u8  g_blit_r14;
static u8  g_blit_lo;
static u8  g_blit_hi;
static u8  g_blit_len;
static u8* g_blit_dst;

// One row.  Sets the VDP write pointer, maps the data in, pushes g_blit_len
// bytes at the data port and puts the code segment back.
//
// The inner loop is `outi / nop / jr nz`, 32 T-states per byte.  OTIR's 21 is
// below the ~29 GRAPHIC 7 wants with the display enabled, and bytes the VDP is
// not ready for are silently dropped -- which shows up as a card with diagonal
// tearing, not as an error.
static void Msx2_BlitRow(void)
{
	__asm
		di
		ld		hl, (_g_blit_segment)
		ld		(#MSX2_NEO_BANK2_REG), hl

		ld		a, (_g_blit_r14)
		out		(#0x99), a
		ld		a, #(14 | 0x80)
		out		(#0x99), a
		ld		a, (_g_blit_lo)
		out		(#0x99), a
		ld		a, (_g_blit_hi)
		out		(#0x99), a

		ld		hl, (_g_blit_src)
		ld		a, (_g_blit_len)
		ld		b, a
		ld		c, #0x98
	blit_px:
		outi
		nop
		jr		nz, blit_px

		ld		hl, #MSX2_NEO_CODE_SEGMENT
		ld		(#MSX2_NEO_BANK2_REG), hl
		ei
	__endasm;
}

void Msx2_StreamRect(u16 segment, u16 offset, u8 x, u8 y, u8 w, u8 h)
{
	// The command engine and the data port share the VDP's VRAM access slot;
	// writing through 0x98 while an HMMV is still running loses both.
	VDP_CommandWait();

	u8 page = Msx2_VideoGetDrawPage();
	u8 row;

	g_blit_len = w;
	g_blit_lo = x;

	for(row = 0; row < h; ++row)
	{
		u16 line = (u16)y + row;
		// GRAPHIC 7 addressing: A16 is the page, A15-A8 the line, A7-A0 the
		// column.  R#14 carries A16-A14, and the 0x40 marks the write.
		g_blit_r14 = (u8)(((u16)page << 2) | (u8)(line >> 6));
		g_blit_hi = (u8)((u8)(line & 0x3F) | 0x40);
		g_blit_segment = segment;
		g_blit_src = (u16)(MSX2_NEO_WINDOW + offset);
		Msx2_BlitRow();

		offset = (u16)(offset + w);
		if(offset >= MSX2_NEO_SEGMENT_SZ)
		{
			offset = (u16)(offset - MSX2_NEO_SEGMENT_SZ);
			++segment;
		}
	}
}

// A whole band of full-width rows.  Same inner loop as a rectangle -- the width
// is 256, which is exactly what a zero byte count means to `outi`, so the row
// blitter needs no change to carry it.
void Msx2_StreamBand(u16 segment, u8 y, u8 h)
{
	VDP_CommandWait();

	u8 page = Msx2_VideoGetDrawPage();
	u16 offset = 0;
	u8 row;

	g_blit_len = 0;                     // 0 means 256 to the djnz-style loop
	g_blit_lo = 0;

	for(row = 0; row < h; ++row)
	{
		u16 line = (u16)y + row;
		g_blit_r14 = (u8)(((u16)page << 2) | (u8)(line >> 6));
		g_blit_hi = (u8)((u8)(line & 0x3F) | 0x40);
		g_blit_segment = segment;
		g_blit_src = (u16)(MSX2_NEO_WINDOW + offset);
		Msx2_BlitRow();

		offset = (u16)(offset + 256);
		if(offset >= MSX2_NEO_SEGMENT_SZ)
		{
			offset = 0;
			++segment;
		}
	}
}

// The DUP of §8.4.  The VDP's write address auto-advances, so a magnified run
// is n OUTs and nothing else -- no source pointer, no address re-set.  The
// padding is the same 32 T-states per byte the rectangle blitter pays: with the
// display on, GRAPHIC 7 drops bytes the VDP was not ready for, and a dropped
// byte shows up as tearing rather than as an error.
static u8 g_poke_value;
static u8 g_poke_count;

void Msx2_PokeRun(u8 value, u8 n)
{
	if(n == 0)
		return;
	g_poke_value = value;
	g_poke_count = n;
	__asm
		ld		a, (_g_poke_count)
		ld		b, a
		ld		a, (_g_poke_value)
		ld		c, #0x98
		// A local label, not a named one: a named label ends the assembler's
		// local-label scope, and the compiler's own "00103$" branch out of the
		// early return above is then undefined at the point it is used.
	00090$:
		out		(c), a
		nop
		nop
		djnz	00090$
	__endasm;
}

static const u8* g_poke_src;

void Msx2_PokeBlock(const u8* src, u8 n)
{
	if(n == 0)
		return;
	g_poke_src = src;
	g_poke_count = n;
	__asm
		ld		hl, (_g_poke_src)
		ld		a, (_g_poke_count)
		ld		b, a
		ld		c, #0x98
	00091$:
		outi
		nop
		jr		nz, 00091$
	__endasm;
}

void Msx2_PokeAt(u8 x, u8 y)
{
	u16 line = (u16)y + ((u16)Msx2_VideoGetDrawPage() << 8);

	VDP_CommandWait();
	g_blit_r14 = (u8)(line >> 6);
	g_blit_lo = x;
	g_blit_hi = (u8)((u8)(line & 0x3F) | 0x40);
	__asm
		di
		ld		a, (_g_blit_r14)
		out		(#0x99), a
		ld		a, #(14 | 0x80)
		out		(#0x99), a
		ld		a, (_g_blit_lo)
		out		(#0x99), a
		ld		a, (_g_blit_hi)
		out		(#0x99), a
		ei
	__endasm;
}

void Msx2_RomReadLong(u16 segment, u16 offset, u8* dst, u16 len)
{
	while(len != 0)
	{
		u16 room = (u16)(MSX2_NEO_SEGMENT_SZ - offset);
		u8 chunk = (len > 255) ? 255 : (u8)len;
		if((u16)chunk > room)
			chunk = (u8)room;
		Msx2_RomRead(segment, offset, dst, chunk);
		dst += chunk;
		len = (u16)(len - chunk);
		offset = (u16)(offset + chunk);
		if(offset >= MSX2_NEO_SEGMENT_SZ)
		{
			offset = 0;
			++segment;
		}
	}
}

void Msx2_RomRead(u16 segment, u16 offset, u8* dst, u8 len)
{
	g_blit_segment = segment;
	g_blit_src = (u16)(MSX2_NEO_WINDOW + offset);
	g_blit_dst = dst;
	g_blit_len = len;
	__asm
		di
		ld		hl, (_g_blit_segment)
		ld		(#MSX2_NEO_BANK2_REG), hl

		ld		hl, (_g_blit_src)
		ld		de, (_g_blit_dst)
		ld		a, (_g_blit_len)
		ld		c, a
		ld		b, #0
		ldir

		ld		hl, #MSX2_NEO_CODE_SEGMENT
		ld		(#MSX2_NEO_BANK2_REG), hl
		ei
	__endasm;
}
