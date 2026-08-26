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

/* Last palette actually uploaded, so unchanged entries can be skipped.  Each
 * entry costs four I/O port writes (common/palette.c), so re-sending all 256
 * every frame is ~1000 port accesses -- easily milliseconds on a 386SX, and
 * pure waste on the many frames where the palette is identical.  Comparing
 * 768 bytes of RAM first is far cheaper than the writes it avoids.  It also
 * matters for correctness now, not just speed: the upload has to fit inside
 * the vertical blanking window (see below), so the fewer entries it touches
 * the better.  A fade still rewrites all 256 -- that is the worst case the
 * window has to absorb. */
static unsigned char g_last_palette[256 * 3];
static int g_last_palette_count = -1;

static int palette_entry_differs(const unsigned char *pal, int i)
{
    return g_last_palette[i * 3 + 0] != pal[i * 3 + 0]
        || g_last_palette[i * 3 + 1] != pal[i * 3 + 1]
        || g_last_palette[i * 3 + 2] != pal[i * 3 + 2];
}

/* See fmtowns_video_take_vblank_wait_us(). */
static uint32_t g_vblank_wait_us;

uint32_t fmtowns_video_take_vblank_wait_us(void)
{
    uint32_t us = g_vblank_wait_us;
    g_vblank_wait_us = 0;
    return us;
}

void fmtowns_video_present_8bpp(const unsigned char *framebuffer,
                                 const unsigned char *palette_rgb888,
                                 int palette_count, int fade_q8)
{
    fmtowns_video_present_8bpp_rows(framebuffer, palette_rgb888, palette_count,
                                    fade_q8, 0, 0);
}

void fmtowns_video_present_8bpp_rows(const unsigned char *framebuffer,
                                      const unsigned char *palette_rgb888,
                                      int palette_count, int fade_q8,
                                      const unsigned char *row_mask,
                                      const unsigned char *force_mask)
{
    /* The faded copy, built only on frames that are actually fading.  Static
     * rather than automatic because the payload's stack is small and 768
     * bytes of it is not free. */
    static unsigned char faded[256 * 3];
    int i;
    int force = (palette_count != g_last_palette_count);

    if (fade_q8 < 256) {
        if (fade_q8 < 0) fade_q8 = 0;
        for (i = 0; i < palette_count * 3; ++i) {
            faded[i] = (unsigned char)(((unsigned int)palette_rgb888[i]
                                        * (unsigned int)fade_q8) >> 8);
        }
        palette_rgb888 = faded;
    }

    /* VRAM first: the draw page is not being scanned out, so the blit has no
     * timing constraint and can have the whole active-display period.
     * Only the 32-byte groups that changed are written -- see
     * fmt_put_image_dirty() in libfmt.c. */
#if defined(FMTOWNS_MEASURE_BLIT_EVERY)
    /* Attribution knob: run the VRAM blit only every Nth frame, so the
     * `present` figure in the frame stamp becomes
     * (palette + flip) + (blit / N) and the blit's own share can be solved
     * for.  Skipping it outright is no use -- the screen then never updates
     * and the stamp cannot be read back. */
    {
        static unsigned int tick;
        if ((tick++ % (FMTOWNS_MEASURE_BLIT_EVERY)) == 0)
            fmt_put_image_dirty_rows(framebuffer, row_mask, force_mask);
    }
#else
    fmt_put_image_dirty_rows(framebuffer, row_mask, force_mask);
#endif

    /* Palette RAM, by contrast, is read by the CRTC on every displayed pixel,
     * and the TOWNS palette ports latch immediately.  Writing them while the
     * picture is being scanned out changes the colours mid-frame, which shows
     * on hardware as flashing/tearing bands -- most obvious during a fade,
     * where every entry is rewritten every single frame (the flicker when
     * entering the game).  So park on a fresh vertical blank BEFORE touching
     * a single palette entry, and do the page flip in that same window rather
     * than waiting for a second one. */
    {
        uint16_t before = fmtowns_clock_ticks();
        fmt_wait_vsync();
        g_vblank_wait_us +=
            fmtowns_ticks_us((uint16_t)(before - fmtowns_clock_ticks()));
    }

    i = 0;
    while (i < palette_count) {
        int run;
        if (!force && !palette_entry_differs(palette_rgb888, i)) {
            ++i;
            continue;
        }
        run = i;
        while (run < palette_count
               && (force || palette_entry_differs(palette_rgb888, run))) {
            g_last_palette[run * 3 + 0] = palette_rgb888[run * 3 + 0];
            g_last_palette[run * 3 + 1] = palette_rgb888[run * 3 + 1];
            g_last_palette[run * 3 + 2] = palette_rgb888[run * 3 + 2];
            ++run;
        }
        fmt_load_palette_range(palette_rgb888, i, run - i);
        i = run;
    }
    g_last_palette_count = palette_count;

    /* Still inside the same blanking window -- flip without waiting again,
     * otherwise the palette and the frame it belongs to land a field apart
     * and the frame rate halves. */
    fmt_flip_page_now();
}

int fmtowns_video_present_waits_vblank(void)
{
    return 1;
}

void fmtowns_video_wait_vblank(void)
{
    fmt_wait_vsync();
}
