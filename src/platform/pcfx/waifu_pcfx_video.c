#include "waifu_pcfx_video.h"
#include "fastking.h"

#include <eris/king.h>
#include <eris/tetsu.h>
#include <eris/7up.h>
#include <eris/low/7up.h>
#include <eris/v810.h>
#include <string.h>
#include "waifu_assets.h"
#include "assets.h"
#include "pcfx_palette_assets.h"
#include "title_asset.h"
#include "font_menudata.h"

#define WAIFU_PCFX_W WAIFU_FM_WIDTH
#define WAIFU_PCFX_H WAIFU_FM_HEIGHT
#define WAIFU_PCFX_FRAME_BYTES (WAIFU_PCFX_W * WAIFU_PCFX_H)
#define WAIFU_PCFX_FRAME_WORDS (WAIFU_PCFX_FRAME_BYTES / 2)
#define WAIFU_PCFX_TITLE_16M_WORDS (WAIFU_PCFX_W * WAIFU_PCFX_H)
/* KING BG0 is configured as a 256x256 8bpp plane.  The visible game frame is
   256x240, but the hardware page stride must remain 256x256 so page 1 does
   not alias page 0's hidden bottom 16 scanlines. */
#define WAIFU_PCFX_PAGE_STRIDE_WORDS ((WAIFU_PCFX_W * 256) / 2)
#define WAIFU_PCFX_KING_BG_PAGE_0 0
#define WAIFU_PCFX_KING_BG_PAGE_1 (WAIFU_PCFX_PAGE_STRIDE_WORDS / 1024)
#define WAIFU_PCFX_NEUTRAL_BLACK 0x0088u
#define WAIFU_PCFX_16M_BLACK_Y 0x0101u
#define WAIFU_PCFX_16M_BLACK_UV 0x8080u
#define WAIFU_PCFX_BLACK_WORD ((uint16_t)((IDX_BLACK << 8) | IDX_BLACK))
#ifndef WAIFU_PCFX_DIRTY_PRESENT
#define WAIFU_PCFX_DIRTY_PRESENT 1
#endif
#define WAIFU_PCFX_DIRTY_FULL_THRESHOLD_BYTES (WAIFU_PCFX_FRAME_BYTES * 3 / 4)
#define WAIFU_PCFX_DIRTY_BLOCK_W 16
#define WAIFU_PCFX_DIRTY_BLOCKS_X (WAIFU_PCFX_W / WAIFU_PCFX_DIRTY_BLOCK_W)
#define WAIFU_PCFX_DIRTY_MAX_ROW_RUNS WAIFU_PCFX_DIRTY_BLOCKS_X
#define WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS 1024

#if defined(__GNUC__)
#define WAIFU_PCFX_NOINLINE __attribute__((noinline))
#define WAIFU_PCFX_COLD __attribute__((noinline,cold))
#else
#define WAIFU_PCFX_NOINLINE
#define WAIFU_PCFX_COLD
#endif

static uint16_t g_king_microprog[16];
static const WaifuBigArtDraw *g_pcfx_direct_big_art_draws = 0;
static int g_pcfx_direct_big_art_draw_count = 0;
static int g_pcfx_dirty_scan_y = 0;
static uint16_t g_title16m_composite[WAIFU_PCFX_TITLE_16M_WORDS] __attribute__((aligned(4)));

static inline __attribute__((always_inline)) int pcfx_block16_is_direct_big_art(int y, int block)
{
    int x0 = block * WAIFU_PCFX_DIRTY_BLOCK_W;
    int x1 = x0 + WAIFU_PCFX_DIRTY_BLOCK_W;
    for (int i = 0; i < g_pcfx_direct_big_art_draw_count; ++i) {
        const WaifuBigArtDraw *d = &g_pcfx_direct_big_art_draws[i];
        if (y < d->y || y >= d->y + WAIFU_BIG_H) continue;
        if (x1 <= d->x || x0 >= d->x + WAIFU_BIG_W) continue;
        return 1;
    }
    return 0;
}

struct WaifuPcfxVideo {
    int front_page;
    int back_page;
    int pending_title_page_flip;
    int pending_title_kram_page;
    int pending_title_front_page;
    int pending_title_back_page;
    int initialized;
    WaifuFmPaletteId active_palette;
    int active_fade_q8;
    uint16_t base_yuv[256];
    int have_base_yuv;
    uint32_t last_frame_sum;
    uint32_t last_frame_mix;
    uint32_t last_title_dirty_serial;
    uint8_t title16m_page_valid[2];
    int vdc_overlay_ready;
    int vdc_overlay_shutdown_countdown;
    int have_last_frame;
    WaifuPcfxVideoMode mode;
    WaifuPcfxVdcBackground vdc_bg;
#if WAIFU_PCFX_DIRTY_PRESENT
    uint8_t page_shadow[2][WAIFU_PCFX_FRAME_BYTES] __attribute__((aligned(4)));
    uint8_t page_shadow_valid[2];
#endif
};

static WaifuPcfxVideo g_video;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Convert PC/headless RGB888 palette entries to the PC-FX/TETSU Y8U4V4
   palette format.  Do not use the naive BT.601 formula here: the PC-FX
   emulator and hardware treat U/V as 4-bit chroma nibbles expanded to the high
   four bits before YUV->RGB conversion, so a direct formula undersaturates the
   game badly.  This fixed-point search mirrors Cascade FX's offline converter
   and chooses the closest representable PC-FX color for every RGB entry. */
static uint16_t rgb888_to_pcfx_yuv(uint8_t r, uint8_t g, uint8_t b)
{
    int best_err = 0x7fffffff;
    int best_y = 0;
    int best_u4 = 8;
    int best_v4 = 8;

    for (int u4 = 0; u4 < 16; ++u4) {
        int u = (u4 << 4) - 128;
        for (int v4 = 0; v4 < 16; ++v4) {
            int v = (v4 << 4) - 128;

            /* Q10 approximation of the emulator's PC-FX YUV matrix offsets:
               R = Y - 0.000039U + 1.139828V
               G = Y - 0.394610U - 0.580500V
               B = Y + 2.031999U - 0.000481V */
            int ro = (0 * u + 1167 * v) / 1024;
            int go = (-404 * u - 594 * v) / 1024;
            int bo = (2081 * u - 1 * v) / 1024;

            /* Weighted least-squares luma choice, matching the offline tools'
               green/luma-biased visual error metric. */
            int y = (2 * ((int)r - ro) + 4 * ((int)g - go) + ((int)b - bo) + 3) / 7;
            y = clamp_int(y, 0, 255);

            int rr = clamp_int(y + ro, 0, 255);
            int gg = clamp_int(y + go, 0, 255);
            int bb = clamp_int(y + bo, 0, 255);
            int dr = rr - (int)r;
            int dg = gg - (int)g;
            int db = bb - (int)b;
            int err = 2 * dr * dr + 4 * dg * dg + db * db;
            if (err < best_err) {
                best_err = err;
                best_y = y;
                best_u4 = u4;
                best_v4 = v4;
            }
        }
    }

    return (uint16_t)((best_y << 8) | (best_u4 << 4) | best_v4);
}

static inline __attribute__((always_inline)) int page_word_offset(int page)
{
    return page ? WAIFU_PCFX_PAGE_STRIDE_WORDS : 0;
}

static inline __attribute__((always_inline)) int page_bat_offset(int page)
{
    return page ? WAIFU_PCFX_KING_BG_PAGE_1 : WAIFU_PCFX_KING_BG_PAGE_0;
}

static inline __attribute__((always_inline)) uint32_t title16m_page_word_offset(int page)
{
    /* 16M title/menu double-buffer: two 256x240 YUV surfaces fit in one BG
       KRAM page.  The visible surface is selected by BG0/BG0SUB CG base
       0 or 64; writes go to the hidden surface only. */
    return page ? 65536u : 0u;
}

static inline __attribute__((always_inline)) int title16m_bg_cg_page(int page)
{
    return page ? 64 : 0;
}

/* Presenter-local copy macro.  This intentionally expands to asm volatile at
   each hot use site instead of relying on an out-of-line helper.  The V810
   path is compact: 32 bytes per loop, plus a tiny byte tail. */
#if defined(__v810__)
#define pcfx_copy_bytes_inline(dst_arg, src_arg, count_arg) do { \
    int __pcfx_count = (int)(count_arg); \
    if (__pcfx_count > 0) { \
        uint8_t *__pcfx_dst = (uint8_t *)(dst_arg); \
        const uint8_t *__pcfx_src = (const uint8_t *)(src_arg); \
        uint32_t __pcfx_n = (uint32_t)__pcfx_count; \
        uint32_t __pcfx_groups; \
        __asm__ volatile ( \
            "mov %[n],%[groups]\n" \
            "shr 5,%[groups]\n" \
            "cmp 0,%[groups]\n" \
            "be 2f\n" \
            "1:\n" \
            "ld.w 0[%[src]],r10\n" \
            "st.w r10,0[%[dst]]\n" \
            "ld.w 4[%[src]],r10\n" \
            "st.w r10,4[%[dst]]\n" \
            "ld.w 8[%[src]],r10\n" \
            "st.w r10,8[%[dst]]\n" \
            "ld.w 12[%[src]],r10\n" \
            "st.w r10,12[%[dst]]\n" \
            "ld.w 16[%[src]],r10\n" \
            "st.w r10,16[%[dst]]\n" \
            "ld.w 20[%[src]],r10\n" \
            "st.w r10,20[%[dst]]\n" \
            "ld.w 24[%[src]],r10\n" \
            "st.w r10,24[%[dst]]\n" \
            "ld.w 28[%[src]],r10\n" \
            "st.w r10,28[%[dst]]\n" \
            "addi 32,%[src],%[src]\n" \
            "addi 32,%[dst],%[dst]\n" \
            "add -1,%[groups]\n" \
            "bne 1b\n" \
            "2:\n" \
            "andi 31,%[n],%[n]\n" \
            "cmp 0,%[n]\n" \
            "be 4f\n" \
            "3:\n" \
            "ld.b 0[%[src]],r10\n" \
            "st.b r10,0[%[dst]]\n" \
            "add 1,%[src]\n" \
            "add 1,%[dst]\n" \
            "add -1,%[n]\n" \
            "bne 3b\n" \
            "4:\n" \
            : [dst] "+r" (__pcfx_dst), [src] "+r" (__pcfx_src), \
              [n] "+r" (__pcfx_n), [groups] "=&r" (__pcfx_groups) \
            : \
            : "r10", "memory"); \
    } \
} while (0)
#else
#define pcfx_copy_bytes_inline(dst_arg, src_arg, count_arg) do { \
    int __pcfx_count = (int)(count_arg); \
    if (__pcfx_count > 0) { \
        memcpy((uint8_t *)(dst_arg), (const uint8_t *)(src_arg), (size_t)__pcfx_count); \
    } \
} while (0)
#endif


