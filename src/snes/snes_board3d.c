#include "snes_board3d.h"
#include "snes_video.h"
#include "snes_textures.h"
#include "snes_cards.h"
#include "snes_stamp.h"

/* ── Board geometry ──────────────────────────────────────────────────────── */

void snesSlotCentre(u8 row, u8 col, u8 mirror, s16 *wx, s16 *wz)
{
    /* Rows 0..3 far to near, on the same one-unit pitch the columns use, so a
     * slot is a whole checkerboard tile and a card sits inside one.  They
     * straddle the origin: z = 1.5 .. -1.5 inside a slab that runs -2 .. 2. */
    static const s16 row_z[SNES_ROWS] = { 384, 128, -128, -384 };
    s16 x = (s16)(((s16)col - 2) << 8);
    s16 z = row_z[row];
    /* The yaw-128 board is the same table seen from the far side: the
     * camera's own coordinates are the world's negated, so every slot lands
     * where its diagonal opposite would. */
    if (mirror) { x = (s16)(-x); z = (s16)(-z); }
    *wx = x;
    *wz = z;
}

void snesCameraSet(SnesCamera *cam, s16 x, s16 z, s16 height, s16 focal,
                   s16 horizon)
{
    cam->x = x;
    cam->z = z;
    cam->height = height;
    cam->focal = focal;
    cam->horizon = horizon;
    cam->yaw = cam->pitch = 0;
}

/* a / b as Q8.8, with BOTH signs taken off first and the result's sign put
 * back afterwards.  The divide is unsigned because that is the shape the
 * shift-subtract in snes_math.asm wants, and doing the signs here keeps them
 * out of the loop.  The quad rasteriser needs a negative divisor as well as a
 * negative dividend: an edge that runs up-left has both. */
static s16 signed_qdiv(s16 a, s16 b)
{
    u8 neg = 0;
    if (a < 0) { a = (s16)(-a); neg ^= 1; }
    if (b < 0) { b = (s16)(-b); neg ^= 1; }
    if (!b) return 0;
    {
        const u16 q = snesUQDiv((u16)a, (u16)b);
        return neg ? (s16)(-(s16)q) : (s16)q;
    }
}

/* The unit viewport is always 128x72: the pixel viewport is that shifted. */
#define UNIT_W(vp)   ((u16)((vp)->w >> (vp)->sub))
#define UNIT_H(vp)   ((u16)((vp)->h >> (vp)->sub))
/* Q8.8 units -> pixels, and a per-unit-row quantity -> per pixel row.  The
 * shift counts are spelled out because 816-tcc turns a variable shift into
 * a loop, and because sub is only ever 0 or 1. */
#define UNIT_TO_PX(v, sub)   ((sub) ? (s16)((v) >> 7) : (s16)((v) >> 8))
#define PER_PX(v, sub)       ((sub) ? (s16)((v) >> 1) : (s16)(v))

u8 snesProjectQ(const SnesCamera *cam, const SnesViewport *vp,
                s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y)
{
    /* Camera space: +z away from the viewer, so the board's far edge is at a
     * larger z than the camera and depth = wz - cam->z. */
    s16 depth;
    s16 rx, ry;
    s16 x = wx, z = wz, h = cam->height - wy;
    if (cam->yaw) {
        x = snesQMul(wx, snesCos(cam->yaw)) - snesQMul(wz, snesSin(cam->yaw));
        z = snesQMul(wx, snesSin(cam->yaw)) + snesQMul(wz, snesCos(cam->yaw));
    }
    depth = z - cam->z;
    if (cam->pitch) {
        s16 d = depth;
        depth = snesQMul(d, snesCos(cam->pitch)) + snesQMul(h, snesSin(cam->pitch));
        h = snesQMul(h, snesCos(cam->pitch)) - snesQMul(d, snesSin(cam->pitch));
    }
    if (depth < 32) return 0;                    /* at or behind the near plane */

    /* The ratio is taken FIRST and scaled by the focal length second.  The
     * other order -- (wx - cam.x) * focal -- is the multiplication that does
     * not fit: a 2.5-unit offset times a 64-pixel focal length is 40960 in
     * Q8.8, past the top of a signed word, and that overflow is exactly the
     * Atari ST fxdiv bug wearing different clothes. */
    rx = signed_qdiv((s16)(x - cam->x), depth);
    ry = signed_qdiv(h, depth);
    /* The horizon is in pixels; the unit viewport's is that shifted. */
    *out_x = (s16)((u16)((u16)(UNIT_W(vp) >> 1) << 8) + (u16)snesQMul(rx, cam->focal));
    *out_y = (s16)((u16)((u16)(cam->horizon >> vp->sub) << 8) + (u16)snesQMul(ry, cam->focal));
    return 1;
}

/* The same projection rounded to whole PIXELS, which is all the
 * non-textured callers -- markers, the board rim, the flight path -- need. */
