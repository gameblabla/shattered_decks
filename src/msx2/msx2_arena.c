// ─────────────────────────────────────────────────────────────────────────────
//  msx2_arena.c — one pose of the board, from 157 bytes of geometry
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_arena.h"
#include "msx2_poly.h"
#include "msx2_stream.h"
#include "msx2_video.h"
#include "msx2_scenes.h"

// The pose, unpacked.  x is 16-bit because the arena leaves the screen on both
// sides during the opening descent; y never does, so it is a byte.
static i16 g_mesh_x[MSX2_MESH_POINTS];
static u8  g_mesh_y[MSX2_MESH_POINTS];
static u8  g_mesh_flags;
static u8  g_mesh_quad[2][MSX2_FIELD_SLOTS][8];
static u8  g_pose = 0xFF;

void Msx2_ArenaPose(u8 pose)
{
	u16 segment = (u16)(MSX2_MESH_SEGMENT + pose / MSX2_MESH_PER_SEG);
	u16 offset = (u16)((pose % MSX2_MESH_PER_SEG) * MSX2_MESH_STRIDE);

	if(pose == g_pose)
		return;
	g_pose = pose;
	// Three reads rather than one record into a scratch buffer: the blob is
	// laid out as the two arrays this file wants, so unpacking is the read.
	Msx2_RomRead(segment, offset, (u8*)g_mesh_x, MSX2_MESH_POINTS * 2);
	Msx2_RomRead(segment, (u16)(offset + MSX2_MESH_POINTS * 2), g_mesh_y,
	             MSX2_MESH_POINTS);
	Msx2_RomRead(segment, (u16)(offset + MSX2_MESH_POINTS * 3), &g_mesh_flags, 1);
	// Forty quads are 320 bytes.  The short reader's length is u8, so using it
	// here would silently wrap to 64 and leave sixteen cards with stale/zero
	// corners -- exactly the kind of half-populated orbit this record prevents.
	Msx2_RomReadLong(segment, (u16)(offset + MSX2_MESH_QUAD_OFFSET),
	                 &g_mesh_quad[0][0][0], MSX2_MESH_QUAD_BYTES * 2);
}

const u8* Msx2_ArenaCardQuad(u8 slot, u8 defense)
{
	return g_mesh_quad[defense ? 1 : 0][slot];
}

u8 Msx2_ArenaCurrentPose(void)
{
	return g_pose;
}

static void Msx2_ArenaQuad(u8 a, u8 b, u8 c, u8 d, u8 color)
{
	Msx2Point q[4];

	q[0].x = g_mesh_x[a]; q[0].y = g_mesh_y[a];
	q[1].x = g_mesh_x[b]; q[1].y = g_mesh_y[b];
	q[2].x = g_mesh_x[c]; q[2].y = g_mesh_y[c];
	q[3].x = g_mesh_x[d]; q[3].y = g_mesh_y[d];
	Msx2_PolyQuad(q, color);
}

// The board, in the shared renderer's own order: the two camera-facing slab
// walls first, then the opaque top surface over them.  Only two walls can ever
// face the camera, and draw_field_slab_sides_fast() paints exactly those two --
// the flags in the mesh record are that same test, taken at bake time.
static void Msx2_ArenaBody(void)
{
	u8 r, c;
	u8 zrow = (g_mesh_flags & MSX2_MESH_FLAG_ZPOS) ? MSX2_MESH_ROWS : 0;
	u8 zbot = (g_mesh_flags & MSX2_MESH_FLAG_ZPOS) ? MSX2_MESH_BZ1 : MSX2_MESH_BZ0;
	u8 xcol = (g_mesh_flags & MSX2_MESH_FLAG_XPOS) ? MSX2_MESH_COLS : 0;
	u8 xbot = (g_mesh_flags & MSX2_MESH_FLAG_XPOS) ? MSX2_MESH_BX1 : MSX2_MESH_BX0;

	for(c = 0; c < MSX2_MESH_COLS; ++c)
		Msx2_ArenaQuad(MSX2_MESH_TOP(zrow, c), MSX2_MESH_TOP(zrow, c + 1),
		               (u8)(zbot + c + 1), (u8)(zbot + c), MSX2_BOARD_WALL_Z);
	for(r = 0; r < MSX2_MESH_ROWS; ++r)
		Msx2_ArenaQuad(MSX2_MESH_TOP(r, xcol), MSX2_MESH_TOP(r + 1, xcol),
		               (u8)(xbot + r + 1), (u8)(xbot + r), MSX2_BOARD_WALL_X);
	for(r = 0; r < MSX2_MESH_ROWS; ++r)
		for(c = 0; c < MSX2_MESH_COLS; ++c)
			Msx2_ArenaQuad(MSX2_MESH_TOP(r, c), MSX2_MESH_TOP(r, c + 1),
			               MSX2_MESH_TOP(r + 1, c + 1), MSX2_MESH_TOP(r + 1, c),
			               ((r + c) & 1) ? MSX2_BOARD_TILE_B : MSX2_BOARD_TILE_A);
}

