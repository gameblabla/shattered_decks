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
 *
 * Milestone 6: a flat-shaded, backface-culled rotating cube
 * (fmtowns_cube_demo.c) replaces the title art as the main scene, still
 * under the same pad-status strip and CD-DA swatch overlays. This is the
 * "flat-shaded 3D renderer" milestone: see fmtowns_cube_demo.c's own header
 * comment for why it is a standalone demo (integer-only rasterizer, no FPU
 * assumed) rather than the portable game core's actual renderer3d.c
 * pipeline -- src/engine/renderer3d_fmtowns.c registers that seam for
 * later.
 *
 * Milestone 7 (current default, FMTOWNS_MILESTONE 7): the real game core
 * (src/main.c's waifu_fm_init()/waifu_fm_step() loop, the same one
 * cd32x_sh2_main.c drives) compiled freestanding for -m32 -march=i386 and
 * wired to this target's video/input/audio backends -- see
 * waifu_fm_game_loop() below. src/platform/fmtowns/waifu_fmtowns_runtime.c
 * supplies the freestanding libc pieces gcc still emits calls to (memcpy,
 * strcmp, snprintf, ...) mirroring waifu_cd32x_runtime.c; ...platform.c
 * implements src/engine/platform.h's seam (session-only RAM saves, no
 * hardware background/text/sprite layers yet -- see that file's header);
 * ...cdrom.c implements the CD asset blob reader assets.c calls into for
 * WAIFU_ASSET_USE_CDROM builds, the same shape as waifu_pcfx_cdrom.c.
 */
#include <stdint.h>

#include "fmtowns_audio.h"
#include "fmtowns_cd_diag.h"
#include "fmtowns_cube_demo.h"
#include "fmtowns_input.h"
#include "fmtowns_sfx.h"
#include "fmtowns_video.h"
#include "io.h"     /* inw() -- the free-running 1us counter used to profile
                       frame time; see fmtowns_prof_split() below. */
#include "libfmt.h"
#include "machine.h" /* fmt_machine_detect() -- Marty/UX (narrow) vs standard
                        (wide) memory map, see machine.h's block comment */
#include "media.h"
#include "test.h"   /* struct cpu_ident -- head.S writes CPUID probe results
                        into cpu_id at this struct's exact field offsets. */

#ifndef FMTOWNS_MILESTONE
#define FMTOWNS_MILESTONE 7
#endif

#if FMTOWNS_MILESTONE >= 7
#include "game_api.h"
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
    fmt_wait_vsync();
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
    fmtowns_video_present_8bpp(g_fmtowns_title_pixels, g_fmtowns_title_palette, 256, 256);
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
    fmtowns_video_present_8bpp(frame, palette, 1, 256);
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
    fmtowns_video_present_8bpp(g_fmtowns_title_pixels_cd, g_fmtowns_title_palette_cd, 256, 256);
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
    fmt_wait_vsync();
    fmt_load_palette(g_fmtowns_title_palette_cd, 256);

    for (;;) {
        unsigned int pad_status = fmtowns_input_read_pad1();
        fmtowns_draw_pad_status(g_fmtowns_title_pixels_cd, pad_status);
#if FMTOWNS_MILESTONE >= 5
        fmtowns_draw_cdda_status(g_fmtowns_title_pixels_cd, g_fmtowns_title_palette_cd);
#endif
        fmt_put_image(g_fmtowns_title_pixels_cd, 256, 240, 256);
        /* Palette RAM is CRTC-visible immediately: only ever write it inside
         * vertical blanking, then flip in the same window. */
        fmt_wait_vsync();
#if FMTOWNS_MILESTONE >= 5
        fmt_load_palette(g_fmtowns_title_palette_cd, 256);
#endif
        fmt_flip_page_now();
    }
}
#endif

#if FMTOWNS_MILESTONE >= 6
static uint8_t g_fmtowns_cube_frame[256 * 240];
static uint8_t g_fmtowns_cube_palette[256 * 3];

