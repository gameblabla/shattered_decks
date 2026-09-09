#include "snes_board3d.h"
#include "snes_video.h"
#include "snes_textures.h"
#include "snes_cards.h"

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
    *out_x = (s16)((u16)((u16)(vp->w >> 1) << 8) + (u16)snesQMul(rx, cam->focal));
    *out_y = (s16)((u16)((u16)cam->horizon << 8) + (u16)snesQMul(ry, cam->focal));
    return 1;
}

/* The same projection rounded to whole viewport pixels, which is all the
 * non-textured callers -- markers, the board rim -- need. */
u8 snesProject(const SnesCamera *cam, const SnesViewport *vp,
               s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y)
{
    s16 qx, qy;
    if (!snesProjectQ(cam, vp, wx, wz, wy, &qx, &qy)) return 0;
    *out_x = (s16)(qx >> 8);
    *out_y = (s16)(qy >> 8);
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

/* THERE IS NO BACKDROP PICTURE.  Everything the slab does not cover is flat
 * black, and that is the whole of the arena's surround: the board is the only
 * textured thing on the screen.  A painted sky cost a second span walker, a
 * second 16 KB texture bank and the top eighth of every frame, to put a band
 * of quantised desert behind a board the player is looking down at -- and in
 * two bits of blue it read as banding, not as sky. */

/* The slab's front wall, in direct colour: the sandstone in shadow.  It is a
 * flat tone rather than a sampled texture because it is four to seven rows
 * tall and a texture read over that is detail nobody sees at the cost of the
 * whole wall's pixels going through the span walker. */
#define SNES_SLAB_WALL   ((u8)((3) | (2 << 3) | (0 << 6)))

/* ── The floor ───────────────────────────────────────────────────────────── */

void snesDrawFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop)
{
    /* Loop invariants, hoisted.  height*focal is the whole numerator of the
     * plane equation, and the camera's own position in texels does not depend
     * on the row. */
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    /* HALF A TILE, AND THAT HALF IS THE BOARD'S FIVE COLUMNS.
     *
     * A slot centre is at an INTEGER world x (snesSlotCentre: col - 2) while
     * the checkerboard's cells break at integer x too, so without this the
     * grid line runs straight down the middle of every slot: the slab's
     * -2.5..2.5 shows as four whole tiles with a half tile at each end, and
     * the centre card sits on the seam between two of them.  Shifting the
     * texture half a cell puts a cell boundary at every x.5 instead, which is
     * exactly the slab's own edges -- five whole tiles across, twenty on the
     * board.  The rows already line up: they are centred on the half-integers
     * z = +-0.5, +-1.5 inside a slab that runs -2..2. */
    /* u IS Q8.8 TEXELS, so half a cell is 16 << 8 and not 16.  Sixteen alone
     * is a sixteenth of a texel, which moves the grid by nothing and leaves
     * the seam exactly where it was. */
    const u16 u_camera = (u16)((cam->x << 5)
                               + ((SNES_FLOOR_TEXELS_PER_UNIT / 2) << 8));
    const u16 v_camera = (u16)(cam->z << 5);
    const s16 d_near = SNES_BOARD_Z_NEAR - cam->z;
    const s16 d_far  = SNES_BOARD_Z_FAR  - cam->z;
    const u8  half_w = vp->w >> 1;
    /* du = depth * texels_per_unit / focal, and the ratio is the viewport's
     * own du_k -- one multiply a scanline, because the bend's focal length is
     * not a power of two.  See SnesViewport. */
    const u16 du_k = vp->du_k;

    /* BOTH SLAB EDGES ARE LINEAR IN THE ROW INDEX, and that is the whole
     * reason the board's bounds are free.  The projected x of a fixed world x
     * is focal * dx / depth, depth is hf / rows_below, so the offset is
     * dx * focal * rows_below / hf -- a constant times the row.  Two divides a
     * frame replace two a scanline. */
    const s16 step_l = edge_step(-SNES_BOARD_HALF_X - cam->x, cam->focal, hf);
    const s16 step_r = edge_step( SNES_BOARD_HALF_X - cam->x, cam->focal, hf);

    /* An edge stops accumulating once it reaches the side of the viewport.
     * The accumulators are Q8.8 in a SIGNED word, so they wrap about 127
     * pixels off centre -- which a slab edge passes well before the bottom of
     * the board, and the wrap turns the near end of the board into backdrop.
     * Latching costs one comparison a row and is exact: the steps are constant
     * and opposite, so an edge that has left the screen never comes back. */
    const s16 edge_limit = (s16)((u16)half_w << 8);
    u16 row_base = vp->origin;
    s16 acc_l = 0, acc_r = 0;
    /* The slab's front wall: how many rows of it are left, and the near edge's
     * x range, which is the wall's. */
    u8  wall = (u8)((vp->h >> 4) + 2);
    s16 wall_x0 = 0, wall_x1 = 0;
    u8  l_off = 0, r_off = 0;
    u8  y;

    for (y = 0; y < vp->h; ++y, row_base += vp->stride) {
        s16 rows_below = (s16)y - cam->horizon;
        u16 depth, du, u, v;
        s16 x0, x1;

        if (rows_below <= 0) {
            snesSpanFill(row_base, vp->w, backdrop);
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
            snesSpanFill(row_base, vp->w, backdrop);
            continue;
        }
        if ((s16)depth < d_near) {
            /* PAST THE NEAR EDGE IS THE SLAB'S OWN FRONT WALL, not backdrop.
             *
             * The board the other ports show is a slab with thickness, and the
             * band of stone under its near rim is most of what says it is an
             * object standing on the ground rather than a rug painted on it.
             * The wall's world-space sides are vertical, and a vertical edge
             * over a handful of screen rows is within a pixel of the near
             * edge's own x range -- so it costs one fill a row and no geometry
             * at all. */
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

        wall_x0 = x0;
        wall_x1 = x1;
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

/* ── Cards ───────────────────────────────────────────────────────────────── */

/* Where a card's side edge stands after `rows` scanlines, in Q8.8 pixels.
 *
 * The floor accumulates its edges from the horizon down, one add a row, and
 * latches them at the viewport's side before the signed word can wrap.  A card
 * starts partway down the screen, so its accumulator has to be SEEDED, and the
 * seed is where the wrap would come back: step * rows is a Q8.8 pixel count
 * that reaches thousands for a near card whose edge is far off screen.  So the
 * product is taken out of both halves of the 32-bit result and anything past
 * the viewport latches immediately, exactly as an accumulated edge would.
 */
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

/* One card's span on one screen row: the row has already produced the depth,
 * the texture step and v, and the column walk produces the two edges. */
static void card_span(u16 row_base, const SnesViewport *vp, u8 face, u8 half_w,
                      s16 l, s16 r, u16 du, u16 tex_v)
{
    s16 xl, x0, x1;
    u16 u;

    if (face == SNES_CARD_NONE_FACE) return;
    xl = (s16)half_w + (l >> 8);
    x0 = xl;
    x1 = (s16)half_w + (r >> 8);
    if (x0 < 0) x0 = 0;
    if (x1 > (s16)vp->w) x1 = (s16)vp->w;
    if (x1 <= x0) return;

    /* A span that starts on the card's own left edge starts at texel zero, so
     * only a card clipped by the side of the viewport pays a multiply -- which
     * is at most two of the twenty on the board. */
    u = (x0 > xl) ? snesMulLo(du, (u16)(s16)(x0 - xl)) : 0;
    /* The walker steps before it reads, so it is handed the texel BEFORE the
     * first one; without this a card loses its left keyline column, which at
     * sixteen texels is a sixteenth of the picture. */
    u = (u >= 0x8000u || u < du) ? 0 : (u16)(u - du);
    if (u > 0x0FFF) u = 0x0FFF;

    snesSpanCard(row_base + (u16)x0, (u16)(x1 - x0),
                 (u16)(snesCardPage(face) | tex_v | ((u >> 8) & 0x000F)),
                 (u16)(u & 0xFF), du);
}

void snesDrawCardRow(const SnesViewport *vp, const SnesCamera *cam, u8 row,
                     const u8 *faces)
{
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    const u8  half_w = vp->w >> 1;
    const s16 edge_limit = (s16)((u16)half_w << 8);
    /* Two thirds of the floor's rate across the card, because sixteen texels
     * cover three quarters of a unit there against the floor's thirty-two over
     * a whole one.  Taken once for the row, not once a scanline. */
    const s16 du_k = (s16)snesQMul((s16)vp->du_k, SNES_CARD_U_NUM);
    s16 cx, cz, d_near, d_far;
    s16 step_l, step_r, step_pitch, acc_l, acc_r, acc_pitch;
    u16 r_top, r_bot, rows_below, row_base;
    s16 y, y_end;
    u8  l_off = 0, r_off = 0, p_off = 0, col, any = 0;

    for (col = 0; col < SNES_COLS; ++col)
        if (faces[col] != SNES_CARD_NONE_FACE) any = 1;
    if (!any) return;

    /* The CENTRE column, because that is the card the row's edge accumulators
     * describe and the one the column walk starts from. */
    snesSlotCentre(row, SNES_COLS / 2, &cx, &cz);
    d_far  = (s16)(cz + SNES_CARD_HALF_Z - cam->z);
    d_near = (s16)(cz - SNES_CARD_HALF_Z - cam->z);
    if (d_near < 32) return;                     /* at or behind the near plane */

    /* The rows this board row covers are the plane equation solved the other
     * way round: rows_below = hf / depth, the far edge giving the first row
     * and the near edge the last.  Two divides for five cards. */
    r_top = (u16)(snesUQDiv(hf, (u16)d_far) >> 8);
    r_bot = (u16)(snesUQDiv(hf, (u16)d_near) >> 8);
    if (r_top >= SNES_RECIP_ROWS) return;
    if (r_bot >= SNES_RECIP_ROWS) r_bot = SNES_RECIP_ROWS - 1;

    /* THREE ACCUMULATORS FOR FIVE CARDS, AND THEY START AT THE MIDDLE ONE.
     *
     * The columns are one world unit apart and every one of them is the same
     * quad translated, so on any screen row the slot pitch is the same number
     * of pixels for all of them -- it is the projection of one world unit at
     * that depth.  So the row carries the left and right edges of the CENTRE
     * card and the pitch, all three linear in the row index exactly as the
     * slab's own edges are, and a column then costs one add rather than a pair
     * of edges of its own.
     *
     * It walks outwards from the centre because an edge accumulator latches at
     * the side of the viewport -- a Q8.8 pixel count wraps about 127 pixels off
     * centre, and the near row's outer columns are further off than that.  A
     * chain seeded on a LATCHED value is a chain built on a clamp, which put
     * the whole near row in the wrong place; seeded on the centre column it is
     * built on a real number, because the centre card is never off screen (the
     * camera's sway is smaller than the card is wide).  Walking outwards then
     * stops at the first column past the viewport, which is also what keeps
     * the running total inside a signed word.
     */
    step_l = edge_step((s16)(cx - SNES_CARD_HALF_X - cam->x), cam->focal, hf);
    step_r = edge_step((s16)(cx + SNES_CARD_HALF_X - cam->x), cam->focal, hf);
    step_pitch = edge_step((s16)256, cam->focal, hf);
    acc_l = edge_seed(step_l, r_top, &l_off, edge_limit);
    acc_r = edge_seed(step_r, r_top, &r_off, edge_limit);
    acc_pitch = edge_seed(step_pitch, r_top, &p_off, edge_limit);

    y = (s16)(cam->horizon + (s16)r_top);
    y_end = (s16)(cam->horizon + (s16)r_bot + 1);
    if (y_end > (s16)vp->h) y_end = (s16)vp->h;
    if (y < 0) return;
    row_base = (u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride));

    for (rows_below = r_top; y < y_end; ++y, ++rows_below, row_base += vp->stride) {
        u16 depth, du, v, tex_v;
        s16 l, r;
        s8  col_i;

        if (!l_off) {
            acc_l += step_l;
            if (acc_l >= edge_limit || acc_l <= -edge_limit) l_off = 1;
        }
        if (!r_off) {
            acc_r += step_r;
            if (acc_r >= edge_limit || acc_r <= -edge_limit) r_off = 1;
        }
        if (!p_off) {
            acc_pitch += step_pitch;
            if (acc_pitch >= edge_limit) p_off = 1;
        }

        depth = snesDepthAtRow(hf, (u8)rows_below);
        /* The row range came out of a divide and the depth out of a table, so
         * the two disagree by a fraction of a row at each end; this is what
         * stops that disagreement sampling past the card. */
        if ((s16)depth > d_far || (s16)depth < d_near) continue;

        /* Across: two thirds of the floor's rate, folded into du_k above.
         * Down: the card is exactly one unit deep and sixteen texels tall, so
         * v is the depth into the card shifted four, with nothing to scale. */
        du = (u16)snesQMul((s16)depth, du_k);
        v = (u16)(d_far - (s16)depth);
        v = (u16)((u16)v << 4);
        if (v > 0x0FFF) v = 0x0FFF;
        tex_v = (u16)((v >> 4) & 0x00F0);
        if (row <= SNES_ROW_COM_MONSTER) tex_v ^= 0x00F0;

        /* Outwards from the centre: right first, then left. */
        l = acc_l;
        r = acc_r;
        for (col_i = SNES_COLS / 2; col_i < SNES_COLS; ++col_i) {
            if (l >= edge_limit) break;
            card_span(row_base, vp, faces[col_i], half_w, l, r, du, tex_v);
            if (l > (s16)(edge_limit - acc_pitch)) break;
            l += acc_pitch;
            r += acc_pitch;
        }
        l = (s16)(acc_l - acc_pitch);
        r = (s16)(acc_r - acc_pitch);
        for (col_i = SNES_COLS / 2 - 1; col_i >= 0; --col_i) {
            if (r <= -edge_limit) break;
            card_span(row_base, vp, faces[col_i], half_w, l, r, du, tex_v);
            if (r < (s16)(acc_pitch - edge_limit)) break;
            l -= acc_pitch;
            r -= acc_pitch;
        }
    }
}


/* A flat marker lying on a whole slot: the cursor, and the slot the COM is
 * acting on.
 *
 * It is a FILL, not a texture, and that is the same trade every other port
 * makes -- a marker that cost a texture fetch a pixel would be paid on every
 * frame the cursor moves, which is exactly the frames that must stay cheap.
 * It covers the whole tile against a card's four fifths, so a card sitting in
 * the slot leaves the marker showing as a rim around it. */
void snesDrawSlotMarker(const SnesViewport *vp, const SnesCamera *cam,
                        u8 row, u8 col, u8 colour)
{
    const u16 hf = (u16)snesQMul(cam->height, cam->focal);
    const u8  half_w = vp->w >> 1;
    const s16 edge_limit = (s16)((u16)half_w << 8);
    const s16 half = (s16)128;                   /* half a world unit */
    s16 cx, cz, d_near, d_far, step_l, step_r, acc_l, acc_r;
    u16 r_top, r_bot, rows_below, row_base;
    u8  l_off = 0, r_off = 0;
    s16 y, y_end;

    snesSlotCentre(row, col, &cx, &cz);
    d_far  = (s16)(cz + half - cam->z);
    d_near = (s16)(cz - half - cam->z);
    if (d_near < 32) return;

    r_top = (u16)(snesUQDiv(hf, (u16)d_far) >> 8);
    r_bot = (u16)(snesUQDiv(hf, (u16)d_near) >> 8);
    if (r_top >= SNES_RECIP_ROWS) return;
    if (r_bot >= SNES_RECIP_ROWS) r_bot = SNES_RECIP_ROWS - 1;

    step_l = edge_step((s16)(cx - half - cam->x), cam->focal, hf);
    step_r = edge_step((s16)(cx + half - cam->x), cam->focal, hf);
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
        depth = snesDepthAtRow(hf, (u8)rows_below);
        if ((s16)depth > d_far || (s16)depth < d_near) continue;

        x0 = (s16)half_w + (acc_l >> 8);
        x1 = (s16)half_w + (acc_r >> 8);
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) continue;
        snesSpanFill(row_base + (u16)x0, (u16)(x1 - x0), colour);
    }
}

