#ifndef WAIFU_SDL3_VIDEO_H
#define WAIFU_SDL3_VIDEO_H

/* Opaque SDL3 GPU presenter: renders the captured scene (sdl3_scene3d.c) as
 * real depth-buffered polygons plus 32bpp sprite/quad batches for the 2D
 * layer — no 8bpp surface is ever uploaded or presented. The frame is drawn
 * into a fixed high-resolution offscreen target and blitted, aspect-correct,
 * to the swapchain. */

#include <stdint.h>

#include "sdl3_settings.h"

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuSdl3Video WaifuSdl3Video;

/* Creates the window (game resolution x scale, resizable) and GPU device.
 * Returns NULL on failure (SDL_GetError() has the reason). */
WaifuSdl3Video *waifu_sdl3_video_create(const char *title,
                                        const WaifuSettings *settings);

/* Uploads this frame's captured geometry, draws backdrop / 3D scene / UI,
 * presents, and resets the capture. fade_q8 (0..256) scales all layers
 * (hardware-style palette-intensity fade). Returns 0 on GPU failure. */
int waifu_sdl3_video_present(WaifuSdl3Video *video, int fade_q8);

/* Renderer output resolution (the offscreen target the frame is drawn at). */
int waifu_sdl3_video_render_width(const WaifuSdl3Video *video);
int waifu_sdl3_video_render_height(const WaifuSdl3Video *video);

/* Downloads the last presented frame as tightly-packed RGBA8. `rgba` must
 * hold render_width * render_height * 4 bytes. Returns 0 on failure. */
int waifu_sdl3_video_read_frame(WaifuSdl3Video *video, uint8_t *rgba);

/* Re-applies the settings the video layer owns: window mode/size, display,
   vsync and the canvas sizing policy (render scale + aspect mode). Safe to
   call every time the options menu changes something. */
void waifu_sdl3_video_apply_settings(WaifuSdl3Video *video);

/* Cycles windowed -> borderless fullscreen -> windowed (the F11 hotkey) and
   writes the new mode back into the settings the video layer was given. */
void waifu_sdl3_video_toggle_fullscreen(WaifuSdl3Video *video, WaifuSettings *settings);

/* Records the current windowed size into `settings` (called on resize so the
   size the player left the window at is what gets saved). */
void waifu_sdl3_video_note_window_size(WaifuSdl3Video *video, WaifuSettings *settings);

/* --- display enumeration for the options menu --- */

/* Number of connected displays, and a human name for one. */
int waifu_sdl3_video_display_count(void);
const char *waifu_sdl3_video_display_name(int index);

/* Distinct fullscreen modes of a display, largest first. Returns the count;
   waifu_sdl3_video_mode() fills one entry (w/h/refresh may be NULL). */
int waifu_sdl3_video_mode_count(int display);
int waifu_sdl3_video_mode(int display, int index, int *w, int *h, float *hz);

/* The frontend's SDL window (icon, platform integration). */
SDL_Window *waifu_sdl3_video_window(const WaifuSdl3Video *video);

/* The window's current drawable size (what the options menu reports). */
void waifu_sdl3_video_window_size(const WaifuSdl3Video *video, int *w, int *h);

/* Maps a mouse position in window coordinates to the overlay's virtual units
   (sdl3_overlay.h), accounting for HiDPI, the letterbox and the stretch mode.
   Returns 0 when the point is outside the presented image. */
int waifu_sdl3_video_window_to_overlay(const WaifuSdl3Video *video,
                                       float mx, float my, float *ox, float *oy);

void waifu_sdl3_video_destroy(WaifuSdl3Video *video);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_VIDEO_H */
