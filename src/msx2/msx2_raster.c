// ─────────────────────────────────────────────────────────────────────────────
//  msx2_raster.c — one card, mapped into its projected quad
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_raster.h"
#include "msx2_video.h"
#include "msx2_stream.h"
#include "msx2_scenes.h"

// The card being drawn.  It lives in RAM for the duration: a texel fetch is
// then an ordinary `ld a,(hl)`, so the inner loop never touches the mapper and
// never pays a bank switch per row.
static u8 g_tex[MSX2_CARD_W * MSX2_CARD_H];

// Which texture is already in g_tex, so a repaint that redraws the same card --
// the common case, once per page -- skips the 1,920-byte read.  0xFFFF is
// "nothing".
static u16 g_tex_loaded;
static u8  g_view;

#define TEX_KEY(card, def)  (u16)(((u16)(card) << 1) | (def))
#define TEX_NONE            0xFFFF

void Msx2_RasterInit(void)
{
	g_tex_loaded = TEX_NONE;
	g_view = MSX2_VIEW_TOP;
}

void Msx2_RasterSetView(u8 view)
{
	if(view >= MSX2_BOARD_VIEWS)
		view = MSX2_VIEW_TOP;
	g_view = view;
}

static void Msx2_RasterLoad(u8 card_index, u8 defense)
{
	u16 key = TEX_KEY(card_index, defense);
	u16 base;

	if(g_tex_loaded == key)
		return;
	// A card in defence position is turned a quarter turn on the board, and the
	// turn is in the ART -- the defence set is stored 48 wide by 40 tall -- so
	// the mapper below is untouched by it: it still walks one texture row
	// forwards per destination row.
	base = defense ? MSX2_CARD_DEF_SEGMENT : MSX2_CARD_ART_SEGMENT;
	Msx2_RomReadLong((u16)(base + card_index / MSX2_CARD_ART_PER_SEG),
	                 (u16)((card_index % MSX2_CARD_ART_PER_SEG)
	                       * MSX2_CARD_ART_STRIDE),
	                 g_tex, MSX2_CARD_W * MSX2_CARD_H);
	g_tex_loaded = key;
}

// ── The texel run ────────────────────────────────────────────────────────────
//
// One destination row: `n` bytes at the VDP's current write address, sampled
// from `src` with a fractional step.  GRAPHIC 7 will not accept two data-port
// writes closer than 32 T-states while the display is on, and this loop is
// about sixty, so the stepping is free -- it happens inside the spacing the
// hardware demands anyway.  That is the whole reason a live mapper costs no
// more than replaying a baked program did.
static const u8* g_dda_src;
static u8 g_dda_n;
static u8 g_dda_int;        // whole texels per destination pixel
static u8 g_dda_frac;       // and the fraction, in 1/256ths
static u8 g_dda_back;       // 1 when the row's texels run right to left

