#include "zmbv_mkv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/*
 * Small Matroska/ZMBV recorder, adapted from the GP32emu MKV recorder supplied
 * by the user. This version intentionally ignores the rest of the emulator and
 * records this game's 256x240 indexed framebuffer directly as V_MS/VFW/FOURCC
 * ZMBV keyframes in an MKV container.
 */

#define ZMBV_BLOCK 16u
#define ZMBV_FMT_32BPP 8u
#define ZMBV_KEYFRAME 1u
#define MKV_TIMECODE_SCALE_NS 100000u
#define MKV_TIMECODE_TICKS_PER_SECOND (1000000000ull / MKV_TIMECODE_SCALE_NS)
#define MKV_CLUSTER_MAX_TICKS 30000ull

typedef struct membuf {
    uint8_t *p;
    size_t n;
    size_t cap;
} membuf_t;

struct zmbv_mkv_recorder {
    FILE *f;
    int w;
    int h;
    int fps_num;
    int fps_den;
    uint64_t current_cluster_ticks;
    int cluster_open;
    char err[256];
    uint8_t *raw_bgra;
    size_t raw_cap;
    uint8_t *zcomp;
    size_t zcomp_cap;
};

static void seterr(char *err, size_t err_len, const char *msg) {
    if (err && err_len) snprintf(err, err_len, "%s", msg ? msg : "unknown error");
}

static void rec_seterr(zmbv_mkv_recorder_t *r, const char *msg) {
    if (r) snprintf(r->err, sizeof(r->err), "%s", msg ? msg : "unknown recorder error");
}

const char *zmbv_mkv_error(const zmbv_mkv_recorder_t *r) {
    return r && r->err[0] ? r->err : "recorder error";
}

static void put_u16be(FILE *f, uint16_t v) { fputc((int)((v >> 8) & 255u), f); fputc((int)(v & 255u), f); }
static void put_u32be(FILE *f, uint32_t v) { put_u16be(f, (uint16_t)(v >> 16)); put_u16be(f, (uint16_t)v); }
static void put_u64be(FILE *f, uint64_t v) { put_u32be(f, (uint32_t)(v >> 32)); put_u32be(f, (uint32_t)v); }