/* Milestone 6: no CD reads at all (the cube demo needs no assets), so
 * CD-DA can start immediately -- unlike fmtowns_pad_status_loop() above,
 * there is no title-asset load to wait out first. Draws the rotating cube
 * into rows 0-231 every frame, then the same pad-status strip (rows
 * 232-239) and CD-DA swatch (top-left 8x8, drawn after the cube so it
 * overlays it) as milestones 4-5. */
static void fmtowns_cube_demo_loop(void)
{
    fmtowns_cube_demo_palette(g_fmtowns_cube_palette);
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_OFF * 3 + 0] = 24;
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_OFF * 3 + 1] = 24;
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_OFF * 3 + 2] = 24;
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_ON * 3 + 0] = 40;
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_ON * 3 + 1] = 220;
    g_fmtowns_cube_palette[FMTOWNS_PAD_IDX_ON * 3 + 2] = 60;

    (void)fmtowns_audio_start_music();

    fmtowns_video_init();
    fmt_wait_vsync();
    fmt_load_palette(g_fmtowns_cube_palette, 256);

    for (;;) {
        unsigned int pad_status = fmtowns_input_read_pad1();

        fmtowns_cube_demo_frame(g_fmtowns_cube_frame);
        fmtowns_draw_pad_status(g_fmtowns_cube_frame, pad_status);
        fmtowns_draw_cdda_status(g_fmtowns_cube_frame, g_fmtowns_cube_palette);

        fmt_put_image(g_fmtowns_cube_frame, 256, 240, 256);
        /* Palette RAM is CRTC-visible immediately: only ever write it inside
         * vertical blanking, then flip in the same window. */
        fmt_wait_vsync();
        fmt_load_palette(g_fmtowns_cube_palette, 256);
        fmt_flip_page_now();
    }
}
#endif

#if FMTOWNS_MILESTONE >= 7
/* Raw pad bit layout (pad.h): 0=Up 1=Down 2=Left 3=Right 4=A 5=B 6=RUN
 * 7=SELECT 8=Z 9=Y 10=X 11=C, 0=pressed/1=released (idle-high wiring). RUN
 * is this port's START; Z stands in for the fourth face button the other
 * targets call TAB (PC-FX SELECT / CD32X's fourth button) since neither
 * SELECT nor the other three face buttons are otherwise used yet. */
#define FMTOWNS_PAD_BIT_UP     0
#define FMTOWNS_PAD_BIT_DOWN   1
#define FMTOWNS_PAD_BIT_LEFT   2
#define FMTOWNS_PAD_BIT_RIGHT  3
#define FMTOWNS_PAD_BIT_A      4
#define FMTOWNS_PAD_BIT_B      5
#define FMTOWNS_PAD_BIT_RUN    6
#define FMTOWNS_PAD_BIT_Z      8

static void fmtowns_read_input(WaifuFmInput *in)
{
    unsigned int s = fmtowns_input_read_pad1();
    in->up    = (s & (1u << FMTOWNS_PAD_BIT_UP))    ? 0 : 1;
    in->down  = (s & (1u << FMTOWNS_PAD_BIT_DOWN))  ? 0 : 1;
    in->left  = (s & (1u << FMTOWNS_PAD_BIT_LEFT))  ? 0 : 1;
    in->right = (s & (1u << FMTOWNS_PAD_BIT_RIGHT)) ? 0 : 1;
    in->a     = (s & (1u << FMTOWNS_PAD_BIT_A))     ? 0 : 1;
    in->b     = (s & (1u << FMTOWNS_PAD_BIT_B))     ? 0 : 1;
    in->start = (s & (1u << FMTOWNS_PAD_BIT_RUN))   ? 0 : 1;
    in->tab   = (s & (1u << FMTOWNS_PAD_BIT_Z))     ? 0 : 1;
}

