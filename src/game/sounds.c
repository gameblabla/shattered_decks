#include "sounds.h"

#include <string.h>

#include "sound_assets.h"

#define WAIFU_SOUND_MAX_VOICES 16
#define WAIFU_SOUND_MASTER_NUM 3
#define WAIFU_SOUND_MASTER_DEN 4
#define WAIFU_SOUND_SFX_GAIN_NUM 1
#define WAIFU_SOUND_SFX_GAIN_DEN 1
#define WAIFU_SOUND_MUSIC_GAIN_NUM 1
#define WAIFU_SOUND_MUSIC_GAIN_DEN 2

typedef struct WaifuSoundVoice {
    int active;
    WaifuSoundEffect effect;
    int pos;
} WaifuSoundVoice;

static WaifuSoundVoice g_voices[WAIFU_SOUND_MAX_VOICES];
static WaifuMusicTrack g_music_track = WAIFU_MUSIC_NONE;
static uint32_t g_music_pos = 0;

static int16_t clamp_s16(int v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int asset_count(void)
{
    return (int)(sizeof(waifu_sound_assets) / sizeof(waifu_sound_assets[0]));
}

static int16_t asset_sample_s16(WaifuSoundEffect effect, int pos)
{
    const WaifuSoundAsset *a;
    if (effect < 0 || effect >= WAIFU_SOUND_EFFECT_COUNT) return 0;
    if ((int)effect >= asset_count()) return 0;
    a = &waifu_sound_assets[(int)effect];
    if (pos < 0 || pos >= a->frame_count) return 0;
    return (int16_t)(((int)a->pcm_u8[pos] - 128) << 8);
}

static int asset_length(WaifuSoundEffect effect)
{
    if (effect < 0 || effect >= WAIFU_SOUND_EFFECT_COUNT) return 0;
    if ((int)effect >= asset_count()) return 0;
    return waifu_sound_assets[(int)effect].frame_count;
}

typedef struct WaifuMusicPattern {
    const uint16_t *notes;
    int note_count;
    int ticks_per_note;
    int amp;
    int duty;
} WaifuMusicPattern;

static const uint16_t music_title_notes[]        = {330, 392, 494, 392, 440, 523, 494, 392};
static const uint16_t music_dream_notes[]        = {220, 277, 330, 370, 330, 277, 247, 220};
static const uint16_t music_editor_notes[]       = {523, 659, 784, 659, 587, 698, 880, 698};
static const uint16_t music_boss_notes[]         = {196, 196, 233, 196, 262, 233, 220, 196};
static const uint16_t music_final_boss_notes[]   = {147, 196, 147, 220, 165, 247, 196, 294};
static const uint16_t music_random_battle_notes[]= {392, 440, 494, 587, 494, 440, 392, 330};
static const uint16_t music_results_notes[]      = {523, 659, 784, 1047, 784, 659, 587, 523};

static const WaifuMusicPattern *music_pattern_for_track(WaifuMusicTrack track)
{
    static const WaifuMusicPattern title      = {music_title_notes,         8, WAIFU_SOUND_SAMPLE_RATE / 5,  900, 2};
    static const WaifuMusicPattern dream      = {music_dream_notes,         8, WAIFU_SOUND_SAMPLE_RATE / 3,  760, 3};
    static const WaifuMusicPattern editor     = {music_editor_notes,        8, WAIFU_SOUND_SAMPLE_RATE / 6,  780, 2};
    static const WaifuMusicPattern boss       = {music_boss_notes,          8, WAIFU_SOUND_SAMPLE_RATE / 7, 1100, 1};
    static const WaifuMusicPattern final_boss = {music_final_boss_notes,    8, WAIFU_SOUND_SAMPLE_RATE / 8, 1300, 1};
    static const WaifuMusicPattern battle     = {music_random_battle_notes, 8, WAIFU_SOUND_SAMPLE_RATE / 7, 1000, 2};
    static const WaifuMusicPattern results    = {music_results_notes,       8, WAIFU_SOUND_SAMPLE_RATE / 4,  900, 2};
    switch (track) {
    case WAIFU_MUSIC_TITLE: return &title;
    case WAIFU_MUSIC_OPENING_DREAM: return &dream;
    case WAIFU_MUSIC_DECK_EDITOR: return &editor;
    case WAIFU_MUSIC_BOSS: return &boss;
    case WAIFU_MUSIC_FINAL_BOSS: return &final_boss;
    case WAIFU_MUSIC_RANDOM_BATTLE: return &battle;
    case WAIFU_MUSIC_RESULTS: return &results;
    default: return NULL;
    }
}

static int16_t placeholder_music_sample(void)
{
    const WaifuMusicPattern *p = music_pattern_for_track(g_music_track);
    uint32_t pos;
    int note_index;
    int freq;
    int period;
    int phase;
    int s;
    if (!p || p->note_count <= 0 || p->ticks_per_note <= 0) {
        ++g_music_pos;
        return 0;
    }
    pos = g_music_pos++;
    note_index = (int)((pos / (uint32_t)p->ticks_per_note) % (uint32_t)p->note_count);
    freq = p->notes[note_index];
    if (freq <= 0) return 0;
    period = WAIFU_SOUND_SAMPLE_RATE / freq;
    if (period < 2) period = 2;
    phase = (int)(pos % (uint32_t)period);
    if (p->duty <= 1) {
        s = (phase < period / 4) ? p->amp : -p->amp;
    } else if (p->duty == 3) {
        int half = period / 2;
        if (half < 1) half = 1;
        s = phase < half ? (-p->amp + (phase * p->amp * 2) / half)
                         : (p->amp - ((phase - half) * p->amp * 2) / half);
    } else {
        s = (phase < period / 2) ? p->amp : -p->amp;
    }
    /* Small octave shimmer so placeholder tracks are audibly distinct without
       requiring real music assets yet. Replace this generator with streamed
       music data per platform when final music files are available. */
    if (((pos / (uint32_t)(p->ticks_per_note * 2)) & 1u) != 0u) s = (s * 3) / 4;
    return (int16_t)s;
}

void waifu_sound_init(void)
{
    memset(g_voices, 0, sizeof(g_voices));
    g_music_track = WAIFU_MUSIC_NONE;
    g_music_pos = 0;
}

void waifu_sound_reset(void)
{
    memset(g_voices, 0, sizeof(g_voices));
    g_music_track = WAIFU_MUSIC_NONE;
    g_music_pos = 0;
}

void waifu_sound_set_music(WaifuMusicTrack track)
{
    if (track < 0 || track >= WAIFU_MUSIC_TRACK_COUNT) track = WAIFU_MUSIC_NONE;
    if (g_music_track != track) {
        g_music_track = track;
        g_music_pos = 0;
    }
}

WaifuMusicTrack waifu_sound_music_track(void)
{
    return g_music_track;
}

const char *waifu_sound_music_name(WaifuMusicTrack track)
{
    switch (track) {
    case WAIFU_MUSIC_TITLE: return "title";
    case WAIFU_MUSIC_OPENING_DREAM: return "opening_dream";
    case WAIFU_MUSIC_DECK_EDITOR: return "deck_editor";
    case WAIFU_MUSIC_BOSS: return "boss";
    case WAIFU_MUSIC_FINAL_BOSS: return "final_boss";
    case WAIFU_MUSIC_RANDOM_BATTLE: return "random_battle";
    case WAIFU_MUSIC_RESULTS: return "results";
    default: return "none";
    }
}

void waifu_sound_play(WaifuSoundEffect effect)
{
    int i;
    int best = -1;
    if (effect < 0 || effect >= WAIFU_SOUND_EFFECT_COUNT) return;
    if (asset_length(effect) <= 0) return;

    for (i = 0; i < WAIFU_SOUND_MAX_VOICES; ++i) {
        if (!g_voices[i].active) {
            best = i;
            break;
        }
    }
    if (best < 0) {
        int oldest_pos = -1;
        for (i = 0; i < WAIFU_SOUND_MAX_VOICES; ++i) {
            if (g_voices[i].pos > oldest_pos) {
                oldest_pos = g_voices[i].pos;
                best = i;
            }
        }
    }
    if (best < 0) return;
    g_voices[best].active = 1;
    g_voices[best].effect = effect;
    g_voices[best].pos = 0;
}

void waifu_sound_mix_s16(int16_t *dst, int frames)
{
    int f, ch;
    if (!dst || frames <= 0) return;
    for (f = 0; f < frames; ++f) {
        int mix = 0;
        int music = placeholder_music_sample();
        int i;
        mix += (music * WAIFU_SOUND_MUSIC_GAIN_NUM) / WAIFU_SOUND_MUSIC_GAIN_DEN;
        for (i = 0; i < WAIFU_SOUND_MAX_VOICES; ++i) {
            if (g_voices[i].active) {
                int len = asset_length(g_voices[i].effect);
                if (g_voices[i].pos >= len) {
                    g_voices[i].active = 0;
                } else {
                    int s = asset_sample_s16(g_voices[i].effect, g_voices[i].pos++);
                    mix += (s * WAIFU_SOUND_SFX_GAIN_NUM) / WAIFU_SOUND_SFX_GAIN_DEN;
                    if (g_voices[i].pos >= len) g_voices[i].active = 0;
                }
            }
        }
        mix = (mix * WAIFU_SOUND_MASTER_NUM) / WAIFU_SOUND_MASTER_DEN;
        for (ch = 0; ch < WAIFU_SOUND_CHANNELS; ++ch) dst[f * WAIFU_SOUND_CHANNELS + ch] = clamp_s16(mix);
    }
}

int waifu_sound_sample_rate(void) { return WAIFU_SOUND_SAMPLE_RATE; }
int waifu_sound_channels(void) { return WAIFU_SOUND_CHANNELS; }
int waifu_sound_samples_per_frame(void) { return WAIFU_SOUND_SAMPLES_PER_FRAME; }

const char *waifu_sound_effect_name(WaifuSoundEffect effect)
{
    switch (effect) {
    case WAIFU_SOUND_SELECT: return "Select";
    case WAIFU_SOUND_CONFIRM: return "Confirm";
    case WAIFU_SOUND_CONFIRM_ALT: return "ConfirmAlt";
    case WAIFU_SOUND_CARD_PLACED: return "CardPlaced";
    case WAIFU_SOUND_CARD_DESTROYED: return "CardDestroyed";
    case WAIFU_SOUND_TURN_PASSED: return "TurnPassed";
    case WAIFU_SOUND_YOU_LOST: return "Youlost";
    case WAIFU_SOUND_LASER_SHOOT: return "laserShoot";
    default: return "Unknown";
    }
}

static void write_le16(FILE *fp, uint16_t v)
{
    fputc((int)(v & 255u), fp);
    fputc((int)((v >> 8) & 255u), fp);
}

static void write_le32(FILE *fp, uint32_t v)
{
    fputc((int)(v & 255u), fp);
    fputc((int)((v >> 8) & 255u), fp);
    fputc((int)((v >> 16) & 255u), fp);
    fputc((int)((v >> 24) & 255u), fp);
}

static void write_wav_header(FILE *fp, uint32_t data_bytes, int sample_rate, int channels)
{
    uint32_t byte_rate = (uint32_t)(sample_rate * channels * 2);
    uint16_t block_align = (uint16_t)(channels * 2);
    fwrite("RIFF", 1, 4, fp);
    write_le32(fp, 36u + data_bytes);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    write_le32(fp, 16);
    write_le16(fp, 1);
    write_le16(fp, (uint16_t)channels);
    write_le32(fp, (uint32_t)sample_rate);
    write_le32(fp, byte_rate);
    write_le16(fp, block_align);
    write_le16(fp, 16);
    fwrite("data", 1, 4, fp);
    write_le32(fp, data_bytes);
}

int waifu_sound_wav_open(WaifuSoundWavWriter *wr, const char *path)
{
    if (!wr || !path) return 0;
    memset(wr, 0, sizeof(*wr));
    wr->fp = fopen(path, "wb");
    if (!wr->fp) return 0;
    wr->sample_rate = WAIFU_SOUND_SAMPLE_RATE;
    wr->channels = WAIFU_SOUND_CHANNELS;
    write_wav_header(wr->fp, 0, wr->sample_rate, wr->channels);
    if (ferror(wr->fp)) {
        fclose(wr->fp);
        memset(wr, 0, sizeof(*wr));
        return 0;
    }
    return 1;
}

int waifu_sound_wav_write(WaifuSoundWavWriter *wr, const int16_t *samples, int frames)
{
    int total_samples, i;
    if (!wr || !wr->fp || !samples || frames < 0) return 0;
    total_samples = frames * wr->channels;
    for (i = 0; i < total_samples; ++i) {
        uint16_t v = (uint16_t)samples[i];
        write_le16(wr->fp, v);
    }
    wr->data_bytes += (uint32_t)(total_samples * 2);
    return ferror(wr->fp) ? 0 : 1;
}

int waifu_sound_wav_close(WaifuSoundWavWriter *wr)
{
    int ok;
    if (!wr || !wr->fp) return 0;
    ok = 1;
    if (fseek(wr->fp, 0, SEEK_SET) != 0) ok = 0;
    if (ok) write_wav_header(wr->fp, wr->data_bytes, wr->sample_rate, wr->channels);
    if (fclose(wr->fp) != 0) ok = 0;
    memset(wr, 0, sizeof(*wr));
    return ok;
}

void waifu_sound_wav_abort(WaifuSoundWavWriter *wr)
{
    if (!wr) return;
    if (wr->fp) fclose(wr->fp);
    memset(wr, 0, sizeof(*wr));
}
