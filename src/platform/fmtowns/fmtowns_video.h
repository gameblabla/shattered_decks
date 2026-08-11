#ifndef FMTOWNS_VIDEO_H
#define FMTOWNS_VIDEO_H

/* Sets the CRTC to the game's native 256x240 8bpp mode. Call once at boot. */
void fmtowns_video_init(void);

/* Uploads `palette_count` RGB888 triplets *if they changed since the last
 * present*, blits a 256x240 8bpp linear framebuffer, then flips the display
 * page (see libfmt.h's fmt_flip_page() for why this is a real hardware page
 * flip rather than a plain copy).
 *
 * This already blocks until a vertical blank -- see
 * fmtowns_video_present_waits_vblank(); callers must not wait again. */
void fmtowns_video_present_8bpp(const unsigned char *framebuffer,
                                 const unsigned char *palette_rgb888,
                                 int palette_count);

/* Non-zero if fmtowns_video_present_8bpp() already synchronises to vblank,
 * so a caller's own frame pacing must not add a second wait (that would cap
 * the game at half the display rate). */
int fmtowns_video_present_waits_vblank(void);

/* Blocks until the next vertical blank. */
void fmtowns_video_wait_vblank(void);

#endif /* FMTOWNS_VIDEO_H */
