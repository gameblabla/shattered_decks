#ifndef WAIFU_PCFX_INPUT_H
#define WAIFU_PCFX_INPUT_H

#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuPcfxInput WaifuPcfxInput;

WaifuPcfxInput *waifu_pcfx_input_create(void);
void waifu_pcfx_input_destroy(WaifuPcfxInput *input);
void waifu_pcfx_input_poll(WaifuPcfxInput *input, WaifuFmInput *out);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_INPUT_H */