static int mb_reserve(membuf_t *b, size_t add) {
    if (add > (size_t)-1 - b->n) return 0;
    size_t need = b->n + add;
    if (need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap * 2u : 256u;
    while (cap < need) {
        if (cap > (size_t)-1 / 2u) { cap = need; break; }
        cap *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p; b->cap = cap;
    return 1;
}
static int mb_put(membuf_t *b, const void *p, size_t n) { if (!mb_reserve(b, n)) return 0; memcpy(b->p + b->n, p, n); b->n += n; return 1; }
static int mb_u8(membuf_t *b, uint8_t v) { return mb_put(b, &v, 1); }
static int mb_u16le(membuf_t *b, uint16_t v) { uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; return mb_put(b, x, 2); }
static int mb_u32le(membuf_t *b, uint32_t v) { return mb_u16le(b, (uint16_t)v) && mb_u16le(b, (uint16_t)(v >> 16)); }
static int mb_u32be(membuf_t *b, uint32_t v) { uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; return mb_put(b, x, 4); }
static int mb_u64be(membuf_t *b, uint64_t v) { return mb_u32be(b, (uint32_t)(v >> 32)) && mb_u32be(b, (uint32_t)v); }

static void ebml_id(FILE *f, uint32_t id) {
    if (id > 0x00ffffffu) { fputc((int)(id >> 24), f); fputc((int)(id >> 16), f); fputc((int)(id >> 8), f); fputc((int)id, f); }
    else if (id > 0x0000ffffu) { fputc((int)(id >> 16), f); fputc((int)(id >> 8), f); fputc((int)id, f); }
    else if (id > 0x000000ffu) { fputc((int)(id >> 8), f); fputc((int)id, f); }
    else fputc((int)id, f);
}
static void ebml_size(FILE *f, uint64_t n) {
    if (n < 0x7full) fputc((int)(0x80u | (uint8_t)n), f);
    else if (n < 0x3fffull) { uint16_t v = (uint16_t)(0x4000u | n); put_u16be(f, v); }
    else if (n < 0x1fffffull) { fputc((int)(0x20u | (uint8_t)(n >> 16)), f); put_u16be(f, (uint16_t)n); }
    else if (n < 0x0fffffffull) { put_u32be(f, (uint32_t)(0x10000000u | n)); }
    else if (n < 0x07ffffffffull) { fputc((int)(0x08u | (uint8_t)(n >> 32)), f); put_u32be(f, (uint32_t)n); }
    else if (n < 0x03ffffffffffull) { fputc((int)(0x04u | (uint8_t)(n >> 40)), f); fputc((int)(n >> 32), f); put_u32be(f, (uint32_t)n); }
    else if (n < 0x01ffffffffffffull) { fputc((int)(0x02u | (uint8_t)(n >> 48)), f); fputc((int)(n >> 40), f); fputc((int)(n >> 32), f); put_u32be(f, (uint32_t)n); }
    else { put_u64be(f, 0x0100000000000000ull | n); }
}
static void ebml_unknown_size8(FILE *f) { static const uint8_t u[8] = {0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff}; fwrite(u, 1, sizeof(u), f); }
static void ebml_elem(FILE *f, uint32_t id, const void *p, size_t n) { ebml_id(f, id); ebml_size(f, (uint64_t)n); fwrite(p, 1, n, f); }
static void ebml_uint(FILE *f, uint32_t id, uint64_t v) {
    uint8_t b[8]; size_t n = 1;
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> ((7 - i) * 8));
    while (n < 8 && b[8 - n - 1] != 0) n++;
    ebml_elem(f, id, b + 8 - n, n);
}

static int mb_ebml_id(membuf_t *b, uint32_t id) {
    if (id > 0x00ffffffu) return mb_u32be(b, id);
    if (id > 0x0000ffffu) { uint8_t x[3] = {(uint8_t)(id >> 16),(uint8_t)(id >> 8),(uint8_t)id}; return mb_put(b, x, 3); }
    if (id > 0x000000ffu) { uint8_t x[2] = {(uint8_t)(id >> 8),(uint8_t)id}; return mb_put(b, x, 2); }
    return mb_u8(b, (uint8_t)id);
}
static int mb_ebml_size(membuf_t *b, uint64_t n) {
    if (n < 0x7full) return mb_u8(b, (uint8_t)(0x80u | n));
    if (n < 0x3fffull) { uint16_t v = (uint16_t)(0x4000u | n); uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v}; return mb_put(b, x, 2); }
    if (n < 0x1fffffull) { uint8_t x[3] = {(uint8_t)(0x20u | (n >> 16)), (uint8_t)(n >> 8), (uint8_t)n}; return mb_put(b, x, 3); }
    if (n < 0x0fffffffull) return mb_u32be(b, (uint32_t)(0x10000000u | n));
    uint64_t v = 0x0100000000000000ull | n;
    return mb_u64be(b, v);
}
static int mb_ebml_elem(membuf_t *b, uint32_t id, const void *p, size_t n) { return mb_ebml_id(b, id) && mb_ebml_size(b, n) && mb_put(b, p, n); }
static int mb_ebml_uint(membuf_t *b, uint32_t id, uint64_t v) {
    uint8_t x[8]; size_t n = 1;
    for (int i = 0; i < 8; ++i) x[i] = (uint8_t)(v >> ((7 - i) * 8));
    while (n < 8 && x[8 - n - 1] != 0) n++;
    return mb_ebml_elem(b, id, x + 8 - n, n);
}
static int mb_ebml_str(membuf_t *b, uint32_t id, const char *s) { return mb_ebml_elem(b, id, s, strlen(s)); }
static int mb_ebml_master(membuf_t *b, uint32_t id, const membuf_t *child) { return mb_ebml_id(b, id) && mb_ebml_size(b, child->n) && mb_put(b, child->p, child->n); }

