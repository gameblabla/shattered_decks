#include "sounds.h"

#include <string.h>
#include <stdlib.h>

#if !defined(WAIFU_FM_PCFX) && !defined(WAIFU_FM_CD32X)
#define WAIFU_SOUND_USE_STREAMED_MUSIC 1
#endif

#if !defined(WAIFU_FM_PCFX) && !defined(WAIFU_FM_CD32X)
#include "sound_assets.h"
#endif

#define WAIFU_SOUND_MAX_VOICES 16
#define WAIFU_SOUND_MASTER_NUM 3
#define WAIFU_SOUND_MASTER_DEN 4
#define WAIFU_SOUND_SFX_GAIN_NUM 1
#define WAIFU_SOUND_SFX_GAIN_DEN 1
#define WAIFU_SOUND_MUSIC_GAIN_NUM 1
#define WAIFU_SOUND_MUSIC_GAIN_DEN 2

#if defined(WAIFU_FM_PCFX)
extern void waifu_pcfx_sfx_play(int effect);
#endif
#if defined(WAIFU_FM_CD32X)
extern void waifu_cd32x_audio_play_sfx(int effect);
#endif

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

#if defined(WAIFU_SOUND_USE_STREAMED_MUSIC)
/* ---------------------------------------------------------------------------
 * Streamed music playback.
 *
 * The music tracks live on disk as ordinary WAV files in Music/. They are far
 * too large to embed, so they are streamed a few KB at a time straight through
 * the existing software mixer (no SDL_mixer). The reader is plain stdio + a
 * small ring of bytes, so it is platform agnostic; only the PC-FX build (which
 * uses CD-DA hardware for music) compiles it out.
 *
 * Source frames are converted to signed-16 stereo and resampled to the mixer
 * rate with a fixed-point (.16) phase accumulator (nearest-neighbour, which is
 * inaudible here and avoids any floating point). Tracks loop seamlessly.
 * ------------------------------------------------------------------------- */
#define WAIFU_MUSIC_READ_BUF 8192

typedef struct WaifuMusicStream {
    FILE *fp;
    long data_start;       /* byte offset of the WAV data chunk */
    long data_bytes;       /* size of the data chunk */
    long data_pos;         /* bytes consumed from the data chunk */
    int src_rate;
    int src_channels;
    int src_bits;          /* 8 (unsigned) or 16 (signed) */
    uint32_t step;         /* source frames per output frame, .16 fixed point */
    uint32_t acc;          /* fractional resample position, .16 */
    int cur_l, cur_r;      /* current source frame (s16 range) */
    int have_cur;
    unsigned char buf[WAIFU_MUSIC_READ_BUF];
    int buf_len;
    int buf_pos;
} WaifuMusicStream;

static WaifuMusicStream g_music_stream;

static uint16_t rd_le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *music_track_path(WaifuMusicTrack track)
{
    /* Several states share a track when no dedicated file exists yet. */
    switch (track) {
    case WAIFU_MUSIC_TITLE:         return "Music/Titlescreen_MoonlitCipher.wav";
    case WAIFU_MUSIC_OPENING_DREAM: return "Music/Overworld.wav";
    case WAIFU_MUSIC_DECK_EDITOR:   return "Music/Overworld.wav";
    case WAIFU_MUSIC_BOSS:          return "Music/Boss.wav";
    case WAIFU_MUSIC_FINAL_BOSS:    return "Music/FinalBoss.wav";
    case WAIFU_MUSIC_RANDOM_BATTLE: return "Music/Battle.wav";
    case WAIFU_MUSIC_RESULTS:       return "Music/Victory.wav";
    case WAIFU_MUSIC_LOST:          return "Music/Fail.wav";
    default:                        return NULL;
    }
}

static void music_stream_close(WaifuMusicStream *st)
{
    if (st->fp) fclose(st->fp);
    memset(st, 0, sizeof(*st));
}

