// ─────────────────────────────────────────────────────────────────────────────
//  msx2_sprite.c — the V9938 sprite layer
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_sprite.h"
#include "msx2_video.h"

#include "msx2_scenes.h"
#include "msx2_stream.h"
#ifdef MSX2_DEBUG_REGRESSION
#include "msx2_probe.h"
#endif

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

static void Msx2_SpriteBuildSlash(void);
static void Msx2_SpriteBuildBurn(void);

void Msx2_SpriteInit(void)
{
	u8 f;

	// The bitmap is direct colour in GRAPHIC 7, so these registers cost it
	// nothing -- and, as msx2_sprite.h sets out, they do not reach the sprite
	// plane in this mode either: the chip colours a GRAPHIC 7 sprite from a
	// fixed table.  They are kept because they are the intent, and because a
	// mode that is not GRAPHIC 7 would honour them.  Colour is (R,G,B) in 3 bits each,
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
	VDP_SetPaletteEntry(MSX2_SPR_ORANGE,  0x7003);

	// THE TABLES, by hand.
	// MSXgl's sprite module is compiled out (VDP_USE_SPRITE is FALSE in
	// msxgl_config.h): it cost a kilobyte of _CODE for one register write
	// this file used, since the game assembles every attribute, colour and
	// pattern byte itself and writes it with VDP_WriteVRAM.  So the three
	// registers VDP_SetModeGraphic7 used to program are written here, once,
	// after Msx2_VideoInit has set the mode; nothing else writes them.
	// R#5/R#11 hold the attribute table (SPR_ATR_ADDR >> 7, low three bits
	// set as sprite mode 2 requires, and >> 15); R#6 the pattern table
	// (SPR_PAT_ADDR >> 11).  The colour table is fixed by the chip at the
	// attribute table minus 0x200 -- which is SPR_COL_ADDR.
	VDP_RegWrite(5,  (u8)(SPR_ATR_ADDR >> 7) | 0x07);
	VDP_RegWrite(11, (u8)(SPR_ATR_ADDR >> 15));
	VDP_RegWrite(6,  (u8)(SPR_PAT_ADDR >> 11));

	// 16x16, magnified to 32x32: an explosion or a letter has to read at the
	// same size the 88x120 cut-in cards do.
	VDP_RegWriteBakMask(1, (u8)~(R01_ST | R01_MAG), R01_ST | R01_MAG);
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

	Msx2_SpriteBuildSlash();
	Msx2_SpriteBuildBurn();
	Msx2_SpriteClear();
}

void Msx2_SpriteClear(void)
{
	u8 i;
	for(i = 0; i < SPR_COUNT; ++i)
		Msx2_SpriteHide(i);
#ifdef MSX2_DEBUG_REGRESSION
	g_msx2_regression_diag.gem_visible = FALSE;
#endif
}

