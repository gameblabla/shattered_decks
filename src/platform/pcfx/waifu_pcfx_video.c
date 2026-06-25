#include "waifu_pcfx_video.h"
#include "fastking.h"

#include <eris/king.h>
#include <eris/tetsu.h>
#include <eris/7up.h>
#include <eris/low/7up.h>
#include <eris/v810.h>
#include <stdint.h>
#include <string.h>
#include "waifu_assets.h"
#include "assets.h"
#include "pcfx_palette_assets.h"
#include "waifu_pcfx_cdrom.h"
#include "title_asset.h"
#include "font_menudata.h"

#define WAIFU_PCFX_W WAIFU_FM_WIDTH
#define WAIFU_PCFX_H WAIFU_FM_HEIGHT
#define WAIFU_PCFX_FRAME_BYTES (WAIFU_PCFX_W * WAIFU_PCFX_H)
#define WAIFU_PCFX_FRAME_WORDS (WAIFU_PCFX_FRAME_BYTES / 2)
#define WAIFU_PCFX_TITLE_16M_VISIBLE_WORDS (WAIFU_PCFX_W * WAIFU_PCFX_H)
#define WAIFU_PCFX_TITLE_16M_KRAM_ROWS 256
#define WAIFU_PCFX_TITLE_16M_KRAM_WORDS (WAIFU_PCFX_W * WAIFU_PCFX_TITLE_16M_KRAM_ROWS)
#define WAIFU_PCFX_TITLE_16M_SCROLL_Y 4
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
/* Present path: render once to the CPU framebuffer, then stream it to the hidden
   KRAM page with the inline out.h writer (no fastking jal/rts) and flip BG0 to
   it for tear-free double buffering.  The page_shadow[2] mirrors are NOT a second
   software back buffer for buffering's sake -- they let the presenter diff the
   new frame against what each KRAM page already holds and upload only the changed
   16px bands.  A full 256x240 KRAM upload is ~30k out.h (close to a whole frame's
   budget), so this diff is the main thing keeping partially-changed frames cheap;
   it stays on.  Set to 0 to force a full inline upload every changed frame. */
#define WAIFU_PCFX_DIRTY_PRESENT 1
#endif
#ifndef WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
/* Directly stream cached 112x112 big-card art to KING KRAM when its art
   window is 16-pixel aligned.  For the normal battle lanes/motion, which can
   place art at half-block offsets, stream the enclosing 16-pixel-aligned
   framebuffer envelope instead so the presenter always moves whole edge
   blocks and never falls back to small unaligned edge uploads. */
#define WAIFU_PCFX_DIRECT_BIG_ART_ENABLE 1
#endif

#define WAIFU_PCFX_DIRTY_FULL_THRESHOLD_BYTES (WAIFU_PCFX_FRAME_BYTES * 3 / 4)
#define WAIFU_PCFX_DIRTY_BLOCK_W 16
#define WAIFU_PCFX_DIRTY_BLOCKS_X (WAIFU_PCFX_W / WAIFU_PCFX_DIRTY_BLOCK_W)
/* A single row can yield up to BLOCKS_X runs when dirty blocks alternate
   black / non-black (e.g. a swirl/portal effect over a black background). */
#define WAIFU_PCFX_DIRTY_MAX_ROW_RUNS WAIFU_PCFX_DIRTY_BLOCKS_X
/* While merging runs into vertical bands, the active table transiently holds the
   previous row's bands plus this row's new runs before unmatched bands are
   flushed, i.e. up to 2*BLOCKS_X.  Sizing the table for that worst case keeps
   the merge from dropping runs (a dropped run never uploads, leaving the hidden
   page stale -> page-flip shows black/garbage during such frames). */
#define WAIFU_PCFX_DIRTY_MAX_BANDS (WAIFU_PCFX_DIRTY_BLOCKS_X * 2)
#define WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS 1024

#if defined(__GNUC__)
#define WAIFU_PCFX_NOINLINE __attribute__((noinline))
#define WAIFU_PCFX_COLD __attribute__((noinline,cold))
#else
#define WAIFU_PCFX_NOINLINE
#define WAIFU_PCFX_COLD
#endif

static uint16_t g_king_microprog[16];

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
    uint32_t ps = 0x01000000u | (page ? 0x00000100u : 0u);
    /* KING PageSetting layout matches liberis eris_king_set_kram_pages():
       byte0=SCSI, byte1=BG, byte2=RAINBOW, byte3=ADPCM.  Keep ADPCM on
       physical page 1, but only select BG physical page 1 when the renderer
       explicitly asks for it.  Do not use 0x0100 as ADPCM -- that is BG page 1
       and corrupts the in-game card/BG plane. */
    __asm__ volatile (
        "movea 15,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.w %[ps],0x604[r0]\n"
        : [reg] "=&r" (reg)
        : [ps] "r" (ps)
        : "memory");
