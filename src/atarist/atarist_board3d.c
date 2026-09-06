#include "atarist_board3d.h"
#include <stddef.h>

/* ─────────────────────────────────────────────────────────────────────────────
 *  Fixed point helpers.  A 68000 has no 32x32 multiply and no 32/32 divide, so
 *  every one of these is a libgcc call; they are kept out of inner loops on
 *  purpose and appear only in per-row and per-polygon setup.
 * ───────────────────────────────────────────────────────────────────────────── */

/* NO 64-BIT ARITHMETIC ANYWHERE IN THIS FILE.
 * The obvious `((int64_t)a * b) >> 16` costs libgcc's __muldi3 and __divdi3,
 * which is a couple of kilobytes of code and several hundred cycles a call on
 * a machine with neither to spare.  Both are decomposed into the 16x16 MULS
 * and 32/32 DIVS the 68000 actually has. */
static int32_t fxmul(int32_t a, int32_t b)
{
    int16_t ah = (int16_t)(a >> 16);
    uint16_t al = (uint16_t)a;
    int16_t bh = (int16_t)(b >> 16);
    uint16_t bl = (uint16_t)b;
    int32_t hi = (int32_t)ah * (int32_t)bh;
    int32_t mid = (int32_t)ah * (int32_t)bl + (int32_t)bh * (int32_t)al;
    uint32_t lo = (uint32_t)al * (uint32_t)bl;
    return (hi << 16) + mid + (int32_t)(lo >> 16);
}

/* (a << 16) / b, and the reason it is written this way is a shipped bug.
 *
 * The obvious version -- integer quotient plus (remainder << 16) / b -- is
 * correct only while b <= 1.0, because the remainder is bounded by b and
 * `r << 16` overflows 32 bits the moment b exceeds 65536.  The duel camera's
 * focal length is 160.0, so every ground row asked for depth/focal and got
 * zero back; the board rendered as flat horizontal bands with a seam down the
 * middle where u crossed zero, which reads exactly like a broken texture
 * mapper rather than a broken divide.
 *
 * Instead a is shifted left as far as it will go and whatever is left of the
 * 16 places is taken off b, so the division is always a single 32/32 that
 * cannot overflow, and it stays one libgcc call rather than a bit loop.
 */
