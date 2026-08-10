#ifndef FMT_MP2_H
#define FMT_MP2_H

#include <stdint.h>

/* Decoder for the deliberately narrow on-disc format used by this project:
 * MPEG-2 Audio Layer II, mono, 16000 Hz, 32 kbit/s.  A frame is 288 bytes
 * (or 289 when padded) and expands to 1152 unsigned 8-bit PCM samples.
 * Unlike MPEG-2 Layer III, Layer II retains 1152 samples per frame. */
#define FMT_MP2_SAMPLES_PER_FRAME 1152u

/* Returns the compressed frame size, or zero when the header is not our
 * supported stream.  `frame` must point at at least four bytes. */
unsigned fmt_mp2_frame_size(const uint8_t *frame);

/* Decode one supported frame plus optional bounded look-ahead bytes.  This Towns version uses a
 * 16-bit, intentionally low-precision synthesis bank: no 32x32->64 imul
 * is used anywhere in Layer-II requantisation or synthesis. */
int fmt_mp2_decode_frame(const uint8_t *frame, unsigned size,
                         uint8_t pcm[FMT_MP2_SAMPLES_PER_FRAME]);

/* GCC/GAS 386 helper: signed 16x16 -> signed 32 fixed-point product. */
int32_t fmt_mp2_mul16(int16_t a, int16_t b);

#endif
