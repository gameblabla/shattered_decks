#include "mp2stream.h"
#include "mp2.h"
#include "kjmp2_fast.h"
#include "dacout.h"
#include "io.h"
#include "media.h"

/*
 * MP2 playback off the data track, through the YM2612 channel-6 DAC.
 *
 * Three things have to happen at once here and none of them can block the
 * others: the drive has to be kept reading, a frame has to be decoded roughly
 * every 72ms, and a sample has to reach the DAC every 62.5us.  The last of
 * those is the hard deadline, and it is met by polling rather than by
 * interrupts (nothing in this payload is interrupt-driven - see pcmstream.c).
 *
 * The arrangement is:
 *
 *   - fmt_dac_tick() (dacout.h) owns the deadline.  It is paced by the TOWNS
 *     1us free-running timer at I/O 0x26, not by the YM2612 busy flag, so the
 *     output rate is exactly 16000 Hz on hardware and emulator alike.
 *   - The decoder calls that same tick from inside its inner loops, so
 *     decoding happens in the gaps between samples instead of stalling them.
 *     This is the part the previous version got wrong: it decoded up to five
 *     frames in one blocking burst after the pacing wait, which meant no
 *     sample reached the DAC for the duration - around 45% of real time.
 *   - Two frame buffers alternate.  One is playing out while the other is
 *     decoded into, which gives the decoder a whole frame period of slack and
 *     removes any need for ring-wrap arithmetic in the decoder's hot path.
 *   - Refills happen between frames and from the idle loop, where a few
 *     hundred microseconds of latency costs nothing.
 */

/* 32 KiB of compressed input is eight seconds at 32 kbit/s: far more slack
 * than the drive needs, and cheap now that the decoded side is two 1152-byte
 * buffers rather than a 4.6 KiB-per-frame PSG word array. */
#define MP2_INPUT_RING  32768u
#define MP2_LOOKAHEAD   16u

static uint8_t g_input[MP2_INPUT_RING];
static uint8_t g_frame[289 + MP2_LOOKAHEAD];
static uint8_t g_pcm[2][FMT_MP2_SAMPLES_PER_FRAME];
static kjmp2v_context_t g_decoder;
static uint32_t g_media_off, g_size;
static uint8_t g_loaded;
static volatile uint8_t g_stop;

/* Diagnostic counters, published to a fixed address so the player can be
 * observed in-machine with `MEMDUMP PHYS:00080000` while it runs. Enabled by
 * `make BUSY_PROBE=1`; compiled out otherwise. See docs and src/main.c. */
#ifdef FMT_MP2_STATS
#define MP2_STATS ((volatile uint32_t *)0x00080000u)
static uint32_t g_frames;
static uint32_t g_elapsed_us;
static uint16_t g_stat_prev;
/* Must be called far more often than the free-running counter's 65.536ms
 * wrap, not once per frame: a frame is 72ms of audio, so per-frame sampling
 * aliased every interval down by exactly one wrap and made playback look 11x
 * faster than it was. Called from the drain loop, which runs every tick. */
static void mp2_stats_publish(void)
{
    uint16_t now = inw(FMT_DAC_FREERUN_TIMER);
    g_elapsed_us += (uint16_t)(now - g_stat_prev);
    g_stat_prev = now;
    MP2_STATS[0] = 0x3253504du;            /* "MP2S" */
    MP2_STATS[1] = g_frames;
    MP2_STATS[2] = g_elapsed_us;
    MP2_STATS[3] = fmt_dac.underruns;
    MP2_STATS[4] = (uint32_t)fmt_dac.pos;
    MP2_STATS[5] = 0xA5A5A5A5u;
}
#define MP2_STATS_FRAME()  do { g_frames++; mp2_stats_publish(); } while (0)
#define MP2_STATS_SAMPLE() mp2_stats_publish()
#define MP2_STATS_START() do { g_frames = 0; g_elapsed_us = 0; \
                               g_stat_prev = inw(FMT_DAC_FREERUN_TIMER); } while (0)