/* ------------------------------------------------------------------
 * The frame clock.
 *
 * Everything below -- pacing, the profiler, the elapsed-frame count handed
 * to the core -- measures time with PIT channel 1, NOT with the 1 us
 * free-running counter at I/O 0x26 that the rest of this port uses.  That
 * choice is the whole reason the pacing works, so it is worth the paragraph:
 *
 * The 0x26 counter is 16 bits at 1 us and therefore wraps every 65.536 ms.
 * A frame slower than that is indistinguishable from a fast one -- an 80 ms
 * frame reads back as 14.5 ms -- and there is nothing to sample in between,
 * because the CPU is inside waifu_fm_step() for the whole interval.  This
 * port's duel frames are exactly in that range, so the first version of the
 * pacing measured an 80 ms frame as 14.5 ms, concluded it owed the game one
 * 60 Hz period, and left the slow-motion bug it was written to fix fully in
 * place (while also spinning out the "missing" 2 ms, making it worse).
 *
 * PIT channel 1 (I/O 0x42, control 0x46) runs at 307,200 Hz.  Loaded with a
 * count of 0 in mode 0 it becomes a free-running 16-bit down-counter that
 * wraps every 65536 ticks -- 213.3 ms, over three whole 0x26 wraps -- with
 * 3.26 us of resolution, which is far finer than anything here needs.  Two
 * further pieces of luck make it the right clock rather than merely a longer
 * one:
 *
 *   - 307200 / 60 is exactly 5120, so a 60 Hz frame is a whole number of
 *     ticks and the pacing needs no division on a 16 MHz 386SX.
 *   - Nothing else in the game build touches channel 1.  common/vgmplay.c
 *     does (mode 0 one-shots), but that player only runs from the standalone
 *     VGM path, never from the game loop.  Channel 0 -- the system timer --
 *     is left alone.
 *
 * Interrupts are masked (boot/head.S ends with cli), so channel 1's timeout
 * flag setting at I/O 0x60 goes nowhere; we never read it.
 *
 * The remaining blind spot is a frame longer than 213 ms, which aliases the
 * same way.  In practice that only happens across a CD read, which the
 * pacing already refuses to charge the game for (see the cap below).
 */
#define FMTOWNS_PIT1_COUNT      0x0042
#define FMTOWNS_PIT_CONTROL     0x0046
#define FMTOWNS_TICKS_PER_FRAME 5120u    /* 307200 Hz / 60 -- exact */

static void fmtowns_clock_init(void)
{
    outb(0x70, FMTOWNS_PIT_CONTROL);  /* ch1, lo+hi byte, mode 0, binary */
    outb(0x00, FMTOWNS_PIT1_COUNT);
    outb(0x00, FMTOWNS_PIT1_COUNT);   /* count 0 == 65536 ticks == 213.3 ms */
}

/* The raw down-counter.  Elapsed ticks between two reads are
 * (uint16_t)(earlier - later) -- it counts down, and the 16-bit subtraction
 * absorbs the wrap for free. */
uint16_t fmtowns_clock_ticks(void)
{
    unsigned int lo, hi;
    outb(0x40, FMTOWNS_PIT_CONTROL);  /* latch channel 1 */
    lo = inb(FMTOWNS_PIT1_COUNT);
    hi = inb(FMTOWNS_PIT1_COUNT);
    return (uint16_t)(lo | (hi << 8));
}

/* Ticks -> microseconds: one tick is 3.255208 us, and 13333/4096 is that to
 * within 0.003%.  Kept as a shift so there is no 32-bit divide; the product
 * stays inside 32 bits for any interval under a second. */
uint32_t fmtowns_ticks_us(uint32_t ticks)
{
    return (ticks * 13333u) >> 12;
}