u8 snesProject(const SnesCamera *cam, const SnesViewport *vp,
               s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y)
{
    s16 qx, qy;
    if (!snesProjectQ(cam, vp, wx, wz, wy, &qx, &qy)) return 0;
    *out_x = UNIT_TO_PX(qx, vp->sub);
    *out_y = UNIT_TO_PX(qy, vp->sub);
    return 1;
}

/* dx * focal / hf, in Q8.8 UNITS PER PIXEL ROW: how far the board's side edge
 * moves across the screen for each scanline below the horizon.  On the 1:1
 * viewport a pixel row is half a unit row, so the focal length is halved.
 *
 * The ORDER matters and is the opposite of the one snesProject needs.  hf is
 * height*focal, a large number -- 176.0 for a camera 2.75 units up behind a
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

/* THERE IS NO BACKDROP PICTURE.  Everything the slab does not cover is flat
 * black, and that is the whole of the arena's surround. */

/* The slab's front wall, in direct colour: the sandstone in shadow. */
#define SNES_SLAB_WALL   ((u8)((3) | (2 << 3) | (0 << 6)))

/* ── The floor ───────────────────────────────────────────────────────────── */

void snesDrawFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop)
{
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    /* HALF A TILE, AND THAT HALF IS THE BOARD'S FIVE COLUMNS: a cell boundary
     * on every x.5, which is exactly the slab's own edges.  u IS Q8.8 TEXELS,
     * so half a cell is 16 << 8. */
    const u16 u_camera = (u16)((cam->x << 5)
                               + ((SNES_FLOOR_TEXELS_PER_UNIT / 2) << 8));
    const u16 v_camera = (u16)(cam->z << 5);
    const s16 d_near = SNES_BOARD_Z_NEAR - cam->z;
    const s16 d_far  = SNES_BOARD_Z_FAR  - cam->z;
    const u16 half_w = vp->w >> 1;
    const u8  sub = vp->sub;
    const u16 du_k = vp->du_k;

    /* BOTH SLAB EDGES ARE LINEAR IN THE ROW INDEX, so the board's bounds are
     * free: two divides a frame replace two a scanline. */
    const s16 step_l = edge_step(-SNES_BOARD_HALF_X - cam->x, PER_PX(cam->focal, sub), hf);
    const s16 step_r = edge_step( SNES_BOARD_HALF_X - cam->x, PER_PX(cam->focal, sub), hf);

    /* An edge stops accumulating once it reaches the side of the viewport.
     * The accumulators are Q8.8 units in a SIGNED word, so they wrap about
     * 127 units off centre; latching costs one comparison a row and is
     * exact. */
    const s16 edge_limit = (s16)((u16)(half_w >> sub) << 8);
    u16 row_base = vp->origin;
    s16 acc_l = 0, acc_r = 0;
    u8  wall = (u8)((vp->h >> 4) + 2);
    s16 wall_x0 = 0, wall_x1 = 0;
    u8  l_off = 0, r_off = 0;
    u16 y;

    for (y = 0; y < vp->h; ++y, row_base += vp->stride) {
        s16 rows_below = (s16)y - cam->horizon;
        u16 depth, du, u, v;
        s16 x0, x1;

        if (rows_below <= 0 || rows_below >= SNES_RECIP_ROWS) {
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

        depth = snesDepthAtRowSub(hf, (u8)rows_below, sub);
        if ((s16)depth > d_far) {
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        if ((s16)depth < d_near) {
            /* PAST THE NEAR EDGE IS THE SLAB'S OWN FRONT WALL, not backdrop. */
            if (wall) {
                --wall;
                if (wall_x0 > 0) snesSpanFill(row_base, (u16)wall_x0, backdrop);
                snesSpanFill(row_base + (u16)wall_x0,
                             (u16)(wall_x1 - wall_x0), SNES_SLAB_WALL);
                if (wall_x1 < (s16)vp->w)
                    snesSpanFill(row_base + (u16)wall_x1,
                                 (u16)((s16)vp->w - wall_x1), backdrop);
            } else {
                snesSpanFill(row_base, vp->w, backdrop);
            }
            continue;
        }
        du = (u16)snesQMul((s16)depth, (s16)du_k);

        x0 = (s16)half_w + UNIT_TO_PX(acc_l, sub);
        x1 = (s16)half_w + UNIT_TO_PX(acc_r, sub);
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) {
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        if (x0 > 0) snesSpanFill(row_base, (u16)x0, backdrop);
        if (x1 < (s16)vp->w) snesSpanFill(row_base + (u16)x1,
                                          (u16)((s16)vp->w - x1), backdrop);

        wall_x0 = x0;
        wall_x1 = x1;
        v = v_camera + (u16)(depth << 5);
        u = u_camera + snesMulLo(du, (u16)(s16)(x0 - (s16)half_w));
        snesSpanFloor(row_base + (u16)x0, (u16)(x1 - x0),
                      (u16)((((v >> 8) & (SNES_FLOOR_PATTERN_H - 1)) << 8)
                            | ((u >> 8) & 0xFF)),
                      (u16)(u & 0xFF), du);
    }
}

/* ── Cards ───────────────────────────────────────────────────────────────── */

/* Where a card's side edge stands after `rows` scanlines, in Q8.8 units.  A
 * card starts partway down the screen, so its accumulator has to be SEEDED,
 * and anything past the viewport latches immediately, exactly as an
 * accumulated edge would. */
static s16 edge_seed(s16 step, u16 rows, u8 *latched, s16 limit)
{
    const u16 mag = (step < 0) ? (u16)(-step) : (u16)step;
    const u16 hi = snesMulHi(mag, rows);
    const u16 lo = snesMulLo(mag, rows);
    if (hi || lo >= (u16)limit) {
        *latched = 1;
        return (step < 0) ? (s16)(-limit) : limit;
    }
    return (step < 0) ? (s16)(-(s16)lo) : (s16)lo;
}

void snesDrawCardRow(const SnesViewport *vp, const SnesCamera *cam, u8 row,
                     const u8 *faces, u8 mirror, u16 clip_y0, u16 clip_y1)
{
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    const u16 half_w = vp->w >> 1;
    const u8  sub = vp->sub;
    const s16 edge_limit = (s16)((u16)(half_w >> sub) << 8);
    /* The card's rate across it, folded in once for the row. */
    const s16 du_k = (s16)snesQMul((s16)vp->du_k,
                                   sub ? SNES_CARD32_U_NUM : SNES_CARD_U_NUM);
    /* Whether this row's faces are turned towards the far side: the COM's
     * rows from the player's seat, the player's from the COM's. */
    const u8 flip = (u8)((row <= SNES_ROW_COM_MONSTER) ^ mirror);
    s16 cx, cz, d_near, d_far;
    s16 step_l, step_r, step_pitch, acc_l, acc_r, acc_pitch;
    u16 r_top, r_bot;
    s16 y, y_end;
    u8  l_off = 0, r_off = 0, p_off = 0, col, any = 0;

    for (col = 0; col < SNES_COLS; ++col)
        if (faces[col] != SNES_CARD_NONE_FACE) any = 1;
    if (!any) return;
    if (clip_y1 > vp->h) clip_y1 = vp->h;

    /* The CENTRE column, because that is the card the row's edge accumulators
     * describe and the one the column walk starts from. */
    snesSlotCentre(row, SNES_COLS / 2, mirror, &cx, &cz);
    d_far  = (s16)(cz + SNES_CARD_HALF_Z - cam->z);
    d_near = (s16)(cz - SNES_CARD_HALF_Z - cam->z);
    if (d_near < 32) return;                     /* at or behind the near plane */

    /* The rows this board row covers, in PIXELS below the horizon: rows_below
     * = hf / depth, the far edge giving the first row and the near edge the
     * last.  Two divides for five cards. */
    r_top = (u16)UNIT_TO_PX(snesUQDiv(hf, (u16)d_far), sub);
    r_bot = (u16)UNIT_TO_PX(snesUQDiv(hf, (u16)d_near), sub);
    if (r_top >= SNES_RECIP_ROWS) return;
    if (r_bot >= SNES_RECIP_ROWS) r_bot = SNES_RECIP_ROWS - 1;

    /* THREE ACCUMULATORS FOR FIVE CARDS, AND THEY START AT THE MIDDLE ONE:
     * the centre card's two edges and the slot pitch, all three linear in
     * the row index, walked outwards from the centre because an accumulator
     * latches at the side of the viewport and a chain seeded on a latched
     * value is a chain built on a clamp. */
    step_l = edge_step((s16)(cx - SNES_CARD_HALF_X - cam->x), PER_PX(cam->focal, sub), hf);
    step_r = edge_step((s16)(cx + SNES_CARD_HALF_X - cam->x), PER_PX(cam->focal, sub), hf);
    step_pitch = edge_step((s16)256, PER_PX(cam->focal, sub), hf);
    acc_l = edge_seed(step_l, r_top, &l_off, edge_limit);
    acc_r = edge_seed(step_r, r_top, &r_off, edge_limit);
    acc_pitch = edge_seed(step_pitch, r_top, &p_off, edge_limit);

    y = (s16)(cam->horizon + (s16)r_top);
    y_end = (s16)(cam->horizon + (s16)r_bot + 1);
    if (y_end > (s16)clip_y1) y_end = (s16)clip_y1;
    if (y < 0) return;

    /* FROM HERE IT IS ASSEMBLY (snesCardRows, snes_raster.asm): the rows,
     * the depth, the texel step and the five spans.  816-tcc spent four
     * times the walker's own cost stepping these through the stack. */
    cr_y = (u16)y;
    cr_yend = (u16)y_end;
    cr_rows = r_top;
    cr_base = (u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride));
    cr_stride = vp->stride;
    cr_acc_l = acc_l;
    cr_acc_r = acc_r;
    cr_acc_p = acc_pitch;
    cr_step_l = step_l;
    cr_step_r = step_r;
    cr_step_p = step_pitch;
    cr_l_off = l_off;
    cr_r_off = r_off;
    cr_p_off = p_off;
    cr_limit = edge_limit;
    cr_hf = hf;
    cr_du_k = du_k;
    cr_d_far = d_far;
    cr_d_near = d_near;
    cr_clip_y0 = clip_y0;
    cr_sub = sub;
    cr_halfw = half_w;
    cr_w = vp->w;
    cr_flip = flip ? (u16)(sub ? 0x03E0 : 0x00F0) : 0;
    /* In COLUMN order: on the mirrored board column c sits where 4 - c
     * does. */
    for (col = 0; col < SNES_COLS; ++col)
        cr_faces[col] = faces[mirror ? 4 - col : col];
    snesCardRows();
}


/* A flat marker lying on a whole slot: the cursor, and the slot the COM is
 * acting on.  A FILL, not a texture; it covers the whole tile against a
 * card's four fifths, so a card in the slot leaves the marker as a rim. */
void snesDrawSlotMarker(const SnesViewport *vp, const SnesCamera *cam,
                        u8 row, u8 col, u8 mirror, u8 colour)
{
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    const u16 half_w = vp->w >> 1;
    const u8  sub = vp->sub;
    const s16 edge_limit = (s16)((u16)(half_w >> sub) << 8);
    const s16 half = (s16)128;                   /* half a world unit */
    s16 cx, cz, d_near, d_far, step_l, step_r, acc_l, acc_r;
    u16 r_top, r_bot, rows_below, row_base;
    u8  l_off = 0, r_off = 0;
    s16 y, y_end;

    snesSlotCentre(row, col, mirror, &cx, &cz);
    d_far  = (s16)(cz + half - cam->z);
    d_near = (s16)(cz - half - cam->z);
    if (d_near < 32) return;

    r_top = (u16)UNIT_TO_PX(snesUQDiv(hf, (u16)d_far), sub);
    r_bot = (u16)UNIT_TO_PX(snesUQDiv(hf, (u16)d_near), sub);
    if (r_top >= SNES_RECIP_ROWS) return;
    if (r_bot >= SNES_RECIP_ROWS) r_bot = SNES_RECIP_ROWS - 1;

    step_l = edge_step((s16)(cx - half - cam->x), PER_PX(cam->focal, sub), hf);
    step_r = edge_step((s16)(cx + half - cam->x), PER_PX(cam->focal, sub), hf);
    acc_l = edge_seed(step_l, r_top, &l_off, edge_limit);
    acc_r = edge_seed(step_r, r_top, &r_off, edge_limit);

    y = (s16)(cam->horizon + (s16)r_top);
    y_end = (s16)(cam->horizon + (s16)r_bot + 1);
    if (y_end > (s16)vp->h) y_end = (s16)vp->h;
    if (y < 0) return;
    row_base = (u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride));

    for (rows_below = r_top; y < y_end; ++y, ++rows_below, row_base += vp->stride) {
        u16 depth;
        s16 x0, x1;

        if (!l_off) {
            acc_l += step_l;
            if (acc_l >= edge_limit || acc_l <= -edge_limit) l_off = 1;
        }
        if (!r_off) {
            acc_r += step_r;
            if (acc_r >= edge_limit || acc_r <= -edge_limit) r_off = 1;
        }
        depth = snesDepthAtRowSub(hf, (u8)rows_below, sub);
        if ((s16)depth > d_far || (s16)depth < d_near) continue;

        x0 = (s16)half_w + UNIT_TO_PX(acc_l, sub);
        x1 = (s16)half_w + UNIT_TO_PX(acc_r, sub);
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) continue;
        snesSpanFill(row_base + (u16)x0, (u16)(x1 - x0), colour);
    }
}

