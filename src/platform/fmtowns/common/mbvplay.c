#include "mbvplay.h"
#include "mbv.h"
#include "mbv_blit.h"
#include "dacout.h"
#include "libfmt.h"
#include "palette.h"
#include "io.h"
#include "media.h"

/* See mbvplay.h for how the four jobs in here share the CPU. */

/* The disc is streamed into this ring.  Power of two, a multiple of 2048, and
 * comfortably larger than the largest chunk in any stream the encoder will
 * produce (tools/mbvenc.c's -m ceiling, 30000 by default): a whole chunk has
 * to be resident before it can be decoded, and the drive has to be able to
 * keep working ahead of that while it is. */
#define MBV_RING        65536u

/* Chunks are decoded in place out of the ring; this is only used for the one
 * chunk in every ring-full that straddles the wrap. */
#define MBV_SCRATCH     32768u

/* 256x240 8bpp - the only mode this player targets.  Sized statically because
 * there is no allocator here and no reason to want one. */
#define MBV_MAX_W       256u
#define MBV_MAX_H       240u

/* One frame of audio at 12fps/16kHz is 1334 bytes; leave room for slower
 * frame rates without making the encoder's job a special case. */
#define MBV_AUDIO_MAX   4096u

static uint8_t g_ring[MBV_RING];
static uint8_t g_scratch[MBV_SCRATCH];
static uint8_t g_frame[MBV_MAX_W * MBV_MAX_H];
static uint8_t g_audio[2][MBV_AUDIO_MAX];

/* Diagnostic counters, published to a fixed physical address so playback can
 * be watched in-machine with `MEMDUMP PHYS:00100000 28 1` in the emulator's
 * console while it runs.  Enabled by `make VIDEO_PLAYER=1 MBV_STATS=1`.
 *
 * Not 0x00080000, which is where mp2stream.c and the YM busy probe publish
 * theirs: this player's buffers are ~160KB of .bss and the payload loads at
 * 0x10000, so 0x80000 is inside them.
 *
 * Layout, little-endian 32-bit after the magic:
 *   0  "MBVS"
 *   4  stage reached (see MBV_STAGE_* below)
 *   8  frames presented
 *  12  bytes consumed from the stream
 *  16  DAC underruns
 *  20  streaming reader state (FMT_MEDIA_STREAM_*)
 *  24  vertical blanking interval, microseconds (measured once at startup)
 *  28  palette entries written on the last keyframe
 *  32  microseconds that upload took
 *  36  worst palette upload seen, microseconds
 *  40  0xA5A5A5A5 trailer
 */
#ifdef FMT_MBV_STATS
#define MBV_STATS ((volatile uint32_t *)0x00100000u)
#define MBV_STAGE_LOADED    1u
#define MBV_STAGE_MODE      2u
#define MBV_STAGE_PRIMED    3u
#define MBV_STAGE_FIRST     4u
#define MBV_STAGE_PLAYING   5u
#define MBV_STAGE_DONE      6u
static uint32_t g_stat_stage, g_stat_frames, g_stat_pos;
static uint32_t g_stat_vblank, g_stat_pal_n, g_stat_pal_us, g_stat_pal_worst;
static void mbv_stats(const fmt_media_stream *media)
{
    MBV_STATS[0] = 0x5356424du;    /* "MBVS" */
    MBV_STATS[1] = g_stat_stage;
    MBV_STATS[2] = g_stat_frames;
    MBV_STATS[3] = g_stat_pos;
    MBV_STATS[4] = fmt_dac.underruns;
    MBV_STATS[5] = media ? media->state : 0xffffffffu;
    MBV_STATS[6] = g_stat_vblank;
    MBV_STATS[7] = g_stat_pal_n;
    MBV_STATS[8] = g_stat_pal_us;
    MBV_STATS[9] = g_stat_pal_worst;
    MBV_STATS[10] = 0xA5A5A5A5u;
}
#define MBV_STAT_PAL(n, us) do { \
    g_stat_pal_n = (n); g_stat_pal_us = (us); \
    if ((us) > g_stat_pal_worst) g_stat_pal_worst = (us); } while (0)
#define MBV_STAT_VBLANK(us) do { g_stat_vblank = (us); } while (0)
#define MBV_STAGE(s, media)  do { g_stat_stage = (s); mbv_stats(media); } while (0)
#define MBV_PROGRESS(f, p, media) \
    do { g_stat_frames = (f); g_stat_pos = (p); mbv_stats(media); } while (0)
#else
#define MBV_STAGE(s, media)        ((void)0)
#define MBV_PROGRESS(f, p, media)  ((void)0)
#define MBV_STAT_PAL(n, us)     ((void)0)
#define MBV_STAT_VBLANK(us)     ((void)0)
#endif

