#include "waifu_pcfx_video.h"
#include "fastking.h"

#include <pcfx/king.h>
#include <pcfx/tetsu.h>
#include <pcfx/vdc.h>
#include <pcfx/v810.h>
#include <stdint.h>
#include <string.h>
#include "waifu_assets.h"
#include "assets.h"
#include "platform.h"
#include "pcfx_palette_assets.h"
#include "waifu_pcfx_cdrom.h"
#include "title_asset.h"
#include "rainbow_bg_assets.h"
#include "font_menudata.h"

/* --- liberis 7up -> libpcfx VDC compatibility helpers ------------------------
   libpcfx's VDC layer (vdc_setreg + friends) is register-oriented and, unlike
   the old liberis eris_low_sup_* API, has no set_control / set_video_mode /
   set_access_width helpers and cannot read registers back.  These small wrappers
   reproduce exactly what liberis wrote (verified against liberis src/low/7up.S)
   so the migration keeps identical VDC programming.  set_control needs a
   read-modify-write of CR (reg 5) to preserve the interrupt-enable bits, so a
   software shadow of CR is kept here (init 0 == post-vdc_init hardware state). */
#ifndef SUP_LOW_MAP_64X32
#define SUP_LOW_MAP_64X32 1  /* liberis sup_low_mapsize: 64x32 tile virtual map */
#endif

static uint16_t g_waifu_vdc_cr[2] = { 0, 0 };

static void waifu_vdc_setreg(int chip, int reg, int value)
{
    if (reg == 0x05)
        g_waifu_vdc_cr[chip & 1] = (uint16_t)value;
    vdc_setreg(chip, reg, value);
}

static void waifu_vdc_set_control(int chip, int increment, int bg_show, int spr_show)
{
    /* Keep everything except the increment field (bits 11-12) and the BG/sprite
       show bits (7/6); mirrors liberis' 0xE73F keep-mask read-modify-write. */
    uint16_t cr = (uint16_t)((g_waifu_vdc_cr[chip & 1] & 0xE73F)
                             | ((increment & 3) << 11)
                             | ((bg_show   & 1) << 7)
                             | ((spr_show  & 1) << 6));
    g_waifu_vdc_cr[chip & 1] = cr;
    vdc_setreg(chip, 0x05, cr);
}

static void waifu_vdc_set_video_mode(int chip, int hdispstrt, int hsyncwid,
        int hdispend, int hdispwid, int vdispstrt, int vsyncwid,
        int vdispwid, int vdispend)
{
    vdc_setreg(chip, 0x0A, ((hdispstrt & 0xFF) << 8) | (hsyncwid & 0xFF)); /* HSR */
    vdc_setreg(chip, 0x0B, ((hdispend  & 0xFF) << 8) | (hdispwid & 0xFF)); /* HDR */
    vdc_setreg(chip, 0x0C, ((vdispstrt & 0xFF) << 8) | (vsyncwid & 0xFF)); /* VPR */
    vdc_setreg(chip, 0x0D, vdispwid & 0xFFFF);                             /* VDR */
    vdc_setreg(chip, 0x0E, vdispend & 0xFFFF);                             /* VCR */
}

static void waifu_vdc_set_access_width(int chip, int cg_mode, int mapsize,
                                       int spr_px_w, int vram_px_w)
{
    vdc_setreg(chip, 0x09,                                                 /* MWR */
               ((cg_mode & 1) << 7)
               | (mapsize << 4)
               | ((spr_px_w & 3) << 2)
               | (vram_px_w & 3));
}
/* --------------------------------------------------------------------------- */

#define WAIFU_PCFX_W WAIFU_FM_WIDTH
#define WAIFU_PCFX_H WAIFU_FM_HEIGHT
#define WAIFU_PCFX_FRAME_BYTES (WAIFU_PCFX_W * WAIFU_PCFX_H)
#define WAIFU_PCFX_FRAME_WORDS (WAIFU_PCFX_FRAME_BYTES / 2)
#define WAIFU_PCFX_TITLE_16M_VISIBLE_WORDS (WAIFU_PCFX_W * WAIFU_PCFX_H)
#define WAIFU_PCFX_TITLE_16M_KRAM_ROWS 256
#define WAIFU_PCFX_TITLE_16M_KRAM_WORDS (WAIFU_PCFX_W * WAIFU_PCFX_TITLE_16M_KRAM_ROWS)
#define WAIFU_PCFX_TITLE_16M_SCROLL_Y 4
/* KING BG0 is an affine 512x256 8bpp source.  A=2.0 makes the 256-dot display
   sample every other source texel, so every logical framebuffer pixel occupies
   an entire KRAM word (both bytes contain the same palette index).  This avoids
   the paired-pixel/halfword artefacts of the old 256x256 packed BG0 path. */
#define WAIFU_PCFX_BG_SOURCE_W 512
#define WAIFU_PCFX_BG_SOURCE_H 256
#define WAIFU_PCFX_BG_ROW_WORDS (WAIFU_PCFX_BG_SOURCE_W / 2)
#define WAIFU_PCFX_PAGE_STRIDE_WORDS ((WAIFU_PCFX_BG_SOURCE_W * WAIFU_PCFX_BG_SOURCE_H) / 2)
#define WAIFU_PCFX_KING_BG_PAGE_0 0
#define WAIFU_PCFX_KING_BG_PAGE_1 (WAIFU_PCFX_PAGE_STRIDE_WORDS / 1024)
/* KING 8bpp gameplay is DOUBLE buffered.  The two 128 KB affine sources live at
   KRAM words 0 / 0x10000 (CG bases 0 / 64), both inside KRAM bank 0 so a single
   programmed BG rotation microprogram bank serves both.  The display flip is
   deferred to the tear-safe vblank window, and the game presents exactly once
   per field (present -> wait_vblank in pcfx_main), so the just-shown page is
   always free again before the next present draws into it -- a third buffer
   would only ever sit idle. */
#define WAIFU_PCFX_PAGE_COUNT 2
#define WAIFU_PCFX_NEUTRAL_BLACK 0x0088u
#define WAIFU_PCFX_16M_BLACK_Y 0x0101u
#define WAIFU_PCFX_16M_BLACK_UV 0x8080u
#define WAIFU_PCFX_BLACK_WORD ((uint16_t)((IDX_BLACK << 8) | IDX_BLACK))
#define WAIFU_PCFX_KRAM_PAGESETTING_RAINBOW1 0x00001000u
#ifndef WAIFU_PCFX_DIRTY_PRESENT
/* Keep a logical shadow for each affine source page and emit only source texels
   that differ.  Each changed byte becomes one complete duplicated KRAM word;
   do not substitute masked writes here, as affine fetches can expose stale
   neighbours after a page flip.  Set to 0 only when diagnosing the full-upload
   fallback. */
#define WAIFU_PCFX_DIRTY_PRESENT 1
#endif
#ifndef WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
/* Keep big-card art in the same CPU-framebuffer dirty-band path as every other
   pixel.  The separate direct uploader can desync page_shadow from KING KRAM,
   leaving large card/portrait windows blank or stale after page flips. */
#define WAIFU_PCFX_DIRECT_BIG_ART_ENABLE 0
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
static uint32_t g_king_page_setting_extra;

static int pcfx_tetsu_raster_stable(void);

/* HuC6261 vertical timing (C6261 SVB/EVB): the 240-line picture occupies
   rasters 22..261.  Rasters 240..258 are therefore still active display, not
   blanking; writing VCE palettes, KING live registers or the paired VDC SAT
   there produces hardware-only noise/tearing that pcfxemu does not model. */
#define WAIFU_PCFX_VBLANK_SVB 262u
#define WAIFU_PCFX_VBLANK_EVB  22u

static int pcfx_raster_in_vblank(unsigned raster)
{
    return raster >= WAIFU_PCFX_VBLANK_SVB ||
           raster < WAIFU_PCFX_VBLANK_EVB;
}

/* Blanking is not "safe" the moment the raster reports it: what a caller
   actually needs is enough of the window LEFT for its burst.  A palette flush
   is a couple of thousand cycles and a line is ~1365, so reserve a handful of
   lines; anything entering blanking later than that must wait for the next
   window rather than spill its writes into active display. */
#define WAIFU_PCFX_VBLANK_RESERVE 6u

static int pcfx_blank_window_has_room(unsigned raster)
{
    if (raster >= WAIFU_PCFX_VBLANK_SVB) return 1;   /* window just opened */
    return raster + WAIFU_PCFX_VBLANK_RESERVE <= WAIFU_PCFX_VBLANK_EVB;
}

/* Spin until the VCE reports a raster inside the vertical blanking window with
   room to spare.  Arriving already inside blanking is the case this exists
   for: the plain "is it blanking?" test returned instantly at, say, raster 21,
   and the caller's burst -- 256 palette entries on a fade, or a KING
   reconfiguration -- then ran on into active display, which is exactly the
   write it was trying to avoid.  Too late in the window now falls through to
   the next one.  Bounded, so a misbehaving VCE degrades to "did not wait"
   rather than hanging. */
static void pcfx_wait_blank_window(void)
{
    uint32_t spin = 0;
    while (!pcfx_blank_window_has_room((unsigned)pcfx_tetsu_raster_stable()) &&
           spin++ < 2000000u) { }
}

/* ---- VCE (HuC6261) colour palette staging ---------------------------------
   C6261 2.1.3 (5), colour palette DATA WRITE register (CPW, R02):
   "Writing data to the colour palette RAM DURING DISPLAY shows NOISE ON THE
   SCREEN."  (6), the palette READ register, carries the identical warning.
   This is the hardware's own statement, not an inference.

   Every palette upload in this port went straight to the VCE from inside
   waifu_pcfx_video_present_8bpp(), and pcfx_main's loop is
   present -> wait_vblank, so a present always begins at the TOP OF ACTIVE
   DISPLAY.  A fade rewrites all 256 KING entries every field, and begin_8bpp
   blackens all 256 at the mode switch, so the boot/loading window and every
   fade sprayed hundreds of palette writes across live rasters -- exactly the
   brief lines and colour glitches that show on console and never under
   pcfxemu, whose VCE just stores palette writes into an array with no noise
   model.

   So stage entries here and flush the dirty span inside the blanking window.
   The whole 512-entry palette costs a couple of thousand cycles against the
   ~30k the 22-line window affords, and the flush is span-bounded anyway. */
#define WAIFU_PCFX_VCE_PAL_ENTRIES 512
static uint16_t g_vce_pal[WAIFU_PCFX_VCE_PAL_ENTRIES];
static uint32_t g_vce_pal_known[WAIFU_PCFX_VCE_PAL_ENTRIES / 32];
static int g_vce_pal_lo = WAIFU_PCFX_VCE_PAL_ENTRIES;
static int g_vce_pal_hi = -1;

/* Replaces every direct tetsu_set_palette() call in this file.  Entries whose
   staged value is already known to be live are dropped, which is what keeps a
   steady-state fade level or a re-selected identical palette from re-flushing
   256 entries per field. */
static inline __attribute__((always_inline)) void pcfx_vce_set_palette(uint16_t entry, uint16_t yuv)
{
    unsigned i = (unsigned)entry;
    if (i >= WAIFU_PCFX_VCE_PAL_ENTRIES) return;
    if ((g_vce_pal_known[i >> 5] & (1u << (i & 31))) != 0 && g_vce_pal[i] == yuv) return;
    g_vce_pal[i] = yuv;
    g_vce_pal_known[i >> 5] |= 1u << (i & 31);
    if ((int)i < g_vce_pal_lo) g_vce_pal_lo = (int)i;
    if ((int)i > g_vce_pal_hi) g_vce_pal_hi = (int)i;
}

/* Push the staged entries to the VCE.  ONLY call from inside blanking.
   CPA (R01) auto-increments on every CPW (R02) access (C6261 2.1.3 (4)), so the
   palette address goes out once per RUN and the data streams from there.

   Runs, not one span: entries this port has never staged must be SKIPPED.  The
   dirty span can straddle the gap between the 0..255 KING palette and the VDC
   overlay entries at 256+, and blanket-writing the gap would push the shadow's
   initial 0x0000 into VCE entries nobody here owns -- Y=0,U=0,V=0 is a
   saturated green (this exact mistake washed wolf-pcfx's menu green when the
   same fix was ported there).  Break the run at every unknown entry. */
static void pcfx_vce_palette_flush(void)
{
    int lo = g_vce_pal_lo;
    int hi = g_vce_pal_hi;
    int i;
    if (hi < lo) return;
    g_vce_pal_lo = WAIFU_PCFX_VCE_PAL_ENTRIES;
    g_vce_pal_hi = -1;
    i = lo;
    while (i <= hi) {
        if ((g_vce_pal_known[i >> 5] & (1u << (i & 31))) == 0) { ++i; continue; }
#if defined(__v810__)
        {
            uint32_t reg = 1;
            uint32_t val = (uint32_t)i;
            __asm__ volatile (
                "out.h %[reg],0x300[r0]\n"
                "out.h %[val],0x304[r0]\n"
                : : [reg] "r" (reg), [val] "r" (val) : "memory");
            reg = 2;
            __asm__ volatile ("out.h %[reg],0x300[r0]\n" : : [reg] "r" (reg) : "memory");
            do {
                uint32_t c = g_vce_pal[i];
                __asm__ volatile ("out.h %[c],0x304[r0]\n" : : [c] "r" (c) : "memory");
                ++i;
            } while (i <= hi && (g_vce_pal_known[i >> 5] & (1u << (i & 31))) != 0);
        }
#else
        do {
            tetsu_set_palette((uint16_t)i, g_vce_pal[i]);
            ++i;
        } while (i <= hi && (g_vce_pal_known[i >> 5] & (1u << (i & 31))) != 0);
#endif
    }
}

/* Cold paths that stage a palette without a following present (the mode-switch
   blackout in begin_8bpp) need it live now; wait one blanking window and go. */
static void pcfx_vce_palette_flush_in_blank(void)
{
    pcfx_wait_blank_window();
    pcfx_vce_palette_flush();
}

/* ---- KING reconfiguration window ------------------------------------------
   C6272_1 (24) BG format register REG.10: "change this register while the
   microprogram is stopped", and its content is IMMEDIATELY effective.
   C6272_1 (23) KRAM page register REG.0F: rewriting the BG page bit is
   FORBIDDEN while BG display is running -- the permitted windows are the
   vertical blanking period or a microprogram (MPSW) stop.
   C6272_2 3.6.6: writing an immediate-effect register during the display
   period disturbs THAT RASTER, and "while MPSW is 0 the image is disturbed".

   Both king_set_bg_mode() and the microprogram download were being issued
   mid-active-display from present-time paths (set_king_8bpp_video,
   set_king_16m_title_video, the RAINBOW apply and the VDC background clear),
   with MPSW left running for the REG.10 write.  Wrap those sequences so they
   run inside blanking with the microprogram stopped, as the manual requires.
   Nesting is counted because the microprogram download helpers below are
   called both standalone and from inside a larger reconfiguration. */
static int g_king_reconfig_depth;

static void pcfx_king_reconfig_begin(void)
{
    if (g_king_reconfig_depth++ == 0) {
        pcfx_wait_blank_window();
        king_disable_microprogram();
    }
}

static void pcfx_king_reconfig_end(void)
{
    if (g_king_reconfig_depth > 0 && --g_king_reconfig_depth == 0)
        king_enable_microprogram();
}

struct WaifuPcfxVideo {
    int front_page;   /* KRAM 8bpp page currently latched on the display */
    int back_page;    /* KRAM 8bpp page the next present renders into */
    int pending_page; /* 8bpp page drawn and awaiting its vblank flip (-1 = none) */
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
    /* Frames to wait before re-attempting a 16M title/ending upload that the
       drive refused.  Without it a single failed upload retries every frame
       forever -- see pcfx_present_title_16m(). */
    uint8_t title16m_upload_backoff;
    int vdc_overlay_ready;
    int vdc_overlay_shutdown_countdown;
    int have_last_frame;
    WaifuPcfxVideoMode mode;
    WaifuPcfxVdcBackground vdc_bg;
#if WAIFU_PCFX_DIRTY_PRESENT
    uint8_t page_shadow[WAIFU_PCFX_PAGE_COUNT][WAIFU_PCFX_FRAME_BYTES] __attribute__((aligned(4)));
    uint8_t page_shadow_valid[WAIFU_PCFX_PAGE_COUNT];
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
    /* page 0/1 -> KRAM word 0 / 0x10000 (both in bank 0) */
    return page * WAIFU_PCFX_PAGE_STRIDE_WORDS;
}

