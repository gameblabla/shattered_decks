#include "atarist_audio.h"
#include "atarist_hw.h"

typedef struct AtaristStream {
    const uint8_t *data;
    int32_t        size;
    uint32_t       loop_off;
    uint16_t       flags;
} AtaristStream;

static AtaristStream  g_streams[ATARIST_MUSIC_COUNT];
static const uint8_t *g_play;      /* next frame's record */
static const uint8_t *g_play_end;
static const uint8_t *g_play_loop;
static uint16_t       g_play_flags;
static uint8_t        g_track;
static uint8_t        g_playing;

/* Register 7 is the mixer: bits 0-2 mute the tone channels, 3-5 the noise.
 * 0x3f is "everything off", which is what silence has to mean -- clearing the
 * volumes alone leaves an envelope running. */
static void ym_write(uint8_t reg, uint8_t val)
{
    ST_YM_SELECT = reg;
    ST_YM_WRITE = val;
}

static void ym_silence(void)
{
    ym_write(8, 0);
    ym_write(9, 0);
    ym_write(10, 0);
    ym_write(7, 0x3f);
}

void Atarist_AudioInit(void)
{
    int i;
    for (i = 0; i < ATARIST_MUSIC_COUNT; ++i) {
        g_streams[i].data = 0;
        g_streams[i].size = 0;
    }
    g_playing = 0;
    g_track = ATARIST_MUSIC_NONE;
    ym_silence();
}

void Atarist_AudioShutdown(void)
{
    g_playing = 0;
    ym_silence();
}

void Atarist_MusicSetStream(int track, const uint8_t *stream, int32_t len)
{
    uint32_t magic;
    if (track <= 0 || track >= ATARIST_MUSIC_COUNT) return;
    if (!stream || len < 16) return;
    magic = ((uint32_t)stream[0] << 24) | ((uint32_t)stream[1] << 16) |
            ((uint32_t)stream[2] << 8) | stream[3];
    if (magic != 0x594d5331u /* 'YMS1' */) return;
    g_streams[track].flags    = (uint16_t)((stream[6] << 8) | stream[7]);
    g_streams[track].loop_off = ((uint32_t)stream[8] << 24) |
                                ((uint32_t)stream[9] << 16) |
                                ((uint32_t)stream[10] << 8) | stream[11];
    g_streams[track].data = stream + 16;
    g_streams[track].size = len - 16;
}

void Atarist_MusicPlay(int track)
{
    AtaristStream *s;
    if (track <= 0 || track >= ATARIST_MUSIC_COUNT) { Atarist_MusicStop(); return; }
    if (g_playing && g_track == (uint8_t)track) return;
    s = &g_streams[track];
    if (!s->data) { Atarist_MusicStop(); return; }

    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    g_play       = s->data;
    g_play_end   = s->data + s->size;
    g_play_loop  = s->data + s->loop_off;
    g_play_flags = s->flags;
    g_track      = (uint8_t)track;
    g_playing    = 1;
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");
}

void Atarist_MusicStop(void)
{
    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    g_playing = 0;
    g_track = ATARIST_MUSIC_NONE;
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");
    ym_silence();
}

int Atarist_MusicTrack(void) { return g_track; }

void Atarist_AudioTick(void)
{
    const uint8_t *p;
    uint8_t count;

    if (!g_playing) return;
    p = g_play;
    if (p >= g_play_end) {
        if (!(g_play_flags & 1u)) { g_playing = 0; ym_silence(); return; }
        p = g_play_loop;
        if (p >= g_play_end) { g_playing = 0; ym_silence(); return; }
    }
    count = *p++;
    while (count--) {
        uint8_t reg = *p++;
        uint8_t val = *p++;
        ym_write(reg, val);
    }
    g_play = p;
}
