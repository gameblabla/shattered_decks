/* SDL3 audio output: an SDL_AudioStream bound to the default playback
 * device, filled on demand from the game core's pull-model mixer. */

#include "sdl3_audio.h"

#include <stdlib.h>

#include <SDL3/SDL.h>

#include "game_api.h"

struct WaifuSdl3Audio {
    SDL_AudioStream *stream;
    int channels;
};

#define SDL3_AUDIO_CHUNK_FRAMES 1024

static void SDLCALL sdl3_audio_feed(void *userdata, SDL_AudioStream *stream,
                                    int additional_amount, int total_amount)
{
    WaifuSdl3Audio *audio = (WaifuSdl3Audio *)userdata;
    int16_t chunk[SDL3_AUDIO_CHUNK_FRAMES * 2];
    const int bytes_per_frame = audio->channels * (int)sizeof(int16_t);
    (void)total_amount;

    while (additional_amount > 0) {
        int frames = additional_amount / bytes_per_frame;
        if (frames > SDL3_AUDIO_CHUNK_FRAMES) frames = SDL3_AUDIO_CHUNK_FRAMES;
        if (frames <= 0) break;
        waifu_fm_audio_mix_s16(chunk, frames);
        SDL_PutAudioStreamData(stream, chunk, frames * bytes_per_frame);
        additional_amount -= frames * bytes_per_frame;
    }
}

WaifuSdl3Audio *waifu_sdl3_audio_create(void)
{
    WaifuSdl3Audio *audio;
    SDL_AudioSpec spec;

    if (waifu_fm_audio_channels() < 1 || waifu_fm_audio_channels() > 2) return NULL;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("SDL audio init failed: %s", SDL_GetError());
        return NULL;
    }

    audio = (WaifuSdl3Audio *)calloc(1, sizeof(*audio));
    if (!audio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    audio->channels = waifu_fm_audio_channels();

    spec.format = SDL_AUDIO_S16;
    spec.channels = audio->channels;
    spec.freq = waifu_fm_audio_sample_rate();

    audio->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &spec, sdl3_audio_feed, audio);
    if (!audio->stream) {
        SDL_Log("SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        free(audio);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }

    SDL_ResumeAudioStreamDevice(audio->stream);
    return audio;
}

void waifu_sdl3_audio_lock(WaifuSdl3Audio *audio)
{
    if (audio) SDL_LockAudioStream(audio->stream);
}

void waifu_sdl3_audio_unlock(WaifuSdl3Audio *audio)
{
    if (audio) SDL_UnlockAudioStream(audio->stream);
}

void waifu_sdl3_audio_destroy(WaifuSdl3Audio *audio)
{
    if (!audio) return;
    SDL_DestroyAudioStream(audio->stream);
    free(audio);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
