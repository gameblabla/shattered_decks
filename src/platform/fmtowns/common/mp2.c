#include "mp2.h"
#include "mp2_synth_table.h"
#include "mp2_synth_window_ref.h"

/* Host-side decoder tests run as x86-64 binaries.  The Towns build links
 * the 386 GAS implementation in mp2_fast.S; this fallback keeps the exact
 * interface available to the host test without contaminating target code. */
#if !defined(__i386__)
int32_t fmt_mp2_mul16(int16_t a, int16_t b)
{
    return (int32_t)a * b;
}
#endif

#ifdef FMT_MP2_DEBUG
unsigned fmt_mp2_debug_bits;
unsigned fmt_mp2_debug_q;
unsigned fmt_mp2_debug_calls;
unsigned fmt_mp2_debug_requested;
uint8_t fmt_mp2_debug_alloc[30];
uint8_t fmt_mp2_debug_raw[30];
#endif

typedef struct { const uint8_t *p; unsigned bits, limit; } bit_reader;

/* ISO/IEC 13818-3, Table B.2 (MPEG-2 low sampling-frequency Layer II).
 * Entry: high nibble = allocation lookup row, low nibble = bit count. */
static const uint8_t alloc_desc[32] = {
    0x45,0x45,0x45,0x45, 0x34,0x34,0x34,0x34,0x34,0x34,0x34,
    0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,
    0x24,0x24,0x24,0x24,0x24,0x24, 0,0
};
static const uint8_t alloc_lut[6][16] = {
    {0,1,2,17}, {0,1,2,3,4,5,6,17},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,17},
    {0,1,3,5,6,7,8,9,10,11,12,13,14,15,16,17},
    {0,1,2,4,5,6,7,8,9,10,11,12,13,14,15,17},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}
};
/* 2^(1-i/3), Q13.  The tiny tail naturally becomes silence at 8-bit. */
static const uint16_t scale_q13[64] = {
    16384,13004,10321,8192,6502,5161,4096,3251,2580,2048,1625,1290,
    1024,813,645,512,406,323,256,203,161,128,102,81,64,51,40,32,
    25,20,16,13,10,8,6,5,4,3,3,2,2,1,1,1,1,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0
};

static int getbits(bit_reader *r, unsigned n)
{
    unsigned v = 0;
#ifdef FMT_MP2_DEBUG
    fmt_mp2_debug_requested += n;
#endif
    /* The final Layer-II codeword can straddle the packet end by one zero
     * stuffing bit.  Keep that bit implicit instead of reading into the next
     * CD frame; anything larger is a malformed frame. */
    if (n > r->limit - r->bits + 1u) return -1;
    while (n--) {
        v <<= 1;
        if (r->bits < r->limit)
            v |= (r->p[r->bits >> 3] >> (7 - (r->bits & 7))) & 1;
        r->bits++;
    }
    return (int)v;
}

