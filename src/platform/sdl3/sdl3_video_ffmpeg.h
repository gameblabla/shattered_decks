#ifndef WAIFU_SDL3_VIDEO_FFMPEG_H
#define WAIFU_SDL3_VIDEO_FFMPEG_H

#include <stdint.h>

/* Looping title-screen video (PC only).
 *
 * The desktop title is an animated 16:9 clip rather than a still photograph.
 * This is the decoder behind it: one FFmpeg (libav*) pipeline that decodes
 * assets/source/videos/animated_titlescreen.mp4 in real time, restarting it
 * from the top when it runs out, and hands the renderer a tightly packed RGBA8
 * frame whenever a new one is due.
 *
 * Hardware decoding is used when the machine has it: an NVDEC/CUDA device is
 * tried first and the decoder falls back to software silently if that device,
 * or a CUDA-capable decoder for the stream, is not there. The whole module is
 * compiled out when the build has no FFmpeg (WAIFU_SDL3_FFMPEG undefined), in
 * which case the title keeps using the still image.
 */

/* Open `path` (NULL = the built-in title clip). Returns 1 on success. Safe to
 * call repeatedly; a second call on an open decoder is a no-op returning 1. */
int waifu_sdl3_titlevid_open(const char *path);

/* Non-blocking: advance the clip to wall-clock time and, if a new frame became
 * current since the last call, point *rgba at it (owned by the decoder, valid
 * until the next poll) with its size, and return 1. Returns 0 when the current
 * frame still stands, and -1 when there is no usable video at all. */
int waifu_sdl3_titlevid_poll(const uint8_t **rgba, int *w, int *h);

/* Drop the whole pipeline (called at shutdown). */
void waifu_sdl3_titlevid_close(void);

#endif /* WAIFU_SDL3_VIDEO_FFMPEG_H */
