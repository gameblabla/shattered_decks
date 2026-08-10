/* FM TOWNS Marty bare-metal entry point for the waifu card game port.
 *
 * This is the payload's C entry (called by src/platform/fmtowns/boot/head.S
 * as `start_main`, no DOS / no TOWNS OS -- see FMTOWNSCD_EXAMPLE_Cube's
 * README.md and docs/HOWFMTOWNS_BOOTS_FROM_CD.txt for how the IPL4 boot
 * sector gets here).
 *
 * Milestone 1 status (see src/platform/fmtowns/STATUS.md): this only proves
 * out the boot -> 32-bit flat mode -> 256x240 8bpp CRTC/palette -> VRAM
 * present path with a synthetic test pattern. It does not yet call into the
 * portable game core (src/game, src/engine) -- that is the next milestone
 * and needs the platform.h seam (src/platform/fmtowns/waifu_fmtowns_platform.c,
 * added in this same commit but not yet wired to a game loop) plus a CD asset
 * pipeline analogous to the CD32X port's waifu_cd32x_cdrom.c.
 */
#include <stdint.h>

#include "libfmt.h"
#include "media.h"
#include "test.h"   /* struct cpu_ident -- head.S writes CPUID probe results
                        into cpu_id at this struct's exact field offsets. */

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

void start_main(void)
{
    (void)fmt_media_init();

    fmtowns_draw_test_pattern();

    for (;;) {
        fmt_wait_vsync();
    }
}