/* ── The general convex-quad affine mapper ───────────────────────────────── */

u8 snesCardQuad(const SnesCamera *cam, const SnesViewport *vp,
                u8 row, u8 col, u8 mirror, s16 lift, s16 tilt, SnesVert *quad)
{
    /* Vertex 0 is the far-left corner and the texture's top-left, so a
     * painting's top points away from the camera, the way it does on every
     * other port's board. */
    static const s8 sx[4] = { -1, 1, 1, -1 };
    static const s8 sz[4] = {  1, 1, -1, -1 };
    const s16 texels = vp->sub ? SNES_CARD32_TEXELS : SNES_CARD_TEXELS;
    const u8 flip = (u8)((row <= SNES_ROW_COM_MONSTER) ^ mirror);
    s16 cx, cz;
    u8 i;

    snesSlotCentre(row, col, mirror, &cx, &cz);
    for (i = 0; i < 4; ++i) {
        s16 ox, oy;
        /* The FAR edge stands higher than the near one, so the card leans back
         * towards the player and is a genuine convex quad on screen rather
         * than a trapezoid. */
        if (!snesProjectQ(cam, vp, (s16)(cx + sx[i] * SNES_CARD_HALF_X),
                          (s16)(cz + sz[i] * SNES_CARD_HALF_Z),
                          (s16)(lift + ((sz[i] > 0) ? tilt : 0)), &ox, &oy))
            return 0;
        quad[i].x = ox;
        quad[i].y = oy;
        quad[i].u = (i == 1 || i == 2) ? (s16)((texels << 8) - 1) : 0;
        quad[i].v = (i >= 2) ? (s16)((texels << 8) - 1) : 0;
        if (flip)
            quad[i].v = (s16)((texels << 8) - 1 - quad[i].v);
    }
    return 1;
}

