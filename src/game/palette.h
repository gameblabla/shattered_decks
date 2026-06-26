#ifndef WAIFU_FM_PALETTE_H
#define WAIFU_FM_PALETTE_H

#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void waifu_fm_use_palette(WaifuFmPaletteId id);
void waifu_fm_use_common_palette(void);
void waifu_fm_use_title_palette(void);
void waifu_fm_use_ending_palette(void);
void waifu_fm_use_ending_black_palette(void);
void waifu_fm_use_dialogue_palette(void);
WaifuFmPaletteId waifu_fm_palette_id(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_PALETTE_H */