static inline __attribute__((always_inline)) int page_bat_offset(int page)
{
    /* page 0/1 -> BG0/BG0SUB CG base 0 / 64 (1024-word units) */
    return page * WAIFU_PCFX_KING_BG_PAGE_1;
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
   path is compact: 32 bytes per loop, plus a tiny byte tail.

   Both call sites copy the CPU framebuffer into the page shadow (a full frame in
   pcfx_present_full_upload, a dirty band in pcfx_shadow_copy_rect) -- two
   separate ~60 KB arrays in different 2 KiB DRAM pages.  The V810 has no data
   cache and charges +3 cyc per 2 KiB page *change*, so batch all eight loads
   (r10..r17) THEN all eight stores: one contiguous src run + one contiguous dst
   run costs ~2 page changes per 32-byte group instead of the ~15 the old
   interleaved ld/st paid.  (Same fix as main.c copy_u8_fast.) */
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
            "ld.w 4[%[src]],r11\n" \
            "ld.w 8[%[src]],r12\n" \
            "ld.w 12[%[src]],r13\n" \
            "ld.w 16[%[src]],r14\n" \
            "ld.w 20[%[src]],r15\n" \
            "ld.w 24[%[src]],r16\n" \
            "ld.w 28[%[src]],r17\n" \
            "st.w r10,0[%[dst]]\n" \
            "st.w r11,4[%[dst]]\n" \
            "st.w r12,8[%[dst]]\n" \
            "st.w r13,12[%[dst]]\n" \
            "st.w r14,16[%[dst]]\n" \
            "st.w r15,20[%[dst]]\n" \
            "st.w r16,24[%[dst]]\n" \
            "st.w r17,28[%[dst]]\n" \
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
            : "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "memory"); \
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


/* Last value written to KING REG.0F, or -1 when unknown (boot / after any path
   that may have let the BIOS touch it). */
static int32_t g_king_page_setting_live = -1;

static inline __attribute__((always_inline)) void pcfx_king_set_bg_kram_page_inline(int page)
{
#if defined(__v810__)
    uint32_t reg;
    uint32_t ps = 0x00000100u | g_king_page_setting_extra | (page ? 0x00000010u : 0u);
    /* C6272_1 (23): rewriting a REG.0F page bit while the corresponding
       transfer/display is running is forbidden -- the BG bit may only change in
       vertical blanking or with MPSW stopped.  The RAINBOW branch of
       present_8bpp re-issued this register with an UNCHANGED value on every
       single field, from the top of active display.  Drop redundant writes so
       the register is only ever touched when the value genuinely changes (which
       happens inside pcfx_king_reconfig_begin/end windows). */
    if ((int32_t)ps == g_king_page_setting_live) return;
    g_king_page_setting_live = (int32_t)ps;
    /* Preserve ADPCM page 1 and any active RAINBOW page selection while flipping
       the KING BG page.  pcfxemu's ADPCM page bit is 0x100 and RAINBOW page bit
       is 0x1000; clearing either makes those decoders read page 0. */
    __asm__ volatile (
        "movea 15,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        "out.w %[ps],0x604[r0]\n"
        : [reg] "=&r" (reg)
        : [ps] "r" (ps)
        : "memory");
#else
    {
        int32_t ps = (int32_t)(0x00000100u | g_king_page_setting_extra | (page ? 0x00000010u : 0u));
        if (ps == g_king_page_setting_live) return;
        g_king_page_setting_live = ps;
    }
    king_set_kram_pages(0, page ? 1 : 0, g_king_page_setting_extra ? 1 : 0, 1);
#endif
}

static inline __attribute__((always_inline)) void pcfx_king_set_bg0_page_inline(int cg_page)
{
#if defined(__v810__)
    uint32_t page = (uint32_t)cg_page;
    uint32_t reg;
    /* Inline king_set_bat_cg_addr(KING_BG0,0,page) and the matching
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
    king_set_bat_cg_addr(KING_BG0, 0, (uint32_t)cg_page);
    king_set_bat_cg_addr(KING_BG0SUB, 0, (uint32_t)cg_page);
#endif
}

/* Raw KING indexed 16-bit register write (index port 0x600, data port 0x604).
   libpcfx exposes no setter for REG.16, and the affine block is written through
   fastking's own out.h pairs, so keep one local primitive rather than pulling in
   a call for a two-instruction sequence. */
static inline __attribute__((always_inline)) void pcfx_king_reg16(uint32_t reg, uint32_t val)
{
#if defined(__v810__)
    __asm__ volatile (
        "out.h %[reg],0x600[r0]\n"
        "out.h %[val],0x604[r0]\n"
        :
        : [reg] "r" (reg), [val] "r" (val)
        : "memory");
#else
    (void)reg; (void)val;
#endif
}

/* ---- BG0 rotation microprogram, per displayed KRAM bank -------------------
   Vendored from wolf-pcfx (platform/wolf_video.c king_bg0_rotate_mprog), whose
   recipe is the one confirmed on real PC-FX silicon.

   The 16 microprogram words are NOT two alternative programs.  C6272_2 3.6.7.3:
   "Load the Bank A access microprogram into storage locations 0-7, and the Bank
   B access microprogram into storage locations 8-F... the BG plane processing
   section of HuC6272 independently accesses BOTH KRAM banks SIMULTANEOUSLY
   within one dot cycle", and "a Bank A microprogram directs access to Bank A
   KRAM, and a Bank B microprogram directs access to Bank B KRAM."  Slot i is
   cycle i on bank A; slot 8+i is cycle i on bank B, and both run in the same
   cycle.

   For a rotated plane the schedule is fixed.  3.6.7.4)(D): "Because the
   source-image coordinates are recalculated for every dot, EIGHT CG-data
   accesses must be described (all with offset 0)" -- eight, one per dot of the
   8-dot basic cycle -- and Example 35 (3.6.8.4), which the manual calls "the
   only possible microprogram description for internal dot-sequential format",
   is eight BG0/ROT/direct/CG0 in ONE bank with NOP in the other.

   Filling all 16 slots with KING_CODE_ROTATE (what this port used to do)
   therefore describes SIXTEEN CG accesses for one rotated plane, two per dot,
   against both banks at once.  That is not an expensive-but-valid schedule, it
   is a malformed one, and 3.6.7.4)(D) says the display will be incorrect when a
   rotated plane's microprogram conflicts.  pcfxemu never sees it: king.c decodes
   only the 8 slots of the bank its CG base selects and otherwise uses the
   opcodes for a bus-contention model, so a 16-slot fill renders identically
   there and the bug only shows on hardware.

   This port cannot commit to one bank statically either: the three 0x10000-word
   affine sources span words 0..0x2FFFF, so pages 0 and 1 are in bank A and page
   2 is in bank B (D17).  The legal 8-access program is instead reloaded to
   target whichever bank the page being displayed lives in.  That is 18 extra
   register writes on the flips that change bank, inside the vblank window the
   presenter already flips in -- 3.6.6 warns only that "the image is disturbed
   while MPSW is 0", which is exactly what vblank is for. */
#define WAIFU_PCFX_KRAM_BANK_WORDS 0x20000u   /* D17: word 0x20000 = bank B */

static int g_king_mprog_bank = -1;   /* bank the loaded rotation program targets */
static int32_t g_king_reg16_live = -1; /* last value written to REG.16, -1 = unknown */

static void pcfx_king_bg0_rotate_mprog(int bank)
{
    for (int i = 0; i < 16; ++i) g_king_microprog[i] = KING_CODE_NOP;
    for (int i = 0; i < 8; ++i)  g_king_microprog[(bank ? 8 : 0) + i] = KING_CODE_ROTATE;

    /* REG.13/REG.14 may only be written with MPSW clear (3.6.7.3 steps 1-5), and
       3.6.6 warns the image is disturbed while MPSW is 0 -- so take the shared
       reconfiguration window, which parks the download inside blanking. */
    pcfx_king_reconfig_begin();
    king_write_microprogram(g_king_microprog, 0, 16);
    pcfx_king_reconfig_end();
    g_king_mprog_bank = bank;
}

/* The KING keeps affine state in live registers.  Reassert it at every vblank
   AND at every page flip: BIOS/video activity can otherwise leave stale
   coefficients or a stale centre behind, producing the intermittent horizontal
   scaling/offset corruption seen after mode changes on hardware.

   wolf-pcfx establishes (real PC-FXGA silicon, 2026-07-21) that BG0's affine
   coefficients must be valid EVERY time the plane is presented, and that the
   commercial title Miraculum rewrites priority REG.12 (affine-enable) every
   vblank rather than once at boot -- so the rotate-enable priority write belongs
   here too, not just in the one-shot mode setup.  An init-only write is what
   looked correct under pcfxemu while leaving BG0 zoomed/scrambled on hardware.

   The priority/rotate-enable and REG.16 writes do NOT belong here, even though
   wolf-pcfx keeps them in the same function: this refresh is called every field
   from waifu_pcfx_video_wait_vblank() whenever video->mode is KING_8BPP, and
   that mode flag stays set across the 16M title/menu surface (the title path
   flips BG0 to KING_BGMODE_16M without changing it).  Asserting BG0's
   rotate-enable bit from there re-enables the affine fetch underneath a 16M
   plane and blacks the title out.  wolf-pcfx has no 16M mode to collide with.
   They live in pcfx_king_set_8bpp_display_page() below instead, which only ever
   runs on an actual 8bpp page set.  The coefficients themselves are inert
   outside rotate mode, so refreshing them every field stays safe. */
static inline __attribute__((always_inline)) void pcfx_king_refresh_8bpp_affine_state(void)
{
    eris_king_set_bg0_affine_coefficient_a(0x0200); /* 256 display dots -> 512 source texels */
    eris_king_set_bg0_affine_coefficient_b(0);
    eris_king_set_bg0_affine_coefficient_c(0);
    eris_king_set_bg0_affine_coefficient_d(0x0100);
    eris_king_set_bg_affine_center_x(0);
    eris_king_set_bg_affine_center_y(0);
}

/* Point BG0 (and its sub layer) at 8bpp affine page 0..2 and make the plane
   presentable in the same breath: re-aim the rotation microprogram at that
   page's KRAM bank BEFORE the fetch is pointed at it (so no raster is ever
   scheduled against the wrong bank), then reassert priority/affine.  Every 8bpp
   display-page change must go through here -- see wolf_video.c's
   king_set_display_page(), which this mirrors. */
static void pcfx_king_set_8bpp_display_page(int page)
{
    int bank = (page_word_offset(page) & WAIFU_PCFX_KRAM_BANK_WORDS) ? 1 : 0;
    if (bank != g_king_mprog_bank)
        pcfx_king_bg0_rotate_mprog(bank);
    pcfx_king_set_bg0_page_inline(page_bat_offset(page));
    /* Rotate-enable and out-of-area mode belong to the presented 8bpp plane, so
       they are reasserted here rather than in the per-field coefficient refresh
       -- see that function's comment for the 16M-title collision. */
    king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE,
                     KING_BGPRIO_HIDE, 1);
    /* REG.16 is an IMMEDIATE-effect register (C6272_1 (29)), so re-issuing it
       from a mid-display page set disturbs that raster (3.6.6).  Every 8bpp
       present under RAINBOW does exactly that, always with the same value.
       Write it only when it actually changes; REG.12 and the affine block below
       are next-HSYNC registers ((25), (33)) and are safe to reassert. */
    if (g_king_reg16_live != 0x0000) {
        pcfx_king_reg16(0x16, 0x0000); /* transparent-paste outside the main screen */
        g_king_reg16_live = 0x0000;
    }
    pcfx_king_refresh_8bpp_affine_state();
}

static inline __attribute__((always_inline)) void pcfx_schedule_title_page_flip(WaifuPcfxVideo *video, int kram_page, int new_front, int new_back)
{
    video->pending_title_kram_page = kram_page;
    video->pending_title_front_page = new_front;
    video->pending_title_back_page = new_back;
    video->pending_title_page_flip = 1;
    /* A 16M title/menu flip supersedes any deferred 8bpp page flip. */
    video->pending_page = -1;
}

#if 0 /* Retired 256-wide helpers and block-band presenter. */
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

/* Per-scanline 16px-block dirty/black bitmasks (bit b = block b).
 *
 * The V810 has no data cache and charges +3 cyc on every 2 KiB DRAM *page
 * change* (single last-page register, see pcfxemu core.c RAMLPCHECK).  `cur`
 * (the CPU framebuffer) and `old` (the page shadow) are two separate ~60 KB
 * arrays in different pages, so the old block test -- `a[0]!=b[0] | a[1]!=b[1]
 * | ...` -- ping-ponged cur<->old on nearly every word and made this diff
 * ~22% of the whole field.  Here we read a contiguous run of 8 `cur` words,
 * THEN the matching 8 `old` words: consecutive accesses stay on one page, so a
 * chunk costs ~2 page changes instead of ~16.  Bit-for-bit identical result. */
static inline __attribute__((always_inline)) unsigned pcfx_row_block_masks(const uint8_t *cur, const uint8_t *old, unsigned *black_mask_out)
{
    const uint32_t *c = (const uint32_t *)cur;
    const uint32_t *o = (const uint32_t *)old;
    unsigned dirty = 0, black = 0;
    for (int b = 0; b < WAIFU_PCFX_DIRTY_BLOCKS_X; b += 2) {
        uint32_t c0 = c[0], c1 = c[1], c2 = c[2], c3 = c[3];
        uint32_t c4 = c[4], c5 = c[5], c6 = c[6], c7 = c[7];
        /* Compiler barrier: force all eight `cur` loads to be emitted before the
           eight `old` loads.  Without it the V810 scheduler re-interleaves them
           (cur,old,cur,old,...), which reintroduces the 2 KiB DRAM page
           ping-pong this batching exists to avoid.  With the barrier each chunk
           is one contiguous cur run then one contiguous old run: ~2 page
           changes instead of ~16. */
        __asm__ volatile("" ::: "memory");
        uint32_t o0 = o[0], o1 = o[1], o2 = o[2], o3 = o[3];
        uint32_t o4 = o[4], o5 = o[5], o6 = o[6], o7 = o[7];
        if ((c0 ^ o0) | (c1 ^ o1) | (c2 ^ o2) | (c3 ^ o3)) {
            dirty |= 1u << b;
            if ((c0 & c1 & c2 & c3) == 0xffffffffu) black |= 1u << b;
        }
        if ((c4 ^ o4) | (c5 ^ o5) | (c6 ^ o6) | (c7 ^ o7)) {
            dirty |= 1u << (b + 1);
            if ((c4 & c5 & c6 & c7) == 0xffffffffu) black |= 1u << (b + 1);
        }
        c += 8;
        o += 8;
    }
    *black_mask_out = black;
    return dirty;
}

static inline __attribute__((always_inline)) int pcfx_dirty_collect_row_runs(const uint8_t *cur, const uint8_t *old, PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS])
{
    unsigned black_mask;
    unsigned dirty = pcfx_row_block_masks(cur, old, &black_mask);
    int count = 0;
    int b = 0;
    while (b < WAIFU_PCFX_DIRTY_BLOCKS_X) {
        if (!((dirty >> b) & 1u)) { ++b; continue; }
        /* Affine-source uploads must include changed black texels too. */
        int black = 0;
        int start = b++;
        while (b < WAIFU_PCFX_DIRTY_BLOCKS_X &&
               ((dirty >> b) & 1u)) {
            ++b;
        }
        runs[count].x0b = (uint8_t)start;
        runs[count].x1b = (uint8_t)(b - 1);
        runs[count].black = (uint8_t)black;
        ++count;
    }
    return count;
}

/* Per-row dirty runs produced by the plan pass and consumed by the band-present
   pass, so the framebuffer/shadow diff is scanned ONCE per frame instead of
   twice (the plan pass computed identical runs and threw them away, then
   pcfx_present_dirty_bands re-read the whole 256x240 frame + shadow to rebuild
   them -- the single biggest data-side 2 KiB DRAM page source).  A dirty-band
   present only happens when the plan pass scanned every row without crossing a
   full-upload threshold, so at most WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS runs (plus
   one row's worth for the row that trips the threshold) are ever stored. */
#define WAIFU_PCFX_DIRTY_PLAN_RUN_CAP (WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS + WAIFU_PCFX_DIRTY_MAX_ROW_RUNS)
static PcfxDirtyRun g_plan_runs[WAIFU_PCFX_DIRTY_PLAN_RUN_CAP];
static uint8_t g_plan_row_run_count[WAIFU_PCFX_H];
/* Number of leading rows whose runs in g_plan_runs are valid for reuse.  Set to
   WAIFU_PCFX_H only when the plan pass scanned all rows (i.e. a dirty-band
   present will follow); a lower value makes the present pass fall back to
   re-scanning those rows (it never does in practice, but keeps it correct). */
static int g_plan_rows_valid = 0;

