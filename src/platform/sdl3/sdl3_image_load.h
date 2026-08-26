#ifndef WAIFU_SDL3_IMAGE_LOAD_H
#define WAIFU_SDL3_IMAGE_LOAD_H

/* Minimal PC-only image loader for the SDL3 (desktop) frontend: decodes a PNG
 * or WebP file from disk into a tightly packed RGBA8 buffer. Used to load the
 * full-resolution card art and the 16:9 title/ending images straight from the
 * source assets, bypassing the 8bpp console asset pipeline. SDL3-only. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `path` (PNG or WebP, chosen by content) into a newly malloc'd RGBA8
 * buffer (row-major, 4 bytes/pixel, no padding). Returns the buffer and sets
 * w/h on success; the caller frees it with free(). Returns NULL on any error
 * (missing file, unsupported format, decode failure). */
uint8_t *waifu_sdl3_image_load_rgba(const char *path, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_IMAGE_LOAD_H */