#else
    eris_king_set_kram_pages(0, page ? 1 : 0, 0, 1);
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
    uint32_t row_end, tmp_addr, reg, data, t1, t2, t3, lo, hi;
    if (row_bytes == 0 || rows == 0) return;
    __asm__ volatile (
        "cmp 0,%[rows]\n"
        "be 9f\n"
        "1:\n"
        "mov %[addr],%[tmp]\n"
        "or %[inc],%[tmp]\n"
        "mov %[tmp],%[lo]\n"
        "andi 65535,%[lo],%[lo]\n"
        "mov %[tmp],%[hi]\n"
        "shr 16,%[hi]\n"
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[lo],0x604[r0]\n"
        "out.h %[hi],0x606[r0]\n"
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
          [data] "=&r" (data), [t1] "=&r" (t1), [t2] "=&r" (t2), [t3] "=&r" (t3),
          [lo] "=&r" (lo), [hi] "=&r" (hi)
        : [row_bytes] "r" (row_bytes), [src_delta] "r" (src_delta), [inc] "r" (inc)
        : "memory");
#else
    king_kram_upload_rect_256_bytes(src_arg, page_word_offset_arg, x0_arg, y0_arg, row_bytes_arg, rows_arg);
#endif
}


/* Upload an arbitrary-pitch 8bpp source rectangle to a linear 256-pixel KING
   page.  Unlike pcfx_kram_upload_rect_bytes_inline(), src_arg already points to
   the rectangle's first source byte.  This is used for preloaded 112x112 card art
   so the presenter can stream from the card cache instead of the full CPU
   framebuffer. */
static inline __attribute__((always_inline)) void pcfx_kram_upload_linear_rect_bytes_inline(const uint8_t *src_arg, int page_word_offset_arg, int x0_arg, int y0_arg, int row_bytes_arg, int rows_arg, int src_pitch_arg)
{
#if defined(__v810__)
    const uint8_t *src = src_arg;
    uint32_t word_addr = (uint32_t)(page_word_offset_arg + y0_arg * 128 + (x0_arg >> 1));
    uint32_t rows = (uint32_t)rows_arg;
    uint32_t row_bytes = (uint32_t)row_bytes_arg;
    uint32_t src_delta = (uint32_t)(src_pitch_arg - row_bytes_arg);
    uint32_t inc = (uint32_t)(1u << 18);
    uint32_t row_end, tmp_addr, reg, data, t1, t2, t3, lo, hi;
    if (row_bytes == 0 || rows == 0) return;
    __asm__ volatile (
        "cmp 0,%[rows]\n"
        "be 9f\n"
        "1:\n"
        "mov %[addr],%[tmp]\n"
        "or %[inc],%[tmp]\n"
        "mov %[tmp],%[lo]\n"
        "andi 65535,%[lo],%[lo]\n"
        "mov %[tmp],%[hi]\n"
        "shr 16,%[hi]\n"
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[lo],0x604[r0]\n"
        "out.h %[hi],0x606[r0]\n"
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
          [data] "=&r" (data), [t1] "=&r" (t1), [t2] "=&r" (t2), [t3] "=&r" (t3),
          [lo] "=&r" (lo), [hi] "=&r" (hi)
        : [row_bytes] "r" (row_bytes), [src_delta] "r" (src_delta), [inc] "r" (inc)
        : "memory");
#else
    (void)src_arg; (void)page_word_offset_arg; (void)x0_arg; (void)y0_arg; (void)row_bytes_arg; (void)rows_arg; (void)src_pitch_arg;
#endif
}
#endif /* WAIFU_PCFX_DIRTY_PRESENT (carved out: the full-frame presenter below is always built) */

/* Inline full-frame presenter.  The visible 256x240 8bpp frame is contiguous in
   a KING BG page (CPU row pitch 256 bytes == KRAM row pitch 128 words), so the
   whole frame is one seek + one streamed run.  This is the inline equivalent of
   fastking's _king_kram_write_buffer_bytes_at: one out.w address select, then a
   16-byte-unrolled byte-swapping out.h stream with no jal/rts in the hot loop.
   FRAME_BYTES (61440) is a multiple of 16 so the unrolled body covers it exactly. */
