#ifndef WAIFU_PCFX_AUDIO_H
#define WAIFU_PCFX_AUDIO_H

#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuPcfxAudio WaifuPcfxAudio;

WaifuPcfxAudio *waifu_pcfx_audio_create(void);
void waifu_pcfx_audio_destroy(WaifuPcfxAudio *audio);
void waifu_pcfx_audio_set_music(WaifuPcfxAudio *audio, WaifuFmMusicTrack track);
void waifu_pcfx_audio_pump(WaifuPcfxAudio *audio);
void waifu_pcfx_audio_stop_all(WaifuPcfxAudio *audio);
int waifu_pcfx_adpcm_samples_load(void);
void waifu_pcfx_adpcm_sample_play(int effect);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_AUDIO_H */
