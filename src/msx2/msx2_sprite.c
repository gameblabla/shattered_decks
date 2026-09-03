// ─────────────────────────────────────────────────────────────────────────────
//  msx2_sprite.c — the V9938 sprite layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_sprite.h"
#include "msx2_video.h"

#include "msx2_scenes.h"
#include "msx2_stream.h"

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
	// The selector's bands.  Two steps either side of the flat colour, far
	// enough apart that eight faces of a turning solid separate at 20 pixels
	// square, and all three still read as the one gem.
	VDP_SetPaletteEntry(MSX2_SPR_RED_DK,  0x3000);
	VDP_SetPaletteEntry(MSX2_SPR_RED_HI,  0x7405);
	VDP_SetPaletteEntry(MSX2_SPR_TEAL_DK, 0x0102);
	VDP_SetPaletteEntry(MSX2_SPR_TEAL_HI, 0x4607);

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

	// THE PATTERNS, out of the cartridge.
	// Eight frames of the burst, then the selector's eight frames of
	// MSX2_GEM_PLANES planes each -- baked by tools/msx2/gen_msx_scenes.py in
	// the V9938's own quarter layout, so this is a read and a write and no
	// shuffling at all.
	// They are cartridge data for the same reason the interface strings are:
	// _CODE is 32 KB and there are six megabytes on the other side of the
	// mapper.
	for(f = 0; f < (MSX2_SPR_BURST_N + MSX2_SPR_GEM_N); ++f)
	{
		Msx2_RomRead(MSX2_SPRITE_PAT_SEGMENT,
		             (u16)((u16)f * MSX2_SPRITE_PAT_BYTES), g_pat,
		             MSX2_SPRITE_PAT_BYTES);
		VDP_WriteVRAM(g_pat, (u16)(SPR_PAT_ADDR + (u16)f * 32), 0, 32);
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

// SLIDING ON FROM OFF THE SCREEN.
// A sprite's X is one unsigned byte, so there is no such place as -20 -- and a
// word that simply appeared at the left edge would pop rather than arrive.
// The V9938 has the answer in the sprite's own colour byte: bit 7, "early
// clock", displays that sprite thirty-two pixels further left than its X says.
// So a letter between -32 and -1 is placed at x + 32 with EC set, which is
// exactly the partial letter the hardware clips against the border for us, and
// anything further out is simply not on the screen yet.
#define SPR_EARLY_CLOCK 0x80

void Msx2_SpriteWordAt(const c8* text, u8 n, i16 x, u8 y, u8 color)
{
	u8 i;

	for(i = 0; i < n; ++i)
	{
		i16 lx = (i16)(x + (i16)((u16)i * MSX2_SPR_WORD_PITCH));

		if(text[i] == ' ')
			Msx2_SpriteHide(i);
		else if(lx >= 0)
		{
			if(lx > 255)
				Msx2_SpriteHide(i);
			else
				Msx2_SpriteAt(i, (u8)lx, y, (u8)(MSX2_SPR_LETTER0 + i), color);
		}
		else if(lx >= -32)
			Msx2_SpriteAt(i, (u8)(lx + 32), y, (u8)(MSX2_SPR_LETTER0 + i),
			              (u8)(color | SPR_EARLY_CLOCK));
		else
			Msx2_SpriteHide(i);
	}
	for(; i < MSX2_SPR_LETTER_N; ++i)
		Msx2_SpriteHide(i);
}

void Msx2_SpriteShowWord(const c8* text, u8 y, u8 color)
{
	u8 n = Msx2_SpriteWord(text);
	i16 x = (i16)((MSX2_SCREEN_W - (u16)n * MSX2_SPR_WORD_PITCH) / 2);

	Msx2_SpriteWordAt(text, n, x, y, color);
}

// The three tints of each cursor colour, darkest first, indexed by the flat
// colour the board asks for.  A table rather than a pair of branches: the gem
// is placed every frame the cursor is on the screen.
static const u8 g_gem_tint[2][MSX2_GEM_PLANES] =
{
	{ MSX2_SPR_RED_DK,  MSX2_SPR_RED,  MSX2_SPR_RED_HI  },
	{ MSX2_SPR_TEAL_DK, MSX2_SPR_TEAL, MSX2_SPR_TEAL_HI },
};

void Msx2_SpriteGem(u8 x, u8 y, u8 frame, u8 color)
{
	// One sprite a shade, all at the same place: the planes are disjoint masks
	// of one solid, so what the viewer sees is a single shaded gem and not
	// three sprites on top of each other.
	const u8* tint = g_gem_tint[(color == MSX2_SPR_TEAL) ? 1 : 0];
	u8 slot = (u8)(MSX2_SPR_GEM0 +
	               (frame & (MSX2_GEM_FRAMES - 1)) * MSX2_GEM_PLANES);
	u8 p;

	for(p = 0; p < MSX2_GEM_PLANES; ++p)
		Msx2_SpriteAt((u8)(MSX2_SPR_CURSOR + p), x, y, (u8)(slot + p), tint[p]);
}

void Msx2_SpriteHideGem(void)
{
	u8 p;
	for(p = 0; p < MSX2_GEM_PLANES; ++p)
		Msx2_SpriteHide((u8)(MSX2_SPR_CURSOR + p));
}