#else
#define MP2_STATS_FRAME()  ((void)0)
#define MP2_STATS_SAMPLE() ((void)0)
#define MP2_STATS_START()  ((void)0)
#endif

int fmt_mp2_stream_load_file(const char *name)
{
    g_loaded = 0;
    if (fmt_media_find(name, &g_media_off, &g_size) || !g_size) return -1;
    kjmp2v_init(&g_decoder);
    g_loaded = 1;
    return 0;
}

void fmt_mp2_stream_stop(void)
{
    g_stop = 1;
    fmt_dac_stop();
}

/* Copy one compressed frame out of the input ring, or return 0 if the medium has
 * not delivered all of it yet.  The extra MP2_LOOKAHEAD bytes cover the bit
 * reader's read-ahead window, which can reach just past the frame proper. */
static unsigned take_frame(fmt_media_stream *media, uint32_t *input_pos)
{
    unsigned size, i;

    if (media->fill_pos - *input_pos < 4) return 0;
    for (i = 0; i < 4; i++)
        g_frame[i] = g_input[(*input_pos + i) & (MP2_INPUT_RING - 1u)];
    size = fmt_mp2_frame_size(g_frame);
    if (!size) return 0;
    if (media->fill_pos - *input_pos < size + MP2_LOOKAHEAD) return 0;
    for (i = 4; i < size + MP2_LOOKAHEAD; i++)
        g_frame[i] = g_input[(*input_pos + i) & (MP2_INPUT_RING - 1u)];
    return size;
}

int fmt_mp2_stream_play_streaming(void)
{
    fmt_media_stream media;
    uint32_t input_pos = 0;
    unsigned size;
    uint8_t which = 0;

    if (!g_loaded) return -1;
    g_stop = 0;
    fmt_media_stream_init(&media, g_media_off, g_size, g_input, MP2_INPUT_RING, 1);

    /* Decode the first frame before making a sound.  Nothing is playing yet,
     * so this one may take as long as it likes. */
    for (;;) {
        if (media.state == FMT_MEDIA_STREAM_ERROR) return -1;
        size = take_frame(&media, &input_pos);
        if (size) break;
        if (media.state == FMT_MEDIA_STREAM_DONE) return -1;
        fmt_media_stream_step(&media, input_pos, 256);
    }
    if (kjmp2v_decode_frame_pcm8(&g_decoder, g_frame, g_pcm[0]) != size) return -1;
    input_pos += size;

    fmt_dac_start();
    fmt_dac_submit(g_pcm[0], FMT_MP2_SAMPLES_PER_FRAME);
    MP2_STATS_START();
    __asm__ __volatile__("cli");

    for (;;) {
        uint8_t next = which ^ 1u;

        /* Decode the following frame while the current one plays out.  Every
         * inner loop of the decoder ticks the DAC, so this call keeps the
         * output running rather than interrupting it. */
        for (;;) {
            if (g_stop) goto done;
            size = take_frame(&media, &input_pos);
            if (size) break;
            if (media.state == FMT_MEDIA_STREAM_ERROR ||
                media.state == FMT_MEDIA_STREAM_DONE) goto done;
            /* Waiting on the drive: one I/O step at a time, ticking between
             * each so the deadline is still met while we wait. */
            fmt_dac_tick();
            fmt_media_stream_step(&media, input_pos, 1);
        }
        if (kjmp2v_decode_frame_pcm8(&g_decoder, g_frame, g_pcm[next]) != size) goto done;
        input_pos += size;

        /* Hand over only once the playing buffer is actually finished, so a
         * decode that came in early does not cut the current frame short. */
        while (!fmt_dac_drained()) {
            if (g_stop) goto done;
            fmt_dac_tick();
            MP2_STATS_SAMPLE();
            fmt_media_stream_step(&media, input_pos, 1);
        }
        fmt_dac_submit(g_pcm[next], FMT_MP2_SAMPLES_PER_FRAME);
        which = next;
        MP2_STATS_FRAME();
    }

done:
    fmt_dac_stop();
    return 0;
}

uint32_t fmt_mp2_stream_underruns(void)
{
    return fmt_dac.underruns;
}