/* CRTC status register, read after selecting register 30 - libfmt's
 * fmt_wait_vsync() uses the same pair.  Bit 2 is asserted for the duration of
 * the vertical blanking interval. */
#define MBV_CRTC_STATUS     0x443
#define MBV_CRTC_VSYNC_BIT  0x04

static fmt_mbv_info g_info;
static fmt_mbv_dec  g_dec;
static uint32_t g_media_off, g_size;
static uint8_t  g_loaded;
static volatile uint8_t g_stop;

/* fmt_flip_page_poll() takes a plain function pointer, and this is all it
 * needs to do: keep the sample clock running across the vertical blank. */
static void poll_dac(void)
{
    fmt_dac_tick();
}

int fmt_mbv_stream_load_file(const char *name)
{
    g_loaded = 0;
    if (fmt_media_find(name, &g_media_off, &g_size) != 0 || g_size <= MBV_HEADER_BYTES) {
        return -1;
    }
    /* The header is the first 32 bytes of the file; one sector read is the
     * cheapest way to see them, and the streaming reader will fetch the
     * sector again from the start when playback begins. */
    if (fmt_media_read_block(g_media_off, g_scratch) != 0) {
        return -1;
    }
    if (fmt_mbv_parse_header(g_scratch, &g_info) != 0) {
        return -1;
    }
    if (g_info.width > MBV_MAX_W || g_info.height > MBV_MAX_H ||
        g_info.max_chunk > MBV_SCRATCH || g_info.max_chunk == 0) {
        return -1;
    }
    g_loaded = 1;
    MBV_STAGE(MBV_STAGE_LOADED, (fmt_media_stream *)0);
    return 0;
}

void fmt_mbv_stream_stop(void)
{
    g_stop = 1;
}

uint32_t fmt_mbv_stream_underruns(void)
{
    return fmt_dac.underruns;
}

/*
 * Locates the whole chunk at `pos`.  Returns 1 with `out`/`out_len` set, 0 if
 * the drive has not delivered all of it yet, or -1 if the stream is corrupt.
 *
 * The common case hands back a pointer straight into the ring and copies
 * nothing: the streaming reader never overwrites bytes ahead of the read
 * cursor we pass it, and the cursor stays parked at the start of this chunk
 * until it has been decoded.  Only a chunk that straddles the ring's wrap has
 * to be linearised into g_scratch, which is one chunk in every 64KB.
 */
static int chunk_at(fmt_media_stream *media, uint32_t pos,
                    const uint8_t **out, uint32_t *out_len)
{
    uint8_t hdr[MBV_CHUNK_HEADER_BYTES];
    uint32_t avail = media->fill_pos - pos;
    uint32_t start = pos & (MBV_RING - 1u);
    uint32_t clen, i;

    if (avail < MBV_CHUNK_HEADER_BYTES) {
        return 0;
    }
    for (i = 0; i < MBV_CHUNK_HEADER_BYTES; i++) {
        hdr[i] = g_ring[(pos + i) & (MBV_RING - 1u)];
    }
    clen = fmt_mbv_chunk_bytes(hdr, MBV_CHUNK_HEADER_BYTES);
    if (clen == 0 || clen > MBV_SCRATCH) {
        return -1;            /* corrupt stream, or one this build cannot hold */
    }
    if (avail < clen) {
        return 0;
    }

    *out_len = clen;
    if (start + clen <= MBV_RING) {
        *out = g_ring + start;
        return 1;
    }
    for (i = 0; i < clen; i++) {
        g_scratch[i] = g_ring[(pos + i) & (MBV_RING - 1u)];
    }
    *out = g_scratch;
    return 1;
}

