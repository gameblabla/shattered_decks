#ifndef WAIFU_SDL3_VIDEO_H
#define WAIFU_SDL3_VIDEO_H

/* Opaque SDL3 GPU presenter: renders the captured scene (sdl3_scene3d.c) as
 * real depth-buffered polygons plus 32bpp sprite/quad batches for the 2D
 * layer — no 8bpp surface is ever uploaded or presented. The frame is drawn
 * into a fixed high-resolution offscreen target and blitted, aspect-correct,
 * to the swapchain. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuSdl3Video WaifuSdl3Video;

/* Creates the window (game resolution x scale, resizable) and GPU device.
 * Returns NULL on failure (SDL_GetError() has the reason). */
WaifuSdl3Video *waifu_sdl3_video_create(const char *title, int scale,
                                        int fullscreen, int vsync);

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

void waifu_sdl3_video_toggle_fullscreen(WaifuSdl3Video *video);

void waifu_sdl3_video_destroy(WaifuSdl3Video *video);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_VIDEO_H */
