#ifndef WAIFU_CD32X_AUDIO_H
#define WAIFU_CD32X_AUDIO_H

#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuCd32xAudio WaifuCd32xAudio;

WaifuCd32xAudio *waifu_cd32x_audio_create(void);
void waifu_cd32x_audio_destroy(WaifuCd32xAudio *audio);
void waifu_cd32x_audio_set_music(WaifuCd32xAudio *audio, WaifuFmMusicTrack track);
void waifu_cd32x_audio_pump(WaifuCd32xAudio *audio);
void waifu_cd32x_audio_play_sfx(int sfx_id);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_CD32X_AUDIO_H */