static int32_t fxdiv(int32_t a, int32_t b)
{
    uint32_t ua, ub, q;
    int neg = 0;
    int sh = 16;

    if (!b) return 0;
    if (a < 0) { a = -a; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    ua = (uint32_t)a;
    ub = (uint32_t)b;

    while (sh > 0 && !(ua & 0x80000000u)) { ua <<= 1; --sh; }
    ub >>= sh;
    if (!ub) return neg ? -0x7fffffff : 0x7fffffff;
    q = ua / ub;
    if (q > 0x7fffffffu) q = 0x7fffffffu;
    return neg ? -(int32_t)q : (int32_t)q;
}

/* ── Board geometry ──────────────────────────────────────────────────────── */

/* One world unit is one slot pitch.  Columns run -2..2 across, rows -1.5..1.5
 * away from the camera, so the board is centred on the origin and the player's
 * monsters are the near row. */
void Atarist_SlotCentre(int row, int col, int32_t *wx, int32_t *wz)
{
    *wx = (int32_t)((col - (ATARIST_COLS - 1) / 2) << 16);
    /* Rows 0..3 far to near; the two monster rows sit closer together than the
     * support rows so the fighting line reads as the middle of the board. */
    {
        /* The support rows are pushed well behind their monster rows rather
         * than half a unit back.  At the duel camera's angle a half unit of
         * depth is three screen rows, and the two rows landed on top of each
         * other; 2.5 is what separates them into readable bands. */
        static const int32_t row_z[ATARIST_ROWS] = {
            (int32_t)(5 << 15),      /*  2.5  COM supports, farthest */
            (int32_t)(1 << 16),      /*  1.0  COM monsters */
            (int32_t)(-(1 << 16)),   /* -1.0  your monsters */
            (int32_t)(-(5 << 15))    /* -2.5  your supports, nearest */
        };
        *wz = row_z[row];
    }
}

/* ── Camera ──────────────────────────────────────────────────────────────── */

void Atarist_CameraSet(AtaristCamera *cam, int32_t x, int32_t z, int32_t height,
                       int32_t yaw_q16, int32_t focal)
{
    /* The board camera never rolls and never looks more than a few degrees off
     * the board's axis, so the heading is carried as its sine and cosine and
     * the caller passes them already resolved. */
    cam->x = x;
    cam->z = z;
    cam->height = height;
    cam->yaw_sin = 0;
    cam->yaw_cos = 1 << 16;
    (void)yaw_q16;
    cam->focal = focal;
    cam->horizon = 0;
}

int Atarist_Project(const AtaristCamera *cam, const AtaristViewport *vp,
                    int32_t wx, int32_t wz, int32_t wy,
                    int32_t *out_x, int32_t *out_y)
{
    /* Camera space: +z away from the viewer.  The board's far edge is at
     * larger z than the camera, so depth = wz - cam->z. */
    int32_t depth = wz - cam->z;
    int32_t sx, sy;
    if (depth < (1 << 12)) return 0;          /* at or behind the near plane */

    sx = fxdiv(fxmul(wx - cam->x, cam->focal), depth);
    sy = fxdiv(fxmul(cam->height - wy, cam->focal), depth);

    *out_x = (int32_t)((vp->w << 15)) + sx;   /* centre + offset */
    *out_y = (cam->horizon << 16) + sy;
    return 1;
}

/* ── Ground plane ────────────────────────────────────────────────────────── */

void Atarist_DrawGround(const AtaristViewport *vp, const AtaristCamera *cam,
                        const AtaristTexture *tex, int texel_log2,
                        uint8_t sky)
{
    const uint8_t *texels = tex->texels;
    int u_mask = (1 << tex->w_log2) - 1;
    int v_mask = (1 << tex->h_log2) - 1;
    int v_shift = tex->w_log2;
    int horizon = (int)cam->horizon;
    int half_w = vp->w / 2;
    /* Loop invariants, hoisted: height*focal is the whole numerator of the
     * plane equation, and the camera's own position in texels does not depend
     * on the row. */
    int32_t hf = fxmul(cam->height, cam->focal);
    int32_t u_camera = cam->x << texel_log2;
    int32_t v_camera = cam->z << texel_log2;
    int y;

    /* THE FAR CLAMP IS NOT COSMETIC.  Depth goes to infinity as a row
     * approaches the horizon, and u/v are 16.16: a row a fraction of a pixel
     * below the horizon asks for tens of thousands of texels across the span
     * and the texture coordinate wraps into nonsense, which paints the top of
     * the board as wide horizontal bands of one colour.  Rows beyond the far
     * plane are the backdrop instead. */
#define GROUND_FAR (24 << 16)

    /* Above the horizon there is no ground: that band is the arena backdrop,
     * and one fill per row is cheaper than any gradient the board can afford
     * while the camera is moving. */
    {
        int rows = horizon < vp->h ? horizon : vp->h;
        uint8_t *p = vp->pixels;
        int r;
        for (r = 0; r < rows; ++r) {
            Atarist_ChunkyFill(p, vp->w, sky);
            p += vp->stride;
        }
    }

    for (y = (horizon > 0 ? horizon : 0); y < vp->h; ++y) {
        uint8_t *dst = vp->pixels + (size_t)y * vp->stride;
        int32_t rows_below = ((int32_t)(y - horizon) << 16) + (1 << 15);
        int32_t depth, u, v, du;

        if (rows_below <= 0) continue;
        /* Exact perspective for a plane: every pixel on this row has the same
         * depth, so one divide serves the whole scanline and the texture step
         * is constant along it. */
        depth = fxdiv(hf, rows_below);
        if (depth > GROUND_FAR || depth <= 0) {
            Atarist_ChunkyFill(dst, vp->w, sky);
            continue;
        }
        du = fxdiv(depth, cam->focal) << texel_log2;   /* texels per pixel */
        v = v_camera + (depth << texel_log2);
        u = u_camera - du * half_w;

        Atarist_GroundSpan(dst, texels + (((v >> 16) & v_mask) << v_shift),
                           (uint32_t)u, (uint32_t)du, vp->w, u_mask);
    }
}

/* ── Convex quad rasteriser ──────────────────────────────────────────────── */

/* Walk one edge chain of the quad, producing a left or right x per scanline.
 * The quad is convex and given in order, so the chain from the topmost vertex
 * to the bottommost one on each side is unambiguous. */
typedef struct EdgeWalk {
    int32_t x, dx;
    int y_end;
} EdgeWalk;

static void edge_init(EdgeWalk *e, const AtaristVert *a, const AtaristVert *b)
{
    int32_t dy = b->y - a->y;
    e->x = a->x;
    e->dx = dy > 0 ? fxdiv(b->x - a->x, dy) : 0;
    e->y_end = (int)(b->y >> 16);
}

/* Shared skeleton: find the top and bottom vertices, then step both chains a
 * scanline at a time and hand each span to `emit`. */
typedef void (*SpanFn)(const AtaristViewport *vp, int y, int x0, int x1,
                       void *ctx);

static void raster_quad(const AtaristViewport *vp, const AtaristVert *q,
                        SpanFn emit, void *ctx)
{
    int top = 0, bottom = 0, i;
    int li, ri, y, y_bottom;
    EdgeWalk left, right;

    for (i = 1; i < 4; ++i) {
        if (q[i].y < q[top].y) top = i;
        if (q[i].y > q[bottom].y) bottom = i;
    }
    y = (int)(q[top].y >> 16);
    y_bottom = (int)(q[bottom].y >> 16);
    if (y_bottom <= y) return;

    li = ri = top;
    edge_init(&left, &q[top], &q[(top + 3) & 3]);
    edge_init(&right, &q[top], &q[(top + 1) & 3]);

    for (; y < y_bottom; ++y) {
        while (y >= left.y_end && li != bottom) {
            li = (li + 3) & 3;
            edge_init(&left, &q[li], &q[(li + 3) & 3]);
        }
        while (y >= right.y_end && ri != bottom) {
            ri = (ri + 1) & 3;
            edge_init(&right, &q[ri], &q[(ri + 1) & 3]);
        }
        if (y >= 0 && y < vp->h) {
            int x0 = (int)(left.x >> 16);
            int x1 = (int)(right.x >> 16);
            if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
            if (x0 < 0) x0 = 0;
            if (x1 > vp->w) x1 = vp->w;
            if (x1 > x0) emit(vp, y, x0, x1, ctx);
        }
        left.x += left.dx;
        right.x += right.dx;
    }
}

typedef struct FlatCtx { uint8_t colour; } FlatCtx;

static void emit_flat(const AtaristViewport *vp, int y, int x0, int x1, void *ctx)
{
    Atarist_ChunkyFill(vp->pixels + (size_t)y * vp->stride + x0, x1 - x0,
                       ((FlatCtx *)ctx)->colour);
}

void Atarist_FillQuad(const AtaristViewport *vp, const AtaristVert *quad,
                      uint8_t colour)
{
    FlatCtx ctx;
    ctx.colour = colour;
    raster_quad(vp, quad, emit_flat, &ctx);
}

/* The affine texture gradient is a property of the whole polygon, not of a
 * scanline, so it is solved once from three vertices and the inner loop is two
 * adds and a table read.  This is the shortcut the requirement allows for
 * cards: they are convex, they never fold, and their perspective error over a
 * 30-pixel-tall quad is under a texel. */
typedef struct TexCtx {
    const uint8_t *texels;
    int32_t dudx, dvdx;
    int32_t dudy, dvdy;
    int32_t u0, v0;
    int32_t x0, y0;
    int u_mask, v_mask, v_shift;
} TexCtx;

static void emit_tex(const AtaristViewport *vp, int y, int x0, int x1, void *ctx)
{
    TexCtx *t = (TexCtx *)ctx;
    uint8_t *p = vp->pixels + (size_t)y * vp->stride + x0;
    int32_t dx = ((int32_t)x0 << 16) - t->x0;
    int32_t dy = ((int32_t)y << 16) - t->y0;
    /* EVERY FIELD IS PULLED INTO A LOCAL BEFORE THE LOOP.  `t` is a pointer to
     * a struct the compiler cannot prove the stores do not alias, so left as
     * t->dudx / t->u_mask the inner loop re-read six longs from memory per
     * pixel. */
    const uint8_t *texels = t->texels;
    int32_t dudx = t->dudx, dvdx = t->dvdx;
    int u_mask = t->u_mask, v_mask = t->v_mask, v_shift = t->v_shift;
    int32_t u = t->u0 + fxmul(dudx, dx) + fxmul(t->dudy, dy);
    int32_t v = t->v0 + fxmul(dvdx, dx) + fxmul(t->dvdy, dy);
    int n = x1 - x0;
    while (n--) {
        *p++ = texels[(((v >> 16) & v_mask) << v_shift) | ((u >> 16) & u_mask)];
        u += dudx;
        v += dvdx;
    }
}

void Atarist_TexQuad(const AtaristViewport *vp, const AtaristVert *quad,
                     const AtaristTexture *tex)
{
    TexCtx t;
    int32_t ax = quad[1].x - quad[0].x, ay = quad[1].y - quad[0].y;
    int32_t bx = quad[2].x - quad[0].x, by = quad[2].y - quad[0].y;
    int32_t au = quad[1].u - quad[0].u, av = quad[1].v - quad[0].v;
    int32_t bu = quad[2].u - quad[0].u, bv = quad[2].v - quad[0].v;
    int32_t den = fxmul(ax, by) - fxmul(bx, ay);

    if (!den) return;
    t.texels = tex->texels;
    t.u_mask = (1 << tex->w_log2) - 1;
    t.v_mask = (1 << tex->h_log2) - 1;
    t.v_shift = tex->w_log2;
    t.dudx = fxdiv(fxmul(au, by) - fxmul(bu, ay), den);
    t.dvdx = fxdiv(fxmul(av, by) - fxmul(bv, ay), den);
    t.dudy = fxdiv(fxmul(ax, bu) - fxmul(bx, au), den);
    t.dvdy = fxdiv(fxmul(ax, bv) - fxmul(bx, av), den);
    t.u0 = quad[0].u;
    t.v0 = quad[0].v;
    t.x0 = quad[0].x;
    t.y0 = quad[0].y;
    raster_quad(vp, quad, emit_tex, &t);
}
