// ─────────────────────────────────────────────────────────────────────────────
//  msx2_raster.c — one card, mapped into its projected quad
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_raster.h"
#include "msx2_video.h"
#include "msx2_stream.h"
#include "msx2_scenes.h"
#include "msx2_arena.h"

// The card being drawn.  It lives in RAM for the duration: a texel fetch is
// then an ordinary `ld a,(hl)`, so the inner loop never touches the mapper and
// never pays a bank switch per row.
static u8 g_tex[MSX2_CARD_W * MSX2_CARD_H];

// Which texture is already in g_tex, so a repaint that redraws the same card --
// the common case, once per page -- skips the 1,920-byte read.  0xFFFF is
// "nothing".
static u16 g_tex_loaded;

#define TEX_KEY(card, def)  (u16)(((u16)(card) << 1) | (def))
#define TEX_NONE            0xFFFF

void Msx2_RasterInit(void)
{
	g_tex_loaded = TEX_NONE;
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
// writes closer than 32 T-states while the display is on, so 32 is the floor
// this loop is written against and every T-state above it is wasted time.
//
// THERE ARE TWO OF THEM, AND THE FAST ONE IS THE POINT.
//
// The general walk below carries the source as a pointer plus a separate
// fractional accumulator, which costs four register moves and a carry branch
// per pixel -- about sixty T-states, nearly twice the floor.  The fast walk
// carries the whole position in HL instead, with H the source address's LOW
// byte and L the fraction, so ONE `add hl,bc` is the entire step, carry
// included; the fetch is `ld e,h` / `ld a,(de)` with D holding the page.  That
// works only while the source row stays inside one 256-byte page, which is a
// property of where the row happens to land in g_tex -- true for about five
// rows in six, and the general walk is what draws the sixth.
//
// A backward row needs no second loop: the step is simply negated, and
// position + (-step) borrows out of the fraction into the address byte exactly
// as the forward one carries.
//
// Sixteen pixels are unrolled with a computed entry (the run is entered part
// way into the block so it ends on the last pixel), which leaves the loop
// overhead at under three T-states a pixel.  The `nop` in the body is
// deliberate: without it the body is 33 T-states and the hardware's floor is
// 32, and one T-state is not a margin worth shipping to real machines.
static const u8* g_dda_src;
static u8 g_dda_n;
static u8 g_dda_int;        // whole texels per destination pixel
static u8 g_dda_frac;       // and the fraction, in 1/256ths
static u8 g_dda_back;       // 1 when the row's texels run right to left

// The fast walk's state.
static u16 g_dda_pos;       // H = the source address's low byte, L = the fraction
static u16 g_dda_step;      // 8.8 texels per pixel, negated for a backward row
static u8  g_dda_hi;        // the page the source row lives in
static u8  g_dda_grp;       // sixteen-pixel groups still to run

#define DDA_UNROLL  16
#define DDA_MAX_N   128     // the computed entry's arithmetic stays in a byte

// One row, the fast way.  Every register is spoken for: HL is the position,
// BC the step, D the page, E the scratch the fetch addresses through, A the
// texel.  The group counter is therefore in memory -- 42 T-states once every
// sixteen pixels, which is cheaper than anything that would free a register.
static void Msx2_RasterRunFast(void)
{
	__asm
		ld		a, (_g_dda_n)
		or		a
		ret		z
		ld		c, a
		add		a, #(DDA_UNROLL - 1)
		rrca							// n + 15, then / 16 -- the low bits are
		rrca							// zero-filled because n <= 128 keeps the
		rrca							// sum inside a byte
		rrca
		and		#0x0F
		ld		(_g_dda_grp), a
		ld		a, c
		and		#(DDA_UNROLL - 1)		// pixels in the short first group
		neg
		and		#(DDA_UNROLL - 1)		// bodies to skip
		ld		l, a
		ld		h, #0
		add		hl, hl					// x2
		ld		d, h
		ld		e, l
		add		hl, hl					// x4
		add		hl, de					// x6 -- one body is six bytes
		ld		de, #00210$
		add		hl, de
		push	hl						// the entry, taken by the ret below

		ld		hl, (_g_dda_pos)
		ld		bc, (_g_dda_step)
		ld		a, (_g_dda_hi)
		ld		d, a
		ret

		// Sixteen of these.  Six bytes and 37 T-states each; the assembler
		// would happily let the block drift out of step with the arithmetic
		// above, so nothing else may be inserted here.
	00210$:
		.rept	DDA_UNROLL
		ld		e, h
		ld		a, (de)
		out		(#0x98), a
		add		hl, bc
		nop
		.endm

		ld		a, (_g_dda_grp)
		dec		a
		ld		(_g_dda_grp), a
		jr		nz, 00210$
	__endasm;
}

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
//
// X IS UNSIGNED, and that is not a detail.  A screen x of 136 is 0x8800 in 8.8,
// which as an i16 is NEGATIVE -- so an edge right of the halfway column read
// back as x-256 while an edge left of it read back as itself, and the span
// between them came out the width of the screen.  The run then walked off the
// end of its row into the next ones: the streaks across the far half of the
// board that only ever appeared on the middle column's slots, the two whose
// quad straddles x=128.  msx2_poly.c's walker carries x biased by 512 for the
// same reason; this one keeps it unsigned, which is the same fix a byte
// cheaper.  The steps stay signed and are added modulo 65536.
static u16 g_lx, g_rx;
static i16 g_lv, g_ldx, g_ldv;
static i16 g_rv, g_rdx, g_rdv;

// One side edge, as an x and a v that step once per INCREASING row -- so the
// edge's own direction cancels itself inside the division and the row loop
// never asks which way round the quad is.  The result lands in these four
// globals rather than through pointer arguments: SDCC reaches a pointed-to i16
// through IX twice for every read, and this is called four times a card.
static u16 g_ex;
static i16 g_ev, g_edx, g_edv;

static void Msx2_RasterEdge(u8 a, u8 b, u8 tex_h)
{
	i16 h = (i16)((i16)g_qy[b] - (i16)g_qy[a]);
	i16 dx = (i16)((i16)g_qx[b] - (i16)g_qx[a]);
	u16 step;

	if(h == 0)
	{
		g_ex = (u16)((u16)g_qx[a] << 8);
		g_ev = 0;
		g_edx = 0;
		g_edv = 0;
		return;
	}
	// Scale the unsigned magnitude: signed dx << 8 overflows beyond 127
	// pixels (and shifting a negative dx is undefined). Divide before signing.
	step = (u16)(((u16)(dx < 0 ? -dx : dx) << 8) /
	             (u16)(h < 0 ? -h : h));
	g_edx = (i16)(((dx < 0) != (h < 0)) ? (u16)(0u - step) : step);
	g_edv = (i16)((((i16)tex_h) << 8) / h);
	if(h > 0)
	{
		g_ex = (u16)((u16)g_qx[a] << 8);
		g_ev = 0;
	}
	else
	{
		g_ex = (u16)((u16)g_qx[b] << 8);
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
		u8 xl = (u8)(g_lx >> 8);
		u8 xr = (u8)(g_rx >> 8);
		i16 row = (i16)(((g_lv >> 1) + (g_rv >> 1)) >> 8);
		u16 width;
		u16 step;
		u8 x0;
		const u8* rowbase;

		if(row < 0)
			row = 0;
		else if(row >= (i16)tex_h)
			row = (i16)(tex_h - 1);

		rowbase = &g_tex[(u16)row * tex_w];
		if(xr >= xl)
		{
			x0 = xl;
			width = (u16)((u16)(xr - xl) + 1);
			g_dda_back = 0;
			g_dda_src = rowbase;
		}
		else
		{
			x0 = xr;
			width = (u16)((u16)(xl - xr) + 1);
			g_dda_back = 1;
			g_dda_src = rowbase + (tex_w - 1);
		}

		// A row is written straight at the data port, so a run that reaches
		// past column 255 does not clip -- it carries on into the rows below
		// it.  Nothing should produce one now, and this is what makes sure a
		// quad that ever does costs a short card instead of a striped board.
		if((u16)x0 + width > MSX2_SCREEN_W)
			width = (u16)(MSX2_SCREEN_W - x0);

		// How much of the texture each destination pixel is worth.  A slot is
		// narrower than the card in every chair view, so this is nearly always
		// a minification and the fractional part carries it.
		step = (u16)(((u16)tex_w << 8) / width);
		g_dda_n = (u8)width;

		Msx2_PokeAt(x0, y);
		// Which walk this row gets is decided by where the row happens to sit
		// in g_tex: the fast one keeps the source address's low byte in a
		// register half and cannot carry into the page.
		// A row wider than the texture is magnifying it, and the last pixel of
		// such a run can step one texel off the end -- where the two walks
		// disagree about the page byte.  It cannot happen with a projected
		// slot (every one of them is narrower than the card), and the general
		// walk keeps whatever it always did there.
		if((width <= (u16)tex_w) && (width <= DDA_MAX_N) &&
		   ((((u16)rowbase & 0x00FFu) + tex_w) <= 256u))
		{
			g_dda_hi = (u8)((u16)rowbase >> 8);
			g_dda_pos = (u16)((((u16)g_dda_src & 0x00FFu) << 8) | 0x0080u);
			g_dda_step = g_dda_back ? (u16)(0u - step) : step;
			Msx2_RasterRunFast();
		}
		else
		{
			g_dda_int = (u8)(step >> 8);
			g_dda_frac = (u8)step;
			Msx2_RasterRun();
		}

		g_lx = (i16)(g_lx + g_ldx);
		g_rx = (i16)(g_rx + g_rdx);
		g_lv = (i16)(g_lv + g_ldv);
		g_rv = (i16)(g_rv + g_rdv);
	}
}

void Msx2_RasterCard(u8 card_index, u8 slot, u8 defense)
{
	const u8* q = Msx2_ArenaCardQuad(slot, defense);
	u8 i;

	for(i = 0; i < 4; ++i)
	{
		g_qx[i] = q[i * 2];
		g_qy[i] = q[i * 2 + 1];
	}
	Msx2_RasterLoad(card_index, defense);
	Msx2_RasterDraw(defense);
}
