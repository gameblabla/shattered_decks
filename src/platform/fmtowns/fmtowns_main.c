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
 * Milestone 2: presents the game's real title-screen art
 * (src/generated/title_asset.h, the same asset every other platform's
 * title screen uses) through fmtowns_video.c instead of a synthetic
 * pattern, linked directly into the payload with .incbin
 * (fmtowns_title_asset.S). Proved the present path against real production
 * pixel data + palette ahead of CD-ROM loading.
 *
 * Milestone 3: the exact same title asset, but read at runtime off the CD
 * image's ISO9660 filesystem (src/platform/fmtowns/common/cdrom.c +
 * iso9660.c, through media.h's fmt_media_load()) instead of being linked
 * into the payload. Proved the CD-ROM read path this platform's real asset
 * streaming (cards, portraits, ending art, etc) will eventually go
 * through.
 *
 * Milestone 4: reads the real 6-button pad
 * (src/platform/fmtowns/common/pad.c, unused since milestone 1) every
 * frame through fmtowns_input.c and renders its 12-bit status as a strip of
 * lit/unlit blocks across the bottom of the title screen -- pixel indices
 * 254/255 are repurposed for this (see fmtowns_pad_status_loop() below).
 * This is a debug overlay, not the final HUD treatment: it proved the pad
 * read call executes safely every vblank without hanging boot (it has its
 * own I/O strobe wait loop -- see pad.c).
 *
 * Milestone 5 (current default, FMTOWNS_MILESTONE 5): starts CD-DA music
 * (src/platform/fmtowns/common/cdda.c, unused since milestone 1) through
 * fmtowns_audio.c once, right after the title asset's CD reads finish and
 * before entering the live pad loop -- see cdda.h's block comment on why
 * the drive cannot read data sectors and play a CD-DA track at once, which
 * is exactly why this waits until after fmtowns_load_title_asset_cdrom().
 * A one-pixel corner swatch (top-left 8x8, palette index 253) reflects
 * fmt_cdda_state() every frame: green while playing, red otherwise, since
 * nothing in this headless setup can otherwise confirm audio came out of
 * the (virtual) speakers -- see STATUS.md's verification notes. Built with
 * a real music track staged as CD-DA track 2 by Makefile.fmtowns (a clip
 * of Music/Titlescreen_MoonlitCipher.wav, the same music PC-FX/CD32X play
 * over their own title screens).
 */
#include <stdint.h>

#include "fmtowns_audio.h"
#include "fmtowns_input.h"
#include "fmtowns_video.h"
#include "libfmt.h"
#include "media.h"
#include "test.h"   /* struct cpu_ident -- head.S writes CPUID probe results
                        into cpu_id at this struct's exact field offsets. */

#ifndef FMTOWNS_MILESTONE
#define FMTOWNS_MILESTONE 5
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

#if FMTOWNS_MILESTONE == 2
/* Linked in by fmtowns_title_asset.S -- see that file for provenance. */
extern const unsigned char g_fmtowns_title_pixels[];
extern const unsigned char g_fmtowns_title_palette[];

static void fmtowns_present_title_asset_incbin(void)
{
    fmtowns_video_init();
    fmtowns_video_present_8bpp(g_fmtowns_title_pixels, g_fmtowns_title_palette, 256);
}
#endif

#if FMTOWNS_MILESTONE >= 3
/* Milestone 3: the same two files, staged onto the CD image as CD/TITLE.BIN
 * and CD/TITLE.PAL (see Makefile.fmtowns), read back at runtime through
 * fmt_media_load() -> fmt_iso9660_load() -> fmt_cdrom_read(). Static, not
 * stack, both to keep them out of the payload's small default stack and
 * because .bss this size is exactly what the PAYLOAD_LIMIT check in
 * Makefile.fmtowns exists to catch if it ever grows past what fits below
 * the FMR VRAM window. */
static uint8_t g_fmtowns_title_pixels_cd[256 * 240];
static uint8_t g_fmtowns_title_palette_cd[256 * 3];