void Msx2_ArenaDraw(void)
{
	Msx2_PolyBegin(MSX2_BAND_Y, MSX2_BAND_H);
	Msx2_ArenaBody();
	Msx2_PolyBackdrop(MSX2_BLACK);
}

void Msx2_ArenaDrawBox(u8 x, u8 y, u8 w, u8 h)
{
	Msx2_PolyBegin(y, h);
	Msx2_PolyClipX(x, (u16)((u16)x + w));
	Msx2_ArenaBody();
	Msx2_PolyBackdrop(MSX2_BLACK);
}

// ── The overhead table ───────────────────────────────────────────────────────
//
// Not a projection: this view is the board seen straight down, so its twenty
// slots are axis-aligned rectangles.  Every one of them is a single HMMV with
// NY set -- Msx2_Fill's own command -- so the whole table is one black ground
// and twenty beige tiles rather than the 54 KB picture of itself it used to be
// streamed as.  It is the one view where drawing costs almost nothing at all.

// The rectangle actually painted is the caller's box intersected with the clip
// window, so one function serves both the whole view and the repair of a single
// emptied slot.
static u8 g_over_cx0, g_over_cy0;
static u16 g_over_cx1;
static u8 g_over_cy1;

static void Msx2_ArenaOverFill(i16 x, i16 y, i16 w, i16 h, u8 color)
{
	i16 x1 = (i16)(x + w);
	i16 y1 = (i16)(y + h);

	if(x < (i16)g_over_cx0) x = (i16)g_over_cx0;
	if(y < (i16)g_over_cy0) y = (i16)g_over_cy0;
	if(x1 > (i16)g_over_cx1) x1 = (i16)g_over_cx1;
	if(y1 > (i16)g_over_cy1) y1 = (i16)g_over_cy1;
	if((x1 <= x) || (y1 <= y))
		return;
	Msx2_Fill((u8)x, (u8)y, (u16)(x1 - x), (u8)(y1 - y), color);
}

static void Msx2_ArenaOverBody(void)
{
	u8 r, c;

	// Straight down means there are no visible slab walls, bevels or slot
	// outlines: this view is literally the board's alternating dark-beige and
	// beige top faces.  Fill the whole pitch so adjacent tiles meet; the old
	// four-pixel wall-coloured gutters and inverse-colour hairlines were what
	// made the tactical view look like a grid of outlined buttons.
	Msx2_ArenaOverFill(0, MSX2_OVER_Y, MSX2_SCREEN_W, MSX2_OVER_H,
	                   MSX2_BLACK);
	for(r = 0; r < MSX2_OVER_TILE_ROWS; ++r)
	{
		i16 ty = (i16)(MSX2_OVER_Y + MSX2_OVER_TILE_Y0
		               + r * MSX2_OVER_TILE_PITCH_Y);
		for(c = 0; c < MSX2_OVER_TILE_COLS; ++c)
		{
			i16 tx = (i16)(MSX2_OVER_TILE_X0 + c * MSX2_OVER_TILE_PITCH_X);
			u8 face = ((r + c) & 1) ? MSX2_BOARD_TILE_B : MSX2_BOARD_TILE_A;

			Msx2_ArenaOverFill(tx, ty, MSX2_OVER_TILE_PITCH_X,
			                   MSX2_OVER_TILE_PITCH_Y, face);
		}
	}
}

void Msx2_ArenaDrawOver(void)
{
	g_over_cx0 = 0;
	g_over_cy0 = MSX2_OVER_Y;
	g_over_cx1 = MSX2_SCREEN_W;
	g_over_cy1 = (u8)(MSX2_OVER_Y + MSX2_OVER_H);
	Msx2_ArenaOverBody();
}

void Msx2_ArenaDrawOverBox(u8 x, u8 y, u8 w, u8 h)
{
	g_over_cx0 = x;
	g_over_cy0 = y;
	g_over_cx1 = (u16)((u16)x + w);
	g_over_cy1 = (u8)(y + h);
	Msx2_ArenaOverBody();
}