static inline __attribute__((always_inline)) void pcfx_king_set_bg_kram_page_inline(int page)
{
#if defined(__v810__)
    uint32_t reg;
    uint32_t ps = page ? 0x0010u : 0u;
    /* KING PageSetting bit 4 selects the BG BAT/CG physical KRAM page in
       pcfxemu's accurate and fast BG renderers.  Use a direct register write
       instead of eris_king_set_kram_pages() here so the 16M title flip changes
       exactly the BG page and does not disturb the CG base registers. */
    __asm__ volatile (
        "movea 15,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.w %[ps],0x604[r0]\n"
        : [reg] "=&r" (reg)
        : [ps] "r" (ps)
        : "memory");
#else
    eris_king_set_kram_pages(0, page ? 1 : 0, 0, 0);
#endif
}

static inline __attribute__((always_inline)) void pcfx_king_set_bg0_page_inline(int cg_page)
{
#if defined(__v810__)
    uint32_t page = (uint32_t)cg_page;
    uint32_t reg;
    /* Inline eris_king_set_bat_cg_addr(KING_BG0,0,page) and the matching
       BG0SUB call.  This runs every presented frame, so avoiding two jal/rts
       pairs is worthwhile and the sequence is still tiny. */
    __asm__ volatile (
        "movea 32,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h r0,0x604[r0]\n"
        "add 1,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[page],0x604[r0]\n"
        "add 1,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h r0,0x604[r0]\n"
        "add 1,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[page],0x604[r0]\n"
        : [reg] "=&r" (reg)
        : [page] "r" (page)
        : "memory");
#else
    eris_king_set_bat_cg_addr(KING_BG0, 0, (uint32_t)cg_page);
    eris_king_set_bat_cg_addr(KING_BG0SUB, 0, (uint32_t)cg_page);
#endif
}

static inline __attribute__((always_inline)) void pcfx_schedule_title_page_flip(WaifuPcfxVideo *video, int kram_page, int new_front, int new_back)
{
    video->pending_title_kram_page = kram_page;
    video->pending_title_front_page = new_front;
    video->pending_title_back_page = new_back;
    video->pending_title_page_flip = 1;
}

#if WAIFU_PCFX_DIRTY_PRESENT
typedef struct PcfxDirtyRun {
    uint8_t x0b;
    uint8_t x1b;
    uint8_t black;
} PcfxDirtyRun;

typedef struct PcfxDirtyBand {
    uint8_t x0b;
    uint8_t x1b;
    uint8_t y0;
    uint8_t y1;
    uint8_t black;
    uint8_t used;
    uint8_t matched;
} PcfxDirtyBand;

typedef struct PcfxDirtyPlanStats {
    int dirty_blocks;
    int row_runs;
} PcfxDirtyPlanStats;

static inline __attribute__((always_inline)) int pcfx_block16_dirty(const uint8_t *cur, const uint8_t *old, int block)
{
    if (pcfx_block16_is_direct_big_art(g_pcfx_dirty_scan_y, block)) return 0;
    const uint32_t *a = (const uint32_t *)(cur + block * WAIFU_PCFX_DIRTY_BLOCK_W);
    const uint32_t *b = (const uint32_t *)(old + block * WAIFU_PCFX_DIRTY_BLOCK_W);
    return (a[0] != b[0]) | (a[1] != b[1]) | (a[2] != b[2]) | (a[3] != b[3]);
}

static inline __attribute__((always_inline)) int pcfx_block16_is_black(const uint8_t *cur, int block)
{
    const uint32_t *a = (const uint32_t *)(cur + block * WAIFU_PCFX_DIRTY_BLOCK_W);
    const uint32_t black32 = 0xffffffffu;
    return (a[0] == black32) & (a[1] == black32) & (a[2] == black32) & (a[3] == black32);
}

static inline __attribute__((always_inline)) int pcfx_dirty_collect_row_runs(const uint8_t *cur, const uint8_t *old, PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS])
{
    int count = 0;
    int b = 0;
    while (b < WAIFU_PCFX_DIRTY_BLOCKS_X) {
        if (!pcfx_block16_dirty(cur, old, b)) { ++b; continue; }
        int black = pcfx_block16_is_black(cur, b);
        int start = b++;
        while (b < WAIFU_PCFX_DIRTY_BLOCKS_X &&
               pcfx_block16_dirty(cur, old, b) &&
               pcfx_block16_is_black(cur, b) == black) {
            ++b;
        }
        runs[count].x0b = (uint8_t)start;
        runs[count].x1b = (uint8_t)(b - 1);
        runs[count].black = (uint8_t)black;
        ++count;
    }
    return count;
}

static WAIFU_PCFX_NOINLINE PcfxDirtyPlanStats pcfx_dirty_plan_stats(const uint8_t *cur, const uint8_t *old)
{
    PcfxDirtyPlanStats stats;
    stats.dirty_blocks = 0;
    stats.row_runs = 0;
    PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS];
    for (int y = 0; y < WAIFU_PCFX_H; ++y) {
        const uint8_t *a = cur + y * WAIFU_PCFX_W;
        const uint8_t *b = old + y * WAIFU_PCFX_W;
        g_pcfx_dirty_scan_y = y;
        int rc = pcfx_dirty_collect_row_runs(a, b, runs);
        stats.row_runs += rc;
        for (int i = 0; i < rc; ++i) {
            stats.dirty_blocks += (int)runs[i].x1b - (int)runs[i].x0b + 1;
        }
    }
    return stats;
}

static inline __attribute__((always_inline)) void pcfx_fill_black_shadow_rect(uint8_t *dst, int x0, int y0, int width, int rows)
{
    uint8_t *p = dst + y0 * WAIFU_PCFX_W + x0;
    for (int y = 0; y < rows; ++y) {
#if defined(__v810__)
        int count = width;
        uint8_t *row = p;
        uint32_t n = (uint32_t)count;
        uint32_t groups;
        uint32_t black = 0xffffffffu;
        __asm__ volatile (
            "mov %[n],%[groups]\n"
            "shr 5,%[groups]\n"
            "cmp 0,%[groups]\n"
            "be 2f\n"
            "1:\n"
            "st.w %[black],0[%[row]]\n"
            "st.w %[black],4[%[row]]\n"
            "st.w %[black],8[%[row]]\n"
            "st.w %[black],12[%[row]]\n"
            "st.w %[black],16[%[row]]\n"
            "st.w %[black],20[%[row]]\n"
            "st.w %[black],24[%[row]]\n"
            "st.w %[black],28[%[row]]\n"
            "addi 32,%[row],%[row]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            "2:\n"
            "andi 31,%[n],%[n]\n"
            "cmp 0,%[n]\n"
            "be 4f\n"
            "3:\n"
            "st.b %[black],0[%[row]]\n"
            "add 1,%[row]\n"
            "add -1,%[n]\n"
            "bne 3b\n"
            "4:\n"
            : [row] "+r" (row), [n] "+r" (n), [groups] "=&r" (groups)
            : [black] "r" (black)
            : "memory");
#else
        memset(p, IDX_BLACK, (size_t)width);
#endif
        p += WAIFU_PCFX_W;
    }
}

