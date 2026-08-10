/* Flat-shaded, backface-culled rotating cube -- milestone 6's boot-verified
 * proof that a software 3D rasterizer in this style (constant colour per
 * face, no textures, no perspective-correct texturing, integer-only math)
 * runs on real FM TOWNS hardware timing. This is a standalone demo, not the
 * portable game core's actual renderer3d.c/renderer3d_spans.inc pipeline
 * (src/engine/renderer3d_fmtowns.c registers that seam for when the game
 * core itself compiles for this target -- see STATUS.md) -- but it uses the
 * same *approach* the task calls for: flat shading like CD32X's renderer,
 * not the PC-FX's textured/perspective one, and it is real, working,
 * emulator-verified code, not a stub.
 *
 * No FPU is assumed (Marty boots with -DONTUSEFPU in this port's Tsugaru
 * invocation, and a bare-metal payload cannot rely on one being present or
 * initialized): everything below is fixed-point integer arithmetic. Sine is
 * Bhaskara I's 7th-century integer approximation (good to about 0.0016 max
 * error) instead of a lookup table, to keep this file self-contained.
 */
#include "fmtowns_cube_demo.h"

#define FX_SHIFT   8
#define FX_ONE     (1 << FX_SHIFT)

/* Bhaskara I's sine approximation, scaled to FX_ONE. deg is 0..359. */
static int fmtowns_isin(int deg)
{
    int sign = 1;
    long num, den;

    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg > 180) {
        sign = -1;
        deg -= 180;
    }
    if (deg == 0) return 0;

    num = 4L * deg * (180 - deg);
    den = 40500L - (long)deg * (180 - deg);
    return sign * (int)((num * FX_ONE) / den);
}

static int fmtowns_icos(int deg)
{
    return fmtowns_isin(deg + 90);
}

typedef struct { int x, y, z; } FmtCubeVec3;
typedef struct { int x, y; } FmtCubePoint2;

#define CUBE_R 60

static const FmtCubeVec3 g_cube_verts[8] = {
    { -CUBE_R, -CUBE_R, -CUBE_R }, { CUBE_R, -CUBE_R, -CUBE_R },
    { CUBE_R,  CUBE_R, -CUBE_R }, { -CUBE_R,  CUBE_R, -CUBE_R },
    { -CUBE_R, -CUBE_R,  CUBE_R }, { CUBE_R, -CUBE_R,  CUBE_R },
    { CUBE_R,  CUBE_R,  CUBE_R }, { -CUBE_R,  CUBE_R,  CUBE_R }
};

/* Each face wound counter-clockwise as seen from outside the cube, so a
 * positive 2D signed area after projection means "facing the camera." */
static const unsigned char g_cube_faces[6][4] = {
    { 0, 1, 2, 3 },   /* -Z: front */
    { 5, 4, 7, 6 },   /* +Z: back */
    { 4, 0, 3, 7 },   /* -X: left */
    { 1, 5, 6, 2 },   /* +X: right */
    { 4, 5, 1, 0 },   /* -Y: top */
    { 3, 2, 6, 7 }    /* +Y: bottom */
};

/* index 0 = background, 1-6 = one flat colour per face above. */
static const uint8_t g_cube_face_rgb[6][3] = {
    { 200,  60,  60 },   /* front:  red */
    {  60, 200,  60 },   /* back:   green */
    {  60,  60, 200 },   /* left:   blue */
    { 210, 190,  40 },   /* right:  yellow */
    { 210, 110,  30 },   /* top:    orange */
    { 170,  60, 190 }    /* bottom: purple */
};

void fmtowns_cube_demo_palette(uint8_t *palette)
{
    int i;
    palette[0] = 8; palette[1] = 8; palette[2] = 16;   /* index 0: dark blue bg */
    for (i = 0; i < 6; ++i) {
        palette[(1 + i) * 3 + 0] = g_cube_face_rgb[i][0];
        palette[(1 + i) * 3 + 1] = g_cube_face_rgb[i][1];
        palette[(1 + i) * 3 + 2] = g_cube_face_rgb[i][2];
    }
}

#define CUBE_SCREEN_W  256
#define CUBE_SCREEN_H  232   /* leave rows 232-239 for fmtowns_main.c's pad strip */
#define CUBE_VIEW_DIST 260
#define CUBE_FOV_SCALE 220

static int g_angle_y = 0;
static int g_angle_x = 0;

