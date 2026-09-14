/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_video.h — the duel's display model: Mode 3, BG1 Direct Colour, 1:1.
 *
 *  THE BOARD IS ONE 256x144 PICTURE AT THE SCREEN'S OWN RESOLUTION IN EVERY
 *  POSE.  The software renderer draws it as chunky direct-colour bytes
 *  (BBGGGRRR: the texel byte is the colour, and CGRAM is left entirely to the
 *  sprite layer) into snes_frame_fb; snes_conv_drivers.inc transposes the
 *  8x8 cells that changed into planar tiles in a ring, and snes_fb.asm's NMI
 *  drain carries them into VRAM.
 *
 *  A SPARSE TILE STORE WITH TWO MAPS.  Empty surround cells name the
 *  permanent blank tile 0; the 702 other tiles are allocated to the cells
 *  the board reaches (at most 351 a frame, so two disjoint frames fit).  Two
 *  logical maps exist: one on screen and one being built, which shares every
 *  unchanged cell's tile with the shown one, copy-on-write with a reference
 *  count per tile.  A transaction is: render, convert the dirty cells into
 *  fresh tiles, upload them and the whole inactive map, then switch BG1SC in
 *  ONE vblank once everything queued before it has gone up.  A slow frame
 *  repeats the last complete picture; nothing half-written is ever visible.
 *
 *  VRAM (words):
 *      $0000-$57FF   704 8bpp tiles: 0 the shared blank, 1..702 allocatable
 *      $5800-$5BFF   BG1 map A, 32x32
 *      $5C00-$5FFF   BG1 map B, 32x32
 *      $6000-$7FFF   OBJ characters (OBSEL name base 3)
 *  In the chair view the HUD band under line 144 hides BG1 by an HDMA write
 *  to TM.  The tactical overhead view moves that clip down to the HUD
 *  plate's top line (197) and scrolls the same tile map vertically, so the
 *  board may use the band's lines but never the plate's.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_VIDEO_H
#define WAIFU_SNES_VIDEO_H

#include "snes_types.h"

#define SNES_FRAME_W          256
#define SNES_FRAME_H          144
#define SNES_FRAME_STRIDE     256
#define SNES_BOARD_LINES      144
#define SNES_HUD_LINES        (224 - SNES_BOARD_LINES)
/* The first line of the HUD plate's gradient (snes_video.c GRAD_LEAD): BG1
 * is clipped above it in either view and no sprite but the plate's own
 * words may cross it. */
#define SNES_PLATE_Y          197
#define SNES_CELL_COLS        32
#define SNES_CELL_ROWS        18
#define SNES_FRAME_CELLS      (SNES_CELL_COLS * SNES_CELL_ROWS)
#define SNES_SPARSE_TILES     351

#define SNES_VRAM_BOARD_CHARS 0x0000u
#define SNES_VRAM_BOARD_MAP_A 0x5800u
#define SNES_VRAM_BOARD_MAP_B 0x5C00u
#define SNES_VRAM_OBJ         0x6000u
#define SNES_MAP_ATTR         0x1C00u

enum SnesPresentationOwner {
    SNES_OWNER_BOARD = 0,
    SNES_OWNER_CARD_CHECK = 1,
    SNES_OWNER_BATTLE = 2
};

extern u8 snes_frame_fb[];          /* 256x144, bank $7E */
extern u8 snes_conv_rowspan[];      /* 18 x {first x, last x}, 255 = empty */
extern u16 snes_conv_dirty[];       /* 18 x {lo, hi}: cells to reconvert */
extern u16 snes_conv_rom[];         /* 18 x {lo, hi}: cells the ROM floor supplies */
extern u8 snes_fb_tm[];
/* Set to have the next NMI copy snes_oam_shadow to OAM (snes_fb.asm). */
extern u16 snes_fb_oam_pending;

void snesFbInit(void);
void snesFbDrain(u16 on);
void snesFbCancel(void);
void snesFbReserve(u16 bytes);
u16  snesFbJobsPending(void);
u16  snesFbFramesPending(void);
u16  snesFbPresentedGeneration(void);
u16  snesFbOccupied(void);
u16  snesFbPeakOccupied(void);
u16  snesFbNmiSkips(void);
/* Frames the converter refused because their span was over the cell budget.
 * Each one is a rendered picture that was never shown. */
u16  snesFbOverflows(void);
void snesFbJobPush(u16 src, u16 bank, u16 dst_word, u16 bytes, u16 kind);
void snesFbRectCopy(u16 src, u16 src_bank, u16 dst, u16 dst_bank,
                    u16 w, u16 h, u16 src_stride, u16 dst_stride);
void snesFbWramDma(u16 src, u16 src_bank, u16 dst, u16 dst_bank, u16 bytes);
void snesFbWramFill(u16 dst, u16 dst_bank, u16 bytes, u16 value);
void snesFbNmi(void);

