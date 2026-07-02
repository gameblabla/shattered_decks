#ifndef WAIFU_CD32X_INPUT_H
#define WAIFU_CD32X_INPUT_H

#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuCd32xInput WaifuCd32xInput;

WaifuCd32xInput *waifu_cd32x_input_create(void);
void waifu_cd32x_input_destroy(WaifuCd32xInput *input);
void waifu_cd32x_input_poll(WaifuCd32xInput *input, WaifuFmInput *out);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_INPUT_H */