static inline __attribute__((always_inline)) void pcfx_kram_write_frame_inline(const uint8_t *framebuffer, int page_word_offset_arg)
{
#if defined(__v810__)
    const uint8_t *src = framebuffer;
    const uint8_t *src_end = framebuffer + WAIFU_PCFX_FRAME_BYTES;
    uint32_t word_addr = (uint32_t)page_word_offset_arg | (1u << 18);
    uint32_t word_addr_lo = word_addr & 0xffffu;
    uint32_t word_addr_hi = word_addr >> 16;
    uint32_t reg, data, t1, t2, t3;
    __asm__ volatile (
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[addr_lo],0x604[r0]\n"
        "out.h %[addr_hi],0x606[r0]\n"
        "movea 14,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "1:\n"
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
        "cmp %[end],%[src]\n"
        "bl 1b\n"
        : [src] "+r" (src), [reg] "=&r" (reg), [data] "=&r" (data),
          [t1] "=&r" (t1), [t2] "=&r" (t2), [t3] "=&r" (t3)
        : [addr_lo] "r" (word_addr_lo), [addr_hi] "r" (word_addr_hi), [end] "r" (src_end)
        : "memory");
#else
    king_kram_write_buffer_bytes_at((void *)framebuffer, WAIFU_PCFX_FRAME_BYTES, page_word_offset_arg);
#endif
}

