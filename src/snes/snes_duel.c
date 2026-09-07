/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_duel.c — duel presentation.
 *
 *  M1 stage: the framebuffer harness.  What it draws is a calibration pattern,
 *  not art -- a 16x16 grid of direct-colour blocks with a one-texel border --
 *  because the thing being verified at this milestone is that a texel lands on
 *  exactly 2x2 screen pixels in the still band and 4x4 in the moving one, and
 *  a photograph of real art cannot tell you that.  The pattern is confined to
 *  this function and no part of it survives into the textured board.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_video.h"
#include "snes_stamp.h"

static u8 calib_frame;

static void draw_calibration(void)
{
    u16 y, x;
    u8 *row;

    for (y = 0; y < SNES_HUD_ROW_STILL + SNES_HUD_H; ++y) {
        row = &snes_fb[y * SNES_FB_STRIDE];
        for (x = 0; x < SNES_FB_STRIDE; ++x) {
            /* One direct-colour block per 8x8 texels, and a black texel on
             * every eighth column and row.  In the still band that border is
             * two screen pixels wide, in the moving band four: the harness
             * measures the border to prove the scale. */
            if (((x & 7) == 0) || ((y & 7) == 0)) row[x] = 0;
            else row[x] = SNES_DC(x >> 4, y >> 4, (x + y) >> 5);
        }
    }
}

void snesDuelEnter(void)
{
    calib_frame = 0;
    snesVideoSetBoardRes(SNES_RES_STILL);
    draw_calibration();
    snesVideoPresentRestart();
}

void snesDuelFrame(void)
{
    /* Y toggles the still/moving board, which is also the input the harness
     * uses to capture the same picture at both scales. */
    if (padsDown(0) & KEY_Y) {
        snesVideoSetBoardRes(snesVideoBoardRes() == SNES_RES_STILL
                             ? SNES_RES_MOVING : SNES_RES_STILL);
    }
    ++calib_frame;
}
