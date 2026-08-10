#ifndef FMT_SOUND_H
#define FMT_SOUND_H

#include <stdint.h>

/* Ricoh RF5C68 wavetable PCM driver (FM TOWNS Technical Databook,
 * section 5.3).  The chip has eight channels and 64 KiB of shared wave RAM.
 * libfmt gives each channel an independent 8 KiB slot, including a small
 * silent tail used to make non-looping samples stop cleanly. */
#define FMT_SOUND_CHANNELS             8u
#define FMT_SOUND_SLOT_SIZE            8192u
#define FMT_SOUND_MAX_SAMPLE_SIZE      7935u
#define FMT_SOUND_NATIVE_RATE          19200u

#define FMT_SOUND_PAN_LEFT             0x0fu
#define FMT_SOUND_PAN_CENTER           0x88u
#define FMT_SOUND_PAN_RIGHT            0xf0u
#define FMT_SOUND_VOLUME_MAX           0xffu

/* Raw RF5C68 ports, exposed for programs which need functionality beyond the
 * helpers below. */
#define FMT_SND_PCM_ENV                0x4F0
#define FMT_SND_PCM_PAN                0x4F1
#define FMT_SND_PCM_FDL                0x4F2
#define FMT_SND_PCM_FDH                0x4F3
#define FMT_SND_PCM_LSL                0x4F4
#define FMT_SND_PCM_LSH                0x4F5
#define FMT_SND_PCM_ST                 0x4F6
#define FMT_SND_PCM_CTRL               0x4F7
#define FMT_SND_PCM_CH_ON_OFF          0x4F8

/* Stops all channels, clears wave RAM, initializes all channel registers, and
 * enables the TOWNS PCM output path. */
void fmt_sound_init(void);

/* Uploads RF5C68-format 8-bit PCM to channel's private slot and starts it.
 * 0xff is the chip's loop marker and 0x00 is invalid sample data, so neither
 * may occur in data.  Returns 0, or -1 for a bad channel/pointer/length. */
int fmt_sound_play(uint8_t channel, const uint8_t *data, uint32_t len);

/* As above, but loops back to the beginning instead of falling into silence. */
int fmt_sound_play_loop(uint8_t channel, const uint8_t *data, uint32_t len);

/* Stops one channel.  Other channels continue playing. */
void fmt_sound_stop(uint8_t channel);

/* Per-channel RF5C68 controls.  PAN is RIGHT in bits 7..4 and LEFT in bits
 * 3..0; 0x0f is hard left, 0xf0 hard right, and the documented equal-power
 * center value is 0x88. */
void fmt_sound_set_volume(uint8_t channel, uint8_t volume);
void fmt_sound_set_pan(uint8_t channel, uint8_t pan);

/* Sets the 5.11 fixed-point address increment directly, or derives it from a
 * desired sample rate using the documented 19.2 kHz native rate.  Rates above
 * the representable range are clamped. */
void fmt_sound_set_frequency(uint8_t channel, uint16_t increment);
void fmt_sound_set_sample_rate(uint8_t channel, uint32_t sample_rate);

#endif