u8 snesQuadBounds(const SnesViewport *vp, const SnesVert *q,
                  u16 *x0, u16 *y0, u16 *x1, u16 *y1)
{
    const u8 sub = vp->sub;
    s16 ax = q[0].x, ay = q[0].y, bx = q[0].x, by = q[0].y;
    u8 i;
    for (i = 1; i < 4; ++i) {
        if (q[i].x < ax) ax = q[i].x;
        if (q[i].x > bx) bx = q[i].x;
        if (q[i].y < ay) ay = q[i].y;
        if (q[i].y > by) by = q[i].y;
    }
    ax = (s16)UNIT_TO_PX(ax, sub);
    ay = (s16)UNIT_TO_PX(ay, sub);
    bx = (s16)(UNIT_TO_PX(bx, sub) + 1);
    by = (s16)(UNIT_TO_PX(by, sub) + 1);
    if (bx <= 0 || by <= 0 || ax >= (s16)vp->w || ay >= (s16)vp->h) return 0;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx > (s16)vp->w) bx = (s16)vp->w;
    if (by > (s16)vp->h) by = (s16)vp->h;
    *x0 = (u16)ax; *y0 = (u16)ay; *x1 = (u16)bx; *y1 = (u16)by;
    return 1;
}

typedef struct EdgeWalk {
    s16 x, dx;
    s16 u, du;
    s16 v, dv;
    s16 y_end;
} EdgeWalk;

