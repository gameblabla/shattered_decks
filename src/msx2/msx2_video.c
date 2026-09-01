// ─────────────────────────────────────────────────────────────────────────────
//  msx2_video.c — GRAPHIC 7 video layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_video.h"

// An 8x8 bitmap font from MSXgl's content set.  The shipping game wants its own
// outlined font strip in offscreen VRAM (plan §7.2); this is the bring-up one.
#include "font/font_mgl_sample6.h"

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

	Print_SetBitmapFont(g_Font_MGL_Sample6);
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

// All 256 rows, so the offscreen stashes a scene bakes below the visible 212
// travel with the picture and the second buffer is a true duplicate.
void Msx2_VideoCopyPage(u8 src, u8 dst)
{
	VDP_CommandWait();
	VDP_CommandHMMM(0, (u16)src << 8, 0, (u16)dst << 8, MSX2_SCREEN_W, 256);
}

// One line through the VDP's LINE command.  The direction and major-axis bits
// have to be worked out here because the command takes a length along the major
// axis and a delta along the minor one, not two endpoints.
static void Msx2_Line(u8 x1, u8 y1, u8 x2, u8 y2, u8 color)
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
	VDP_CommandLINE(x1, sy, nx, ny, color, arg, VDP_OP_IMP);
}

void Msx2_QuadOutline(const u8* quad, u8 color)
{
	u8 i;
	for(i = 0; i < 4; ++i)
	{
		u8 j = (u8)((i + 1) & 3);
		Msx2_Line(quad[i * 2], quad[i * 2 + 1], quad[j * 2], quad[j * 2 + 1],
		          color);
	}
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
	Print_SetColor(fg, bg);
}

// MSXgl's bitmap printer draws with the command engine (HMMC), whose Y axis
// spans both pages, so text reaches the hidden page by adding 256 -- no R#2
// fiddling, and no risk of briefly displaying the page being composed.
void Msx2_TextAt(u8 x, u8 y, const c8* text)
{
	VDP_CommandWait();
	Print_SetPosition(x, Msx2_PageY(y));
	Print_DrawText(text);
}

u8 Msx2_TextWidth(const c8* text)
{
	u8 n = 0;
	while(*text++)
		++n;
	return (u8)(n * 6);   // font_mgl_sample6 is 6 pixels wide
}

void Msx2_TextCenter(u8 y, const c8* text)
{
	u8 w = Msx2_TextWidth(text);
	Msx2_TextAt((u8)((MSX2_SCREEN_W - w) / 2), y, text);
}

void Msx2_NumAt(u8 x, u8 y, i16 value)
{
	VDP_CommandWait();
	Print_SetPosition(x, Msx2_PageY(y));
	Print_DrawInt(value);
}

// ── Double-size text ─────────────────────────────────────────────────────────
//
// The MSXgl font header is 4 bytes: [data size x|y], [font size x|y], first
// character, last character, followed by 8 bytes of 1bpp pattern per character
// with the leftmost pixel in bit 7.  Reading it here rather than hardcoding 6x8
// means swapping in the shipping font later changes nothing else.

#define MSX2_FONT_W 6
#define MSX2_FONT_H 8
#define MSX2_BIG_W  (MSX2_FONT_W * 2)
#define MSX2_BIG_H  (MSX2_FONT_H * 2)

u8 Msx2_TextBigWidth(const c8* text)
{
	u8 n = 0;
	while(*text++)
		++n;
	return (u8)(n * MSX2_BIG_W);
}

// Draw one 2x-scaled glyph as horizontal runs of lit pixels, so whatever is
// already on the page shows through the gaps.  A 6x8 glyph is at most three
// runs per row, so a character costs about two dozen fills.
static void Msx2_BigGlyphRuns(u8 x, u8 y, const u8* glyph, u8 color)
{
	u8 row;
	for(row = 0; row < MSX2_FONT_H; ++row)
	{
		u8 bits = glyph[row];
		u8 col = 0;
		while(col < MSX2_FONT_W)
		{
			if(bits & (u8)(0x80 >> col))
			{
				u8 start = col;
				while((col < MSX2_FONT_W) && (bits & (u8)(0x80 >> col)))
					++col;
				Msx2_Fill((u8)(x + start * 2), (u8)(y + row * 2),
				          (u16)((col - start) * 2), 2, color);
			}
			else
				++col;
		}
	}
}