static inline __attribute__((always_inline)) void pcfx_kram_upload_rect_bytes_inline(const uint8_t *src_arg, int page_word_offset_arg, int x0_arg, int y0_arg, int row_bytes_arg, int rows_arg)
{
#if defined(__v810__)
    const uint8_t *src = src_arg + y0_arg * WAIFU_PCFX_W + x0_arg;
    uint32_t word_addr = (uint32_t)(page_word_offset_arg + y0_arg * 128 + (x0_arg >> 1));
    uint32_t rows = (uint32_t)rows_arg;
    uint32_t row_bytes = (uint32_t)row_bytes_arg;
    uint32_t src_delta = (uint32_t)(WAIFU_PCFX_W - row_bytes_arg);
    uint32_t inc = (uint32_t)(1u << 18);
    uint32_t row_end, tmp_addr, reg, data, t1, t2, t3;
    if (row_bytes == 0 || rows == 0) return;
    __asm__ volatile (
        "cmp 0,%[rows]\n"
        "be 9f\n"
        "1:\n"
        "mov %[addr],%[tmp]\n"
        "or %[inc],%[tmp]\n"
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.w %[tmp],0x604[r0]\n"
        "movea 14,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "mov %[src],%[row_end]\n"
        "add %[row_bytes],%[row_end]\n"
        "2:\n"
        "ld.w 0[%[src]],%[data]\n"
        "mov %[data],%[t1]\n"
        "shr 8,%[t1]\n"
        "mov %[data],%[t2]\n"
        "andi 255,%[t1],%[t3]\n"
        "shl 8,%[t2]\n"
        "andi 65280,%[t1],%[t1]\n"
        "shr 24,%[data]\n"
        "or %[t2],%[t3]\n"
        "or %[data],%[t1]\n"
        "out.h %[t3],0x604[r0]\n"
        "out.h %[t1],0x604[r0]\n"
        "ld.w 4[%[src]],%[data]\n"
        "mov %[data],%[t1]\n"
        "shr 8,%[t1]\n"
        "mov %[data],%[t2]\n"
        "andi 255,%[t1],%[t3]\n"
        "shl 8,%[t2]\n"
        "andi 65280,%[t1],%[t1]\n"
        "shr 24,%[data]\n"
        "or %[t2],%[t3]\n"
        "or %[data],%[t1]\n"
        "out.h %[t3],0x604[r0]\n"
        "out.h %[t1],0x604[r0]\n"
        "ld.w 8[%[src]],%[data]\n"
        "mov %[data],%[t1]\n"
        "shr 8,%[t1]\n"
        "mov %[data],%[t2]\n"
        "andi 255,%[t1],%[t3]\n"
        "shl 8,%[t2]\n"
        "andi 65280,%[t1],%[t1]\n"
        "shr 24,%[data]\n"
        "or %[t2],%[t3]\n"
        "or %[data],%[t1]\n"
        "out.h %[t3],0x604[r0]\n"
        "out.h %[t1],0x604[r0]\n"
        "ld.w 12[%[src]],%[data]\n"
        "mov %[data],%[t1]\n"
        "shr 8,%[t1]\n"
        "mov %[data],%[t2]\n"
        "andi 255,%[t1],%[t3]\n"
        "shl 8,%[t2]\n"
        "andi 65280,%[t1],%[t1]\n"
        "shr 24,%[data]\n"
        "or %[t2],%[t3]\n"
        "or %[data],%[t1]\n"
        "out.h %[t3],0x604[r0]\n"
        "out.h %[t1],0x604[r0]\n"
        "addi 16,%[src],%[src]\n"
        "cmp %[row_end],%[src]\n"
        "bl 2b\n"
        "add %[src_delta],%[src]\n"
        "addi 128,%[addr],%[addr]\n"
        "add -1,%[rows]\n"
        "bne 1b\n"
        "9:\n"
        : [src] "+r" (src), [addr] "+r" (word_addr), [rows] "+r" (rows),
          [row_end] "=&r" (row_end), [tmp] "=&r" (tmp_addr), [reg] "=&r" (reg),
          [data] "=&r" (data), [t1] "=&r" (t1), [t2] "=&r" (t2), [t3] "=&r" (t3)
        : [row_bytes] "r" (row_bytes), [src_delta] "r" (src_delta), [inc] "r" (inc)
        : "memory");
#else
    king_kram_upload_rect_256_bytes(src_arg, page_word_offset_arg, x0_arg, y0_arg, row_bytes_arg, rows_arg);
#endif
}

static inline __attribute__((always_inline)) void pcfx_kram_clear_rect_black_inline(int page_word_offset_arg, int x0_arg, int y0_arg, int width_bytes_arg, int rows_arg)
{
#if defined(__v810__)
    int page_word_offset = page_word_offset_arg;
    int x0 = x0_arg;
    int y0 = y0_arg;
    int width_words = width_bytes_arg >> 1;
    int rows = rows_arg;
    if (width_words <= 0 || rows <= 0) return;
    uint32_t word_addr = (uint32_t)(page_word_offset + y0 * 128 + (x0 >> 1));
    uint32_t r = (uint32_t)rows;
    uint32_t w = (uint32_t)width_words;
    uint32_t black = (uint32_t)WAIFU_PCFX_BLACK_WORD;
    uint32_t inc = (uint32_t)(1u << 18);
    uint32_t tmp_addr, groups, tail, reg;
    __asm__ volatile (
        "cmp 0,%[rows]\n"
        "be 9f\n"
        "1:\n"
        "mov %[addr],%[tmp]\n"
        "or %[inc],%[tmp]\n"
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.w %[tmp],0x604[r0]\n"
        "movea 14,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "mov %[width],%[groups]\n"
        "shr 4,%[groups]\n"
        "cmp 0,%[groups]\n"
        "be 3f\n"
        "2:\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "out.h %[black],0x604[r0]\n"
        "add -1,%[groups]\n"
        "bne 2b\n"
        "3:\n"
        "mov %[width],%[tail]\n"
        "andi 15,%[tail],%[tail]\n"
        "cmp 0,%[tail]\n"
        "be 5f\n"
        "4:\n"
        "out.h %[black],0x604[r0]\n"
        "add -1,%[tail]\n"
        "bne 4b\n"
        "5:\n"
        "addi 128,%[addr],%[addr]\n"
        "add -1,%[rows]\n"
        "bne 1b\n"
        "9:\n"
        : [addr] "+r" (word_addr), [rows] "+r" (r), [groups] "=&r" (groups),
          [tail] "=&r" (tail), [tmp] "=&r" (tmp_addr), [reg] "=&r" (reg)
        : [width] "r" (w), [black] "r" (black), [inc] "r" (inc)
        : "memory");
#else
    (void)page_word_offset_arg; (void)x0_arg; (void)y0_arg; (void)width_bytes_arg; (void)rows_arg;
#endif
}

static inline __attribute__((always_inline)) void pcfx_shadow_copy_rect(uint8_t *dst, const uint8_t *src, int x0, int y0, int width, int rows)
{
    dst += y0 * WAIFU_PCFX_W + x0;
    src += y0 * WAIFU_PCFX_W + x0;
    for (int y = 0; y < rows; ++y) {
        pcfx_copy_bytes_inline(dst, src, width);
        dst += WAIFU_PCFX_W;
        src += WAIFU_PCFX_W;
    }
}


static int pcfx_shadow_rect_equals(const uint8_t *shadow, const uint8_t *framebuffer, int x0, int y0, int width, int rows)
{
    const uint8_t *s = shadow + y0 * WAIFU_PCFX_W + x0;
    const uint8_t *f = framebuffer + y0 * WAIFU_PCFX_W + x0;
    for (int y = 0; y < rows; ++y) {
        if (memcmp(s, f, (size_t)width) != 0) return 0;
        s += WAIFU_PCFX_W;
        f += WAIFU_PCFX_W;
    }
    return 1;
}

static WAIFU_PCFX_COLD void pcfx_upload_direct_big_art_rects(uint8_t *shadow, const uint8_t *framebuffer, int page_word_offset_value)
{
    for (int i = 0; i < g_pcfx_direct_big_art_draw_count; ++i) {
        const WaifuBigArtDraw *d = &g_pcfx_direct_big_art_draws[i];
        if (d->x < 0 || d->y < 0 || d->x + WAIFU_BIG_W > WAIFU_PCFX_W || d->y + WAIFU_BIG_H > WAIFU_PCFX_H) continue;

        /* The dirty scanner suppresses complete 16-pixel horizontal blocks that
           intersect the direct big-art draw.  The upload must therefore cover
           the same block-aligned area, not only the 112x112 art pixels.  The
           old exact-rect upload left the card frame/nearby pixels in the
           skipped edge blocks stale, and odd x positions also misaligned KING
           byte pairs.  Block-aligning the upload keeps KRAM writes even, 16-byte
           streamed, and congruent with the skipped dirty blocks. */
        int x0 = (d->x / WAIFU_PCFX_DIRTY_BLOCK_W) * WAIFU_PCFX_DIRTY_BLOCK_W;
        int x1 = ((d->x + WAIFU_BIG_W + WAIFU_PCFX_DIRTY_BLOCK_W - 1) / WAIFU_PCFX_DIRTY_BLOCK_W) * WAIFU_PCFX_DIRTY_BLOCK_W;
        if (x0 < 0) x0 = 0;
        if (x1 > WAIFU_PCFX_W) x1 = WAIFU_PCFX_W;
        int width = x1 - x0;
        if (width <= 0) continue;

        /* Static card-check screens draw the same large card for many frames.
           Once both hidden/display pages have the same aligned card rectangle,
           do not re-stream it.  Any battle flash, burn, damage text, or card
           movement changes the CPU framebuffer rectangle and forces an upload. */
        if (pcfx_shadow_rect_equals(shadow, framebuffer, x0, d->y, width, WAIFU_BIG_H)) continue;
        pcfx_kram_upload_rect_bytes_inline(framebuffer, page_word_offset_value, x0, d->y, width, WAIFU_BIG_H);
        pcfx_shadow_copy_rect(shadow, framebuffer, x0, d->y, width, WAIFU_BIG_H);
    }
}

static inline __attribute__((always_inline)) void pcfx_flush_dirty_band(uint8_t *shadow, const uint8_t *framebuffer,
                                  int page_word_offset_value, const PcfxDirtyBand *band)
{
    if (!band->used) return;
    int x0 = (int)band->x0b * WAIFU_PCFX_DIRTY_BLOCK_W;
    int width = ((int)band->x1b - (int)band->x0b + 1) * WAIFU_PCFX_DIRTY_BLOCK_W;
    int y0 = (int)band->y0;
    int rows = (int)band->y1 - (int)band->y0 + 1;
    if (band->black) {
        pcfx_kram_clear_rect_black_inline(page_word_offset_value, x0, y0, width, rows);
        pcfx_fill_black_shadow_rect(shadow, x0, y0, width, rows);
    } else {
        pcfx_kram_upload_rect_bytes_inline(framebuffer, page_word_offset_value, x0, y0, width, rows);
        pcfx_shadow_copy_rect(shadow, framebuffer, x0, y0, width, rows);
    }
}