/* ------------------------------------------------------------------
 * Frame pacing.
 *
 * The present path ends in fmt_flip_page(), which waits for a vsync edge
 * off I/O 0x443, and for a long time that was the only thing holding the
 * game to a sane speed.  It is not enough.  Once the game step and the VRAM
 * blit got fast enough, that wait started returning after ~3 ms rather than
 * a display field, and the whole game ran at ~325 frames per second under
 * Tsugaru -- every animation five times too fast.  The port had simply
 * never been quick enough to find out.
 *
 * So the loop paces itself against the frame clock above instead of trusting
 * the video hardware to do it.
 *
 * A frame that overruns its budget is not slowed down further: the spin only
 * ever waits out the remainder, so a heavy frame just runs late.
 *
 * THE RETURN VALUE IS THE OTHER HALF OF THE JOB.  Pacing alone only stops
 * the game running too *fast*; on its own it leaves the opposite bug in
 * place, which is the one this port actually shipped with.  waifu_fm_step()
 * advances the game by however many 60 Hz frames the platform tells it, and
 * this loop used to hard-code waifu_fm_set_frame_vblanks(1) -- so a frame
 * that took 33 ms still counted as one frame of game time and the entire
 * game, phases and animations alike, ran at half speed.  The heavier the
 * scene, the slower the game progressed.
 *
 * This returns how many 60 Hz periods the frame really consumed, which the
 * loop hands straight to the core.  Two details make that count honest:
 *
 *  - The leftover ticks are carried, not dropped.  A steady 25 ms frame is
 *    1.5 periods; truncating every frame to 1 would lose a third of the
 *    game's clock.  With the carry it alternates 1,2,1,2 and averages
 *    exactly right.
 *  - The carry is capped at one period so a long stall (a CD read, a cache
 *    prewarm) cannot bank seconds of debt and then fast-forward the game
 *    once it clears.  The core clamps the count itself as well.
 */
static unsigned int fmtowns_frame_pace(void)
{
    static uint16_t mark;
    static int armed;
    static uint32_t carry;      /* unspent ticks from earlier frames */
    uint32_t elapsed;
    unsigned int periods;

    if (!armed) {
        fmtowns_clock_init();
        mark = fmtowns_clock_ticks();
        armed = 1;
        return 1;
    }

    elapsed = carry;
    for (;;) {
        uint16_t now = fmtowns_clock_ticks();
        elapsed += (uint16_t)(mark - now);   /* counts down */
        mark = now;
        if (elapsed >= FMTOWNS_TICKS_PER_FRAME) break;
    }

    periods = 0;
    while (elapsed >= FMTOWNS_TICKS_PER_FRAME) { /* no 32-bit divide on a 386SX */
        elapsed -= FMTOWNS_TICKS_PER_FRAME;
        ++periods;
        if (periods >= 8u) {                /* a stall, not a slow frame */
            elapsed = 0;
            break;
        }
    }
    carry = elapsed;
    return periods;
}

#ifdef FMTOWNS_DEBUG_INPUT
/* Generated from a text script by tools/fmtowns/gen_input_script.py -- see
 * that tool's docstring for why the payload has to press its own buttons in
 * this environment (nothing can inject a pad press into Tsugaru_CUI: its
 * monitor's TYPE reaches the keyboard controller but not the game ports, and
 * AUTOSHOT only drives a physical host gamepad).  Debug builds only:
 * FMTOWNS_DEBUG_INPUT is off in the default build, so the shipping payload
 * carries neither the table nor this code. */
#include "fmtowns_input_script.h"

/* ORs the scripted buttons into whatever the real pad reported, so a script
 * never blocks a human also holding the pad, and the real fmtowns_read_input()
 * path stays exercised underneath. */
