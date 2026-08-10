#ifndef FMT_SPRITE_H
#define FMT_SPRITE_H
#include <stdint.h>

#define FMT_SPRITE_ROT_0 0
#define FMT_SPRITE_FLIP_Y 1
#define FMT_SPRITE_FLIP_X 2
#define FMT_SPRITE_ROT_180 3
#define FMT_SPRITE_ROT_270 5
#define FMT_SPRITE_ROT_90 6

void fmt_sprite_init(void);
void fmt_sprite_load_15bpp(uint16_t first_pattern,const void *tiles,uint16_t count);
void fmt_sprite_set(uint16_t index,uint16_t x,uint16_t y,uint16_t pattern,
                    uint8_t transform,uint8_t shrink_x,uint8_t shrink_y);
void fmt_sprite_hide(uint16_t index);
void fmt_sprite_enable(uint16_t first_index);
void fmt_sprite_disable(void);
int fmt_sprite_busy(void);
void fmt_sprite_freeze(void);
#endif
