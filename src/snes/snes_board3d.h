/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_board3d.h — the 3D duel board: camera, projection and slot geometry.
 *
 *  Ported from src/atarist/atarist_board3d.c, which is already the 32-bit-free,
 *  libgcc-free version of the board model, and narrowed from 16.16 to Q8.8
 *  because that is what fits a 16-bit register on this machine.  One world unit
 *  is one slot pitch, columns run -2..2 and rows -1.5..1.5, and the camera
 *  never rolls.  The board is only about five units across, so eight integer
 *  bits is ample and the intermediate the Atari ST's fxdiv overflowed on is not
 *  representable here at all.
 *
 *  The floor is not a general polygon.  A plane through the camera has an exact
 *  closed form per screen row -- depth = height * focal / (row - horizon) --
 *  so a row costs one reciprocal lookup and two multiplies and then two adds a
 *  pixel.  That is what makes a textured board affordable.
 *
 *  UNITS AND PIXELS.  All geometry is in a 128x72 "unit" viewport with a
 *  focal length of 64 -- half its width, which is what makes the floor's
 *  texture step a shift.  The motion frame IS that viewport (SnesViewport.sub
 *  = 0).  The resting board is the same viewport at two pixels a unit (sub =
 *  1, 256x144): rows are walked in pixels with the half-unit reciprocal table
 *  and every unit quantity is shifted when it becomes a pixel.  Q8.8 could
 *  not hold a 256-wide viewport's own focal length or its row count.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_BOARD3D_H
#define WAIFU_SNES_BOARD3D_H

#include "snes_types.h"
#include "snes_math.h"

/* Five columns; four rows, ordered far to near: the COM's support row, the
 * COM's monsters, the player's monsters, the player's supports. */
#define SNES_COLS   5
#define SNES_ROWS   4
#define SNES_ROW_COM_SUPPORT  0
#define SNES_ROW_COM_MONSTER  1
#define SNES_ROW_YOU_MONSTER  2
#define SNES_ROW_YOU_SUPPORT  3

/* The slab, in Q8.8 world units.  Tiles are one unit square: five columns
 * centred on the origin and four rows straddling it, which is exactly the
 * checkerboard tools/snes/gen_snes_textures.py bakes into the floor texture.
 * Everything outside is the backdrop. */
#define SNES_BOARD_HALF_X   ((s16)(5 * 128))      /*  2.5 */
#define SNES_BOARD_Z_NEAR   ((s16)(-2 * 256))
#define SNES_BOARD_Z_FAR    ((s16)(2 * 256))

typedef struct SnesCamera {
    s16 x, z;        /* world position, Q8.8 */
    s16 height;      /* above the board plane, Q8.8 */
    s16 focal;       /* Q8.8, in unit-viewport pixels: always 64.0 */
    s16 horizon;     /* the horizon's row, in the viewport's PIXELS */
    u8 yaw, pitch;   /* 256 angles per turn; pitch 64 looks straight down */
} SnesCamera;

/* The render target.  `origin` is an absolute byte offset in `bank` of pixel
 * (0,0); w/h/stride are in pixels; `sub` is the pixels-per-unit shift (0 for
 * the 128x72 motion frame, 1 for the 256x144 rest frame). */
typedef struct SnesViewport {
    u16 origin;
    u16 w, h;
    u16 stride;
    u8  bank;
    u8  sub;
    /* 32/focal, Q8.8, per PIXEL: the floor's texture step at unit depth. */
    u16 du_k;
} SnesViewport;

void snesCameraSet(SnesCamera *cam, s16 x, s16 z, s16 height, s16 focal,
                   s16 horizon);
/* Project a point on the board plane.  Returns 0 when it falls at or behind
 * the near plane.  The Q form is in Q8.8 UNITS from the viewport's top-left
 * (what the quad rasteriser needs, since a card's corners land between
 * pixels); the plain form is whole PIXELS. */
u8   snesProjectQ(const SnesCamera *cam, const SnesViewport *vp,
                  s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y);
u8   snesProject(const SnesCamera *cam, const SnesViewport *vp,
                 s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y);
/* World position of the centre of a board slot, in Q8.8.  `mirror` is the
 * yaw-128 board: the table seen from the other side, so every slot's world
 * position is negated. */
void snesSlotCentre(u8 row, u8 col, u8 mirror, s16 *wx, s16 *wz);