static void Msx2_RasterRun(void)
{
	__asm
		ld		hl, (_g_dda_src)
		ld		a, (_g_dda_n)
		or		a
		jr		z, 00130$
		ld		b, a
		ld		a, (_g_dda_frac)
		ld		c, a
		ld		a, (_g_dda_int)
		ld		d, a
		ld		e, #0x80				// start half a texel in
		ld		a, (_g_dda_back)
		or		a
		jr		nz, 00120$

	00110$:
		ld		a, (hl)
		out		(#0x98), a
		ld		a, e
		add		a, c
		ld		e, a
		ld		a, l
		adc		a, d
		ld		l, a
		jr		nc, 00111$
		inc		h
	00111$:
		djnz	00110$
		jr		00130$

		// The same walk with the source pointer running down.  This is what
		// replaced the mirrored copy of the entire card blob: a COM-side card
		// is rotated half a turn on the board plane, so its rows read
		// backwards, and a block move could only ever go forwards.
	00120$:
		ld		a, (hl)
		out		(#0x98), a
		ld		a, e
		sub		a, c
		ld		e, a
		ld		a, l
		sbc		a, d
		ld		l, a
		jr		nc, 00121$
		dec		h
	00121$:
		djnz	00120$
	00130$:
	__endasm;
}

// ── The quad ─────────────────────────────────────────────────────────────────
//
// Corner order is the capture's, which is the order the shared renderer hands
// its own rasterizer: 0-1 is the texture's top edge and 3-2 its bottom, so the
// two side edges are 0->3 and 1->2.  Both of a slot's horizontal edges really
// are horizontal -- the projection of a board row is a trapezoid -- so the two
// side edges span the same rows and one DDA each is the whole geometry.
static u8 g_qx[4], g_qy[4];

// Edge state, in 8.8: x across the screen and v down the texture.
static i16 g_lx, g_lv, g_ldx, g_ldv;
static i16 g_rx, g_rv, g_rdx, g_rdv;

// One side edge, as an x and a v that step once per INCREASING row -- so the
// edge's own direction cancels itself inside the division and the row loop
// never asks which way round the quad is.  The result lands in these four
// globals rather than through pointer arguments: SDCC reaches a pointed-to i16
// through IX twice for every read, and this is called four times a card.
static i16 g_ex, g_ev, g_edx, g_edv;

static void Msx2_RasterEdge(u8 a, u8 b, u8 tex_h)
{
	i16 h = (i16)((i16)g_qy[b] - (i16)g_qy[a]);

	if(h == 0)
	{
		g_ex = (i16)((u16)g_qx[a] << 8);
		g_ev = 0;
		g_edx = 0;
		g_edv = 0;
		return;
	}
	g_edx = (i16)((((i16)g_qx[b] - (i16)g_qx[a]) << 8) / h);
	g_edv = (i16)((((i16)tex_h) << 8) / h);
	if(h > 0)
	{
		g_ex = (i16)((u16)g_qx[a] << 8);
		g_ev = 0;
	}
	else
	{
		g_ex = (i16)((u16)g_qx[b] << 8);
		g_ev = (i16)((u16)tex_h << 8);
	}
}

static void Msx2_RasterDraw(u8 defense)
{
	// The defence set is stored turned, so the texture is 48 wide by 40 tall
	// and nothing here rotates a pixel.
	u8 tex_w = defense ? MSX2_CARD_H : MSX2_CARD_W;
	u8 tex_h = defense ? MSX2_CARD_W : MSX2_CARD_H;
	u8 ytop, ybot, y;

	Msx2_RasterEdge(0, 3, tex_h);
	g_lx = g_ex; g_lv = g_ev; g_ldx = g_edx; g_ldv = g_edv;
	Msx2_RasterEdge(1, 2, tex_h);
	g_rx = g_ex; g_rv = g_ev; g_rdx = g_edx; g_rdv = g_edv;

	ytop = (g_qy[0] < g_qy[3]) ? g_qy[0] : g_qy[3];
	ybot = (g_qy[0] < g_qy[3]) ? g_qy[3] : g_qy[0];
	if(ybot <= ytop)
		return;

	for(y = ytop; y < ybot; ++y)
	{
		i16 xl = (i16)(g_lx >> 8);
		i16 xr = (i16)(g_rx >> 8);
		i16 row = (i16)(((g_lv >> 1) + (g_rv >> 1)) >> 8);
		u16 width;
		u16 step;
		u8 x0;

		if(row < 0)
			row = 0;
		else if(row >= (i16)tex_h)
			row = (i16)(tex_h - 1);

		if(xr >= xl)
		{
			x0 = (u8)xl;
			width = (u16)(xr - xl + 1);
			g_dda_back = 0;
			g_dda_src = &g_tex[(u16)row * tex_w];
		}
		else
		{
			x0 = (u8)xr;
			width = (u16)(xl - xr + 1);
			g_dda_back = 1;
			g_dda_src = &g_tex[(u16)row * tex_w + (tex_w - 1)];
		}

		// How much of the texture each destination pixel is worth.  A slot is
		// narrower than the card in every chair view, so this is nearly always
		// a minification and the fractional part carries it.
		step = (u16)(((u16)tex_w << 8) / width);
		g_dda_int = (u8)(step >> 8);
		g_dda_frac = (u8)step;
		g_dda_n = (u8)width;

		Msx2_PokeAt(x0, y);
		Msx2_RasterRun();

		g_lx = (i16)(g_lx + g_ldx);
		g_rx = (i16)(g_rx + g_rdx);
		g_lv = (i16)(g_lv + g_ldv);
		g_rv = (i16)(g_rv + g_rdv);
	}
}

void Msx2_RasterCard(u8 card_index, u8 slot, u8 defense)
{
	const u8* q = g_msx2_slot_quad[g_view][slot];
	u8 i;

	for(i = 0; i < 4; ++i)
	{
		g_qx[i] = q[i * 2];
		g_qy[i] = q[i * 2 + 1];
	}
	if(defense)
	{
		// A CARD LYING DOWN IS INSCRIBED IN THE SLOT, NOT LAID ACROSS IT.
		// The chair slots sit shoulder to shoulder and every erase on this
		// board is the slot's own footprint, so a turned card that reached
		// past it would leave a strip of itself behind for the rest of the
		// duel.  It keeps the full width and MSX2_DEF_INSET of the height at
		// each end -- the same inset the baked programs used.
		u8 j;
		u8 nx[4], ny[4];

		for(j = 0; j < 2; ++j)
		{
			u8 a = j;                    // 0 -> 3 and 1 -> 2 are the side edges
			u8 b = (u8)(3 - j);
			i16 dx = (i16)((i16)g_qx[b] - (i16)g_qx[a]);
			i16 dy = (i16)((i16)g_qy[b] - (i16)g_qy[a]);

			nx[a] = (u8)(g_qx[a] + ((dx * MSX2_DEF_INSET) >> 8));
			ny[a] = (u8)(g_qy[a] + ((dy * MSX2_DEF_INSET) >> 8));
			nx[b] = (u8)(g_qx[b] - ((dx * MSX2_DEF_INSET) >> 8));
			ny[b] = (u8)(g_qy[b] - ((dy * MSX2_DEF_INSET) >> 8));
		}
		for(j = 0; j < 4; ++j)
		{
			g_qx[j] = nx[j];
			g_qy[j] = ny[j];
		}
	}
	Msx2_RasterLoad(card_index, defense);
	Msx2_RasterDraw(defense);
}
