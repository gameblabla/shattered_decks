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

void Msx2_VideoInit(void)
{
	VDP_SetMode(VDP_MODE_GRAPHIC7);
	VDP_SetColor(MSX2_BLACK);
	VDP_EnableVBlank(TRUE);

	Msx2_TextColor(MSX2_WHITE, MSX2_BLACK);

	// Both pages start black, so a flip can never reveal boot garbage.
	g_flip_pending = FALSE;
	g_draw_page = MSX2_PAGE_1;
	Msx2_ClearPage(MSX2_BLACK);
	g_draw_page = MSX2_PAGE_0;
	Msx2_ClearPage(MSX2_BLACK);
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

void Msx2_Fill(u8 x, u8 y, u16 w, u8 h, u8 color)
{
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

void Msx2_FrameRectXor(u8 x, u8 y, u16 w, u8 h, u8 color)
{
	Msx2_LineXor(x, y, (u8)(x + w - 1), y, color);
	Msx2_LineXor(x, (u8)(y + h - 1), (u8)(x + w - 1),
	             (u8)(y + h - 1), color);
	if(h > 2)
	{
		Msx2_LineXor(x, (u8)(y + 1), x, (u8)(y + h - 2), color);
		Msx2_LineXor((u8)(x + w - 1), (u8)(y + 1),
		             (u8)(x + w - 1), (u8)(y + h - 2), color);
	}
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
static void Msx2_LineOp(u8 x1, u8 y1, u8 x2, u8 y2, u8 color, u8 op)
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

static void Msx2_QuadOutlineOp(const u8* quad, u8 color, u8 op)
{
	u8 i;
	for(i = 0; i < 4; ++i)
	{
		u8 j = (u8)((i + 1) & 3);
		Msx2_LineOp(quad[i * 2], quad[i * 2 + 1],
		            quad[j * 2], quad[j * 2 + 1], color, op);
	}
}

void Msx2_QuadOutline(const u8* quad, u8 color)
{
	Msx2_QuadOutlineOp(quad, color, VDP_OP_IMP);
}

void Msx2_QuadOutlineXor(const u8* quad, u8 color)
{
	Msx2_QuadOutlineOp(quad, color, VDP_OP_XOR);
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
static void Msx2_CmdOut(void)
{
	__asm
		di
		ld		a, #2
		out		(#0x99), a
		ld		a, #(15 | 0x80)
		out		(#0x99), a
	00060$:
		in		a, (#0x99)
		rra								// S#2 bit 0 = CE
		jr		c, 00060$
		xor		a
		out		(#0x99), a
		ld		a, #(15 | 0x80)
		out		(#0x99), a

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