static WAIFU_PCFX_NOINLINE void pcfx_present_dirty_bands(uint8_t *shadow, const uint8_t *framebuffer, int page_word_offset_value)
{
    PcfxDirtyBand active[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS];
    PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS];
    int active_count = 0;
    for (int i = 0; i < WAIFU_PCFX_DIRTY_MAX_ROW_RUNS; ++i) active[i].used = 0;

    for (int y = 0; y < WAIFU_PCFX_H; ++y) {
        for (int i = 0; i < active_count; ++i) active[i].matched = 0;
        g_pcfx_dirty_scan_y = y;
        int run_count = pcfx_dirty_collect_row_runs(framebuffer + y * WAIFU_PCFX_W,
                                                    shadow + y * WAIFU_PCFX_W,
                                                    runs);
        for (int r = 0; r < run_count; ++r) {
            int found = -1;
            for (int i = 0; i < active_count; ++i) {
                if (active[i].used && !active[i].matched &&
                    active[i].x0b == runs[r].x0b &&
                    active[i].x1b == runs[r].x1b &&
                    active[i].black == runs[r].black) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                active[found].y1 = (uint8_t)y;
                active[found].matched = 1;
            } else if (active_count < WAIFU_PCFX_DIRTY_MAX_ROW_RUNS) {
                PcfxDirtyBand *b = &active[active_count++];
                b->x0b = runs[r].x0b;
                b->x1b = runs[r].x1b;
                b->black = runs[r].black;
                b->y0 = (uint8_t)y;
                b->y1 = (uint8_t)y;
                b->used = 1;
                b->matched = 1;
            }
        }

        int write = 0;
        for (int i = 0; i < active_count; ++i) {
            if (active[i].used && !active[i].matched) {
                pcfx_flush_dirty_band(shadow, framebuffer, page_word_offset_value, &active[i]);
            } else if (active[i].used) {
                if (write != i) {
                    active[write].x0b = active[i].x0b;
                    active[write].x1b = active[i].x1b;
                    active[write].y0 = active[i].y0;
                    active[write].y1 = active[i].y1;
                    active[write].black = active[i].black;
                    active[write].used = active[i].used;
                    active[write].matched = active[i].matched;
                }
                ++write;
            }
        }
        active_count = write;
    }
    for (int i = 0; i < active_count; ++i) {
        if (active[i].used) pcfx_flush_dirty_band(shadow, framebuffer, page_word_offset_value, &active[i]);
    }
}
#endif

#if !WAIFU_PCFX_DIRTY_PRESENT
static void pcfx_frame_signature(const uint8_t *framebuffer, uint32_t *out_sum, uint32_t *out_mix)
{
    const uint32_t *p = (const uint32_t *)framebuffer;
    uint32_t sum = 0x9e3779b9u;
    uint32_t mix = 0x85ebca6bu;
    for (int i = 0; i < WAIFU_PCFX_FRAME_BYTES / 4; ++i) {
        uint32_t v = p[i];
        sum += v;
        mix ^= v + 0x9e3779b9u + (mix << 6) + (mix >> 2);
    }
    *out_sum = sum;
    *out_mix = mix;
}
#endif

static void king_seek_write_words(uint32_t word_addr)
{
#if defined(__v810__)
    uint32_t reg;
    uint32_t addr = word_addr | (1u << 18);
    uint32_t lo = addr & 0xffffu;
    uint32_t hi = addr >> 16;
    /* eris_king_set_kram_write() uses out.w.  For physical KRAM page 1 we
       need bit 31 of KRAMWA to survive exactly, so write the 32-bit KRAMWA
       register as explicit low/high halfwords before streaming register 0x0E. */
    __asm__ volatile (
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[lo],0x604[r0]\n"
        "out.h %[hi],0x606[r0]\n"
        : [reg] "=&r" (reg)
        : [lo] "r" (lo), [hi] "r" (hi)
        : "memory");
#else
    eris_king_set_kram_write(word_addr, 1);
#endif
}



static uint8_t pcfx_clamp_u8_i(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static void pcfx_yuv16m_pixel_to_rgb(uint16_t yword, uint16_t uvword, int second, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int y = second ? (yword & 0xff) : ((yword >> 8) & 0xff);
    int u = ((uvword >> 8) & 0xff) - 128;
    int v = (uvword & 0xff) - 128;
    int ro = (v * 146) >> 7;
    int go = -((u * 50 + v * 74) >> 7);
    int bo = (u * 260) >> 7;
    *r = pcfx_clamp_u8_i(y + ro);
    *g = pcfx_clamp_u8_i(y + go);
    *b = pcfx_clamp_u8_i(y + bo);
}

static void pcfx_rgb_pair_to_yuv16m_words(uint8_t r0, uint8_t g0, uint8_t b0,
                                           uint8_t r1, uint8_t g1, uint8_t b1,
                                           uint16_t *out_y, uint16_t *out_uv)
{
    int y0 = (77 * (int)r0 + 150 * (int)g0 + 29 * (int)b0) >> 8;
    int y1 = (77 * (int)r1 + 150 * (int)g1 + 29 * (int)b1) >> 8;
    int u0, v0, u1, v1, u, v;
    if (y0 <= 0) y0 = 1;
    if (y1 <= 0) y1 = 1;
    u0 = 128 + (((int)b0 - y0) * 63 >> 7);
    v0 = 128 + (((int)r0 - y0) * 112 >> 7);
    u1 = 128 + (((int)b1 - y1) * 63 >> 7);
    v1 = 128 + (((int)r1 - y1) * 112 >> 7);
    u = ((int)pcfx_clamp_u8_i(u0) + (int)pcfx_clamp_u8_i(u1) + 1) >> 1;
    v = ((int)pcfx_clamp_u8_i(v0) + (int)pcfx_clamp_u8_i(v1) + 1) >> 1;
    *out_y = (uint16_t)((y0 << 8) | y1);
    *out_uv = (uint16_t)((u << 8) | v);
}


#define WAIFU_PCFX_VDC_FONT_TILE_BASE 0x120
#define WAIFU_PCFX_VDC_BLANK_TILE WAIFU_PCFX_VDC_FONT_TILE_BASE
#define WAIFU_PCFX_VDC_FONT_FIRST 0x20
#define WAIFU_PCFX_VDC_FONT_LAST  0x7f
#define WAIFU_PCFX_VDC_MAP_W 64
#define WAIFU_PCFX_VDC_MAP_H 32
#define WAIFU_PCFX_VDC_PAL_BLACK 0x01
#define WAIFU_PCFX_VDC_PAL_WHITE 0x02
#define WAIFU_PCFX_VDC_PAL_GOLD  0x03
#define WAIFU_PCFX_VDC_PAL_RED   0x04
/* Fade tiles live immediately after the 64x32 BAT so tile 0 remains unusable
   for transparent blanks.  Each level is a screen-space black dither mask. */
#define WAIFU_PCFX_VDC_FADE_TILE_BASE 0x080
#define WAIFU_PCFX_VDC_FADE_LEVELS 17

typedef enum WaifuPcfxOverlayMode {
    WAIFU_PCFX_OVERLAY_OFF = 0,
    WAIFU_PCFX_OVERLAY_TITLE_PROMPT = 1,
    WAIFU_PCFX_OVERLAY_MENU = 2
} WaifuPcfxOverlayMode;

static WaifuPcfxOverlayMode g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF;
static WaifuPcfxOverlayMode g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
static int g_vdc_overlay_prompt_visible = 0;
static int g_vdc_overlay_has_save = 0;
static int g_vdc_overlay_menu_selected = 1;
/* 0 = no black overlay, 16 = fully black.  This is deliberately platform-local:
   the core only exposes visible_q8, and the PC-FX presenter decides how to hide
   its 16M KING surface without touching KRAM. */
static int g_vdc_overlay_fade_level = 16;
static int g_vdc_overlay_applied_fade_level = -1;
static int g_vdc_overlay_dirty = 1;
static void pcfx_vdc_overlay_flush(WaifuPcfxVideo *video);

static int pcfx_strlen_limited(const char *s, int max_len)
{
    int n = 0;
    if (!s || max_len <= 0) return 0;
    while (n < max_len && s[n]) ++n;
    return n;
}

static uint8_t pcfx_font_row(unsigned char ch, int row)
{
    if (row < 0 || row >= 8) return 0;
    return n2DLib_font[((uint32_t)ch * 8u) + (uint32_t)row];
}

static uint8_t pcfx_outline_row(unsigned char ch, int row)
{
    uint8_t center = pcfx_font_row(ch, row);
    uint8_t above = pcfx_font_row(ch, row - 1);
    uint8_t below = pcfx_font_row(ch, row + 1);
    uint8_t neigh = (uint8_t)(above | below | (uint8_t)(center << 1) | (uint8_t)(center >> 1) |
                              (uint8_t)(above << 1) | (uint8_t)(above >> 1) |
                              (uint8_t)(below << 1) | (uint8_t)(below >> 1));
    return (uint8_t)(neigh & (uint8_t)~center);
}


static uint8_t pcfx_vdc_fade_pattern_row(int level, int row)
{
    /* 8x8 ordered dither.  Thresholds are 0..63; each level covers four more
       pixels per tile.  The spatial pattern is stable, so fade changes never
       touch the 16M title surface and cannot expose half-written YUV data. */
    static const uint8_t bayer8[8][8] = {
        { 0, 48, 12, 60,  3, 51, 15, 63 },
        {32, 16, 44, 28, 35, 19, 47, 31 },
        { 8, 56,  4, 52, 11, 59,  7, 55 },
        {40, 24, 36, 20, 43, 27, 39, 23 },
        { 2, 50, 14, 62,  1, 49, 13, 61 },
        {34, 18, 46, 30, 33, 17, 45, 29 },
        {10, 58,  6, 54,  9, 57,  5, 53 },
        {42, 26, 38, 22, 41, 25, 37, 21 }
    };
    uint8_t bits = 0;
    int cutoff;
    if (level <= 0) return 0x00;
    if (level >= 16) return 0xff;
    cutoff = level << 2;
    for (int x = 0; x < 8; ++x) {
        if (bayer8[row & 7][x] < cutoff) bits |= (uint8_t)(0x80u >> x);
    }
    return bits;
}

static void pcfx_vdc_overlay_upload_fade_tiles(void)
{
    eris_low_sup_set_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_FADE_TILE_BASE * 16);
    for (int level = 0; level < WAIFU_PCFX_VDC_FADE_LEVELS; ++level) {
        for (int row = 0; row < 16; ++row) eris_low_sup_vram_write(VDC_CHIP_0, 0x0000);
    }

    eris_low_sup_set_vram_write(VDC_CHIP_1, WAIFU_PCFX_VDC_FADE_TILE_BASE * 16);
    for (int level = 0; level < WAIFU_PCFX_VDC_FADE_LEVELS; ++level) {
        for (int row = 0; row < 8; ++row) {
            eris_low_sup_vram_write(VDC_CHIP_1, pcfx_vdc_fade_pattern_row(level, row));
        }
        for (int row = 0; row < 8; ++row) eris_low_sup_vram_write(VDC_CHIP_1, 0x0000);
    }
}