/* ── The general convex-quad affine mapper ───────────────────────────────── */

u8 snesCardQuad(const SnesCamera *cam, const SnesViewport *vp,
                u8 row, u8 col, s16 lift, s16 tilt, SnesVert *quad)
{
    /* Vertex 0 is the far-left corner and the texture's top-left, so a
     * painting's top points away from the camera, the way it does on every
     * other port's board. */
    static const s8 sx[4] = { -1, 1, 1, -1 };
    static const s8 sz[4] = {  1, 1, -1, -1 };
    s16 cx, cz;
    u8 i;

    snesSlotCentre(row, col, &cx, &cz);
    for (i = 0; i < 4; ++i) {
        s16 ox, oy;
        /* The FAR edge stands higher than the near one, so the card leans back
         * towards the player and is a genuine convex quad on screen rather
         * than a trapezoid -- which is the case the two edge chains exist for
         * and the one a trapezoid walk gets wrong. */
        if (!snesProjectQ(cam, vp, (s16)(cx + sx[i] * SNES_CARD_HALF_X),
                          (s16)(cz + sz[i] * SNES_CARD_HALF_Z),
                          (s16)(lift + ((sz[i] > 0) ? tilt : 0)), &ox, &oy))
            return 0;
        quad[i].x = ox;
        quad[i].y = oy;
        quad[i].u = (i == 1 || i == 2) ? (s16)((SNES_CARD_TEXELS << 8) - 1) : 0;
        quad[i].v = (i >= 2) ? (s16)((SNES_CARD_TEXELS << 8) - 1) : 0;
        if (row <= SNES_ROW_COM_MONSTER)
            quad[i].v = (s16)((SNES_CARD_TEXELS << 8) - 1 - quad[i].v);
    }
    return 1;
}

