// ─────────────────────────────────────────────────────────────────────────────
//  msx2_video.c — GRAPHIC 7 video layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_video.h"

#include "msx2_stream.h"
#include "msx2_sprite.h"
#include "msx2_scenes.h"
#ifdef MSX2_DEBUG_REGRESSION
#include "msx2_probe.h"
#endif

// THE FONT LIVES IN THE CARTRIDGE.
// MSXgl ships this one as a 1540-byte C array -- 192 characters, of which the
// game prints 64 -- and that is 1540 bytes of a hard 32 KB code budget spent on
// glyphs the cartridge has megabytes of room for.  The printable slice, ASCII
// 32 to 95, is read into RAM once at boot instead.
u8 g_msx2_font[MSX2_FONT_BYTES];

// The machine, as its own main ROM reports it (MSXVER_*): 1 on an MSX2, 2 on
// an MSX2+, 3 on a turbo R.  Written once at boot by msx2_audio_probe.c, which
// is the only place the BIOS can be reached from.  Zero until then, which
// reads as "the slowest machine this cartridge runs on" everywhere it is used.
u8 g_msx2_msxver;

void Msx2_VideoLoadFont(void)
{
	Msx2_RomReadLong(MSX2_TEXT_SEGMENT, MSX2_FONT_OFFSET, g_msx2_font,
	                 MSX2_FONT_BYTES);
}

// THE TEXT WRITER, AND WHY IT IS NOT MSXgl's.
//
// Print_DrawText goes through the command engine, one HMMC per character, and
// measured at roughly four milliseconds a letter on this machine: a duel-screen
// repaint -- the HUD, the card name, the stats and the prompt -- took twenty-two
// frames, so every cursor move in the hand cost a third of a second before the
// bracket followed it, twice over because both pages owe the repaint.
//
// This one is below, and it is the command engine again -- but copying a mask
// that is already in VRAM instead of pushing pixels through the port.
// R#32..R#46 in the order the indirect port takes them: SX, SY, DX, DY, NX, NY,
// CLR, ARG, CMD.  One array, one `otir`, one command.
static u8 g_cmd[15];

static u8 g_draw_page;
static u8 g_show_page;
static u8 g_flip_pending;
static u8 g_text_fg;
static u8 g_text_bg;

#ifdef MSX2_PLUS
#include "msx2_plus_scenes.h"

// SCREEN 10 IS GRAPHIC 7 WITH TWO BITS OF R#25 SET.
// Everything else about the mode -- the 256-byte line, the two pages, the
// command engine, the sprite plane and its FIXED colour table -- is unchanged,
// which is the whole reason this port can carry a second cartridge at all: one
// register write, and the same code draws the same screens out of pictures
// baked in the other encoding.
static bool g_yjk;

void Msx2_VideoModeYjk(void)
{
	if(!g_yjk)
	{
		g_yjk = TRUE;
		VDP_SetYJK(VDP_YJK_YAE);
	}
}

void Msx2_VideoModeG7(void)
{
	if(g_yjk)
	{
		g_yjk = FALSE;
		VDP_SetYJK(VDP_YJK_OFF);
	}
}

bool Msx2_VideoIsYjk(void)
{
	return g_yjk;
}

// A picture's own sixteen colours, out of the slack after it.
// Entries 0..7 are the interface's and are the same in every scene; 8..15 are
// fitted to this one picture, which is where most of YJK's dark end comes from.
static u8 g_pal[MSX2_SCENE_PALETTE_BYTES];