static WAIFU_PCFX_NOINLINE PcfxDirtyPlanStats pcfx_dirty_plan_stats(const uint8_t *cur, const uint8_t *old)
{
    PcfxDirtyPlanStats stats;
    stats.dirty_blocks = 0;
    stats.row_runs = 0;
    int total = 0;
    int y = 0;
    g_plan_rows_valid = 0;
    for (; y < WAIFU_PCFX_H; ++y) {
        const uint8_t *a = cur + y * WAIFU_PCFX_W;
        const uint8_t *b = old + y * WAIFU_PCFX_W;
        int rc = pcfx_dirty_collect_row_runs(a, b, &g_plan_runs[total]);
        g_plan_row_run_count[y] = (uint8_t)rc;
        for (int i = 0; i < rc; ++i) {
            stats.dirty_blocks += (int)g_plan_runs[total + i].x1b - (int)g_plan_runs[total + i].x0b + 1;
        }
        total += rc;
        stats.row_runs += rc;
        /* The only use of these totals is the caller's full-upload test below.
           Once either threshold is provably crossed the exact counts no longer
           matter, so stop scanning -- a heavily-changed (animation) frame need
           not diff all 240 rows just to conclude "upload the whole frame".
           Same decision, far less work on the frames that were most expensive.
           On that early exit the stored runs are incomplete, but the caller
           does a full upload (never a band present), so they go unused. */
        if (stats.dirty_blocks * WAIFU_PCFX_DIRTY_BLOCK_W >= WAIFU_PCFX_DIRTY_FULL_THRESHOLD_BYTES ||
            stats.row_runs > WAIFU_PCFX_DIRTY_MAX_TOTAL_RUNS) {
            return stats;
        }
    }
    /* Full frame scanned without tripping a threshold -> the band present will
       run and can reuse every row's runs. */
    g_plan_rows_valid = WAIFU_PCFX_H;
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

/* 512-wide affine-source upload helpers.  They deliberately write every dirty
   texel, including IDX_BLACK: a black-special-case clear is a masked write in
   this mode and can leave a stale neighbouring source texel visible through the
   affine fetch. */
static void pcfx_kram_upload_rect_affine(const uint8_t *src, int page_word_offset,
                                         int x0, int y0, int width, int rows,
                                         int src_pitch);
static void pcfx_kram_write_frame_affine(const uint8_t *framebuffer,
                                         int page_word_offset);

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

#if 0 /* Retired 256-wide helpers and block-band presenter. */
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

#if WAIFU_PCFX_DIRECT_BIG_ART_ENABLE
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
        pcfx_kram_upload_rect_affine(arts[i].src, page_word_offset_value,
                                     arts[i].x, arts[i].y,
                                     arts[i].width, WAIFU_BIG_H, arts[i].pitch);
    }
}
#endif


static inline __attribute__((always_inline)) void pcfx_flush_dirty_band(uint8_t *shadow, const uint8_t *framebuffer,
                                  int page_word_offset_value, const PcfxDirtyBand *band)
{
    if (!band->used) return;
    int x0 = (int)band->x0b * WAIFU_PCFX_DIRTY_BLOCK_W;
    int width = ((int)band->x1b - (int)band->x0b + 1) * WAIFU_PCFX_DIRTY_BLOCK_W;
    int y0 = (int)band->y0;
    int rows = (int)band->y1 - (int)band->y0 + 1;
    pcfx_kram_upload_rect_affine(framebuffer + y0 * WAIFU_PCFX_W + x0,
                                 page_word_offset_value, x0, y0, width, rows,
                                 WAIFU_PCFX_W);
    pcfx_shadow_copy_rect(shadow, framebuffer, x0, y0, width, rows);
}

static WAIFU_PCFX_NOINLINE void pcfx_present_dirty_bands(uint8_t *shadow, const uint8_t *framebuffer, int page_word_offset_value)
{
    PcfxDirtyBand active[WAIFU_PCFX_DIRTY_MAX_BANDS];
    PcfxDirtyRun runs[WAIFU_PCFX_DIRTY_MAX_ROW_RUNS];
    int active_count = 0;
    int plan_base = 0;
    for (int i = 0; i < WAIFU_PCFX_DIRTY_MAX_BANDS; ++i) active[i].used = 0;

    for (int y = 0; y < WAIFU_PCFX_H; ++y) {
        for (int i = 0; i < active_count; ++i) active[i].matched = 0;
        int run_count;
        const PcfxDirtyRun *rowruns;
        if (y < g_plan_rows_valid) {
            /* Reuse the runs the plan pass already computed for this row (same
               framebuffer-vs-shadow diff -- the shadow for row y is not touched
               until its band flushes, which is after this row is processed), so
               we do not re-read the whole frame here. */
            run_count = g_plan_row_run_count[y];
            rowruns = &g_plan_runs[plan_base];
            plan_base += run_count;
        } else {
            run_count = pcfx_dirty_collect_row_runs(framebuffer + y * WAIFU_PCFX_W,
                                                    shadow + y * WAIFU_PCFX_W,
                                                    runs);
            rowruns = runs;
        }
        for (int r = 0; r < run_count; ++r) {
            int found = -1;
            for (int i = 0; i < active_count; ++i) {
                if (active[i].used && !active[i].matched &&
                    active[i].x0b == rowruns[r].x0b &&
                    active[i].x1b == rowruns[r].x1b &&
                    active[i].black == rowruns[r].black) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                active[found].y1 = (uint8_t)y;
                active[found].matched = 1;
            } else if (active_count < WAIFU_PCFX_DIRTY_MAX_BANDS) {
                PcfxDirtyBand *b = &active[active_count++];
                b->x0b = rowruns[r].x0b;
                b->x1b = rowruns[r].x1b;
                b->black = rowruns[r].black;
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
                one.x0b = rowruns[r].x0b;
                one.x1b = rowruns[r].x1b;
                one.black = rowruns[r].black;
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
    /* king_set_kram_write() uses out.w.  For physical KRAM page 1 we
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
    king_set_kram_write(word_addr, 1);
#endif
}

static inline __attribute__((always_inline)) void pcfx_kram_write_affine_texel(uint8_t texel)
{
    uint16_t word = (uint16_t)texel | ((uint16_t)texel << 8);
#if defined(__v810__)
    __asm__ volatile ("out.h %0,0x604[r0]" : : "r" (word) : "memory");
#else
    king_kram_write(word);
#endif
}

static inline __attribute__((always_inline)) void pcfx_kram_select_data_register(void)
{
#if defined(__v810__)
    uint32_t reg;
    __asm__ volatile (
        "movea 14,r0,%[reg]\n"
        "out.h %[reg],0x600[r0]\n"
        : [reg] "=&r" (reg)
        :
        : "memory");
#endif
}

#if WAIFU_PCFX_DIRTY_PRESENT && defined(__v810__)
/* Upload complete four-word runs after king_seek_write_words() selected their
   destination.  The CPU framebuffer and page shadow live in different 2 KiB
   DRAM pages.  The scalar loop below used to alternate one framebuffer load
   with one shadow store for every four pixels, paying two page changes per
   word.  This loop loads four words first, streams their sixteen duplicated
   texels to KING, then stores all four shadow words: two page changes per
   sixteen pixels, with exactly the same KRAM and shadow contents.

   Keep this inline.  Besides avoiding an ABI call in every changed row, the
   compact four-byte subloops keep the uploader's hot body within the V810's
   1 KiB direct-mapped instruction cache. */
static inline __attribute__((always_inline)) int pcfx_kram_upload_shadow_word_groups(
    const uint32_t *src, uint32_t *dst, int words)
{
    int groups = words >> 2;
    int done = groups << 2;
    if (groups > 0) {
        __asm__ volatile (
            "1:\n"
            "ld.w 0[%[src]],r10\n"
            "ld.w 4[%[src]],r11\n"
            "ld.w 8[%[src]],r12\n"
            "ld.w 12[%[src]],r13\n"

            "mov r10,r14\n"
            "movea 4,r0,r16\n"
            "2:\n"
            "andi 255,r14,r15\n"
            "mov r15,r17\n"
            "shl 8,r17\n"
            "or r17,r15\n"
            "out.h r15,0x604[r0]\n"
            "shr 8,r14\n"
            "add -1,r16\n"
            "bne 2b\n"

            "mov r11,r14\n"
            "movea 4,r0,r16\n"
            "3:\n"
            "andi 255,r14,r15\n"
            "mov r15,r17\n"
            "shl 8,r17\n"
            "or r17,r15\n"
            "out.h r15,0x604[r0]\n"
            "shr 8,r14\n"
            "add -1,r16\n"
            "bne 3b\n"

            "mov r12,r14\n"
            "movea 4,r0,r16\n"
            "4:\n"
            "andi 255,r14,r15\n"
            "mov r15,r17\n"
            "shl 8,r17\n"
            "or r17,r15\n"
            "out.h r15,0x604[r0]\n"
            "shr 8,r14\n"
            "add -1,r16\n"
            "bne 4b\n"

            "mov r13,r14\n"
            "movea 4,r0,r16\n"
            "5:\n"
            "andi 255,r14,r15\n"
            "mov r15,r17\n"
            "shl 8,r17\n"
            "or r17,r15\n"
            "out.h r15,0x604[r0]\n"
            "shr 8,r14\n"
            "add -1,r16\n"
            "bne 5b\n"

            "st.w r10,0[%[dst]]\n"
            "st.w r11,4[%[dst]]\n"
            "st.w r12,8[%[dst]]\n"
            "st.w r13,12[%[dst]]\n"
            "addi 16,%[src],%[src]\n"
            "addi 16,%[dst],%[dst]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [src] "+r" (src), [dst] "+r" (dst), [groups] "+r" (groups)
            :
            : "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17",
              "memory");
    }
    return done;
}
#endif

/* A 512-wide affine source maps each logical pixel to one KRAM word.  Do not
   coalesce or mask writes here: the current source word must match the CPU
   framebuffer exactly before its page is made visible. */
static void pcfx_kram_upload_rect_affine(const uint8_t *src, int page_word_offset,
                                         int x0, int y0, int width, int rows,
                                         int src_pitch)
{
    for (int row = 0; row < rows; ++row) {
        const uint8_t *p = src + row * src_pitch;
        king_seek_write_words((uint32_t)(page_word_offset +
                              (y0 + row) * WAIFU_PCFX_BG_ROW_WORDS + x0));
        /* king_seek_write_words leaves KING's register selector on KRAMWA.
           Select KRAMWD before emitting the unmasked texel halfwords. */
        pcfx_kram_select_data_register();
        for (int x = 0; x < width; ++x) pcfx_kram_write_affine_texel(p[x]);
    }
}

static void pcfx_kram_write_frame_affine(const uint8_t *framebuffer,
                                         int page_word_offset)
{
    pcfx_kram_upload_rect_affine(framebuffer, page_word_offset, 0, 0,
                                 WAIFU_PCFX_W, WAIFU_PCFX_H,
                                 WAIFU_PCFX_W);
}

#if WAIFU_PCFX_DIRTY_PRESENT
/* Return how many leading 32-byte (8-word) groups of `src` and `dst` are
   identical, so the dirty scan can hunt for the next changed word in strides
   instead of one word at a time.

   Why this is asm: `src` (the CPU framebuffer) and `dst` (the page shadow) are
   two ~60 KB arrays in different 2 KiB DRAM pages, and the V810 has no data
   cache and one page register (+3 cyc per page *change*).  The natural
   `src[i] == dst[i]` walk therefore pays a page change on BOTH loads of every
   word -- it was ~21 cyc per word and the single biggest cost of a present.
   Loading all eight source words THEN all eight shadow words costs two page
   changes per group instead of sixteen.  Written in C, gcc happily
   re-interleaves the two load runs and the win disappears (verified in the
   disassembly), which is the same reason pcfx_copy_bytes_inline is asm. */
#if defined(__v810__)
static inline int pcfx_dirty_skip_equal_groups(const uint32_t *src, const uint32_t *dst, int groups)
{
    int skipped;
    __asm__ volatile (
        "mov 0,%[skipped]\n"
        "1:\n"
        "ld.w 0[%[src]],r10\n"
        "ld.w 4[%[src]],r11\n"
        "ld.w 8[%[src]],r12\n"
        "ld.w 12[%[src]],r13\n"
        "ld.w 16[%[src]],r14\n"
        "ld.w 20[%[src]],r15\n"
        "ld.w 24[%[src]],r16\n"
        "ld.w 28[%[src]],r17\n"
        "ld.w 0[%[dst]],r18\n"
        "xor r18,r10\n"
        "ld.w 4[%[dst]],r18\n"
        "xor r18,r11\n"
        "ld.w 8[%[dst]],r18\n"
        "xor r18,r12\n"
        "ld.w 12[%[dst]],r18\n"
        "xor r18,r13\n"
        "ld.w 16[%[dst]],r18\n"
        "xor r18,r14\n"
        "ld.w 20[%[dst]],r18\n"
        "xor r18,r15\n"
        "ld.w 24[%[dst]],r18\n"
        "xor r18,r16\n"
        "ld.w 28[%[dst]],r18\n"
        "xor r18,r17\n"
        "or r11,r10\n"
        "or r13,r12\n"
        "or r15,r14\n"
        "or r17,r16\n"
        "or r12,r10\n"
        "or r16,r14\n"
        "or r14,r10\n"
        "bne 2f\n"
        "addi 32,%[src],%[src]\n"
        "addi 32,%[dst],%[dst]\n"
        "add 1,%[skipped]\n"
        "cmp %[groups],%[skipped]\n"
        "blt 1b\n"
        "2:\n"
        : [skipped] "=&r" (skipped), [src] "+r" (src), [dst] "+r" (dst)
        : [groups] "r" (groups)
        : "r10", "r11", "r12", "r13", "r14", "r15", "r16", "r17", "r18");
    return skipped;
}
#else
static inline int pcfx_dirty_skip_equal_groups(const uint32_t *src, const uint32_t *dst, int groups)
{
    int g;
    for (g = 0; g < groups; ++g) {
        int i;
        for (i = 0; i < 8; ++i) {
            if (src[g * 8 + i] != dst[g * 8 + i]) return g;
        }
    }
    return groups;
}
#endif

/* Maka's 512x256 path uses a compact logical shadow and writes each changed
   source byte as a complete KRAM word.  Keep the same invariant here: the
   shadow is updated only after the corresponding unmasked texel has been sent,
   so every triple-buffer page exactly describes what its affine source holds.
   Consecutive changed four-byte groups share one KRAM seek; unchanged groups
   close a run rather than being bridged with stale/masked data. */
static WAIFU_PCFX_NOINLINE int pcfx_present_dirty_rows(uint8_t *shadow,
                                                        const uint8_t *framebuffer,
                                                        int page_word_offset)
{
    int changed_words = 0;
    for (int y = 0; y < WAIFU_PCFX_H; ++y) {
        const uint32_t *src = (const uint32_t *)(framebuffer + y * WAIFU_PCFX_W);
        uint32_t *dst = (uint32_t *)(shadow + y * WAIFU_PCFX_W);
        int word = 0;
        while (word < WAIFU_PCFX_W / 4) {
            /* Skip unchanged 32-byte groups first (see
               pcfx_dirty_skip_equal_groups), then fall back to the exact
               word-at-a-time walk inside the group that changed.  Emission
               below is untouched, so the KRAM writes and shadow updates stay
               byte-for-byte identical to the plain scan. */
            {
                int groups = (WAIFU_PCFX_W / 4 - word) >> 3;
                if (groups > 0) {
                    word += 8 * pcfx_dirty_skip_equal_groups(src + word, dst + word, groups);
                }
            }
            while (word < WAIFU_PCFX_W / 4 && src[word] == dst[word]) ++word;
            if (word == WAIFU_PCFX_W / 4) break;

            int first = word;
            do {
                ++word;
            } while (word < WAIFU_PCFX_W / 4 && src[word] != dst[word]);

            king_seek_write_words((uint32_t)(page_word_offset +
                                  y * WAIFU_PCFX_BG_ROW_WORDS + first * 4));
            pcfx_kram_select_data_register();
            int i = first;
#if defined(__v810__)
            {
                int grouped = pcfx_kram_upload_shadow_word_groups(src + i, dst + i,
                                                                  word - i);
                i += grouped;
                changed_words += grouped;
            }
#endif
            for (; i < word; ++i) {
                uint32_t texels = src[i];
                pcfx_kram_write_affine_texel((uint8_t)texels);
                pcfx_kram_write_affine_texel((uint8_t)(texels >> 8));
                pcfx_kram_write_affine_texel((uint8_t)(texels >> 16));
                pcfx_kram_write_affine_texel((uint8_t)(texels >> 24));
                dst[i] = texels;
                ++changed_words;
            }
        }
    }
    return changed_words;
}
#endif



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
#define WAIFU_PCFX_VDC_VISIBLE_W 32
#define WAIFU_PCFX_VDC_PALETTE_BASE 256
#define WAIFU_PCFX_VDC_PAL_BLACK 0x01
#define WAIFU_PCFX_VDC_PAL_WHITE 0x02
#define WAIFU_PCFX_VDC_PAL_GOLD  0x03
#define WAIFU_PCFX_VDC_PAL_RED   0x04
#define WAIFU_PCFX_VDC_PAL_PANEL 0x05
#define WAIFU_PCFX_VDC_PAL_EDGE  0x06
#define WAIFU_PCFX_VDC_SANCTUM_TILE_PANEL 0x111
#define WAIFU_PCFX_VDC_SANCTUM_TILE_EDGE  0x112
/* Fade tile lives immediately after the 64x32 BAT so tile 0 remains unusable
   for transparent blanks.  The BAT is filled once, then fade steps update only
   this tile's pattern so title/menu fades do not spend frames rewriting both
   VDC maps. */
#define WAIFU_PCFX_VDC_FADE_TILE_BASE 0x080
#define WAIFU_PCFX_VDC_FADE_LEVELS 17

/* Story portraits use the same paired-HuC6270 256-colour sprite contract as
 * doom-pcfx: VDC0 carries the high nibble, VDC1 carries the low nibble and its
 * palette-bank bit 3 arms the combine.  A 128x128 portrait occupies an 8x2
 * grid of 16x64 cells; two active portraits exactly consume the 32-entry SAT.
 * Pattern storage is deliberately above the 64x32 BAT/font area and below the
 * SAT source at 0xff00. */
#define WAIFU_PCFX_STORY_SPR_MAX       2
#define WAIFU_PCFX_STORY_CELL_W        16
#define WAIFU_PCFX_STORY_CELL_H        64
#define WAIFU_PCFX_STORY_COLS          8
#define WAIFU_PCFX_STORY_ROWS          2
#define WAIFU_PCFX_STORY_CELLS         (WAIFU_PCFX_STORY_COLS * WAIFU_PCFX_STORY_ROWS)
#define WAIFU_PCFX_STORY_NO_STRIDE     8
#define WAIFU_PCFX_STORY_SLOT0_NO      0x080
#define WAIFU_PCFX_STORY_SLOT1_NO      (WAIFU_PCFX_STORY_SLOT0_NO + WAIFU_PCFX_STORY_CELLS * WAIFU_PCFX_STORY_NO_STRIDE)
#define WAIFU_PCFX_STORY_SAT_ADDR      0xff00
#define WAIFU_PCFX_STORY_SAT_H64       0x2000

typedef struct WaifuPcfxStoryPortraitRequest {
    int id;
    int x;
    int y;
} WaifuPcfxStoryPortraitRequest;

static WaifuPcfxStoryPortraitRequest g_story_portrait_request[WAIFU_PCFX_STORY_SPR_MAX];
static int g_story_portrait_count;
static int g_story_portrait_uploaded[WAIFU_PCFX_STORY_SPR_MAX] = { -1, -1 };
static int g_story_portrait_uploaded_y[WAIFU_PCFX_STORY_SPR_MAX] = { -0x4000, -0x4000 };
static int g_story_sat_visible;
static int g_story_sat_dma_armed;

static WaifuPcfxVdcBackground g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;
static int g_rainbow_backdrop_requested;
static int g_rainbow_backdrop_active;
static int g_rainbow_transfer_armed;
static int g_rainbow_hscroll;
static int g_rainbow_hscroll_requested;
static WaifuPcfxSanctumBackdrop g_rainbow_backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_DESERT;
static int g_sanctum_requested;
static int g_sanctum_active;
static int g_sanctum_loaded_backdrop = -1;
static WaifuPcfxSanctumBackdrop g_sanctum_backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_DESERT;
static WaifuPcfxSanctumOverlay g_sanctum_overlay = WAIFU_PCFX_SANCTUM_OVERLAY_MENU;
static int g_sanctum_value;
static int g_sanctum_blink_visible = 1;

typedef enum WaifuPcfxOverlayMode {
    WAIFU_PCFX_OVERLAY_OFF = 0,
    WAIFU_PCFX_OVERLAY_TITLE_PROMPT = 1,
    WAIFU_PCFX_OVERLAY_MENU = 2,
    WAIFU_PCFX_OVERLAY_LOAD_DEVICE = 3,
    WAIFU_PCFX_OVERLAY_ENDING_STORY = 4,
    WAIFU_PCFX_OVERLAY_ENDING_CREDITS = 5
} WaifuPcfxOverlayMode;

static WaifuPcfxOverlayMode g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF;
static WaifuPcfxOverlayMode g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
static int g_vdc_overlay_prompt_visible = 0;
static int g_vdc_overlay_has_save = 0;
static int g_vdc_overlay_ending_page = 0;
static int g_vdc_overlay_ending_prompt_visible = 1;
static int g_vdc_overlay_ending_visible_chars = 255;
static char g_vdc_overlay_ending_name[8] = "SERENA";
static int g_vdc_overlay_ending_name_len = 6;
static int g_vdc_overlay_menu_selected = 1;
static int g_vdc_overlay_load_selected = 0;
static int g_vdc_overlay_load_internal_has = 0;
static int g_vdc_overlay_load_external_has = 0;
/* 0 = no black overlay, 16 = fully black.  This is deliberately platform-local:
   the core only exposes visible_q8, and the PC-FX presenter decides how to hide
   its 16M KING surface without touching KRAM. */
static int g_vdc_overlay_fade_level = 16;
static int g_vdc_overlay_applied_fade_level = -1;
static int g_vdc_overlay_fade_map_active = 0;
static int g_vdc_overlay_dirty = 1;
static void pcfx_vdc_overlay_init(WaifuPcfxVideo *video);
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

static void pcfx_vdc_overlay_upload_fade_tile(int level)
{
    if (level < 0) level = 0;
    if (level > 16) level = 16;
    vdc_set_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_FADE_TILE_BASE * 16);
    for (int row = 0; row < 16; ++row) vdc_vram_write(VDC_CHIP_0, 0x0000);

    vdc_set_vram_write(VDC_CHIP_1, WAIFU_PCFX_VDC_FADE_TILE_BASE * 16);
    for (int row = 0; row < 8; ++row) {
        vdc_vram_write(VDC_CHIP_1, pcfx_vdc_fade_pattern_row(level, row));
    }
    for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_1, 0x0000);
}

static void pcfx_vdc_overlay_fill_fade_map(void)
{
    uint16_t tile = (uint16_t)WAIFU_PCFX_VDC_FADE_TILE_BASE;
    for (int row = 0; row < WAIFU_PCFX_VDC_MAP_H; ++row) {
        int addr = row * WAIFU_PCFX_VDC_MAP_W;
        vdc_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) vdc_vram_write(VDC_CHIP_0, tile);
        vdc_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) vdc_vram_write(VDC_CHIP_1, (uint16_t)(tile | 0x8000));
    }
    g_vdc_overlay_fade_map_active = 1;
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
    vdc_set_vram_write(VDC_CHIP_0, 0);
    for (int j = 0; j < 16; ++j) vdc_vram_write(VDC_CHIP_0, 0x0000);
    vdc_set_vram_write(VDC_CHIP_1, 0);
    for (int j = 0; j < 16; ++j) vdc_vram_write(VDC_CHIP_1, 0x0000);

    vdc_set_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_FONT_TILE_BASE * 16);
    for (int i = WAIFU_PCFX_VDC_FONT_FIRST; i <= WAIFU_PCFX_VDC_FONT_LAST; ++i) {
        for (int j = 0; j < 16; ++j) vdc_vram_write(VDC_CHIP_0, 0x0000);
    }

    vdc_set_vram_write(VDC_CHIP_1, WAIFU_PCFX_VDC_FONT_TILE_BASE * 16);
    for (int ch = WAIFU_PCFX_VDC_FONT_FIRST; ch <= WAIFU_PCFX_VDC_FONT_LAST; ++ch) {
        for (int row = 0; row < 8; ++row) {
            uint8_t fg = pcfx_font_row((unsigned char)ch, row);
            uint8_t outline = pcfx_outline_row((unsigned char)ch, row);
            vdc_vram_write(VDC_CHIP_1, (uint16_t)(((uint16_t)fg << 8) | outline));
        }
        for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_1, 0x0000);
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
        vdc_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < w; ++col) vdc_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_BLANK_TILE);
        vdc_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < w; ++col) vdc_vram_write(VDC_CHIP_1, (uint16_t)(WAIFU_PCFX_VDC_BLANK_TILE | 0x8000));
    }
}