typedef struct EdgeWalk {
    s16 x, dx;
    s16 y_end;
} EdgeWalk;

static void edge_init(EdgeWalk *e, const SnesVert *a, const SnesVert *b)
{
    const s16 dy = (s16)(b->y - a->y);
    e->x = a->x;
    e->dx = (dy > 0) ? signed_qdiv((s16)(b->x - a->x), dy) : 0;
    e->y_end = (s16)(b->y >> 8);
}

static void texture_quad(const SnesViewport *vp, const SnesVert *q, u8 face,
                         const SnesCamera *cam)
{
    const u16 page = snesCardPage(face);
    /* The affine gradient is a property of the whole polygon, so it is solved
     * once from three vertices and the span walk is two adds a texel.  A card
     * is convex and never folds, and its perspective error across a quad this
     * small is under a texel -- the shortcut the other ports take as well. */
    /* THE EDGE VECTORS ARE TAKEN IN SIXTEENTHS OF A PIXEL, NOT IN Q8.8, and
     * that shift is what keeps the whole solve inside sixteen bits.  A card is
     * forty pixels across, which is 10240 in Q8.8, and the cross product of two
     * of those is four hundred thousand -- the same overflow the Atari ST port
     * hit in fxdiv, and it comes back as a texture that is one flat colour
     * because every gradient divides by a wrapped determinant.  Both the
     * numerators and the determinant are scaled by the same 1/256, so the
     * ratios that come out are untouched. */
    const s16 ax = (s16)((q[1].x - q[0].x) >> 4), ay = (s16)((q[1].y - q[0].y) >> 4);
    const s16 bx = (s16)((q[2].x - q[0].x) >> 4), by = (s16)((q[2].y - q[0].y) >> 4);
    const s16 au = (s16)((q[1].u - q[0].u) >> 4), av = (s16)((q[1].v - q[0].v) >> 4);
    const s16 bu = (s16)((q[2].u - q[0].u) >> 4), bv = (s16)((q[2].v - q[0].v) >> 4);
    const s16 den = (s16)(snesQMul(ax, by) - snesQMul(bx, ay));
    s16 dudx, dvdx, dudy, dvdy;
    s16 y, y_bottom;
    u8 top = 0, bottom = 0, li, ri, i;
    EdgeWalk left, right;
    s16 cp = 0, sp = 0, cy = 0, sn = 0;

    if (cam) {
        cp = snesCos(cam->pitch); sp = snesSin(cam->pitch);
        cy = snesCos(cam->yaw); sn = snesSin(cam->yaw);
    }

    if (!den) return;
    dudx = signed_qdiv((s16)(snesQMul(au, by) - snesQMul(bu, ay)), den);
    dvdx = signed_qdiv((s16)(snesQMul(av, by) - snesQMul(bv, ay)), den);
    dudy = signed_qdiv((s16)(snesQMul(ax, bu) - snesQMul(bx, au)), den);
    dvdy = signed_qdiv((s16)(snesQMul(ax, bv) - snesQMul(bx, av)), den);

    for (i = 1; i < 4; ++i) {
        if (q[i].y < q[top].y) top = i;
        if (q[i].y > q[bottom].y) bottom = i;
    }
    y = (s16)(q[top].y >> 8);
    y_bottom = (s16)(q[bottom].y >> 8);
    if (y_bottom <= y) return;

    /* TWO CHAINS WALKED INDEPENDENTLY, each turning at its own vertex.  A
     * trapezoid walk -- one edge pair, no turn -- is what put a card off the
     * board rim on the MSX2 port, and a lifted card is exactly the case where
     * the two chains turn at different scanlines. */
    li = ri = top;
    edge_init(&left, &q[top], &q[(top + 3) & 3]);
    edge_init(&right, &q[top], &q[(top + 1) & 3]);

    for (; y < y_bottom; ++y, left.x += left.dx, right.x += right.dx) {
        s16 x0, x1, dx, dy;
        u16 u, v;

        while (y >= left.y_end && li != bottom) {
            li = (u8)((li + 3) & 3);
            edge_init(&left, &q[li], &q[(li + 3) & 3]);
        }
        while (y >= right.y_end && ri != bottom) {
            ri = (u8)((ri + 1) & 3);
            edge_init(&right, &q[ri], &q[(ri + 1) & 3]);
        }
        if (y < 0 || y >= (s16)vp->h) continue;

        x0 = (s16)(left.x >> 8);
        x1 = (s16)(right.x >> 8);
        if (x1 < x0) { const s16 t = x0; x0 = x1; x1 = t; }
        if (x0 < 0) x0 = 0;
        if (x1 > (s16)vp->w) x1 = (s16)vp->w;
        if (x1 <= x0) continue;

        if (cam) {
            /* Inverse camera ray / horizontal plane intersection.  Pitch
             * changes depth per row; yaw rotates the two texture increments.
             * Depth remains constant across a scanline (there is no roll). */
            s16 sy = (y - cam->horizon) * (vp->w == 32 ? 16 : vp->w == 64 ? 8 : 4);
            s16 denom = sp + snesQMul(sy, cp);
            s16 depth, z, x, step, wx, wz, du, dv;
            if (denom <= 0) continue;
            depth = signed_qdiv(cam->height, denom);
            z = cam->z + snesQMul(depth, cp - snesQMul(sy, sp));
            step = depth >> (vp->w == 32 ? 4 : vp->w == 64 ? 5 : 6);
            x = cam->x + (s16)snesMulLo((u16)step, (u16)(x0 - (vp->w >> 1)));
            if (!cam->yaw) {
                wx = x; wz = z; du = step * 32; dv = 0;
            } else {
                wx = snesQMul(x, cy) + snesQMul(z, sn);
                wz = snesQMul(z, cy) - snesQMul(x, sn);
                du = snesQMul(step, cy) * 32;
                dv = -snesQMul(step, sn) * 32;
            }
            u = (u16)(wx * 32 + 4096);
            v = (u16)(wz * 32);
            snesSpanFloorQuad((u16)(vp->origin + (u16)y * vp->stride + x0),
                              (u16)(x1 - x0), u - du, v - dv, du, dv);
            continue;
        }
        if (face == SNES_CARD_NONE_FACE) {
            snesSpanFill((u16)(vp->origin + (u16)y * vp->stride + x0),
                          (u16)(x1 - x0), SNES_SLAB_WALL);
            continue;
        }
        dx = (s16)(((s16)x0 << 8) - q[0].x);
        dy = (s16)(((s16)y << 8) - q[0].y);
        u = (u16)(q[0].u + snesQMul(dudx, dx) + snesQMul(dudy, dy));
        v = (u16)(q[0].v + snesQMul(dvdx, dx) + snesQMul(dvdy, dy));
        /* Pre-stepped for the same reason the flat card is: the walker adds
         * before it reads. */
        snesSpanCardQuad((u16)(vp->origin + snesMulLo((u16)y, (u16)vp->stride)
                               + (u16)x0),
                         (u16)(x1 - x0), (u16)(u - dudx), (u16)(v - dvdx),
                         (u16)dudx, (u16)dvdx, page);
    }
}

