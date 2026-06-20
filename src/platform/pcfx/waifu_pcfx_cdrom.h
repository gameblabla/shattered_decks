#ifndef WAIFU_PCFX_CDROM_H
#define WAIFU_PCFX_CDROM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuPcfxCdrom WaifuPcfxCdrom;

typedef enum WaifuPcfxCdAssetId {
    WAIFU_PCFX_CD_ASSET_NONE = 0,
    WAIFU_PCFX_CD_ASSET_TITLE,
    WAIFU_PCFX_CD_ASSET_CARD_BANK,
    WAIFU_PCFX_CD_ASSET_STORY_PORTRAITS
} WaifuPcfxCdAssetId;

WaifuPcfxCdrom *waifu_pcfx_cdrom_create(void);
void waifu_pcfx_cdrom_destroy(WaifuPcfxCdrom *cdrom);
int waifu_pcfx_cdrom_read_asset(WaifuPcfxCdrom *cdrom, WaifuPcfxCdAssetId asset, void *dst, size_t dst_size);
void waifu_pcfx_cdrom_play_cdda_stub(WaifuPcfxCdrom *cdrom, int track, int loop);
void waifu_pcfx_cdrom_stop_cdda_stub(WaifuPcfxCdrom *cdrom);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_CDROM_H */
