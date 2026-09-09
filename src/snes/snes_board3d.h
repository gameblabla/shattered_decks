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
    u8 yaw, pitch;   /* 256 angles per turn; pitch 64 looks straight down */
} SnesCamera;

/* The render target: a rectangle of the chunky framebuffer.  Three shapes are
 * used -- 64x40 while ordinary board motion is active, 32x28 for the
 * hand-to-top camera lift, and 128x80 when it rests.  Every routine here works
 * in whichever is current, so all three paths share one renderer. */
typedef struct SnesViewport {
    u16 origin;      /* byte offset into snes_fb of pixel (0,0) */
    u8  w, h;
    u8  stride;
    /* 32/focal, Q8.8, for the axis-aligned floor/card span fast path. */
    u16 du_k;
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
void snesDrawCameraFloor(const SnesViewport *vp, const SnesCamera *cam, u8 backdrop);

/* ── Cards ───────────────────────────────────────────────────────────────── */

/* A CARD IS A CARD SHAPE, NOT A TILE.  It is as DEEP as the slot it lies in
 * and three quarters as WIDE, which from the duel camera reads as the 3:4
 * portrait every other port paints -- the square 0.8x0.8 this used to be made
 * the board a carpet of tiles with pictures in them.  Front to back the card
 * meets its neighbours, and that is what the rows are drawn far to near for;
 * across, the quarter unit left over is the groove the slot's own texture
 * shows through, so five cards on a row are still five things.
 *
 * The face is sixteen texels either way, so the two extents also set the two
 * texture rates: 16 texels over one unit down the card, and 16 over three
 * quarters of a unit across it -- 21 1/3 to the unit against the floor's
 * thirty-two, which is two thirds of the floor's own step. */
#define SNES_CARD_HALF_X ((s16)96)       /* 0.375 world units: 3/4 of a tile */
#define SNES_CARD_HALF_Z ((s16)128)      /* 0.5: the tile's whole depth */
/* Two thirds, in Q8.8: the card's texture step as a fraction of the floor's. */
#define SNES_CARD_U_NUM  ((s16)171)

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

/* The cursor's slot, and the slot the COM is acting on: a flat fill over the
 * whole tile, so a card resting in it leaves the marker as a rim. */
void snesDrawSlotMarker(const SnesViewport *vp, const SnesCamera *cam,
                        u8 row, u8 col, u8 colour);

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
