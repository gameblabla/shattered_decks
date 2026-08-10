#include "pcmstream.h"
#include "dacout.h"
#include "io.h"
#include "media.h"

/* See pcmstream.h for the overall design (ring buffer streamed off the
 * disc from inside the DAC pacing loop). */

#define YM_ADDR0            0x4D8
#define YM_DATA0            0x4DA
#define YM_REG_DAC_ENABLE   0x2B
#define YM_REG_DAC_DATA     0x2A
#define YM_BUSY             0x80


/* TOWNSIO_SOUND_MUTE/TOWNSIO_SOUND_AUDIO - see the FM TOWNS Technical
 * Databook, Table I-5-42 (Audio register, 0x4EC) and I-5-43 (FM/PCM
 * mute register, 0x4D5): both are active-high enables, *not* active-
 * high mutes - "0 = no output"/"0 = muted", the bit must be SET for
 * sound to come out. They default to 0 (silent) at reset, and nothing
 * else in this boot path (no BIOS/TowsOS SND library init, unlike
 * FMTOWNS_32KHZPCM_WITHDAC's SND_init()) ever sets them - so without
 * this, the DAC bit-banging below is byte-correct but produces no
 * audible output at all, on real hardware same as in the emulator. */
#define YM_SOUND_MUTE        0x4D5
#define YM_SOUND_AUDIO       0x4EC
#define YM_SOUND_MUTE_FM_PCM_ON   0x03 /* Unmute both FM and PCM. */
#define YM_SOUND_AUDIO_ON        0x7F /* MUTE(bit6)=1=output; reserved bits written 1 per the databook's write-format row. */

/* Ring buffer the disc is streamed into. Power of two and a multiple of
 * 2048, at least twice the CD reader's 8-sector run so a new one can
 * be started while the previous one is still playing out. 32KB is about
 * a second of audio at FMT_PCM_STREAM_RATE - far more slack than the
 * few milliseconds a sector handshake actually needs, but it costs
 * nothing and it means a slow patch on the disc can never be heard. */
#define PCM_RING_BYTES      32768u

static uint8_t g_ring[PCM_RING_BYTES];
static uint32_t g_media_off;
static uint32_t g_size;
static uint8_t g_loaded;
static volatile uint8_t g_stop_requested;

static inline void ym_write(uint8_t reg, uint8_t val)
{
    outb(reg, YM_ADDR0);
    outb(val, YM_DATA0);
}

/* dac_pcm.c wraps its entire playback loop in _disable()/_enable() -
 * "Disable interrupt"/"Enable interrupt" in its own comments. Any IRQ
 * landing mid-loop (timer, keyboard, etc.) steals real time between two
 * DAC writes that this loop's pacing assumes are back-to-back ~30us
 * apart, which shows up directly as an audible stutter - copying
 * dac_pcm.c's interrupt-masking here as well, not just its I/O
 * sequence, is what makes the pacing gap-free.
 *
 * Only the masking half is copied, though: dac_pcm.c runs under DOS,
 * where interrupts were enabled to begin with and _enable() just puts
 * that back. This payload is the opposite - src/boot/head.S only ever
 * sets IF while briefly back in real mode for the BIOS memory queries,
 * and does `cli` again before re-entering protected mode, so protected
 * mode runs with interrupts masked from boot onwards (every driver
 * here polls; nothing is interrupt-driven). Its IDT also only has the
 * 20 CPU-exception vectors - nothing for the hardware IRQ vectors. So
 * an `sti` here doesn't restore a previous state, it enables interrupts
 * for the first time, with whatever became pending during playback
 * dispatching through an IDT that has no entry for it; that reliably
 * derailed the CPU (VM Aborted, wildly corrupted CS/SS) the moment the
 * music ended. Just stay masked - which is the state the rest of this
 * program already runs in. */
static inline void disable_interrupts(void)
{
    __asm__ __volatile__("cli");
}

int fmt_pcm_stream_load_file(const char *name)
{
    g_loaded = 0;
    if (fmt_media_find(name, &g_media_off, &g_size) != 0 || g_size == 0) {
        return -1;
    }
    g_loaded = 1;
    return 0;
}

void fmt_pcm_stream_stop(void)
{
    g_stop_requested = 1;
    outb(0x00, YM_SOUND_MUTE);
}

