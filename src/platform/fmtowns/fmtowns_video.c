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

/* Last palette actually uploaded, so an unchanged one can be skipped.  Each
 * entry costs four I/O port writes (common/palette.c), so re-sending all 256
 * every frame is ~1000 port accesses -- easily milliseconds on a 386SX, and
 * pure waste on the many frames where the palette is identical.  Comparing
 * 768 bytes of RAM first is far cheaper than the writes it avoids, and the
 * frames that do change the palette (fades, the title/ending/dialogue
 * palettes) still upload immediately. */
static unsigned char g_last_palette[256 * 3];
static int g_last_palette_count = -1;

void fmtowns_video_present_8bpp(const unsigned char *framebuffer,
                                 const unsigned char *palette_rgb888,
                                 int palette_count)
{
    int i;
    int changed = (palette_count != g_last_palette_count);

    if (!changed) {
        for (i = 0; i < palette_count * 3; ++i) {
            if (g_last_palette[i] != palette_rgb888[i]) {
                changed = 1;
                break;
            }
        }
    }
    if (changed) {
        for (i = 0; i < palette_count * 3; ++i) {
            g_last_palette[i] = palette_rgb888[i];
        }
        g_last_palette_count = palette_count;
        fmt_load_palette(palette_rgb888, palette_count);
    }

    fmt_put_image(framebuffer, 256, 240, 256);
    if (fmt_page_flipping_available()) {
        /* Already waits for a fresh vblank before switching pages, so the
         * caller must not wait again -- doing both halves the frame rate. */
        fmt_flip_page();
    } else {
        fmt_wait_vsync();
    }
}

int fmtowns_video_present_waits_vblank(void)
{
    return 1;
}

void fmtowns_video_wait_vblank(void)
{
    fmt_wait_vsync();
}
