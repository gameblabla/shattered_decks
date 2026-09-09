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
 *  THE SWITCH BETWEEN THEM IS THREE REGISTER WRITES AND NO FORCE BLANK.  Both
 *  pictures are resident: the bitmap owns VRAM words $0000-$3FFF, the sprites
 *  $4000-$5FFF and the top view $6000-$73FF, and one CGRAM serves both (Mode 7
 *  direct colour reads none of it).  So changing view rewrites no VRAM and no
 *  CGRAM, needs no blanking window, and shows no black frame in between --
 *  which is the whole reason the top view's assets are preloaded at boot
 *  rather than swapped in.  The three writes still happen in vblank, because
 *  nothing but HDMA writes a PPU register during active display; see
 *  SNES_PORT_PLAN.md §11.
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
 * Still: the whole screen samples at 0.5, so 256x224 screen pixels are
 * 128x112 texels, the board taking rows 0..79 and the HUD rows 80..111.
 * Moving: the board band samples at 0.25 -- 64x40 texels over the same 160
 * lines -- while the HUD band keeps 0.5 so its text does not quadruple in
 * size when the camera starts moving.  That per-band scale is the whole
 * reason the presenter drives M7A/M7D/M7VOFS from an HDMA table rather than
 * writing them once a frame. */
#define SNES_BOARD_LINES  160
#define SNES_HUD_LINES    (224 - SNES_BOARD_LINES)

#define SNES_STILL_W      128
#define SNES_STILL_H      80
#define SNES_MOVING_W     64
#define SNES_MOVING_H     40

/* THE BEND IS A THIRD RESOLUTION AND IT OWNS THE WHOLE SCREEN.
 *
 * Walking up into the tactical view is a camera move -- the lens lengthens
 * while the camera backs off, so the board flattens instead of cutting -- and
 * it has to arrive on the SAME rectangle Mode 3's table stands on, which runs
 * from line 16 to line 208.  The board band cannot reach that: it stops at
 * 159 and the HUD band owns what is under it.  So for the length of the bend
 * there is no band split at all -- one scale for all 224 lines -- and 32x28
 * texels at 8x8 screen pixels each is exactly that screen.
 *
 * A quarter of the moving frame's pixels is what pays for it: the camera is
 * moving on every one of these frames and none of them is looked at for more
 * than a field, which is the same bargain the moving resolution already makes,
 * taken one step further. */
#define SNES_BEND_W       32
#define SNES_BEND_H       28

/* The HUD band is always 2x2 and ALWAYS reads framebuffer rows 80..111, in
 * both board resolutions.
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
 * and one chunky texel lands on exactly 2x2 screen pixels.  8.0 gives 4x4.
 * Both are powers of two, so neither shimmers. */
#define SNES_M7_SCALE_STILL   0x0400
#define SNES_M7_SCALE_MOVING  0x0200
#define SNES_M7_SCALE_BEND    0x0800

extern u8 snes_fb[];             /* snes_fb.asm, bank $7F */

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
void snesVideoSetBoardRes(u8 res);      /* enum SnesBoardRes; rebuilds the HDMA table */
u8   snesVideoBoardRes(void);
/* Ask for the other view.  It is applied by the next snesVideoPresent, which
 * runs in vblank; asking twice in a frame is free and asking for the view that
 * is already up does nothing. */
void snesVideoSetView(u8 view);         /* enum SnesView */
u8   snesVideoView(void);
/* Hand the presenter a finished frame.  The still frame is 14336 bytes, which
 * is more than one NTSC vblank of DMA, so it is uploaded over three vblanks
 * top-down; the moving frame is 3584 and always goes up in one.  Call once a
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