/* Loops forever showing a solid colour if either CD read fails, rather than
 * presenting a half-loaded or garbage frame -- there is no console/serial
 * output on this target to report the failure through instead. */
static void fmtowns_fail_pattern(uint8_t color)
{
    static uint8_t frame[256 * 240];
    static uint8_t palette[3];
    uint32_t i;

    palette[0] = color; palette[1] = 0; palette[2] = 0;
    for (i = 0; i < sizeof(frame); ++i) frame[i] = 0;
    fmtowns_video_init();
    fmtowns_video_present_8bpp(frame, palette, 1);
    for (;;) fmt_wait_vsync();
}

static void fmtowns_load_title_asset_cdrom(void)
{
    int32_t got;

    got = fmt_media_load("TITLE.BIN", g_fmtowns_title_pixels_cd, sizeof(g_fmtowns_title_pixels_cd));
    if (got != (int32_t)sizeof(g_fmtowns_title_pixels_cd)) {
        fmtowns_fail_pattern(255);   /* CD read of the pixel data failed */
    }

    got = fmt_media_load("TITLE.PAL", g_fmtowns_title_palette_cd, sizeof(g_fmtowns_title_palette_cd));
    if (got != (int32_t)sizeof(g_fmtowns_title_palette_cd)) {
        fmtowns_fail_pattern(128);   /* CD read of the palette failed */
    }
}

#if FMTOWNS_MILESTONE == 3
static void fmtowns_present_title_asset_cdrom(void)
{
    fmtowns_load_title_asset_cdrom();
    fmtowns_video_init();
    fmtowns_video_present_8bpp(g_fmtowns_title_pixels_cd, g_fmtowns_title_palette_cd, 256);
}
#endif
#endif /* FMTOWNS_MILESTONE >= 3 */

#if FMTOWNS_MILESTONE >= 4
#define FMTOWNS_PAD_IDX_OFF 254
#define FMTOWNS_PAD_IDX_ON  255
#define FMTOWNS_PAD_BITS    12
#define FMTOWNS_PAD_CELL_W  (256 / FMTOWNS_PAD_BITS)
#define FMTOWNS_PAD_STRIP_Y 232
#define FMTOWNS_PAD_STRIP_H 8

/* Repaints the bottom 8-pixel strip with one FMTOWNS_PAD_CELL_W-wide block
 * per pad bit, lit (index 255, forced bright green below) when that bit
 * reads pressed (0, per pad.h's idle-high wiring) and unlit (index 254,
 * forced near-black) otherwise. */
static void fmtowns_draw_pad_status(uint8_t *frame, unsigned int pad_status)
{
    int bit, x, y;

    for (bit = 0; bit < FMTOWNS_PAD_BITS; ++bit) {
        uint8_t idx = (pad_status & (1u << bit)) ? FMTOWNS_PAD_IDX_OFF : FMTOWNS_PAD_IDX_ON;
        int x0 = bit * FMTOWNS_PAD_CELL_W;
        for (y = FMTOWNS_PAD_STRIP_Y; y < FMTOWNS_PAD_STRIP_Y + FMTOWNS_PAD_STRIP_H; ++y) {
            for (x = x0; x < x0 + FMTOWNS_PAD_CELL_W - 1; ++x) {
                frame[(uint32_t)y * 256u + x] = idx;
            }
        }
    }
}

#if FMTOWNS_MILESTONE >= 5
#define FMTOWNS_CDDA_IDX      253
#define FMTOWNS_CDDA_SWATCH_W 8
#define FMTOWNS_CDDA_SWATCH_H 8

/* Repaints palette index FMTOWNS_CDDA_IDX to reflect fmt_cdda_state()
 * (green while playing == FMT_CDDA_PLAYING (1), red otherwise) and fills
 * the top-left 8x8 corner with it. The palette entry, not just the pixel
 * index, has to change every call -- unlike the pad strip (fixed
 * on/off colours picked once) this swatch's *colour* is the live signal,
 * so the caller must re-upload the palette after this returns. */
