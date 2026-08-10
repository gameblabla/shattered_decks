/******************************************************************************
** kjmp2_fast -- V810-oriented MPEG Audio Layer II decoder for PC-FX PSG       **
** Derived from kjmp2 1.1 by Martin J. Fiedler.                               **
******************************************************************************/

#ifndef FMT_KJMP2_FAST_H
#define FMT_KJMP2_FAST_H

#include <stdint.h>

#define KJMP2_MAX_FRAME_SIZE    1440u
#define KJMP2_SAMPLES_PER_FRAME 1152u

/* One output tick for the paired-channel PSG10 renderer.
 *   bits  0..9   = left/mono 10-bit code, high bits at [9..5], low bits [4..0]
 *   bits 16..25  = right 10-bit code, same packing
 * Mono streams use only bits 0..9 and are rendered with two PSG channels.
 * Stereo streams use both fields and are rendered with four PSG channels.
 */
typedef uint32_t kjmp2v_psg10_sample_t;

typedef uint16_t kjmp2v_psg10_code_t;

#define KJMP2V_PSG10_CODE_HIGH(x) ((uint8_t)(((x) >> 5) & 31u))
#define KJMP2V_PSG10_CODE_LOW(x)  ((uint8_t)((x) & 31u))
#define KJMP2V_PSG10_LEFT(x)      ((kjmp2v_psg10_code_t)((x) & 0x03FFu))
#define KJMP2V_PSG10_RIGHT(x)     ((kjmp2v_psg10_code_t)(((x) >> 16) & 0x03FFu))
#define KJMP2V_PSG10_PACK_MONO(l) ((uint32_t)((l) & 0x03FFu))
#define KJMP2V_PSG10_PACK_STEREO(l,r) (((uint32_t)((l) & 0x03FFu)) | (((uint32_t)((r) & 0x03FFu)) << 16))

/* Compile-time gain.  Default is x1.5, chosen from the full-song 16k mono
 * reference: it increases level without clipping that asset and costs only one
 * shift+add in the output hot path.
 */
#ifndef MP2PSG_GAIN_NUM
#define MP2PSG_GAIN_NUM 3
#endif
#ifndef MP2PSG_GAIN_SHIFT
#define MP2PSG_GAIN_SHIFT 1
#endif

/* kjmp2v_context_t: decoder state.  Allocate one per stream.
 *
 * V is the synthesis delay bank.  It is 16-bit, not 32-bit: every value
 * written into it is a DCT output already scaled down by 64, and probing the
 * full 306-second 16 kHz asset never produced a magnitude above 264 - nine
 * significant bits, against int16's fifteen.  Halving the type halves both
 * the buffer (16 KiB -> 8 KiB) and, on the 386SX's 16-bit external bus, the
 * bus cycles every one of the ~30k V reads per frame costs.  A 32-bit load
 * needs two bus cycles there; a 16-bit load needs one. */
typedef struct _kjmp2_context {
    int id;
    short V[2][2048];
    int Voffs;
} kjmp2v_context_t;

void kjmp2v_init(kjmp2v_context_t *mp2);
int kjmp2v_get_sample_rate(const unsigned char *frame);

/* Decode one complete MP2 frame directly into packed paired-PSG samples.
 * Mono output: 1152 words with only the low 10-bit field used.
 * Stereo output: 1152 words with left in bits 0..9 and right in bits 16..25.
 * Passing out == NULL parses only the frame header and returns frame size.
 */
unsigned long kjmp2v_decode_frame_psg10(
    kjmp2v_context_t *mp2,
    const unsigned char *frame,
    kjmp2v_psg10_sample_t *out
);

/* Decode one frame of the FM TOWNS asset profile - MPEG-2 LSF, 16 kHz, mono,
 * 32 kbit/s - straight to 1152 unsigned 8-bit samples, in the exact form the
 * YM2612 channel-6 DAC register takes.  Returns the frame size in bytes, or 0
 * if the frame is not that profile (callers wanting the general decoder should
 * use kjmp2v_decode_frame_psg10 instead).  Passing out == NULL parses only the
 * header.
 *
 * This is the fast path this project actually ships.  It differs from the
 * PSG10 renderer in more than the output type: it applies the synthesis window
 * with ten taps rather than sixteen, accumulates the taps without a per-tap
 * rounding shift, and folds the output gain, DC offset and both scaling shifts
 * into the window table, so one tap costs one multiply and one add.  See
 * synth_window10_pcm8() for the measurements behind each of those. */
unsigned long kjmp2v_decode_frame_pcm8(
    kjmp2v_context_t *mp2,
    const unsigned char *frame,
    unsigned char *out
);

#endif
