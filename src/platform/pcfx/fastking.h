#ifndef MY_ASM_FUNCS_H
#define MY_ASM_FUNCS_H

#include <stdint.h>
#include "pcfx.h"
void king_kram_write_buffer(void* addr, int size);
void king_kram_write_buffer_bytes(void* addr, int size);
void king_kram_write_buffer_bytes_at(void* addr, int size, int word_addr);
void king_kram_fill_words(uint16_t value, int words);
void king_kram_clear_rect_256_packed(uint16_t value, int page_word_offset, unsigned xy, unsigned wh);
void king_kram_clear_rect_256_zero_packed(int page_word_offset, unsigned xy, unsigned wh);
void king_kram_upload_rect_256_packed(const void *src, int page_word_offset, unsigned xy, unsigned wh);
void king_kram_upload_rect_256_bytes_packed(const void *src, int page_word_offset, unsigned xy, unsigned wh);
static inline void king_kram_clear_rect_256_zero(int page_word_offset, int x0, int y0, int width_words, int rows)
{
    king_kram_clear_rect_256_zero_packed(page_word_offset, (unsigned)((x0 & 0xffff) | ((y0 & 0xffff) << 16)), (unsigned)((width_words & 0xffff) | ((rows & 0xffff) << 16)));
}
static inline void king_kram_clear_rect_256(uint16_t value, int page_word_offset, int x0, int y0, int width_words, int rows)
{
    if (value == 0) {
        king_kram_clear_rect_256_zero(page_word_offset, x0, y0, width_words, rows);
    } else {
        king_kram_clear_rect_256_packed(value, page_word_offset, (unsigned)((x0 & 0xffff) | ((y0 & 0xffff) << 16)), (unsigned)((width_words & 0xffff) | ((rows & 0xffff) << 16)));
    }
}
static inline void king_kram_upload_rect_256(const void *src, int page_word_offset, int x0, int y0, int row_bytes, int rows)
{
    king_kram_upload_rect_256_packed(src, page_word_offset, (unsigned)((x0 & 0xffff) | ((y0 & 0xffff) << 16)), (unsigned)((row_bytes & 0xffff) | ((rows & 0xffff) << 16)));
}
static inline void king_kram_upload_rect_256_bytes(const void *src, int page_word_offset, int x0, int y0, int row_bytes, int rows)
{
    king_kram_upload_rect_256_bytes_packed(src, page_word_offset, (unsigned)((x0 & 0xffff) | ((y0 & 0xffff) << 16)), (unsigned)((row_bytes & 0xffff) | ((rows & 0xffff) << 16)));
}
void eris_king_set_bg0_affine_coefficient_a(int16_t x);
void eris_king_set_bg0_affine_coefficient_b(int16_t x);
void eris_king_set_bg0_affine_coefficient_c(int16_t x);
void eris_king_set_bg0_affine_coefficient_d(int16_t x);

void eris_king_set_bg_affine_center_x(int16_t x) ;
void eris_king_set_bg_affine_center_y(int16_t x) ;

#endif