static void pcfx_vdc_overlay_clear_all(void)
{
    pcfx_vdc_overlay_clear_rect(0, 0, WAIFU_PCFX_VDC_MAP_W, WAIFU_PCFX_VDC_MAP_H);
}

static void pcfx_vdc_select_overlay_palette(void)
{
    tetsu_set_vdc_palette(WAIFU_PCFX_VDC_PALETTE_BASE, WAIFU_PCFX_VDC_PALETTE_BASE);
}

static uint16_t pcfx_vdc_palette_entry(uint16_t index)
{
    return (uint16_t)(WAIFU_PCFX_VDC_PALETTE_BASE + index);
}

static void pcfx_vdc_restore_overlay_palette(void)
{
    pcfx_vdc_select_overlay_palette();
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_BLACK), 0x0088);
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_WHITE), 0xE088);
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_GOLD),  0xB468);
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_RED),   0x5F0F);
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_PANEL), rgb888_to_pcfx_yuv(12, 18, 28));
    pcfx_vce_set_palette(pcfx_vdc_palette_entry(WAIFU_PCFX_VDC_PAL_EDGE),  rgb888_to_pcfx_yuv(210, 172, 90));
}

static void pcfx_vdc_sanctum_upload_solid_tile(uint16_t tile, uint16_t vdc0_row, uint16_t vdc1_row)
{
    vdc_set_vram_write(VDC_CHIP_0, tile * 16);
    for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_0, vdc0_row);
    for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_0, 0x0000);

    vdc_set_vram_write(VDC_CHIP_1, tile * 16);
    for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_1, vdc1_row);
    for (int row = 0; row < 8; ++row) vdc_vram_write(VDC_CHIP_1, 0x0000);
}

static void pcfx_vdc_sanctum_upload_tiles(void)
{
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_PANEL, 0xffff, 0x0000);
    pcfx_vdc_sanctum_upload_solid_tile(WAIFU_PCFX_VDC_SANCTUM_TILE_EDGE, 0xffff, 0xffff);
}

static uint32_t pcfx_rainbow_bytes_for_backdrop(WaifuPcfxSanctumBackdrop backdrop)
{
    switch (backdrop) {
    case WAIFU_PCFX_SANCTUM_BACKDROP_STONE: return WAIFU_PCFX_RAINBOW_STONE_BYTES;
    case WAIFU_PCFX_SANCTUM_BACKDROP_EMBER: return WAIFU_PCFX_RAINBOW_EMBER_BYTES;
    case WAIFU_PCFX_SANCTUM_BACKDROP_SKY: return WAIFU_PCFX_RAINBOW_SKY_BYTES;
    default: return WAIFU_PCFX_RAINBOW_DESERT_BYTES;
    }
}

static WaifuPcfxRainbowBgAsset pcfx_rainbow_asset_for_backdrop(WaifuPcfxSanctumBackdrop backdrop)
{
    switch (backdrop) {
    case WAIFU_PCFX_SANCTUM_BACKDROP_STONE: return WAIFU_PCFX_RAINBOW_BG_STONE;
    case WAIFU_PCFX_SANCTUM_BACKDROP_EMBER: return WAIFU_PCFX_RAINBOW_BG_EMBER;
    case WAIFU_PCFX_SANCTUM_BACKDROP_SKY: return WAIFU_PCFX_RAINBOW_BG_SKY;
    default: return WAIFU_PCFX_RAINBOW_BG_DESERT;
    }
}

static void pcfx_vdc_sanctum_clear_all(void)
{
    for (int row = 0; row < WAIFU_PCFX_VDC_MAP_H; ++row) {
        vdc_set_vram_write(VDC_CHIP_0, row * WAIFU_PCFX_VDC_MAP_W);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) vdc_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_BLANK_TILE);
        vdc_set_vram_write(VDC_CHIP_1, row * WAIFU_PCFX_VDC_MAP_W);
        for (int col = 0; col < WAIFU_PCFX_VDC_MAP_W; ++col) vdc_vram_write(VDC_CHIP_1, (uint16_t)(WAIFU_PCFX_VDC_BLANK_TILE | 0x8000));
    }
}

static void pcfx_vdc_sanctum_panel(int tx, int ty, int w, int h)
{
    if (tx < 0) { w += tx; tx = 0; }
    if (ty < 0) { h += ty; ty = 0; }
    if (tx + w > WAIFU_PCFX_VDC_MAP_W) w = WAIFU_PCFX_VDC_MAP_W - tx;
    if (ty + h > WAIFU_PCFX_VDC_MAP_H) h = WAIFU_PCFX_VDC_MAP_H - ty;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; ++row) {
        int addr = (ty + row) * WAIFU_PCFX_VDC_MAP_W + tx;
        int edge_row = (row == 0 || row == h - 1);
        vdc_set_vram_write(VDC_CHIP_0, addr);
        for (int col = 0; col < w; ++col) {
            int edge_col = (col == 0 || col == w - 1);
            vdc_vram_write(VDC_CHIP_0, (edge_row || edge_col) ? WAIFU_PCFX_VDC_SANCTUM_TILE_EDGE : WAIFU_PCFX_VDC_SANCTUM_TILE_PANEL);
        }
        vdc_set_vram_write(VDC_CHIP_1, addr);
        for (int col = 0; col < w; ++col) vdc_vram_write(VDC_CHIP_1, (uint16_t)(WAIFU_PCFX_VDC_BLANK_TILE | 0x8000));
    }
}

static void pcfx_vdc_sanctum_print(int tx, int ty, const char *str, int max_len)
{
    int len;
    if (!str || max_len <= 0) return;
    if (tx < 0 || ty < 0 || tx >= WAIFU_PCFX_VDC_MAP_W || ty >= WAIFU_PCFX_VDC_MAP_H) return;
    if (max_len > WAIFU_PCFX_VDC_MAP_W - tx) max_len = WAIFU_PCFX_VDC_MAP_W - tx;
    len = pcfx_strlen_limited(str, max_len);

    vdc_set_vram_write(VDC_CHIP_0, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) vdc_vram_write(VDC_CHIP_0, WAIFU_PCFX_VDC_SANCTUM_TILE_PANEL);
    vdc_set_vram_write(VDC_CHIP_1, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) {
        unsigned char ch = (i < len) ? (unsigned char)str[i] : (unsigned char)' ';
        uint16_t tile = WAIFU_PCFX_VDC_BLANK_TILE;
        if (ch >= WAIFU_PCFX_VDC_FONT_FIRST && ch <= WAIFU_PCFX_VDC_FONT_LAST) {
            tile = (uint16_t)(WAIFU_PCFX_VDC_FONT_TILE_BASE + (ch - WAIFU_PCFX_VDC_FONT_FIRST));
        }
        vdc_vram_write(VDC_CHIP_1, (uint16_t)(tile | 0x8000));
    }
}

static void pcfx_rainbow_write_reg16(uint16_t reg, uint16_t value)
{
#if defined(__v810__)
    __asm__ volatile (
        "out.h %[reg],0x600[r0]\n"
        "out.h %[value],0x604[r0]\n"
        :
        : [reg] "r" (reg), [value] "r" (value)
        : "memory");
#else
    (void)reg;
    (void)value;
#endif
}

/* REG.41 is the RAINBOW's 32-bit KRAM SOURCE ADDRESS; it must be written with a
   full out.w, not an out.h.  A halfword write leaves the upper half holding
   whatever the last 32-bit KING access put there, so the decoder can be pointed
   at an arbitrary KRAM address -- it then decodes garbage or nothing at all.
   doom-pcfx uses king_reg32() here for exactly this reason. */
static void pcfx_rainbow_write_reg32(uint16_t reg, uint32_t value)
{
#if defined(__v810__)
    __asm__ volatile (
        "out.h %[reg],0x600[r0]\n"
        "out.w %[value],0x604[r0]\n"
        :
        : [reg] "r" ((uint32_t)reg), [value] "r" (value)
        : "memory");
#else
    (void)reg;
    (void)value;
#endif
}