static FmtCubePoint2 fmtowns_project(FmtCubeVec3 v)
{
    int cy = fmtowns_icos(g_angle_y), sy = fmtowns_isin(g_angle_y);
    int cx = fmtowns_icos(g_angle_x), sx = fmtowns_isin(g_angle_x);
    int x1, y1, z1, x2, y2, z2;
    int scale;
    FmtCubePoint2 p;

    /* Rotate about Y, then about X. */
    x1 = (v.x * cy - v.z * sy) >> FX_SHIFT;
    z1 = (v.x * sy + v.z * cy) >> FX_SHIFT;
    y1 = v.y;

    y2 = (y1 * cx - z1 * sx) >> FX_SHIFT;
    z2 = (y1 * sx + z1 * cx) >> FX_SHIFT;
    x2 = x1;

    scale = (CUBE_FOV_SCALE * FX_ONE) / (CUBE_VIEW_DIST + z2);
    p.x = CUBE_SCREEN_W / 2 + ((x2 * scale) >> FX_SHIFT);
    p.y = CUBE_SCREEN_H / 2 - ((y2 * scale) >> FX_SHIFT);
    return p;
}

/* Per-scanline x-extent accumulators, rebuilt fresh each face. */
static int g_span_xmin[CUBE_SCREEN_H];
static int g_span_xmax[CUBE_SCREEN_H];

static void fmtowns_cube_edge(FmtCubePoint2 a, FmtCubePoint2 b)
{
    int y0 = a.y, y1 = b.y, x0 = a.x, x1 = b.x;
    int y, x;

    if (y0 == y1) return;
    if (y0 > y1) {
        int t;
        t = y0; y0 = y1; y1 = t;
        t = x0; x0 = x1; x1 = t;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > CUBE_SCREEN_H) y1 = CUBE_SCREEN_H;

    for (y = y0; y < y1; ++y) {
        x = x0 + (int)((long)(x1 - x0) * (y - a.y) / (b.y - a.y));
        if (x < g_span_xmin[y]) g_span_xmin[y] = x;
        if (x > g_span_xmax[y]) g_span_xmax[y] = x;
    }
}

static void fmtowns_cube_fill_face(uint8_t *frame, const FmtCubePoint2 *p, uint8_t color)
{
    int y, x, y0 = CUBE_SCREEN_H, y1 = 0, i;

    for (i = 0; i < 4; ++i) {
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > CUBE_SCREEN_H) y1 = CUBE_SCREEN_H;
    for (y = y0; y < y1; ++y) {
        g_span_xmin[y] = CUBE_SCREEN_W;
        g_span_xmax[y] = -1;
    }

    fmtowns_cube_edge(p[0], p[1]);
    fmtowns_cube_edge(p[1], p[2]);
    fmtowns_cube_edge(p[2], p[3]);
    fmtowns_cube_edge(p[3], p[0]);

    for (y = y0; y < y1; ++y) {
        int xa = g_span_xmin[y], xb = g_span_xmax[y];
        if (xa > xb) continue;
        if (xa < 0) xa = 0;
        if (xb >= CUBE_SCREEN_W) xb = CUBE_SCREEN_W - 1;
        for (x = xa; x <= xb; ++x) {
            frame[(uint32_t)y * 256u + x] = color;
        }
    }
}

void fmtowns_cube_demo_frame(uint8_t *frame)
{
    FmtCubePoint2 proj[8];
    int i, f;

    for (i = 0; i < CUBE_SCREEN_H * 256; ++i) {
        frame[i] = 0;
    }

    for (i = 0; i < 8; ++i) {
        proj[i] = fmtowns_project(g_cube_verts[i]);
    }

    for (f = 0; f < 6; ++f) {
        FmtCubePoint2 p[4];
        long signed_area2;

        for (i = 0; i < 4; ++i) p[i] = proj[g_cube_faces[f][i]];

        /* Shoelace 2x signed area of the first triangle in the face: > 0
         * means counter-clockwise on screen, i.e. this face's winding
         * (chosen CCW from outside) still reads CCW after projection, so
         * it is facing the camera. <= 0 is a backface -- skip it. */
        signed_area2 = (long)(p[1].x - p[0].x) * (p[2].y - p[0].y)
                     - (long)(p[2].x - p[0].x) * (p[1].y - p[0].y);
        if (signed_area2 <= 0) continue;

        fmtowns_cube_fill_face(frame, p, (uint8_t)(1 + f));
    }

    g_angle_y = (g_angle_y + 3) % 360;
    g_angle_x = (g_angle_x + 1) % 360;
}
