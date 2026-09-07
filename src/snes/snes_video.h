/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_video.h — the two display models, and the switch between them.
 *
 *  DUEL  — Mode 7 driven as a 128x128 chunky bitmap in DIRECT COLOUR, so all
 *          256 CGRAM entries stay free for the hand-card and HUD sprites and
 *          the texel byte is itself a BBGGGRRR colour.  See snes_fb.asm for
 *          why a Mode 7 tilemap is a chunky framebuffer.
 *  SCENE — Mode 3: BG1 at 8bpp from CGRAM, BG2 at 4bpp for text.  Real
 *          pictures, no software rendering.
 *
 *  A mode change is always done under force blank ($2100 = $8F), which is the
 *  only safe window for a bulk VRAM/CGRAM rewrite.  Nothing writes PPU
 *  registers during active display except HDMA -- see SNES_PORT_PLAN.md §11.
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

/* The HUD band is always 2x2, so it is always 128 texels wide and 32 tall.
 * It lives directly under whichever board band is in use, which is why the
 * moving board is capped at row 40. */
#define SNES_HUD_W        128
#define SNES_HUD_H        32
#define SNES_HUD_ROW      SNES_MOVING_H      /* moving: rows 40..71 */
#define SNES_HUD_ROW_STILL SNES_STILL_H      /* still:  rows 80..111 */

enum SnesBoardRes { SNES_RES_MOVING = 0, SNES_RES_STILL = 1 };

/* The Mode 7 matrix scale for each band, in the PPU's 8.8 format.
 *
 * These look eight times too big and are not.  Mode 7 samples a 1024x1024
 * space of PIXELS and a tilemap entry covers 8x8 of them, so an A of 4.0
 * advances the source by four pixels a screen pixel -- half a tilemap entry --
 * and one chunky texel lands on exactly 2x2 screen pixels.  8.0 gives 4x4.
 * Both are powers of two, so neither shimmers. */
#define SNES_M7_SCALE_STILL   0x0400
#define SNES_M7_SCALE_MOVING  0x0200

extern u8 snes_fb[];             /* snes_fb.asm, bank $7F */

/* snes_fb.asm */
void snesFbPresentRows(u16 first_row, u16 rows, u16 width);
void snesFbWriteChars(void);

/* snes_m7fb.c */
void snesVideoInitDuel(void);           /* Mode 7 + direct colour, force blank */
void snesVideoSetBoardRes(u8 res);      /* enum SnesBoardRes; rebuilds the HDMA table */
u8   snesVideoBoardRes(void);
/* Hand the presenter a finished frame.  The still frame is 14336 bytes, which
 * is more than one NTSC vblank of DMA, so it is uploaded over three vblanks
 * top-down; the moving frame is 3584 and always goes up in one.  Call once a
 * frame; it returns non-zero once the frame is fully on screen. */
u8   snesVideoPresent(void);
void snesVideoPresentRestart(void);     /* a new frame is ready: start again at the top */
void snesVideoPresentHud(void);         /* the HUD band only, one vblank */
void snesVideoClear(u8 colour);

/* Direct colour is BBGGGRRR: blue only has two bits.  Everything the Mode 7
 * bitmap shows is quantised into this cube by tools/snes/. */
#define SNES_DC(r, g, b)  (u8)(((r) & 7) | (((g) & 7) << 3) | (((b) & 3) << 6))

#endif /* WAIFU_SNES_VIDEO_H */
