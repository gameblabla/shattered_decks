/* FM TOWNS Marty bare-metal entry point for the waifu card game port.
 *
 * This is the payload's C entry (called by src/platform/fmtowns/boot/head.S
 * as `start_main`, no DOS / no TOWNS OS -- see FMTOWNSCD_EXAMPLE_Cube's
 * README.md and docs/HOWFMTOWNS_BOOTS_FROM_CD.txt for how the IPL4 boot
 * sector gets here). See src/platform/fmtowns/STATUS.md for the milestone
 * plan and current status; it does not yet call into the portable game
 * core (src/game, src/engine) -- that is a later milestone.
 *
 * Milestone 1: fmtowns_draw_test_pattern() below, a synthetic 256x240 8bpp
 * pattern, proved the boot -> protected mode -> CRTC/palette -> VRAM
 * present path end to end.
 *
 * Milestone 2 (current default, FMTOWNS_MILESTONE 2): presents the game's
 * real title-screen art (src/generated/title_asset.h, the same asset every
 * other platform's title screen uses) through fmtowns_video.c instead of a
 * synthetic pattern, linked directly into the payload
 * (fmtowns_title_asset.S). Proves the present path against real production
 * pixel data + palette ahead of CD-ROM loading (milestone 3) or the game
 * core itself compiling for this target.
 */
#include <stdint.h>

#include "fmtowns_video.h"
#include "libfmt.h"
#include "media.h"
#include "test.h"   /* struct cpu_ident -- head.S writes CPUID probe results
                        into cpu_id at this struct's exact field offsets. */

#ifndef FMTOWNS_MILESTONE
#define FMTOWNS_MILESTONE 2
#endif

struct cpu_ident cpu_id;

/* Trap handler required by head.S; nothing to do on this milestone. */
struct eregs;
void inter(struct eregs *trap_regs)
{
    (void)trap_regs;
}

/* 256x240 8bpp test pattern: four colour quadrants plus a white border and a
 * centred block, close to what FMTOWNSCD_EXAMPLE_Cube/src/main.c's
 * draw_256x240_test() draws, but kept local so this platform directory does
 * not depend on the example's main.c. Confirms CRTC mode set, palette load,
 * and the VRAM present path all work end to end before any game asset
 * pipeline exists. */
static void fmtowns_draw_test_pattern(void)
{
    static uint8_t frame[256 * 240];
    static const uint8_t palette[5 * 3] = {
          0,   0,   0,   /* 0: black (border background) */
        200,  40,  40,   /* 1: red quadrant */
         40, 200,  40,   /* 2: green quadrant */
         40,  40, 200,   /* 3: blue quadrant */
        230, 230, 230    /* 4: white border / centre block */
    };
    uint16_t x, y;

    fmt_set_mode(FMT_MODE_256x240_8BPP);
    fmt_load_palette(palette, 5);

    for (y = 0; y < 240; ++y) {
        for (x = 0; x < 256; ++x) {
            uint8_t color = ((x >> 5) ^ (y >> 5)) & 1 ? 1 : 2;
            if (x >= 96 && x < 160 && y >= 88 && y < 152) {
                color = 4;
            } else if (x >= 128) {
                color = ((x >> 5) ^ (y >> 5)) & 1 ? 3 : 2;
            }
            if (x == 0 || x == 255 || y == 0 || y == 239) {
                color = 4;
            }
            frame[(uint32_t)y * 256u + x] = color;
        }
    }

    fmt_put_image(frame, 256, 240, 256);
    if (fmt_page_flipping_available()) {
        fmt_flip_page();
    }
}

#if FMTOWNS_MILESTONE >= 2
/* Linked in by fmtowns_title_asset.S -- see that file for provenance. */
extern const unsigned char g_fmtowns_title_pixels[];
extern const unsigned char g_fmtowns_title_palette[];

static void fmtowns_present_title_asset(void)
{
    fmtowns_video_init();
    fmtowns_video_present_8bpp(g_fmtowns_title_pixels, g_fmtowns_title_palette, 256);
}
#endif

void start_main(void)
{
    (void)fmt_media_init();

#if FMTOWNS_MILESTONE >= 2
    fmtowns_present_title_asset();
#else
    fmtowns_draw_test_pattern();
#endif

    for (;;) {
        fmt_wait_vsync();
    }
}
