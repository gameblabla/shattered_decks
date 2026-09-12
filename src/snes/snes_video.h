/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_video.h — the two display models, and the switch between them.
 *
 *  BOARD — Mode 7 driven as a 128x128 chunky bitmap in DIRECT COLOUR, so the
 *          texel byte is itself a BBGGGRRR colour and CGRAM is left entirely
 *          to the sprite layer.  See snes_fb.asm for why a Mode 7 tilemap is
 *          a chunky framebuffer.
 *  TOP   — Mode 3: the overhead table as an 8bpp BG1 at the full 256x224,
 *          with the field's cards on it as sprites.  No software rendering at
 *          all, so it also runs at sixty fields a second.
 *
 *  THE RESTING SWITCH BETWEEN THEM IS THREE REGISTER WRITES AND NO FORCE
 *  BLANK.  Both pictures are resident: the bitmap owns VRAM words $0000-$3FFF,
 *  the sprites $4000-$5FFF and the top view $6000-$73FF, and one CGRAM serves
 *  both (Mode 7 direct colour reads none of it).  The duel renders the short
 *  hand-to-top camera lift in the same 128x80 board surface; only its endpoint
 *  changes modes, so the top view still needs no asset upload or black loading
 *  frame.
 *  The three endpoint writes happen in vblank, because nothing but HDMA writes
 *  a PPU register during active display; see SNES_PORT_PLAN.md §11.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_VIDEO_H
#define WAIFU_SNES_VIDEO_H

#include "snes_types.h"

/* ── The chunky framebuffer ──────────────────────────────────────────────── */
#define SNES_FB_STRIDE   128
#define SNES_FB_ROWS     128

/* The 3D viewport, and the HUD band under it.  Both are measured in TEXELS;
 * the PPU turns them into screen pixels at the band's scale.
 *
 * Every 3D board frame uses 128x80 texels.  Motion admits at most one
 * complete board update every twelve vblanks; the software renderer and staged
 * VRAM upload may take longer on a card-heavy frame, but never lower the
 * source resolution.  The HUD is always sprites at the screen's own
 * resolution. */
#define SNES_BOARD_LINES  160
#define SNES_HUD_LINES    (224 - SNES_BOARD_LINES)

#define SNES_STILL_W      128
#define SNES_STILL_H      80
#define SNES_MOVING_W     SNES_STILL_W
#define SNES_MOVING_H     SNES_STILL_H
#define SNES_MOTION_FIELDS 12       /* minimum interval: 60.1 Hz / 12 */

/* Camera motion remains in the same 128x80 board surface.  The HUD stays
 * outside that surface and remains readable while the camera turns or lifts. */
#define SNES_BEND_W       SNES_STILL_W
#define SNES_BEND_H       SNES_STILL_H

/* The HUD band is always 2x2 and ALWAYS reads framebuffer rows 80..111, in
 * every 3D board state, including camera motion.
 *
 * That fixed row is not a convenience, it is what lets the vertical offset
 * stay zero for the whole screen.  With D = 4.0 the band's source row is
 * line/2, so lines 160..223 land on rows 80..111 with M7VOFS = 0; parking the
 * panel under the moving board instead (rows 40..71) needs a per-band VOFS,
 * and a VOFS written by HDMA does NOT take effect per band -- the board came
 * out shifted twenty texel rows down the screen, which is the whole frame
 * being drawn with the HUD band's offset.  Rows 40..79 simply go unused while
 * the board is moving: framebuffer space, not time. */
#define SNES_HUD_W        128
#define SNES_HUD_H        32
#define SNES_HUD_ROW      SNES_STILL_H       /* rows 80..111, always */

enum SnesBoardRes { SNES_RES_MOVING = 0, SNES_RES_STILL = 1, SNES_RES_BEND = 2 };
enum SnesView     { SNES_VIEW_BOARD = 0, SNES_VIEW_TOP = 1 };

/* The Mode 7 matrix scale for each band, in the PPU's 8.8 format.
 *
 * These look eight times too big and are not.  Mode 7 samples a 1024x1024
 * space of PIXELS and a tilemap entry covers 8x8 of them, so an A of 4.0
 * advances the source by four pixels a screen pixel -- half a tilemap entry --
 * and one chunky texel lands on exactly 2x2 screen pixels.  The moving and
 * bend enums use this same scale; they select cadence and camera state rather
 * than a lower-resolution picture. */
#define SNES_M7_SCALE_STILL   0x0400

