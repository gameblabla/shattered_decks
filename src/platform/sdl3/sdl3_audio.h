#ifndef WAIFU_SDL3_AUDIO_H
#define WAIFU_SDL3_AUDIO_H

/* Opaque SDL3 audio output for the game core's pull-model mixer
 * (waifu_fm_audio_mix_s16). The device stream pulls directly from the game
 * mixer on the audio thread; waifu_sdl3_audio_lock()/unlock() must bracket
 * every waifu_fm_step() on the main thread so the mixer state is never
 * stepped mid-mix. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuSdl3Audio WaifuSdl3Audio;

/* Opens the default playback device at the game mixer's native rate/channel
 * layout and starts pulling. Returns NULL on failure (the game runs silent;
 * callers treat audio as optional, same as the SDL 1.2 frontend). */
WaifuSdl3Audio *waifu_sdl3_audio_create(void);

void waifu_sdl3_audio_lock(WaifuSdl3Audio *audio);
void waifu_sdl3_audio_unlock(WaifuSdl3Audio *audio);

void waifu_sdl3_audio_destroy(WaifuSdl3Audio *audio);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_AUDIO_H */
