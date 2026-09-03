// ─────────────────────────────────────────────────────────────────────────────
//  msx2_video.c — GRAPHIC 7 video layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_video.h"

#include "msx2_stream.h"
#include "msx2_sprite.h"
#include "msx2_scenes.h"

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
// A glyph is 6x8 bytes of flat colour in GRAPHIC 7, which is exactly what the
// streamer's own padded write loop already moves.  So a string is built one
// scanline at a time into a RAM row -- the whole string, not one character --
// and each row goes out as a single set-address plus block write.  That is five
// times faster, and it drops MSXgl's print module from the link entirely.
#define MSX2_TEXT_MAX_CHARS  42
static u8 g_text_row[MSX2_TEXT_MAX_CHARS * MSX2_FONT_W_PX];

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
}

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
	// Clear all 256 lines of the page, not just the 212 displayed: the
	// offscreen strip is where the font and card caches will live, and leaving
	// boot garbage there makes later bugs unreadable.
	VDP_CommandWait();
	VDP_CommandHMMV(0, g_draw_page ? 256u : 0u, MSX2_SCREEN_W, 256, color);
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

// One string, on the draw page, a scanline at a time.  Msx2_PokeAt() sets the
// VDP write address for (x, y) on whichever page is being drawn -- the same
// call the card streamer uses -- and Msx2_PokeBlock() pushes the row out with
// the padding GRAPHIC 7 needs while the display is on.
void Msx2_TextAt(u8 x, u8 y, const c8* text)
{
	const u8* patterns = g_msx2_font;
	u8 n = 0;
	u8 row;

	// Clip to the screen by whole characters: a glyph running off the right
	// edge would wrap onto the next scanline, which is worse than losing it.
	while((text[n] != 0) && (n < MSX2_TEXT_MAX_CHARS) &&
	      ((u16)x + (u16)(n + 1) * MSX2_FONT_W_PX <= MSX2_SCREEN_W))
		++n;
	if(n == 0)
		return;

	VDP_CommandWait();
	for(row = 0; row < MSX2_FONT_H_PX; ++row)
	{
		u8* d = g_text_row;
		u8 i;
		for(i = 0; i < n; ++i)
		{
			u8 bits = patterns[(u16)((u8)text[i] - MSX2_FONT_FIRST)
			                   * MSX2_FONT_H_PX + row];
			u8 c;
			for(c = 0; c < MSX2_FONT_W_PX; ++c)
				*d++ = (bits & (u8)(0x80 >> c)) ? g_text_fg : g_text_bg;
		}
		Msx2_PokeAt(x, (u8)(y + row));
		Msx2_PokeBlock(g_text_row, (u8)(n * MSX2_FONT_W_PX));
	}
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
