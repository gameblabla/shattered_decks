#ifndef FMTOWNS_VIDEO_H
#define FMTOWNS_VIDEO_H

#include <stdint.h>

/* The machine's free-running PIT channel 1, defined in fmtowns_main.c.  See
 * the comment there: this is the only clock on this port with a usable
 * range -- the 1 us counter at I/O 0x26 wraps at 65.5 ms, which is shorter
 * than a frame here, and once invalidated a whole session of measurements. */
uint16_t fmtowns_clock_ticks(void);
uint32_t fmtowns_ticks_us(uint32_t ticks);

/* Microseconds the last present spent parked in fmt_wait_vsync(), and
 * reset.  The caller's profiler subtracts it so `present` means work done
 * rather than time waited -- see the call site in fmtowns_main.c. */
uint32_t fmtowns_video_take_vblank_wait_us(void);

/* Sets the CRTC to the game's native 256x240 8bpp mode. Call once at boot. */
void fmtowns_video_init(void);

/* Uploads `palette_count` RGB888 triplets *if they changed since the last
 * present*, blits a 256x240 8bpp linear framebuffer, then flips the display
 * page (see libfmt.h's fmt_flip_page() for why this is a real hardware page
 * flip rather than a plain copy).
 *
 * `fade_q8` is waifu_fm_video_fade_q8(): 256 for a normal frame, less while
 * the game is fading, 0 for black.  Every colour is scaled by it before the
 * comparison, so a fade is a palette upload rather than a walk over 61440
 * pixels -- which is what the game core's software dither would otherwise
 * cost this machine on every frame of every transition.
 *
 * This already blocks until a vertical blank -- see
 * fmtowns_video_present_waits_vblank(); callers must not wait again. */
void fmtowns_video_present_8bpp(const unsigned char *framebuffer,
                                 const unsigned char *palette_rgb888,
                                 int palette_count, int fade_q8);

/* Non-zero if fmtowns_video_present_8bpp() already synchronises to vblank,
 * so a caller's own frame pacing must not add a second wait (that would cap
 * the game at half the display rate). */
int fmtowns_video_present_waits_vblank(void);

/* Blocks until the next vertical blank. */
void fmtowns_video_wait_vblank(void);

#endif /* FMTOWNS_VIDEO_H */