static void pcfx_vdc_overlay_fill_fade_level(int level)
{
    uint16_t tile;
    if (level < 0) level = 0;
    if (level > 16) level = 16;
    tile = (uint16_t)(WAIFU_PCFX_VDC_FADE_TILE_BASE + level);
    for (int row = 0; row < WAIFU_PCFX_VDC_MAP_H; ++row) {
        int addr = row * WAIFU_PCFX_VDC_MAP_W;
        uint16_t row_tile = tile;
        /* The PC-FX mixer can leak a stale bottom scanline during the title/menu
           16M -> 8bpp mode transition while a sparse dither tile is in front.
           Keep the lowest visible tile row opaque black, but only once the fade
           is already near-black: forcing it at every level made the bottom edge
           visibly pop to solid black mid-fade.  Gating it to the top fade levels
           keeps the fade uniform while still covering the mode-switch frame
           (which always finishes the fade at level 16). */
        if (level >= 15 && row >= 29) row_tile = (uint16_t)(WAIFU_PCFX_VDC_FADE_TILE_BASE + 16);
        eris_low_sup_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) eris_low_sup_vram_write(VDC_CHIP_0, row_tile);
        eris_low_sup_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) eris_low_sup_vram_write(VDC_CHIP_1, (uint16_t)(row_tile | 0x8000));
    }
}

static void pcfx_vdc_overlay_set_fade_q8(int visible_q8)
{
    int cover;
    if (visible_q8 < 0) visible_q8 = 0;
    if (visible_q8 > 256) visible_q8 = 256;
    cover = ((256 - visible_q8) * 16 + 128) >> 8;
    if (cover < 0) cover = 0;
    if (cover > 16) cover = 16;
    if (g_vdc_overlay_fade_level != cover) {
        g_vdc_overlay_fade_level = cover;
        g_vdc_overlay_dirty = 1;
    }
}

static void pcfx_vdc_overlay_force_black(WaifuPcfxVideo *video)
{
    if (!video || !video->vdc_overlay_ready) return;
    g_vdc_overlay_fade_level = 16;
    g_vdc_overlay_dirty = 1;
    pcfx_vdc_overlay_flush(video);
}

static void pcfx_vdc_overlay_upload_font(void)
{
    /* VDC0 supplies the high nibble; keep all overlay glyph pixels zero there.
       VDC1 supplies the low nibble: 0 transparent, 1 black outline, 2 white
       foreground.  In pcfxemu's dual-VDC BG-combo path, a low nibble of zero
       leaves the KING/Rainbow layer visible, so the 16M title image never has
       to be rewritten just to blink text. */
    eris_low_sup_set_vram_write(VDC_CHIP_0, 0);
    for (int j = 0; j < 16; ++j) eris_low_sup_vram_write(VDC_CHIP_0, 0x0000);
    eris_low_sup_set_vram_write(VDC_CHIP_1, 0);
    for (int j = 0; j < 16; ++j) eris_low_sup_vram_write(VDC_CHIP_1, 0x0000);

    eris_low_sup_set_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_FONT_TILE_BASE * 16);
    for (int i = WAIFU_PCFX_VDC_FONT_FIRST; i <= WAIFU_PCFX_VDC_FONT_LAST; ++i) {
        for (int j = 0; j < 16; ++j) eris_low_sup_vram_write(VDC_CHIP_0, 0x0000);
    }

    eris_low_sup_set_vram_write(VDC_CHIP_1, WAIFU_PCFX_VDC_FONT_TILE_BASE * 16);
    for (int ch = WAIFU_PCFX_VDC_FONT_FIRST; ch <= WAIFU_PCFX_VDC_FONT_LAST; ++ch) {
        for (int row = 0; row < 8; ++row) {
            uint8_t fg = pcfx_font_row((unsigned char)ch, row);
            uint8_t outline = pcfx_outline_row((unsigned char)ch, row);
            eris_low_sup_vram_write(VDC_CHIP_1, (uint16_t)(((uint16_t)fg << 8) | outline));
        }
        for (int row = 0; row < 8; ++row) eris_low_sup_vram_write(VDC_CHIP_1, 0x0000);
    }
}

static void pcfx_vdc_overlay_clear_rect(int tx, int ty, int w, int h)
{
    if (tx < 0) { w += tx; tx = 0; }
    if (ty < 0) { h += ty; ty = 0; }
    if (tx + w > WAIFU_PCFX_VDC_MAP_W) w = WAIFU_PCFX_VDC_MAP_W - tx;
    if (ty + h > WAIFU_PCFX_VDC_MAP_H) h = WAIFU_PCFX_VDC_MAP_H - ty;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; ++row) {
        int addr = (ty + row) * WAIFU_PCFX_VDC_MAP_W + tx;
        eris_low_sup_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < w; ++col) eris_low_sup_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_BLANK_TILE);
        eris_low_sup_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < w; ++col) eris_low_sup_vram_write(VDC_CHIP_1, (uint16_t)(WAIFU_PCFX_VDC_BLANK_TILE | 0x8000));
    }
}

static void pcfx_vdc_overlay_clear_all(void)
{
    pcfx_vdc_overlay_clear_rect(0, 0, WAIFU_PCFX_VDC_MAP_W, WAIFU_PCFX_VDC_MAP_H);
}

static void pcfx_vdc_overlay_print(int tx, int ty, const char *str, int max_len)
{
    int len;
    if (!str || max_len <= 0) return;
    if (tx < 0 || ty < 0 || tx >= WAIFU_PCFX_VDC_MAP_W || ty >= WAIFU_PCFX_VDC_MAP_H) return;
    if (max_len > WAIFU_PCFX_VDC_MAP_W - tx) max_len = WAIFU_PCFX_VDC_MAP_W - tx;
    len = pcfx_strlen_limited(str, max_len);

    eris_low_sup_set_vram_write(VDC_CHIP_0, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) {
        unsigned char ch = (i < len) ? (unsigned char)str[i] : (unsigned char)' ';
        uint16_t tile = WAIFU_PCFX_VDC_BLANK_TILE;
        if (ch >= WAIFU_PCFX_VDC_FONT_FIRST && ch <= WAIFU_PCFX_VDC_FONT_LAST) {
            tile = (uint16_t)(WAIFU_PCFX_VDC_FONT_TILE_BASE + (ch - WAIFU_PCFX_VDC_FONT_FIRST));
        }
        eris_low_sup_vram_write(VDC_CHIP_0, tile);
    }

    eris_low_sup_set_vram_write(VDC_CHIP_1, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) {
        unsigned char ch = (i < len) ? (unsigned char)str[i] : (unsigned char)' ';
        uint16_t tile = WAIFU_PCFX_VDC_BLANK_TILE;
        if (ch >= WAIFU_PCFX_VDC_FONT_FIRST && ch <= WAIFU_PCFX_VDC_FONT_LAST) {
            tile = (uint16_t)(WAIFU_PCFX_VDC_FONT_TILE_BASE + (ch - WAIFU_PCFX_VDC_FONT_FIRST));
        }
        eris_low_sup_vram_write(VDC_CHIP_1, (uint16_t)(tile | 0x8000));
    }
}

static void pcfx_vdc_overlay_init(WaifuPcfxVideo *video)
{
    if (!video) return;

    eris_low_sup_set_control(VDC_CHIP_0, 0, 1, 0);
    eris_low_sup_set_control(VDC_CHIP_1, 0, 1, 0);
    eris_low_sup_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_0, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_1, 0, 0);
    eris_low_sup_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    eris_low_sup_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);

    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_BLACK, 0x0088);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_WHITE, 0xE088);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_GOLD,  0xB468);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_RED,   0x5F0F);

    pcfx_vdc_overlay_upload_fade_tiles();
    pcfx_vdc_overlay_upload_font();
    /* Start covered.  The first title frame may require a full 16M KRAM upload;
       keeping an opaque VDC black mask in front prevents any one-frame reveal
       of uninitialized or partially uploaded KING data. */
    g_vdc_overlay_fade_level = 16;
    g_vdc_overlay_applied_fade_level = -1;
    pcfx_vdc_overlay_fill_fade_level(16);
    eris_low_sup_setreg(VDC_CHIP_0, 5, 0x88);
    eris_low_sup_setreg(VDC_CHIP_1, 5, 0x80);

    video->vdc_overlay_ready = 1;
    g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
    g_vdc_overlay_dirty = 1;
}

static void pcfx_vdc_overlay_shutdown(WaifuPcfxVideo *video)
{
    if (!video || !video->vdc_overlay_ready) return;
    pcfx_vdc_overlay_clear_all();
    video->vdc_overlay_ready = 0;
    g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
    g_vdc_overlay_applied_fade_level = -1;
}

