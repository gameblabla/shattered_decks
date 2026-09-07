/* See sdl3_video_ffmpeg.h — the looping title-screen video decoder (PC). */

#include "sdl3_video_ffmpeg.h"

#ifndef WAIFU_SDL3_FFMPEG

int waifu_sdl3_titlevid_open(const char *path) { (void)path; return 0; }
int waifu_sdl3_titlevid_poll(const uint8_t **rgba, int *w, int *h)
{
    (void)rgba; (void)w; (void)h;
    return -1;
}
void waifu_sdl3_titlevid_close(void) {}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#ifndef WAIFU_SDL3_TITLE_VIDEO
#define WAIFU_SDL3_TITLE_VIDEO "assets/source/videos/animated_titlescreen.mp4"
#endif

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    AVBufferRef *hw_device;
    enum AVPixelFormat hw_pix_fmt;   /* AV_PIX_FMT_NONE when decoding in software */
    struct SwsContext *sws;
    int stream;

    AVPacket *pkt;
    AVFrame *frame;                  /* decoder output (may live on the GPU) */
    AVFrame *sw_frame;               /* host copy of a hardware frame */
    AVFrame *pending;                /* decoded, not yet due */
    double pending_t;

    uint8_t *rgba;                   /* tightly packed RGBA8, w*h*4 */
    int w, h;
    int sws_src_fmt, sws_src_w, sws_src_h;

    double time_base;
    double loop_base;                /* seconds added to this pass's timestamps */
    double last_t;                   /* display time of the newest decoded frame */
    int64_t start_us;                /* playback clock origin */
    int started;
    int opened;
    int failed;
    int have_rgba;
} TitleVid;

static TitleVid g_tv;

/* The decoder asks which pixel format to produce; take the hardware one when
   this stream/codec pair actually offers it, otherwise let FFmpeg pick. */
static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    const enum AVPixelFormat *p;
    (void)ctx;
    for (p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == g_tv.hw_pix_fmt) return *p;
    return fmts[0];
}

/* Attach an NVDEC/CUDA device to `dec` if this decoder supports one and the
   machine has a usable CUDA device. Returns 1 when hardware decoding is armed. */
static int try_nvdec(AVCodecContext *dec, const AVCodec *codec)
{
    int i;
    for (i = 0;; ++i) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg) return 0;
        if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
        if (cfg->device_type != AV_HWDEVICE_TYPE_CUDA) continue;
        if (av_hwdevice_ctx_create(&g_tv.hw_device, AV_HWDEVICE_TYPE_CUDA,
                                   NULL, NULL, 0) < 0)
            return 0;
        g_tv.hw_pix_fmt = cfg->pix_fmt;
        dec->hw_device_ctx = av_buffer_ref(g_tv.hw_device);
        dec->get_format = pick_format;
        return 1;
    }
}

static void tv_free(TitleVid *tv)
{
    if (tv->sws) sws_freeContext(tv->sws);
    av_frame_free(&tv->pending);
    av_frame_free(&tv->sw_frame);
    av_frame_free(&tv->frame);
    av_packet_free(&tv->pkt);
    avcodec_free_context(&tv->dec);
    if (tv->fmt) avformat_close_input(&tv->fmt);
    av_buffer_unref(&tv->hw_device);
    free(tv->rgba);
    memset(tv, 0, sizeof(*tv));
    tv->hw_pix_fmt = AV_PIX_FMT_NONE;
}

