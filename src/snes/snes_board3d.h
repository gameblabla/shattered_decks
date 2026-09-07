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
/* The same, in Q8.8 viewport pixels: what the quad rasteriser needs, since a
 * card's corners land between pixels and rounding them first is what makes an
 * edge crawl a pixel a frame. */
u8   snesProjectQ(const SnesCamera *cam, const SnesViewport *vp,
                  s16 wx, s16 wz, s16 wy, s16 *out_x, s16 *out_y);
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

/* ── Cards ───────────────────────────────────────────────────────────────── */

/* A CARD IS FOUR FIFTHS OF A BOARD TILE, and the fraction is arithmetic rather
 * than taste.  The face is sixteen texels, so four fifths of a one-unit tile
 * puts twenty texels on a world unit against the floor's thirty-two, and
 * 20/32 is 5/8: the card's texture step is the floor's own step shifted twice
 * and added, with no multiply and no divide in the row at all.  A card that
 * covered its whole tile would need neither either, but then every slot
 * touches its neighbour and the board reads as a carpet of cards rather than
 * as a board with cards on it. */
#define SNES_CARD_HALF   ((s16)102)      /* 0.4 world units, Q8.8 */

/* ONE BOARD ROW AT A TIME, not one card at a time.
 *
 * Five slots in a row sit at the same depth, so they share the row's depth,
 * its texture step and its v -- everything the plane equation produces -- and
 * only their screen edges and their starting u differ.  Drawing them together
 * is what makes a full board affordable: the per-row setup is 816-tcc's most
 * expensive code (measured, SNES_PORT_PLAN.md 4.4) and doing it once for a
 * board row instead of once per card cuts it by five.
 *
 * `faces` is five face ids, SNES_CARD_NONE_FACE for an empty slot.  Rows are
 * drawn far to near, which is the whole hidden-surface algorithm here: there
 * is no z buffer.
 */
#define SNES_CARD_NONE_FACE  0xFFu
void snesDrawCardRow(const SnesViewport *vp, const SnesCamera *cam, u8 row,
                     const u8 *faces);

/* A screen-space vertex for the general quad path: position in Q8.8 viewport
 * pixels, texture coordinates in Q8.8 texels. */
typedef struct SnesVert {
    s16 x, y;
    s16 u, v;
} SnesVert;

/* The convex-quad affine mapper -- a card that is NOT lying flat.
 *
 * TWO EDGE CHAINS, NOT A TRAPEZOID.  A card in the air is a convex quad whose
 * left and right chains each turn at their own vertex; assuming a trapezoid is
 * exactly what put a card off the board rim on the MSX2 port, so the chains
 * here are walked independently from the topmost vertex to the bottommost one.
 * Vertices must be given in order around the quad. */
void snesTexQuad(const SnesViewport *vp, const SnesVert *quad, u8 face);

/* The quad of a card lifted `lift` world units above its slot and leaning back
 * by `tilt` -- the far edge raised, so the picture turns towards the player --
 * ready for snesTexQuad.  Returns 0 when any corner is at or behind the near
 * plane. */
u8 snesCardQuad(const SnesCamera *cam, const SnesViewport *vp,
                u8 row, u8 col, s16 lift, s16 tilt, SnesVert *quad);

#endif /* WAIFU_SNES_BOARD3D_H */
