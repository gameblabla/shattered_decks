#ifndef WAIFU_FM_SOUNDS_H
#define WAIFU_FM_SOUNDS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_SOUND_SAMPLE_RATE 44100
#define WAIFU_SOUND_CHANNELS 2
#define WAIFU_SOUND_SAMPLES_PER_FRAME (WAIFU_SOUND_SAMPLE_RATE / 60)

typedef enum WaifuMusicTrack {
    WAIFU_MUSIC_NONE = 0,
    WAIFU_MUSIC_TITLE,
    WAIFU_MUSIC_OPENING_DREAM,
    WAIFU_MUSIC_DECK_EDITOR,
    WAIFU_MUSIC_BOSS,
    WAIFU_MUSIC_FINAL_BOSS,
    WAIFU_MUSIC_RANDOM_BATTLE,
    WAIFU_MUSIC_RESULTS,
    WAIFU_MUSIC_LOST,
    WAIFU_MUSIC_TRACK_COUNT
} WaifuMusicTrack;

typedef enum WaifuSoundEffect {
    WAIFU_SOUND_SELECT = 0,
    WAIFU_SOUND_CONFIRM,
    WAIFU_SOUND_CONFIRM_ALT,
    WAIFU_SOUND_CARD_PLACED,
    WAIFU_SOUND_CARD_DESTROYED,
    WAIFU_SOUND_TURN_PASSED,
    WAIFU_SOUND_YOU_LOST,
    WAIFU_SOUND_LASER_SHOOT,
    WAIFU_SOUND_DIRECT_HIT,
    WAIFU_SOUND_CARD_DRAWN,
    WAIFU_SOUND_EFFECT_COUNT
} WaifuSoundEffect;

typedef struct WaifuSoundWavWriter {
    FILE *fp;
    uint32_t data_bytes;
    int sample_rate;
    int channels;
} WaifuSoundWavWriter;

void waifu_sound_init(void);
void waifu_sound_reset(void);
void waifu_sound_play(WaifuSoundEffect effect);
void waifu_sound_set_music(WaifuMusicTrack track);
WaifuMusicTrack waifu_sound_music_track(void);
const char *waifu_sound_music_name(WaifuMusicTrack track);
void waifu_sound_mix_s16(int16_t *dst, int frames);
int waifu_sound_sample_rate(void);
int waifu_sound_channels(void);
int waifu_sound_samples_per_frame(void);
const char *waifu_sound_effect_name(WaifuSoundEffect effect);

int waifu_sound_wav_open(WaifuSoundWavWriter *wr, const char *path);
int waifu_sound_wav_write(WaifuSoundWavWriter *wr, const int16_t *samples, int frames);
int waifu_sound_wav_close(WaifuSoundWavWriter *wr);
void waifu_sound_wav_abort(WaifuSoundWavWriter *wr);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_SOUNDS_H */