/*
 * Getting a new palette in without a visible glitch.
 *
 * The palette DAC is consulted per pixel as the CRTC scans out, so an entry
 * written after the vertical blanking interval has ended recolours the rest of
 * that frame, from the current raster line down.  The window is not large: in
 * this mode the CRTC's VDS0 puts the first displayed line 70 half-lines into a
 * 1050 half-line frame, which at 31.5kHz is 1.11ms from the vertical sync
 * edge fmt_flip_page() returns on.  (The sync pulse itself, VST1..VST2, is
 * only 64us - measured at 59us, MBV_STATS[6] - so waiting for the pulse to
 * end would leave nothing.)
 *
 * A keyframe can change the whole palette, and 256 entries written one by one
 * with a DAC tick between each came to roughly a millisecond: right at the
 * edge of that window, over it often enough to show up as reported - a brief
 * wrong-colour flash, at a GOP boundary.
 *
 * Three things keep the upload inside the interval:
 *
 *   - Only entries that actually differ from what the DAC already holds are
 *     written.  g_pal_shadow tracks that.  The encoder deliberately keeps
 *     palettes aligned across GOP boundaries and snaps near-identical entries
 *     to the ones already there (align_palette() in tools/mbvenc.c), so a
 *     keyframe within a scene now changes tens of entries rather than 256.
 *   - Working out *which* entries changed is done before the flip, not after
 *     it.  Comparing all 256 costs a couple of hundred microseconds, and
 *     there is no reason to spend them inside the one window that matters
 *     when there are 80ms of frame either side of it.
 *   - The DAC is serviced every eight entries instead of every one.  Eight
 *     entries is 32 port writes, comfortably inside the 62.5us sample period,
 *     and it keeps ~250 timer reads out of the critical window.
 *   - The upload is measured against the free-running 1us counter and
 *     published (MBV_STATS[7..9]) so this stays a fact rather than a hope.
 *     Measured on sailor.mkv: 115us for a 45-entry keyframe, 258us for 101
 *     entries, and 653us for the one scene cut that replaces all 256 - all
 *     inside the 1.11ms available, the worst case with 40% to spare.
 *
 * A scene cut that genuinely replaces all 256 entries still writes all of
 * them rather than stopping at the window's edge and finishing next frame:
 * running a little long costs one frame with a seam in it, whereas deferring
 * would leave the whole picture in the wrong colours for a further 83ms.
 */
static uint8_t g_pal_shadow[768];
static uint8_t g_pal_pending[256];
static unsigned g_pal_pending_n;
static uint8_t g_pal_shadow_valid;

/* Works out what will have to be written, outside the window where writing it
 * matters.  Called before the blit; upload_palette() is called after the flip
 * and does nothing but the port writes. */
static void prepare_palette(void)
{
    unsigned i;

    g_pal_pending_n = 0;
    if (!g_dec.pal_dirty) {
        return;
    }
    for (i = 0; i < 256u; i++) {
        const uint8_t *e = g_dec.pal + i * 3;
        uint8_t *s = g_pal_shadow + i * 3;

        if (g_pal_shadow_valid && s[0] == e[0] && s[1] == e[1] && s[2] == e[2]) {
            continue;
        }
        s[0] = e[0];
        s[1] = e[1];
        s[2] = e[2];
        g_pal_pending[g_pal_pending_n++] = (uint8_t)i;
        if ((g_pal_pending_n & 31u) == 0u) {
            fmt_dac_tick();
        }
    }
    g_pal_shadow_valid = 1;
}

static void upload_palette(void)
{
    unsigned i;
#ifdef FMT_MBV_STATS
    uint16_t t0 = inw(FMT_DAC_FREERUN_TIMER);
#endif

    for (i = 0; i < g_pal_pending_n; i++) {
        unsigned c = g_pal_pending[i];
        const uint8_t *e = g_pal_shadow + c * 3;

        set_palette((uint8_t)c, e[0], e[1], e[2]);
        if ((i & 7u) == 7u) {
            fmt_dac_tick();
        }
    }
    MBV_STAT_PAL(g_pal_pending_n,
                 (uint32_t)(uint16_t)(inw(FMT_DAC_FREERUN_TIMER) - t0));
    g_pal_pending_n = 0;
    fmt_dac_tick();
}

/* Blit the decoded frame into the page being drawn, show it, then apply any
 * new palette.  Order matters twice over: a keyframe's palette belongs to the
 * picture that arrives with it, so loading it before the flip would apply it
 * to the frame still on screen; and the flip returns at the leading edge of
 * the vertical blank, which is the only part of the frame where the palette
 * can be changed unseen - so the upload goes here, immediately after it, and
 * nothing else is allowed in between. */
static void present(void)
{
    prepare_palette();
    fmt_mbv_blit((volatile uint8_t *)g_fmt_vram0_base, g_fmt_draw_buffer_offset,
                 g_frame, (uint32_t)g_info.width * g_info.height);
    fmt_flip_page_poll(poll_dac);
    if (g_pal_pending_n) {
        upload_palette();
    }
}

/* Audio has to be copied out of the ring: the samples belong to a chunk whose
 * bytes the drive is free to overwrite as soon as the read cursor moves past
 * them, and they are still playing while the next chunk is being fetched. */
static uint16_t take_audio(const uint8_t *audio, uint16_t alen, uint8_t *dst)
{
    uint16_t i;

    if (alen > MBV_AUDIO_MAX) {
        alen = MBV_AUDIO_MAX;
    }
    for (i = 0; i < alen; i++) {
        dst[i] = audio[i];
        if ((i & 63u) == 0u) {
            fmt_dac_tick();
        }
    }
    return alen;
}

#ifdef FMT_MBV_STATS
/* How much blanking there actually is to spend on the palette, measured
 * rather than derived from the CRTC numbers.  Runs once, before any audio is
 * playing, so blocking here costs nothing. */
