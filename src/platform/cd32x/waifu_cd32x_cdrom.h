#ifndef WAIFU_CD32X_CDROM_H
#define WAIFU_CD32X_CDROM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuCd32xCdrom WaifuCd32xCdrom;

WaifuCd32xCdrom *waifu_cd32x_cdrom_create(void);
void waifu_cd32x_cdrom_destroy(WaifuCd32xCdrom *cdrom);
int waifu_cd32x_cdrom_read_file(const char *path, void *dst, size_t dst_size, size_t *bytes_read);
int waifu_cd32x_cdrom_async_read_card_big(int card_id, void *dst);
int waifu_cd32x_cdrom_async_poll(void);
int waifu_cd32x_cdda_play(uint8_t track, uint8_t loop);
int waifu_cd32x_cdda_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_CDROM_H */