int fmt_pcm_stream_play_streaming(void)
{
    fmt_media_stream media;
    uint32_t play_pos = 0;
    uint32_t poll = 0;
    uint16_t deadline;
    uint8_t frac = 0;

    if (!g_loaded) {
        return -1;
    }

    g_stop_requested = 0;
    fmt_media_stream_init(&media, g_media_off, g_size, g_ring, PCM_RING_BYTES, 1);

    /* Prime the ring before the first sample goes out - nothing is
     * playing yet, so there is no harm in spinning the machine flat
     * out here. */
    while (media.fill_pos - play_pos < PCM_RING_BYTES) {
        if (media.state == FMT_MEDIA_STREAM_ERROR) {
            return -1;
        }
        if (media.state == FMT_MEDIA_STREAM_DONE) {
            break; /* file shorter than the ring */
        }
        fmt_media_stream_step(&media, play_pos, 64);
    }
    if (media.fill_pos == 0) {
        return -1;
    }

    outb(YM_SOUND_MUTE_FM_PCM_ON, YM_SOUND_MUTE);   /* Unmute FM (drives channel 6/DAC) and PCM. */
    outb(YM_SOUND_AUDIO_ON, YM_SOUND_AUDIO);        /* Enable audio output. */

    ym_write(YM_REG_DAC_ENABLE, 0x80); /* Channel 6 FM off, DAC on. */
    while (inb(YM_ADDR0) & YM_BUSY) {
        /* dac_pcm.c waits out the busy window from the register write
         * above before latching the data-register address. */
    }
    outb(YM_REG_DAC_DATA, YM_ADDR0);   /* Latch the DAC data register once - every sample below just hits YM_DATA0. */

    disable_interrupts();
    deadline = inw(FMT_DAC_FREERUN_TIMER);
    for (;;) {
        if (g_stop_requested) {
            goto done;
        }

        /* One disc byte per DAC write, 1:1 - the file is authored at
         * FMT_PCM_STREAM_RATE, so there is no resampling to do here. */
        outb(g_ring[play_pos & (PCM_RING_BYTES - 1u)], YM_DATA0);
        play_pos++;
        fmt_dac_advance(&deadline, &frac, FMT_DAC_PERIOD_Q4(FMT_PCM_STREAM_RATE));

        /* Refill until the next sample is due, one I/O operation at a time,
         * re-checking the clock between each. This used to spin on the
         * YM2612's busy flag instead and take that as the sample period,
         * which is where the old "about 32kHz, measured at 32.3kHz under
         * TOWNSEMU" figure came from. The flag is a register-write interlock
         * of about 11us, not a sample clock, so that gave a rate set by
         * however long the rest of this loop happened to take - different on
         * every machine. The free-running 1us counter at I/O 0x26 is an
         * actual clock; see dacout.h. */
        for (;;) {
            if ((int16_t)(inw(FMT_DAC_FREERUN_TIMER) - deadline) >= 0) {
                break;
            }
            fmt_media_stream_step(&media, play_pos, 1);
            if (g_stop_requested) {
                goto done;
            }
        }

        /* Checked occasionally rather than every sample - a failing
         * drive is not going to un-fail in 30ms.
         *
         * dac_pcm.c also polls the game pad here and quits on a button,
         * but that cannot be copied across as-is: bits 4-5 of 0x4D0 are
         * gated by the TRIG lines driven from 0x4D6, and their meaning
         * depends on the COM line from the same port (on a Marty pad,
         * COM low reads A/B, COM high reads L/START). dac_pcm.c runs
         * under TownsOS with the port already set up; this payload
         * never touches 0x4D6, so it reads whatever the BIOS happened
         * to leave there and can see a button that nobody pressed -
         * which stopped playback partway through the song. Reading the
         * pad properly means the COM0/COM1 strobe handshake in
         * fmt_pad_read(), whose spin loops have no place between two
         * DAC writes. There is nothing after this in the demo anyway,
         * so it simply streams until the drive gives up. */
        if (++poll >= 1024u) {
            poll = 0;
            if (media.state == FMT_MEDIA_STREAM_ERROR) {
                break;
            }
        }

        /* Underrun: the disc could not keep up. Hold the last sample
         * and let the machine catch up rather than dropping out of the
         * loop - with the ring sized as it is this should not happen,
         * but a click beats silence. */
        while (play_pos == media.fill_pos) {
            if (media.state == FMT_MEDIA_STREAM_ERROR ||
                media.state == FMT_MEDIA_STREAM_DONE) {
                goto done;
            }
            fmt_media_stream_step(&media, play_pos, 64);
        }
    }

done:
    ym_write(YM_REG_DAC_ENABLE, 0x00); /* Restore channel 6 to normal FM synthesis. */
    return 0;
}

int fmt_pcm_stream_play_buffer(const uint8_t *buffer, uint32_t size)
{
    uint32_t play_pos = 0;
    uint16_t deadline;
    uint8_t frac = 0;

    if (buffer == 0 || size == 0) {
        return -1;
    }

    g_stop_requested = 0;

    outb(YM_SOUND_MUTE_FM_PCM_ON, YM_SOUND_MUTE);
    outb(YM_SOUND_AUDIO_ON, YM_SOUND_AUDIO);

    ym_write(YM_REG_DAC_ENABLE, 0x80);
    while (inb(YM_ADDR0) & YM_BUSY) {
    }
    outb(YM_REG_DAC_DATA, YM_ADDR0);

    disable_interrupts();
    deadline = inw(FMT_DAC_FREERUN_TIMER);
    for (;;) {
        if (g_stop_requested) {
            break;
        }

        outb(buffer[play_pos], YM_DATA0);
        if (++play_pos == size) {
            play_pos = 0;
        }
        fmt_dac_advance(&deadline, &frac, FMT_DAC_PERIOD_Q4(FMT_PCM_STREAM_RATE));

        while ((int16_t)(inw(FMT_DAC_FREERUN_TIMER) - deadline) < 0) {
            if (g_stop_requested) {
                goto done;
            }
        }
    }

done:
    ym_write(YM_REG_DAC_ENABLE, 0x00);
    return 0;
}

int fmt_pcm_stream_play(void)
{
    return fmt_pcm_stream_play_streaming();
}