/* Texture the slab and fill everything around it with `backdrop`.  Axis
 * aligned (yaw 0) only; the camera-motion frames go through
 * snesDrawCameraFloor, which handles yaw and pitch and records, per motion
 * tile row, the texel columns it touched in snes_conv_rowspan. */
void snesDrawFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop);
void snesDrawCameraFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop);
/* The same in two halves, for a frame painted a few rows a display field:
 * Begin clears, draws the walls and walks the slab's edges; when it returns
 * 1 the rows [y0, y1) are still to be mapped -- in any number of calls to
 * snesFloorRowsPitch(y, y + n).  (A yawed camera is mapped inside Begin and
 * returns 0.)  `clear` 0 means the caller has already filled the frame with
 * the backdrop. */
u8   snesDrawCameraFloorBegin(const SnesViewport *vp, const SnesCamera *cam,
                              u8 backdrop, u8 clear, u16 *y0, u16 *y1);

/* ── Cards ───────────────────────────────────────────────────────────────── */

/* A CARD IS A CARD SHAPE, NOT A TILE.  It is as DEEP as the slot it lies in
 * and three quarters as WIDE, which from the duel camera reads as the 3:4
 * portrait every other port paints.  Front to back the card meets its
 * neighbours; across, the quarter unit left over is the groove the slot's
 * own texture shows through, so five cards on a row are still five things. */
#define SNES_CARD_HALF_X ((s16)96)       /* 0.375 world units: 3/4 of a tile */
#define SNES_CARD_HALF_Z ((s16)128)      /* 0.5: the tile's whole depth */
/* The card's texture step as a fraction of the floor's (32 texels a unit):
 * 16 texels over three quarters of a unit is two thirds, 32 is four thirds. */
#define SNES_CARD_U_NUM    ((s16)171)
#define SNES_CARD32_U_NUM  ((s16)341)

/* ONE BOARD ROW AT A TIME, not one card at a time: five slots in a row share
 * the row's depth, texture step and v, and only their screen edges differ.
 * `faces` is five face ids, SNES_CARD_NONE_FACE for an empty slot.  Rows are
 * drawn far to near, which is the whole hidden-surface algorithm here.
 * Only pixel rows in [clip_y0, clip_y1) are drawn: a slot rebake redraws the
 * rows its dirty cells cover and nothing else.  On the rest viewport (sub 1)
 * the 32x32 sheet is used, on the motion viewport the 16x16 one. */
#define SNES_CARD_NONE_FACE  0xFFu
void snesDrawCardRow(const SnesViewport *vp, const SnesCamera *cam, u8 row,
                     const u8 *faces, u8 mirror, u16 clip_y0, u16 clip_y1);

/* The cursor's slot, and the slot the COM is acting on: a flat fill over the
 * whole tile, so a card resting in it leaves the marker as a rim. */
void snesDrawSlotMarker(const SnesViewport *vp, const SnesCamera *cam,
                        u8 row, u8 col, u8 mirror, u8 colour);

/* A screen-space vertex for the general quad path: position in Q8.8 units
 * from the viewport's top-left, texture coordinates in Q8.8 texels. */
typedef struct SnesVert {
    s16 x, y;
    s16 u, v;
} SnesVert;

/* The convex-quad affine mapper -- a card that is NOT lying flat.  Two edge
 * chains walked independently from the topmost vertex to the bottommost one;
 * vertices in order around the quad. */
void snesTexQuad(const SnesViewport *vp, const SnesVert *quad, u8 face);

/* The quad of a card lifted `lift` world units above its slot and leaning back
 * by `tilt`, ready for snesTexQuad; `defense` turns it a quarter on its
 * slot (a unit across, three quarters deep, the painting's top towards its
 * owner's right).  Returns 0 when any corner is at or behind the near
 * plane.  This is also how a resting card in defence is drawn: the row
 * rasteriser knows one footprint and one orientation, so a turned card is
 * this quad over its slot after the row's upright cards. */
u8 snesCardQuad(const SnesCamera *cam, const SnesViewport *vp,
                u8 row, u8 col, u8 mirror, s16 lift, s16 tilt, u8 defense,
                SnesVert *quad);

/* The pixel bounding box of a quad on its viewport, clamped to it.  Returns 0
 * if it is entirely outside. */
u8 snesQuadBounds(const SnesViewport *vp, const SnesVert *quad,
                  u16 *x0, u16 *y0, u16 *x1, u16 *y1);

#endif /* WAIFU_SNES_BOARD3D_H */