static void pcfx_rainbow_setup(void)
{
#if defined(__v810__)
    uint16_t zero = 0;
    uint16_t control = 3;
    __asm__ volatile (
        "out.b %[zero],0x200[r0]\n"
        "out.b %[zero],0x202[r0]\n"
        "movea -128,r0,r10\n"
        "out.h r10,0x208[r0]\n"
        "out.h %[zero],0x20c[r0]\n"
        "out.h %[zero],0x210[r0]\n"
        "out.h %[zero],0x214[r0]\n"
        "out.h %[control],0x204[r0]\n"
        :
        : [zero] "r" (zero), [control] "r" (control)
        : "r10", "memory");
#endif
}

static void pcfx_rainbow_set_hscroll(int hscroll)
{
#if defined(__v810__)
    uint16_t lo = (uint16_t)(hscroll & 0xff);
    uint16_t hi = (uint16_t)((hscroll >> 8) & 0x01);
    __asm__ volatile (
        "out.b %[lo],0x200[r0]\n"
        "out.b %[hi],0x202[r0]\n"
        :
        : [lo] "r" (lo), [hi] "r" (hi)
        : "memory");
#else
    (void)hscroll;
#endif
}

/* Arm the RAINBOW decode for ONE field (KING regs 0x40..0x44).
   The HuC6271 decodes exactly one frame per arm, so this has to run EVERY field
   or the layer goes blank -- see pcfx_rainbow_rearm_field() in the vblank
   handler.  The once-at-setup arm this used to rely on is why the backdrop
   showed up for a single field and then vanished. */
static void pcfx_rainbow_start_transfer(void)
{
    pcfx_rainbow_write_reg16(0x40, 0x0000);
    pcfx_rainbow_write_reg32(0x41, (uint32_t)WAIFU_PCFX_RAINBOW_BG_KRAM_WORD_ADDR);
    pcfx_rainbow_write_reg16(0x42, (uint16_t)WAIFU_PCFX_RAINBOW_BG_TRANSFER_START);
    pcfx_rainbow_write_reg16(0x43, (uint16_t)WAIFU_PCFX_RAINBOW_BG_BLOCK_COUNT);
    pcfx_rainbow_write_reg16(0x44, 0x0000);
    pcfx_rainbow_write_reg16(0x40, 0x0001);
    g_rainbow_transfer_armed = 1;
}

static void pcfx_rainbow_stop_transfer(void)
{
    pcfx_rainbow_write_reg16(0x40, 0x0000);
    g_rainbow_transfer_armed = 0;
}

static void pcfx_vdc_sanctum_draw_overlay(WaifuPcfxSanctumOverlay overlay, int value, int blink_visible)
{
    pcfx_vdc_sanctum_clear_all();
    switch (overlay) {
    case WAIFU_PCFX_SANCTUM_OVERLAY_SAVE:
        pcfx_vdc_sanctum_panel(4, 10, 24, 10);
        if (value < 0) {
            pcfx_vdc_sanctum_print(10, 12, "SAVE FAILED", 12);
            pcfx_vdc_sanctum_print(6, 15, "The memory seal is", 20);
            pcfx_vdc_sanctum_print(10, 16, "broken.", 8);
        } else if (value > 0) {
            pcfx_vdc_sanctum_print(8, 12, "PROGRESS SAVED", 15);
            pcfx_vdc_sanctum_print(6, 15, "The sanctum remembers", 21);
            pcfx_vdc_sanctum_print(12, 16, "Serena.", 8);
        } else {
            pcfx_vdc_sanctum_print(10, 12, "NO SAVE DATA", 12);
            pcfx_vdc_sanctum_print(7, 15, "Nothing is written", 18);
            pcfx_vdc_sanctum_print(12, 16, "yet.", 5);
        }
        if (blink_visible) pcfx_vdc_sanctum_print(10, 18, "A/RUN/B BACK", 13);
        break;
    case WAIFU_PCFX_SANCTUM_OVERLAY_SAVE_DEVICE:
        pcfx_vdc_sanctum_panel(16, 6, 15, 15);
        pcfx_vdc_sanctum_print(20, 8, "SAVE TO", 8);
        pcfx_vdc_sanctum_print(18, 12, value == 0 ? "> INTERNAL" : "  INTERNAL", 11);
        pcfx_vdc_sanctum_print(18, 14, value == 1 ? "> FX-BMP" : "  FX-BMP", 9);
        pcfx_vdc_sanctum_print(18, 16, value == 2 ? "> BACK" : "  BACK", 7);
        break;
    case WAIFU_PCFX_SANCTUM_OVERLAY_MENU:
    default:
        pcfx_vdc_sanctum_panel(16, 5, 15, 18);
        pcfx_vdc_sanctum_print(19, 7, "SANCTUM", 8);
        pcfx_vdc_sanctum_print(18, 10, "A place of rest.", 16);
        pcfx_vdc_sanctum_print(18, 11, "Serena can", 11);
        pcfx_vdc_sanctum_print(18, 12, "prepare before", 14);
        pcfx_vdc_sanctum_print(18, 13, "the next duel.", 14);
        pcfx_vdc_sanctum_print(18, 16, value == 0 ? "> SAVE" : "  SAVE", 7);
        pcfx_vdc_sanctum_print(18, 18, value == 1 ? "> DECK EDITOR" : "  DECK EDITOR", 14);
        pcfx_vdc_sanctum_print(18, 20, value == 2 ? "> BACK" : "  BACK", 7);
        break;
    }
}

static void pcfx_vdc_apply_sanctum(WaifuPcfxVideo *video, WaifuPcfxSanctumBackdrop backdrop,
                                   WaifuPcfxSanctumOverlay overlay, int value, int blink_visible)
{
    int backdrop_changed;
    if (!video) return;
    backdrop_changed = (g_sanctum_loaded_backdrop != (int)backdrop);
    if (backdrop_changed) {
        waifu_pcfx_cdrom_read_rainbow_bg_to_kram(pcfx_rainbow_asset_for_backdrop(backdrop),
                                                 WAIFU_PCFX_RAINBOW_BG_KRAM_WORD_ADDR,
                                                 pcfx_rainbow_bytes_for_backdrop(backdrop));
        g_sanctum_loaded_backdrop = (int)backdrop;
    }

    video->vdc_overlay_ready = 0;
    video->vdc_overlay_shutdown_countdown = 0;
    g_vdc_overlay_dirty = 0;
    g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
    g_vdc_overlay_applied_fade_level = -1;

    waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 0);
    waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 0);
    waifu_vdc_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    waifu_vdc_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    vdc_set_scroll(VDC_CHIP_0, 0, 0);
    vdc_set_scroll(VDC_CHIP_1, 0, 0);
    waifu_vdc_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    waifu_vdc_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    waifu_vdc_setreg(VDC_CHIP_0, 5, 0x88);
    waifu_vdc_setreg(VDC_CHIP_1, 5, 0x80);

    pcfx_vdc_select_overlay_palette();
    pcfx_vdc_restore_overlay_palette();
    pcfx_vdc_sanctum_upload_tiles();
    pcfx_vdc_overlay_upload_font();
    pcfx_vdc_sanctum_draw_overlay(overlay, value, blink_visible);
    if (!g_sanctum_active || backdrop_changed || !g_rainbow_transfer_armed) {
        pcfx_rainbow_setup();
        pcfx_rainbow_start_transfer();
    }
    pcfx_rainbow_set_hscroll(g_rainbow_hscroll_requested ? g_rainbow_hscroll : 0);
    g_king_page_setting_extra = WAIFU_PCFX_KRAM_PAGESETTING_RAINBOW1;
    pcfx_king_reconfig_begin();
    king_set_bg_prio(KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 0);
    king_set_bg_mode(KING_BGMODE_NONE, 0, 0, 0);
    pcfx_king_reconfig_end();
    tetsu_set_priorities(7, 7, 0, 0, 0, 0, 6);
    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_16, TETSU_COLORS_16,
                              1, 1, 0, 0, 0, 0, 1);
    g_sanctum_active = 1;
}

static void pcfx_apply_rainbow_backdrop(WaifuPcfxVideo *video, WaifuPcfxSanctumBackdrop backdrop)
{
    int backdrop_changed;
    if (!video) return;
    backdrop_changed = (g_sanctum_loaded_backdrop != (int)backdrop);
    if (backdrop_changed) {
        waifu_pcfx_cdrom_read_rainbow_bg_to_kram(pcfx_rainbow_asset_for_backdrop(backdrop),
                                                 WAIFU_PCFX_RAINBOW_BG_KRAM_WORD_ADDR,
                                                 pcfx_rainbow_bytes_for_backdrop(backdrop));
        g_sanctum_loaded_backdrop = (int)backdrop;
    }

    if (!g_rainbow_backdrop_active || backdrop_changed) {
        /* RAINBOW composites the 8bpp overlay from a single page (0) only, so it
           runs unbuffered: no deferred flip, no page rotation. */
        video->front_page = 0;
        video->back_page = 0;
        video->pending_page = -1;
        video->have_last_frame = 0;
#if WAIFU_PCFX_DIRTY_PRESENT
        video->page_shadow_valid[0] = 0;
        video->page_shadow_valid[1] = 0;
#endif
    }

    g_king_page_setting_extra = WAIFU_PCFX_KRAM_PAGESETTING_RAINBOW1;
    if (g_king_page_setting_live != (int32_t)(0x00000100u | g_king_page_setting_extra)) {
        /* REG.0F: BG page bit only moves in blanking or with MPSW stopped. */
        pcfx_king_reconfig_begin();
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_reconfig_end();
    }
    if (!g_rainbow_backdrop_active || backdrop_changed) {
        waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 0);
        waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 0);
        waifu_vdc_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
        waifu_vdc_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
        vdc_set_scroll(VDC_CHIP_0, 0, 0);
        vdc_set_scroll(VDC_CHIP_1, 0, 0);
        waifu_vdc_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
        waifu_vdc_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
        waifu_vdc_setreg(VDC_CHIP_0, 5, 0x88);
        waifu_vdc_setreg(VDC_CHIP_1, 5, 0x80);
        pcfx_vdc_overlay_init(video);
        pcfx_king_reconfig_begin();
        king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 1);
        king_set_bg_mode(KING_BGMODE_256_PAL, 0, 0, 0);
        pcfx_king_set_8bpp_display_page(0);
        pcfx_king_reconfig_end();
        /* The RAINBOW still itself cannot be palette-faded, so the fade mask
           is drawn by VDC tiles.  Keep VDC in front of both KING BG0 and
           RAINBOW; transparent VDC tile pixels still let the scene show
           normally once the fade level reaches zero. */
        tetsu_set_priorities(7, 7, 6, 0, 0, 0, 5);
        tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                                  TETSU_COLORS_16, TETSU_COLORS_16,
                                  1, 1, 1, 0, 0, 0, 1);
    }
    if (!g_rainbow_backdrop_active || backdrop_changed || !g_rainbow_transfer_armed) {
        pcfx_rainbow_setup();
        pcfx_rainbow_start_transfer();
    }
    pcfx_rainbow_set_hscroll(g_rainbow_hscroll_requested ? g_rainbow_hscroll : 0);
    g_rainbow_backdrop_active = 1;
    g_sanctum_active = 0;
}

static void pcfx_vdc_clear_background(WaifuPcfxVideo *video)
{
    if (!video || (!g_sanctum_active && !g_rainbow_backdrop_active && video->vdc_bg == WAIFU_PCFX_VDC_BG_NONE)) return;
    pcfx_vdc_sanctum_clear_all();
    pcfx_vdc_restore_overlay_palette();
    pcfx_rainbow_stop_transfer();
    g_king_page_setting_extra = 0;
    pcfx_king_reconfig_begin();
    pcfx_king_set_bg_kram_page_inline(0);
    /* Enable BG0's affine fetch.  The source is 512x256 8bpp; A=2 samples the
       even texel of each duplicated KRAM word for every 256-wide output dot. */
    king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 1);
    king_set_bg_mode(KING_BGMODE_256_PAL, 0, 0, 0);
    pcfx_king_reconfig_end();
    tetsu_set_priorities(1, 0, 7, 0, 0, 0, 0);
    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);
    tetsu_set_vdc_palette(0, 0);
#if WAIFU_PCFX_DIRTY_PRESENT
    video->page_shadow_valid[0] = 0;
    video->page_shadow_valid[1] = 0;
#endif
    video->vdc_bg = WAIFU_PCFX_VDC_BG_NONE;
    video->vdc_overlay_ready = 0;
    video->vdc_overlay_shutdown_countdown = 0;
    g_vdc_overlay_dirty = 0;
    g_vdc_overlay_applied_mode = WAIFU_PCFX_OVERLAY_OFF;
    g_vdc_overlay_applied_fade_level = -1;
    g_sanctum_active = 0;
    g_rainbow_backdrop_active = 0;
    video->active_palette = (WaifuFmPaletteId)-1;
    video->active_fade_q8 = -1;
    video->front_page = 0;
    video->back_page = 1;
    video->pending_page = -1;
}

static void pcfx_vdc_apply_requested_background(WaifuPcfxVideo *video, WaifuPcfxVdcBackground bg)
{
    if (!video) return;
    if (bg == WAIFU_PCFX_VDC_BG_NONE) pcfx_vdc_clear_background(video);
}

static void pcfx_vdc_overlay_print(int tx, int ty, const char *str, int max_len)
{
    int len;
    if (!str || max_len <= 0) return;
    if (tx < 0 || ty < 0 || tx >= WAIFU_PCFX_VDC_MAP_W || ty >= WAIFU_PCFX_VDC_MAP_H) return;
    if (max_len > WAIFU_PCFX_VDC_MAP_W - tx) max_len = WAIFU_PCFX_VDC_MAP_W - tx;
    len = pcfx_strlen_limited(str, max_len);

    vdc_set_vram_write(VDC_CHIP_0, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) {
        unsigned char ch = (i < len) ? (unsigned char)str[i] : (unsigned char)' ';
        uint16_t tile = WAIFU_PCFX_VDC_BLANK_TILE;
        if (ch >= WAIFU_PCFX_VDC_FONT_FIRST && ch <= WAIFU_PCFX_VDC_FONT_LAST) {
            tile = (uint16_t)(WAIFU_PCFX_VDC_FONT_TILE_BASE + (ch - WAIFU_PCFX_VDC_FONT_FIRST));
        }
        vdc_vram_write(VDC_CHIP_0, tile);
    }

    vdc_set_vram_write(VDC_CHIP_1, ty * WAIFU_PCFX_VDC_MAP_W + tx);
    for (int i = 0; i < max_len; ++i) {
        unsigned char ch = (i < len) ? (unsigned char)str[i] : (unsigned char)' ';
        uint16_t tile = WAIFU_PCFX_VDC_BLANK_TILE;
        if (ch >= WAIFU_PCFX_VDC_FONT_FIRST && ch <= WAIFU_PCFX_VDC_FONT_LAST) {
            tile = (uint16_t)(WAIFU_PCFX_VDC_FONT_TILE_BASE + (ch - WAIFU_PCFX_VDC_FONT_FIRST));
        }
        vdc_vram_write(VDC_CHIP_1, (uint16_t)(tile | 0x8000));
    }
}

static void pcfx_vdc_overlay_print_centered(int ty, const char *str)
{
    int len = pcfx_strlen_limited(str, WAIFU_PCFX_VDC_VISIBLE_W);
    pcfx_vdc_overlay_print((WAIFU_PCFX_VDC_VISIBLE_W - len) / 2, ty, str, len);
}

static void pcfx_story_upload_portrait(int slot, int portrait_id, int screen_y)
{
    const uint8_t *pix = waifu_assets_story_portrait_pixels(portrait_id);
    const uint8_t *mask = waifu_assets_story_portrait_mask(portrait_id);
    uint16_t base_no = slot == 0 ? WAIFU_PCFX_STORY_SLOT0_NO : WAIFU_PCFX_STORY_SLOT1_NO;
    if (!pix) return;

    for (int cell = 0; cell < WAIFU_PCFX_STORY_CELLS; ++cell) {
        int cx = (cell % WAIFU_PCFX_STORY_COLS) * WAIFU_PCFX_STORY_CELL_W;
        int cy = (cell / WAIFU_PCFX_STORY_COLS) * WAIFU_PCFX_STORY_CELL_H;
        for (int block = 0; block < 4; ++block) {
            uint16_t hi[4][16] = {{0}};
            uint16_t lo[4][16] = {{0}};
            for (int row = 0; row < 16; ++row) {
                int sy = cy + block * 16 + row;
                if (sy >= WAIFU_STORY_PORTRAIT_H || screen_y + sy >= WAIFU_FM_HEIGHT - 66) continue;
                for (int x = 0; x < 16; ++x) {
                    int sx = cx + x;
                    int src_i;
                    uint8_t v;
                    uint16_t bit;
                    if (sx >= WAIFU_STORY_PORTRAIT_W) continue;
                    src_i = sy * WAIFU_STORY_PORTRAIT_W + sx;
                    v = pix[src_i];
                    if (mask ? !mask[src_i] : v == 0) continue;
                    /* Combined VDC sprites key transparency from the LOW
                     * nibble.  Preserve every other palette index and nudge
                     * only the sixteen unsafe x0 colours to a neighbour. */
                    if ((v & 0x0f) == 0) v = (uint8_t)(v | 1);
                    bit = (uint16_t)(1u << (15 - x));
                    for (int plane = 0; plane < 4; ++plane) {
                        if ((v >> (plane + 4)) & 1) hi[plane][row] |= bit;
                        if ((v >> plane) & 1) lo[plane][row] |= bit;
                    }
                }
            }
            for (int chip = 0; chip < 2; ++chip) {
                uint16_t addr = (uint16_t)((base_no + cell * WAIFU_PCFX_STORY_NO_STRIDE) * 64 + block * 128);
                vdc_set_vram_write(chip, addr);
                for (int plane = 0; plane < 4; ++plane)
                    for (int row = 0; row < 16; ++row)
                        vdc_vram_write(chip, chip == 0 ? hi[plane][row] : lo[plane][row]);
            }
        }
    }
    g_story_portrait_uploaded[slot] = portrait_id;
    g_story_portrait_uploaded_y[slot] = screen_y;
}

