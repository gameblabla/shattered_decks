// ─────────────────────────────────────────────────────────────────────────────
//  msx2_poly.c — the span emitter and the convex filler
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_poly.h"
#include "msx2_video.h"

// ── The span ─────────────────────────────────────────────────────────────────
//
// Parameters go through globals rather than arguments: SDCC pushes five of them
// onto the stack and unpacks them again, which on a thousand-span frame is more
// time than the fill.  The routine is called from one file and never recurses.
static u8  g_span_x;
static u16 g_span_y;        // VRAM row, page included
static u16 g_span_n;
static u8  g_span_color;

// One HMMV.
//
// The CE poll comes FIRST and the command is issued last, which is what buys
// the overlap: the caller steps its two edges after this returns, while the VDP
// is still painting the span that was just issued.  Nothing is gained by
// waiting for completion at the end -- the next span's poll is that wait.
//
// S#2 is selected inside the DI window on every span rather than once for the
// whole board, and S#0 is put back before the window closes: the interrupt
// handler reads S#0 and leaves R#15 pointing at it, so a selection that
// outlives an interrupt is a selection that is silently wrong -- and one that
// outlives the DI window stops the V-blank flag from ever being cleared.  Four
// `out`s, and the alternative is a board that polls a status register nobody
// set on a machine that never leaves its interrupt handler.
static void Msx2_PolySpanOut(void)
{
	__asm
		di
		ld		a, #2
		out		(#0x99), a
		ld		a, #(15 | 0x80)
		out		(#0x99), a
	00080$:
		in		a, (#0x99)
		rra								// S#2 bit 0 = CE
		jr		c, 00080$

		// PUT S#0 BACK BEFORE THE INTERRUPT HANDLER SEES IT.
		// The V-blank flag is cleared by READING S#0, and the handler reads
		// whatever status register R#15 selects.  Leaving it on S#2 means the
		// flag is never cleared, the interrupt is still pending the moment the
		// handler returns, and the machine spends the rest of its life
		// re-entering it -- the board still drew correctly and the game
		// advanced about one frame a second.  MSXgl's VDP_CommandWait restores
		// S#0 for exactly this reason (VDP_USE_RESTORE_S0); a hand-written poll
		// owes the same debt.
		xor		a
		out		(#0x99), a
		ld		a, #(15 | 0x80)
		out		(#0x99), a

		ld		a, #36					// R#17 -> R#36, autoincrementing
		out		(#0x99), a
		ld		a, #(17 | 0x80)
		out		(#0x99), a

		ld		a, (_g_span_x)
		out		(#0x9B), a				// DX low
		xor		a
		out		(#0x9B), a				// DX high
		ld		hl, (_g_span_y)
		ld		a, l
		out		(#0x9B), a				// DY low
		ld		a, h
		out		(#0x9B), a				// DY high
		ld		hl, (_g_span_n)
		ld		a, l
		out		(#0x9B), a				// NX low
		ld		a, h
		out		(#0x9B), a				// NX high
		ld		a, #1
		out		(#0x9B), a				// NY low
		xor		a
		out		(#0x9B), a				// NY high
		ld		a, (_g_span_color)
		out		(#0x9B), a				// CLR
		xor		a
		out		(#0x9B), a				// ARG
		ld		a, #0xC0
		out		(#0x9B), a				// CMD = HMMV
		ei
	__endasm;
}

// ── The frame ────────────────────────────────────────────────────────────────

static u8 g_band_y0;
static u8 g_band_y1;                        // one past the last row
// The x window as well, because an empty slot is repaired by redrawing the
// arena tiles that cover it and nothing else: without this the repair would
// repaint the whole board to put one 40x48 hole back.
static u8  g_clip_x0;
static u16 g_clip_x1;                       // one past the last column
// The leftmost and rightmost pixel any quad touched on each row of the band.
// l > r means "nothing here", which is how an untouched row is told apart from
// a one-pixel one.
static u8 g_cov_l[MSX2_POLY_MAX_ROWS];
static u8 g_cov_r[MSX2_POLY_MAX_ROWS];

// ── The walker's state, which is assembly's to read ──────────────────────────
//
// X IS CARRIED BIASED BY 512, so the walker orders two corners with an unsigned
// `sbc hl,de` and no sign fixup at all.  The arena runs off both sides of the
// screen during the opening descent, so x really is signed; the bias is what
// keeps that from costing an overflow test on every scanline of every tile.
#define POLY_XBIAS   512

// One edge of the shape, as the row loop wants it.  The field ORDER is part of
// the contract: the assembly below addresses these by offset.
typedef struct
{
	u16 x;          // +0  current x, biased
	u16 err;        // +2  Bresenham accumulator
	u16 adx;        // +4  |dx| of this edge
	u16 dy;         // +6  its height, never zero
	u8  dir;        // +8  0 = x increases, 1 = x decreases
	u8  pad;        // +9
} Msx2Edge;

static Msx2Edge g_ea;
static Msx2Edge g_eb;

static u8  g_run_rows;      // scanlines this call walks
static u8  g_run_draw;      // 0 = step the edges but paint nothing
static u8  g_run_color;
static u8  g_cov_index;     // row - g_band_y0
static u16 g_run_yv;        // VRAM row of the current one, page included
static u16 g_clip0b;        // the clip window, biased
static u16 g_clip1b;
static u16 g_tmp_xl;
static u16 g_tmp_xr;

// THE ROW LOOP IS ASSEMBLY, AND THAT IS THE WHOLE PERFORMANCE STORY.
//
// It was C first, and it worked: the same board, the same pixels, at about one
// camera pose a second.  SDCC compiles `while (err >= dy) { err -= dy; x += s; }`
// on a struct pointer into 521 instructions -- every field re-fetched through IX
// on every iteration -- so one scanline of one tile cost some two thousand
// T-states before the VDP was asked to do anything at all.  The command engine
// paints the whole 256x114 band in 65 ms; the C walker took 1,200.
//
// docs/MSX2_REALTIME_POLYGON_FINDINGS.md §2 predicted the shape of this ("a
// live polygon board must not be built on Msx2_Fill()... it needs a dedicated
// assembly span emitter that owns the loop") and put the win at about 2x.  On
// the C-versus-assembly axis it is nearer ten.
//
// Nothing here is clever: the same Bresenham, with its state at fixed addresses
// instead of behind a frame pointer, x kept in BC and the error term in HL
// across the whole step, and the clip and coverage bookkeeping done with the
// values already in registers.
static void Msx2_PolyRun(void)
{
	__asm
	00200$:
		// ── order the two chains: the smaller x is the left edge ──────────
		ld		hl, (_g_ea + 0)
		ld		de, (_g_eb + 0)
		ld		(_g_tmp_xl), hl
		ld		(_g_tmp_xr), de
		or		a
		sbc		hl, de
		jr		c, 00201$				// A is left of B: already in order
		ld		hl, (_g_eb + 0)
		ld		(_g_tmp_xl), hl
		ld		hl, (_g_ea + 0)
		ld		(_g_tmp_xr), hl
	00201$:
		ld		a, (_g_run_draw)
		or		a
		jp		z, 00230$

		// ── clip ─────────────────────────────────────────────────────────
		ld		hl, (_g_tmp_xl)
		ld		de, (_g_clip0b)
		or		a
		sbc		hl, de
		jr		nc, 00202$
		ld		(_g_tmp_xl), de
	00202$:
		ld		hl, (_g_tmp_xr)
		ld		de, (_g_clip1b)
		or		a
		sbc		hl, de
		jr		c, 00203$
		ld		(_g_tmp_xr), de
	00203$:
		// width = xr - xl.  Zero or negative means the span missed the window.
		ld		hl, (_g_tmp_xr)
		ld		de, (_g_tmp_xl)
		or		a
		sbc		hl, de
		jr		z, 00230$
		bit		7, h
		jr		nz, 00230$
		ld		(_g_span_n), hl

		// x = xl - the bias, which is a byte again by construction
		ex		de, hl					// hl = xl (biased)
		ld		de, #0xFE00				// -POLY_XBIAS
		add		hl, de
		ld		a, l
		ld		(_g_span_x), a

		// ── coverage, so the backdrop knows what it need NOT fill ─────────
		ld		c, a					// c = the span's first pixel
		ld		a, (_g_cov_index)
		ld		e, a
		ld		d, #0
		ld		hl, #_g_cov_l
		add		hl, de
		ld		a, c
		cp		a, (hl)
		jr		nc, 00210$
		ld		(hl), a
	00210$:
		ld		hl, #_g_cov_r
		add		hl, de
		ld		de, (_g_span_n)
		ld		b, #0
		push	hl
		ld		h, b
		ld		l, c					// hl = first pixel
		add		hl, de
		dec		hl						// last pixel = first + n - 1
		ld		a, l
		pop		hl
		cp		a, (hl)
		jr		c, 00211$
		ld		(hl), a
	00211$:
		ld		hl, (_g_run_yv)
		ld		(_g_span_y), hl
		ld		a, (_g_run_color)
		ld		(_g_span_color), a
		call	_Msx2_PolySpanOut

		// ── step both edges ──────────────────────────────────────────────
	00230$:
		ld		hl, (_g_ea + 2)			// err
		ld		de, (_g_ea + 4)			// adx
		add		hl, de
		ld		de, (_g_ea + 6)			// dy
		ld		bc, (_g_ea + 0)			// x
		ld		a, (_g_ea + 8)			// direction
		or		a						// clear carry for the first sbc
	00231$:
		sbc		hl, de
		jr		c, 00234$
		or		a						// dir: 0 = right, 1 = left
		jr		nz, 00232$
		inc		bc
		jr		00233$
	00232$:
		dec		bc
	00233$:
		or		a						// clear carry for the next sbc
		jr		00231$
	00234$:
		add		hl, de					// put the borrowed dy back
		ld		(_g_ea + 2), hl
		ld		(_g_ea + 0), bc

		ld		hl, (_g_eb + 2)
		ld		de, (_g_eb + 4)
		add		hl, de
		ld		de, (_g_eb + 6)
		ld		bc, (_g_eb + 0)
		ld		a, (_g_eb + 8)
		or		a
	00241$:
		sbc		hl, de
		jr		c, 00244$
		or		a
		jr		nz, 00242$
		inc		bc
		jr		00243$
	00242$:
		dec		bc
	00243$:
		or		a
		jr		00241$
	00244$:
		add		hl, de
		ld		(_g_eb + 2), hl
		ld		(_g_eb + 0), bc

		// ── next row ─────────────────────────────────────────────────────
		ld		hl, (_g_run_yv)
		inc		hl
		ld		(_g_run_yv), hl
		ld		a, (_g_cov_index)
		inc		a
		ld		(_g_cov_index), a
		ld		a, (_g_run_rows)
		dec		a
		ld		(_g_run_rows), a
		jp		nz, 00200$
	__endasm;
}

void Msx2_PolyBegin(u8 y0, u8 h)
{
	u8 i;

	if(h > MSX2_POLY_MAX_ROWS)
		h = MSX2_POLY_MAX_ROWS;
	g_band_y0 = y0;
	g_band_y1 = (u8)(y0 + h);
	g_clip_x0 = 0;
	g_clip_x1 = 256;
	g_clip0b = POLY_XBIAS;
	g_clip1b = POLY_XBIAS + 256;
	for(i = 0; i < h; ++i)
	{
		g_cov_l[i] = 255;
		g_cov_r[i] = 0;
	}
}

void Msx2_PolyClipX(u8 x0, u16 x1)
{
	g_clip_x0 = x0;
	g_clip_x1 = x1;
	g_clip0b = (u16)(POLY_XBIAS + x0);
	g_clip1b = (u16)(POLY_XBIAS + x1);
}

void Msx2_PolySpanAt(u8 x, u8 y, u16 n, u8 color)
{
	if(n == 0)
		return;
	g_span_x = x;
	g_span_y = (u16)y + (Msx2_VideoGetDrawPage() ? 256u : 0u);
	g_span_n = n;
	g_span_color = color;
	Msx2_PolySpanOut();
}

// ── The convex filler ────────────────────────────────────────────────────────
//
// Two chains walk away from the topmost corner in opposite directions and meet
// at the bottom one.  Which of them is the left edge is never asked: on a convex
// shape the span is simply the smaller x to the larger, and asking would cost a
// cross product and get the degenerate cases wrong.
//
// The edge step is integer Bresenham, not fixed point.  A Q8.8 x overflows
// sixteen bits at this screen width, and a Q10.6 one accumulates almost two
// pixels of drift over a hundred-row edge -- which shows as a seam between two
// tiles that share that edge.  An error accumulator has neither problem and is
// exact: two tiles that share corners compute the same x on every row, so the
// board tiles without a gap or an overlap.

static u8 g_a_idx, g_b_idx;      // the corner each chain's current edge ends at
static u8 g_a_left, g_b_left;    // rows still to walk on it

// Take a chain to the next corner that is genuinely lower.  A horizontal edge
// contributes no rows; sliding along it rather than drawing it is what keeps a
// flat-topped or flat-bottomed quad -- which every tile of a board row is --
// from being drawn a pixel narrow.
static void Msx2_PolyEdge(const Msx2Point* p, Msx2Edge* e, u8* idx, i8 dir,
                          u8* left)
{
	for(;;)
	{
		u8 here = *idx;
		u8 next = (u8)((here + (dir > 0 ? 1 : 3)) & 3);
		i16 dx;

		if(p[next].y > p[here].y)
		{
			e->x = (u16)(p[here].x + POLY_XBIAS);
			e->dy = (u16)(p[next].y - p[here].y);
			dx = (i16)(p[next].x - p[here].x);
			e->dir = (dx < 0) ? 1 : 0;
			e->adx = (u16)((dx < 0) ? -dx : dx);
			e->err = (u16)(e->dy >> 1);
			*left = (u8)e->dy;
			*idx = next;
			return;
		}
		if(p[next].y < p[here].y)
		{
			*left = 0;               // past the bottom corner: nothing left
			return;
		}
		*idx = next;                 // horizontal: slide along and keep looking
	}
}

static void Msx2_PolyRunRows(u8 rows, u8 y, u8 draw)
{
	if(rows == 0)
		return;
	g_run_rows = rows;
	g_run_draw = draw;
	g_run_yv = (u16)y + (Msx2_VideoGetDrawPage() ? 256u : 0u);
	g_cov_index = (u8)(y - g_band_y0);
	Msx2_PolyRun();
}

void Msx2_PolyQuad(const Msx2Point* p, u8 color)
{
	u8 i, top = 0, bottom = 0;
	u8 y;

	for(i = 1; i < 4; ++i)
	{
		if(p[i].y < p[top].y)
			top = i;
		if(p[i].y >= p[bottom].y)
			bottom = i;
	}
	if(p[bottom].y == p[top].y)
		return;                          // edge-on: no rows to fill

	g_a_idx = g_b_idx = top;
	Msx2_PolyEdge(p, &g_ea, &g_a_idx, 1, &g_a_left);
	Msx2_PolyEdge(p, &g_eb, &g_b_idx, -1, &g_b_left);
	g_run_color = color;

	y = p[top].y;
	while(y < p[bottom].y)
	{
		u8 rows;

		if(g_a_left == 0)
			Msx2_PolyEdge(p, &g_ea, &g_a_idx, 1, &g_a_left);
		if(g_b_left == 0)
			Msx2_PolyEdge(p, &g_eb, &g_b_idx, -1, &g_b_left);
		if((g_a_left == 0) || (g_b_left == 0))
			break;
		if(y >= g_band_y1)
			break;

		// One run is as many rows as both edges can promise, cut at the band's
		// two boundaries -- rows above it still have to be WALKED, or the edges
		// reach the first visible row in the wrong place.
		rows = (g_a_left < g_b_left) ? g_a_left : g_b_left;
		if((u16)y + rows > (u16)p[bottom].y)
			rows = (u8)(p[bottom].y - y);
		if(y < g_band_y0)
		{
			u8 skip = (u8)(g_band_y0 - y);
			if(skip < rows)
				rows = skip;
			Msx2_PolyRunRows(rows, y, 0);
		}
		else
		{
			if((u16)y + rows > (u16)g_band_y1)
				rows = (u8)(g_band_y1 - y);
			Msx2_PolyRunRows(rows, y, 1);
		}
		y = (u8)(y + rows);
		g_a_left = (u8)(g_a_left - rows);
		g_b_left = (u8)(g_b_left - rows);
	}
}

void Msx2_PolyBackdrop(u8 color)
{
	u8 y;

	for(y = g_band_y0; y < g_band_y1; ++y)
	{
		u8  idx = (u8)(y - g_band_y0);
		u16 l = g_cov_l[idx];
		u16 r = (u16)g_cov_r[idx] + 1;      // one past the last covered pixel

		if(l >= r)
		{
			Msx2_PolySpanAt(g_clip_x0, y, (u16)(g_clip_x1 - g_clip_x0), color);
			continue;
		}
		if(l > (u16)g_clip_x0)
			Msx2_PolySpanAt(g_clip_x0, y, (u16)(l - g_clip_x0), color);
		if(r < g_clip_x1)
			Msx2_PolySpanAt((u8)r, y, (u16)(g_clip_x1 - r), color);
	}
}