#define SNES_JOB_DMA       0
#define SNES_JOB_RING      1
#define SNES_JOB_COMMIT_A  2
#define SNES_JOB_COMMIT_B  3

u16 snesConvFrame(u16 pool, u16 generation);
/* The same in three steps, for a frame converted a few rows a field. */
u16 snesConvBegin(u16 pool, u16 generation);
void snesConvRows(u16 row0, u16 row1);
void snesConvCells(u16 row, u16 col0, u16 col1);
void snesConvEnd(void);
void snesConvSetFloor(u16 src, u16 bank);
/* The frame to convert: 0 the 256x144 picture at 1:1, 1 the 128x72 motion
 * frame at the top of the same buffer (stride 128), converted doubled -- a
 * cell is four texels by four lines, each texel a 2x2 block of pixels. */
void snesConvSetHalf(u16 on);

void snesRasterTarget(u16 bank);
void snesSpanFloor(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                   u16 u_step);
void snesSpanFloorTex(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                      u16 u_step);
void snesSpanFill(u16 fb_index, u16 count, u8 colour);
void snesSpanCard(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                  u16 u_step);
void snesSpanCard32(u16 fb_index, u16 count, u16 tex_index, u16 u_frac,
                    u16 u_step);
void snesSpanCardQuad(u16 fb_index, u16 count, u16 u, u16 v, u16 du, u16 dv,
                      u16 page);
void snesSpanCardQuad32(u16 fb_index, u16 count, u16 u, u16 v, u16 du, u16 dv,
                        u16 page, u16 sheet);
void snesSpanFloorQuad(u16 index, u16 count, u16 u, u16 v, u16 du, u16 dv);
/* The pitch-only floor mapper (snes_raster.asm): Setup takes the slab's
 * half width on the first row and its step (the trapezoid's edges), the
 * camera's row terms and the frame; Pitch maps and walks every row in
 * [y0, y1), in as many calls as the caller likes. */
void snesFloorRowsSetup(s16 half, s16 dhalf, s16 denom16, s16 dstep,
                        s16 a16, s16 astep, s16 height, s16 camz,
                        u16 ubase, u16 origin, u16 sub);
void snesFloorRowsPitch(u16 y0, u16 y1);
/* The resting card rows (snes_raster.asm): snesDrawCardRow fills these
 * bank-0 words with its per-row-of-slots setup and snesCardRows walks the
 * pixel rows and the five spans.  See the cr_* block in the asm. */
extern u16 cr_y, cr_yend, cr_rows, cr_base, cr_stride;
extern s16 cr_acc_l, cr_acc_r, cr_acc_p, cr_step_l, cr_step_r, cr_step_p;
extern u16 cr_l_off, cr_r_off, cr_p_off;
extern s16 cr_limit;
extern u16 cr_hf;
extern s16 cr_du_k, cr_d_far, cr_d_near;
extern u16 cr_clip_y0, cr_sub, cr_halfw, cr_w, cr_flip;
extern u8  cr_faces[6];
void snesCardRows(void);
void snesBoardTextureClear(void);
void snesBoardTextureCard(u16 centre, u16 face, u16 flip, u16 width, u16 height);
/* THE WORLD TEXTURE'S u ORIGIN: the texel under world x = 0, the board's
 * centre column.  The texture is 256 texels round and the board five units
 * (160 texels) wide, so with the centre at 144 the board lies on texels
 * 64..223 and no row of it crosses the u wrap -- every constant-v span over
 * it is one bounded run (snes_raster.asm, RS_RUN) and every overhead row
 * one block move.  At the old 16 the board straddled the wrap (192..255
 * and 0..95) and every row through it was two pieces.  The move from 16
 * is 128, a whole number of the cleared ground's 64-texel checker, so the
 * pattern under the board is the same picture; and a row's first u, which
 * rounds a texel or two under the slab's edge, stays well above zero. */
#define SNES_WORLD_U_CENTRE 144

void snesVideoInitDuel(void);
void snesVideoRestartHdma(void);
/* The overhead board uses the main screen down to the HUD plate and follows
 * its row with BG1 tile scrolling.  Call from vblank; `scroll_y` is source
 * minus screen pixels (the normal chair view is zero). */
void snesVideoBoardViewport(u8 overhead, s16 scroll_y);
void snesVideoRestartSceneHdma(void);
void snesVideoSetSkyTables(u16 rg, u8 rg_bank, u16 b, u8 b_bank);
void snesVideoTitleMenuPlate(u8 on);
void snesVideoSetOwner(u8 owner);
u8   snesVideoOwner(void);
u16  snesVideoRequestGeneration(void);
u16  snesVideoPresentedGeneration(void);
u8   snesVideoPresent(void);
u8   snesVideoPresentDone(void);

#define SNES_DC(r, g, b)  (u8)(((r) & 7) | (((g) & 7) << 3) | (((b) & 3) << 6))

#endif