#if WAIFU_PCFX_DIRTY_PRESENT
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
    uint32_t tmp_addr, groups, tail, reg, lo, hi;
    __asm__ volatile (
        "cmp 0,%[rows]\n"
        "be 9f\n"
        "1:\n"
        "mov %[addr],%[tmp]\n"
        "or %[inc],%[tmp]\n"
        "mov %[tmp],%[lo]\n"
        "andi 65535,%[lo],%[lo]\n"
        "mov %[tmp],%[hi]\n"
        "shr 16,%[hi]\n"
        "movea 13,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.h %[lo],0x604[r0]\n"
        "out.h %[hi],0x606[r0]\n"
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
          [tail] "=&r" (tail), [tmp] "=&r" (tmp_addr), [reg] "=&r" (reg),
          [lo] "=&r" (lo), [hi] "=&r" (hi)
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


typedef struct PcfxDirectBigArt {
    const uint8_t *src;
    int x;
    int y;
    int width;
    int pitch;
} PcfxDirectBigArt;

static int pcfx_big_art_matches_framebuffer(const uint8_t *src, const uint8_t *framebuffer, int x, int y)
{
    for (int yy = 0; yy < WAIFU_BIG_H; ++yy) {
        const uint8_t *sp = src + yy * WAIFU_BIG_W;
        const uint8_t *fp = framebuffer + (y + yy) * WAIFU_PCFX_W + x;
        for (int xx = 0; xx < WAIFU_BIG_W; ++xx) {
            if (sp[xx] != fp[xx]) return 0;
        }
    }
    return 1;
}

static int pcfx_collect_direct_big_art(PcfxDirectBigArt *out, int max_out, const uint8_t *framebuffer)
{
    int n = 0;
    int count = waifu_assets_big_art_draw_count();
    const WaifuBigArtDraw *draws = waifu_assets_big_art_draws();
    for (int i = 0; i < count && n < max_out; ++i) {
        int x = draws[i].x;
        int y = draws[i].y;
        const uint8_t *src;
        if ((x & 1) != 0) continue;
        if (x < 0 || y < 0 || x + WAIFU_BIG_W > WAIFU_PCFX_W || y + WAIFU_BIG_H > WAIFU_PCFX_H) continue;
        src = waifu_assets_big_art_cached(draws[i].kind, draws[i].card_id);
        if (!src) continue;
        /* Fade/dither/overlay passes can touch the art rectangle after the draw
           marker is emitted.  Only suppress the generic dirty upload when the
           framebuffer still exactly matches the cached source. */
        if (!pcfx_big_art_matches_framebuffer(src, framebuffer, x, y)) continue;
        /* The inline KING rectangle streamer is safe and fastest when every
           upload starts and ends on the presenter's 16-pixel dirty-block grid.
           Fully aligned art windows can stream directly from the resident
           big-art cache.  Battle cut-ins/lunges often draw the 112px art at a
           half-block x (for example 8 or 136); for those, upload the enclosing
           block-aligned 128px framebuffer envelope.  This keeps the moving
           card on aligned edge blocks and prevents the generic dirty planner
           from spending separate work on the left/right slivers. */
        if ((x & (WAIFU_PCFX_DIRTY_BLOCK_W - 1)) == 0) {
            if ((((uintptr_t)src) & 3u) != 0u) continue;
            out[n].src = src;
            out[n].x = x;
            out[n].y = y;
            out[n].width = WAIFU_BIG_W;
            out[n].pitch = WAIFU_BIG_W;
        } else {
            int block_x0 = x & ~(WAIFU_PCFX_DIRTY_BLOCK_W - 1);
            int block_x1 = (x + WAIFU_BIG_W + (WAIFU_PCFX_DIRTY_BLOCK_W - 1)) & ~(WAIFU_PCFX_DIRTY_BLOCK_W - 1);
            if (block_x0 < 0) block_x0 = 0;
            if (block_x1 > WAIFU_PCFX_W) block_x1 = WAIFU_PCFX_W;
            int block_w = block_x1 - block_x0;
            if (block_w <= 0) continue;
            out[n].src = framebuffer + y * WAIFU_PCFX_W + block_x0;
            out[n].x = block_x0;
            out[n].y = y;
            out[n].width = block_w;
            out[n].pitch = WAIFU_PCFX_W;
        }
        if (((out[n].x | out[n].width) & (WAIFU_PCFX_DIRTY_BLOCK_W - 1)) != 0) continue;
        ++n;
    }
    return n;
}

static void pcfx_prime_shadow_for_direct_big_art(uint8_t *shadow, const uint8_t *framebuffer, const PcfxDirectBigArt *arts, int count)
{
    for (int i = 0; i < count; ++i) {
        pcfx_shadow_copy_rect(shadow, framebuffer, arts[i].x, arts[i].y, arts[i].width, WAIFU_BIG_H);
    }
}

static void pcfx_upload_direct_big_art(const PcfxDirectBigArt *arts, int count, int page_word_offset_value)
{
    for (int i = 0; i < count; ++i) {
        pcfx_kram_upload_linear_rect_bytes_inline(arts[i].src, page_word_offset_value,
                                                  arts[i].x, arts[i].y,
                                                  arts[i].width, WAIFU_BIG_H, arts[i].pitch);
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
    PcfxDirtyBand active[WAIFU_PCFX_DIRTY_MAX_BANDS];
    PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS];
    int active_count = 0;
    for (int i = 0; i < WAIFU_PCFX_DIRTY_MAX_BANDS; ++i) active[i].used = 0;

    for (int y = 0; y < WAIFU_PCFX_H; ++y) {
        for (int i = 0; i < active_count; ++i) active[i].matched = 0;
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
            } else if (active_count < WAIFU_PCFX_DIRTY_MAX_BANDS) {
                PcfxDirtyBand *b = &active[active_count++];
                b->x0b = runs[r].x0b;
                b->x1b = runs[r].x1b;
                b->black = runs[r].black;
                b->y0 = (uint8_t)y;
                b->y1 = (uint8_t)y;
                b->used = 1;
                b->matched = 1;
            } else {
                /* Active-band table full (should not happen given the 2*BLOCKS_X
                   sizing, but never drop a run: a dropped run leaves the hidden
                   page stale and the flip shows black/garbage).  Flush it now as
                   a single-row band. */
                PcfxDirtyBand one;
                one.x0b = runs[r].x0b;
                one.x1b = runs[r].x1b;
                one.black = runs[r].black;
                one.y0 = (uint8_t)y;
                one.y1 = (uint8_t)y;
                one.used = 1;
                one.matched = 1;
                pcfx_flush_dirty_band(shadow, framebuffer, page_word_offset_value, &one);
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
#define WAIFU_PCFX_VDC_SANCTUM_TILE_SKY     0x110
#define WAIFU_PCFX_VDC_SANCTUM_TILE_SAND    0x111
#define WAIFU_PCFX_VDC_SANCTUM_TILE_HORIZON 0x112
#define WAIFU_PCFX_VDC_SANCTUM_TILE_BLANK   0x113
/* Fade tiles live immediately after the 64x32 BAT so tile 0 remains unusable
   for transparent blanks.  Each level is a screen-space black dither mask. */
#define WAIFU_PCFX_VDC_FADE_TILE_BASE 0x080
#define WAIFU_PCFX_VDC_FADE_LEVELS 17

static WaifuPcfxVdcBackground g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;

typedef enum WaifuPcfxOverlayMode {
    WAIFU_PCFX_OVERLAY_OFF = 0,
    WAIFU_PCFX_OVERLAY_TITLE_PROMPT = 1,
    WAIFU_PCFX_OVERLAY_MENU = 2,
    WAIFU_PCFX_OVERLAY_LOAD_DEVICE = 3
} WaifuPcfxOverlayMode;

static WaifuPcfxOverlayMode g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF;
static WaifuPcfxOverlayMode g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
static int g_vdc_overlay_prompt_visible = 0;
static int g_vdc_overlay_has_save = 0;
static int g_vdc_overlay_menu_selected = 1;
static int g_vdc_overlay_load_selected = 0;
static int g_vdc_overlay_load_internal_has = 0;
static int g_vdc_overlay_load_external_has = 0;
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

static void pcfx_vdc_sanctum_clear_rect(int tx, int ty, int w, int h)
{
    if (tx < 0) { w += tx; tx = 0; }
    if (ty < 0) { h += ty; ty = 0; }
    if (tx + w > WAIFU_PCFX_VDC_MAP_W) w = WAIFU_PCFX_VDC_MAP_W - tx;
    if (ty + h > WAIFU_PCFX_VDC_MAP_H) h = WAIFU_PCFX_VDC_MAP_H - ty;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; ++row) {
        int addr = (ty + row) * WAIFU_PCFX_VDC_MAP_W + tx;
        eris_low_sup_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < w; ++col) eris_low_sup_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_SANCTUM_TILE_BLANK);
        eris_low_sup_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < w; ++col) eris_low_sup_vram_write(VDC_CHIP_1, WAIFU_PCFX_VDC_SANCTUM_TILE_BLANK);
    }
}

