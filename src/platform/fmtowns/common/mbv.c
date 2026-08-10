#include "mbv.h"

/*
 * MBV frame decoder.  See mbv.h for the format and docs/MBV_FORMAT.md for the
 * on-disc layout.  This file is built twice: once for the Marty payload, and
 * once on the host, where tools/mbvenc.c links it to reconstruct exactly what
 * the machine will and tests/mbv_roundtrip_test.c checks the two agree.
 *
 * With FMT_MBV_DAC_TICK defined, the macroblock loop services the YM2612 DAC
 * between blocks, the same arrangement kjmp2_fast.c uses: a frame takes tens
 * of milliseconds to decode and a sample is due every 62.5us, so a decoder
 * that runs to completion between two DAC writes stalls the audio for as long
 * as it runs.  One macroblock is well under a sample period of work.
 */
#ifdef FMT_MBV_DAC_TICK
#include "dacout.h"
#define MBV_TICK() fmt_dac_tick()
#else
#define MBV_TICK() ((void)0)
#endif

/* Pixels are written four at a time.  The frame buffer is a byte array, so
 * say so explicitly rather than relying on the compiler not to notice. */
typedef uint32_t mbv_u32 __attribute__((may_alias));

const uint8_t mbv_block_bytes[MBV_B_COUNT] = {
    [MBV_B_SKIP]   = 1,
    [MBV_B_FILL]   = 2,
    [MBV_B_CLR2]   = 5,
    [MBV_B_CLR4]   = 9,
    [MBV_B_CLR8]   = 15,
    [MBV_B_RAW]    = 17,
    [MBV_B_MOTION] = 2,
};

/* nibble -> four bytes of 0x00/0xFF, so a 1bpp row becomes a select mask. */
static const uint32_t mbv_mask4[16] = {
    0x00000000u, 0x000000ffu, 0x0000ff00u, 0x0000ffffu,
    0x00ff0000u, 0x00ff00ffu, 0x00ffff00u, 0x00ffffffu,
    0xff000000u, 0xff0000ffu, 0xff00ff00u, 0xff00ffffu,
    0xffff0000u, 0xffff00ffu, 0xffffff00u, 0xffffffffu,
};

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void fmt_mbv_fill(uint8_t *dst, int stride, int n, uint8_t color)
{
    uint32_t v = (uint32_t)color * 0x01010101u;
    int y;

    for (y = 0; y < n; y++, dst += stride) {
        mbv_u32 *row = (mbv_u32 *)dst;
        row[0] = v;
        if (n > 4) {
            row[1] = v;
        }
    }
}

void fmt_mbv_raw(uint8_t *dst, int stride, int n, const uint8_t *px)
{
    int x, y;

    for (y = 0; y < n; y++, dst += stride, px += n) {
        for (x = 0; x < n; x++) {
            dst[x] = px[x];
        }
    }
}

void fmt_mbv_clr2(uint8_t *dst, int stride, const uint8_t *colors, uint16_t map)
{
    uint32_t c0 = (uint32_t)colors[0] * 0x01010101u;
    uint32_t c1 = (uint32_t)colors[1] * 0x01010101u;
    int y;

    for (y = 0; y < 4; y++, dst += stride, map >>= 4) {
        uint32_t m = mbv_mask4[map & 15u];
        *(mbv_u32 *)dst = (c1 & m) | (c0 & ~m);
    }
}

void fmt_mbv_clr4(uint8_t *dst, int stride, const uint8_t *colors, const uint8_t *map)
{
    int y;

    for (y = 0; y < 4; y++, dst += stride) {
        unsigned b = map[y];
        *(mbv_u32 *)dst = (uint32_t)colors[b & 3u]
                        | ((uint32_t)colors[(b >> 2) & 3u] << 8)
                        | ((uint32_t)colors[(b >> 4) & 3u] << 16)
                        | ((uint32_t)colors[(b >> 6) & 3u] << 24);
    }
}

void fmt_mbv_clr8(uint8_t *dst, int stride, const uint8_t *colors, const uint8_t *map)
{
    int half;

    /* Three bits per pixel does not divide a four-pixel row, so the map is
     * carried as two 24-bit groups of eight pixels - two rows each. */
    for (half = 0; half < 2; half++, map += 3) {
        uint32_t acc = (uint32_t)map[0] | ((uint32_t)map[1] << 8) |
                       ((uint32_t)map[2] << 16);
        int y;

        for (y = 0; y < 2; y++, dst += stride) {
            uint32_t v = (uint32_t)colors[acc & 7u];
            v |= (uint32_t)colors[(acc >> 3) & 7u] << 8;
            v |= (uint32_t)colors[(acc >> 6) & 7u] << 16;
            v |= (uint32_t)colors[(acc >> 9) & 7u] << 24;
            *(mbv_u32 *)dst = v;
            acc >>= 12;
        }
    }
}