static void fmtowns_apply_input_script(WaifuFmInput *in, unsigned int frame)
{
    /* First step that has not fully expired.  The table is sorted by frame
     * and `frame` only ever counts up, so steps can be retired for good
     * rather than rescanned every frame -- which matters because a script
     * that walks a whole duel is ~1000 steps, and rescanning all of them at
     * 60 fps on a 386SX is a measurable slice of the frame this build exists
     * to measure. */
    static unsigned int cursor;
    unsigned int bits = 0;
    unsigned int i;

    while (cursor < FMTOWNS_INPUT_SCRIPT_STEPS
           && frame >= (unsigned int)(g_fmtowns_input_script[cursor].frame
                                      + g_fmtowns_input_script[cursor].hold)) {
        ++cursor;
    }

    for (i = cursor; i < FMTOWNS_INPUT_SCRIPT_STEPS; ++i) {
        const FmtownsInputStep *s = &g_fmtowns_input_script[i];
        if (s->frame > frame) break;   /* sorted: nothing later is active yet */
        if (frame < (unsigned int)(s->frame + s->hold)) bits |= s->bits;
    }

    if (bits & (1u << 0)) in->up = 1;
    if (bits & (1u << 1)) in->down = 1;
    if (bits & (1u << 2)) in->left = 1;
    if (bits & (1u << 3)) in->right = 1;
    if (bits & (1u << 4)) in->a = 1;
    if (bits & (1u << 5)) in->b = 1;
    if (bits & (1u << 6)) in->start = 1;
    if (bits & (1u << 7)) in->tab = 1;
}

/* ------------------------------------------------------------------
 * Frame-time measurement.
 *
 * The port's frame rate had only ever been worked out by hand, from
 * wall-clock timings of a headless run divided by a frame count.  That
 * conflates two different things -- how fast the emulated machine runs and
 * how fast the host emulates it -- and it cannot say where inside a frame
 * the time actually goes, which is exactly what an optimisation pass needs
 * to know.
 *
 * The clock is the PIT channel 1 counter set up above: a hardware tick rate
 * that does not depend on CPU speed, so a measurement taken under -NOWAIT is
 * still a true Marty figure, and a 213 ms range so that a duel frame -- which
 * really can exceed the 65.536 ms the old 1 us clock could express -- is
 * measured rather than aliased.  Sections are summed over
 * FMTOWNS_PROF_WINDOW frames and published as averages, because individual
 * frames vary a lot (a CD read, a scene change) and a single sampled frame
 * is not a number worth optimising against.
 */
#define FMTOWNS_PROF_WINDOW 16

static uint16_t g_prof_mark;              /* last timer read */
static uint32_t g_prof_acc_total;         /* running sums over the window */
static uint32_t g_prof_acc_step;
static uint32_t g_prof_acc_present;
static unsigned int g_prof_frames;
/* Published averages, in microseconds.  32-bit on purpose: a single slow
 * frame can exceed the 65.535 ms a uint16_t holds, and a wrapped average
 * reads as an absurdly fast frame rather than as an obviously broken one. */
static uint32_t g_prof_avg_total;
static uint32_t g_prof_avg_step;
static uint32_t g_prof_avg_present;
/* The worst single frame in the window, published alongside the averages.
 * An average over 16 frames hides exactly the frames worth finding: a camera
 * sweep or a fade is a handful of frames inside an otherwise idle scene, and
 * a 130 ms frame every sixteenth frame moves a 16.6 ms average to 23 ms --
 * which reads as "a bit slow" rather than as the eight-frame lurch it is.
 *
 * It runs on a much longer window than the averages, because a screenshot
 * only samples the frames immediately before it: at 16 frames the field
 * describes a quarter of a second out of the twenty between shots, so a
 * capture can walk a whole duel and never once look at the frame that
 * stutters.  256 frames is several seconds of coverage and still recent
 * enough to attribute to the scene the shot shows. */
#define FMTOWNS_PROF_WORST_WINDOW 256
static uint32_t g_prof_acc_worst;
static unsigned int g_prof_worst_frames;
static uint32_t g_prof_worst;

/* Microseconds since the previous call, and re-arm.  The 16-bit wrap is
 * handled for free: the subtraction is done in 16-bit width, so a counter
 * that has wrapped once still yields the correct difference. */
static uint32_t fmtowns_prof_split(void)
{
    uint16_t now = fmtowns_clock_ticks();
    uint16_t delta = (uint16_t)(g_prof_mark - now);   /* counts down */
    g_prof_mark = now;
    return fmtowns_ticks_us(delta);
}