void Msx2_SpriteTransitionBegin(void)
{
	Msx2_SpriteHideGem();
	Msx2_SpriteSlashHide();
	Msx2_SpriteBurnHide();
	VDP_CommandWait();
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

// ── The blade sweep ─────────────────────────────────────────────────────────
//
// THE ATTACK IS SPRITES NOW, AND THAT IS THE WHOLE OF WHY IT IS FAST.
// The sweep used to be seven V9938 LINE commands a pose drawn INTO the bitmap,
// which meant every pose had to start by levelling the hidden page from the
// other one -- a 256x240 HMMM, sixty thousand pixels, once a frame, for sixteen
// frames.  That is what made an attack crawl, and levelling one page from
// another that is one pose behind is also what made it flicker.  As sprites the
// same figure is twenty bytes a segment and nothing underneath it is ever
// touched, so there is nothing to put back and no page to level.
//
// One stroke is MSX2_SPR_SLASH_SEGS cells of 32x32, stacked at exactly 32 rows
// and stepped SLASH_DX across.  The bar inside a cell drifts the same distance
// over its sixteen pattern rows, so the four cells join into one unbroken cut.
#define SLASH_DX      18       // screen pixels a segment is offset from the last
#define SLASH_THICK    4       // pattern columns; doubled by the magnification

static void Msx2_SpriteBuildSlash(void)
{
	u8 r;

	VDP_CommandWait();
	for(r = 0; r < 16; ++r)
	{
		// The bar's left column at this row: it crosses SLASH_DX/2 pattern
		// columns over the sixteen rows, which is the offset to the next cell.
		u8 c = (u8)(3 + (u8)(((u16)r * (SLASH_DX / 2)) / 16));
		u16 bar = 0;
		u8 i;

		for(i = 0; i < SLASH_THICK; ++i)
			bar |= (u16)(0x8000u >> (c + i));
		g_pat[r]      = (u8)(bar >> 8);
		g_pat[16 + r] = (u8)(bar & 0xFF);
	}
	VDP_WriteVRAM(g_pat, (u16)(SPR_PAT_ADDR + (u16)MSX2_SPR_SLASH_R * 32),
	              0, 32);
	// The mirror, column by column, for the stroke that leans the other way.
	for(r = 0; r < 16; ++r)
	{
		u16 bar = (u16)(((u16)g_pat[r] << 8) | g_pat[16 + r]);
		u16 flip = 0;
		u8 i;
		for(i = 0; i < 16; ++i)
			if(bar & (u16)(0x8000u >> i))
				flip |= (u16)(1u << i);
		g_pat[r]      = (u8)(flip >> 8);
		g_pat[16 + r] = (u8)(flip & 0xFF);
	}
	VDP_WriteVRAM(g_pat, (u16)(SPR_PAT_ADDR + (u16)MSX2_SPR_SLASH_L * 32),
	              0, 32);
}

static void Msx2_SpriteBuildBurn(void)
{
	u8 i;

	for(i = 0; i < 32; ++i)
		g_pat[i] = 0xFF;
	VDP_CommandWait();
	VDP_WriteVRAM(g_pat,
	              (u16)(SPR_PAT_ADDR + (u16)MSX2_SPR_BURN_PATTERN * 32),
	              0, 32);
}

// Is this segment inside the innermost `grown` of the stroke?
#define SEG_IN(seg, grown) \
	(((seg) >= (u8)((MSX2_SPR_SLASH_SEGS - (grown)) / 2)) && \
	 ((seg) <  (u8)((MSX2_SPR_SLASH_SEGS + (grown) + 1) / 2)))

// The heat of a segment by how far out from the crossing point it is: the ends
// are the cooling tail of the cut and the middle is where the blade bit.
static u8 Msx2_SpriteSlashHeat(u8 seg, u8 core)
{
	if((seg == 0) || (seg == MSX2_SPR_SLASH_SEGS - 1))
		return MSX2_SPR_RED;
	return core;
}

void Msx2_SpriteSlash(u8 x, u8 y, u8 grown_a, u8 grown_b, u8 core)
{
	// The stroke's own half-width and half-height, in screen pixels.  A cell is
	// 32 tall, so four of them reach 64 either side of the middle.
	const u8 half_h = (u8)(MSX2_SPR_SLASH_SEGS * 16);
	const u8 half_w = (u8)((MSX2_SPR_SLASH_SEGS - 1) * SLASH_DX / 2);
	u8 seg;

	// A stroke opens from the middle out and withdraws the same way, so the
	// visible segments are the `grown` innermost ones rather than the first
	// `grown`.  Taking them off the bottom up instead left the cut as a V while
	// the burst was at its widest, which reads as half a picture.
	for(seg = 0; seg < MSX2_SPR_SLASH_SEGS; ++seg)
	{
		u8 sy = (u8)(y - half_h + seg * 32);
		u8 id = (u8)(MSX2_SPR_SLASH_ID + seg);

		if(SEG_IN(seg, grown_a))
			Msx2_SpriteAt(id,
			              (u8)(x - half_w + seg * SLASH_DX - 16), sy,
			              MSX2_SPR_SLASH_R, Msx2_SpriteSlashHeat(seg, core));
		else
			Msx2_SpriteHide(id);

		id = (u8)(MSX2_SPR_SLASH_ID + MSX2_SPR_SLASH_SEGS + seg);
		if(SEG_IN(seg, grown_b))
			Msx2_SpriteAt(id,
			              (u8)(x + half_w - seg * SLASH_DX - 16), sy,
			              MSX2_SPR_SLASH_L, Msx2_SpriteSlashHeat(seg, core));
		else
			Msx2_SpriteHide(id);
	}
}

void Msx2_SpriteSlashHide(void)
{
	u8 i;
	for(i = 0; i < (MSX2_SPR_SLASH_SEGS * 2); ++i)
		Msx2_SpriteHide((u8)(MSX2_SPR_SLASH_ID + i));
}

void Msx2_SpriteBurnCard(u8 which, u8 x, u8 y, u8 step)
{
	u8 base = which ? MSX2_SPR_BURN_CARD_IDS : 0;
	// Code 0 is the only black the fixed GRAPHIC 7 sprite table has, and a
	// sprite only draws it while colour 0 is not transparent.
	VDP_EnableTransparency(FALSE);
	u8 rows = (u8)((step * 4 + 5) / 6);
	u8 row, col;

	if(rows > 4)
		rows = 4;
	for(row = 0; row < 4; ++row)
		for(col = 0; col < 3; ++col)
		{
			u8 id = (u8)(base + row * 3 + col);
			if(row < rows)
				Msx2_SpriteAt(id, (u8)(x + col * 28),
				              (u8)(y + row * 32),
				              MSX2_SPR_BURN_PATTERN, MSX2_SPR_BLACK);
			else
				Msx2_SpriteHide(id);
		}
}

void Msx2_SpriteBurnHide(void)
{
	u8 i;
	for(i = 0; i < MSX2_SPR_BURN_CARD_IDS * 2; ++i)
		Msx2_SpriteHide(i);
	// Colour 0 goes back to meaning transparent the moment the wipe is gone.
	VDP_EnableTransparency(TRUE);
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

#ifdef MSX2_PLUS
// THE SAME GEM, NAMED OUT OF THE PALETTE.
// GRAPHIC 7 is the one mode whose sprites come from the chip's own fixed
// sixteen colours; in SCREEN 10 the plane reads the PALETTE REGISTERS instead
// (openMSX picks palBg for every mode but GRAPHIC7, and the V9958 does the
// same).  Entries 8..15 of that palette are fitted to whichever painting is on
// the screen (tools/msx2/msx2_yjk.py), so the fixed-table indices above name
// nothing stable there: the sanctum road's selector took a different colour on
// every backdrop.  These are the same three bands out of the interface's own
// half of the palette, which every SCREEN 10 picture carries unchanged.
// THE SHADOW BAND MAY NOT BE THE PANEL'S OWN NAVY.
// Entry 1 is what every list panel is filled with, and the selector always
// stands ON a panel -- so a shadow band painted in 1 was the panel again and
// the gem lost the whole of its lit-from-the-left half.  Entry 4, dark sand,
// is the only other dark ink the interface's half of the palette owns, and it
// reads against navy at 2x magnification.
static const u8 g_gem_tint_yjk[2][MSX2_GEM_PLANES] =
{
	{ 4, 6, 2 },        // shadow dark sand, red, gold highlight
	{ 4, 5, 7 },        // shadow dark sand, teal, white highlight
};
#endif

void Msx2_SpriteGem(u8 x, u8 y, u8 frame, u8 color)
{
	// One sprite a shade, all at the same place: the planes are disjoint masks
	// of one solid, so what the viewer sees is a single shaded gem and not
	// three sprites on top of each other.
	u8 which = (u8)((color == MSX2_SPR_TEAL) ? 1 : 0);
#ifdef MSX2_PLUS
	const u8* tint = Msx2_VideoIsYjk() ? g_gem_tint_yjk[which]
	                                   : g_gem_tint[which];
#else
	const u8* tint = g_gem_tint[which];
#endif
	u8 slot = (u8)(MSX2_SPR_GEM0 +
	               (frame & (MSX2_GEM_FRAMES - 1)) * MSX2_GEM_PLANES);
	u8 p;

	for(p = 0; p < MSX2_GEM_PLANES; ++p)
		Msx2_SpriteAt((u8)(MSX2_SPR_CURSOR + p), x, y, (u8)(slot + p), tint[p]);
#ifdef MSX2_DEBUG_REGRESSION
	g_msx2_regression_diag.gem_visible = TRUE;
	g_msx2_regression_diag.gem_x = x;
	g_msx2_regression_diag.gem_y = y;
#endif
}

void Msx2_SpriteHideGem(void)
{
	u8 p;
	for(p = 0; p < MSX2_GEM_PLANES; ++p)
		Msx2_SpriteHide((u8)(MSX2_SPR_CURSOR + p));
#ifdef MSX2_DEBUG_REGRESSION
	g_msx2_regression_diag.gem_visible = FALSE;
#endif
}