static void pcfx_story_write_sat(void)
{
    uint16_t sat[64 * 4];
    int n = 0;
    int was_visible = g_story_sat_visible;
    memset(sat, 0, sizeof(sat));
    for (int slot = 0; slot < g_story_portrait_count; ++slot) {
        uint16_t base_no = slot == 0 ? WAIFU_PCFX_STORY_SLOT0_NO : WAIFU_PCFX_STORY_SLOT1_NO;
        for (int cell = 0; cell < WAIFU_PCFX_STORY_CELLS && n < 64; ++cell) {
            int col = cell % WAIFU_PCFX_STORY_COLS;
            int row = cell / WAIFU_PCFX_STORY_COLS;
            if (g_story_portrait_request[slot].y + row * WAIFU_PCFX_STORY_CELL_H >= WAIFU_FM_HEIGHT - 66) continue;
            sat[n * 4 + 0] = (uint16_t)((g_story_portrait_request[slot].y + row * WAIFU_PCFX_STORY_CELL_H + 0x40) & 0x3ff);
            sat[n * 4 + 1] = (uint16_t)((g_story_portrait_request[slot].x + col * WAIFU_PCFX_STORY_CELL_W + 0x20) & 0x3ff);
            sat[n * 4 + 2] = (uint16_t)(((base_no + cell * WAIFU_PCFX_STORY_NO_STRIDE) << 1) & 0x7ff);
            sat[n * 4 + 3] = WAIFU_PCFX_STORY_SAT_H64;
            ++n;
        }
    }
    vdc_set_vram_write(VDC_CHIP_0, WAIFU_PCFX_STORY_SAT_ADDR);
    for (int i = 0; i < 64 * 4; ++i) vdc_vram_write(VDC_CHIP_0, sat[i]);
    vdc_set_vram_write(VDC_CHIP_1, WAIFU_PCFX_STORY_SAT_ADDR);
    for (int i = 0; i < 64; ++i) {
        vdc_vram_write(VDC_CHIP_1, sat[i * 4 + 0]);
        vdc_vram_write(VDC_CHIP_1, sat[i * 4 + 1]);
        vdc_vram_write(VDC_CHIP_1, sat[i * 4 + 2]);
        vdc_vram_write(VDC_CHIP_1, sat[i * 4 + 3] ? (WAIFU_PCFX_STORY_SAT_H64 | 0x0008) : 0);
    }
    /* Arm only after both complete source tables exist, and re-arm once when
     * portraits become visible.  The boot-time empty-SAT cleanup happens while
     * title video is active; the later title->8bpp timing/control handoff can
     * leave real VDCs no longer repeating that old arm even though our software
     * flag still says it happened.  That made the first Serena portrait remain
     * absent on hardware.  A hidden->visible edge is safely away from the VDW
     * latch (this flush runs in the VCE blank window), so both chips receive a
     * fresh source/auto-DMA arm without restoring the old per-frame sequential
     * re-arm that mixed high and low nibbles for one field during duels. */
    if (!g_story_sat_dma_armed || (n != 0 && !was_visible)) {
        vdc_setreg(VDC_CHIP_0, VDC_REG_DCR, VDC_DCR_SATB_AUTO);
        vdc_setreg(VDC_CHIP_1, VDC_REG_DCR, VDC_DCR_SATB_AUTO);
        vdc_set_satb_address(VDC_CHIP_0, WAIFU_PCFX_STORY_SAT_ADDR);
        vdc_set_satb_address(VDC_CHIP_1, WAIFU_PCFX_STORY_SAT_ADDR);
        g_story_sat_dma_armed = 1;
    }
    g_story_sat_visible = n != 0;
}

static void pcfx_story_layers_flush(WaifuPcfxVideo *video)
{
    int active = g_story_portrait_count != 0;
    if (!active && !g_story_sat_visible) return;
    if (!active) {
        g_story_portrait_count = 0;
        pcfx_story_write_sat();
        return;
    }
    /* VDC sprites sit in front of KING and RAINBOW.  The dialog bar keeps its
     * established KING palette/text path; portrait cells are clipped before
     * that bar, so it remains an opaque foreground HUD. */
    waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 1);
    waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 1);
    if (g_rainbow_backdrop_active) {
        tetsu_set_priorities(7, 6, 5, 0, 0, 0, 4);
        tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                             TETSU_COLORS_256, TETSU_COLORS_256, 1, 1, 1, 0, 0, 0, 1);
    } else {
        tetsu_set_priorities(7, 6, 5, 0, 0, 0, 0);
        tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                             TETSU_COLORS_256, TETSU_COLORS_256, 1, 1, 1, 0, 0, 0, 0);
    }
    for (int slot = 0; slot < g_story_portrait_count; ++slot)
        if (g_story_portrait_uploaded[slot] != g_story_portrait_request[slot].id ||
            g_story_portrait_uploaded_y[slot] != g_story_portrait_request[slot].y)
            pcfx_story_upload_portrait(slot, g_story_portrait_request[slot].id,
                                       g_story_portrait_request[slot].y);
    pcfx_story_write_sat();
    if (video) video->vdc_overlay_shutdown_countdown = 0;
}

static void pcfx_vdc_overlay_print_story_line(int tx, int ty, const char *str, int max_len, int *visible_chars)
{
    int len;
    if (!visible_chars) return;
    if (*visible_chars <= 0) {
        pcfx_vdc_overlay_print(tx, ty, "", max_len);
        return;
    }
    len = pcfx_strlen_limited(str, max_len);
    if (len > *visible_chars) len = *visible_chars;
    pcfx_vdc_overlay_print(tx, ty, str, len);
    if (max_len > len) pcfx_vdc_overlay_print(tx + len, ty, "", max_len - len);
    *visible_chars -= len;
}

static void pcfx_vdc_overlay_init(WaifuPcfxVideo *video)
{
    if (!video) return;

    waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 0);
    waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 0);
    waifu_vdc_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    waifu_vdc_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    vdc_set_scroll(VDC_CHIP_0, 0, 0);
    vdc_set_scroll(VDC_CHIP_1, 0, 0);
    waifu_vdc_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    waifu_vdc_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);

    pcfx_vdc_restore_overlay_palette();

    pcfx_vdc_overlay_upload_fade_tile(16);
    pcfx_vdc_overlay_upload_font();
    /* The BIOS leaves a live VDC SAT behind.  Title mode hides VDC sprites,
     * but RAINBOW story scenes enable that output again; without an explicit
     * empty SAT transfer the BIOS logo/sprites reappear above a loaded story.
     * Ask the vblank story flush to publish a paired zero SAT after this VDC
     * setup is complete. */
    g_story_sat_visible = 1;
    /* Start covered.  The first title frame may require a full 16M KRAM upload;
       keeping an opaque VDC black mask in front prevents any one-frame reveal
       of uninitialized or partially uploaded KING data. */
    g_vdc_overlay_fade_level = 16;
    g_vdc_overlay_applied_fade_level = -1;
    pcfx_vdc_overlay_fill_fade_map();
    waifu_vdc_setreg(VDC_CHIP_0, 5, 0x88);
    waifu_vdc_setreg(VDC_CHIP_1, 5, 0x80);

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
    g_vdc_overlay_fade_map_active = 0;
}

static void pcfx_vdc_overlay_flush(WaifuPcfxVideo *video)
{
    if (!video || !video->vdc_overlay_ready || !g_vdc_overlay_dirty) return;

    if (g_vdc_overlay_fade_level > 0) {
        /* Fade masks are authoritative for the whole VDC overlay while active.
           Suppress text during the fade so prompt/menu glyph tiles cannot punch
           transparent holes through the black dither mask. */
        if (!g_vdc_overlay_fade_map_active ||
            g_vdc_overlay_applied_fade_level <= 0 ||
            g_vdc_overlay_applied_mode != g_vdc_overlay_mode) {
            pcfx_vdc_overlay_fill_fade_map();
        }
        if (g_vdc_overlay_applied_fade_level != g_vdc_overlay_fade_level) {
            pcfx_vdc_overlay_upload_fade_tile(g_vdc_overlay_fade_level);
        }
        g_vdc_overlay_applied_fade_level = g_vdc_overlay_fade_level;
        g_vdc_overlay_applied_mode = g_vdc_overlay_mode;
        g_vdc_overlay_dirty = 0;
        return;
    }

    if (g_vdc_overlay_applied_fade_level != 0 ||
        g_vdc_overlay_applied_mode != g_vdc_overlay_mode) {
        pcfx_vdc_overlay_clear_all();
        g_vdc_overlay_applied_fade_level = 0;
        g_vdc_overlay_applied_mode = g_vdc_overlay_mode;
        g_vdc_overlay_fade_map_active = 0;
    }

    switch (g_vdc_overlay_mode) {
    default:
    case WAIFU_PCFX_OVERLAY_OFF:
        pcfx_vdc_overlay_clear_all();
        break;

    case WAIFU_PCFX_OVERLAY_TITLE_PROMPT:
        pcfx_vdc_overlay_clear_rect(6, 23, 24, 4);
        if (g_vdc_overlay_prompt_visible) {
            pcfx_vdc_overlay_print(7, 23, "PRESS RUN TO START", 22);
        }
        pcfx_vdc_overlay_print_centered(25, "(C) 2026 GAMEBLABLA");
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

    case WAIFU_PCFX_OVERLAY_ENDING_STORY:
    {
        int visible_chars = g_vdc_overlay_ending_visible_chars;
        pcfx_vdc_overlay_clear_rect(0, 18, WAIFU_PCFX_VDC_VISIBLE_W, 10);
        if (visible_chars > 0) pcfx_vdc_overlay_print(2, 19, g_vdc_overlay_ending_name, g_vdc_overlay_ending_name_len);
        switch (g_vdc_overlay_ending_page) {
        case 1:
            pcfx_vdc_overlay_print_story_line(2, 21, "I FINALLY DEFEATED THEM.", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 22, "THE ONES WHO TURNED DREAMS", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 23, "INTO CHAINS.", 28, &visible_chars);
            break;
        case 2:
            pcfx_vdc_overlay_print_story_line(2, 21, "THE DECK IS WHOLE AGAIN.", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 22, "EVERY STOLEN CARD HAS", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 23, "FOUND ITS WAY HOME.", 28, &visible_chars);
            break;
        case 3:
            pcfx_vdc_overlay_print_story_line(2, 21, "WHEN MORNING COMES, I WILL", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 22, "CARRY THESE CARDS BEYOND", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 23, "THE RUINS.", 28, &visible_chars);
            break;
        case 0:
        default:
            pcfx_vdc_overlay_print_story_line(2, 21, "THE LAST SHARD IS SILENT.", 28, &visible_chars);
            pcfx_vdc_overlay_print_story_line(2, 22, "NO ENEMY ANSWERS ITS CALL.", 28, &visible_chars);
            break;
        }
        if (g_vdc_overlay_ending_visible_chars > 0 && g_vdc_overlay_ending_prompt_visible) {
            pcfx_vdc_overlay_print_centered(26, "A/RUN CONTINUE");
        }
        break;
    }

    case WAIFU_PCFX_OVERLAY_ENDING_CREDITS:
        pcfx_vdc_overlay_clear_all();
        pcfx_vdc_overlay_print_centered(10, "THANK YOU FOR PLAYING.");
        pcfx_vdc_overlay_print_centered(13, "SHATTERED DECKS.");
        pcfx_vdc_overlay_print_centered(16, "A GAME BY GAMEBLABLA.");
        pcfx_vdc_overlay_print_centered(19, "(C) 2026");
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

void waifu_pcfx_video_overlay_ending_story(const char *name, int page, int prompt_visible, int visible_chars)
{
    char name_buf[8];
    int name_len = 0;
    if (page < 0) page = 0;
    if (page > 3) page = 3;
    prompt_visible = prompt_visible ? 1 : 0;
    if (visible_chars < 0) visible_chars = 0;
    if (visible_chars > 255) visible_chars = 255;
    if (name) {
        while (name_len < (int)sizeof(name_buf) - 1 && name[name_len]) {
            name_buf[name_len] = name[name_len];
            ++name_len;
        }
    }
    if (name_len == 0) {
        name_buf[0] = 'S'; name_buf[1] = 'E'; name_buf[2] = 'R'; name_buf[3] = 'E';
        name_buf[4] = 'N'; name_buf[5] = 'A'; name_len = 6;
    }
    name_buf[name_len] = '\0';
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_ENDING_STORY ||
        g_vdc_overlay_ending_page != page ||
        g_vdc_overlay_ending_prompt_visible != prompt_visible ||
        g_vdc_overlay_ending_visible_chars != visible_chars ||
        g_vdc_overlay_ending_name_len != name_len ||
        memcmp(g_vdc_overlay_ending_name, name_buf, (size_t)name_len) != 0) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_ENDING_STORY;
        g_vdc_overlay_ending_page = page;
        g_vdc_overlay_ending_prompt_visible = prompt_visible;
        g_vdc_overlay_ending_visible_chars = visible_chars;
        memcpy(g_vdc_overlay_ending_name, name_buf, (size_t)(name_len + 1));
        g_vdc_overlay_ending_name_len = name_len;
        g_vdc_overlay_dirty = 1;
    }
}

void waifu_pcfx_video_overlay_ending_credits(void)
{
    if (g_vdc_overlay_mode != WAIFU_PCFX_OVERLAY_ENDING_CREDITS) {
        g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_ENDING_CREDITS;
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
    tetsu_set_priorities(7, 0, 6, 0, 0, 0, 0);
    pcfx_vdc_select_overlay_palette();
    tetsu_set_king_palette(0, 0, 0, 0);
    tetsu_set_rainbow_palette(0);

    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);

    /* Bring the opaque VDC black mask up BEFORE switching KING BG0 into 16M
       mode.  The 16M title page shares KRAM word offset 0 with the 8bpp page,
       so flipping to 16M while the previous 8bpp/loading frame is still resident
       (the title is not uploaded until later in this same present) shows one
       frame of that data reinterpreted as YUV garbage: a pink wash with a
       diagonal mode-switch seam.  Masking first hides the switch entirely. */
    waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 0);
    waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 0);
    pcfx_vdc_overlay_init(&g_video);

    /* REG.10 (BG format) is immediate-effect and must be changed with the
       microprogram stopped, and REG.0F's BG page bit may only move in blanking
       or with MPSW stopped.  Take the window for the whole KING block below,
       including the microprogram download it ends with. */
    pcfx_king_reconfig_begin();
    king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 0);
    king_set_bg_mode(KING_BGMODE_16M, 0, 0, 0);
    pcfx_king_set_bg_kram_page_inline(0);

    /* Bank A holds both 16M surfaces (words 0 and 0x10000, below the D17 bank
       boundary at 0x20000), so the eight CG accesses go in slots 0-7 and bank
       B's slots 8-F must hold KING_CODE_NOP -- not the all-zero word memset
       used to leave there.  Opcode 0 is not NOP (C6272_2 3.6.7.2: NOP is its
       own encoding); an all-zero slot describes a real bank-B access with every
       field zero, so KING schedules eight spurious bank-B fetches per dot cycle
       alongside the intended ones.  pcfxemu decodes only the CG base's bank and
       so never shows it. */
    for (int i = 0; i < 16; ++i) g_king_microprog[i] = KING_CODE_NOP;
    g_king_microprog[0] = KING_CODE_BG0_CG_0;
    g_king_microprog[1] = KING_CODE_BG0_CG_1;
    g_king_microprog[2] = KING_CODE_BG0_CG_2;
    g_king_microprog[3] = KING_CODE_BG0_CG_3;
    g_king_microprog[4] = KING_CODE_BG0_CG_4;
    g_king_microprog[5] = KING_CODE_BG0_CG_5;
    g_king_microprog[6] = KING_CODE_BG0_CG_6;
    g_king_microprog[7] = KING_CODE_BG0_CG_7;
    king_write_microprogram(g_king_microprog, 0, 16);
    /* The rotation program is gone; force a reload on the next 8bpp page set. */
    g_king_mprog_bank = -1;
    pcfx_king_set_bg0_page_inline(0);
    /* In 16M KING mode with the dual-VDC overlay enabled, the top four
       visible scanlines sample the wrapped hidden rows unless BG0 is scrolled
       down by four pixels.  Keep the title/Menu surface aligned to the
       256x240 framebuffer instead of exposing stale hidden KRAM at the top. */
    king_set_scroll(KING_BG0, 0, WAIFU_PCFX_TITLE_16M_SCROLL_Y);
    king_set_scroll(KING_BG0SUB, 0, WAIFU_PCFX_TITLE_16M_SCROLL_Y);
    king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
    king_set_bg_size(KING_BG0SUB, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256, KING_BGSIZE_256);
    pcfx_king_reconfig_end();
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