static void fmtowns_prof_frame_end(uint32_t step_us, uint32_t present_us,
                                   uint32_t other_us)
{
    g_prof_acc_step    += step_us;
    g_prof_acc_present += present_us;
    g_prof_acc_total   += step_us + present_us + other_us;
    if (step_us + present_us + other_us > g_prof_acc_worst)
        g_prof_acc_worst = step_us + present_us + other_us;
    if (++g_prof_worst_frames >= FMTOWNS_PROF_WORST_WINDOW) {
        g_prof_worst = g_prof_acc_worst;
        g_prof_acc_worst = 0;
        g_prof_worst_frames = 0;
    }
    if (++g_prof_frames < FMTOWNS_PROF_WINDOW) return;

    g_prof_avg_total   = g_prof_acc_total   / FMTOWNS_PROF_WINDOW;
    g_prof_avg_step    = g_prof_acc_step    / FMTOWNS_PROF_WINDOW;
    g_prof_avg_present = g_prof_acc_present / FMTOWNS_PROF_WINDOW;
    g_prof_acc_total = g_prof_acc_step = g_prof_acc_present = 0;
    g_prof_frames = 0;
}

/* Stamps machine-readable state into the top-left 58 pixels of the frame as
 * little-endian bit patterns (lit = 1), so a screenshot is self-describing:
 *
 *   pixels  0-15  game frame counter
 *   pixels 16-20  CD-DA track number currently started (0 = silence)
 *   pixel     21  set while the drive last reported PLAYING
 *   pixels 22-33  average whole-frame time, in 128us units
 *   pixels 34-45  average waifu_fm_step() time, in 128us units
 *   pixels 46-57  average present time, in 128us units
 *   pixels 58-61  60 Hz periods this frame was charged to the game (pacing)
 *   pixels 62-73  worst whole-frame time in the window, in 128us units
 *   pixels 74-81  waifu_fm_video_fade_q8() for this frame (255 = fully lit)
 *
 * The frame counter is what lets input scripts be calibrated against real
 * captures instead of guessed at -- wall-clock timing is useless for that
 * here, since the emulator does not run at a fixed speed and asset-loading
 * states burn frames at their own rate.  The music bits are the only way to
 * confirm from a headless run that the right track is playing at the right
 * time, there being no way to listen.  The three timing fields turn any
 * capture into a profile: 128us units keep a whole 12-bit field under half a
 * second, far past the slowest frame this port has ever produced, while
 * still resolving well inside one 16.7 ms vblank.  Indices 3/255 are the game
 * palette's white/black (src/generated/waifu_assets.h).
 *
 * The pacing field is what makes a slow-motion complaint diagnosable from a
 * capture: it is the number the loop actually charged the core for, so a
 * 60 ms frame stamped with 1 period means the clock lied, while the same
 * frame stamped with 4 means the pacing is right and the frame rate is the
 * only problem left. */
#define FMTOWNS_STAMP_BITS 82

/* Microseconds -> the 12-bit, 128us-per-unit field the stamp carries.
 * Saturating rather than truncating: a frame slower than 524 ms is a
 * pathology worth seeing as "pinned at the maximum", not one worth
 * disguising as a fast frame by dropping its high bits. */
static unsigned int fmtowns_prof_field(uint32_t us)
{
    uint32_t units = us >> 7;
    return (units > 0xfffu) ? 0xfffu : (unsigned int)units;
}

