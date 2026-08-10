#ifndef FMT_MBV_H
#define FMT_MBV_H

#include <stdint.h>

/*
 * MBV - "Marty Block Video", the codec described in video.txt.
 *
 * A frame is an array of 8x8 macroblocks, each either left alone, copied from
 * somewhere else in the previous frame, filled with one palette index, or
 * split into four 4x4 blocks carrying 1, 2, 4, 8 or 16 palette indices.  Every
 * value in the stream is already a palette index for the machine's 256-colour
 * mode, so the decoder never converts a colour space, never touches a bit
 * reader, and spends nearly all of its time in memcpy/memset-shaped loops with
 * a couple of small table lookups.  That is the point: this has to keep up on
 * a 16MHz 386SX while the same CPU is also feeding a DAC every 62.5us and
 * pulling sectors off the CD by PIO.
 *
 * Everything is byte-aligned and little-endian.  The opcode stream is walked
 * with a single forward pointer.
 *
 * See docs/MBV_FORMAT.md for the on-disc layout in full, and tools/mbvenc.c
 * for the encoder.
 *
 * In-place reconstruction
 * -----------------------
 * There is exactly one frame buffer.  SKIP is free (the pixels are simply not
 * touched), and MOTION reads the same buffer it writes, so a vector pointing
 * up or left may source pixels that this very frame has already updated.  That
 * is deliberate, not an accident to be worked around: the encoder reconstructs
 * through the identical primitives below, in the identical order, so what it
 * measured is exactly what the decoder produces.  Both sides therefore agree
 * bit-for-bit and no drift can accumulate - see tests/mbv_roundtrip_test.c,
 * which checks that on real encoded output.
 */

#define MBV_MAGIC               "MBV1"
#define MBV_HEADER_BYTES        32u
#define MBV_CHUNK_HEADER_BYTES  8u

/* Frame types (chunk header byte 6). */
#define MBV_FTYPE_INTER         0u
#define MBV_FTYPE_KEY           1u   /* full 256-entry palette, no SKIP/MOTION */

/* Header flags. */
#define MBV_FLAG_AUDIO          0x01u

/* Macroblock opcodes (8x8). */
#define MBV_MB_SKIP_MAX         0x40u /* 0x00..0x3F: skip (op+1) macroblocks */
#define MBV_MB_MOTION           0x40u /* + 1 vector byte */
#define MBV_MB_FILL             0x41u /* + 1 palette index */
#define MBV_MB_SPLIT            0x42u /* + four 4x4 blocks */
#define MBV_MB_RAW              0x43u /* + 64 palette indices, row-major */

/* Block opcodes (4x4), only valid inside MBV_MB_SPLIT. */
#define MBV_B_SKIP              0u   /* nothing follows */
#define MBV_B_FILL              1u   /* + 1 palette index */
#define MBV_B_CLR2              2u   /* + 2 indices, + 2 bytes of 1bpp map */
#define MBV_B_CLR4              3u   /* + 4 indices, + 4 bytes of 2bpp map */
#define MBV_B_CLR8              4u   /* + 8 indices, + 6 bytes of 3bpp map */
#define MBV_B_RAW               5u   /* + 16 indices, row-major */
#define MBV_B_MOTION            6u   /* + 1 vector byte */
#define MBV_B_COUNT             7u

/* Motion vectors: one byte, high nibble dx, low nibble dy, both biased by 8,
 * so each component covers -8..+7.  video.txt's "one byte total". */
#define MBV_MV_MIN              (-8)
#define MBV_MV_MAX              (7)
#define MBV_MV_PACK(dx, dy)     ((uint8_t)((((dx) + 8) << 4) | (((dy) + 8) & 15)))
#define MBV_MV_DX(v)            ((int)((v) >> 4) - 8)
#define MBV_MV_DY(v)            ((int)((v) & 15) - 8)

/* Encoded size in bytes of each block type, opcode byte included. */
#define MBV_B_BYTES(op)         (mbv_block_bytes[op])
extern const uint8_t mbv_block_bytes[MBV_B_COUNT];

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps_q8;        /* frames per second, 8.8 fixed point */
    uint8_t  bpp;           /* 8 */
    uint8_t  flags;         /* MBV_FLAG_* */
    uint32_t frame_count;
    uint32_t audio_rate;    /* Hz; samples are unsigned 8-bit mono */
    uint32_t max_chunk;     /* largest chunk in the file, header included */
    uint32_t audio_samples; /* total across the file */
} fmt_mbv_info;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t stride;        /* bytes per line of `frame`; >= width, multiple of 4 */
    uint16_t mb_w;          /* macroblocks across */
    uint16_t mb_h;
    uint8_t *frame;         /* the one persistent reconstruction, stride*height */
    uint8_t  pal[768];      /* RGB888 triplets, current palette */
    uint8_t  pal_dirty;     /* set by a chunk that changed the palette */
    uint32_t frames;        /* chunks decoded so far */
} fmt_mbv_dec;

/*----------------------------------------------------------------------
 * Reconstruction primitives.
 *
 * Shared with the encoder so that both sides cannot drift apart: the encoder
 * builds its reference picture by calling exactly these, in stream order.
 * `dst` points at the block's top-left pixel; `stride` is the frame pitch.
 *--------------------------------------------------------------------*/

void fmt_mbv_fill(uint8_t *dst, int stride, int n, uint8_t color);
void fmt_mbv_raw(uint8_t *dst, int stride, int n, const uint8_t *px);
void fmt_mbv_clr2(uint8_t *dst, int stride, const uint8_t *colors, uint16_t map);
void fmt_mbv_clr4(uint8_t *dst, int stride, const uint8_t *colors, const uint8_t *map);
void fmt_mbv_clr8(uint8_t *dst, int stride, const uint8_t *colors, const uint8_t *map);

/* Copies an n x n block from (x+dx, y+dy) to (x, y) within the same frame,
 * one byte at a time in row-major order.  The overlap that produces is part
 * of the format's definition - see the note at the top of this file. */
void fmt_mbv_motion(uint8_t *frame, int stride, int x, int y, int dx, int dy, int n);

/*----------------------------------------------------------------------
 * Decoder
 *--------------------------------------------------------------------*/

/* Reads the 32-byte file header.  Returns 0, or -1 if it is not an MBV1
 * stream this build can play. */
int fmt_mbv_parse_header(const uint8_t *hdr, fmt_mbv_info *info);

/* `framebuf` must hold stride*height bytes and stay valid for the whole
 * playback; it is the persistent reconstruction.  Cleared to index 0. */
void fmt_mbv_dec_init(fmt_mbv_dec *d, const fmt_mbv_info *info,
                      uint8_t *framebuf, uint16_t stride);

/* Total length of the chunk starting at `chunk`, or 0 if fewer than
 * MBV_CHUNK_HEADER_BYTES bytes are available yet. */
uint32_t fmt_mbv_chunk_bytes(const uint8_t *chunk, uint32_t avail);

/* Decodes one whole chunk: applies any palette change (setting pal_dirty),
 * hands back the audio it carries, and reconstructs the frame in place.
 * Returns 0, or -1 if the chunk is malformed or overruns `len`. */
int fmt_mbv_decode_chunk(fmt_mbv_dec *d, const uint8_t *chunk, uint32_t len,
                         const uint8_t **audio, uint16_t *audio_len);

#endif
