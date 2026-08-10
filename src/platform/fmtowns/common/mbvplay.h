#ifndef FMT_MBVPLAY_H
#define FMT_MBVPLAY_H

#include <stdint.h>

/*
 * Full-motion video from the data track: MBV frames (src/common/mbv.h) into a
 * 256x240 8bpp page, with the interleaved 16kHz mono soundtrack going out
 * through the YM2612 channel-6 DAC.
 *
 * Four things have to share one 16MHz 386SX and only one of them has a hard
 * deadline:
 *
 *   - a sample is due at the DAC every 62.5us.  That is the deadline, met by
 *     polling fmt_dac_tick() from inside everything else (dacout.h), never by
 *     interrupts - nothing in this payload is interrupt-driven.
 *   - a frame has to be decoded every ~83ms.  The decoder ticks the DAC once
 *     per macroblock, so decoding happens in the gaps between samples rather
 *     than instead of them.
 *   - the decoded frame has to reach VRAM.  That is 61440 bytes; the blit in
 *     mbv_blit.h moves it as 32-bit words down two linear pointers and ticks
 *     every 64 bytes.
 *   - the drive has to be kept reading, which happens between frames and
 *     from every wait in the loop.
 *
 * Audio is the clock.  Each chunk carries exactly the samples belonging to its
 * frame, so a frame is shown when the previous frame's audio has played out,
 * and picture and sound cannot drift apart however long a particular frame
 * takes to decode: a slow frame stretches time for both.
 */

/* Looks the file up in the ISO9660 root directory and reads its header.
 * Returns 0, or -1 if it is missing or is not a playable MBV1 stream. */
int fmt_mbv_stream_load_file(const char *name);

/* Plays the loaded file through to its end.  Sets the video mode, drives the
 * palette, and returns 0 when the stream is finished (or -1 on a drive or
 * stream error).  Leaves the last frame on screen and the DAC stopped. */
int fmt_mbv_stream_play(void);

/* Asks a running fmt_mbv_stream_play() to return early. */
void fmt_mbv_stream_stop(void);

/* Samples the DAC could not be supplied in time - 0 if playback kept up. */
uint32_t fmt_mbv_stream_underruns(void);

#endif
