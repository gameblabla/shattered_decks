#include "snes_board3d.h"
#include "snes_video.h"
#include "snes_textures.h"

/* ── Board geometry ──────────────────────────────────────────────────────── */

void snesSlotCentre(u8 row, u8 col, s16 *wx, s16 *wz)
{
    /* Rows 0..3 far to near, on the same one-unit pitch the columns use, so a
     * slot is a whole checkerboard tile and a card sits inside one.  They
     * straddle the origin: z = 1.5 .. -1.5 inside a slab that runs -2 .. 2. */
    static const s16 row_z[SNES_ROWS] = { 384, 128, -128, -384 };
    *wx = (s16)(((s16)col - 2) << 8);
    *wz = row_z[row];
}

void snesCameraSet(SnesCamera *cam, s16 x, s16 z, s16 height, s16 focal,
                   s16 horizon)
{
    cam->x = x;
    cam->z = z;
    cam->height = height;
    cam->focal = focal;
    cam->horizon = horizon;
}

/* |a| / b as Q8.8, with the sign put back afterwards.  The divide is unsigned
 * because that is the shape the shift-subtract in snes_math.asm wants, and
 * doing the sign here keeps it out of the loop. */
static s16 signed_qdiv(s16 a, u16 b)
{
    if (a < 0) return -(s16)snesUQDiv((u16)(-a), b);
    return (s16)snesUQDiv((u16)a, b);
}

u8 snesProject(const SnesCamera *cam, const SnesViewport *vp,
               s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y)
{
    /* Camera space: +z away from the viewer, so the board's far edge is at a
     * larger z than the camera and depth = wz - cam->z. */
    s16 depth = wz - cam->z;
    s16 rx, ry;
    if (depth < 32) return 0;                    /* at or behind the near plane */

    /* The ratio is taken FIRST and scaled by the focal length second.  The
     * other order -- (wx - cam.x) * focal -- is the multiplication that does
     * not fit: a 2.5-unit offset times a 64-pixel focal length is 40960 in
     * Q8.8, past the top of a signed word, and that overflow is exactly the
     * Atari ST fxdiv bug wearing different clothes. */
    rx = signed_qdiv(wx - cam->x, (u16)depth);
    ry = signed_qdiv(cam->height - wy, (u16)depth);
    *out_x = (s16)((vp->w >> 1) + ((s16)snesMulLo((u16)rx, (u16)cam->focal) >> 8));
    *out_y = (s16)(cam->horizon + ((s16)snesMulLo((u16)ry, (u16)cam->focal) >> 8));
    return 1;
}

/* dx * focal / hf, in Q8.8: how far the board's side edge moves across the
 * screen for each scanline below the horizon.
 *
 * The ORDER matters and is the opposite of the one snesProject needs.  hf is
 * height*focal, a large number -- 96.0 for a camera 1.5 units up behind a
 * 64-pixel focal length -- so taking the ratio dx/hf first leaves a Q8.8
 * quotient of about six counts and throws away every significant bit of the
 * answer.  Multiplying first keeps them, and the product only fits because it
 * is done UNSIGNED: 2.5 units by a focal length of 64 is 40960, past the top
 * of a signed word but comfortably inside an unsigned one.  The sign is put
 * back at the end.
 */
static s16 edge_step(s16 dx, s16 focal, u16 hf)
{
    const u16 mag = (dx < 0) ? (u16)(-dx) : (u16)dx;
    /* (mag * focal) >> 8, unsigned, out of the two halves of the product. */
    const u16 num = (u16)((snesMulLo(mag, (u16)focal) >> 8)
                          | (snesMulHi(mag, (u16)focal) << 8));
    const u16 q = snesUQDiv(num, hf);
    return (dx < 0) ? (s16)(-(s16)q) : (s16)q;
}

/* One row of the backdrop.  Same walker as the floor, reading the desert
 * painting instead of the sandstone: the sky has constant v down a screen row
 * too, so it costs the same two adds a texel. */
static void sky_row(u16 row_base, u8 w, u16 v, u16 u, u16 du)
{
    snesSpanHorizon(row_base, w,
                    (u16)((((v >> 8) & (SNES_HORIZON_H - 1)) << 8)
                          | ((u >> 8) & 0xFF)),
                    (u16)(u & 0xFF), du);
}

/* ── The floor ───────────────────────────────────────────────────────────── */

void snesDrawFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop)
{
    /* Loop invariants, hoisted.  height*focal is the whole numerator of the
     * plane equation, and the camera's own position in texels does not depend
     * on the row. */
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    const u16 u_camera = (u16)(cam->x << 5);     /* SNES_FLOOR_TEXELS_PER_UNIT */
    const u16 v_camera = (u16)(cam->z << 5);
    const s16 d_near = SNES_BOARD_Z_NEAR - cam->z;
    const s16 d_far  = SNES_BOARD_Z_FAR  - cam->z;
    const u8  half_w = vp->w >> 1;
    /* du = depth * texels_per_unit / focal, and both are powers of two chosen
     * so the ratio is a shift: 32 texels a unit over a focal length of half
     * the viewport width. */
    const u8  du_shift = (vp->w >= 128) ? 1 : 0;

    /* BOTH SLAB EDGES ARE LINEAR IN THE ROW INDEX, and that is the whole
     * reason the board's bounds are free.  The projected x of a fixed world x
     * is focal * dx / depth, depth is hf / rows_below, so the offset is
     * dx * focal * rows_below / hf -- a constant times the row.  Two divides a
     * frame replace two a scanline. */
    const s16 step_l = edge_step(-SNES_BOARD_HALF_X - cam->x, cam->focal, hf);
    const s16 step_r = edge_step( SNES_BOARD_HALF_X - cam->x, cam->focal, hf);

    /* THE BACKDROP RUNS DOWN TO THE SLAB'S FAR EDGE, not to the horizon line.
     * Rows between the two are ground the board does not cover, and a flat
     * fill there reads as a hole cut in the picture; the desert painting the
     * other ports use is what is actually behind the arena, so it is stretched
     * over exactly that band and its own horizon lands where the board's far
     * edge does.  rows_below at the far edge is hf / d_far -- the same plane
     * equation the loop uses, solved the other way round. */
    const u16 sky_rows = (u16)cam->horizon
                       + ((d_far > 0) ? (snesUQDiv(hf, (u16)d_far) >> 8) : 0);
    const u16 sky_dv = (sky_rows > 0) ? snesUQDiv(SNES_HORIZON_H, sky_rows) : 0;
    const u16 sky_du = snesUQDiv(SNES_HORIZON_W, (u16)vp->w);
    const u16 sky_u  = (u16)(cam->x << 3);   /* a quarter of the floor's pan */
    u16 sky_v = 0;

    /* An edge stops accumulating once it reaches the side of the viewport.
     * The accumulators are Q8.8 in a SIGNED word, so they wrap about 127
     * pixels off centre -- which a slab edge passes well before the bottom of
     * the board, and the wrap turns the near end of the board into backdrop.
     * Latching costs one comparison a row and is exact: the steps are constant
     * and opposite, so an edge that has left the screen never comes back. */
    const s16 edge_limit = (s16)((u16)half_w << 8);
    u16 row_base = vp->origin;
    s16 acc_l = 0, acc_r = 0;
    u8  l_off = 0, r_off = 0;
    u8  y;

    for (y = 0; y < vp->h; ++y, row_base += vp->stride) {
        s16 rows_below = (s16)y - cam->horizon;
        u16 depth, du, u, v;
        s16 x0, x1;

        if (rows_below <= 0) {
            sky_row(row_base, vp->w, sky_v, sky_u, sky_du);
            sky_v += sky_dv;
            continue;
        }
        if (rows_below >= SNES_RECIP_ROWS) {
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        if (!l_off) {
            acc_l += step_l;
            if (acc_l >= edge_limit || acc_l <= -edge_limit) l_off = 1;
        }
        if (!r_off) {
            acc_r += step_r;
            if (acc_r >= edge_limit || acc_r <= -edge_limit) r_off = 1;
        }

        /* Exact perspective for a plane: every pixel on this row has the same
         * depth, so one table read serves the whole scanline and the texture
         * step is constant along it. */
        depth = snesDepthAtRow(hf, (u8)rows_below);
        if ((s16)depth > d_far) {          /* still behind the slab's far edge */
            sky_row(row_base, vp->w, sky_v, sky_u, sky_du);
            sky_v += sky_dv;
            continue;
        }
        if ((s16)depth < d_near) {         /* in front of it: the near surround */
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        du = depth >> du_shift;

        x0 = (s16)half_w + (acc_l >> 8);
        x1 = (s16)half_w + (acc_r >> 8);
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) {
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        if (x0 > 0) snesSpanFill(row_base, (u16)x0, backdrop);
        if (x1 < (s16)vp->w) snesSpanFill(row_base + (u16)x1,
                                          (u16)((s16)vp->w - x1), backdrop);

        v = v_camera + (u16)(depth << 5);
        u = u_camera + snesMulLo(du, (u16)(s16)(x0 - (s16)half_w));
        /* (v << 8) | u_int is the texture index the span walker keeps in X.
         * v is masked to the texture's 64-row pattern here so the inner loop
         * does not have to; u needs no mask at all, because the walker's
         * eight-bit add wraps it inside its own 256-byte row. */
        snesSpanFloor(row_base + (u16)x0, (u16)(x1 - x0),
                      (u16)((((v >> 8) & (SNES_FLOOR_PATTERN_H - 1)) << 8)
                            | ((u >> 8) & 0xFF)),
                      (u16)(u & 0xFF), du);
    }
}