static void pcfx_vdc_overlay_flush(WaifuPcfxVideo *video)
{
    if (!video || !video->vdc_overlay_ready || !g_vdc_overlay_dirty) return;

    if (g_vdc_overlay_fade_level > 0) {
        /* Fade masks are authoritative for the whole VDC overlay while active.
           Suppress text during the fade so prompt/menu glyph tiles cannot punch
           transparent holes through the black dither mask. */
        if (g_vdc_overlay_applied_fade_level != g_vdc_overlay_fade_level ||
            g_vdc_overlay_applied_mode != g_vdc_overlay_mode) {
            pcfx_vdc_overlay_fill_fade_level(g_vdc_overlay_fade_level);
            g_vdc_overlay_applied_fade_level = g_vdc_overlay_fade_level;
            g_vdc_overlay_applied_mode = g_vdc_overlay_mode;
        }
        g_vdc_overlay_dirty = 0;
        return;
    }

    if (g_vdc_overlay_applied_fade_level != 0 ||
        g_vdc_overlay_applied_mode != g_vdc_overlay_mode) {
        pcfx_vdc_overlay_clear_all();
        g_vdc_overlay_applied_fade_level = 0;
        g_vdc_overlay_applied_mode = g_vdc_overlay_mode;
    }

    switch (g_vdc_overlay_mode) {
    default:
    case WAIFU_PCFX_OVERLAY_OFF:
        pcfx_vdc_overlay_clear_all();
        break;

    case WAIFU_PCFX_OVERLAY_TITLE_PROMPT:
        pcfx_vdc_overlay_clear_rect(6, 23, 24, 3);
        if (g_vdc_overlay_prompt_visible) {
            pcfx_vdc_overlay_print(7, 23,
                g_vdc_overlay_has_save ? "RUN START   B LOAD" : "PUSH RUN TO START", 22);
        }
        break;

    case WAIFU_PCFX_OVERLAY_MENU:
        pcfx_vdc_overlay_clear_rect(4, 15, 28, 13);
        pcfx_vdc_overlay_print(9, 16, "SELECT MODE", 16);
        pcfx_vdc_overlay_print(7, 18, g_vdc_overlay_menu_selected == 0 ? "> STORY MODE" : "  STORY MODE", 16);
        pcfx_vdc_overlay_print(7, 20, g_vdc_overlay_menu_selected == 1 ? "> BATTLE MODE" : "  BATTLE MODE", 16);
        pcfx_vdc_overlay_print(7, 22, g_vdc_overlay_menu_selected == 2 ? "> LOAD STORY" : "  LOAD STORY", 16);
        if (g_vdc_overlay_menu_selected == 0) {
            pcfx_vdc_overlay_print(6, 25, "ENTER NAME / FIRST DREAM", 26);
        } else if (g_vdc_overlay_menu_selected == 2) {
            pcfx_vdc_overlay_print(6, 25, g_vdc_overlay_has_save ? "RESUME SAVED STORY" : "NO SAVE FILE FOUND", 26);
        } else {
            pcfx_vdc_overlay_print(6, 25, "RANDOM DECK / FREE DUEL", 26);
        }
        break;
    }

    g_vdc_overlay_dirty = 0;
}

void waifu_pcfx_video_overlay_title_prompt(int visible, int has_save)
{
    visible = visible ? 1 : 0;
    has_save = has_save ? 1 : 0;
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_TITLE_PROMPT ||
        g_vdc_overlay_prompt_visible != visible ||
        g_vdc_overlay_has_save != has_save) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_TITLE_PROMPT;
        g_vdc_overlay_prompt_visible = visible;
        g_vdc_overlay_has_save = has_save;
        g_vdc_overlay_dirty = 1;
    }
}

void waifu_pcfx_video_overlay_menu(int selected, int has_save)
{
    if (selected < 0) selected = 0;
    if (selected > 2) selected = 2;
    has_save = has_save ? 1 : 0;
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_MENU ||
        g_vdc_overlay_menu_selected != selected ||
        g_vdc_overlay_has_save != has_save) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_MENU;
        g_vdc_overlay_menu_selected = selected;
        g_vdc_overlay_has_save = has_save;
        g_vdc_overlay_dirty = 1;
    }
}

void waifu_pcfx_video_overlay_clear(void)
{
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_OFF) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF;
        g_vdc_overlay_dirty = 1;
    }
}

static void set_king_16m_title_video(void)
{
    /* VDC BG must be in front of KING BG0 for prompt/menu text. */
    eris_tetsu_set_priorities(7, 0, 6, 0, 0, 0, 0);
    eris_tetsu_set_7up_palette(0, 0);
    eris_tetsu_set_king_palette(0, 0, 0, 0);
    eris_tetsu_set_rainbow_palette(0);

    eris_tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);

    /* Bring the opaque VDC black mask up BEFORE switching KING BG0 into 16M
       mode.  The 16M title page shares KRAM word offset 0 with the 8bpp page,
       so flipping to 16M while the previous 8bpp/loading frame is still resident
       (the title is not uploaded until later in this same present) shows one
       frame of that data reinterpreted as YUV garbage: a pink wash with a
       diagonal mode-switch seam.  Masking first hides the switch entirely. */
    eris_low_sup_set_control(VDC_CHIP_0, 0, 1, 0);
    eris_low_sup_set_control(VDC_CHIP_1, 0, 1, 0);
    pcfx_vdc_overlay_init(&g_video);

    eris_king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 0);
    eris_king_set_bg_mode(KING_BGMODE_16M, 0, 0, 0);
    pcfx_king_set_bg_kram_page_inline(0);

    memset(g_king_microprog, 0, sizeof(g_king_microprog));
    g_king_microprog[0] = KING_CODE_BG0_CG_0;
    g_king_microprog[1] = KING_CODE_BG0_CG_1;
    g_king_microprog[2] = KING_CODE_BG0_CG_2;
    g_king_microprog[3] = KING_CODE_BG0_CG_3;
    g_king_microprog[4] = KING_CODE_BG0_CG_4;
    g_king_microprog[5] = KING_CODE_BG0_CG_5;
    g_king_microprog[6] = KING_CODE_BG0_CG_6;
    g_king_microprog[7] = KING_CODE_BG0_CG_7;
    eris_king_disable_microprogram();
    eris_king_write_microprogram(g_king_microprog, 0, 16);
    eris_king_enable_microprogram();
    pcfx_king_set_bg0_page_inline(0);
    eris_king_set_scroll(KING_BG0, 0, 0);
    eris_king_set_scroll(KING_BG0SUB, 0, 0);
    eris_king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
    eris_king_set_bg_size(KING_BG0SUB, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
}

static void pcfx_title_clip_dirty_rect(const WaifuFmDirtyRect *rect, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int x0 = (int)rect->x;
    int y0 = (int)rect->y;
    int x1 = x0 + (int)rect->w;
    int y1 = y0 + (int)rect->h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WAIFU_PCFX_W) x1 = WAIFU_PCFX_W;
    if (y1 > WAIFU_PCFX_H) y1 = WAIFU_PCFX_H;

    /* The 16M title buffer stores one Y word and one shared UV word per
       two-pixel pair.  KRAM streaming code writes 16-byte groups, so expand
       to an 8-pixel boundary.  The prompt and menu hints are already coarse;
       this just makes arbitrary callers safe. */
    x0 &= ~7;
    x1 = (x1 + 7) & ~7;
    if (x1 > WAIFU_PCFX_W) x1 = WAIFU_PCFX_W;

    if (x0 >= x1 || y0 >= y1) {
        *out_x = 0;
        *out_y = 0;
        *out_w = 0;
        *out_h = 0;
        return;
    }

    *out_x = x0;
    *out_y = y0;
    *out_w = x1 - x0;
    *out_h = y1 - y0;
}

static WAIFU_PCFX_COLD void pcfx_title_composite_rect(const uint16_t *title_yuv422,
                                                      const uint8_t *title8,
                                                      const uint8_t *framebuffer,
                                                      int x0, int y0, int w, int h)
{
    int y_end = y0 + h;
    int x_end = x0 + w;
    if (x0 & 1) x0--;
    if (x_end & 1) x_end++;
    if (x0 < 0) x0 = 0;
    if (x_end > WAIFU_PCFX_W) x_end = WAIFU_PCFX_W;
    if (y0 < 0) y0 = 0;
    if (y_end > WAIFU_PCFX_H) y_end = WAIFU_PCFX_H;
    if (x0 >= x_end || y0 >= y_end) return;

    for (int y = y0; y < y_end; ++y) {
        int row = y * WAIFU_PCFX_W;
        for (int x = x0; x < x_end; x += 2) {
            int pi = row + x;
            uint16_t base_y = title_yuv422[pi];
            uint16_t base_uv = title_yuv422[pi + 1];
            uint8_t idx0 = framebuffer[pi];
            uint8_t idx1 = framebuffer[pi + 1];
            int base0 = title8 && idx0 == title8[pi];
            int base1 = title8 && idx1 == title8[pi + 1];
            if (base0 && base1) {
                g_title16m_composite[pi] = base_y;
                g_title16m_composite[pi + 1] = base_uv;
            } else {
                uint8_t r0, g0, b0, r1, g1, b1;
                if (base0) {
                    pcfx_yuv16m_pixel_to_rgb(base_y, base_uv, 0, &r0, &g0, &b0);
                } else {
                    const uint8_t *p0 = &title_screen_palette_rgb[(int)idx0 * 3];
                    r0 = p0[0]; g0 = p0[1]; b0 = p0[2];
                }
                if (base1) {
                    pcfx_yuv16m_pixel_to_rgb(base_y, base_uv, 1, &r1, &g1, &b1);
                } else {
                    const uint8_t *p1 = &title_screen_palette_rgb[(int)idx1 * 3];
                    r1 = p1[0]; g1 = p1[1]; b1 = p1[2];
                }
                pcfx_rgb_pair_to_yuv16m_words(r0, g0, b0, r1, g1, b1,
                                               &g_title16m_composite[pi],
                                               &g_title16m_composite[pi + 1]);
            }
            if ((g_title16m_composite[pi] & 0xff00u) == 0) g_title16m_composite[pi] |= 0x0100u;
            if ((g_title16m_composite[pi] & 0x00ffu) == 0) g_title16m_composite[pi] |= 0x0001u;
        }
    }
}