int waifu_sdl3_titlevid_open(const char *path)
{
    TitleVid *tv = &g_tv;
    const AVCodec *codec = NULL;
    AVStream *st;

    if (tv->opened) return 1;
    if (tv->failed) return 0;
    if (!path || !path[0]) path = WAIFU_SDL3_TITLE_VIDEO;

    memset(tv, 0, sizeof(*tv));
    tv->hw_pix_fmt = AV_PIX_FMT_NONE;

    if (avformat_open_input(&tv->fmt, path, NULL, NULL) < 0) goto fail;
    if (avformat_find_stream_info(tv->fmt, NULL) < 0) goto fail;
    tv->stream = av_find_best_stream(tv->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (tv->stream < 0 || !codec) goto fail;
    st = tv->fmt->streams[tv->stream];

    tv->dec = avcodec_alloc_context3(codec);
    if (!tv->dec) goto fail;
    if (avcodec_parameters_to_context(tv->dec, st->codecpar) < 0) goto fail;
    tv->dec->thread_count = 0;   /* software fallback decodes on all cores */
    try_nvdec(tv->dec, codec);
    if (avcodec_open2(tv->dec, codec, NULL) < 0) {
        /* A hardware device that refuses the stream must not cost us the
           title: drop it and reopen the same decoder in software. */
        if (!tv->hw_device) goto fail;
        avcodec_free_context(&tv->dec);
        av_buffer_unref(&tv->hw_device);
        tv->hw_pix_fmt = AV_PIX_FMT_NONE;
        tv->dec = avcodec_alloc_context3(codec);
        if (!tv->dec) goto fail;
        if (avcodec_parameters_to_context(tv->dec, st->codecpar) < 0) goto fail;
        tv->dec->thread_count = 0;
        if (avcodec_open2(tv->dec, codec, NULL) < 0) goto fail;
    }

    tv->pkt = av_packet_alloc();
    tv->frame = av_frame_alloc();
    tv->sw_frame = av_frame_alloc();
    if (!tv->pkt || !tv->frame || !tv->sw_frame) goto fail;

    tv->time_base = av_q2d(st->time_base);
    if (tv->time_base <= 0.0) tv->time_base = 1.0 / 1000.0;
    tv->opened = 1;
    return 1;

fail:
    tv_free(tv);
    tv->failed = 1;
    return 0;
}

/* Rewind to the first frame and shift the clock so the clip plays again. */
static void tv_restart(TitleVid *tv)
{
    tv->loop_base = tv->last_t > 0.0 ? tv->last_t + 1.0 / 30.0 : 0.0;
    av_seek_frame(tv->fmt, tv->stream, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(tv->dec);
}

/* Pull the next displayable frame. Returns a frame owned by the caller (to be
   freed with av_frame_free) and its display time, or NULL when the clip cannot
   produce one at all. Loops at end of stream. */
static AVFrame *tv_decode_next(TitleVid *tv, double *out_t)
{
    int loops = 0;

    for (;;) {
        int r = avcodec_receive_frame(tv->dec, tv->frame);
        if (r == 0) {
            AVFrame *src = tv->frame;
            AVFrame *out;
            int64_t pts = src->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = src->pts;
            if (tv->hw_pix_fmt != AV_PIX_FMT_NONE && src->format == tv->hw_pix_fmt) {
                av_frame_unref(tv->sw_frame);
                if (av_hwframe_transfer_data(tv->sw_frame, src, 0) < 0) {
                    av_frame_unref(tv->frame);
                    continue;
                }
                tv->sw_frame->best_effort_timestamp = pts;
                src = tv->sw_frame;
            }
            out = av_frame_alloc();
            if (!out) { av_frame_unref(tv->frame); return NULL; }
            av_frame_move_ref(out, src);
            av_frame_unref(tv->frame);
            *out_t = tv->loop_base + (pts == AV_NOPTS_VALUE ? tv->last_t
                                                           : (double)pts * tv->time_base);
            tv->last_t = *out_t;
            return out;
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) return NULL;

        if (r == AVERROR_EOF) {
            if (++loops > 1) return NULL;
            tv_restart(tv);
        }

        /* Feed the decoder until it has something. */
        for (;;) {
            int rd = av_read_frame(tv->fmt, tv->pkt);
            if (rd < 0) {
                if (++loops > 1) return NULL;
                avcodec_send_packet(tv->dec, NULL);   /* drain, then rewind */
                tv_restart(tv);
                continue;
            }
            if (tv->pkt->stream_index != tv->stream) { av_packet_unref(tv->pkt); continue; }
            r = avcodec_send_packet(tv->dec, tv->pkt);
            av_packet_unref(tv->pkt);
            if (r >= 0 || r == AVERROR(EAGAIN)) break;
            return NULL;
        }
    }
}

/* Convert one decoded frame into the module's RGBA8 buffer. */
static int tv_to_rgba(TitleVid *tv, AVFrame *f)
{
    uint8_t *dst[4];
    int stride[4];

    if (f->width <= 0 || f->height <= 0) return 0;
    if (!tv->rgba || tv->w != f->width || tv->h != f->height) {
        uint8_t *buf = (uint8_t *)malloc((size_t)f->width * (size_t)f->height * 4);
        if (!buf) return 0;
        free(tv->rgba);
        tv->rgba = buf;
        tv->w = f->width;
        tv->h = f->height;
    }
    if (!tv->sws || tv->sws_src_fmt != f->format ||
        tv->sws_src_w != f->width || tv->sws_src_h != f->height) {
        if (tv->sws) sws_freeContext(tv->sws);
        tv->sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                                 f->width, f->height, AV_PIX_FMT_RGBA,
                                 SWS_BILINEAR, NULL, NULL, NULL);
        if (!tv->sws) return 0;
        tv->sws_src_fmt = f->format;
        tv->sws_src_w = f->width;
        tv->sws_src_h = f->height;
    }
    dst[0] = tv->rgba; dst[1] = dst[2] = dst[3] = NULL;
    stride[0] = tv->w * 4; stride[1] = stride[2] = stride[3] = 0;
    sws_scale(tv->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
              dst, stride);
    tv->have_rgba = 1;
    return 1;
}

int waifu_sdl3_titlevid_poll(const uint8_t **rgba, int *w, int *h)
{
    TitleVid *tv = &g_tv;
    double now;
    int got = 0;

    if (!tv->opened && !waifu_sdl3_titlevid_open(NULL)) return -1;

    if (!tv->started) {
        tv->start_us = av_gettime_relative();
        tv->started = 1;
    }
    now = (double)(av_gettime_relative() - tv->start_us) / 1000000.0;

    for (;;) {
        if (!tv->pending) {
            tv->pending = tv_decode_next(tv, &tv->pending_t);
            if (!tv->pending) break;
        }
        if (tv->pending_t > now && tv->have_rgba) break;
        got |= tv_to_rgba(tv, tv->pending);
        av_frame_free(&tv->pending);
        if (!tv->have_rgba) break;    /* conversion failed; do not spin */
    }

    if (!tv->have_rgba) return -1;
    if (!got) return 0;
    if (rgba) *rgba = tv->rgba;
    if (w) *w = tv->w;
    if (h) *h = tv->h;
    return 1;
}

void waifu_sdl3_titlevid_close(void)
{
    if (g_tv.opened) tv_free(&g_tv);
}

#endif /* WAIFU_SDL3_FFMPEG */