/* THE HAND-TO-TOP CAMERA MOVE IS A MATRIX ANIMATION, NOT A RE-RENDER.
 *
 * A full software board is about 3400 scanlines -- thirteen fields -- plus
 * five more to upload, so a "live" camera lift rendered eight poses took the
 * better part of seven seconds and spent all of it re-baking the board
 * texture.  The board bitmap already in VRAM is a perfectly good picture of
 * the board; what the move has to do is get the eye closer to it.  So the
 * board band's Mode 7 scale is walked instead, one value a FIELD, and nothing
 * is rendered or uploaded at all.
 *
 * It zooms about the centre of the board rectangle, which is what the four
 * matrix-centre registers below are for: with M7X/M7Y at the board's middle in
 * Mode 7 pixel space and the two scroll registers cancelling the screen
 * coordinate of that middle, A = D = 4.0 is EXACTLY the mapping this port
 * always had -- one chunky texel on 2x2 screen pixels, framebuffer rows 0..79
 * on screen lines 0..159 -- and any other scale magnifies about the board's
 * middle instead of about the top-left corner.
 *
 * The HUD band keeps 4.0 in the same table, so the panel under the board never
 * moves while the board pushes in. */
#define SNES_M7_SCALE_TOP     0x02A0    /* the top of the lift, 1.5x in */
#define SNES_M7_CENTRE_X      512       /* screen x 128, at 4 pixels a pixel */
#define SNES_M7_CENTRE_Y      320       /* screen line 80: board rows 0..79 */
#define SNES_M7_HOFS          (SNES_M7_CENTRE_X - 128)
#define SNES_M7_VOFS          (SNES_M7_CENTRE_Y - 80)

extern u8 snes_fb[];             /* snes_fb.asm, bank $7F */
void snesSpanFloorQuad(u16 index, u16 count, u16 u, u16 v, u16 du, u16 dv);
void snesBoardTextureClear(void);
void snesBoardTextureCard(u16 centre, u16 face, u16 flip, u16 width, u16 height);

/* snes_raster.asm -- the span walkers.  Every pixel that reaches the
 * framebuffer is written by one of these three: C loops over the framebuffer
 * cost about a hundred CPU cycles a byte on 816-tcc (measured), which is two
 * orders off what the same loop costs in hand-written 65816. */
void snesSpanFloor(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                   u16 u_step);
void snesSpanFill(u16 fb_index, u16 count, u8 colour);
/* A resting card: the floor's walk over the card sheet, where the texture
 * index is `page << 8 | (v << 4) | u`. */
void snesSpanCard(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                  u16 u_step);
/* A card in the air: the general affine walk, u and v both stepping. */
void snesSpanCardQuad(u16 fb_index, u16 count, u16 u, u16 v, u16 du, u16 dv,
                      u16 page);

/* snes_fb.asm */
void snesFbPresentRows(u16 first_row, u16 rows, u16 width);
void snesFbWriteChars(void);

/* snes_m7fb.c */
void snesVideoInitDuel(void);           /* Mode 7 + direct colour, force blank */
void snesVideoRestartHdma(void);         /* re-prime after a scene boundary */
void snesVideoRestartSceneHdma(void);    /* Mode 3 dialogue-window gradient */
/* The story sky: two HDMA tables tinting the backdrop per line (red/green
 * pairs and blue), or NULL.  Set before snesVideoRestartSceneHdma. */
void snesVideoSetSkyTables(u16 rg, u8 rg_bank, u16 b, u8 b_bank);
/* The title menu's darkened plate: colour math inside an HDMA'd window. */
void snesVideoTitleMenuPlate(u8 on);
void snesVideoRestartDeckHdma(void);     /* Mode 3 full-screen purple gradient */
void snesVideoSetBoardRes(u8 res);      /* enum SnesBoardRes; rebuilds the HDMA table */
/* The board band's Mode 7 scale.  SNES_M7_SCALE_STILL is the resting board and
 * anything smaller magnifies it about its own centre; the HUD band is not
 * touched.  It is applied by the next snesVideoPresent, in vblank. */
void snesVideoSetBoardZoom(u16 scale);
u16  snesVideoBoardZoom(void);
u8   snesVideoBoardRes(void);
/* Ask for the other view.  It is applied by the next snesVideoPresent, which
 * runs in vblank; asking twice in a frame is free and asking for the view that
 * is already up does nothing. */
void snesVideoSetView(u8 view);         /* enum SnesView */
u8   snesVideoView(void);
/* Hand the presenter a finished frame.  The still frame is 14336 bytes, which
 * is more than one NTSC vblank of DMA, so it is uploaded over five bounded
 * vblanks top-down, leaving room for OAM and one card-row DMA.  Call once a
 * frame; it returns non-zero once the frame is fully on screen. */
u8   snesVideoPresent(void);
u8   snesVideoPresentDone(void);        /* is the last frame fully uploaded? */
void snesVideoPresentRestart(void);     /* a new frame is ready: start again at the top */
void snesVideoPresentHud(void);         /* the HUD band only, one vblank */
/* Ask for one HUD-band upload.  The band is static between the events that
 * change it, so it does not ride along with every board frame. */
void snesVideoHudDirty(void);
void snesVideoClear(u8 colour);

/* Direct colour is BBGGGRRR: blue only has two bits.  Everything the Mode 7
 * bitmap shows is quantised into this cube by tools/snes/. */
#define SNES_DC(r, g, b)  (u8)(((r) & 7) | (((g) & 7) << 3) | (((b) & 3) << 6))

#endif /* WAIFU_SNES_VIDEO_H */