/* Edges are sampled at the pixel centre of scanline y. */
static void edge_init(EdgeWalk *e, const SnesVert *a, const SnesVert *b,
                      u8 sub, s16 y)
{
    const s16 dy = (s16)(b->y - a->y);
    const s16 sample_y = (s16)((y << (sub ? 7 : 8)) + (sub ? 64 : 128));
    s16 slope = 0, uslope = 0, vslope = 0;
    /* The quotient goes through a local before it is shifted.  816-tcc
     * shifts a call's result with `cmp #$8000 / ror` while A still holds the
     * stack pointer from the argument clean-up, so the sign of a shifted
     * RETURN VALUE is whatever bit 15 of S happens to be: an edge sloping
     * left came back as a huge positive step on every other row. */
    if (dy > 0) {
        slope = signed_qdiv((s16)(b->x - a->x), dy);
        uslope = signed_qdiv((s16)(b->u - a->u), dy);
        vslope = signed_qdiv((s16)(b->v - a->v), dy);
    }
    e->x = (s16)(a->x + snesQMul(slope, (s16)(sample_y - a->y)));
    e->u = (s16)(a->u + snesQMul(uslope, (s16)(sample_y - a->y)));
    e->v = (s16)(a->v + snesQMul(vslope, (s16)(sample_y - a->y)));
    e->dx = PER_PX(slope, sub);
    e->du = PER_PX(uslope, sub);
    e->dv = PER_PX(vslope, sub);
    e->y_end = UNIT_TO_PX(b->y, sub);
}

/* Per motion tile row, the texel columns the mapper touched.  The converter
 * skips the cells outside; a row it never touched stays min 255 / max 0. */
static void span_note(u16 y, s16 x0, s16 x1, u8 sub)
{
    /* A cell is eight lines of the 1:1 frame, four of the motion frame. */
    u8 *e = &snes_conv_rowspan[(sub ? (y >> 3) : (y >> 2)) << 1];
    if ((u8)x0 < e[0]) e[0] = (u8)x0;
    if ((u8)(x1 - 1) > e[1]) e[1] = (u8)(x1 - 1);
}

/* The pitch-only floor's rows, left for the caller to walk (in one go, or a
 * few a field): set by texture_quad, consumed by snesDrawCameraFloorBegin. */
static u8  floor_pending = 0;
static u16 floor_pending_y0 = 0, floor_pending_y1 = 0;

