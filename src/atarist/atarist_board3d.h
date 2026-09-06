/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_board3d.h — the 3D duel board.
 *
 *  Two rasterisers, both writing 8-bit indices into the chunky board buffer,
 *  because that is the only surface the C2P reads:
 *
 *    * the BOARD PLANE is a perspective-correct textured plane, CLIPPED TO THE
 *      SLAB.  It is not a general polygon: a plane through the camera has an
 *      exact closed form per screen row (depth = height * focal / (row -
 *      horizon)), so each row is one divide and then two adds per pixel.  That
 *      is what makes a textured board affordable on an 8 MHz 68000 at all.
 *      The board is FINITE -- five columns by four rows of checkerboard with a
 *      groove between the tiles, floating over black -- because that is what
 *      the MSX2 and PC-FX boards are; an unbounded sand plane with a grid
 *      painted on it reads as a road, not as a duel field.
 *
 *    * CARDS and the board RIM are convex quads.  Cards are texture mapped with
 *      a per-polygon affine gradient (one plane solve, then two adds per pixel);
 *      the rim is flat filled, which is the shortcut the requirement allows for
 *      the sides.
 *
 *  All fixed point is 16.16 unless a name says otherwise.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_BOARD3D_H
#define WAIFU_ATARIST_BOARD3D_H

#include <stdint.h>

/* Board layout, shared with the duel scene.  Five columns; four rows, ordered
 * far to near: the COM's support row, the COM's monsters, the player's
 * monsters, the player's supports. */
#define ATARIST_COLS      5
#define ATARIST_ROWS      4
#define ATARIST_ROW_COM_SUPPORT  0
#define ATARIST_ROW_COM_MONSTER  1
#define ATARIST_ROW_YOU_MONSTER  2
#define ATARIST_ROW_YOU_SUPPORT  3

/* The slab, in world units.  Tiles are one unit square: five columns centred
 * on the origin (x in -2.5 .. 2.5) and four rows straddling it (z in -2 .. 2),
 * which is exactly the checkerboard tools/atarist/gen_atarist_assets.py bakes
 * into ARENA.TEX.  Everything outside is the black surround. */
#define ATARIST_BOARD_HALF_X  ((int32_t)(5 << 15))      /* 2.5 */
#define ATARIST_BOARD_Z_NEAR  (-(int32_t)(2 << 16))
#define ATARIST_BOARD_Z_FAR   ((int32_t)(2 << 16))
/* How thick the slab is.  It is the front face of this that gives the board a
 * solid edge instead of looking like a rug. */
#define ATARIST_BOARD_THICK   ((int32_t)18350)          /* 0.28 */

typedef struct AtaristCamera {
    int32_t x, z;      /* world position, 16.16 */
    int32_t height;    /* above the board plane, 16.16 */
    int32_t yaw_sin;   /* 16.16 sin/cos of the heading */
    int32_t yaw_cos;
    int32_t horizon;   /* screen row of the horizon, in viewport pixels */
    int32_t focal;     /* 16.16 */
} AtaristCamera;

/* The render target: a rectangle of the chunky buffer.  Two shapes are used --
 * 160x56 while the camera moves and 320x112 when it rests -- and every routine
 * here works in whichever is current, so the two paths share one renderer. */
typedef struct AtaristViewport {
    uint8_t *pixels;
    int      w, h;
    int      stride;
    int      half;     /* 1 when this is the halved moving-camera viewport */
} AtaristViewport;

/* A texture.  Dimensions are powers of two and the bytes are ALREADY
 * multiplied by four, because that is what the chunky buffer stores (see
 * atarist_c2p.S) -- so the rasteriser's inner loop copies a texel straight out
 * with no shift. */
typedef struct AtaristTexture {
    const uint8_t *texels;
    uint16_t w_log2, h_log2;
} AtaristTexture;

typedef struct AtaristVert {
    int32_t x, y;      /* viewport pixels, 16.16 */
    int32_t u, v;      /* texels, 16.16 */
} AtaristVert;

/* ── Camera ──────────────────────────────────────────────────────────────── */
void Atarist_CameraSet(AtaristCamera *cam, int32_t x, int32_t z, int32_t height,
                       int32_t yaw_q16, int32_t focal);
/* Project a point on the board plane (y = 0) to viewport coordinates.
 * Returns 0 when it falls behind the camera. */
int  Atarist_Project(const AtaristCamera *cam, const AtaristViewport *vp,
                     int32_t wx, int32_t wz, int32_t wy,
                     int32_t *out_x, int32_t *out_y);

/* ── Inner loops (atarist_raster.S) ──────────────────────────────────────── */
/* One textured scanline of a plane: `u` steps by `du` per pixel and wraps
 * inside `mask`+1 texels of `tex_row`. */
void Atarist_GroundSpan(uint8_t *dst, const uint8_t *tex_row,
                        uint32_t u, uint32_t du, int count, int mask);
/* A solid run of chunky pixels. */
void Atarist_ChunkyFill(uint8_t *dst, int count, int value);
/* A flat block move between chunky surfaces. */
void Atarist_ChunkyCopy(uint8_t *dst, const uint8_t *src, int bytes);

/* ── The board plane ─────────────────────────────────────────────────────── */
/* Texture the slab and fill everything around it with `backdrop`.
 *
 * `texel_log2` is how many texels one world unit spans, as a power of two.  It
 * is a shift and not a factor because that is three fixed-point multiplies per
 * scanline the 68000 does not have to make.
 *
 * The bounds cost two extra divides a row and nothing per pixel: depth is
 * constant along a scanline, so the two screen columns where the slab's side
 * edges fall are a closed form, exactly as the row's depth is. */
void Atarist_DrawBoardPlane(const AtaristViewport *vp, const AtaristCamera *cam,
                            const AtaristTexture *tex, int texel_log2,
                            int32_t half_x, int32_t z_near, int32_t z_far,
                            uint8_t backdrop);

/* ── Quads ───────────────────────────────────────────────────────────────── */
void Atarist_FillQuad(const AtaristViewport *vp, const AtaristVert *quad,
                      uint8_t colour);
void Atarist_TexQuad(const AtaristViewport *vp, const AtaristVert *quad,
                     const AtaristTexture *tex);

/* World position of the centre of a board slot. */
void Atarist_SlotCentre(int row, int col, int32_t *wx, int32_t *wz);

#endif /* WAIFU_ATARIST_BOARD3D_H */
