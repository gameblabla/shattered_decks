#ifndef WAIFU_ZMBV_MKV_H
#define WAIFU_ZMBV_MKV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zmbv_mkv_recorder zmbv_mkv_recorder_t;

zmbv_mkv_recorder_t *zmbv_mkv_open(const char *path, int width, int height, int fps_num, int fps_den, char *err, size_t err_len);
int zmbv_mkv_add_indexed_frame(zmbv_mkv_recorder_t *r, const uint8_t *fb8, const uint8_t *palette_rgb, uint64_t frame_index);
int zmbv_mkv_close(zmbv_mkv_recorder_t *r);
void zmbv_mkv_abort(zmbv_mkv_recorder_t *r);
const char *zmbv_mkv_error(const zmbv_mkv_recorder_t *r);

#ifdef __cplusplus
}
#endif

#endif
