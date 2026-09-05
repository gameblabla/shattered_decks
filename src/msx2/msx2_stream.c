// ─────────────────────────────────────────────────────────────────────────────
//  msx2_stream.c — cartridge segment → VRAM streaming
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_stream.h"
#include "msx2_sprite.h"
#include "msx2_video.h"
#include "msx2_scenes.h"
#ifdef MSX2_DEBUG_REGRESSION
#include "msx2_probe.h"
#endif

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
// cartridge segment g_chunk_segment mapped there.
//
// The byte count is always a multiple of 256 (segments are, and so is the
// 5,120-byte tail of a 54,272-byte scene), so the loop is whole OTIR pages.
//
// THE MUSIC KEEPS ITS FRAME BETWEEN PAGES.
// Interrupts have to be off while the window holds the picture -- the ISR
// executes out of that window and remaps it for the lVGM stream -- but they do
// not have to be off for the whole chunk, and they used to be: a 16 KB OTIR is
// about 344,000 T-states, six V-blanks the audio tick never got, and a scene
// is four of those chunks.  That is the note the tune hangs on whenever a
// screen changes, and it is why a transition sounds like the machine stalled.
//
// So each 256-byte page ends with the code segment back in the window and one
// instruction of EI: a pending V-blank is taken there, the ISR decodes its
// lVGM frame and latches the keyboard out of the resident bank exactly as it
// would from the main loop, and the picture is mapped back afterwards.  The
// VDP's own write pointer is untouched by any of that -- the ISR does no VRAM
// work at all, which is the rule that makes this safe -- so the copy resumes
// where it left off.  It costs about 40 T-states a page against 5,400, and the
// window it opens is one page long instead of one chunk.
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
		jr		z, stream_done				// the last page needs no window

		// One interrupt window, with the resident code mapped back.  The page
		// counter and the source pointer go on the stack rather than into the
		// alternate set: the ISR is allowed to use AF'/BC'/DE'/HL'.
		push	af
		push	hl
		ld		hl, #MSX2_NEO_CODE_SEGMENT
		ld		(#MSX2_NEO_BANK2_REG), hl
		ld		(_g_bank2), hl
		ei
		nop									// the ISR is taken here, if pending
		di
		ld		hl, (_g_chunk_segment)
		ld		(#MSX2_NEO_BANK2_REG), hl
		ld		(_g_bank2), hl
		pop		hl
		pop		af
		jr		stream_page
	stream_done:

		ld		hl, #MSX2_NEO_CODE_SEGMENT
		ld		(#MSX2_NEO_BANK2_REG), hl	// map the code back
		ld		(_g_bank2), hl				// keep the ISR's shadow coherent
	__endasm;
	__asm ei __endasm;
}

void Msx2_StreamSceneBlanked(u16 segment, u8 page)
{
	u16 remaining = MSX2_SCENE_BYTES;
	u8 chunk = 0;

#ifdef MSX2_DEBUG_REGRESSION
	++g_msx2_regression_diag.full_view_streams;
#endif

	while(remaining != 0)
	{
		g_chunk_bytes = (remaining > MSX2_NEO_SEGMENT_SZ) ? MSX2_NEO_SEGMENT_SZ : remaining;
		g_chunk_segment = (u16)(segment + chunk);

		Msx2_StreamSetVramChunk((u8)((page << 2) | chunk));
		Msx2_StreamChunk();

		remaining = (u16)(remaining - g_chunk_bytes);
		++chunk;
	}

	// A SCREEN 10 picture brings its own sixteen colours, in the slack after
	// it.  The screen has already said which mode it is (Msx2_VideoModeYjk),
	// so this is the one place that has to know: the duel's own streams are
	// GRAPHIC 7 and never ask.
	if(Msx2_VideoIsYjk())
		Msx2_VideoScenePalette(segment);
}

void Msx2_StreamScene(u16 segment, u8 page)
{
	// THE SPRITE PLANE IS NOT PART OF THE PAGE FLIP.
	// This entry point is only ever the switch to another screen -- the title,
	// the sanctum map, a talk, the deck editor, the code screens.  None of them
	// uses a sprite, but the selector is an attribute in VRAM and nothing about
	// streaming a new picture takes it down: the gem stayed standing over the
	// new screen until whatever came next happened to hide it, which is the
	// frame or two of it that was visible on every transition.  Clearing here
	// costs 32 attribute writes and cannot flicker the duel's own selector,
	// because the board and the cut-ins stream through
	// Msx2_StreamSceneBlanked() instead.
	Msx2_SpriteClear();

	// EVERY SCREEN THAT COMES THROUGH HERE IS A PICTURE SCREEN.
	// This entry point is the switch to another 2-D screen -- the title, the
	// sanctum road, the continue code, the reward -- and in the MSX2+ build
	// every one of those is SCREEN 10.  The duel and its cut-ins stream through
	// Msx2_StreamSceneBlanked() and stay in GRAPHIC 7, so the mode is decided
	// by which door a screen came in at, not by a flag each screen has to
	// remember to set.
	Msx2_VideoModeYjk();

	// GRAPHIC 7 cannot keep up with OTIR while it is scanning out; blanking is
	// what makes the copy legal, and it is invisible anyway because a streamed
	// scene is always presented by a page flip afterwards.
	Msx2_VideoDisplayBlank();
	Msx2_StreamSceneBlanked(segment, page);
	Msx2_VideoDisplayRestore();
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
		ld		(_g_bank2), hl				// keep the ISR's shadow coherent
	__endasm;
	__asm ei __endasm;
}

void Msx2_StreamRect(u16 segment, u16 offset, u8 x, u8 y, u8 w, u8 h)
{
	// The command engine and the data port share the VDP's VRAM access slot;
	// writing through 0x98 while an HMMV is still running loses both.
	VDP_CommandWait();

	u8 page = Msx2_VideoGetDrawPage();
	u8 row;

	// Normalise the offset the way Msx2_RomReadLong does, and for the same
	// reason: a caller that addresses part of an asset -- the second half of a
	// card, say -- computes its offset by adding rows, and that sum can land
	// past the 16 KB window.  The loop below already carries the offset over
	// as it walks; without this the FIRST row could be read from the wrong
	// segment at a wrapped address.
	while(offset >= MSX2_NEO_SEGMENT_SZ)
	{
		offset = (u16)(offset - MSX2_NEO_SEGMENT_SZ);
		++segment;
	}

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

#ifdef MSX2_DEBUG_REGRESSION
	++g_msx2_regression_diag.band_streams;
#endif

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
	Msx2_PokeAtLine(x, (u16)((u16)y + ((u16)Msx2_VideoGetDrawPage() << 8)));
}

// The same address set-up against an absolute VRAM line, which is how the
// offscreen rows above the visible page -- where the font mask lives -- are
// written without pretending they belong to a draw page.
void Msx2_PokeAtLine(u8 x, u16 line)
{
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

// ─────────────────────────────────────────────────────────────────────────────
//  One pixel at a time, keeping the chroma that is already there
//
//  SCREEN 10 spends the low three bits of every byte on a hue its whole group
//  of four shares, so a byte written blind carries a quarter of a colour that
//  belongs to the three pixels beside it.  Anything that draws a shape whose
//  edge does not land on a group boundary -- a cut-out bust, and only that so
//  far -- has to read the byte back, keep those three bits and change nothing
//  but the brightness.  That is the whole of this routine, and it is what
//  gives a figure a per-pixel silhouette in a mode whose hue is four pixels
//  wide.
//
//  It is a VRAM READ with the display running, so it obeys the same rule the
//  rectangle blitter does: the VDP needs its ~29 T-states between accesses,
//  and the register set-up between one pixel and the next is far more than
//  that.  Interrupts stay off for the row -- the address is two writes to
//  0x99 and an ISR that touched the VDP between them would put the pixel
//  somewhere else entirely.
// ─────────────────────────────────────────────────────────────────────────────

static const u8* g_merge_rec;
static u8 g_merge_n;

// `rec` is `n` (column, value) pairs; each value is a whole byte whose low
// three bits are zero.  All of them land on one VRAM line.
void Msx2_MergeRow(const u8* rec, u8 n, u16 line)
{
	VDP_CommandWait();

	g_merge_rec = rec;
	g_merge_n = n;
	g_blit_r14 = (u8)(line >> 6);
	g_blit_hi = (u8)(line & 0x3F);          // bit 6 clear: a READ address

	__asm
		di
		ld		a, (_g_merge_n)
		or		a
		jr		z, merge_done
		ld		hl, (_g_merge_rec)
		ld		b, a
	merge_px:
		ld		e, (hl)						// column
		inc		hl
		ld		d, (hl)						// the Y bits, low three clear
		inc		hl
		push	hl

		ld		a, (_g_blit_r14)			// point at it for reading
		out		(#0x99), a
		ld		a, #(14 | 0x80)
		out		(#0x99), a
		ld		a, e
		out		(#0x99), a
		ld		a, (_g_blit_hi)
		out		(#0x99), a
		ex		(sp), hl					// the VDP's read latency, spent
		ex		(sp), hl					// on something rather than on nops
		in		a, (#0x98)
		and		#0x07						// the group's chroma stays
		or		d
		ld		c, a

		ld		a, (_g_blit_r14)			// and again for writing
		out		(#0x99), a
		ld		a, #(14 | 0x80)
		out		(#0x99), a
		ld		a, e
		out		(#0x99), a
		ld		a, (_g_blit_hi)
		or		#0x40
		out		(#0x99), a
		ld		a, c
		out		(#0x98), a

		pop		hl
		djnz	merge_px
	merge_done:
	__endasm;
	__asm ei __endasm;
}

// One interface line, out of the cartridge.
//
// The words the duel and story screens print are cartridge data, not code:
// segment 2 holds both screens in a hard 16 KB, and their literals were 1.6 KB
// of it.  A caller uses the returned pointer straight away -- there is one
// buffer, so two live strings at once would be one string twice.
static c8 g_ui_text[MSX2_UI_STRIDE];

const c8* Msx2_UiText(u8 id)
{
	Msx2_RomRead(MSX2_TEXT_SEGMENT,
	             (u16)(MSX2_UI_OFFSET + (u16)id * MSX2_UI_STRIDE),
	             (u8*)g_ui_text, MSX2_UI_STRIDE);
	g_ui_text[MSX2_UI_STRIDE - 1] = 0;
	return g_ui_text;
}

void Msx2_RomReadLong(u16 segment, u16 offset, u8* dst, u16 len)
{
	// Same normalisation as Msx2_RomRead, and for the same reason: the room
	// left in the segment is computed here, and an offset past the window
	// turned that subtraction into a very large number.
	while(offset >= MSX2_NEO_SEGMENT_SZ)
	{
		offset = (u16)(offset - MSX2_NEO_SEGMENT_SZ);
		++segment;
	}
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
	// AN OFFSET IS NOT A WINDOW POSITION.
	// Callers address a blob by its first segment and a byte offset into it,
	// and blobs outgrow 16 KB: the text table did, the moment card
	// descriptions went into it, and every string past the boundary silently
	// came back as whatever happened to be at the same place in the first
	// segment.  Normalise here, once, rather than at each of the dozen call
	// sites -- and split a record that straddles the seam.
	while(offset >= MSX2_NEO_SEGMENT_SZ)
	{
		offset = (u16)(offset - MSX2_NEO_SEGMENT_SZ);
		++segment;
	}
	if((u16)(offset + len) > MSX2_NEO_SEGMENT_SZ)
	{
		u8 head = (u8)(MSX2_NEO_SEGMENT_SZ - offset);
		Msx2_RomRead(segment, offset, dst, head);
		Msx2_RomRead((u16)(segment + 1), 0, dst + head, (u8)(len - head));
		return;
	}

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
		ld		(_g_bank2), hl				// keep the ISR's shadow coherent
	__endasm;
	__asm ei __endasm;
}