/* Retry spacing for a refused 16M upload, in frames (~1s at 60Hz). */
#define WAIFU_PCFX_TITLE_UPLOAD_RETRY_FRAMES 60u

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

static WAIFU_PCFX_COLD int pcfx_ending_upload_full_16m_direct_cd(int page)
{
#if defined(WAIFU_ASSET_USE_CDROM)
    return waifu_pcfx_cdrom_read_ending_yuv422_to_kram(title16m_page_word_offset(page),
                                                       WAIFU_PCFX_TITLE_16M_KRAM_WORDS * 2u);
#else
    (void)page;
    return 0;
#endif
}

static WAIFU_PCFX_COLD void pcfx_upload_black_16m_page(int page)
{
    uint32_t page_base = title16m_page_word_offset(page);
    static uint16_t black_row[WAIFU_PCFX_W];
    for (int x = 0; x < WAIFU_PCFX_W; x += 2) {
        black_row[x] = WAIFU_PCFX_16M_BLACK_Y;
        black_row[x + 1] = WAIFU_PCFX_16M_BLACK_UV;
    }
    king_seek_write_words(page_base);
    for (int row = 0; row < WAIFU_PCFX_TITLE_16M_KRAM_ROWS; ++row) {
        king_kram_write_buffer((void *)black_row, WAIFU_PCFX_W * 2);
    }
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

    need_upload = reconfigure || video->active_palette != WAIFU_FM_PALETTE_TITLE || !video->title16m_page_valid[0];
    if (reconfigure) video->title16m_upload_backoff = 0;
    if (need_upload && video->title16m_upload_backoff) {
        --video->title16m_upload_backoff;
        need_upload = 0;
    }
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
            video->active_palette = WAIFU_FM_PALETTE_TITLE;
            video->front_page = 0;
            video->back_page = 0;
            video->have_last_frame = 1;
            video->title16m_upload_backoff = 0;
        } else {
            /* The upload failed and there is no RAM copy to fall back on:
               waifu_assets_title_screen_pcfx_yuv422() returns NULL
               unconditionally in PC-FX CD-ROM builds, because the 128 KiB
               CPU-side title buffer was removed in favour of this direct
               CD->KRAM DMA.  So `ok` stays 0, title16m_page_valid[0] is never
               set, and without this backoff need_upload is true again on the
               very next frame -- a fresh 128 KiB CD->KRAM read EVERY FRAME,
               for as long as the title or menu is on screen.

               That is not a theoretical loop.  Each of those reads bumps the
               CD-DA read sequence, and the music manager can only restart a
               track from its beginning, so the drive audibly seeks and the
               title theme restarts over and over.  It is also self-sustaining:
               a data read issued while CD-DA is streaming is exactly the read
               most likely to be refused, so the failure keeps reproducing
               itself.  pcfxemu's reads always succeed, so it never shows any
               of this.

               Back off instead: retry about once a second, which still
               recovers if the drive was merely busy, but cannot thrash. */
            video->title16m_upload_backoff = WAIFU_PCFX_TITLE_UPLOAD_RETRY_FRAMES;
        }
    }
}

static WAIFU_PCFX_COLD void pcfx_present_ending_16m(WaifuPcfxVideo *video, int black)
{
    int reconfigure;
    int need_upload;
    WaifuFmPaletteId target = black ? WAIFU_FM_PALETTE_ENDING_BLACK : WAIFU_FM_PALETTE_ENDING;

    if (!video) return;

    reconfigure = (!video->initialized || video->mode != WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR);
    if (reconfigure) {
        set_king_16m_title_video();
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_set_bg0_page_inline(title16m_bg_cg_page(0));
        video->mode = WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR;
        video->initialized = 1;
        video->active_palette = (WaifuFmPaletteId)-1;
        video->active_fade_q8 = -1;
        video->front_page = 0;
        video->back_page = 0;
        video->pending_title_page_flip = 0;
        video->title16m_page_valid[0] = 0;
        video->title16m_page_valid[1] = 0;
    }

    pcfx_vdc_overlay_set_fade_q8(waifu_fm_video_fade_q8());

    need_upload = reconfigure || video->active_palette != target || !video->title16m_page_valid[0];
    if (reconfigure) video->title16m_upload_backoff = 0;
    if (need_upload && video->title16m_upload_backoff) {
        --video->title16m_upload_backoff;
        need_upload = 0;
    }
    if (need_upload) {
        int ok = 1;
        if (black) {
            pcfx_upload_black_16m_page(0);
        } else {
            ok = pcfx_ending_upload_full_16m_direct_cd(0);
            if (!ok) {
                const uint16_t *ending_yuv422 = waifu_assets_ending_screen_pcfx_yuv422();
                if (ending_yuv422) {
                    pcfx_title_upload_full_16m_from_ram(0, ending_yuv422);
                    ok = 1;
                }
            }
        }
        if (ok) {
            video->title16m_page_valid[0] = 1;
            video->active_palette = target;
            video->front_page = 0;
            video->back_page = 0;
            video->have_last_frame = 1;
            video->title16m_upload_backoff = 0;
        } else {
            /* Same per-frame CD re-read loop as the title path above. */
            video->title16m_upload_backoff = WAIFU_PCFX_TITLE_UPLOAD_RETRY_FRAMES;
        }
    }
}

static void set_king_8bpp_video(int display_page)
{
    /* Keep the front VDC overlay intact until the new 8bpp KING page has been
       cleared.  Clearing it here exposes the previous 16M title surface for a
       frame during title/menu -> loading/battle mode switches. */
    tetsu_set_priorities(1, 0, 7, 0, 0, 0, 0);
    pcfx_vdc_select_overlay_palette();
    /* KING palette offsets are stored as 8-bit values in two-color units in
       the VCE.  Use palette bank 0 for the 8bpp page path and upload the same
       RGB-derived entries there.  Passing 256 wrapped through the low byte on
       some paths, leaving BIOS/default colors visible. */
    tetsu_set_king_palette(0, 0, 0, 0);
    tetsu_set_rainbow_palette(0);

    /* Same reconfiguration window as the 16M path: REG.10 with MPSW stopped,
       REG.0F and the immediate-effect size registers inside blanking. */
    pcfx_king_reconfig_begin();
    king_set_bg_prio(KING_BGPRIO_0, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, KING_BGPRIO_HIDE, 1);
    king_set_bg_mode(KING_BGMODE_256_PAL, 0, 0, 0); /* 8bpp KING BG0. */
    pcfx_king_set_bg_kram_page_inline(0);

    king_set_scroll(KING_BG0, 0, 0);
    king_set_scroll(KING_BG0SUB, 0, 0);
    king_set_bg_size(KING_BG0, KING_BGSIZE_256, KING_BGSIZE_512, KING_BGSIZE_256, KING_BGSIZE_512);
    king_set_bg_size(KING_BG0SUB, KING_BGSIZE_256, KING_BGSIZE_512, KING_BGSIZE_256, KING_BGSIZE_512);
    /* Point the display at the caller-selected page directly as the BG mode
       becomes 8bpp.  begin_8bpp selects the third page for the mode handoff;
       it is outside both 16M title surfaces and stays clean black across the
       mid-scan 16M -> 8bpp switch.  This also loads the 8-access rotation
       microprogram into that page's KRAM bank and asserts priority/affine. */
    g_king_mprog_bank = -1;   /* mode switch invalidates any loaded program */
    pcfx_king_set_8bpp_display_page(display_page);
    pcfx_king_reconfig_end();

    tetsu_set_video_mode(TETSU_LINES_262, 0, TETSU_DOTCLOCK_5MHz,
                              TETSU_COLORS_256, TETSU_COLORS_16,
                              1, 0, 1, 0, 0, 0, 0);

    /* Keep VDC layers initialized and ready for converted backgrounds, but the
       first port leaves them transparent/empty unless a PC-FX VDC background
       is explicitly selected. */
    waifu_vdc_set_control(VDC_CHIP_0, 0, 1, 0);
    waifu_vdc_set_control(VDC_CHIP_1, 0, 1, 0);
    waifu_vdc_set_access_width(VDC_CHIP_0, 0, SUP_LOW_MAP_64X32, 0, 0);
    waifu_vdc_set_access_width(VDC_CHIP_1, 0, SUP_LOW_MAP_64X32, 0, 0);
    vdc_set_scroll(VDC_CHIP_0, 0, 0);
    vdc_set_scroll(VDC_CHIP_1, 0, 0);
    waifu_vdc_set_video_mode(VDC_CHIP_0, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
    waifu_vdc_set_video_mode(VDC_CHIP_1, 2, 2, 4, 0x1F, 0x11, 2, 239, 2);
}

/* KRAM MODE (KING REG.61) -- must be programmed after reset and BEFORE any KRAM
   access.  C6272_1 2.1 step 3; REG.61 is a single 16-bit cell, D15..D1 unused,
   D0 = MOD (0 = 1-Mbit, 1 = 4-Mbit).

   Nothing in this port ever wrote it, so the console kept whatever the retail
   BIOS left -- and the wolf-pcfx / doom-pcfx hardware burns established that is
   1-MBIT mode.  In 1-Mbit mode (Hudson map figure c260c7b9) only words
   0x00000..0x0FFFF of each bank are SOLID; 0x10000..0x1FFFF is the DOTTED
   optional-expansion half and reads back as open bus, and 1.3 requires every
   REG.0F page selector to be zero, so page routing silently does nothing.

   This port's KRAM map does not survive that, and every part of it that does not
   survive arrived with the affine/ROTATE work (21fef51), which grew the BG0 page
   stride from 30720 words to 0x10000:

     - affine page 1 sits at word 0x10000, entirely inside the dead half;
     - affine page 2 sits at word 0x20000 (bank B), which is the page
       waifu_pcfx_video_begin_8bpp() parks the display on for the 16M->8bpp
       handoff -- so the loading screen is composited from a plane that, in
       1-Mbit mode, is not there.  That is the black boot: the console never
       reaches a visible "LOADING...";
     - the CD->RAM DMA bounce window is at word 0x30000 (waifu_pcfx_cdrom.c),
       bank B's dead half, so asset reads bounce through open bus;
     - the RAINBOW backdrop stream and the SFX ADPCM bank both live on KRAM
       PAGE 1, which in 1-Mbit mode is not a place: their writes fold back onto
       page 0, i.e. on top of the framebuffers.

   None of it reports an error, and pcfxemu models KRAM as the full map
   unconditionally (king.c records REG.61 into king->KRAM_Mode and never narrows
   addressing by it), so the whole class is invisible in the emulator -- the
   title screen captures clean either way.  4-Mbit mode (figure 61476210) makes
   all of it real: 2 pages x 262144 words, per page bank A = 0x00000..0x1FFFF and
   bank B = 0x20000..0x3FFFF, every 64K block solid, page selectors live.

   It is not a speed trade.  libpcfx example 024 swept REG.61 on real hardware
   against a fixed BG schedule and timed 30720 CPU->KRAM halfwords at
   26038/26035/26033/26032 ticks for $61 = 0/1/2/3 -- a 0.023% spread with no
   bit-0 structure, on a bench that separates microprogram variants by 10%.  It
   is a capacity knob.

   No interrupt guard around the select/data pair: main() masks every source at
   the controller and disables at the CPU (PSW) before this runs, so nothing can
   land between the index and data writes. */
void waifu_pcfx_video_init_kram_mode(void)
{
    static int done;
    if (done) return;
    done = 1;
    king_set_kram_mode(1);   /* 1 = 4-Mbit: 2 pages x 262144 words */
}

WaifuPcfxVideo *waifu_pcfx_video_create(void)
{
    /* Normally already done from main(); harmless and idempotent if not, and it
       keeps this module correct standalone.  king_init() below is itself a KRAM
       access (it clears all four half-pages), and only reaches them in 4-Mbit. */
    waifu_pcfx_video_init_kram_mode();

    king_init();
    tetsu_init();
    memset(&g_video, 0, sizeof(g_video));
    g_video.front_page = 0;
    g_video.back_page = 1;
    g_video.pending_page = -1;
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
    g_rainbow_backdrop_requested = 0;
    g_rainbow_backdrop_active = 0;
    g_sanctum_requested = 0;
    g_sanctum_active = 0;
    g_king_page_setting_extra = 0;
    pcfx_rainbow_stop_transfer();
    video->vdc_bg = WAIFU_PCFX_VDC_BG_NONE;
    pcfx_vdc_overlay_force_black(video);
    if (video->mode == WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR) pcfx_title_blackout_pages(video);
    /* The 16M->8bpp mode switch happens mid-scan, and KRAM word offset 0 is
       shared by 8bpp page 0 and 16M title CG page 0.  No single value is black
       in both interpretations (16M-black 0x0101/0x8080 reads as an 8bpp index
       stripe pattern; 8bpp-black 0xFFFF reads as bright 16M), so a page at
       offset 0 always shows a stripe band or a white line on the switch frame.
       Instead, leave offset 0 untouched and bring up 8bpp on the SECOND page
       (offset PAGE_STRIDE_WORDS): the switch frame then never shows the old 16M
       surface reinterpreted as palette indices, and the opaque VDC black mask
       forced above covers the transient either way. */
    king_seek_write_words(WAIFU_PCFX_PAGE_STRIDE_WORDS);
    king_kram_fill_words(WAIFU_PCFX_BLACK_WORD, WAIFU_PCFX_PAGE_STRIDE_WORDS);
    set_king_8bpp_video(1);
    /* Blacken the KING 8bpp VCE palette (entries 0..255) now.  set_king_8bpp_video
       only configures the palette BANK; the 256 colour entries are not written
       until the first present_8bpp uploads a real frame (during asset load / the
       loading screen).  The KRAM just filled above holds the IDX_BLACK *index*,
       so until that first present the screen shows index-0-ish pixels through
       whatever the VCE palette RAM happens to hold.  At cold boot that is the
       undefined/BIOS power-up state -- bright on real hardware -- producing a
       full-screen WHITE flash right after the BIOS hands off and as the loading
       screen comes up (pcfxemu clears VCE to black, so it hides this; the console
       shows it).  Force every entry to neutral black here so the boot/loading
       window is black regardless of power-up state.  active_palette stays invalid
       (set below), so the first present still uploads the real palette. */
    for (int i = 0; i < 256; ++i) pcfx_vce_set_palette((uint16_t)i, WAIFU_PCFX_NEUTRAL_BLACK);
    /* Nothing presents between here and the first loading frame, so push the
       blackout out now -- inside blanking, per C6261 2.1.3 (5). */
    pcfx_vce_palette_flush_in_blank();
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
       frame into page 0 (whose KRAM still holds the 16M title bytes) before it
       is ever displayed, so force its shadow invalid. */
    video->front_page = 1;
    video->back_page = 0;
    video->pending_page = -1;
#if WAIFU_PCFX_DIRTY_PRESENT
    memset(video->page_shadow[1], IDX_BLACK, WAIFU_PCFX_FRAME_BYTES);
    video->page_shadow_valid[1] = 1;
    video->page_shadow_valid[0] = 0;
#endif
    /* Entering 8bpp gameplay means the title/menu/load/ending VDC overlay is
       gone for good.  Force the overlay *intent* back to OFF here, not just the
       applied state: otherwise a later RAINBOW story-map fade re-runs
       pcfx_vdc_overlay_flush() at fade level 0 with the stale MENU mode and
       re-prints "SELECT MODE / STORY MODE / ..." into the VDC BAT, leaking the
       title menu text over the pyramid map (the VDC remnant glitch). */
    g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF;
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

void waifu_pcfx_video_request_rainbow_backdrop(WaifuPcfxSanctumBackdrop backdrop)
{
    g_rainbow_backdrop = backdrop;
    g_rainbow_backdrop_requested = 1;
    g_rainbow_hscroll = 0;
    g_rainbow_hscroll_requested = 1;
}

void waifu_pcfx_video_request_rainbow_hscroll(int hscroll)
{
    g_rainbow_hscroll = hscroll & 0x01ff;
    g_rainbow_hscroll_requested = 1;
}

/* Platform background seam: present the requested scene background on the
   hardware RAINBOW layer. Returns 1 so the common scene code leaves the
   framebuffer background transparent for the RAINBOW layer to show through. */
/* No widescreen HUD room on this target: keep the fixed 2D UI layout. */
int waifu_platform_ui_extra_w(void) { return 0; }
int waifu_platform_arena_backdrop(void) { return 0; }
int waifu_platform_arena_backdrop_band(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; return 0; }
void waifu_platform_ui_hud(int on) { (void)on; }
int waifu_platform_performance_tier(void) { return 0; }
int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch, unsigned char fg, unsigned char shadow)
{ (void)x; (void)y; (void)cell_w; (void)ch; (void)fg; (void)shadow; return 0; }

/* Do the ending image's blocking CD->KRAM DMA now, before the ending scene's
   typewriter starts. Called at the end of the reward->ending black fade, so the
   read is hidden behind the black frame instead of freezing a few characters
   into the narration. Mirrors the upload the first ending present would do. */
void waifu_platform_prewarm_ending(void)
{
    pcfx_present_ending_16m(&g_video, 0);
}

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    WaifuPcfxSanctumBackdrop backdrop;
    switch (kind) {
    case WAIFU_BACKGROUND_STONE: backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_STONE; break;
    case WAIFU_BACKGROUND_EMBER: backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_EMBER; break;
    case WAIFU_BACKGROUND_SKY:   backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_SKY;   break;
    default:                     backdrop = WAIFU_PCFX_SANCTUM_BACKDROP_DESERT; break;
    }
    waifu_pcfx_video_request_rainbow_backdrop(backdrop);
    waifu_pcfx_video_request_rainbow_hscroll(hscroll);
    return 1;
}

