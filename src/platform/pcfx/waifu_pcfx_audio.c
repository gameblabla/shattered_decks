#include "waifu_pcfx_audio.h"

struct WaifuPcfxAudio {
    WaifuFmMusicTrack current_music;
};

static WaifuPcfxAudio g_audio;

WaifuPcfxAudio *waifu_pcfx_audio_create(void)
{
    g_audio.current_music = WAIFU_FM_MUSIC_NONE;
    return &g_audio;
}

void waifu_pcfx_audio_destroy(WaifuPcfxAudio *audio)
{
    (void)audio;
}

void waifu_pcfx_audio_set_music(WaifuPcfxAudio *audio, WaifuFmMusicTrack track)
{
    if (!audio) return;
    audio->current_music = track;
    /* PC-FX CDDA/music hook intentionally stubbed for the first port. */
}

void waifu_pcfx_audio_pump(WaifuPcfxAudio *audio)
{
    (void)audio;
    /* Intentionally empty: later port pass can route SFX to PSG/ADPCM and music to CDDA. */
}

void waifu_pcfx_audio_stop_all(WaifuPcfxAudio *audio)
{
    if (audio) audio->current_music = WAIFU_FM_MUSIC_NONE;
}
