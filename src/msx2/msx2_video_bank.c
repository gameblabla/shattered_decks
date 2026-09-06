// ─────────────────────────────────────────────────────────────────────────────
//  msx2_video_bank.c — cold video helpers in a page-0 bank
//
//  It runs once, at boot, and _CODE is full: sixty-four glyphs' worth of bit
//  expansion has no business holding resident address space for the rest of the
//  game.  Everything it calls -- Msx2_PokeAtLine, Msx2_PokeBlock -- is in
//  _CODE, which msx2_bank.h's one rule allows.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_video.h"
#include "msx2_stream.h"

static u8 g_font_row[MSX2_FONT_W_PX];

#if defined(MSX2_ASCII16X) && defined(MSX2_PLUS)

// SCREEN 10 fills need two logical VDP commands to preserve the YJK chroma
// bits.  This path is used by menus and story composition, so it belongs with
// the modal screen code; the fixed Msx2_Fill entry only performs the bank turn.
static void Msx2_FillOp_In(u8 x, u16 line, u16 w, u8 h, u8 color, u8 op)
{
	VDP_CommandWait();
	VDP_CommandLMMV(x, line, w, h, color, op);
}

void Msx2_Fill_In(u8 x, u8 y, u16 w, u8 h, u8 color)
{
	u16 line = (u16)y + (Msx2_VideoGetDrawPage() ? 256u : 0u);

	if(Msx2_VideoIsYjk())
	{
		Msx2_FillOp_In(x, line, w, h, 0x07, VDP_OP_AND);
		Msx2_FillOp_In(x, line, w, h, color, VDP_OP_OR);
		return;
	}
	VDP_CommandWait();
	VDP_CommandHMMV(x, line, w, h, color);
}

#endif

void Msx2_VideoBakeFont_In(void)
{
	const u8* bits = g_msx2_font;
	u8  g;
	u8  col = 0;
	u16 band = MSX2_FONT_MASK_LINE;

	for(g = 0; g < MSX2_FONT_GLYPHS; ++g)
	{
		u8 row;

		for(row = 0; row < MSX2_FONT_H_PX; ++row)
		{
			u8 b = *bits++;
			u8 c;

			// 0xFF is every bit of the colour the AND pass will let through;
			// 0x00 is none of it.
			for(c = 0; c < MSX2_FONT_W_PX; ++c)
			{
				g_font_row[c] = (b & 0x80) ? 0xFF : 0x00;
				b = (u8)(b << 1);
			}
			Msx2_PokeAtLine(col, (u16)(band + row));
			Msx2_PokeBlock(g_font_row, MSX2_FONT_W_PX);
		}
		col = (u8)(col + MSX2_FONT_W_PX);
		if(col >= MSX2_FONT_MASK_COLS * MSX2_FONT_W_PX)
		{
			col = 0;
			band = (u16)(band + MSX2_FONT_H_PX);
		}
	}
}
