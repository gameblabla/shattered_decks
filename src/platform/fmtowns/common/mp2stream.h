#ifndef FMT_MP2STREAM_H
#define FMT_MP2STREAM_H

#include <stdint.h>

/* CD-streamed MPEG-2 Layer II (mono 16 kHz, 32 kbit/s) playback through
 * the YM2612 DAC.  Like pcmstream, playback is blocking/polled - but unlike
 * pcmstream the sample rate comes from the TOWNS free-running timer rather
 * than the YM2612 busy flag, and the decoder services the DAC from inside its
 * own inner loops.  See mp2stream.c and dacout.h. */
int fmt_mp2_stream_load_file(const char *name);
int fmt_mp2_stream_play_streaming(void);
void fmt_mp2_stream_stop(void);

/* Samples whose deadline passed with nothing decoded to play - zero on a
 * machine fast enough to keep up.  Useful as the single number to check when
 * tuning the decoder against a slower CPU. */
uint32_t fmt_mp2_stream_underruns(void);

#endif
