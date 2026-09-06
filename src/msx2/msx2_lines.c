// ─────────────────────────────────────────────────────────────────────────────
//  msx2_lines.c — the two quad outlines and the XORed frame
//
//  The duel screen's selection furniture, and nothing else in the port calls
//  any of it.  It is compiled into the duel bank (waifu_msx2_s2_b0.c) rather
//  than into _CODE for room: the MSX2+ cartridge needed forty bytes up there
//  for a fill that keeps a pixel's chroma (msx2_video.c), and _CODE had none
//  while segment 2 had a few hundred.
//
//  The LINE command itself stays in _CODE -- a bank may call _CODE, and
//  Msx2_LineOp() is the page-aware half that everything else here goes
//  through.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_video.h"

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