static void pcfx_vdc_restore_overlay_palette(void)
{
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_BLACK, 0x0088);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_WHITE, 0xE088);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_GOLD,  0xB468);
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_RED,   0x5F0F);
}

static void pcfx_vdc_sanctum_upload_solid_tile(uint16_t tile, uint16_t vdc0_row)
{
    eris_low_sup_set_vram_write(VDC_CHIP_0, tile * 16);
    for (int row = 0; row < 8; ++row) eris_low_sup_vram_write(VDC_CHIP_0, vdc0_row);
    for (int row = 0; row < 8; ++row) eris_low_sup_vram_write(VDC_CHIP_0, 0x0000);

    eris_low_sup_set_vram_write(VDC_CHIP_1, tile * 16);
    for (int row = 0; row < 16; ++row) eris_low_sup_vram_write(VDC_CHIP_1, 0x0000);
}

static void pcfx_vdc_sanctum_upload_tiles(void)
{
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_SKY, 0x00ff);
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_SAND, 0xff00);
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_HORIZON, 0xffff);
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_BLANK, 0x0000);
}

static void pcfx_vdc_sanctum_set_palette(void)
{
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_BLACK, rgb888_to_pcfx_yuv(72, 162, 231));
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_WHITE, rgb888_to_pcfx_yuv(207, 169, 95));
    eris_tetsu_set_palette(WAIFU_PCFX_VDC_PAL_GOLD,  rgb888_to_pcfx_yuv(234, 205, 137));
}

static void pcfx_vdc_sanctum_fill_map(WaifuPcfxVdcBackground bg)
{
    pcfx_vdc_sanctum_set_palette();

    for (int row = 0; row < WAIFU_PCFX_VDC_MAP_H; ++row) {
        uint16_t tile = WAIFU_PCFX_VDC_SANCTUM_TILE_SAND;
        if (row < 14) tile = WAIFU_PCFX_VDC_SANCTUM_TILE_SKY;
        else if (row == 14) tile = WAIFU_PCFX_VDC_SANCTUM_TILE_HORIZON;

        eris_low_sup_set_vram_write(VDC_CHIP_0, row * WAIFU_PCFX_VDC_MAP_W);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) eris_low_sup_vram_write(VDC_CHIP_0, tile);
        eris_low_sup_set_vram_write(VDC_CHIP_1, row * WAIFU_PCFX_VDC_MAP_W);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) {
            eris_low_sup_vram_write(VDC_CHIP_1, 0);
        }
    }

    /* VDC is mixed in front for this 2D background, so punch transparent tile
       windows wherever the CPU framebuffer draws opaque menu panels and text. */
    if (bg == WAIFU_PCFX_VDC_BG_SANCTUM_SAVE) {
        pcfx_vdc_sanctum_clear_rect(3, 8, 26, 14);
    } else {
        pcfx_vdc_sanctum_clear_rect(14, 4, 18, 19);
        pcfx_vdc_sanctum_clear_rect(15, 24, 17, 6);
    }
}