// ALL SIXTEEN ENTRIES, FROM ENTRY ZERO.
// MSXgl's VDP_SetPalette starts at index 1 unless VDP_USE_PALETTE16 is
// configured -- it is written for the modes where colour 0 is the border and
// nothing else -- and a palette written one entry late is every colour on the
// screen shifted by one, which is what the first plus capture showed.  Entry 0
// is the interface's black here and has to land where the picture expects it,
// so this writes the register and pushes the thirty-two bytes itself.
void Msx2_VideoScenePalette(u16 segment)
{
	Msx2_RomRead(segment, MSX2_SCENE_PALETTE_OFFSET, g_pal,
	             MSX2_SCENE_PALETTE_BYTES);
	__asm
		di
		xor		a						// palette pointer = entry 0
		out		(#0x99), a
		ld		a, #(16 | 0x80)
		out		(#0x99), a
		ld		hl, #_g_pal
		ld		c, #0x9A				// the palette port, auto-incrementing
		ld		b, #32
		otir
		ei
	__endasm;
}
#endif

void Msx2_VideoInit(void)
{
	VDP_SetMode(VDP_MODE_GRAPHIC7);
	VDP_SetColor(MSX2_BLACK);

	// VBlank on, display off, in the one register write: VDP_SetMode's own
	// default settings just turned the display back on, and the sweep below
	// takes long enough at 512 lines that it was visible as a wipe over
	// whatever the BIOS left on screen. Both bits live in R#1, so one masked
	// write does what VDP_EnableVBlank(TRUE) + VDP_EnableDisplay(FALSE) would
	// have cost as two calls.
	VDP_RegWriteBakMask(1, (u8)~R01_BL, R01_IE0);

	// THE WHOLE 128 KB, ONCE, BEFORE ANYTHING ELSE ASSUMES ITS CONTENTS.
	// A hard reset (or a flash cart with no power-on VRAM clear) can leave
	// VRAM full of whatever the last ROM left there, so one HMMV across the
	// full 512 lines (both pages back to back) zeroes all of it up front --
	// including rows 240..255, which is page 0's sprite tables and page 1's
	// font mask (both written later in boot) and which the per-transition
	// Msx2_ClearPage deliberately never touches, so this is the only place
	// they start out clean. (VDP_CommandHMMV's own VDP_CommandSetupR36
	// already waits for the engine to be free, so no separate wait here.)
	VDP_CommandHMMV(0, 0, MSX2_SCREEN_W, 512, MSX2_BLACK);

	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);

	// Both pages start black, so a flip can never reveal boot garbage.
	// (Msx2_VideoShowPage sets g_draw_page and g_flip_pending itself.)
	Msx2_VideoShowPage(MSX2_PAGE_0);

	// The command fields that never vary: nothing a string draws is ever
	// taller than a glyph, wider than a screen, or drawn right to left.
	g_cmd[1] = 0;                       // the mask never starts past column 255
	g_cmd[3] = (u8)(MSX2_FONT_MASK_LINE >> 8);
	g_cmd[5] = 0;
	g_cmd[9] = 0;
	g_cmd[10] = MSX2_FONT_H_PX;
	g_cmd[11] = 0;
	g_cmd[13] = 0;

	// After the clears, because the mask lives above the lines they touch and
	// before anything prints, because every string is a copy out of it.
	Msx2_VideoBakeFont();

	// Everything the screen can show is clean now.
	VDP_EnableDisplay(TRUE);
}

void Msx2_VideoShowPage(u8 page)
{
	g_show_page = page;
	g_draw_page = (u8)(page ^ 1);
	g_flip_pending = FALSE;
	VDP_SetPage(page);
}

u8 Msx2_VideoGetShowPage(void)
{
	return g_show_page;
}

void Msx2_VideoFlipRequest(void)
{
	g_flip_pending = TRUE;
}

void Msx2_VideoPresent(void)
{
	if(!g_flip_pending)
		return;
	g_flip_pending = FALSE;
	g_show_page = g_draw_page;
	g_draw_page = (u8)(g_draw_page ^ 1);
	VDP_SetPage(g_show_page);
#ifdef MSX2_DEBUG_REGRESSION
	++g_msx2_regression_diag.page_flips;
#endif
}

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_VideoDisplayBlank(void)
{
	VDP_EnableDisplay(FALSE);
	++g_msx2_regression_diag.blank_pairs;
}

void Msx2_VideoDisplayRestore(void)
{
	VDP_EnableDisplay(TRUE);
}
#endif

void Msx2_VideoDrawPage(u8 page)
{
	g_draw_page = page;
}

u8 Msx2_VideoGetDrawPage(void)
{
	return g_draw_page;
}

// In GRAPHIC 7 the command engine sees VRAM as 256 wide by 512 lines, so page 1
// is simply Y + 256.  That is the whole of paging as far as drawing is
// concerned.
static u16 Msx2_PageY(u8 y)
{
	return (u16)y + (g_draw_page ? 256u : 0u);
}

// A FILL ON A SCREEN 10 SCREEN KEEPS EACH PIXEL'S CHROMA.
//
// A YAE pixel is an exact palette colour and owes its neighbours nothing --
// except that its low three bits are still read as part of its group's J or K.
// A byte-mode fill writes those bits as zero, so a panel whose left or right
// edge lands inside a group of four dragged the colour out of the picture
// pixels beside it: a coloured fringe down both sides of the title's menu box
// and of every plate in the story, which is what the interface was moved onto
// the palette to avoid in the first place.
//
// The fix is the same one gen_msx_plus.py uses when it stamps a box into a
// baked picture -- keep the low bits -- said to the command engine instead:
//
//     dest = (dest AND 7) OR ink
//
// which is two logical fills where a GRAPHIC 7 screen needs one byte-mode one.
// They are twice the work over four times the area, and a panel is a few
// thousand pixels once per screen, so nothing that matters pays for it.  The
// duel is GRAPHIC 7 in both cartridges and takes the fast path untouched.
#ifdef MSX2_PLUS
// One logical fill.  Out of line and written once: VDP_CommandLMMV is an
// inline of a dozen sixteen-bit stores, and _CODE has two hundred bytes.
static void Msx2_FillOp(u8 x, u16 line, u16 w, u8 h, u8 color, u8 op)
{
	VDP_CommandWait();
	VDP_CommandLMMV(x, line, w, h, color, op);
}
#endif

void Msx2_Fill(u8 x, u8 y, u16 w, u8 h, u8 color)
{
#ifdef MSX2_PLUS
	if(g_yjk)
	{
		u16 line = Msx2_PageY(y);
		Msx2_FillOp(x, line, w, h, 0x07, VDP_OP_AND);
		Msx2_FillOp(x, line, w, h, color, VDP_OP_OR);
		return;
	}
#endif
	VDP_CommandWait();
	VDP_CommandHMMV(x, Msx2_PageY(y), w, h, color);
}

void Msx2_ClearPage(u8 color)
{
	// Clear the offscreen strip as well as the 212 displayed lines -- it is
	// where the card caches live, and leaving boot garbage there makes later
	// bugs unreadable -- but STOP AT LINE 240.  Above it are page 0's sprite
	// tables and page 1's baked font mask, neither of which belongs to a
	// screen: a story transition clearing its page would otherwise erase every
	// glyph the text writer owns and print blank rectangles from then on.
	VDP_CommandWait();
	VDP_CommandHMMV(0, g_draw_page ? 256u : 0u, MSX2_SCREEN_W,
	                MSX2_SPRITE_VRAM_ROW, color);
}

void Msx2_FrameRect(u8 x, u8 y, u16 w, u8 h, u8 color)
{
	Msx2_Fill(x, y, w, 1, color);
	Msx2_Fill(x, (u8)(y + h - 1), w, 1, color);
	Msx2_Fill(x, y, 1, h, color);
	Msx2_Fill((u8)(x + w - 1), y, 1, h, color);
}

// Rows 0..239, not all 256.  The offscreen stashes a scene bakes below the
// visible 212 have to travel with the picture, so the copy cannot stop at 212 --
// but rows 240..250 of page 0 are the sprite pattern, colour and attribute
// tables (msx2_sprite.h), and a page copy over them would take the sprites
// down with it.  Every stash in the port is therefore below row 240.
void Msx2_VideoCopyPage(u8 src, u8 dst)
{
	VDP_CommandWait();
	VDP_CommandHMMM(0, (u16)src << 8, 0, (u16)dst << 8, MSX2_SCREEN_W,
	                MSX2_SPRITE_VRAM_ROW);
}

// One line through the VDP's LINE command.  The direction and major-axis bits
// have to be worked out here because the command takes a length along the major
// axis and a delta along the minor one, not two endpoints.
void Msx2_LineOp(u8 x1, u8 y1, u8 x2, u8 y2, u8 color, u8 op)
{
	u16 sy = Msx2_PageY(y1);
	u16 dx, dy, nx, ny;
	u8 arg = 0;

	if(x1 > x2) { arg |= VDP_ARG_DIX_LEFT;  dx = (u16)(x1 - x2); }
	else        { arg |= VDP_ARG_DIX_RIGHT; dx = (u16)(x2 - x1); }
	if(y1 > y2) { arg |= VDP_ARG_DIY_UP;    dy = (u16)(y1 - y2); }
	else        { arg |= VDP_ARG_DIY_DOWN;  dy = (u16)(y2 - y1); }

	if(dx > dy) { arg |= VDP_ARG_MAJ_H; nx = dx; ny = dy; }
	else        { arg |= VDP_ARG_MAJ_V; nx = dy; ny = dx; }

	VDP_CommandWait();
	VDP_CommandLINE(x1, sy, nx, ny, color, arg, op);
}

void Msx2_Line(u8 x1, u8 y1, u8 x2, u8 y2, u8 color)

{
	Msx2_LineOp(x1, y1, x2, y2, color, VDP_OP_IMP);
}

void Msx2_LineXor(u8 x1, u8 y1, u8 x2, u8 y2, u8 color)
{
	Msx2_LineOp(x1, y1, x2, y2, color, VDP_OP_XOR);
}

void Msx2_CopyRect(u8 sx, u8 sy, u8 dx, u8 dy, u16 w, u8 h)
{
	VDP_CommandWait();
	VDP_CommandHMMM(sx, Msx2_PageY(sy), dx, Msx2_PageY(dy), w, h);
}

void Msx2_TextColor(u8 fg, u8 bg)
{
	g_text_fg = fg;
	g_text_bg = bg;
}

// ── The font mask, and the three commands a string costs ─────────────────────
//
// The row-at-a-time writer above this one was five times faster than MSXgl's
// per-character HMMC, and it was still the single most expensive thing the game
// did: a sampling profile of a duel put 22% of the whole machine inside
// Msx2_TextAt, at 44 ms a string.  The reason is that GRAPHIC 7 makes a glyph
// forty-eight BYTES -- the Z80 expanded every one of them from a bitmap and then
// pushed it through the data port at the 32 T-states a byte the VDP demands
// while it is scanning out, so a line of text was a hundred thousand T-states
// of pure byte-shovelling.
//
// The command engine can move those bytes on its own, and it is four times
// faster at it -- but only if the pixels already exist in VRAM.  So the font is
// BAKED ONCE into the sixteen offscreen lines above page 1, as a MASK: 0xFF
// where the glyph has ink and 0x00 where it does not.  Colour is then pure
// arithmetic on the destination, and it is exact for any pair of colours:
//
//     dest = bg XOR (mask AND (fg XOR bg))
//
// which is one LMMV to lay down fg^bg over the whole string, one LMMM an
// AND per character, and one LMMV to XOR bg back over it.  The CPU writes
// fifteen bytes a command and never touches a pixel; a twenty-character string
// went from 22 ms to under 3, and the command engine does most of that while
// the Z80 has already moved on.
//
// Page 1's lines 240..255 are the one part of VRAM nothing else can want: the
// sprite tables live at the same lines of page 0 (0xF000 up), Msx2_VideoCopyPage
// stops at 240, and Msx2_ClearPage stops there too so a screen change cannot
// wipe the font out from under the writer.

// The CE poll and the register burst, with the same debt to S#0 the polygon
// filler pays: the interrupt handler reads whatever R#15 selects, so S#2 may
// never outlive the DI window that selected it.
//
// THE WAIT IS NOT PART OF THE BURST.
// The two used to share one DI, and the wait is the long half: a caller that
// prints a line straight after a page copy is waiting on an LMMM over 54,000
// pixels, which is tens of milliseconds -- every one of them a V-blank the
// music never got, on a screen that is only being repainted.  The poll now
// closes and reopens the window on each turn, so S#2 still never outlives its
// DI (the ISR reads whatever R#15 selects) but the tune is decoded on time.
// The burst itself is fifteen `out`s and stays inside one window.
static void Msx2_CmdOut(void)
{
	__asm
	00061$:
		di
		ld		a, #2
		out		(#0x99), a
		ld		a, #(15 | 0x80)
		out		(#0x99), a
		in		a, (#0x99)
		push	af
		xor		a
		out		(#0x99), a				// S#0 back before anything else runs
		ld		a, #(15 | 0x80)
		out		(#0x99), a
		pop		af
		rra								// S#2 bit 0 = CE
		jr		nc, 00062$
		ei
		nop								// the ISR is taken here, if pending
		jr		00061$
	00062$:
		// Still inside the DI that read the last status: the engine is idle
		// and the fifteen registers can go out.

		ld		a, #32					// R#17 -> R#32, autoincrementing
		out		(#0x99), a
		ld		a, #(17 | 0x80)
		out		(#0x99), a
		ld		hl, #_g_cmd
		ld		c, #0x9B
		ld		b, #15
		otir
		ei
	__endasm;
}

// Only four of the fifteen registers change between the three commands a
// string costs: the destination x, the width, the colour and the command
// itself.  The rest -- the high halves that are always zero, the eight-line
// height, the source, the destination line -- are written by the caller once
// per string or once at boot, which is the difference between a call SDCC has
// to push six arguments for and one it does not.
static void Msx2_CmdRect(u8 dx, u8 nx, u8 color, u8 cmd)
{
	g_cmd[4] = dx;
	g_cmd[8] = nx;
	g_cmd[12] = color;
	g_cmd[14] = cmd;
	Msx2_CmdOut();
}

// One string, on the draw page: fg^bg down, the glyphs ANDed over it, bg XORed
// back.  Everything after the character count is the command engine's.
void Msx2_TextAt(u8 x, u8 y, const c8* text)
{
	u8  n = 0;
	u8  i;
	u8  dx;
	u16 line;

	// Clip to the screen by whole characters: a glyph running off the right
	// edge would wrap onto the next scanline, which is worse than losing it.
	while((text[n] != 0) && (n < MSX2_TEXT_MAX_CHARS) &&
	      ((u16)x + (u16)(n + 1) * MSX2_FONT_W_PX <= MSX2_SCREEN_W))
		++n;
	if(n == 0)
		return;

	line = (u16)y + ((u16)Msx2_VideoGetDrawPage() << 8);
	g_cmd[6] = (u8)line;
	g_cmd[7] = (u8)(line >> 8);
	// The first pass has nothing to combine with, so it goes through the
	// byte-mode fill rather than the logical one -- same registers, and the
	// command engine paints it half again as fast.
	Msx2_CmdRect(x, (u8)(n * MSX2_FONT_W_PX),
	             (u8)(g_text_fg ^ g_text_bg), VDP_CMD_HMMV);

	dx = x;
	for(i = 0; i < n; ++i)
	{
		u8 g = (u8)((u8)text[i] - MSX2_FONT_FIRST);
		u8 sy = (u8)MSX2_FONT_MASK_LINE;

		if(g >= MSX2_FONT_GLYPHS)
			g = 0;                       // anything unprintable prints as space
		if(g >= MSX2_FONT_MASK_COLS)
		{
			g = (u8)(g - MSX2_FONT_MASK_COLS);
			sy = (u8)(MSX2_FONT_MASK_LINE + MSX2_FONT_H_PX);
		}
		g_cmd[0] = (u8)(g * MSX2_FONT_W_PX);
		g_cmd[2] = sy;
		Msx2_CmdRect(dx, MSX2_FONT_W_PX, 0, VDP_CMD_LMMM | VDP_OP_AND);
		dx = (u8)(dx + MSX2_FONT_W_PX);
	}

	Msx2_CmdRect(x, (u8)(n * MSX2_FONT_W_PX), g_text_bg,
	             VDP_CMD_LMMV | VDP_OP_XOR);
}

u8 Msx2_TextWidth(const c8* text)
{
	u8 n = 0;
	while(*text++)
		++n;
	return (u8)(n * MSX2_FONT_W_PX);
}

void Msx2_TextCenter(u8 y, const c8* text)
{
	u8 w = Msx2_TextWidth(text);
	Msx2_TextAt((u8)((MSX2_SCREEN_W - w) / 2), y, text);
}

void Msx2_NumAt(u8 x, u8 y, i16 value)
{
	c8 buf[7];
	u8 n = 0;
	u16 v;
	u8 i;

	if(value < 0)
	{
		buf[n++] = '-';
		v = (u16)(-value);
	}
	else
		v = (u16)value;

	// Digits come out backwards, so they are laid down from the end of a
	// six-digit field and the string starts wherever they stopped.
	i = 6;
	buf[6] = 0;
	do
	{
		buf[--i] = (c8)('0' + (v % 10));
		v /= 10;
	}
	while(v != 0);
	if(n != 0)
		buf[--i] = '-';
	Msx2_TextAt(x, y, &buf[i]);
}

// The title's big shadowed logo and its outlined prompt used to be drawn here,
// as runs of fills, one glyph at a time.  They were 780 bytes of a code budget
// with nothing left in it, for three lines of text that never change -- so the
// words are painted into the title picture by tools/msx2/gen_msx_scenes.py
// instead, at the same size and with the same shadow, and the blinking prompt
// is a second baked strip streamed into the offscreen rows.  The cartridge has
// room for pictures; the Z80 has none for glyph renderers.
