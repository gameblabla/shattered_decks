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
    WAIFU_PCFX_RAINBOW_BG_EMBER = 2
} WaifuPcfxRainbowBgAsset;

WaifuPcfxCdrom *waifu_pcfx_cdrom_create(void);
void waifu_pcfx_cdrom_destroy(WaifuPcfxCdrom *cdrom);
int waifu_pcfx_cdrom_read_asset(WaifuPcfxCdrom *cdrom, WaifuPcfxCdAssetId asset, void *dst, size_t dst_size);

/* CDDA playback — standalone (no cdrom context required). */
#define WAIFU_CDDA_SILENT 0x00
#define WAIFU_CDDA_NORMAL 0x01
#define WAIFU_CDDA_LOOP   0x04
void waifu_pcfx_cdda_play(uint8_t start_track, uint8_t end_track, uint8_t loop);
void waifu_pcfx_cdda_stop(void);
void waifu_pcfx_cdda_set_volume(uint8_t left, uint8_t right);

/* Monotonic counter bumped on every CD data read.  A SCSI/CD data read stops
   CD-DA playback on real hardware (and in the emulator), so the audio layer
   watches this to know when a load has interrupted music and must restart it. */
uint32_t waifu_pcfx_cd_read_seq(void);

/* Blocking CD/SCSI DMA directly into KING KRAM.  kram_addr is the KING
 * KRAM word address, matching liberis eris_cd_read_kram(). */
int waifu_pcfx_cdrom_read_title_yuv422_to_kram(uint32_t kram_addr, size_t bytes);
int waifu_pcfx_cdrom_read_sfx_adpcm_to_kram(uint32_t kram_addr, size_t bytes);
int waifu_pcfx_cdrom_read_rainbow_bg_to_kram(WaifuPcfxRainbowBgAsset asset, uint32_t kram_addr, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_CDROM_H */