static int write_header(zmbv_mkv_recorder_t *r) {
    FILE *f = r->f;
    membuf_t ebml = {0}, info = {0}, tracks = {0}, te = {0}, video = {0}, priv = {0};
    int ok = 0;
    if (!mb_ebml_uint(&ebml, 0x4286, 1) || !mb_ebml_uint(&ebml, 0x42F7, 1) || !mb_ebml_uint(&ebml, 0x42F2, 4) || !mb_ebml_uint(&ebml, 0x42F3, 8) || !mb_ebml_str(&ebml, 0x4282, "matroska") || !mb_ebml_uint(&ebml, 0x4287, 4) || !mb_ebml_uint(&ebml, 0x4285, 2)) goto done;
    ebml_id(f, 0x1A45DFA3); ebml_size(f, ebml.n); fwrite(ebml.p, 1, ebml.n, f);
    ebml_id(f, 0x18538067); ebml_unknown_size8(f);
    if (!mb_ebml_uint(&info, 0x2AD7B1, MKV_TIMECODE_SCALE_NS) ||
        !mb_ebml_str(&info, 0x4D80, "WaifuFM headless") ||
        !mb_ebml_str(&info, 0x5741, "GP32emu-derived ZMBV recorder")) goto done;
    ebml_id(f, 0x1549A966); ebml_size(f, info.n); fwrite(info.p, 1, info.n, f);

    /* BITMAPINFOHEADER codec private for V_MS/VFW/FOURCC ZMBV. */
    uint32_t raw_bytes = (uint32_t)(r->w * r->h * 4);
    if (!mb_u32le(&priv, 40u) || !mb_u32le(&priv, (uint32_t)r->w) || !mb_u32le(&priv, (uint32_t)r->h) ||
        !mb_u16le(&priv, 1u) || !mb_u16le(&priv, 32u) || !mb_put(&priv, "ZMBV", 4) || !mb_u32le(&priv, raw_bytes) ||
        !mb_u32le(&priv, 2835u) || !mb_u32le(&priv, 2835u) || !mb_u32le(&priv, 0u) || !mb_u32le(&priv, 0u)) goto done;
    if (!mb_ebml_uint(&video, 0xB0, (uint64_t)r->w) || !mb_ebml_uint(&video, 0xBA, (uint64_t)r->h)) goto done;
    if (!mb_ebml_uint(&te, 0xD7, 1) || !mb_ebml_uint(&te, 0x73C5, 1) || !mb_ebml_uint(&te, 0x83, 1) ||
        !mb_ebml_uint(&te, 0x23E383, (uint64_t)((1000000000ull * (uint64_t)r->fps_den + (uint64_t)r->fps_num / 2ull) / (uint64_t)r->fps_num)) ||
        !mb_ebml_str(&te, 0x86, "V_MS/VFW/FOURCC") || !mb_ebml_elem(&te, 0x63A2, priv.p, priv.n) ||
        !mb_ebml_master(&te, 0xE0, &video)) goto done;
    if (!mb_ebml_master(&tracks, 0xAE, &te)) goto done;
    ebml_id(f, 0x1654AE6B); ebml_size(f, tracks.n); fwrite(tracks.p, 1, tracks.n, f);
    ok = ferror(f) == 0;

done:
    free(ebml.p); free(info.p); free(tracks.p); free(te.p); free(video.p); free(priv.p);
    return ok;
}

static int ensure_cluster(zmbv_mkv_recorder_t *r, uint64_t timestamp_ticks) {
    if (!r->cluster_open || timestamp_ticks < r->current_cluster_ticks || timestamp_ticks - r->current_cluster_ticks > MKV_CLUSTER_MAX_TICKS) {
        ebml_id(r->f, 0x1F43B675); ebml_unknown_size8(r->f);
        r->current_cluster_ticks = timestamp_ticks;
        ebml_uint(r->f, 0xE7, timestamp_ticks);
        r->cluster_open = 1;
    }
    return ferror(r->f) == 0;
}

static int write_simple_block(zmbv_mkv_recorder_t *r, uint8_t track, uint64_t timestamp_ticks, uint8_t flags, const void *data, size_t bytes) {
    if (!ensure_cluster(r, timestamp_ticks)) return 0;
    int64_t rel = (int64_t)timestamp_ticks - (int64_t)r->current_cluster_ticks;
    if (rel < -32768 || rel > 32767) { rec_seterr(r, "Matroska relative timestamp overflow"); return 0; }
    ebml_id(r->f, 0xA3);
    ebml_size(r->f, bytes + 4u);
    fputc((int)(0x80u | track), r->f);
    put_u16be(r->f, (uint16_t)(int16_t)rel);
    fputc((int)flags, r->f);
    fwrite(data, 1, bytes, r->f);
    return ferror(r->f) == 0;
}