void fmt_mbv_motion(uint8_t *frame, int stride, int x, int y, int dx, int dy, int n)
{
    uint8_t *dst = frame + (uint32_t)y * stride + x;
    const uint8_t *src = frame + (uint32_t)(y + dy) * stride + (x + dx);
    int i, j;

    for (i = 0; i < n; i++, dst += stride, src += stride) {
        for (j = 0; j < n; j++) {
            dst[j] = src[j];
        }
    }
}

int fmt_mbv_parse_header(const uint8_t *hdr, fmt_mbv_info *info)
{
    if (hdr[0] != 'M' || hdr[1] != 'B' || hdr[2] != 'V' || hdr[3] != '1') {
        return -1;
    }
    info->width         = rd16(hdr + 4);
    info->height        = rd16(hdr + 6);
    info->fps_q8        = rd16(hdr + 8);
    info->bpp           = hdr[10];
    info->flags         = hdr[11];
    info->frame_count   = rd32(hdr + 12);
    info->audio_rate    = rd32(hdr + 16);
    info->max_chunk     = rd32(hdr + 20);
    info->audio_samples = rd32(hdr + 24);

    if (info->bpp != 8 || info->width == 0 || info->height == 0) {
        return -1;
    }
    /* The block grid has to tile the frame exactly: no partial macroblocks. */
    if ((info->width & 7u) || (info->height & 7u)) {
        return -1;
    }
    return 0;
}

void fmt_mbv_dec_init(fmt_mbv_dec *d, const fmt_mbv_info *info,
                      uint8_t *framebuf, uint16_t stride)
{
    uint32_t i, n;

    d->width  = info->width;
    d->height = info->height;
    d->stride = stride ? stride : info->width;
    d->mb_w   = (uint16_t)(info->width >> 3);
    d->mb_h   = (uint16_t)(info->height >> 3);
    d->frame  = framebuf;
    d->pal_dirty = 0;
    d->frames = 0;

    n = (uint32_t)d->stride * d->height;
    for (i = 0; i < n; i++) {
        framebuf[i] = 0;
    }
    for (i = 0; i < 768; i++) {
        d->pal[i] = 0;
    }
}

uint32_t fmt_mbv_chunk_bytes(const uint8_t *chunk, uint32_t avail)
{
    if (avail < MBV_CHUNK_HEADER_BYTES) {
        return 0;
    }
    return rd32(chunk) + MBV_CHUNK_HEADER_BYTES;
}