unsigned fmt_mp2_frame_size(const uint8_t *f)
{
    static const uint8_t bitrate[16] =
        {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    unsigned br, rate_index, padding;
    if (f[0] != 0xff || (f[1] & 0xf6) != 0xf4) return 0; /* MPEG-2, Layer II */
    br = bitrate[(f[2] >> 4) & 15];
    rate_index = (f[2] >> 2) & 3;
    padding = (f[2] >> 1) & 1;
    if (br != 32 || rate_index != 2 || ((f[3] >> 6) & 3) != 3) return 0;
    return 288u + padding;
}

static int requant(bit_reader *r, unsigned q, unsigned sf, int out[3])
{
    static const uint16_t qscale[17] =
        {16384,10922,8192,6553,4096,2048,1024,512,256,128,64,32,16,8,4,2,1};
    static const uint16_t qbias[17] =
        {1,2,3,4,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767};
    unsigned n, i;
    int code;
#ifdef FMT_MP2_DEBUG
    fmt_mp2_debug_q = q;
    fmt_mp2_debug_calls++;
#endif
    /* Quantizer indices follow the mp3play/ISO order: 3 and 5 steps
     * are grouped, 7 steps is direct 3-bit, and 9 steps is grouped.
     * Keeping the 7/9 entries distinct matters: swapping them consumes
     * one extra bit for each affected sample triplet. */
    if (q > 16) return -1;
    if (q == 0 || q == 1 || q == 3) {
        n = q == 0 ? 3 : (q == 1 ? 5 : 9);
        code = getbits(r, q == 0 ? 5 : (q == 1 ? 7 : 10));
        if (code < 0) return -1;
        for (i = 0; i < 3; i++) {
            int d = code % (int)n;
            code /= (int)n;
            int v = ((int)qbias[q] - d) * (int)qscale[q];
            out[i] = fmt_mp2_mul16((int16_t)v, (int16_t)scale_q13[sf]) >> 15;
        }
        return 0;
    }
    n = q == 2 ? 3u : q; /* q=2 is the direct 3-bit, seven-step quantizer. */
    for (i = 0; i < 3; i++) {
        code = getbits(r, n);
        if (code < 0) return -1;
        code = ((int)qbias[q] - code) * (int)qscale[q];
        out[i] = fmt_mp2_mul16((int16_t)code, (int16_t)scale_q13[sf]) >> 15;
    }
    return 0;
}

/* The validated kjmp2 factorisation writes 64 values per subband group into
 * a duplicated 1024-sample V bank. */
static int synth_buf[2048];
static unsigned synth_write;

#define FASTCALL
#include "mp2_synth_ref.h"
#undef FASTCALL

static void synth8(const int sb[32], uint8_t *dst)
{
    int stride[51];
    unsigned n, tap;

    for (n = 0; n < 17; n++) {
        stride[n * 3u] = sb[n];
        stride[n * 3u + 1u] = stride[n * 3u + 2u] = 0;
    }
    synth_write = (synth_write - 64u) & 1023u;
    synth_dct32_to_v(synth_buf, (int)synth_write, stride);
    for (n = 0; n < 32; n++) {
        const int *v = &synth_buf[synth_write + n];
        int sum = 0;
        int pcm;
        for (tap = 0; tap < 16; tap++)
            sum -= (v[(tap << 6) + ((tap & 1u) ? 32u : 0u)] *
                    mp2_synth_window[(tap << 5) + n] + 32) >> 6;
        /* Match the reference's x1.5 PSG gain, converted from signed
         * 16-bit-ish synthesis units to the YM2612's unsigned 8-bit DAC. */
        pcm = 128 + ((sum * 3) >> 15);
        dst[n] = (uint8_t)(pcm < 0 ? 0 : (pcm > 255 ? 255 : pcm));
    }
}

int fmt_mp2_decode_frame(const uint8_t *frame, unsigned size,
                         uint8_t pcm[FMT_MP2_SAMPLES_PER_FRAME])
{
    bit_reader r;
    uint8_t alloc[30], scfsi[30], sf[3][30];
    unsigned sb, part, group, samples = 0, fs = fmt_mp2_frame_size(frame);
    int values[3], band[3][32];
    if (fs == 0 || size < fs) return -2;
    /* MP3play's bit reader keeps a small look-ahead window.  Some valid
     * encoders leave the final quantizer codeword in that window, so the
     * caller supplies a few following bytes while frame ownership remains
     * exactly `fs` bytes. */
    r.p = frame; r.bits = 32; r.limit = size * 8;
    if (!(frame[1] & 1) && getbits(&r, 16) < 0) return -3;
    for (sb = 0; sb < 30; sb++) {
        unsigned d = alloc_desc[sb];
        int raw = getbits(&r, d >> 4);
        if (raw < 0) return -4;
#ifdef FMT_MP2_DEBUG
        fmt_mp2_debug_raw[sb] = (uint8_t)raw;
#endif
        alloc[sb] = alloc_lut[d & 15][raw];
#ifdef FMT_MP2_DEBUG
        fmt_mp2_debug_alloc[sb] = alloc[sb];
#endif
    }
    for (sb = 0; sb < 30; sb++) if (alloc[sb]) {
        int v = getbits(&r, 2); if (v < 0) return -5; scfsi[sb] = (uint8_t)v;
    }
    for (sb = 0; sb < 30; sb++) if (alloc[sb]) {
        int a = getbits(&r, 6);
        if (a < 0) return -6;
        sf[0][sb] = (uint8_t)a;
        switch (scfsi[sb]) {
        case 0:
            a = getbits(&r, 6); if (a < 0) return -7; sf[1][sb] = (uint8_t)a;
            a = getbits(&r, 6); if (a < 0) return -8; sf[2][sb] = (uint8_t)a;
            break;
        case 1:
            sf[1][sb] = sf[0][sb];
            a = getbits(&r, 6); if (a < 0) return -8; sf[2][sb] = (uint8_t)a;
            break;
        case 2:
            sf[1][sb] = sf[2][sb] = sf[0][sb];
            break;
        default:
            a = getbits(&r, 6); if (a < 0) return -7;
            sf[1][sb] = sf[2][sb] = (uint8_t)a;
            break;
        }
    }
    for (part = 0; part < 3; part++) for (group = 0; group < 4; group++) {
        for (sb = 0; sb < 30; sb++) {
            if (!alloc[sb]) values[0] = values[1] = values[2] = 0;
            else if (requant(&r, alloc[sb] - 1, sf[part][sb], values)) {
#ifdef FMT_MP2_DEBUG
                fmt_mp2_debug_bits = r.bits;
#endif
                return -1;
            }
            band[0][sb] = values[0]; band[1][sb] = values[1]; band[2][sb] = values[2];
        }
        for (; sb < 32; sb++) band[0][sb] = band[1][sb] = band[2][sb] = 0;
        synth8(band[0], pcm + samples); samples += 32;
        synth8(band[1], pcm + samples); samples += 32;
        synth8(band[2], pcm + samples); samples += 32;
    }
    return samples == FMT_MP2_SAMPLES_PER_FRAME ? 0 : -1;
}
