/* FM TOWNS video present, milestone 2.
 *
 * Wraps libfmt's CRTC/palette/VRAM helpers (src/platform/fmtowns/common/
 * libfmt.c) into the same shape the CD32X/PC-FX ports use for their
 * present call (waifu_cd32x_video_present_8bpp(), etc): set the mode once,
 * then each frame load the palette and blit an 8bpp WAIFU_FM_WIDTH x
 * WAIFU_FM_HEIGHT (256x240, src/engine/cfx_screen_config.h) buffer and
 * flip. Not yet fed by the portable game core's real framebuffer/palette
 * (see STATUS.md) -- milestone 2 feeds it the game's real title-screen
 * asset instead of a synthetic pattern, to prove the present path against
 * real production pixel data ahead of the game core actually compiling for
 * this target.
 */
#include "fmtowns_video.h"
#include "libfmt.h"

void fmtowns_video_init(void)
{
    fmt_set_mode(FMT_MODE_256x240_8BPP);
}

void fmtowns_video_present_8bpp(const unsigned char *framebuffer,
                                 const unsigned char *palette_rgb888,
                                 int palette_count)
{
    fmt_load_palette(palette_rgb888, palette_count);
    fmt_put_image(framebuffer, 256, 240, 256);
    if (fmt_page_flipping_available()) {
        fmt_flip_page();
    }
}

void fmtowns_video_wait_vblank(void)
{
    fmt_wait_vsync();
}