static void texture_quad(const SnesViewport *vp, const SnesVert *q, u8 face,
                         const SnesCamera *cam)
{
    const u8 sub = vp->sub;
    s16 dudy, dvdy, dudx_px, dvdx_px;
    s16 y, y_bottom;
    u8 top = 0, bottom = 0, li, ri, i;
    EdgeWalk left, right;
    s16 cp = 0, sp = 0, cy = 0, sn = 0;
    /* The two per-row camera terms, Q4.12, stepped rather than recomputed:
     * denom = sin(pitch) + sy * cos(pitch) and a = cos(pitch) - sy *
     * sin(pitch), both linear in the row, and sy advances by two a row. */
    s16 denom16 = 0, a16 = 0, denom_step = 0, a_step = 0;
    u16 page = 0, sheet = 0;

    if (cam) {
        cp = snesCos(cam->pitch); sp = snesSin(cam->pitch);
        cy = snesCos(cam->yaw); sn = snesSin(cam->yaw);
        /* Per PIXEL row: on the 1:1 viewport that is half a unit row, on
         * the 128x72 motion frame a whole one. */
        denom_step = sub ? (s16)(cp >> 3) : (s16)(cp >> 2);
        a_step = sub ? (s16)(-(sp >> 3)) : (s16)(-(sp >> 2));
    }

    if (face != SNES_CARD_NONE_FACE) {
        page = sub ? (u16)((u16)(face & (SNES_CARD32_SPLIT - 1)) << 10)
                   : snesCardPage(face);
        sheet = (u16)(sub && face >= SNES_CARD32_SPLIT);
    }

    for (i = 1; i < 4; ++i) {
        if (q[i].y < q[top].y) top = i;
        if (q[i].y > q[bottom].y) bottom = i;
    }
    y = (s16)UNIT_TO_PX(q[top].y, sub);
    y_bottom = (s16)UNIT_TO_PX(q[bottom].y, sub);
    if (y_bottom <= y) return;

    /* TWO CHAINS WALKED INDEPENDENTLY, each turning at its own vertex. */
    li = ri = top;
    edge_init(&left, &q[top], &q[(top + 3) & 3], sub, y);
    edge_init(&right, &q[top], &q[(top + 1) & 3], sub, y);

    if (cam) {
        /* sy in quarter unit rows: a pixel row is half a unit row at 1:1
         * and a whole one on the motion frame. */
        const s16 sy0 = sub ? (s16)((y - cam->horizon) << 1)
                            : (s16)((y - cam->horizon) << 2);
        denom16 = (s16)((s16)(sp + snesQMul(sy0, cp)) << 4);
        a16 = (s16)((s16)(cp - snesQMul(sy0, sp)) << 4);
        if (!cam->yaw) {
            /* THE PITCH-ONLY FLOOR IS MAPPED IN ASSEMBLY, EDGES INCLUDED.
             * Without yaw the slab is a trapezoid symmetric about the
             * middle of the viewport: its far and near edges are level
             * (q[0], q[1] and q[3], q[2] share a y) and its half width is
             * linear in the row.  snesFloorRowsPitch is handed the half
             * width on the first row inside the viewport and its step,
             * with the camera terms seeded for that row, and this loop is
             * not run at all: 816-tcc spent four fields a frame walking
             * these edges through the stack. */
            const s16 y_top = y;
            s16 y0 = y < 0 ? 0 : y;
            s16 y1 = y_bottom > (s16)vp->h ? (s16)vp->h : y_bottom;
            s16 sy1 = sub ? (s16)((y0 - cam->horizon) << 1)
                          : (s16)((y0 - cam->horizon) << 2);
            s16 half_top = (s16)((s16)(q[1].x - q[0].x) >> 1);
            s16 half_bot = (s16)((s16)(q[2].x - q[3].x) >> 1);
            s16 rows = (s16)(y_bottom - y_top);
            s16 dhalf = 0, half0;
            if (rows > 0) {
                const s16 d = (s16)(half_bot - half_top);
                const u16 mag = (d < 0) ? (u16)(-d) : (u16)d;
                const u16 q_ = snesUQDiv(mag, (u16)((u16)rows << 8));
                dhalf = (d < 0) ? (s16)(-(s16)q_) : (s16)q_;
            }
            /* Seeded for y0: the rows above the viewport are skipped. */
            half0 = (s16)(half_top + (s16)snesMulLo((u16)dhalf, (u16)(y0 - y_top)));
            /* Stepped BEFORE use inside the walker, so hand it the row
             * before the first. */
            half0 = (s16)(half0 - dhalf);
            snesFloorRowsSetup(half0, dhalf,
                               (s16)((s16)(sp + snesQMul(sy1, cp)) << 4), denom_step,
                               (s16)((s16)(cp - snesQMul(sy1, sp)) << 4), a_step,
                               cam->height, cam->z,
                               (u16)((cam->x << 5) + 4096), vp->origin, sub);
            if (y1 > y0) {
                floor_pending = 1;
                floor_pending_y0 = (u16)y0;
                floor_pending_y1 = (u16)y1;
            }
            return;
        }
    }

    /* The edges are stepped at the END of the body, explicitly: 816-tcc
     * miscompiles a comma-separated pair of struct compound assignments in
     * the for-increment (the left edge came back as ~right.x on every other
     * row), so nothing here `continue`s past the step.  The camera terms
     * are stepped at the START, before anything can `continue`. */
    for (; y < y_bottom; ++y) {
        s16 lx, rx, lu, lv, ru, rv;
        s16 x0, x1, dx, dy;
        u16 u, v;
        s16 denom = (s16)(denom16 >> 4), a = (s16)(a16 >> 4);
        denom16 = (s16)(denom16 + denom_step);
        a16 = (s16)(a16 + a_step);

        while (y >= left.y_end && li != bottom) {
            li = (u8)((li + 3) & 3);
            edge_init(&left, &q[li], &q[(li + 3) & 3], sub, y);
        }
        while (y >= right.y_end && ri != bottom) {
            ri = (u8)((ri + 1) & 3);
            edge_init(&right, &q[ri], &q[(ri + 1) & 3], sub, y);
        }
        lx = left.x;
        rx = right.x;
        lu = left.u; lv = left.v;
        ru = right.u; rv = right.v;
        left.x = (s16)(lx + left.dx);
        right.x = (s16)(rx + right.dx);
        left.u = (s16)(left.u + left.du);
        left.v = (s16)(left.v + left.dv);
        right.u = (s16)(right.u + right.du);
        right.v = (s16)(right.v + right.dv);
        if (y < 0 || y >= (s16)vp->h) continue;

        x0 = (s16)UNIT_TO_PX(lx, sub);
        x1 = (s16)UNIT_TO_PX(rx, sub);
        if (x1 < x0) {
            s16 t = x0; x0 = x1; x1 = t;
            t = lx; lx = rx; rx = t;
            t = lu; lu = ru; ru = t;
            t = lv; lv = rv; rv = t;
        }
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) continue;

        if (cam) {
            /* Inverse camera ray / horizontal plane intersection.  The
             * pixel viewport has two pixels per projection unit, so a pixel
             * row is half a unit row: sy is in quarter unit rows, the scale
             * the reciprocal table and the pitch terms are in.
             *
             * EVERY PRODUCT HERE IS A SHIFT OR THE PPU MULTIPLIER.  816-tcc
             * turns `a * 32` and `y * stride` into a 16-step software
             * multiply of three hundred cycles each; five of those a row was
             * more than the row's pixels cost. */
            s16 depth, z, dtex, du, dv, half_du;
            u16 row_index, tu, tv;
            if (denom <= 2 || denom >= 1024) continue;
            depth = snesQMul(cam->height, snes_recip_plane[denom]);
            z = (s16)(cam->z + snesQMul(depth, a));
            /* THE TEXTURE STEP IS TAKEN IN TEXELS, NOT IN WORLD UNITS.  A
             * pixel is depth/128 world units across and a unit is 32
             * texels, so the step is depth/4 texels: rounded ONCE, here.
             * Rounding it to whole world Q8.8 counts first (depth >> 7, then
             * << 5) threw away up to a tenth of the step, and a tenth of a
             * texel a pixel is twenty texels of drift by the far side of the
             * screen -- the "cards sliding off their slots" of a moving
             * board.  The row's origin is built from the same rounded step,
             * so the walk and the origin agree. */
            dtex = sub ? (s16)((depth + 2) >> 2) : (s16)((depth + 1) >> 1);
            if (!cam->yaw) {
                du = dtex;
                dv = 0;
                tu = (u16)((cam->x << 5) + 4096);
                tv = (u16)(z << 5);
            } else {
                du = snesQMul(dtex, cy);
                dv = (s16)(-snesQMul(dtex, sn));
                tu = (u16)((s16)((snesQMul(cam->x, cy) + snesQMul(z, sn)) << 5) + 4096);
                tv = (u16)((s16)(snesQMul(z, cy) - snesQMul(cam->x, sn)) << 5);
            }
            /* From the middle of the row to the first pixel's CENTRE: the
             * half step is the same pixel-centre convention the ROM floor
             * is generated with. */
            half_du = (s16)(du >> 1);
            tu = (u16)(tu + snesMulLo((u16)du, (u16)(x0 - (vp->w >> 1))) + (u16)half_du);
            half_du = (s16)(dv >> 1);
            tv = (u16)(tv + snesMulLo((u16)dv, (u16)(x0 - (vp->w >> 1))) + (u16)half_du);
            u = tu;
            v = tv;
            span_note((u16)y, x0, x1, sub);
            row_index = (u16)(vp->origin + (sub ? ((u16)y << 8) : ((u16)y << 7))
                              + (u16)x0);
            if (!cam->yaw) {
                /* No yaw: v is constant along the row, so this is the
                 * constant-v walker over the world texture, pre-stepped
                 * the way the flat card spans are. */
                u = (u16)(u - du);
                snesSpanFloorTex(row_index, (u16)(x1 - x0),
                                 (u16)((v & 0x7F00) | ((u >> 8) & 0xFF)),
                                 (u16)(u & 0xFF), du);
                continue;
            }
            snesSpanFloorQuad(row_index, (u16)(x1 - x0), u - du, v - dv, du, dv);
            continue;
        }
        if (face == SNES_CARD_NONE_FACE) {
            span_note((u16)y, x0, x1, sub);
            snesSpanFill((u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride) + x0),
                         (u16)(x1 - x0), SNES_SLAB_WALL);
            continue;
        }
        /* Interpolate between both walked edges.  This uses all four UV
         * corners, unlike the former three-corner affine plane. */
        dx = (s16)(rx - lx);
        if (!dx) continue;
        dudy = signed_qdiv((s16)(ru - lu), dx);
        dvdy = signed_qdiv((s16)(rv - lv), dx);
        dudx_px = PER_PX(dudy, sub);
        dvdx_px = PER_PX(dvdy, sub);
        dy = (s16)(((x0 << (sub ? 7 : 8)) + (sub ? 64 : 128)) - lx);
        u = (u16)(lu + snesQMul(dudy, dy));
        v = (u16)(lv + snesQMul(dvdy, dy));
        /* Pre-stepped for the same reason the flat card is: the walker adds
         * before it reads. */
        if (sub)
            snesSpanCardQuad32((u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride)
                                     + (u16)x0),
                               (u16)(x1 - x0), (u16)(u - dudx_px), (u16)(v - dvdx_px),
                               (u16)dudx_px, (u16)dvdx_px, page, sheet);
        else
            snesSpanCardQuad((u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride)
                                   + (u16)x0),
                             (u16)(x1 - x0), (u16)(u - dudx_px), (u16)(v - dvdx_px),
                             (u16)dudx_px, (u16)dvdx_px, page);
    }
}
void snesTexQuad(const SnesViewport *vp, const SnesVert *q, u8 face)
{
    texture_quad(vp, q, face, 0);
}

