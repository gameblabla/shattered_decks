/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_duel.c — duel presentation.
 *
 *  M2 stage: the board is a textured floor under a camera that moves, drawn by
 *  the constant-v row mapper in snes_raster.asm.  There is no placeholder art
 *  anywhere in it -- the ground is the arena sandstone and the sky is the
 *  desert backdrop the other ports use, both converted by
 *  tools/snes/gen_snes_textures.py -- and the cards, hand and HUD text arrive
 *  on top of it in M3 and M4.
 *
 *  The two resolutions are what the whole video model exists for: the camera
 *  moves at 64x40 texels (4x4 screen pixels) and rests at 128x80 (2x2), and
 *  the PPU does the doubling, so a moving board costs a quarter of the pixels
 *  and nothing extra to display.  Y switches between them, and the resting
 *  camera is what makes the still frame's three-vblank upload invisible.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_textures.h"
#include "snes_stamp.h"

/* The arena camera.  One unit is one slot pitch; the board runs z = -2..2, so
 * a camera one unit up and three back looks along the slab with its far edge
 * near the horizon. */
#define CAM_HEIGHT   ((s16)256)         /* 1.0 */
#define CAM_Z        ((s16)(-3 * 256))
#define CAM_SWAY     ((s16)192)         /* 0.75 units either side */

/* The surround: the texel the board floats on where the slab does not reach.
 * Direct colour is BBGGGRRR, so this is a dark warm brown -- the arena's
 * shadowed ground, not a debug colour. */
#define BACKDROP     SNES_DC(1, 1, 0)

static SnesCamera   cam;
static SnesViewport vp;
static u8  phase;               /* the sway's angle, 0..255 */

static void set_viewport(void)
{
    if (snesVideoBoardRes() == SNES_RES_STILL) {
        vp.w = SNES_STILL_W;
        vp.h = SNES_STILL_H;
    } else {
        vp.w = SNES_MOVING_W;
        vp.h = SNES_MOVING_H;
    }
    vp.origin = 0;
    vp.stride = SNES_FB_STRIDE;

    /* The focal length is half the viewport width, which is what makes the
     * floor's texture step a shift rather than a divide: 32 texels a world
     * unit over a focal length of w/2 is exactly 2 texels a pixel at w = 128
     * and 1 at w = 64.  See snesDrawFloor. */
    cam.focal  = (s16)((u16)vp.w << 7);
    /* An eighth of the band is sky.  Lower would bury the board's far edge in
     * the backdrop; higher wastes rows on ground beyond the slab. */
    cam.horizon = (s16)(vp.h >> 3);
}

/* The HUD band under the board: framebuffer rows 80..111 in both resolutions.
 *
 * It is the same arena sandstone, sampled 1:1, because the alternative at this
 * milestone is a flat rectangle and a flat rectangle is a placeholder.  The
 * life points, phase text and hand sprites land on top of it in M4; nothing
 * about its geometry changes when they do. */
static void draw_hud_panel(void)
{
    u16 y;

    for (y = 0; y < SNES_HUD_H; ++y) {
        /* A different part of the texture from the board's, so the panel does
         * not read as more floor: the groove rows, one tile down. */
        const u16 v = (y + SNES_FLOOR_TEXELS_PER_UNIT) & (SNES_FLOOR_PATTERN_H - 1);
        snesSpanFloor((u16)((SNES_HUD_ROW + y) * SNES_FB_STRIDE), SNES_HUD_W,
                      (u16)((v << 8) | 0), 0, 256);
    }
}

/* A board, timed.
 *
 * NTSC is 262 scanlines a field, and a still frame is more than one field of
 * work, so the V counter alone would wrap and underreport; the vblank count
 * carries the whole fields and the V counter the remainder.  This number is
 * the only thing in the port allowed to support a performance claim -- that,
 * or an ablation.  tools/snes/verify.py reads it out of the WRAM dump. */
static void render(void)
{
    const u16 vbl0  = snes_vblank_count;
    const u16 line0 = snesVCounter();

    snesDrawFloor(&vp, &cam, BACKDROP);

    g_stamp.render_lines = (u16)((snes_vblank_count - vbl0) * 262
                                 + snesVCounter() - line0);
    snesVideoPresentRestart();
}

void snesDuelEnter(void)
{
    phase = 0;
    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, 0, 0);
    snesVideoSetBoardRes(SNES_RES_STILL);
    set_viewport();
    snesVideoClear(BACKDROP);
    draw_hud_panel();
    snesVideoHudDirty();
    render();
}

void snesDuelFrame(void)
{
    const u16 pad = padsCurrent(0);
    u8 res = snesVideoBoardRes();
    u8 redraw = 0;

    if (padsDown(0) & KEY_Y) {
        res = (res == SNES_RES_STILL) ? SNES_RES_MOVING : SNES_RES_STILL;
        snesVideoSetBoardRes(res);
        set_viewport();
        /* The panel does not move with the band, but clearing the buffer
         * wipes it, so it is redrawn and re-uploaded with the change. */
        snesVideoClear(BACKDROP);
        draw_hud_panel();
        snesVideoHudDirty();
        redraw = 1;
    }

    /* The camera sways only in the moving resolution -- that is what the two
     * resolutions MEAN.  A resting camera leaves the still frame on screen and
     * its three-vblank upload finishes undisturbed. */
    if (res == SNES_RES_MOVING) {
        phase += 2;
        cam.x = snesQMul(snesSin(phase), CAM_SWAY);
        redraw = 1;
    }
    if (pad & KEY_LEFT)  { cam.x -= 8; redraw = 1; }
    if (pad & KEY_RIGHT) { cam.x += 8; redraw = 1; }
    if (pad & KEY_UP)    { cam.z += 8; redraw = 1; }
    if (pad & KEY_DOWN)  { cam.z -= 8; redraw = 1; }

    if (redraw) render();
}