static void measure_vblank(void)
{
    uint16_t t0;

    fmt_wait_vsync();              /* returns at the leading edge */
    t0 = inw(FMT_DAC_FREERUN_TIMER);
    while (inb(MBV_CRTC_STATUS) & MBV_CRTC_VSYNC_BIT) {
    }
    MBV_STAT_VBLANK((uint32_t)(uint16_t)(inw(FMT_DAC_FREERUN_TIMER) - t0));
}
#endif

int fmt_mbv_stream_play(void)
{
    fmt_media_stream media;
    uint32_t pos = MBV_HEADER_BYTES;
    uint32_t clen = 0;
    uint32_t frame;
    const uint8_t *chunk, *audio;
    uint16_t alen;
    uint8_t which = 0;

    if (!g_loaded) {
        return -1;
    }
    g_stop = 0;

    fmt_set_mode(FMT_MODE_256x240_8BPP);
    if (!fmt_page_flipping_available()) {
        return -1;
    }
    fmt_mbv_dec_init(&g_dec, &g_info, g_frame, g_info.width);
    g_pal_shadow_valid = 0;
    g_pal_pending_n = 0;
#ifdef FMT_MBV_STATS
    measure_vblank();
#endif
    MBV_STAGE(MBV_STAGE_MODE, (fmt_media_stream *)0);

    fmt_media_stream_init(&media, g_media_off, g_size, g_ring, MBV_RING, 0);

    /* Fill the ring before making a sound.  Nothing is playing yet, so this
     * may take as long as the drive wants. */
    while (media.fill_pos < MBV_RING) {
        if (media.state == FMT_MEDIA_STREAM_ERROR) {
            return -1;
        }
        if (media.state == FMT_MEDIA_STREAM_DONE) {
            break;   /* whole file is shorter than the ring */
        }
        fmt_media_stream_step(&media, 0, 64);
    }

    MBV_STAGE(MBV_STAGE_PRIMED, &media);

    /* Frame 0: a keyframe, so it brings the palette with it. */
    for (;;) {
        int r = chunk_at(&media, pos, &chunk, &clen);
        if (r > 0) {
            break;
        }
        if (r < 0 || media.state == FMT_MEDIA_STREAM_ERROR ||
            media.state == FMT_MEDIA_STREAM_DONE) {
            return -1;
        }
        fmt_media_stream_step(&media, pos, 256);
    }
    if (fmt_mbv_decode_chunk(&g_dec, chunk, clen, &audio, &alen) != 0) {
        return -1;
    }
    alen = take_audio(audio, alen, g_audio[0]);
    pos += clen;

    MBV_STAGE(MBV_STAGE_FIRST, &media);
    present();

    fmt_dac_start();
    fmt_dac_submit(g_audio[0], alen);

    /* Interrupts stay masked for the same reason pcmstream.c masks them: an
     * IRQ landing between two DAC writes steals time the pacing assumes it
     * has, and this payload's IDT has no hardware vectors anyway. */
    __asm__ __volatile__("cli");

    MBV_STAGE(MBV_STAGE_PLAYING, &media);

    for (frame = 1; frame < g_info.frame_count; frame++) {
        uint8_t next = which ^ 1u;

        /* Wait for the whole of the next chunk, ticking the DAC and stepping
         * the drive one I/O operation at a time while it arrives. */
        for (;;) {
            int r;
            if (g_stop) {
                goto done;
            }
            r = chunk_at(&media, pos, &chunk, &clen);
            if (r > 0) {
                break;
            }
            if (r < 0 || media.state == FMT_MEDIA_STREAM_ERROR ||
                media.state == FMT_MEDIA_STREAM_DONE) {
                goto done;
            }
            fmt_dac_tick();
            fmt_media_stream_step(&media, pos, 1);
        }

        /* Decode into the persistent frame buffer.  The decoder ticks the DAC
         * once per macroblock, so the current frame's audio keeps playing
         * throughout. */
        if (fmt_mbv_decode_chunk(&g_dec, chunk, clen, &audio, &alen) != 0) {
            goto done;
        }
        alen = take_audio(audio, alen, g_audio[next]);
        pos += clen;

        /* Hand over only once the frame on screen has played out its audio.
         * This is what keeps sound and picture together. */
        while (!fmt_dac_drained()) {
            if (g_stop) {
                goto done;
            }
            fmt_dac_tick();
            fmt_media_stream_step(&media, pos, 1);
        }
        fmt_dac_submit(g_audio[next], alen);
        which = next;

        /* The new audio is now playing, which is exactly the cover the blit
         * and the wait for vertical blank need. */
        present();
        MBV_PROGRESS(frame, pos, &media);
    }

done:
    MBV_STAGE(MBV_STAGE_DONE, &media);
    fmt_dac_stop();
    return 0;
}