static WAIFU_PCFX_COLD void pcfx_title_upload_rect_16m(int page, int x0, int y0, int w, int h)
{
    uint32_t page_base = title16m_page_word_offset(page);
    if (w <= 0 || h <= 0) return;
    for (int y = y0; y < y0 + h; ++y) {
        int pi = y * WAIFU_PCFX_W + x0;
        king_seek_write_words(page_base + (uint32_t)pi);
        king_kram_write_buffer((void *)&g_title16m_composite[pi], w * 2);
    }
}

static WAIFU_PCFX_COLD void pcfx_title_upload_full_16m(int page)
{
    king_seek_write_words(title16m_page_word_offset(page));
    king_kram_write_buffer((void *)g_title16m_composite, WAIFU_PCFX_TITLE_16M_WORDS * 2);
}

static WAIFU_PCFX_COLD void pcfx_title_blackout_pages(WaifuPcfxVideo *video)
{
    /* Terminal title/menu blackout.  A VDC full-black mask covers the frame,
       but a few mixer/video-mode switch paths can still leak bottom scanlines
       from the 16M KING surface.  Replace both active 16M CG surfaces with
       neutral-black YUV before switching to the 8bpp scene path. */
    int first = video ? video->front_page : 0;
    int second = first ^ 1;
    for (int i = 0; i < WAIFU_PCFX_TITLE_16M_WORDS; i += 2) {
        g_title16m_composite[i + 0] = 0x0101u;
        g_title16m_composite[i + 1] = 0x8080u;
    }
    pcfx_title_upload_full_16m(first);
    pcfx_title_upload_full_16m(second);
    if (video) {
        video->title16m_page_valid[0] = 0;
        video->title16m_page_valid[1] = 0;
        video->pending_title_page_flip = 0;
    }
}

static WAIFU_PCFX_COLD void pcfx_present_title_16m(WaifuPcfxVideo *video, const uint16_t *title_yuv422, const uint8_t *framebuffer)
{
    const uint8_t *title8;
    uint32_t dirty_serial;
    WaifuFmDirtyRect rects[WAIFU_FM_MAX_DIRTY_RECTS];
    int rect_count;
    int reconfigure;
    int upload_page;
    int need_full_upload;

    if (!video || !title_yuv422 || !framebuffer) return;

    reconfigure = (!video->initialized || video->mode != WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR);
    dirty_serial = waifu_fm_frame_dirty_serial();

    if (reconfigure) {
        set_king_16m_title_video();
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_set_bg0_page_inline(0);
        video->mode = WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR;
        video->initialized = 1;
        video->active_palette = WAIFU_FM_PALETTE_TITLE;
        video->active_fade_q8 = -1;
        video->front_page = 0;
        video->back_page = 0;
        video->pending_title_page_flip = 0;
        video->title16m_page_valid[0] = 0;
        video->title16m_page_valid[1] = 0;
    }

    pcfx_vdc_overlay_set_fade_q8(waifu_fm_video_fade_q8());
    if (!reconfigure && video->last_title_dirty_serial == dirty_serial) {
        return;
    }

    title8 = waifu_assets_title_screen_img();
    upload_page = video->back_page;
    need_full_upload = reconfigure || waifu_fm_frame_dirty_full() || !video->title16m_page_valid[upload_page];

    if (need_full_upload) {
        pcfx_title_composite_rect(title_yuv422, title8, framebuffer, 0, 0, WAIFU_PCFX_W, WAIFU_PCFX_H);
        if (reconfigure) {
            /* Prime both 16M title pages before the first blink/menu event.
               Otherwise the first hidden-page use would require a full 120 KiB
               upload while the title is already visible. */
            pcfx_title_upload_full_16m(0);
            pcfx_title_upload_full_16m(1);
            video->title16m_page_valid[0] = 1;
            video->title16m_page_valid[1] = 1;
            video->front_page = 0;
            video->back_page = 1;
            video->last_title_dirty_serial = dirty_serial;
            video->have_last_frame = 1;
            return;
        }
        pcfx_title_upload_full_16m(upload_page);
    } else {
        rect_count = waifu_fm_frame_dirty_rects(rects, WAIFU_FM_MAX_DIRTY_RECTS);
        if (rect_count <= 0) {
            video->last_title_dirty_serial = dirty_serial;
            return;
        }
        for (int i = 0; i < rect_count; ++i) {
            int x0, y0, w, h;
            pcfx_title_clip_dirty_rect(&rects[i], &x0, &y0, &w, &h);
            if (w <= 0 || h <= 0) continue;
            pcfx_title_composite_rect(title_yuv422, title8, framebuffer, x0, y0, w, h);
            pcfx_title_upload_rect_16m(upload_page, x0, y0, w, h);
        }
    }

    video->title16m_page_valid[upload_page] = 1;
    if (reconfigure && upload_page == 0) {
        video->front_page = 0;
        video->back_page = 1;
    } else {
        pcfx_schedule_title_page_flip(video, upload_page, upload_page, video->front_page);
    }
    video->last_title_dirty_serial = dirty_serial;
    video->have_last_frame = 1;
}

static void set_king_8bpp_video(void)
{
    /* Keep the front VDC overlay intact until the new 8bpp KING page has been
       cleared.  Clearing it here exposes the previous 16M title surface for a
       frame during title/menu -> loading/battle mode switches. */
    eris_tetsu_set_priorities(1, 0, 7, 0, 0, 0, 0);
    eris_tetsu_set_7up_palette(0, 0);
    /* KING palette offsets are stored as 8-bit values in two-color units in
       the VCE.  Use palette bank 0 for the 8bpp page path and upload the same
       RGB-derived entries there.  Passing 256 wrapped through the low byte on
       some paths, leaving BIOS/default colors visible. */
    eris_tetsu_set_king_palette(0, 0, 0, 0);
    eris_tetsu_set_rainbow_palette(0);

    eris_king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 0);
    eris_king_set_bg_mode(KING_BGMODE_256_PAL, 0, 0, 0); /* 8bpp KING BG0. */
    pcfx_king_set_bg_kram_page_inline(0);

    memset(g_king_microprog, 0, sizeof(g_king_microprog));
    g_king_microprog[0] = KING_CODE_BG0_CG_0;
    g_king_microprog[1] = KING_CODE_BG0_CG_1;
    g_king_microprog[2] = KING_CODE_BG0_CG_2;
    g_king_microprog[3] = KING_CODE_BG0_CG_3;
    eris_king_disable_microprogram();
    eris_king_write_microprogram(g_king_microprog, 0, 16);
    eris_king_enable_microprogram();
    pcfx_king_set_bg0_page_inline(0);
    eris_king_set_scroll(KING_BG0, 0, 0);
    eris_king_set_scroll(KING_BG0SUB, 0, 0);
    eris_king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
    eris_king_set_bg_size(KING_BG0SUB, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);

    eris_tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);

    /* Keep VDC layers initialized and ready for converted backgrounds, but the
       first port leaves them transparent/empty unless a PC-FX VDC background
       is explicitly selected. */
    eris_low_sup_set_control(VDC_CHIP_0, 0, 1, 0);
    eris_low_sup_set_control(VDC_CHIP_1, 0, 1, 0);
    eris_low_sup_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_0, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_1, 0, 0);
    eris_low_sup_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    eris_low_sup_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
}

WaifuPcfxVideo *waifu_pcfx_video_create(void)
{
    eris_king_init();
    eris_tetsu_init();
    memset(&g_video, 0, sizeof(g_video));
    g_video.front_page = 0;
    g_video.back_page = 1;
    g_video.active_palette = (WaifuFmPaletteId)-1;
    g_video.active_fade_q8 = -1;
    g_video.have_base_yuv = 0;
    g_video.pending_title_page_flip = 0;
    g_video.title16m_page_valid[0] = 0;
    g_video.title16m_page_valid[1] = 0;
    g_video.vdc_overlay_ready = 0;
    g_video.vdc_overlay_shutdown_countdown = 0;
    waifu_pcfx_video_begin_8bpp(&g_video);
    return &g_video;
}

void waifu_pcfx_video_destroy(WaifuPcfxVideo *video)
{
    (void)video;
}

void waifu_pcfx_video_begin_8bpp(WaifuPcfxVideo *video)
{
    if (!video) return;
    pcfx_vdc_overlay_force_black(video);
    if (video->mode == WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR) pcfx_title_blackout_pages(video);
    set_king_8bpp_video();
    /* Re-assert the black VDC mask after the VDC mode registers are touched. */
    pcfx_vdc_overlay_force_black(video);
    video->mode = WAIFU_PCFX_VIDEO_MODE_KING_8BPP;
    video->initialized = 1;
    video->have_last_frame = 0;
    video->last_title_dirty_serial = 0;
    video->pending_title_page_flip = 0;
    video->title16m_page_valid[0] = 0;
    video->title16m_page_valid[1] = 0;
    video->have_base_yuv = 0;
    video->active_palette = (WaifuFmPaletteId)-1;
    video->active_fade_q8 = -1;
    waifu_pcfx_video_clear_black(video);
    if (video->vdc_overlay_ready) {
        /* Defer clearing the front VDC black mask until at least one 8bpp black
           KING page has had a vblank to become authoritative.  Clearing it in
           the same CPU phase as the mode switch was the remaining one-frame
           bottom-edge title leak. */
        video->vdc_overlay_shutdown_countdown = 2;
    }
}

void waifu_pcfx_video_use_vdc_background(WaifuPcfxVideo *video, WaifuPcfxVdcBackground bg)
{
    if (!video) return;
    video->vdc_bg = bg;
    /* Hook point for converted HuC6270 VDC backgrounds.  The source package
       includes Cascade FX's generator in tools/cascade_fx.  The first port keeps
       this opaque to avoid coupling the game core to a specific VDC asset set. */
}

static int fade_q8_to_level(int fade_q8)
{
    return clamp_int((fade_q8 * (WAIFU_PCFX_FADE_LEVELS - 1) + 128) >> 8, 0, WAIFU_PCFX_FADE_LEVELS - 1);
}