static void pcfx_vdc_apply_sanctum_background(WaifuPcfxVideo *video, WaifuPcfxVdcBackground bg)
{
    if (!video) return;
    if (video->vdc_bg == bg) {
        pcfx_vdc_sanctum_set_palette();
        eris_tetsu_set_priorities(7, 0, 6, 0, 0, 0, 0);
        return;
    }

    video->vdc_overlay_ready = 0;
    video->vdc_overlay_shutdown_countdown = 0;
    g_vdc_overlay_dirty = 0;
    g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
    g_vdc_overlay_applied_fade_level = -1;

    eris_low_sup_set_control(VDC_CHIP_0, 0, 1, 0);
    eris_low_sup_set_control(VDC_CHIP_1, 0, 1, 0);
    eris_low_sup_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_0, 0, 0);
    eris_low_sup_set_scroll(VDC_CHIP_1, 0, 0);
    eris_low_sup_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    eris_low_sup_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    eris_low_sup_setreg(VDC_CHIP_0, 5, 0x88);
    eris_low_sup_setreg(VDC_CHIP_1, 5, 0x80);

    pcfx_vdc_sanctum_upload_tiles();
    pcfx_vdc_sanctum_fill_map(bg);
    eris_tetsu_set_priorities(7, 0, 6, 0, 0, 0, 0);
    video->vdc_bg = bg;
}

static void pcfx_vdc_clear_background(WaifuPcfxVideo *video)
{
    if (!video || video->vdc_bg == WAIFU_PCFX_VDC_BG_NONE) return;
    pcfx_vdc_sanctum_clear_rect(0, 0, WAIFU_PCFX_VDC_MAP_W, WAIFU_PCFX_VDC_MAP_H);
    pcfx_vdc_restore_overlay_palette();
    eris_tetsu_set_priorities(1, 0, 7, 0, 0, 0, 0);
    video->vdc_bg = WAIFU_PCFX_VDC_BG_NONE;
}