void snesTexQuad(const SnesViewport *vp, const SnesVert *q, u8 face)
{
    texture_quad(vp, q, face, 0);
}

void snesDrawCameraFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop)
{
    static const s16 xs[4] = { -640, 640, 640, -640 };
    static const s16 zs[4] = { 512, 512, -512, -512 };
    SnesVert q[4], wall[4];
    u8 i, j;
    for (i = 0; i < vp->h; ++i)
        snesSpanFill(vp->origin + (u16)i * vp->stride, vp->w, backdrop);
    for (i = 0; i < 4; ++i) {
        if (!snesProjectQ(cam, vp, xs[i], zs[i], 0, &q[i].x, &q[i].y)) return;
        q[i].u = (i == 1 || i == 2) ? 4095 : 0;
        q[i].v = (i >= 2) ? 4095 : 0;
    }
    /* Draw all four walls before the top; the top occludes the far walls. */
    for (i = 0; i < 4; ++i) {
        j = (i + 1) & 3;
        wall[0] = q[i]; wall[1] = q[j];
        wall[2] = q[j]; wall[3] = q[i];
        snesProjectQ(cam, vp, xs[j], zs[j], -64, &wall[2].x, &wall[2].y);
        snesProjectQ(cam, vp, xs[i], zs[i], -64, &wall[3].x, &wall[3].y);
        texture_quad(vp, wall, SNES_CARD_NONE_FACE, 0);
    }
    texture_quad(vp, q, 0, cam);
}
