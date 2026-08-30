#ifndef WAIFU_SDL3_OVERLAY_H
#define WAIFU_SDL3_OVERLAY_H

/* Frontend overlay layer (pause menu, options, on-screen notices).
 *
 * The game's own 2D output goes through the capture seams into a PERSISTENT
 * canvas, so anything drawn there sticks until the game redraws it — no good
 * for a translucent menu on top of a frozen frame.  The overlay is therefore a
 * separate immediate layer: it is rebuilt from scratch every frame and drawn by
 * the video layer straight onto the swapchain, after the canvas blit.  The game
 * can be fully stopped underneath it and the picture still holds.
 *
 * Coordinates are "overlay units": a virtual space WAIFU_OVERLAY_H units tall
 * whose width follows the real display aspect, so one layout works from 720p to
 * 5K and from 4:3 to 32:9.  Text sizes are cell heights in the same units. */

#include "sdl3_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_OVERLAY_H 360.0f
#define SDL3_OVERLAY_MAX_VERTS  16384
#define SDL3_OVERLAY_MAX_GLYPHS 24576

typedef struct Sdl3Overlay {
    int active;                  /* anything to draw this frame */
    float screen_w, screen_h;    /* overlay units across the display */
    Sdl3UiVertex verts[SDL3_OVERLAY_MAX_VERTS];
    int vert_count;
    Sdl3UiVertex glyphs[SDL3_OVERLAY_MAX_GLYPHS];
    int glyph_count;
} Sdl3Overlay;

Sdl3Overlay *waifu_sdl3_overlay(void);

/* Starts a frame's overlay: clears the geometry and sizes the coordinate space
   for a display of `px_w` x `px_h` pixels (height is always WAIFU_OVERLAY_H). */
void waifu_sdl3_overlay_begin(int px_w, int px_h);

/* Drops everything (the menu closed): the video layer then draws no overlay. */
void waifu_sdl3_overlay_clear(void);

float waifu_sdl3_overlay_width(void);   /* overlay units across the screen */

void waifu_sdl3_overlay_rect(float x, float y, float w, float h,
                             float r, float g, float b, float a);
/* 1-unit-thick outline, drawn as four rects. */
void waifu_sdl3_overlay_frame(float x, float y, float w, float h, float t,
                              float r, float g, float b, float a);
/* Vertical gradient panel (two-colour, alpha interpolated too). */
void waifu_sdl3_overlay_vgrad(float x, float y, float w, float h,
                              const float top_rgba[4], const float bot_rgba[4]);

/* Draws `text` with the FreeType glyph atlas; `size` is the cell height in
   overlay units. Returns the advance width. A NULL text just measures. */
float waifu_sdl3_overlay_text(float x, float y, float size, const char *text,
                              float r, float g, float b, float a);
float waifu_sdl3_overlay_text_width(float size, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_OVERLAY_H */
