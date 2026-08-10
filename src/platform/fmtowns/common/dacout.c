#include "dacout.h"

/* See dacout.h for the design; this file is only the state and the
 * start/stop sequence, which is the one part that is not on a hot path. */

fmt_dac_state fmt_dac;

/* Databook table I-5-42 (audio register 0x4EC) and I-5-43 (FM/PCM mute
 * register 0x4D5): both are active-high enables that reset to 0, and nothing
 * else in this boot path sets them - without these two writes the DAC
 * bit-banging is byte-correct and completely silent. */
#define SOUND_MUTE_FM_PCM_ON 0x03
#define SOUND_AUDIO_ON       0x7F

void fmt_dac_start(void)
{
    fmt_dac.buf = 0;
    fmt_dac.pos = 0;
    fmt_dac.len = 0;
    fmt_dac.frac = 0;
    fmt_dac.last = 128;          /* DAC midpoint: silence, not a click */
    fmt_dac.underruns = 0;

    outb(SOUND_MUTE_FM_PCM_ON, FMT_DAC_SOUND_MUTE);
    outb(SOUND_AUDIO_ON, FMT_DAC_SOUND_AUDIO);

    outb(FMT_DAC_YM_REG_DAC_ENABLE, FMT_DAC_YM_ADDR0);
    outb(0x80, FMT_DAC_YM_DATA0);          /* channel 6 FM off, DAC on */
    while (inb(FMT_DAC_YM_ADDR0) & FMT_DAC_YM_BUSY) {
        /* Wait out the register write before latching the data register.
         * This is the one place the busy flag is the right thing to poll:
         * it is a register-write interlock, which is what it is for. */
    }
    outb(FMT_DAC_YM_REG_DAC_DATA, FMT_DAC_YM_ADDR0);
    /* Every sample from here on is a single write to FMT_DAC_YM_DATA0. */

    fmt_dac.deadline = inw(FMT_DAC_FREERUN_TIMER);
    fmt_dac.active = 1;
}

void fmt_dac_submit(const uint8_t *buf, uint16_t len)
{
    fmt_dac.buf = buf;
    fmt_dac.len = len;
    fmt_dac.pos = 0;
}

void fmt_dac_stop(void)
{
    fmt_dac.active = 0;
    outb(FMT_DAC_YM_REG_DAC_ENABLE, FMT_DAC_YM_ADDR0);
    outb(0x00, FMT_DAC_YM_DATA0);          /* channel 6 back to FM synthesis */
    outb(0x00, FMT_DAC_SOUND_MUTE);
}
