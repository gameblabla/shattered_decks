#include "vgmplay.h"
#include "media.h"
#include "io.h"
#include <stdint.h>

#define FTV_BUFFER_SIZE       (384u * 1024u)
#define FTV_HEADER_SIZE       32u
#define FTV_RATE              44100u
#define YM_ADDR0              0x4D8
#define YM_DATA0              0x4DA
#define YM_ADDR1              0x4DC
#define YM_DATA1              0x4DE
#define YM_BUSY               0x80
#define YM_SOUND_MUTE         0x4D5
#define YM_SOUND_AUDIO        0x4EC
#define PIT1_COUNT            0x0042
#define PIT1_CONTROL          0x0046
#define PIT_INT_REASON        0x0060
#define PIT1_TIMEOUT          0x02

static uint8_t g_ftv[FTV_BUFFER_SIZE];
static uint32_t g_pcm_size;
static uint32_t g_stream_offset;
static uint32_t g_stream_size;
static uint8_t g_loaded;
static volatile uint8_t g_stop_requested;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ym_ready(void)
{
    while (inb(YM_ADDR0) & YM_BUSY) {
    }
}

static void ym_write(uint8_t port, uint8_t reg, uint8_t value)
{
    ym_ready();
    outb(reg, port ? YM_ADDR1 : YM_ADDR0);
    outb(value, port ? YM_DATA1 : YM_DATA0);
}

/* Pace from PIT channel 1's 307.2kHz hardware clock.  Its ratio to VGM's
 * 44.1kHz timeline reduces exactly to 1024/147.  The remainder is carried
 * between calls, so short DAC waits do not accumulate rounding drift.
 * Channel 1 is fixed to mode 0 by TOWNS hardware; rewriting its 16-bit count
 * clears TMOUT1 and starts the one-shot.  The longest wait in this input is
 * 8,545 samples (59,525 PIT ticks), safely below 65,535. */
static void wait_samples(uint16_t samples, uint32_t *fraction)
{
    uint32_t numerator = *fraction + (uint32_t)samples * 1024u;
    uint16_t ticks = (uint16_t)(numerator / 147u);
    *fraction = numerator % 147u;

    if (ticks) {
        /* Start the deadline before waiting for YM BUSY.  BUSY is elapsed
         * wall-clock time after the preceding register write and therefore
         * belongs inside this VGM wait.  Polling it first made every DAC
         * sample add the chip delay on top of the score (about +18.6s here). */
        outb(0x70, PIT1_CONTROL); /* channel 1, low+high, binary, mode 0 */
        outb((uint8_t)ticks, PIT1_COUNT);
        outb((uint8_t)(ticks >> 8), PIT1_COUNT);
        ym_ready();
        while (!(inb(PIT_INT_REASON) & PIT1_TIMEOUT)) {
        }
    } else {
        ym_ready();
    }
}

int fmt_vgm_load_file(const char *name)
{
    int32_t loaded = fmt_media_load(name, g_ftv, sizeof(g_ftv));

    g_loaded = 0;
    if (loaded < (int32_t)FTV_HEADER_SIZE || g_ftv[0] != 'F' ||
        g_ftv[1] != 'T' || g_ftv[2] != 'V' || g_ftv[3] != '1' ||
        rd32(g_ftv + 4) != FTV_HEADER_SIZE) {
        return -1;
    }
    g_pcm_size = rd32(g_ftv + 16);
    g_stream_offset = rd32(g_ftv + 20);
    g_stream_size = rd32(g_ftv + 24);
    if (g_pcm_size > (uint32_t)loaded - FTV_HEADER_SIZE ||
        g_stream_offset < FTV_HEADER_SIZE + g_pcm_size ||
        g_stream_offset > (uint32_t)loaded ||
        g_stream_size > (uint32_t)loaded - g_stream_offset) {
        return -1;
    }

    g_loaded = 1;
    return 0;
}

void fmt_vgm_stop(void)
{
    g_stop_requested = 1;
    outb(0x00, YM_SOUND_MUTE);
}

int fmt_vgm_play(void)
{
    const uint8_t *pcm, *p, *end;
    uint32_t pcm_pos = 0, fraction = 0;

    if (!g_loaded) {
        return -1;
    }

    g_stop_requested = 0;
    pcm = g_ftv + FTV_HEADER_SIZE;
    p = g_ftv + g_stream_offset;
    end = p + g_stream_size;

    outb(0x03, YM_SOUND_MUTE);
    outb(0x7F, YM_SOUND_AUDIO);

    while (p < end) {
        if (g_stop_requested) {
            return 0;
        }
        uint8_t op = *p++;
        if (op == 0x00) {
            return 0;
        } else if (op == 0x01) {
            if ((uint32_t)(end - p) < 3u || p[0] > 1u) return -1;
            ym_write(p[0], p[1], p[2]);
            p += 3;
        } else if (op == 0x02) {
            uint16_t samples;
            if ((uint32_t)(end - p) < 2u) return -1;
            samples = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            p += 2;
            wait_samples(samples, &fraction);
        } else if (op == 0x03) {
            if ((uint32_t)(end - p) < 4u) return -1;
            pcm_pos = rd32(p);
            p += 4;
            if (pcm_pos > g_pcm_size) return -1;
        } else if (op == 0x80) {
            uint16_t count, packed_size, escapes = 0;
            const uint8_t *packed, *escape_data;
            if (p == end) return -1;
            count = *p++;
            if (count == 0) count = 256;
            packed_size = (uint16_t)((count + 3u) / 4u);
            if ((uint32_t)(end - p) < packed_size) return -1;
            packed = p;
            escape_data = p + packed_size;
            for (uint16_t i = 0; i < count; i++)
                if (((packed[i >> 2] >> ((i & 3u) * 2u)) & 3u) == 3u) escapes++;
            if ((uint32_t)(end - escape_data) < escapes ||
                pcm_pos + count > g_pcm_size) return -1;
            for (uint16_t i = 0, e = 0; i < count; i++) {
                uint8_t code = (packed[i >> 2] >> ((i & 3u) * 2u)) & 3u;
                uint8_t samples = code == 0 ? 3 : code == 1 ? 2 :
                                  code == 2 ? 1 : escape_data[e++];
                ym_write(0, 0x2A, pcm[pcm_pos++]);
                if (samples) wait_samples(samples, &fraction);
            }
            p = escape_data + escapes;
        } else {
            return -1;
        }
    }
    return -1;
}