zmbv_mkv_recorder_t *zmbv_mkv_open(const char *path, int width, int height, int fps_num, int fps_den, char *err, size_t err_len) {
    if (!path || !path[0] || width <= 0 || height <= 0 || fps_num <= 0 || fps_den <= 0) { seterr(err, err_len, "invalid recorder parameters"); return NULL; }
    zmbv_mkv_recorder_t *r = (zmbv_mkv_recorder_t *)calloc(1, sizeof(*r));
    if (!r) { seterr(err, err_len, "out of memory opening recorder"); return NULL; }
    r->w = width; r->h = height; r->fps_num = fps_num; r->fps_den = fps_den;
    size_t raw = (size_t)width * (size_t)height * 4u;
    r->raw_bgra = (uint8_t *)malloc(raw);
    r->zcomp_cap = compressBound((uLong)raw);
    r->zcomp = (uint8_t *)malloc(r->zcomp_cap);
    if (!r->raw_bgra || !r->zcomp) {
        free(r->raw_bgra); free(r->zcomp); free(r);
        seterr(err, err_len, "out of memory allocating recorder buffers");
        return NULL;
    }
    r->raw_cap = raw;
    r->f = fopen(path, "wb");
    if (!r->f) {
        char tmp[256]; snprintf(tmp, sizeof(tmp), "recording open failed: %s", strerror(errno));
        free(r->raw_bgra); free(r->zcomp); free(r); seterr(err, err_len, tmp); return NULL;
    }
    if (!write_header(r)) {
        char tmp[256]; snprintf(tmp, sizeof(tmp), "recording header write failed");
        fclose(r->f); free(r->raw_bgra); free(r->zcomp); free(r); seterr(err, err_len, tmp); return NULL;
    }
    return r;
}

int zmbv_mkv_add_indexed_frame(zmbv_mkv_recorder_t *r, const uint8_t *fb8, const uint8_t *palette_rgb, uint64_t frame_index) {
    if (!r || !fb8 || !palette_rgb) return 0;
    size_t pixels = (size_t)r->w * (size_t)r->h;
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t idx = fb8[i];
        const uint8_t *rgb = palette_rgb + (size_t)idx * 3u;
        r->raw_bgra[i * 4u + 0u] = rgb[2];
        r->raw_bgra[i * 4u + 1u] = rgb[1];
        r->raw_bgra[i * 4u + 2u] = rgb[0];
        r->raw_bgra[i * 4u + 3u] = 0u;
    }
    uLongf comp_len = (uLongf)r->zcomp_cap;
    if (compress2(r->zcomp, &comp_len, r->raw_bgra, (uLong)r->raw_cap, Z_BEST_SPEED) != Z_OK) {
        rec_seterr(r, "ZMBV deflate failed");
        return 0;
    }
    size_t packet_len = 7u + (size_t)comp_len;
    uint8_t *packet = (uint8_t *)malloc(packet_len);
    if (!packet) { rec_seterr(r, "out of memory writing ZMBV packet"); return 0; }
    packet[0] = ZMBV_KEYFRAME;
    packet[1] = 0; packet[2] = 1; packet[3] = 1; packet[4] = ZMBV_FMT_32BPP; packet[5] = ZMBV_BLOCK; packet[6] = ZMBV_BLOCK;
    memcpy(packet + 7, r->zcomp, (size_t)comp_len);
    uint64_t ts = (frame_index * (uint64_t)r->fps_den * MKV_TIMECODE_TICKS_PER_SECOND + (uint64_t)r->fps_num / 2ull) / (uint64_t)r->fps_num;
    int ok = write_simple_block(r, 1u, ts, 0x80u, packet, packet_len);
    free(packet);
    if (!ok) rec_seterr(r, "failed to write video block");
    return ok;
}

int zmbv_mkv_close(zmbv_mkv_recorder_t *r) {
    if (!r) return 1;
    int ok = 1;
    if (r->f) {
        fflush(r->f);
        ok = ferror(r->f) == 0;
        fclose(r->f);
    }
    free(r->raw_bgra);
    free(r->zcomp);
    free(r);
    return ok;
}

void zmbv_mkv_abort(zmbv_mkv_recorder_t *r) {
    if (!r) return;
    if (r->f) fclose(r->f);
    free(r->raw_bgra);
    free(r->zcomp);
    free(r);
}
