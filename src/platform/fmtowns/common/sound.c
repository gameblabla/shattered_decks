#include "sound.h"
#include "io.h"

#define FMT_WAVERAM_WINDOW        ((volatile uint8_t *)0xF80000u)
#define FMT_WAVERAM_BANK_SIZE     4096u
#define FMT_SOUND_SILENCE_OFFSET  0x1f00u

#define FMT_SOUND_MUTE_PORT       0x4d5
#define FMT_SOUND_AUDIO_PORT      0x4ec
#define FMT_SOUND_FM_PCM_ON       0x03
#define FMT_SOUND_AUDIO_ON        0x7f

/* 04f8 is active-low: a zero bit plays the corresponding channel. */
static uint8_t g_channel_mask = 0xff;

static void select_channel(uint8_t channel)
{
    outb((uint8_t)(0xc0u | channel), FMT_SND_PCM_CTRL);
}

static void write_wave_byte(uint32_t address, uint8_t value)
{
    outb((uint8_t)((address >> 12) & 0x0fu), FMT_SND_PCM_CTRL);
    FMT_WAVERAM_WINDOW[address & (FMT_WAVERAM_BANK_SIZE - 1u)] = value;
}

static void set_loop_address(uint8_t channel, uint16_t address)
{
    select_channel(channel);
    outb((uint8_t)address, FMT_SND_PCM_LSL);
    outb((uint8_t)(address >> 8), FMT_SND_PCM_LSH);
}

void fmt_sound_init(void)
{
    uint32_t offset;
    uint8_t bank;
    uint8_t channel;

    /* The databook says wave RAM must only be accessed with PCM output off. */
    outb(0xff, FMT_SND_PCM_CH_ON_OFF);
    outb(0x00, FMT_SND_PCM_CTRL);
    g_channel_mask = 0xff;

    for (bank = 0; bank < 16u; bank++) {
        outb(bank, FMT_SND_PCM_CTRL);
        for (offset = 0; offset < FMT_WAVERAM_BANK_SIZE; offset++) {
            FMT_WAVERAM_WINDOW[offset] = 0xff;
        }
    }
    for (channel = 0; channel < FMT_SOUND_CHANNELS; channel++) {
        uint16_t base = (uint16_t)((uint16_t)channel * FMT_SOUND_SLOT_SIZE);
        uint16_t silence = (uint16_t)(base + FMT_SOUND_SILENCE_OFFSET);

        write_wave_byte(silence, 0x80); /* RF5C68 zero-amplitude sample. */
        write_wave_byte((uint16_t)(silence + 1u), 0xff);

        select_channel(channel);
        outb(FMT_SOUND_VOLUME_MAX, FMT_SND_PCM_ENV);
        outb(FMT_SOUND_PAN_CENTER, FMT_SND_PCM_PAN);
        outb(0x00, FMT_SND_PCM_FDL);
        outb(0x08, FMT_SND_PCM_FDH); /* increment 1.0 at 19.2 kHz */
        outb((uint8_t)silence, FMT_SND_PCM_LSL);
        outb((uint8_t)(silence >> 8), FMT_SND_PCM_LSH);
        outb((uint8_t)(base >> 8), FMT_SND_PCM_ST);
    }

    /* Both gates are active-high enables and reset muted on a bare-metal boot. */
    outb(FMT_SOUND_FM_PCM_ON, FMT_SOUND_MUTE_PORT);
    outb(FMT_SOUND_AUDIO_ON, FMT_SOUND_AUDIO_PORT);
    outb(0x80, FMT_SND_PCM_CTRL);
}