static int music_stream_open(WaifuMusicStream *st, const char *path)
{
    unsigned char hdr[8];
    int fmt_ok = 0;
    FILE *fp;
    const char *dir = getenv("WAIFU_MUSIC_DIR");

    memset(st, 0, sizeof(*st));
    if (!path) return 0;

    fp = NULL;
    if (dir && dir[0]) {
        /* Allow overriding just the directory; path is "Music/<file>". */
        const char *base = strrchr(path, '/');
        char buf[512];
        base = base ? base + 1 : path;
        if (snprintf(buf, sizeof(buf), "%s/%s", dir, base) < (int)sizeof(buf))
            fp = fopen(buf, "rb");
    }
    if (!fp) fp = fopen(path, "rb");
    if (!fp) return 0;

    if (fread(hdr, 1, 8, fp) != 8 || memcmp(hdr, "RIFF", 4) != 0) {
        fclose(fp);
        return 0;
    }
    if (fread(hdr, 1, 4, fp) != 4 || memcmp(hdr, "WAVE", 4) != 0) {
        fclose(fp);
        return 0;
    }
    /* Walk the chunk list for 'fmt ' and 'data'. */
    while (fread(hdr, 1, 8, fp) == 8) {
        uint32_t csz = rd_le32(hdr + 4);
        if (memcmp(hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if (csz < 16 || fread(fmt, 1, 16, fp) != 16) break;
            if (rd_le16(fmt) != 1) break;           /* PCM only */
            st->src_channels = rd_le16(fmt + 2);
            st->src_rate     = (int)rd_le32(fmt + 4);
            st->src_bits     = rd_le16(fmt + 14);
            fmt_ok = 1;
            if (csz > 16) fseek(fp, (long)(csz - 16), SEEK_CUR);
        } else if (memcmp(hdr, "data", 4) == 0) {
            st->data_start = ftell(fp);
            st->data_bytes = (long)csz;
            break;
        } else {
            fseek(fp, (long)(csz + (csz & 1u)), SEEK_CUR);
        }
    }
    if (!fmt_ok || st->data_start == 0 || st->data_bytes <= 0 ||
        (st->src_bits != 8 && st->src_bits != 16) ||
        st->src_channels < 1 || st->src_rate <= 0) {
        fclose(fp);
        memset(st, 0, sizeof(*st));
        return 0;
    }
    st->fp = fp;
    st->step = (uint32_t)(((uint64_t)st->src_rate << 16) / WAIFU_SOUND_SAMPLE_RATE);
    if (st->step == 0) st->step = 1;
    return 1;
}

/* Pull n bytes from the data chunk, looping back to data_start at EOF. */
static void music_read_bytes(WaifuMusicStream *st, unsigned char *out, int n)
{
    int got = 0;
    while (got < n) {
        if (st->buf_pos >= st->buf_len) {
            long remain = st->data_bytes - st->data_pos;
            int want = WAIFU_MUSIC_READ_BUF;
            if (remain <= 0) {
                fseek(st->fp, st->data_start, SEEK_SET);
                st->data_pos = 0;
                remain = st->data_bytes;
            }
            if ((long)want > remain) want = (int)remain;
            st->buf_len = (int)fread(st->buf, 1, (size_t)want, st->fp);
            st->buf_pos = 0;
            if (st->buf_len <= 0) {
                /* Read failure: emit silence and bail out. */
                memset(out + got, 0, (size_t)(n - got));
                return;
            }
            st->data_pos += st->buf_len;
        }
        out[got++] = st->buf[st->buf_pos++];
    }
}

static void music_read_src_frame(WaifuMusicStream *st, int *l, int *r)
{
    unsigned char raw[8];
    int bytes = (st->src_bits / 8) * st->src_channels;
    if (bytes > (int)sizeof(raw)) bytes = (int)sizeof(raw);
    music_read_bytes(st, raw, bytes);
    if (st->src_bits == 16) {
        int s0 = (int16_t)rd_le16(raw);
        if (st->src_channels >= 2) {
            *l = s0;
            *r = (int16_t)rd_le16(raw + 2);
        } else {
            *l = *r = s0;
        }
    } else { /* unsigned 8-bit */
        int s0 = ((int)raw[0] - 128) << 8;
        if (st->src_channels >= 2) {
            *l = s0;
            *r = ((int)raw[1] - 128) << 8;
        } else {
            *l = *r = s0;
        }
    }
}

static void music_next_frame(WaifuMusicStream *st, int *l, int *r)
{
    if (!st->have_cur) {
        music_read_src_frame(st, &st->cur_l, &st->cur_r);
        st->have_cur = 1;
    }
    *l = st->cur_l;
    *r = st->cur_r;
    st->acc += st->step;
    while (st->acc >= 0x10000u) {
        st->acc -= 0x10000u;
        music_read_src_frame(st, &st->cur_l, &st->cur_r);
    }
}
#endif /* WAIFU_SOUND_USE_STREAMED_MUSIC */

#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
static int asset_count(void)
{
    return WAIFU_SOUND_EFFECT_COUNT;
}

static int asset_length(WaifuSoundEffect effect)
{
    (void)effect;
    return 0;
}

static int16_t asset_sample_s16(WaifuSoundEffect effect, int pos)
{
    (void)effect;
    (void)pos;
    return 0;
}
#else
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

#endif

#if defined(WAIFU_FM_PCFX)
/* PSG-style placeholder music generator. Only the PC-FX build still references
   this; host/headless stream real WAV music through the mixer instead. */
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
static const uint16_t music_lost_notes[]         = {220, 196, 165, 147, 131, 147, 165, 196};

static const WaifuMusicPattern *music_pattern_for_track(WaifuMusicTrack track)
{
    static const WaifuMusicPattern title      = {music_title_notes,         8, WAIFU_SOUND_SAMPLE_RATE / 5,  900, 2};
    static const WaifuMusicPattern dream      = {music_dream_notes,         8, WAIFU_SOUND_SAMPLE_RATE / 3,  760, 3};
    static const WaifuMusicPattern editor     = {music_editor_notes,        8, WAIFU_SOUND_SAMPLE_RATE / 6,  780, 2};
    static const WaifuMusicPattern boss       = {music_boss_notes,          8, WAIFU_SOUND_SAMPLE_RATE / 7, 1100, 1};
    static const WaifuMusicPattern final_boss = {music_final_boss_notes,    8, WAIFU_SOUND_SAMPLE_RATE / 8, 1300, 1};
    static const WaifuMusicPattern battle     = {music_random_battle_notes, 8, WAIFU_SOUND_SAMPLE_RATE / 7, 1000, 2};
    static const WaifuMusicPattern results    = {music_results_notes,       8, WAIFU_SOUND_SAMPLE_RATE / 4,  900, 2};
    static const WaifuMusicPattern lost       = {music_lost_notes,          8, WAIFU_SOUND_SAMPLE_RATE / 4,  900, 3};
    switch (track) {
    case WAIFU_MUSIC_TITLE: return &title;
    case WAIFU_MUSIC_OPENING_DREAM: return &dream;
    case WAIFU_MUSIC_DECK_EDITOR: return &editor;
    case WAIFU_MUSIC_BOSS: return &boss;
    case WAIFU_MUSIC_FINAL_BOSS: return &final_boss;
    case WAIFU_MUSIC_RANDOM_BATTLE: return &battle;
    case WAIFU_MUSIC_RESULTS: return &results;
    case WAIFU_MUSIC_LOST: return &lost;
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
#endif /* WAIFU_FM_PCFX */

void waifu_sound_init(void)
{
    memset(g_voices, 0, sizeof(g_voices));
    g_music_track = WAIFU_MUSIC_NONE;
    g_music_pos = 0;
#if defined(WAIFU_SOUND_USE_STREAMED_MUSIC)
    music_stream_close(&g_music_stream);
#endif
}

void waifu_sound_reset(void)
{
    memset(g_voices, 0, sizeof(g_voices));
    g_music_track = WAIFU_MUSIC_NONE;
    g_music_pos = 0;
#if defined(WAIFU_SOUND_USE_STREAMED_MUSIC)
    music_stream_close(&g_music_stream);
#endif
}

void waifu_sound_set_music(WaifuMusicTrack track)
{
    if (track < 0 || track >= WAIFU_MUSIC_TRACK_COUNT) track = WAIFU_MUSIC_NONE;
    if (g_music_track != track) {
        g_music_track = track;
        g_music_pos = 0;
#if defined(WAIFU_SOUND_USE_STREAMED_MUSIC)
        music_stream_close(&g_music_stream);
        if (track != WAIFU_MUSIC_NONE)
            music_stream_open(&g_music_stream, music_track_path(track));
#endif
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
    case WAIFU_MUSIC_LOST: return "lost";
    default: return "none";
    }
}

void waifu_sound_play(WaifuSoundEffect effect)
{
#if defined(WAIFU_FM_PCFX)
    if (effect < 0 || effect >= WAIFU_SOUND_EFFECT_COUNT) return;
    waifu_pcfx_sfx_play((int)effect);
#elif defined(WAIFU_FM_CD32X)
    if (effect < 0 || effect >= WAIFU_SOUND_EFFECT_COUNT) return;
    waifu_cd32x_audio_play_sfx((int)effect);
#else
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
#endif
}

void waifu_sound_mix_s16(int16_t *dst, int frames)
{
    int f, ch;
    if (!dst || frames <= 0) return;
    for (f = 0; f < frames; ++f) {
        int left = 0, right = 0;
        int sfx = 0;
        int i;

        /* Music: streamed stereo on host/headless, PSG placeholder on PC-FX
           (whose real music is CD-DA and never reaches this mixer). */
#if defined(WAIFU_FM_PCFX)
        {
            int music = placeholder_music_sample();
            left  += (music * WAIFU_SOUND_MUSIC_GAIN_NUM) / WAIFU_SOUND_MUSIC_GAIN_DEN;
            right += (music * WAIFU_SOUND_MUSIC_GAIN_NUM) / WAIFU_SOUND_MUSIC_GAIN_DEN;
        }
#elif defined(WAIFU_SOUND_USE_STREAMED_MUSIC)
        if (g_music_stream.fp) {
            int ml = 0, mr = 0;
            music_next_frame(&g_music_stream, &ml, &mr);
            left  += (ml * WAIFU_SOUND_MUSIC_GAIN_NUM) / WAIFU_SOUND_MUSIC_GAIN_DEN;
            right += (mr * WAIFU_SOUND_MUSIC_GAIN_NUM) / WAIFU_SOUND_MUSIC_GAIN_DEN;
        }
#endif

        /* Sound effects are mono and play centred on both channels. */
        for (i = 0; i < WAIFU_SOUND_MAX_VOICES; ++i) {
            if (g_voices[i].active) {
                int len = asset_length(g_voices[i].effect);
                if (g_voices[i].pos >= len) {
                    g_voices[i].active = 0;
                } else {
                    int s = asset_sample_s16(g_voices[i].effect, g_voices[i].pos++);
                    sfx += (s * WAIFU_SOUND_SFX_GAIN_NUM) / WAIFU_SOUND_SFX_GAIN_DEN;
                    if (g_voices[i].pos >= len) g_voices[i].active = 0;
                }
            }
        }
        left  += sfx;
        right += sfx;

        left  = (left  * WAIFU_SOUND_MASTER_NUM) / WAIFU_SOUND_MASTER_DEN;
        right = (right * WAIFU_SOUND_MASTER_NUM) / WAIFU_SOUND_MASTER_DEN;

        if (WAIFU_SOUND_CHANNELS >= 2) {
            dst[f * WAIFU_SOUND_CHANNELS + 0] = clamp_s16(left);
            dst[f * WAIFU_SOUND_CHANNELS + 1] = clamp_s16(right);
            for (ch = 2; ch < WAIFU_SOUND_CHANNELS; ++ch)
                dst[f * WAIFU_SOUND_CHANNELS + ch] = clamp_s16(left);
        } else {
            for (ch = 0; ch < WAIFU_SOUND_CHANNELS; ++ch)
                dst[f * WAIFU_SOUND_CHANNELS + ch] = clamp_s16((left + right) / 2);
        }
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
    case WAIFU_SOUND_LASER_SHOOT: return "AttackImpact";
    case WAIFU_SOUND_DIRECT_HIT: return "DirectHit";
    case WAIFU_SOUND_CARD_DRAWN: return "CardDrawn";
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
