#ifndef FMT_PCMSTREAM_H
#define FMT_PCMSTREAM_H

#include <stdint.h>

/*
 * CD PCM playback via the YM2612 channel-6 DAC - separate from
 * fmt_sound_play() (sound.[ch], RF5C68 wave-table playback) and from
 * CD-DA (audio-track playback handled entirely by the CD-ROM drive's
 * own DAC, never touching the CPU). This is a raw unsigned 8-bit mono
 * PCM *file* on the data track, decoded/output entirely in software by
 * bit-banging the YM2612's channel-6 DAC one sample at a time - the
 * same mechanism FMTOWNS_32KHZPCM_WITHDAC/src/dac_pcm.c uses, ported
 * onto this project's outb()/inb().
 *
 * fmt_pcm_stream_play_streaming() streams the loaded file straight off the
 * disc: it
 * primes a ring buffer, then plays out of it while advancing a
 * non-blocking CD reader (fmt_cdrom_stream_step(), cdrom.[ch]) inside
 * the same loop, during the ~30us the YM2612 is busy after each sample.
 * Nothing is ever preloaded, so a file of any length plays - the
 * previous version read the whole thing into a 512KB RAM buffer first,
 * which capped playback at about 47 seconds and spent the whole load
 * time silent.
 *
 * The playback loop masks interrupts (cli) around its busy-wait pacing,
 * matching dac_pcm.c's own _disable() - an IRQ landing between two DAC
 * writes would otherwise steal real time the pacing loop assumes is
 * going entirely to the busy window. It deliberately does not unmask
 * them afterwards; see disable_interrupts() in pcmstream.c for why.
 */

/* Sample rate the PCM file must be authored at.
 *
 * This is now a free choice rather than whatever the hardware happened to
 * give: the loop paces each write against the TOWNS free-running 1us counter
 * at I/O 0x26 (see dacout.h), so the file plays at exactly this rate on any
 * machine.  It previously spun on the YM2612's busy flag and took that as the
 * sample period, which is where the old "about 32kHz, measured at 32.3kHz
 * under TOWNSEMU" came from - the flag is a register-write interlock of about
 * 11us, not a sample clock, so the resulting rate depended on how long the
 * rest of the loop took on the machine in question.
 *
 * 32000 is kept because the shipped asset is authored at it, and because one
 * file sample is still one DAC write - no resampling on the Towns side.
 *
 * Author with: sox <input> -r 32000 -c 1 -b 8 -e unsigned-integer \
 *                  -t raw CD/MUSIC.PCM gain -n -1 */
#define FMT_PCM_STREAM_RATE     32000u /* unsigned 8-bit mono samples/sec */

/* Looks up `name` on the CD (via iso9660.c).  Loading only records its
 * location; it does not start CD reads or change the sound hardware. */
int fmt_pcm_stream_load_file(const char *name);

/* Streams the loaded CD file through the YM2612 DAC, looping at the end of
 * the file. Blocking, and does
 * not return on its own - there's no interrupt-driven playback in this
 * environment, so nothing else runs while this is going (same as
 * dac_pcm.c's own design), and the only way out is the drive reporting a
 * failure or fmt_pcm_stream_stop(). Returns 0 when stopped, -1 if no file
 * was loaded or nothing could be read from it. */
int fmt_pcm_stream_play_streaming(void);

/* Plays an unsigned 8-bit mono PCM buffer already in CPU RAM. The caller
 * retains ownership of `buffer` and must keep it valid for the (blocking)
 * duration of playback. The buffer loops at `size`, using the same YM2612
 * DAC pacing as the streaming variant. Returns 0 when stopped, or -1 when
 * passed a null or empty buffer. */
int fmt_pcm_stream_play_buffer(const uint8_t *buffer, uint32_t size);

/* Backwards-compatible name for fmt_pcm_stream_play_streaming(). */
int fmt_pcm_stream_play(void);

/* Request that playback stop and mute the PCM/FM output immediately. */
void fmt_pcm_stream_stop(void);

#endif