u8 snesDrawCameraFloorBegin(const SnesViewport *vp, const SnesCamera *cam,
                            u8 backdrop, u8 clear, u16 *y0, u16 *y1)
{
    static const s16 xs[4] = { -640, 640, 640, -640 };
    static const s16 zs[4] = { 512, 512, -512, -512 };
    SnesVert q[4], wall[4];
    u8 i, j;
    s16 eye_x = snesQMul(cam->z, snesSin(cam->yaw));
    s16 eye_z = snesQMul(cam->z, snesCos(cam->yaw));
    for (i = 0; i < SNES_CELL_ROWS * 2; i += 2) {
        snes_conv_rowspan[i] = 255;
        snes_conv_rowspan[i + 1] = 0;
    }
    floor_pending = 0;
    if (clear)
        snesFbWramFill(vp->origin, vp->bank, (u16)(vp->stride * vp->h), backdrop);
    for (i = 0; i < 4; ++i) {
        if (!snesProjectQ(cam, vp, xs[i], zs[i], 0, &q[i].x, &q[i].y)) return 0;
        q[i].u = (i == 1 || i == 2) ? 4095 : 0;
        q[i].v = (i >= 2) ? 4095 : 0;
    }
    /* Only outward-facing walls can be seen from this camera. */
    for (i = 0; i < 4; ++i) {
        if ((i == 0 && eye_z <= 512) || (i == 1 && eye_x <= 640) ||
            (i == 2 && eye_z >= -512) || (i == 3 && eye_x >= -640)) continue;
        j = (i + 1) & 3;
        wall[0] = q[i]; wall[1] = q[j];
        wall[2] = q[j]; wall[3] = q[i];
        if (!snesProjectQ(cam, vp, xs[j], zs[j], -64, &wall[2].x, &wall[2].y) ||
            !snesProjectQ(cam, vp, xs[i], zs[i], -64, &wall[3].x, &wall[3].y))
            continue;
        texture_quad(vp, wall, SNES_CARD_NONE_FACE, 0);
    }
    texture_quad(vp, q, 0, cam);
    if (!floor_pending) return 0;
    *y0 = floor_pending_y0;
    *y1 = floor_pending_y1;
    return 1;
}

void snesDrawCameraFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop)
{
    u16 y0, y1;
    if (snesDrawCameraFloorBegin(vp, cam, backdrop, 1, &y0, &y1))
        snesFloorRowsPitch(y0, y1);
}
