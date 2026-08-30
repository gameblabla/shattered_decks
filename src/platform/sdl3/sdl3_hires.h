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
    WAIFU_HIRES_BIG  = 1,   /* square framing (detail big art) */
    WAIFU_HIRES_PORTRAIT = 2, /* story dialogue portrait (id = portrait id) */
    WAIFU_HIRES_BACK = 3,    /* the shared card back (id unused) */
    WAIFU_HIRES_BOARD_TILE = 4 /* a 3D board checker square (id = 0 light, 1 dark) */
};

/* Scale of the decoded portrait against the game's 124x200 portrait box. The
   decoded buffer is exactly WAIFU_STORY_PORTRAIT_W/H * this, so it drops onto
   the same screen rect the 8bpp portrait uses. */
#define WAIFU_SDL3_PORTRAIT_SCALE 4

/* Number of cards the path table knows about (== the game's card count). */
int waifu_sdl3_hires_card_count(void);

/* True if a source image is known for this card id (path table resolved it). */
int waifu_sdl3_hires_has_card(int card_id);

/* Decode + cover-crop card_id's face/big art into a NEW malloc'd RGBA8 buffer
 * (row-major, tight). Caller frees. Returns NULL if the card has no source or
 * decoding failed. w and h set to the cropped dimensions. */
uint8_t *waifu_sdl3_hires_card_decode(int card_id, int kind, int *w, int *h);

/* Decode a story portrait's high-resolution source, framed exactly like the
 * 8bpp portrait the console builds bake (alpha-trimmed, upper-body crop,
 * bottom-anchored and centred in the portrait box) but at
 * WAIFU_SDL3_PORTRAIT_SCALE times the size. Caller frees. NULL if unavailable. */
uint8_t *waifu_sdl3_hires_portrait_decode(int portrait_id, int *w, int *h);

/* Decode the shared card-back texture into a NEW malloc'd RGBA8 buffer (caller
 * frees). It maps over the WHOLE 38x54 card rect -- the art carries its own
 * gold frame -- so it is stretched, not cover-cropped: trimming to the card's
 * aspect would clip the outer rule off the border. NULL if unavailable. */
uint8_t *waifu_sdl3_hires_card_back_decode(int *w, int *h);

/* Decode one of the two board checker textures (0 = light, 1 = dark) into a NEW
 * malloc'd RGBA8 buffer at its original resolution -- the PC build maps the whole
 * source across one board cell instead of the 32x32 atlas downscale the console
 * targets bake. Caller frees. NULL if unavailable. */
uint8_t *waifu_sdl3_hires_board_tile_decode(int index, int *w, int *h);

/* Decode the 16:9 title / ending source into a NEW malloc'd RGBA8 buffer
 * (caller frees). NULL if unavailable. */
uint8_t *waifu_sdl3_hires_title_decode(int *w, int *h);
uint8_t *waifu_sdl3_hires_ending_decode(int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_HIRES_H */
