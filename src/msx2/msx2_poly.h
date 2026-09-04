// ─────────────────────────────────────────────────────────────────────────────
//  msx2_poly.h — flat convex polygons through the V9938 command engine
//
//  docs/MSX2_REALTIME_POLYGON_FINDINGS.md settles the primitive question: a
//  filled polygon on this machine is one HMMV per scanline, and HMMV writes a
//  byte in 48 VDP cycles against the 192 the port's own `outi` span loop pays.
//  Four times faster, and the Z80 is free while the command runs.  So nothing
//  here ever touches the data port: the CPU walks two edges, the command engine
//  paints.
//
//  The other half of that document is the per-span constant.  MSXgl's
//  VDP_CommandHMMV through Msx2_Fill costs 500-700 T-states a span -- it
//  reselects status register S#2 on every poll iteration and pushes eleven
//  bytes through an `otir` behind an SDCC call with five arguments -- and a
//  board is a thousand spans a frame.  Msx2_PolySpan below is the same command
//  in about 200: S#2 selected once per span rather than once per poll, the
//  registers written straight through the indirect port, and no arguments.
//
//  THE BACKDROP IS NOT A CLEAR.  Msx2_ClearPage on the board band is 29,184
//  bytes -- 65 ms, four whole frames -- and painting the arena over it pays for
//  most of those pixels twice.  The filler therefore RECORDS the leftmost and
//  rightmost pixel it touched on every scanline, and Msx2_PolyBackdrop fills
//  only what is outside that, in two spans a row.  A frame then costs the band
//  exactly once.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// A polygon corner.  x is signed and 16-bit because the arena runs off both
// sides of the screen during the opening descent; y is a screen row, so the
// caller clips nothing and this file clips everything.
typedef struct
{
	i16 x;
	u8  y;
} Msx2Point;

// Open a frame: every span from here on is clipped to rows [y0, y0 + h), and
// the coverage record starts empty.  h may not exceed MSX2_POLY_MAX_ROWS.
#define MSX2_POLY_MAX_ROWS  120
void Msx2_PolyBegin(u8 y0, u8 h);

// One convex quad, corners in order around the shape (either winding).
void Msx2_PolyQuad(const Msx2Point* p, u8 color);

// Fill everything inside the band that no quad covered.  This is the black
// surround, and it is why the band never needs clearing first.
void Msx2_PolyBackdrop(u8 color);

// Narrow every subsequent span to [x0, x1).  Reset to the whole screen by
// Msx2_PolyBegin; this is what lets one slot's worth of board be repainted
// without repainting the board.
void Msx2_PolyClipX(u8 x0, u16 x1);

// One flat rectangle through the same fast path, in band coordinates.
void Msx2_PolySpanAt(u8 x, u8 y, u16 n, u8 color);