static void fmtowns_draw_cdda_status(uint8_t *frame, uint8_t *palette)
{
    int x, y;
    int playing = (fmtowns_audio_state() == 1 /* FMT_CDDA_PLAYING */);

    palette[FMTOWNS_CDDA_IDX * 3 + 0] = playing ? 40  : 220;
    palette[FMTOWNS_CDDA_IDX * 3 + 1] = playing ? 220 : 40;
    palette[FMTOWNS_CDDA_IDX * 3 + 2] = 40;

    for (y = 0; y < FMTOWNS_CDDA_SWATCH_H; ++y) {
        for (x = 0; x < FMTOWNS_CDDA_SWATCH_W; ++x) {
            frame[(uint32_t)y * 256u + x] = FMTOWNS_CDDA_IDX;
        }
    }
}
#endif

/* Milestone 4: load the title asset off CD exactly like milestone 3, then
 * stay live -- every vblank, re-read the real pad and repaint the status
 * strip, proving fmt_pad_read() (src/platform/fmtowns/common/pad.c) runs
 * safely on real hardware timing every frame rather than just once at
 * boot. Milestone 5 additionally starts CD-DA music once before the loop
 * and repaints a corner swatch each frame from its live state (see the
 * FMTOWNS_MILESTONE >= 5 block comment above fmtowns_draw_cdda_status()). */
static void fmtowns_pad_status_loop(void)
{
    fmtowns_load_title_asset_cdrom();
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_OFF * 3 + 0] = 24;
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_OFF * 3 + 1] = 24;
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_OFF * 3 + 2] = 24;
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_ON * 3 + 0] = 40;
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_ON * 3 + 1] = 220;
    g_fmtowns_title_palette_cd[FMTOWNS_PAD_IDX_ON * 3 + 2] = 60;

#if FMTOWNS_MILESTONE >= 5
    {
        int playing = fmtowns_audio_start_music();
        /* Bootstrap swatch colour before the first real state poll: dim
         * grey if the drive never even accepted the play command (e.g. a
         * data-only .iso build with no CD-DA track), otherwise let the
         * per-frame state poll below decide green/red. */
        g_fmtowns_title_palette_cd[FMTOWNS_CDDA_IDX * 3 + 0] = playing ? 220 : 60;
        g_fmtowns_title_palette_cd[FMTOWNS_CDDA_IDX * 3 + 1] = playing ? 40  : 60;
        g_fmtowns_title_palette_cd[FMTOWNS_CDDA_IDX * 3 + 2] = playing ? 40  : 60;
    }
#endif

    fmtowns_video_init();
    fmt_load_palette(g_fmtowns_title_palette_cd, 256);

    for (;;) {
        unsigned int pad_status = fmtowns_input_read_pad1();
        fmtowns_draw_pad_status(g_fmtowns_title_pixels_cd, pad_status);
#if FMTOWNS_MILESTONE >= 5
        fmtowns_draw_cdda_status(g_fmtowns_title_pixels_cd, g_fmtowns_title_palette_cd);
        fmt_load_palette(g_fmtowns_title_palette_cd, 256);
#endif
        fmt_put_image(g_fmtowns_title_pixels_cd, 256, 240, 256);
        if (fmt_page_flipping_available()) {
            fmt_flip_page();
        } else {
            fmt_wait_vsync();
        }
    }
}
#endif

void start_main(void)
{
    (void)fmt_media_init();

#if FMTOWNS_MILESTONE >= 4
    fmtowns_pad_status_loop();
#elif FMTOWNS_MILESTONE == 3
    fmtowns_present_title_asset_cdrom();
#elif FMTOWNS_MILESTONE == 2
    fmtowns_present_title_asset_incbin();
#else
    fmtowns_draw_test_pattern();
#endif

    for (;;) {
        fmt_wait_vsync();
    }
}