static int play(uint8_t channel, const uint8_t *data, uint32_t len, int loop)
{
    uint32_t base;
    uint32_t i;
    uint16_t loop_address;

    if (channel >= FMT_SOUND_CHANNELS || data == 0 || len == 0 ||
        len > FMT_SOUND_MAX_SAMPLE_SIZE) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        if (data[i] == 0x00 || data[i] == 0xff) {
            return -1;
        }
    }

    base = (uint32_t)channel * FMT_SOUND_SLOT_SIZE;

    /* Stop only the target channel, then stop the chip globally while the CPU
     * owns its wave-RAM window.  The other channel enable bits are preserved. */
    g_channel_mask |= (uint8_t)(1u << channel);
    outb(g_channel_mask, FMT_SND_PCM_CH_ON_OFF);
    outb(0x00, FMT_SND_PCM_CTRL);

    /* Upload a bank at a time rather than calling write_wave_byte() per byte.
     * Wave RAM is reached through a 4 KiB window whose bank is chosen by a
     * port write, and the bank only changes every 4096 bytes -- selecting it
     * per byte doubles the I/O for a sample that can be nearly 8 KiB long,
     * which is real time on a 386SX every time an effect is triggered. */
    {
        uint32_t done = 0;
        while (done <= len) {          /* <= : the 0xff end marker after data */
            uint32_t addr = base + done;
            uint32_t in_bank = addr & (FMT_WAVERAM_BANK_SIZE - 1u);
            uint32_t chunk = FMT_WAVERAM_BANK_SIZE - in_bank;
            uint32_t remaining = len - done;

            outb((uint8_t)((addr >> 12) & 0x0fu), FMT_SND_PCM_CTRL);
            if (chunk > remaining) {
                chunk = remaining;
            }
            for (i = 0; i < chunk; i++) {
                FMT_WAVERAM_WINDOW[in_bank + i] = data[done + i];
            }
            done += chunk;
            if (done == len) {
                /* End marker, in this bank if it fits or the next one round. */
                if (in_bank + chunk < FMT_WAVERAM_BANK_SIZE) {
                    FMT_WAVERAM_WINDOW[in_bank + chunk] = 0xff;
                } else {
                    write_wave_byte(base + len, 0xff);
                }
                break;
            }
        }
    }

    loop_address = loop ? (uint16_t)base :
                          (uint16_t)(base + FMT_SOUND_SILENCE_OFFSET);
    set_loop_address(channel, loop_address);
    select_channel(channel);
    outb((uint8_t)(base >> 8), FMT_SND_PCM_ST);

    g_channel_mask &= (uint8_t)~(1u << channel);
    outb(g_channel_mask, FMT_SND_PCM_CH_ON_OFF);
    outb(0x80, FMT_SND_PCM_CTRL);
    return 0;
}

int fmt_sound_play(uint8_t channel, const uint8_t *data, uint32_t len)
{
    return play(channel, data, len, 0);
}

int fmt_sound_play_loop(uint8_t channel, const uint8_t *data, uint32_t len)
{
    return play(channel, data, len, 1);
}

void fmt_sound_stop(uint8_t channel)
{
    if (channel < FMT_SOUND_CHANNELS) {
        g_channel_mask |= (uint8_t)(1u << channel);
        outb(g_channel_mask, FMT_SND_PCM_CH_ON_OFF);
    }
}

void fmt_sound_set_volume(uint8_t channel, uint8_t volume)
{
    if (channel < FMT_SOUND_CHANNELS) {
        select_channel(channel);
        outb(volume, FMT_SND_PCM_ENV);
    }
}

void fmt_sound_set_pan(uint8_t channel, uint8_t pan)
{
    if (channel < FMT_SOUND_CHANNELS) {
        select_channel(channel);
        outb(pan, FMT_SND_PCM_PAN);
    }
}

void fmt_sound_set_frequency(uint8_t channel, uint16_t increment)
{
    if (channel < FMT_SOUND_CHANNELS) {
        select_channel(channel);
        outb((uint8_t)increment, FMT_SND_PCM_FDL);
        outb((uint8_t)(increment >> 8), FMT_SND_PCM_FDH);
    }
}

void fmt_sound_set_sample_rate(uint8_t channel, uint32_t sample_rate)
{
    uint32_t increment;
    uint32_t max_rate = (0xffffu * FMT_SOUND_NATIVE_RATE) / 2048u;

    if (sample_rate >= max_rate) {
        increment = 0xffffu;
    } else {
        increment = (sample_rate * 2048u +
                     FMT_SOUND_NATIVE_RATE / 2u) /
                    FMT_SOUND_NATIVE_RATE;
    }
    fmt_sound_set_frequency(channel, (uint16_t)increment);
}
