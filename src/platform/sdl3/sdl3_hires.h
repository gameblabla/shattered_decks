#ifndef WAIFU_SDL3_HIRES_H
#define WAIFU_SDL3_HIRES_H

/* Full-resolution card art + 16:9 title/ending loader for the SDL3 (desktop)
 * frontend. Decodes the source PNG/WebP straight from assets/source/ and
 * cover-crops the card art to the face-thumbnail / big-art framing; the crop is
 * uploaded to the GPU at native resolution (mipmapped) so downscaling stays
 * crisp. SDL3-only; the console builds keep the 8bpp asset pipeline. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WAIFU_HIRES_FACE = 0,   /* upper-torso thumbnail framing (card face) */
    WAIFU_HIRES_BIG  = 1    /* square framing (detail big art) */
};

/* Number of cards the path table knows about (== the game's card count). */
int waifu_sdl3_hires_card_count(void);

/* True if a source image is known for this card id (path table resolved it). */
int waifu_sdl3_hires_has_card(int card_id);

/* Decode + cover-crop card_id's face/big art into a NEW malloc'd RGBA8 buffer
 * (row-major, tight). Caller frees. Returns NULL if the card has no source or
 * decoding failed. w and h set to the cropped dimensions. */
uint8_t *waifu_sdl3_hires_card_decode(int card_id, int kind, int *w, int *h);

/* Decode the 16:9 title / ending source into a NEW malloc'd RGBA8 buffer
 * (caller frees). NULL if unavailable. */
uint8_t *waifu_sdl3_hires_title_decode(int *w, int *h);
uint8_t *waifu_sdl3_hires_ending_decode(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_HIRES_H */
