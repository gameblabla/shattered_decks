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

typedef enum WaifuPcfxRainbowBgAsset {
    WAIFU_PCFX_RAINBOW_BG_DESERT = 0,
    WAIFU_PCFX_RAINBOW_BG_STONE = 1,
    WAIFU_PCFX_RAINBOW_BG_EMBER = 2,
    WAIFU_PCFX_RAINBOW_BG_SKY = 3
} WaifuPcfxRainbowBgAsset;

WaifuPcfxCdrom *waifu_pcfx_cdrom_create(void);
void waifu_pcfx_cdrom_destroy(WaifuPcfxCdrom *cdrom);
int waifu_pcfx_cdrom_read_asset(WaifuPcfxCdrom *cdrom, WaifuPcfxCdAssetId asset, void *dst, size_t dst_size);

/* CD-DA lives in libpcfx (<eris/cdda.h>): the command layer there is the one
   that works on a real console, and its music manager owns the "a data read
   stops the drive's audio engine" recovery.  Every reader in this module calls
   eris_cdda_notify_cd_read(), so the manager sees the loads; the audio layer
   calls eris_cdda_music_* and pumps it once per frame. */

/* Blocking CD/SCSI DMA directly into KING KRAM.  kram_addr is the KING
 * KRAM word address, matching liberis eris_cd_read_kram(). */
int waifu_pcfx_cdrom_read_title_yuv422_to_kram(uint32_t kram_addr, size_t bytes);
int waifu_pcfx_cdrom_read_ending_yuv422_to_kram(uint32_t kram_addr, size_t bytes);
int waifu_pcfx_cdrom_read_sfx_adpcm_to_kram(uint32_t kram_addr, size_t bytes);
int waifu_pcfx_cdrom_read_rainbow_bg_to_kram(WaifuPcfxRainbowBgAsset asset, uint32_t kram_addr, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_CDROM_H */
