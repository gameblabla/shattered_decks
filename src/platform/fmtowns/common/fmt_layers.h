#ifndef FMT_LAYERS_H
#define FMT_LAYERS_H

#include <stdint.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t stride;
} fmt_bg_layer_t;

/* Configure two independent RGB555 backgrounds in the two 256KB VRAM pages.
 * Width/height/stride are independent, subject to stride*height <= 256KB. */
int fmt_set_two_backgrounds(fmt_bg_layer_t bg0, fmt_bg_layer_t bg1,
                            uint8_t foreground_layer);

/* Sprite hardware requires layer 1 to be a 256x256 RGB555 render target with
 * a 512-byte stride. Layer 0 remains an independently sized background. */
int fmt_set_background_and_sprites(fmt_bg_layer_t background);

void fmt_bg_put_pixel(uint8_t layer, uint16_t x, uint16_t y, uint16_t color);
void fmt_bg_clear(uint8_t layer, uint16_t color);
const fmt_bg_layer_t *fmt_bg_layer(uint8_t layer);

#endif