static void pcfx_vdc_apply_requested_background(WaifuPcfxVideo *video, WaifuPcfxVdcBackground bg)
{
    if (!video) return;
    switch (bg) {
    case WAIFU_PCFX_VDC_BG_SANCTUM:
    case WAIFU_PCFX_VDC_BG_SANCTUM_SAVE:
        pcfx_vdc_apply_sanctum_background(video, bg);
        break;
    default:
        pcfx_vdc_clear_background(video);
        break;
    }
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
            pcfx_vdc_overlay_print(7, 23, "PRESS RUN TO START", 22);
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

    case WAIFU_PCFX_OVERLAY_LOAD_DEVICE:
        pcfx_vdc_overlay_clear_rect(4, 15, 28, 13);
        pcfx_vdc_overlay_print(9, 16, "LOAD FROM", 16);
        pcfx_vdc_overlay_print(7, 18,
            g_vdc_overlay_load_selected == 0
                ? (g_vdc_overlay_load_internal_has ? "> INTERNAL    " : "> INTERNAL  --")
                : (g_vdc_overlay_load_internal_has ? "  INTERNAL    " : "  INTERNAL  --"), 16);
        pcfx_vdc_overlay_print(7, 20,
            g_vdc_overlay_load_selected == 1
                ? (g_vdc_overlay_load_external_has ? "> FX-BMP    " : "> FX-BMP  --")
                : (g_vdc_overlay_load_external_has ? "  FX-BMP    " : "  FX-BMP  --"), 16);
        pcfx_vdc_overlay_print(7, 22, g_vdc_overlay_load_selected == 2 ? "> BACK" : "  BACK", 16);
        if (g_vdc_overlay_load_selected == 0) {
            pcfx_vdc_overlay_print(6, 25,
                g_vdc_overlay_load_internal_has ? "INTERNAL BACKUP RAM" : "NO SAVE ON INTERNAL", 26);
        } else if (g_vdc_overlay_load_selected == 1) {
            pcfx_vdc_overlay_print(6, 25,
                g_vdc_overlay_load_external_has ? "EXTERNAL FX-BMP CARD" : "NO SAVE ON FX-BMP", 26);
        } else {
            pcfx_vdc_overlay_print(6, 25, "RETURN TO MENU", 26);
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

void waifu_pcfx_video_overlay_load_menu(int selected, int internal_has_save, int external_has_save)
{
    if (selected < 0) selected = 0;
    if (selected > 2) selected = 2;
    internal_has_save = internal_has_save ? 1 : 0;
    external_has_save = external_has_save ? 1 : 0;
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_LOAD_DEVICE ||
        g_vdc_overlay_load_selected != selected ||
        g_vdc_overlay_load_internal_has != internal_has_save ||
        g_vdc_overlay_load_external_has != external_has_save) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_LOAD_DEVICE;
        g_vdc_overlay_load_selected = selected;
        g_vdc_overlay_load_internal_has = internal_has_save;
        g_vdc_overlay_load_external_has = external_has_save;
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
    /* In 16M KING mode with the dual-VDC overlay enabled, the top four
       visible scanlines sample the wrapped hidden rows unless BG0 is scrolled
       down by four pixels.  Keep the title/Menu surface aligned to the
       256x240 framebuffer instead of exposing stale hidden KRAM at the top. */
    eris_king_set_scroll(KING_BG0, 0, WAIFU_PCFX_TITLE_16M_SCROLL_Y);
    eris_king_set_scroll(KING_BG0SUB, 0, WAIFU_PCFX_TITLE_16M_SCROLL_Y);
    eris_king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
    eris_king_set_bg_size(KING_BG0SUB, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
}

static WAIFU_PCFX_COLD void pcfx_title_upload_full_16m_from_ram(int page, const uint16_t *title_yuv422)
{
    uint32_t page_base = title16m_page_word_offset(page);
    const uint16_t *first_row;
    const uint16_t *last_row;
    int row;
    if (!title_yuv422) return;

    /* Runtime keeps the 16M title BG scrolled down by four rows.  Mirror the
       CD blob layout here: rows 0..3 are safe top padding, rows 4..243 contain
       the exact 256x240 title, and rows 244..255 repeat the final source row.
       This preserves the original bottom art instead of showing repeated/black
       padding in the last visible scanlines. */
    first_row = title_yuv422;
    last_row = title_yuv422 + (WAIFU_PCFX_H - 1) * WAIFU_PCFX_W;

    king_seek_write_words(page_base);
    for (row = 0; row < WAIFU_PCFX_TITLE_16M_SCROLL_Y; ++row) {
        king_kram_write_buffer((void *)first_row, WAIFU_PCFX_W * 2);
    }
    king_kram_write_buffer((void *)title_yuv422, WAIFU_PCFX_TITLE_16M_VISIBLE_WORDS * 2);
    for (row = WAIFU_PCFX_TITLE_16M_SCROLL_Y + WAIFU_PCFX_H;
         row < WAIFU_PCFX_TITLE_16M_KRAM_ROWS; ++row) {
        king_kram_write_buffer((void *)last_row, WAIFU_PCFX_W * 2);
    }
}

static WAIFU_PCFX_COLD int pcfx_title_upload_full_16m_direct_cd(int page)
{
#if defined(WAIFU_ASSET_USE_CDROM)
    /* Direct CD/SCSI DMA into KING KRAM, matching liberis eris_cd_read_kram():
       kram_addr is a KING word address and the payload is the full 256x256
       16M page.  This replaces the old 128 KiB CPU-side title buffer entirely. */
    return waifu_pcfx_cdrom_read_title_yuv422_to_kram(title16m_page_word_offset(page),
                                                     WAIFU_PCFX_TITLE_16M_KRAM_WORDS * 2u);
#else
    (void)page;
    return 0;
#endif
}

static WAIFU_PCFX_COLD void pcfx_title_blackout_pages(WaifuPcfxVideo *video)
{
    /* Do not rewrite the title KING surface on exit.  The title image is a
       one-shot CD->KRAM DMA asset; after that, title/menu fades are VDC-only.
       Cover mode switches with the opaque VDC fade layer instead. */
    if (video) {
        pcfx_vdc_overlay_force_black(video);
        video->title16m_page_valid[0] = 0;
        video->title16m_page_valid[1] = 0;
        video->pending_title_page_flip = 0;
    }
}

static WAIFU_PCFX_COLD void pcfx_present_title_16m(WaifuPcfxVideo *video, const uint8_t *framebuffer)
{
    int reconfigure;
    int need_upload;

    (void)framebuffer;
    if (!video) return;

    reconfigure = (!video->initialized || video->mode != WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR);
    if (reconfigure) {
        set_king_16m_title_video();
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_set_bg0_page_inline(title16m_bg_cg_page(0));
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

    need_upload = reconfigure || !video->title16m_page_valid[0];
    if (need_upload) {
        int ok = pcfx_title_upload_full_16m_direct_cd(0);
        if (!ok) {
            const uint16_t *title_yuv422 = waifu_assets_title_screen_pcfx_yuv422();
            if (title_yuv422) {
                pcfx_title_upload_full_16m_from_ram(0, title_yuv422);
                ok = 1;
            }
        }
        if (ok) {
            video->title16m_page_valid[0] = 1;
            video->front_page = 0;
            video->back_page = 0;
            video->have_last_frame = 1;
        }
    }
}

static void set_king_8bpp_video(int display_page)
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
    /* Point the display at the caller-selected page directly as the BG mode
       becomes 8bpp.  The transition uses the second 8bpp page (which aliases no
       16M title CG page) so the visible page is clean black across the mid-scan
       16M -> 8bpp switch. */
    pcfx_king_set_bg0_page_inline(page_bat_offset(display_page));
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
    g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;
    video->vdc_bg = WAIFU_PCFX_VDC_BG_NONE;
    pcfx_vdc_overlay_force_black(video);
    if (video->mode == WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR) pcfx_title_blackout_pages(video);
    /* The 16M->8bpp mode switch happens mid-scan, and KRAM word offset 0 is
       shared by 8bpp page 0 and 16M title CG page 0.  No single value is black
       in both interpretations (16M-black 0x0101/0x8080 reads as an 8bpp index
       stripe pattern; 8bpp-black 0xFFFF reads as bright 16M), so a page at
       offset 0 always shows a stripe band or a white line on the switch frame.
       Instead, keep offset 0 as 16M-black (from pcfx_title_blackout_pages) and
       display the SECOND 8bpp page (offset PAGE_STRIDE_WORDS), which aliases no
       16M CG page.  Whichever layer a scanline samples across the switch -- the
       old 16M surface at offset 0, or the new 8bpp page -- it reads black. */
    king_seek_write_words(WAIFU_PCFX_PAGE_STRIDE_WORDS);
    king_kram_fill_words(WAIFU_PCFX_BLACK_WORD, WAIFU_PCFX_PAGE_STRIDE_WORDS);
    set_king_8bpp_video(1);
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
    /* Front is the freshly cleared second page; the next present uploads a real
       frame into page 0 (whose KRAM still holds 16M-black/stripe bytes) before
       it is ever displayed, so force its shadow invalid. */
    video->front_page = 1;
    video->back_page = 0;
#if WAIFU_PCFX_DIRTY_PRESENT
    memset(video->page_shadow[1], IDX_BLACK, WAIFU_PCFX_FRAME_BYTES);
    video->page_shadow_valid[1] = 1;
    video->page_shadow_valid[0] = 0;
#endif
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
    pcfx_vdc_apply_requested_background(video, bg);
}

void waifu_pcfx_video_request_vdc_background(WaifuPcfxVdcBackground bg)
{
    g_vdc_bg_requested = bg;
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
    pcfx_kram_write_frame_inline(framebuffer, page_word_offset(video->back_page));
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
        pcfx_present_title_16m(video, framebuffer);
        return;
    }
    if (!video->initialized || video->mode != WAIFU_PCFX_VIDEO_MODE_KING_8BPP) {
        waifu_pcfx_video_begin_8bpp(video);
    }
    int fade_q8 = waifu_fm_video_fade_q8();
    int fade_level = fade_q8_to_level(fade_q8);
    if (video->active_palette != palette_id || video->active_fade_q8 != fade_level) {
        pcfx_present_update_palette_if_needed(video, rgb, palette_id, fade_q8);
    }
    pcfx_vdc_apply_requested_background(video, g_vdc_bg_requested);
    g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;

#if WAIFU_PCFX_DIRTY_PRESENT
    int upload_full = 0;
    uint8_t *shadow = video->page_shadow[video->back_page];
    PcfxDirectBigArt direct_art[WAIFU_ASSET_BIG_ART_DRAW_MAX];
    int direct_art_count = 0;

    if (!video->page_shadow_valid[video->back_page]) {
        upload_full = 1;
    } else {
        /* Large 112x112 card-art draws are usually already resident in the
           global CD cache after the duel prewarm.  Prime block-aligned direct
           art regions before the generic dirty-band planner, then stream them
           to KING KRAM.  Aligned card-art windows use the cached art source;
           half-block battle positions use a 16-pixel-aligned framebuffer
           envelope so moving cards always update whole edge blocks. */
#if WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
        direct_art_count = pcfx_collect_direct_big_art(direct_art, WAIFU_ASSET_BIG_ART_DRAW_MAX, framebuffer);
        if (direct_art_count > 0) pcfx_prime_shadow_for_direct_big_art(shadow, framebuffer, direct_art, direct_art_count);
#else
        (void)direct_art;
        (void)direct_art_count;
#endif

        PcfxDirtyPlanStats dirty_stats = pcfx_dirty_plan_stats(framebuffer, shadow);
        if (dirty_stats.dirty_blocks == 0) {
#if WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
            if (direct_art_count > 0) pcfx_upload_direct_big_art(direct_art, direct_art_count, page_word_offset(video->back_page));
#endif
            /* The hidden page already holds this exact frame; just flip to it. */
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
#if WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
        if (direct_art_count > 0) pcfx_upload_direct_big_art(direct_art, direct_art_count, page_word_offset(video->back_page));
#endif
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
       CPU framebuffer are little-endian byte arrays.  Stream it straight to the
       hidden KRAM page with the inline byte-swapping writer (no fastking jal/rts
       in the present hot path); the BG0 page flip below then makes it visible. */
    pcfx_kram_write_frame_inline(framebuffer, page_word_offset(video->back_page));
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
    pcfx_present_title_16m(video, framebuffer);
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
