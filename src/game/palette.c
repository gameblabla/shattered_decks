#include "palette.h"

#if defined(WAIFU_ASSET_USE_CDROM)
#define WAIFU_ASSET_EXTERNAL_TITLE_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS 1
#endif

#include "waifu_assets.h"
#include "title_asset.h"

static WaifuFmPaletteId g_active_palette_id = WAIFU_FM_PALETTE_COMMON;

void waifu_fm_use_palette(WaifuFmPaletteId id)
{
    if (id != WAIFU_FM_PALETTE_TITLE) id = WAIFU_FM_PALETTE_COMMON;
    g_active_palette_id = id;
}

void waifu_fm_use_common_palette(void)
{
    waifu_fm_use_palette(WAIFU_FM_PALETTE_COMMON);
}

void waifu_fm_use_title_palette(void)
{
    waifu_fm_use_palette(WAIFU_FM_PALETTE_TITLE);
}

WaifuFmPaletteId waifu_fm_palette_id(void)
{
    return g_active_palette_id;
}

const uint8_t *waifu_fm_palette_rgb_for_id(WaifuFmPaletteId id)
{
    return (id == WAIFU_FM_PALETTE_TITLE) ? title_screen_palette_rgb : waifu_palette_rgb;
}

const uint8_t *waifu_fm_palette_rgb(void)
{
    return waifu_fm_palette_rgb_for_id(g_active_palette_id);
}
