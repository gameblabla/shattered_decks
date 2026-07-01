#ifndef WAIFU_CD32X_VIDEO_H
#define WAIFU_CD32X_VIDEO_H

#include <stdint.h>
#include "game_api.h"

#define WAIFU_CD32X_FRAMEBUFFER_LINE_TABLE_BYTES 0x200u
#define WAIFU_CD32X_FRAMEBUFFER_PIXELS (0x24000000u + WAIFU_CD32X_FRAMEBUFFER_LINE_TABLE_BYTES)
#define WAIFU_CD32X_FRAMEBUFFER_PAGE_BYTES 0x20000u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuCd32xVideo WaifuCd32xVideo;

WaifuCd32xVideo *waifu_cd32x_video_create(void);
void waifu_cd32x_video_destroy(WaifuCd32xVideo *video);
void waifu_cd32x_video_begin_8bpp(WaifuCd32xVideo *video);
void waifu_cd32x_video_set_palette_rgb(WaifuCd32xVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8);
void waifu_cd32x_video_clear_black(WaifuCd32xVideo *video);
void waifu_cd32x_video_clear_back_index(uint8_t c);
void waifu_cd32x_video_draw_debug_overlay(WaifuCd32xVideo *video);
void waifu_cd32x_video_present_8bpp(WaifuCd32xVideo *video, const uint8_t *framebuffer, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8);
void waifu_cd32x_video_wait_vblank(WaifuCd32xVideo *video);
volatile uint8_t *waifu_cd32x_video_title_upload_buffer(void);
void waifu_cd32x_video_commit_title_upload(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_VIDEO_H */
