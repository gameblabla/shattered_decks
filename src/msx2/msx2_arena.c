// ─────────────────────────────────────────────────────────────────────────────
//  msx2_arena.c — one pose of the board and its card quads
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_arena.h"
#include "msx2_stream.h"
#include "msx2_video.h"
#include "msx2_scenes.h"

// Card geometry stays paired with the visible floor span pose.
static u8  g_mesh_quad[2][MSX2_FIELD_SLOTS][8];
u8 g_msx2_arena_pose = 0xFF;

#include "msx2_floor.c"

void Msx2_ArenaPose(u8 pose)
{
	u16 segment = (u16)(MSX2_MESH_SEGMENT + pose / MSX2_MESH_PER_SEG);
	u16 offset = (u16)((pose % MSX2_MESH_PER_SEG) * MSX2_MESH_STRIDE);

	if(pose >= MSX2_FLOOR_POSES || pose == g_msx2_arena_pose)
		return;
	g_msx2_arena_pose = pose;
	Msx2_RomReadLong(MSX2_FLOOR_SPAN_SEG + g_floor_segments[pose], g_floor_offsets[pose], g_floor_head, sizeof(g_floor_head));
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
	return g_msx2_arena_pose;
}

void Msx2_ArenaDraw(void)
{
    // A repaint that is not part of a camera move cannot trust either page.
    g_ext_ok[0] = g_ext_ok[1] = 0;
    Msx2_FloorBand(0, 1);
}

// One camera-move frame: the only stretch of the game where nothing but the
// arena and its cards paints inside the band, so the only one where a page's
// last silhouette can be trusted to say which black is already black.
void Msx2_ArenaDrawStep(void)
{
    Msx2_FloorBand(1, g_floor_stride);
}

void Msx2_ArenaMoveStart(void)
{
    g_ext_ok[0] = g_ext_ok[1] = 0;
}

void Msx2_ArenaDrawBox(u8 x, u8 y, u8 w, u8 h)
{
    Msx2_FloorDraw(x, y, (u8)(x + w - 1), (u8)(y + h));
}

// ── The overhead table ───────────────────────────────────────────────────────
//
// Axis-aligned textured tiles use the same sampler as the perspective floor.

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

static void Msx2_ArenaOverTile(i16 tx, i16 ty, u8 material)
{
    i16 x = tx, y = ty;
    i16 x1 = tx + MSX2_OVER_TILE_PITCH_X;
    i16 y1 = ty + MSX2_OVER_TILE_PITCH_Y;
    u16 du = 8192 / MSX2_OVER_TILE_PITCH_X;
    u16 dv = 8192 / MSX2_OVER_TILE_PITCH_Y;

    if(x < g_over_cx0) x = g_over_cx0;
    if(y < g_over_cy0) y = g_over_cy0;
    if(x1 > g_over_cx1) x1 = g_over_cx1;
    if(y1 > g_over_cy1) y1 = g_over_cy1;
    if(x >= x1 || y >= y1) return;
    Msx2_FloorLoad();
    // The table is axis aligned, so every row of it is a constant-v span and
    // takes the fine 32x32 material through the fast sampler.
    for(; y < y1; ++y)
    {
        u16 v = (u16)((u16)(y - ty) * dv);
        u16 off = (u16)(MSX2_FLOOR_FINE_BASE + (material ? 1024 : 0)
                        + (u16)((v >> 8) * 32));

        g_fp[FP_PTR + 1] = (u8)(g_floor_page + (u8)(off >> 8));
        *(u16*)(g_fp + FP_ACC) =
            (u16)(((u16)(off & 255) << 8) + (u16)((u16)(x - tx) * du));
        *(u16*)(g_fp + FP_DU) = du;
        g_fp[FP_N] = (u8)(x1 - x);
        g_fp_rec = (u16)g_fp;
        g_fp_x = (u8)x;
        g_fp_atok = 0;
        Msx2_FloorRowAddr((u8)y);
        Msx2_FloorPoke();
        Msx2_FloorFast();
    }
}

static void Msx2_ArenaOverBody(void)
{
	u8 r, c;

	Msx2_ArenaOverFill(0, MSX2_OVER_Y, MSX2_SCREEN_W, MSX2_OVER_H,
	                   MSX2_BLACK);
	for(r = 0; r < MSX2_OVER_TILE_ROWS; ++r)
	{
		i16 ty = (i16)(MSX2_OVER_Y + MSX2_OVER_TILE_Y0
		               + r * MSX2_OVER_TILE_PITCH_Y);
		for(c = 0; c < MSX2_OVER_TILE_COLS; ++c)
		{
			i16 tx = (i16)(MSX2_OVER_TILE_X0 + c * MSX2_OVER_TILE_PITCH_X);
			u8 face = (r + c) & 1;

			Msx2_ArenaOverTile(tx, ty, face);
		}
	}
}

void Msx2_ArenaDrawOver(void)
{
	g_ext_ok[0] = g_ext_ok[1] = 0;
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