void Msx2_TextBigShadow(u8 y, const c8* text, u8 fg, u8 shadow)
{
	const u8* font = g_Font_MGL_Sample6;
	u8 first = font[2];
	const u8* patterns = font + 4;
	u8 pass;

	// Shadow first, then the face over it: two passes over the string rather
	// than two per glyph, so a letter never shadows the one before it.
	for(pass = 0; pass < 2; ++pass)
	{
		u8 x = (u8)((MSX2_SCREEN_W - Msx2_TextBigWidth(text)) / 2);
		const c8* p = text;
		while(*p)
		{
			const u8* glyph = patterns + (u16)((u8)*p - first) * MSX2_FONT_H;
			if(pass == 0)
				Msx2_BigGlyphRuns((u8)(x + 2), (u8)(y + 2), glyph, shadow);
			else
				Msx2_BigGlyphRuns(x, y, glyph, fg);
			x = (u8)(x + MSX2_BIG_W);
			++p;
		}
	}
}

// ── Outlined text ────────────────────────────────────────────────────────────
//
// The glyph is 6x8 with its pixels in bits 7-2 of each row byte; shifting right
// by one puts them in bits 6-1, which leaves a spare bit on each side for the
// outline to grow into.  The outline is an 8-connected dilation minus the glyph
// itself, so it is exactly one pixel all round.

#define MSX2_OUTLINE_ROWS  (MSX2_FONT_H + 2)

static u8 Msx2_GlyphRow(const u8* glyph, u8 row)
{
	// Row 0 and row 9 are the empty margins the outline lives in.
	if((row == 0) || (row > MSX2_FONT_H))
		return 0;
	return (u8)(glyph[row - 1] >> 1);
}

static u8 Msx2_Spread(u8 bits)
{
	return (u8)(bits | (u8)(bits << 1) | (u8)(bits >> 1));
}

// Emit one row of a bitmask as horizontal fills.  Bit 7 is the leftmost pixel.
static void Msx2_MaskRuns(u8 x, u8 y, u8 bits, u8 color)
{
	u8 col = 0;
	while(col < 8)
	{
		if(bits & (u8)(0x80 >> col))
		{
			u8 start = col;
			while((col < 8) && (bits & (u8)(0x80 >> col)))
				++col;
			Msx2_Fill((u8)(x + start), (u8)(y), (u16)(col - start), 1, color);
		}
		else
			++col;
	}
}

void Msx2_TextOutline(u8 x, u8 y, const c8* text, u8 fg, u8 outline)
{
	const u8* font = g_Font_MGL_Sample6;
	u8 first = font[2];
	const u8* patterns = font + 4;

	// The whole string is outlined before any of it is filled, or a letter
	// would outline over the face of the one before it.
	u8 pass;
	for(pass = 0; pass < 2; ++pass)
	{
		u8 cx = x;
		const c8* p = text;
		while(*p)
		{
			const u8* glyph = patterns + (u16)((u8)*p - first) * MSX2_FONT_H;
			u8 row;
			for(row = 0; row < MSX2_OUTLINE_ROWS; ++row)
			{
				u8 face = Msx2_GlyphRow(glyph, row);
				u8 bits;
				if(pass == 0)
				{
					bits = (u8)((Msx2_Spread(Msx2_GlyphRow(glyph, (u8)(row - 1)))
					           | Msx2_Spread(face)
					           | Msx2_Spread(Msx2_GlyphRow(glyph, (u8)(row + 1))))
					           & (u8)~face);
				}
				else
					bits = face;
				if(bits != 0)
					Msx2_MaskRuns((u8)(cx - 1), (u8)(y - 1 + row), bits,
					              (pass == 0) ? outline : fg);
			}
			cx = (u8)(cx + MSX2_FONT_W);
			++p;
		}
	}
}

void Msx2_TextOutlineCenter(u8 y, const c8* text, u8 fg, u8 outline)
{
	Msx2_TextOutline((u8)((MSX2_SCREEN_W - Msx2_TextWidth(text)) / 2), y,
	                 text, fg, outline);
}
