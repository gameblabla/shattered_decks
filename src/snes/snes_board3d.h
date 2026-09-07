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
    s16 focal;       /* Q8.8, in viewport pixels */
    s16 horizon;     /* the screen row of the horizon, in viewport pixels */
} SnesCamera;

/* The render target: a rectangle of the chunky framebuffer.  Two shapes are
 * used -- 64x40 while the camera moves and 128x80 when it rests -- and every
 * routine here works in whichever is current, so the two paths share one
 * renderer. */
typedef struct SnesViewport {
    u16 origin;      /* byte offset into snes_fb of pixel (0,0) */
    u8  w, h;
    u8  stride;
} SnesViewport;

void snesCameraSet(SnesCamera *cam, s16 x, s16 z, s16 height, s16 focal,
                   s16 horizon);
/* Project a point on the board plane to viewport coordinates.  Returns 0 when
 * it falls at or behind the near plane. */
u8   snesProject(const SnesCamera *cam, const SnesViewport *vp,
                 s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y);
/* World position of the centre of a board slot, in Q8.8. */
void snesSlotCentre(u8 row, u8 col, s16 *wx, s16 *wz);

/* Texture the slab and fill everything around it with `backdrop`.  The board
 * is FINITE -- five columns by four rows of checkerboard over a backdrop --
 * because that is what the MSX2, PC-FX and Atari ST boards are; an unbounded
 * plane with a grid painted on it reads as a road, not as a duel field.
 *
 * The bounds cost two adds a row and nothing per pixel: depth is constant
 * along a scanline, so the two screen columns where the slab's side edges fall
 * are linear in the row index. */
void snesDrawFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop);

#endif /* WAIFU_SNES_BOARD3D_H */