/* Platform text seam: present whole-screen UI panels on the VDC hardware text
   overlay. Returns 1 (handled in hardware) so the common code skips its
   software text fallback. */
int waifu_platform_text_overlay(WaifuTextOverlayKind kind, const WaifuTextOverlayParams *params)
{
    WaifuTextOverlayParams empty = {0};
    const WaifuTextOverlayParams *p = params ? params : &empty;
    switch (kind) {
    case WAIFU_TEXT_OVERLAY_TITLE_PROMPT:
        waifu_pcfx_video_overlay_title_prompt(p->prompt_visible, p->has_save);
        return 1;
    case WAIFU_TEXT_OVERLAY_MENU:
        waifu_pcfx_video_overlay_menu(p->selected, p->has_save);
        return 1;
    case WAIFU_TEXT_OVERLAY_LOAD_MENU:
        waifu_pcfx_video_overlay_load_menu(p->selected, p->internal_has_save, p->external_has_save);
        return 1;
    case WAIFU_TEXT_OVERLAY_ENDING_STORY:
        waifu_pcfx_video_overlay_ending_story(p->name, p->page, p->prompt_visible, p->visible_chars);
        return 1;
    case WAIFU_TEXT_OVERLAY_ENDING_CREDITS:
        waifu_pcfx_video_overlay_ending_credits();
        return 1;
    }
    return 0;
}

void waifu_platform_text_overlay_clear(void)
{
    waifu_pcfx_video_overlay_clear();
}

int waifu_platform_text_overlay_is_hardware(void)
{
    return 1;
}

void waifu_platform_story_layers_begin(void)
{
    g_story_portrait_count = 0;
}

int waifu_platform_story_portrait(int portrait_id, int x, int y)
{
    if (portrait_id < 0 || portrait_id >= WAIFU_STORY_PORTRAIT_COUNT ||
        g_story_portrait_count >= WAIFU_PCFX_STORY_SPR_MAX) return 0;
    g_story_portrait_request[g_story_portrait_count].id = portrait_id;
    g_story_portrait_request[g_story_portrait_count].x = x;
    g_story_portrait_request[g_story_portrait_count].y = y;
    ++g_story_portrait_count;
    return 1;
}

void waifu_pcfx_video_request_sanctum(WaifuPcfxSanctumBackdrop backdrop, WaifuPcfxSanctumOverlay overlay, int value, int blink_visible)
{
    g_sanctum_backdrop = backdrop;
    g_sanctum_overlay = overlay;
    g_sanctum_value = value;
    g_sanctum_blink_visible = blink_visible ? 1 : 0;
    g_sanctum_requested = 1;
}

static int fade_q8_to_level(int fade_q8)
{
    return clamp_int((fade_q8 * (WAIFU_PCFX_FADE_LEVELS - 1) + 128) >> 8, 0, WAIFU_PCFX_FADE_LEVELS - 1);
}

static int pcfx_palette_table_index(WaifuFmPaletteId palette_id)
{
    if (palette_id == WAIFU_FM_PALETTE_TITLE) return 1;
    if (palette_id == WAIFU_FM_PALETTE_DIALOGUE) return 2;
    return 0;
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

    if (palette_id == WAIFU_FM_PALETTE_COMMON ||
        palette_id == WAIFU_FM_PALETTE_TITLE ||
        palette_id == WAIFU_FM_PALETTE_DIALOGUE) {
        const uint16_t *row = waifu_pcfx_palette_fade_lut[pcfx_palette_table_index(palette_id)][level];
        for (int i = 0; i < 256; ++i) {
            uint16_t yuv = row[i];
            pcfx_vce_set_palette((uint16_t)i, yuv);
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
            pcfx_vce_set_palette((uint16_t)i, yuv);
        }
        video->have_base_yuv = 1;
    }

    pcfx_vce_set_palette((uint16_t)IDX_BLACK, WAIFU_PCFX_NEUTRAL_BLACK);
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
    /* Blacken both 8bpp pages (words 0 .. 0x1FFFF, one contiguous run that stays
       within KRAM bank 0). */
    king_kram_fill_words(WAIFU_PCFX_BLACK_WORD, WAIFU_PCFX_PAGE_STRIDE_WORDS * WAIFU_PCFX_PAGE_COUNT);
    video->front_page = 0;
    video->back_page = 1;
    video->pending_page = -1;
    video->have_last_frame = 0;
    video->pending_title_page_flip = 0;
    video->title16m_page_valid[0] = 0;
    video->title16m_page_valid[1] = 0;
#if WAIFU_PCFX_DIRTY_PRESENT
    for (int p = 0; p < WAIFU_PCFX_PAGE_COUNT; ++p) {
        memset(video->page_shadow[p], IDX_BLACK, WAIFU_PCFX_FRAME_BYTES);
        video->page_shadow_valid[p] = 1;
    }
#endif
    pcfx_king_set_8bpp_display_page(video->front_page);
}

#if WAIFU_PCFX_DIRTY_PRESENT
static WAIFU_PCFX_COLD void pcfx_present_full_upload(WaifuPcfxVideo *video, const uint8_t *framebuffer, uint8_t *shadow)
{
    pcfx_kram_write_frame_affine(framebuffer, page_word_offset(video->back_page));
    pcfx_copy_bytes_inline(shadow, framebuffer, WAIFU_PCFX_FRAME_BYTES);
    video->page_shadow_valid[video->back_page] = 1;
}
#endif

static WAIFU_PCFX_COLD void pcfx_present_update_palette_if_needed(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    waifu_pcfx_video_set_palette_rgb_fade(video, rgb, palette_id, fade_q8);
}

/* Commit the 8bpp page a present just finished rendering into (video->back_page).
   Under RAINBOW the overlay is single-page and latches immediately.  Otherwise
   this is the double-buffer commit: the drawn page becomes PENDING and its
   display flip is deferred to the tear-safe vblank window (see
   waifu_pcfx_video_wait_vblank), so the CG base never changes mid-scan.  The
   draw target becomes the page on screen right now; wait_vblank runs before the
   next present (pcfx_main's loop is present -> wait_vblank), retires the pending
   flip and thereby frees it, so no frame ever renders into a live buffer. */
static inline __attribute__((always_inline)) void pcfx_commit_8bpp_page(WaifuPcfxVideo *video)
{
    if (g_rainbow_backdrop_active) {
        pcfx_king_set_8bpp_display_page(video->back_page);
        video->front_page = video->back_page;
        video->pending_page = -1;
    } else {
        video->pending_page = video->back_page;
        video->back_page = video->front_page;
    }
    video->have_last_frame = 1;
}

void waifu_pcfx_video_present_8bpp(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb, WaifuFmPaletteId palette_id)
{
    int applied_rainbow_backdrop = 0;
    if (!video || !framebuffer) return;
    if (palette_id == WAIFU_FM_PALETTE_TITLE) {
        pcfx_present_title_16m(video, framebuffer);
        return;
    }
    if (palette_id == WAIFU_FM_PALETTE_ENDING) {
        pcfx_present_ending_16m(video, 0);
        return;
    }
    if (palette_id == WAIFU_FM_PALETTE_ENDING_BLACK) {
        pcfx_present_ending_16m(video, 1);
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
    if (g_sanctum_requested) {
        pcfx_vdc_apply_sanctum(video, g_sanctum_backdrop, g_sanctum_overlay,
                               g_sanctum_value, g_sanctum_blink_visible);
        g_sanctum_requested = 0;
        g_rainbow_backdrop_requested = 0;
        g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;
        video->have_last_frame = 0;
        return;
    }
    if (g_rainbow_backdrop_requested) {
        pcfx_apply_rainbow_backdrop(video, g_rainbow_backdrop);
        g_rainbow_backdrop_requested = 0;
        g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;
        applied_rainbow_backdrop = 1;
    } else if (g_rainbow_backdrop_active) {
        pcfx_vdc_clear_background(video);
    }
    if (!applied_rainbow_backdrop) {
        pcfx_vdc_apply_requested_background(video, g_vdc_bg_requested);
    }
    g_vdc_bg_requested = WAIFU_PCFX_VDC_BG_NONE;

    if (g_rainbow_backdrop_active) {
        pcfx_vdc_overlay_set_fade_q8(fade_q8);
        video->front_page = 0;
        video->back_page = 0;
        pcfx_king_set_bg_kram_page_inline(0);
    }

#if WAIFU_PCFX_DIRTY_PRESENT
    uint8_t *shadow = video->page_shadow[video->back_page];
    if (!video->page_shadow_valid[video->back_page]) {
        pcfx_present_full_upload(video, framebuffer, shadow);
    } else {
        if (pcfx_present_dirty_rows(shadow, framebuffer,
                                    page_word_offset(video->back_page)) == 0) {
            /* The hidden page already holds this exact frame; commit it. */
            pcfx_commit_8bpp_page(video);
            return;
        }
    }
#else
    uint32_t frame_sum;
    uint32_t frame_mix;
    pcfx_frame_signature(framebuffer, &frame_sum, &frame_mix);
    if (video->have_last_frame && video->last_frame_sum == frame_sum && video->last_frame_mix == frame_mix) {
        return;
    }

    pcfx_kram_write_frame_affine(framebuffer, page_word_offset(video->back_page));
#endif

#if !WAIFU_PCFX_DIRTY_PRESENT
    video->last_frame_sum = frame_sum;
    video->last_frame_mix = frame_mix;
#endif
    pcfx_commit_8bpp_page(video);
}

void waifu_pcfx_video_present_title_hicolor_stub(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb)
{
    (void)framebuffer;
    (void)rgb;
    pcfx_present_title_16m(video, framebuffer);
}

/* Read the HuC6261 (TETSU/VCE) raster line counter with a stable double-read.
   A lone read can latch a bogus transitional value on real hardware, which would
   flip the vblank predicate at the wrong scanline; require two agreeing reads
   (bounded retries).  pcfxemu returns a clean value, so this is a no-op there. */
static int pcfx_tetsu_raster_stable(void)
{
    int a = tetsu_get_raster();
    for (int tries = 0; tries < 8; ++tries) {
        int b = tetsu_get_raster();
        if (a == b) return a;
        a = b;
    }
    return a;
}

void waifu_pcfx_video_wait_vblank(WaifuPcfxVideo *video)
{
    /* Time fields by polling the TETSU raster, NOT the VDC VD-status latch at
       0x80000400.  That bit only latches cleanly under pcfxemu; on real hardware
       it behaves differently, and with all interrupts disabled (see main) the
       BIOS no longer re-arms it -- the old poll then spun FOREVER, leaving the
       KING BG black.  C6261 SVB=262 and EVB=22, so blanking is raster 262 then
       0..21; 240..258 is the bottom of active display.  Every spin is bounded
       so a misbehaving VCE degrades to slow, never hangs.  No VDC vblank IRQ
       is armed. */
    uint32_t spin;
    unsigned raster;
    /* Keep VDC0's BG + sprite planes enabled (story portraits are combined
       256-colour sprites split across VDC0/VDC1) WITHOUT the vblank IRQ-enable
       bit -- interrupts are off, nothing services it. */
    waifu_vdc_setreg(VDC_CHIP_0, VDC_REG_CR, VDC_CR_BB | VDC_CR_SB);
    /* RAINBOW finishes decoding near the bottom of active display.  Re-arm it
       there, before entering real vblank; moving this into blanking aborts the
       progressive transfer before the field is complete. */
    if (g_rainbow_backdrop_active || g_sanctum_active) {
        spin = 0;
        raster = (unsigned)pcfx_tetsu_raster_stable();
        while (raster < 248u && spin++ < 2000000u)
            raster = (unsigned)pcfx_tetsu_raster_stable();
        if (raster >= 248u && raster < WAIFU_PCFX_VBLANK_SVB)
            pcfx_rainbow_start_transfer();
    }
    /* Wait for the real vertical blanking window -- with enough of it left to
       hold the flip, the palette flush and the VDC staging below.  Entering
       this call while blanking has nearly run out (a present that finished
       early) used to pass the plain blanking test and then spray palette RAM
       across the first live rasters. */
    spin = 0;
    while (!pcfx_blank_window_has_room((unsigned)pcfx_tetsu_raster_stable()) &&
           spin++ < 2000000u) { }
    /* Flip first: KING REG.0F is vblank-only and the CG/affine registers affect
       scanout immediately, so they have the shortest deadline in the window.
       Palette and VDC work follow only after the displayed page is coherent. */
    if (video && video->pending_title_page_flip) {
        pcfx_king_set_bg_kram_page_inline(0);
        pcfx_king_set_bg0_page_inline(title16m_bg_cg_page(video->pending_title_kram_page));
        video->front_page = video->pending_title_front_page;
        video->back_page = video->pending_title_back_page;
        video->pending_title_page_flip = 0;
    } else if (video && video->mode == WAIFU_PCFX_VIDEO_MODE_KING_8BPP && video->pending_page >= 0) {
        /* Deferred double-buffer flip: latch the page the last present rendered
           now, inside the vblank window, so the CG-base change is never seen
           mid-scan (tear-free).  This also frees the page that was on screen,
           which the next present draws into.  RAINBOW never sets pending_page
           (it flips in-line). */
        pcfx_king_set_8bpp_display_page(video->pending_page);
        video->front_page = video->pending_page;
        video->pending_page = -1;
    }
    if (video && video->mode == WAIFU_PCFX_VIDEO_MODE_KING_8BPP) {
        /* Must be inside the vblank window: coefficients/centre are live KING
           state and are refreshed every field for reliable affine BG0 output. */
        pcfx_king_refresh_8bpp_affine_state();
    }
    /* C6261 2.1.3 (5) says palette RAM writes during display show noise.
       Flushing after the short page flip keeps the burst in real blanking while
       still landing the palette in the same field as its framebuffer. */
    pcfx_vce_palette_flush();
    if (video) {
        pcfx_vdc_overlay_flush(video);
        /* Overlay flush may rebuild the VDC BAT for a fade.  Story HUD and
         * sprites are staged afterwards, inside vblank, so both VDCs latch a
         * complete paired SAT on the next field. */
        pcfx_story_layers_flush(video);
    }
    /* Wait out the vblank window so the next call catches the following field. */
    spin = 0;
    while (pcfx_raster_in_vblank((unsigned)pcfx_tetsu_raster_stable()) &&
           spin++ < 2000000u) { }
    if (video && video->mode == WAIFU_PCFX_VIDEO_MODE_KING_8BPP && video->vdc_overlay_shutdown_countdown > 0) {
        --video->vdc_overlay_shutdown_countdown;
        if (video->vdc_overlay_shutdown_countdown == 0) pcfx_vdc_overlay_shutdown(video);
    }
}