static void fmtowns_stamp_debug_state(uint8_t *frame_buffer, unsigned int frame,
                                      unsigned int steps)
{
    unsigned int lo = frame & 0xffffu;
    unsigned int hi;   /* pixels 32..63 */
    unsigned int top;  /* pixels 64..81 */
    int i;

    lo |= ((unsigned int)fmtowns_audio_started_track() & 0x1fu) << 16;
    if (fmtowns_audio_last_state() == 1 /* FMT_CDDA_PLAYING */) {
        lo |= 1u << 21;
    }
    lo |= fmtowns_prof_field(g_prof_avg_total) << 22;

    hi  = fmtowns_prof_field(g_prof_avg_total)   >> 10;
    hi |= fmtowns_prof_field(g_prof_avg_step)    << 2;
    hi |= fmtowns_prof_field(g_prof_avg_present) << 14;
    hi |= (steps > 15u ? 15u : steps)            << 26;
    hi |= fmtowns_prof_field(g_prof_worst)       << 30;
    top = fmtowns_prof_field(g_prof_worst)       >> 2;
    {   /* Fade is 0..256; 256 and 255 both stamp as 255, which is what the
         * reader wants -- "fully lit" is the only thing that distinction
         * would mean and neither value is a fade. */
        int fade = waifu_fm_video_fade_q8();
        if (fade > 255) fade = 255;
        if (fade < 0) fade = 0;
        top |= (unsigned int)fade << 10;
    }

    for (i = 0; i < 32; ++i) {
        frame_buffer[i] = (lo & (1u << i)) ? 3 : 255;
    }
    for (i = 32; i < 64; ++i) {
        frame_buffer[i] = (hi & (1u << (i - 32))) ? 3 : 255;
    }
    for (i = 64; i < FMTOWNS_STAMP_BITS; ++i) {
        frame_buffer[i] = (top & (1u << (i - 64))) ? 3 : 255;
    }
}
#endif /* FMTOWNS_DEBUG_INPUT */

/* Milestone 7: the real portable game core.  Music is CD-DA (see
 * fmtowns_audio.h); this build's disc only carries one clip staged as
 * track 2 (Makefile.fmtowns's MUSIC_CLIP), so unlike CD32X/PC-FX this does
 * not yet switch tracks on waifu_fm_audio_music_track() changes -- it just
 * starts that one track the first time the core asks for *any* music and
 * leaves it looping, matching milestone 5's proven CD-DA start-up. Getting
 * per-track switching means building the full CDDA_TRACK_* disc
 * (STATUS.md's "Next steps" #3), not a change to this loop. */


