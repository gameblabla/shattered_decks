// ─────────────────────────────────────────────────────────────────────────────
//  msx2_sprite.c — the V9938 sprite layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_sprite.h"
#include "msx2_video.h"

#include "msx2_scenes.h"

#define SPR_PAT_ADDR   0xF000u
#define SPR_COL_ADDR   0xF800u
#define SPR_ATR_ADDR   0xFA00u
#define SPR_COUNT      32
#define SPR_HIDDEN_Y   217     // past the 212 that display, and not the 216
                               // that would end the sprite list

// One sprite's 32 pattern bytes, and one sprite's 16 colour bytes, assembled
// here before they go out.  Two small buffers rather than a shadow of the whole
// table: nothing ever reads the tables back.
static u8 g_pat[32];
static u8 g_col[16];
static u8 g_atr[4];

static const u16 g_burst[MSX2_SPR_BURST_N][16] =
{
	{ 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0180, 0x02E0, 0x07E0, 0x0380, 0x0180, 0x0200, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
	{ 0x0000, 0x0000, 0x0000, 0x0180, 0x07E0, 0x07E0, 0x1FF0, 0x1FF8, 0x0FF0, 0x0FE0, 0x1FF0, 0x0B80, 0x0440, 0x0000, 0x0000, 0x0000 },
	{ 0x0000, 0x0440, 0x0B80, 0x1FF8, 0x2FF8, 0x7FFC, 0x3FF8, 0x1FFC, 0x3FFE, 0x7FFC, 0x1FF8, 0x1FF8, 0x0FE0, 0x0760, 0x0080, 0x0000 },
	{ 0x0180, 0x0BB8, 0x0FF4, 0x3FFE, 0x3FFC, 0xBEBE, 0x7C3E, 0x783E, 0xF81F, 0xB83F, 0x7E7E, 0x7FFE, 0x1FF8, 0x1FF8, 0x07E0, 0x0260 },
	{ 0x0980, 0x1FD0, 0x2FFC, 0x7E7C, 0xB81C, 0x701D, 0x600E, 0x6006, 0xF00F, 0xF00F, 0x700E, 0x741E, 0x3FB8, 0x1FF8, 0x07E0, 0x0640 },
	{ 0x0640, 0x07E0, 0x1918, 0x3808, 0x6006, 0x6006, 0xC003, 0xC003, 0x4006, 0x6002, 0x4001, 0xA00C, 0x7004, 0x2E3C, 0x1FD0, 0x0980 },
	{ 0x0260, 0x0460, 0x1808, 0x0004, 0x6002, 0x4002, 0x8001, 0x8001, 0x0002, 0x4000, 0x8000, 0x0001, 0x2006, 0x3004, 0x0898, 0x0190 },
	{ 0x0190, 0x0818, 0x2004, 0x2002, 0x0001, 0x8000, 0x0000, 0x0000, 0x8001, 0x8001, 0x0000, 0x4002, 0x0000, 0x0008, 0x0000, 0x0260 },
};

void Msx2_SpriteInit(void)
{
	u8 f;

	// The sixteen palette registers apply to sprites only in GRAPHIC 7, so
	// this palette costs the bitmap nothing.  Colour is (R,G,B) in 3 bits each,
	// packed as MSXgl wants it: 0xRRRB, 0x0GGG.
	VDP_SetPaletteEntry(MSX2_SPR_WHITE, 0x7707);
	VDP_SetPaletteEntry(MSX2_SPR_GOLD,  0x7005);
	VDP_SetPaletteEntry(MSX2_SPR_RED,   0x7000);
	VDP_SetPaletteEntry(MSX2_SPR_TEAL,  0x0305);
	VDP_SetPaletteEntry(MSX2_SPR_BLUE,  0x2306);

	// 16x16, magnified to 32x32: an explosion or a letter has to read at the
	// same size the 88x120 cut-in cards do.
	VDP_SetSpriteFlag(VDP_SPRITE_SIZE_16 | VDP_SPRITE_SCALE_2);
	VDP_EnableSprite(TRUE);

	// THE COMMAND ENGINE FIRST.
	// Msx2_VideoInit() has just cleared both pages with an HMMV, which is
	// 65,536 bytes and still running when this is reached -- and the command
	// engine and the data port share the VDP's VRAM access slot, so every one
	// of these pattern bytes was silently lost.  The sprites were placed, they
	// were coloured, and they were invisible because their patterns were all
	// zero.  Every write below waits for the engine before it starts.
	VDP_CommandWait();

	// EIGHT FRAMES OF A BURST, as sixteen row masks each.
	// A ring that opens, breaks up and blows apart: the outer edge is ragged
	// and the inside empties out from frame three, so what plays is an
	// explosion rather than a circle getting bigger.  They are a table because
	// the arithmetic that drew them (a distance per pixel, and a hash for the
	// ragged edge) costs more code than the 256 bytes it would save.
	for(f = 0; f < MSX2_SPR_BURST_N; ++f)
	{
		u8 y;
		for(y = 0; y < 16; ++y)
		{
			u16 bits = g_burst[f][y];
			// A 16x16 pattern is four 8x8 quarters: top-left, bottom-left,
			// then top-right, bottom-right.
			u8 q = (u8)((y & 7) + ((y & 8) ? 8 : 0));
			g_pat[q]      = (u8)(bits >> 8);
			g_pat[16 + q] = (u8)(bits & 0xFF);
		}
		VDP_WriteVRAM(g_pat,
		              (u16)(SPR_PAT_ADDR + (u16)(MSX2_SPR_BURST0 + f) * 32),
		              0, 32);
	}

	Msx2_SpriteClear();
}

void Msx2_SpriteClear(void)
{
	u8 i;
	for(i = 0; i < SPR_COUNT; ++i)
		Msx2_SpriteHide(i);
}

void Msx2_SpriteHide(u8 id)
{
	VDP_CommandWait();
	g_atr[0] = SPR_HIDDEN_Y;
	VDP_WriteVRAM(g_atr, (u16)(SPR_ATR_ADDR + (u16)id * 4), 0, 1);
}

void Msx2_SpriteAt(u8 id, u8 x, u8 y, u8 slot, u8 color)
{
	u8 i;

	VDP_CommandWait();
	for(i = 0; i < 16; ++i)
		g_col[i] = color;
	VDP_WriteVRAM(g_col, (u16)(SPR_COL_ADDR + (u16)id * 16), 0, 16);

	// The VDP puts a sprite one line below its Y, so a Y of 255 is line 0.
	g_atr[0] = (u8)(y - 1);
	g_atr[1] = x;
	g_atr[2] = (u8)(slot * 4);
	g_atr[3] = 0;
	VDP_WriteVRAM(g_atr, (u16)(SPR_ATR_ADDR + (u16)id * 4), 0, 4);
}

u8 Msx2_SpriteWord(const c8* text)
{
	const u8* patterns = g_msx2_font;
	u8 n = 0;

	VDP_CommandWait();
	while((text[n] != 0) && (n < MSX2_SPR_LETTER_N))
	{
		const u8* glyph = patterns + (u16)((u8)text[n] - MSX2_FONT_FIRST) * 8;
		u8 y;
		// A 6x8 glyph doubled is 12x16, which sits inside the 16x16 cell with
		// two blank columns each side: every pixel becomes a 2x2 block, so the
		// letter is as chunky as the 32x32 magnification wants it.
		for(y = 0; y < 8; ++y)
		{
			u8 bits = glyph[y];
			u16 wide = 0;
			u8 c;
			for(c = 0; c < 6; ++c)
				if(bits & (u8)(0x80 >> c))
					wide |= (u16)(0xC000u >> (c * 2));
			wide >>= 2;                      // centre the 12 columns in 16
			g_pat[y * 2]      = (u8)(wide >> 8);
			g_pat[y * 2 + 1]  = (u8)(wide >> 8);
			g_pat[16 + y * 2]     = (u8)(wide & 0xFF);
			g_pat[16 + y * 2 + 1] = (u8)(wide & 0xFF);
		}
		VDP_WriteVRAM(g_pat,
		              (u16)(SPR_PAT_ADDR + (u16)(MSX2_SPR_LETTER0 + n) * 32),
		              0, 32);
		++n;
	}
	return n;
}

void Msx2_SpriteShowWord(const c8* text, u8 y, u8 color)
{
	u8 n = Msx2_SpriteWord(text);
	u8 x = (u8)((MSX2_SCREEN_W - (u16)n * 26) / 2);
	u8 i;

	for(i = 0; i < n; ++i)
	{
		if(text[i] == ' ')
			Msx2_SpriteHide(i);
		else
			Msx2_SpriteAt(i, (u8)(x + i * 26), y,
			              (u8)(MSX2_SPR_LETTER0 + i), color);
	}
	for(; i < MSX2_SPR_LETTER_N; ++i)
		Msx2_SpriteHide(i);
}