static int pcfx_palette_table_index(WaifuFmPaletteId palette_id)
{
    return (palette_id == WAIFU_FM_PALETTE_TITLE) ? 1 : 0;
}

static void waifu_pcfx_video_rebuild_base_palette(WaifuPcfxVideo *video, const uint8_t *rgb)
{
    /* Fallback only for future dynamic palettes.  The normal common/title
       palettes use generated PC-FX-native fade tables so no expensive
       conversion runs on the V810 during transitions. */
    for (int i = 0; i < 256; ++i) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        video->base_yuv[i] = rgb888_to_pcfx_yuv(r, g, b);
    }
    video->base_yuv[IDX_BLACK] = WAIFU_PCFX_NEUTRAL_BLACK;
    video->have_base_yuv = 1;
}

void waifu_pcfx_video_set_palette_rgb_fade(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    if (!video || !rgb) return;
    fade_q8 = clamp_int(fade_q8, 0, 256);

    int level = fade_q8_to_level(fade_q8);
    if (video->active_palette == palette_id && video->active_fade_q8 == level) return;

    if (palette_id == WAIFU_FM_PALETTE_COMMON || palette_id == WAIFU_FM_PALETTE_TITLE) {
        const uint16_t *row = waifu_pcfx_palette_fade_lut[pcfx_palette_table_index(palette_id)][level];
        for (int i = 0; i < 256; ++i) {
            uint16_t yuv = row[i];
            eris_tetsu_set_palette((uint16_t)i, yuv);
            eris_tetsu_set_palette((uint16_t)(256 + i), yuv);
        }
    } else {
        if (!video->have_base_yuv || video->active_palette != palette_id) {
            waifu_pcfx_video_rebuild_base_palette(video, rgb);
        }
        for (int i = 0; i < 256; ++i) {
            uint16_t yuv;
            if (i == IDX_BLACK || fade_q8 <= 0) yuv = WAIFU_PCFX_NEUTRAL_BLACK;
            else if (fade_q8 >= 256) yuv = video->base_yuv[i];
            else {
                int r = (rgb[i * 3 + 0] * fade_q8 + 128) >> 8;
                int g = (rgb[i * 3 + 1] * fade_q8 + 128) >> 8;
                int b = (rgb[i * 3 + 2] * fade_q8 + 128) >> 8;
                yuv = rgb888_to_pcfx_yuv((uint8_t)r, (uint8_t)g, (uint8_t)b);
            }
            eris_tetsu_set_palette((uint16_t)i, yuv);
            eris_tetsu_set_palette((uint16_t)(256 + i), yuv);
        }
        video->have_base_yuv = 1;
    }

    eris_tetsu_set_palette((uint16_t)IDX_BLACK, WAIFU_PCFX_NEUTRAL_BLACK);
    eris_tetsu_set_palette((uint16_t)(256 + IDX_BLACK), WAIFU_PCFX_NEUTRAL_BLACK);
    video->active_palette = palette_id;
    video->active_fade_q8 = level;
}

void waifu_pcfx_video_set_palette_rgb(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id)
{
    waifu_pcfx_video_set_palette_rgb_fade(video, rgb, palette_id, 256);
}

void waifu_pcfx_video_clear_black(WaifuPcfxVideo *video)
{
    if (!video) return;
    king_seek_write_words(0);
    king_kram_fill_words(WAIFU_PCFX_BLACK_WORD, WAIFU_PCFX_PAGE_STRIDE_WORDS * 2);
    video->front_page = 0;
    video->back_page = 1;
    video->have_last_frame = 0;
    video->pending_title_page_flip = 0;
    video->title16m_page_valid[0] = 0;
    video->title16m_page_valid[1] = 0;
#if WAIFU_PCFX_DIRTY_PRESENT
    memset(video->page_shadow[0], IDX_BLACK, WAIFU_PCFX_FRAME_BYTES);
    memset(video->page_shadow[1], IDX_BLACK, WAIFU_PCFX_FRAME_BYTES);
    video->page_shadow_valid[0] = 1;
    video->page_shadow_valid[1] = 1;
#endif
    pcfx_king_set_bg0_page_inline(page_bat_offset(video->front_page));
}

#if WAIFU_PCFX_DIRTY_PRESENT
static WAIFU_PCFX_COLD void pcfx_present_full_upload(WaifuPcfxVideo *video, const uint8_t *framebuffer, uint8_t *shadow)
{
    king_kram_write_buffer_bytes_at((void *)framebuffer, WAIFU_PCFX_FRAME_BYTES, page_word_offset(video->back_page));
    pcfx_copy_bytes_inline(shadow, framebuffer, WAIFU_PCFX_FRAME_BYTES);
    video->page_shadow_valid[video->back_page] = 1;
}
#endif

static WAIFU_PCFX_COLD void pcfx_present_update_palette_if_needed(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    waifu_pcfx_video_set_palette_rgb_fade(video, rgb, palette_id, fade_q8);
}

void waifu_pcfx_video_present_8bpp(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb, WaifuFmPaletteId palette_id)
{
    if (!video || !framebuffer) return;
    if (palette_id == WAIFU_FM_PALETTE_TITLE) {
        const uint16_t *title16m = waifu_assets_title_screen_pcfx_yuv422();
        if (title16m) {
            pcfx_present_title_16m(video, title16m, framebuffer);
            return;
        }
    }
    if (!video->initialized || video->mode != WAIFU_PCFX_VIDEO_MODE_KING_8BPP) {
        waifu_pcfx_video_begin_8bpp(video);
    }
    int fade_q8 = waifu_fm_video_fade_q8();
    int fade_level = fade_q8_to_level(fade_q8);
    if (video->active_palette != palette_id || video->active_fade_q8 != fade_level) {
        pcfx_present_update_palette_if_needed(video, rgb, palette_id, fade_q8);
    }

#if WAIFU_PCFX_DIRTY_PRESENT
    int upload_full = 0;
    uint8_t *shadow = video->page_shadow[video->back_page];
    g_pcfx_direct_big_art_draws = waifu_assets_big_art_draws();
    g_pcfx_direct_big_art_draw_count = waifu_assets_big_art_draw_count();

    if (!video->page_shadow_valid[video->back_page]) {
        upload_full = 1;
    } else {
        PcfxDirtyPlanStats dirty_stats = pcfx_dirty_plan_stats(framebuffer, shadow);
        if (dirty_stats.dirty_blocks == 0) {
            /* The hidden page already contains this frame, except that direct
               big-art overlays are deliberately skipped by the dirty scanner. */
            if (g_pcfx_direct_big_art_draw_count > 0) {
                pcfx_upload_direct_big_art_rects(shadow, framebuffer, page_word_offset(video->back_page));
            }
            pcfx_king_set_bg0_page_inline(page_bat_offset(video->back_page));
            video->front_page = video->back_page;
            video->back_page ^= 1;
            video->have_last_frame = 1;
            return;
        }
        if (dirty_stats.dirty_blocks * WAIFU_PCFX_DIRTY_BLOCK_W >= WAIFU_PCFX_DIRTY_FULL_THRESHOLD_BYTES ||
            dirty_stats.row_runs > WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS) {
            upload_full = 1;
        }
    }

    if (upload_full) {
        pcfx_present_full_upload(video, framebuffer, shadow);
    } else {
        pcfx_present_dirty_bands(shadow, framebuffer, page_word_offset(video->back_page));
    }
    if (g_pcfx_direct_big_art_draw_count > 0) {
        pcfx_upload_direct_big_art_rects(shadow, framebuffer, page_word_offset(video->back_page));
    }
#else
    uint32_t frame_sum;
    uint32_t frame_mix;
    pcfx_frame_signature(framebuffer, &frame_sum, &frame_mix);
    if (video->have_last_frame && video->last_frame_sum == frame_sum && video->last_frame_mix == frame_mix) {
        return;
    }

    /* Framebuffer is 256x240 packed 8bpp.  KING 8bpp CG data is byte-ordered
       like a big-endian pair inside each 16-bit KRAM word, while the V810 and
       CPU framebuffer are little-endian byte arrays.  Use the byte-swapping
       unrolled uploader; the plain word uploader produces swapped/mispaired
       palette indices and visibly wrong colors. */
    king_kram_write_buffer_bytes_at((void *)framebuffer, WAIFU_PCFX_FRAME_BYTES, page_word_offset(video->back_page));
#endif

    pcfx_king_set_bg0_page_inline(page_bat_offset(video->back_page));
    video->front_page = video->back_page;
    video->back_page ^= 1;
#if !WAIFU_PCFX_DIRTY_PRESENT
    video->last_frame_sum = frame_sum;
    video->last_frame_mix = frame_mix;
#endif
    video->have_last_frame = 1;
}

void waifu_pcfx_video_present_title_hicolor_stub(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb)
{
    (void)framebuffer;
    (void)rgb;
    pcfx_present_title_16m(video, waifu_assets_title_screen_pcfx_yuv422(), framebuffer);
}

void waifu_pcfx_video_wait_vblank(WaifuPcfxVideo *video)
{
    volatile uint16_t * const sr = (volatile uint16_t *)0x80000400u;
    while ((*sr & 0x0020u) == 0) { }
    if (video && video->pending_title_page_flip) {
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_set_bg0_page_inline(title16m_bg_cg_page(video->pending_title_kram_page));
        video->front_page = video->pending_title_front_page;
        video->back_page = video->pending_title_back_page;
        video->pending_title_page_flip = 0;
    }
    if (video) pcfx_vdc_overlay_flush(video);
    while ((*sr & 0x0020u) != 0) { }
    if (video && video->mode == WAIFU_PCFX_VIDEO_MODE_KING_8BPP && video->vdc_overlay_shutdown_countdown > 0) {
        --video->vdc_overlay_shutdown_countdown;
        if (video->vdc_overlay_shutdown_countdown == 0) pcfx_vdc_overlay_shutdown(video);
    }
}