static void waifu_fm_game_loop(void)
{
    /* GAME frames, not rendered ones: it advances by the paced step count, so
     * it counts the 60 Hz ticks the core has been given.  Both the debug input
     * script and the stamp are indexed by it, which is what makes a script
     * replay identically at any frame rate -- and identically to the same
     * script under ./waifu_fm_headless, which is where scripts get written and
     * verified.  Indexing by rendered frames instead (what this used to do)
     * silently retimes every script the moment the frame rate moves: the
     * capture that walked a duel at 60 fps parks in one phase at 30, because
     * every tap lands twice as late in game time. */
    unsigned int frame = 0;
    /* 60 Hz frames of game time the *previous* iteration consumed; the first
     * one has nothing to measure yet, so it is worth exactly one. */
    unsigned int steps = 1;

    fmtowns_clock_init();       /* before anything times anything */
    waifu_fm_init();
    waifu_fm_reset_interactive();
    fmtowns_video_init();
    fmtowns_audio_init();
    fmtowns_sfx_init();

    for (;;) {
        WaifuFmInput in;
#ifdef FMTOWNS_DEBUG_INPUT
        uint32_t t_step, t_present, t_other, t_vblank_wait = 0;
#endif

        fmtowns_read_input(&in);
#ifdef FMTOWNS_DEBUG_INPUT
        fmtowns_apply_input_script(&in, frame);
#endif
        /* Charge the core for the wall-clock time the last frame really took,
         * so phases and animations advance at the same speed whatever the
         * render rate is.  See fmtowns_frame_pace(). */
        waifu_fm_set_frame_vblanks((int)steps);
#ifdef FMTOWNS_DEBUG_INPUT
        (void)fmtowns_prof_split();     /* close out the previous frame's tail */
#endif
        waifu_fm_step(&in);
#ifdef FMTOWNS_DEBUG_INPUT
        t_step = fmtowns_prof_split();

        fmtowns_stamp_debug_state(waifu_fm_framebuffer(), frame, steps);
#endif
        /* No extra vblank wait here: the present path's page flip already
         * blocks on a vsync edge, and waiting a second time would leave the
         * game running at half the display rate no matter how fast it
         * renders.  Holding the loop to 60 Hz is fmtowns_frame_pace()'s job
         * at the bottom, not this call's -- see that function. */
        {
            /* What the game says it drew.  A full frame reports "everything"
             * and this is the blit that always ran; a settled screen reports
             * a handful of scanlines and the present neither reads nor
             * uploads the rest.  See waifu_fm_frame_damage(). */
            const unsigned char *rows = 0;
            int full = waifu_fm_frame_damage(&rows);
#ifdef FMTOWNS_DEBUG_INPUT
            /* The debug stamp is written above, after the game's own damage
             * accounting closed, so declare it here: pixels 0..81 of row 0,
             * i.e. the first two 64-pixel groups. */
            static unsigned char stamped_rows[240];
            if (!full && rows) {
                unsigned int y;
                for (y = 0; y < 240u; ++y) stamped_rows[y] = rows[y];
                stamped_rows[0] |= 0x03u;
                rows = stamped_rows;
            }
#endif
            fmtowns_video_present_8bpp_rows(waifu_fm_framebuffer(),
                                            waifu_fm_palette_rgb(), 256,
                                            waifu_fm_video_fade_q8(),
                                            full ? 0 : rows,
                                            full ? 0 : waifu_fm_frame_damage_forced());
            waifu_fm_frame_damage_clear();
        }
#ifdef FMTOWNS_DEBUG_INPUT
        t_present = fmtowns_prof_split();
        /* The vertical-blank wait inside the present is not present *work*:
         * it is whatever is left of the field after the frame finished, so
         * leaving it in makes a faster blit look like a slower one and two
         * builds impossible to compare.  Charge it to `other` (which the
         * stamp folds into the whole-frame total) and let `present` mean the
         * VRAM blit plus the palette upload plus the flip. */
        {
            uint32_t wait_us = fmtowns_video_take_vblank_wait_us();
            if (wait_us > t_present) wait_us = t_present;
            t_present -= wait_us;
            t_vblank_wait = wait_us;
        }
#endif

        /* Music and SFX after the present, so neither sits between the
         * frame being finished and it reaching the screen.  Both are
         * reconciliation steps: they compare what the core wants with what
         * the hardware is doing and cost almost nothing when those agree. */
        fmtowns_audio_update(waifu_fm_audio_music_track());
        fmtowns_sfx_update();

        /* Last thing in the frame, so everything above counts towards the
         * 60 Hz budget rather than being spent on top of it. */
        steps = fmtowns_frame_pace();
        frame += steps;
#ifdef FMTOWNS_DEBUG_INPUT
        t_other = fmtowns_prof_split();
        fmtowns_prof_frame_end(t_step, t_present, t_other + t_vblank_wait);
#endif
    }
}
#endif

void start_main(void)
{
    /* Must run before anything below touches VRAM (fmt_set_mode() included,
     * inside fmt_media_init()'s callers and every present path) -- it picks
     * the physical VRAM base every later VRAM store uses. See machine.h. */
    fmt_machine_detect();

    (void)fmt_media_init();

#if FMTOWNS_MILESTONE >= 7
    /* Before the core, because the core's answer to a dead disc is the
     * bare "LOADING... 0%" screen that made the Marty freeze impossible to
     * diagnose from a photograph.  This walks the same CD path one labelled
     * step at a time and leaves the result on screen -- including the case
     * where a step never returns.  See fmtowns_cd_diag.h. */
    (void)fmtowns_cd_diag_selftest();

    waifu_fm_game_loop();
#elif FMTOWNS_MILESTONE >= 6
    fmtowns_cube_demo_loop();
#elif FMTOWNS_MILESTONE >= 4
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