int fmt_mbv_decode_chunk(fmt_mbv_dec *d, const uint8_t *chunk, uint32_t len,
                         const uint8_t **audio, uint16_t *audio_len)
{
    const uint8_t *p, *end;
    uint32_t size;
    uint16_t abytes;
    uint8_t ftype, pal_entries;
    int stride = d->stride;
    int mb_x, mb_y;

    if (len < MBV_CHUNK_HEADER_BYTES) {
        return -1;
    }
    size        = rd32(chunk);
    abytes      = rd16(chunk + 4);
    ftype       = chunk[6];
    pal_entries = chunk[7];
    if (size > len - MBV_CHUNK_HEADER_BYTES) {
        return -1;
    }
    p   = chunk + MBV_CHUNK_HEADER_BYTES;
    end = p + size;

    /* 1. Palette.  A keyframe carries all 256 entries; an inter frame may
     *    carry a handful of (index, R, G, B) patches instead. */
    d->pal_dirty = 0;
    if (ftype == MBV_FTYPE_KEY) {
        int i;
        if (end - p < 768) {
            return -1;
        }
        for (i = 0; i < 768; i++) {
            d->pal[i] = p[i];
        }
        p += 768;
        d->pal_dirty = 1;
    } else if (pal_entries) {
        unsigned i;
        if ((uint32_t)(end - p) < (uint32_t)pal_entries * 4u) {
            return -1;
        }
        for (i = 0; i < pal_entries; i++, p += 4) {
            uint8_t *e = d->pal + (unsigned)p[0] * 3u;
            e[0] = p[1];
            e[1] = p[2];
            e[2] = p[3];
        }
        d->pal_dirty = 1;
    }

    /* 2. Audio, handed back where it lies rather than copied. */
    if ((uint32_t)(end - p) < abytes) {
        return -1;
    }
    if (audio) {
        *audio = p;
    }
    if (audio_len) {
        *audio_len = abytes;
    }
    p += abytes;

    /* 3. Video: macroblocks in raster order, one forward pointer, no
     *    bit reader.  Every opcode checks its own operands fit. */
    mb_x = 0;
    mb_y = 0;
    while (mb_y < d->mb_h) {
        uint8_t *mb;
        unsigned op;

        if (p >= end) {
            return -1;
        }
        op = *p++;
        mb = d->frame + (uint32_t)(mb_y << 3) * stride + (mb_x << 3);

        if (op < MBV_MB_SKIP_MAX) {
            /* Run of untouched macroblocks: the whole point of the format.
             * Nothing is read and nothing is written. */
            unsigned run = op + 1u;
            unsigned pos = (unsigned)mb_y * d->mb_w + mb_x + run;
            if (pos > (unsigned)d->mb_w * d->mb_h) {
                return -1;
            }
            mb_y = (int)(pos / d->mb_w);
            mb_x = (int)(pos % d->mb_w);
            continue;
        }

        switch (op) {
        case MBV_MB_MOTION: {
            int dx, dy;
            if (p >= end) {
                return -1;
            }
            dx = MBV_MV_DX(*p);
            dy = MBV_MV_DY(*p);
            p++;
            if ((mb_x << 3) + dx < 0 || (mb_x << 3) + dx + 8 > d->width ||
                (mb_y << 3) + dy < 0 || (mb_y << 3) + dy + 8 > d->height) {
                return -1;
            }
            fmt_mbv_motion(d->frame, stride, mb_x << 3, mb_y << 3, dx, dy, 8);
            break;
        }
        case MBV_MB_FILL:
            if (p >= end) {
                return -1;
            }
            fmt_mbv_fill(mb, stride, 8, *p++);
            break;

        case MBV_MB_RAW:
            if (end - p < 64) {
                return -1;
            }
            fmt_mbv_raw(mb, stride, 8, p);
            p += 64;
            break;

        case MBV_MB_SPLIT: {
            int sub;
            for (sub = 0; sub < 4; sub++) {
                int bx = (mb_x << 3) + ((sub & 1) << 2);
                int by = (mb_y << 3) + ((sub & 2) << 1);
                uint8_t *dst = d->frame + (uint32_t)by * stride + bx;
                unsigned bop;

                if (p >= end) {
                    return -1;
                }
                bop = *p++;
                if (bop >= MBV_B_COUNT ||
                    (uint32_t)(end - p) < (uint32_t)mbv_block_bytes[bop] - 1u) {
                    return -1;
                }
                switch (bop) {
                case MBV_B_SKIP:
                    break;
                case MBV_B_FILL:
                    fmt_mbv_fill(dst, stride, 4, *p++);
                    break;
                case MBV_B_CLR2:
                    fmt_mbv_clr2(dst, stride, p, rd16(p + 2));
                    p += 4;
                    break;
                case MBV_B_CLR4:
                    fmt_mbv_clr4(dst, stride, p, p + 4);
                    p += 8;
                    break;
                case MBV_B_CLR8:
                    fmt_mbv_clr8(dst, stride, p, p + 8);
                    p += 14;
                    break;
                case MBV_B_RAW:
                    fmt_mbv_raw(dst, stride, 4, p);
                    p += 16;
                    break;
                case MBV_B_MOTION: {
                    int dx = MBV_MV_DX(*p);
                    int dy = MBV_MV_DY(*p);
                    p++;
                    if (bx + dx < 0 || bx + dx + 4 > d->width ||
                        by + dy < 0 || by + dy + 4 > d->height) {
                        return -1;
                    }
                    fmt_mbv_motion(d->frame, stride, bx, by, dx, dy, 4);
                    break;
                }
                default:
                    return -1;
                }
            }
            break;
        }
        default:
            return -1;   /* reserved opcode */
        }

        MBV_TICK();

        if (++mb_x == d->mb_w) {
            mb_x = 0;
            mb_y++;
        }
    }

    d->frames++;
    return 0;
}
