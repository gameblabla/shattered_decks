/* Sega CD 32X video backend.
 *
 * This target intentionally exposes only the generic game framebuffer seam to
 * common code.  Platform-specific work stays here: 32X VDP setup, CRAM upload,
 * and 320x224 NTSC 8bpp framebuffer presentation. */
#include "waifu_cd32x_video.h"
#include "cd32x_32x.h"
#include "assets.h"
#include "platform.h"
#include "waifu_assets.h"
#include "cd32x_title_asset.h"
#include "font_menudata.h"

#include <stdint.h>
#include <string.h>

#define WAIFU_CD32X_W WAIFU_FM_WIDTH
#define WAIFU_CD32X_H WAIFU_FM_HEIGHT

#define WAIFU_CD32X_LINE_TABLE_WORDS 0x100
#define WAIFU_CD32X_FB_BYTE_OFFSET ((int)WAIFU_CD32X_FRAMEBUFFER_LINE_TABLE_BYTES)
#define WAIFU_CD32X_TITLE_BYTES (CD32X_TITLE_SCREEN_W * CD32X_TITLE_SCREEN_H)
#define WAIFU_CD32X_TITLE_WORDS (WAIFU_CD32X_TITLE_BYTES / 2)
#define WAIFU_CD32X_TITLE_XOFF ((WAIFU_CD32X_W - CD32X_TITLE_SCREEN_W) / 2)

#define CD32X_COMM_READY        0x0001u
#define CD32X_MD_CMD_SET_FADE   0xCD02u
#define CD32X_MD_CMD_SET_BG     0xCD0Au


#if WAIFU_CD32X_W != 320 || WAIFU_CD32X_H != 224
#error "CD32X video backend expects WAIFU_FM_WIDTH=320 and WAIFU_FM_HEIGHT=224"
#endif
#if CD32X_TITLE_SCREEN_W != 320 || CD32X_TITLE_SCREEN_H != 224
#error "CD32X title asset must be 320x224"
#endif

struct WaifuCd32xVideo {
    uint16_t current_fb;
    WaifuFmPaletteId current_palette_id;
    int current_fade_q8;
    int current_md_fade_q8;
    /* Frame-pacing probe: a single last-frame vblank delta (1 == 60 fps,
       2 == 30 fps).  Originally debug-overlay-only; now always compiled
       because the game core consumes it via waifu_fm_set_frame_vblanks() to
       advance battle animations by real hardware time.  Still just ~8 bytes
       of state and a COMM12 read per flip.  The previous 32-entry ring-buffer
       average was ~100 bytes of debug-only code that pushed the autobattle
       debug build past the 128 KiB BlastEm SH2 staging limit and broke
       boot. */
    uint32_t last_flip_vblank;
    uint8_t last_frame_vblanks;
};

static WaifuCd32xVideo g_video;
static int g_cd32x_overlay_kind = -1;
static int g_cd32x_overlay_base_pages_remaining = 0;
static int g_cd32x_prompt_visible = -1;
static int g_cd32x_prompt_pages_remaining = 0;
static int g_cd32x_menu_selected = -1;
static int g_cd32x_menu_has_save = -1;
static int g_cd32x_menu_pages_remaining = 0;

static void cd32x_put_px_back(int x, int y, uint8_t c);
static void cd32x_fill_rect_back(int x, int y, int w, int h, uint8_t c);
static void cd32x_background_frame_end(void);

static inline void cd32x_copy_pairs_to_back(volatile uint16_t *dst, const uint8_t *src, int pairs)
{
    while (pairs >= 8) {
        dst[0] = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
        dst[1] = (uint16_t)(((uint16_t)src[2] << 8) | src[3]);
        dst[2] = (uint16_t)(((uint16_t)src[4] << 8) | src[5]);
        dst[3] = (uint16_t)(((uint16_t)src[6] << 8) | src[7]);
        dst[4] = (uint16_t)(((uint16_t)src[8] << 8) | src[9]);
        dst[5] = (uint16_t)(((uint16_t)src[10] << 8) | src[11]);
        dst[6] = (uint16_t)(((uint16_t)src[12] << 8) | src[13]);
        dst[7] = (uint16_t)(((uint16_t)src[14] << 8) | src[15]);
        src += 16;
        dst += 8;
        pairs -= 8;
    }
    while (pairs-- > 0) {
        *dst++ = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
        src += 2;
    }
}

static int cd32x_request_md_palette_fade(int fade_q8)
{
    if (fade_q8 < 0) fade_q8 = 0;
    if (fade_q8 > 256) fade_q8 = 256;

    /* The resident Sega-CD supervisor owns the MD VDP.  Fade requests are
       intentionally opportunistic: never steal the COMM registers while a CD
       transfer is in progress.  A skipped MD-palette step is visually harmless
       and will be retried on a later palette update. */
    if (MARS_SYS_COMM0 != 0u) return 0;
    MARS_SYS_COMM2 = (uint16_t)fade_q8;
    MARS_SYS_COMM4 = CD32X_MD_CMD_SET_FADE;
    MARS_SYS_COMM0 = CD32X_COMM_READY;
    return 1;
}

static void cd32x_wait_fill_done(void)
{
    while (MARS_VDP_FBCTL & MARS_VDP_FEN) {
    }
}

static void cd32x_auto_fill_back_words(uint16_t start_word, int words, uint16_t value)
{
    while (words > 0) {
        int page_left = 256 - ((int)start_word & 255);
        int run = words < page_left ? words : page_left;
        if (run > 256) run = 256;
        cd32x_wait_fill_done();
        MARS_VDP_FILLEN = (uint16_t)(run - 1);
        MARS_VDP_FILADR = start_word;
        MARS_VDP_FILDAT = value;
        start_word = (uint16_t)(start_word + run);
        words -= run;
    }
    cd32x_wait_fill_done();
}

static uint32_t cd32x_vblank_count(void)
{
    return (uint32_t)MARS_SYS_COMM12;
}

static void cd32x_record_frame_pacing(WaifuCd32xVideo *video)
{
    uint32_t now;
    uint32_t delta;
    if (!video) return;
    now = cd32x_vblank_count();
    delta = video->last_flip_vblank ? (now - video->last_flip_vblank) : 1u;
    if (delta == 0u) delta = 1u;
    if (delta > 255u) delta = 255u;
    video->last_flip_vblank = now;
    video->last_frame_vblanks = (uint8_t)delta;
}

int waifu_cd32x_video_last_frame_vblanks(const WaifuCd32xVideo *video)
{
    if (!video || video->last_frame_vblanks == 0u) return 1;
    return (int)video->last_frame_vblanks;
}

static uint16_t cd32x_rgb_to_cram(uint8_t r, uint8_t g, uint8_t b, int fade_q8)
{
    unsigned rr = (unsigned)((r * fade_q8) >> 8);
    unsigned gg = (unsigned)((g * fade_q8) >> 8);
    unsigned bb = (unsigned)((b * fade_q8) >> 8);
    if (rr > 255u) rr = 255u;
    if (gg > 255u) gg = 255u;
    if (bb > 255u) bb = 255u;
    return (uint16_t)COLOR(rr >> 3, gg >> 3, bb >> 3);
}

/* 32X palette RAM may only be written while the display is not drawing:
   the VDP arbitrates CRAM in favour of the raster, so a CPU write landing in
   active display is dropped or shows up as a bright "CRAM dot" on the scanline
   it hit.  All palette updates therefore go into this RAM shadow first and are
   copied out in one burst inside vertical blank (cd32x_cram_flush_vblank).
   The shadow lives in .bss, so it costs nothing in the staged SH-2 image. */
static uint16_t g_cram_shadow[256];
static uint8_t g_cram_dirty;

static void cd32x_cram_flush_vblank(void)
{
    volatile uint16_t *cram = &MARS_CRAM;
    const uint16_t *src = g_cram_shadow;
    int i;
    if (!g_cram_dirty) return;
    /* Callers reach here right after a framebuffer flip (which the VDP honours
       at the start of vblank), so this spin normally falls straight through.
       Keep it anyway: it is the only thing guaranteeing the burst below cannot
       start mid-raster.  256 words is ~0.5 ms of the ~2.4 ms NTSC vblank. */
    while ((MARS_VDP_FBCTL & MARS_VDP_VBLK) == 0) {
    }
    for (i = 0; i < 256; ++i) cram[i] = src[i];
    g_cram_dirty = 0;
}

static void cd32x_wait_fb_flip(WaifuCd32xVideo *video)
{
    MARS_VDP_FBCTL = (uint16_t)(video->current_fb ^ 1u);
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == video->current_fb) {
    }
    video->current_fb ^= 1u;
    /* Still inside the vblank that performed the flip: this is the one window
       where CRAM is safe to touch, and it keeps the new palette in step with
       the page it belongs to. */
    cd32x_cram_flush_vblank();
    cd32x_record_frame_pacing(video);
}

static void cd32x_write_back_line_table(void)
{
    /* Blastem's 32x_video.c is the ground truth here: framebuffer aperture
       writes always target video->back, and changing FS swaps front/back.  The
       0x24020000 aperture is overwrite mode for that same back page, not a
       CPU-addressable second page.  Therefore each page must receive its line
       table while it is the current back page, before requesting an FS flip.
       table while it is the current back page, before requesting an FS flip. */
    volatile uint16_t *fb16 = &MARS_FRAMEBUFFER;
    for (int y = 0; y < WAIFU_CD32X_H; ++y) {
        fb16[y] = (uint16_t)(WAIFU_CD32X_LINE_TABLE_WORDS + y * (WAIFU_CD32X_W / 2));
    }
}

static void cd32x_clear_back_pixels(uint8_t c)
{
    uint16_t pair = (uint16_t)(((uint16_t)c << 8) | c);
    cd32x_auto_fill_back_words((uint16_t)WAIFU_CD32X_LINE_TABLE_WORDS,
                               (WAIFU_CD32X_W * WAIFU_CD32X_H) / 2,
                               pair);
}

static void cd32x_restore_title_rect_back(int x, int y, int w, int h)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    int tw = CD32X_TITLE_SCREEN_W;
    int th = CD32X_TITLE_SCREEN_H;
    const uint8_t *title = waifu_assets_title_screen_img();

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WAIFU_CD32X_W) x1 = WAIFU_CD32X_W;
    if (y1 > WAIFU_CD32X_H) y1 = WAIFU_CD32X_H;
    if (x0 >= x1 || y0 >= y1) return;

    waifu_assets_title_screen_dims(&tw, &th);
    if (!title || tw != WAIFU_CD32X_W || th < WAIFU_CD32X_H) {
        cd32x_fill_rect_back(x0, y0, x1 - x0, y1 - y0, IDX_BLACK);
        return;
    }

    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t *src = title + (uint32_t)yy * (uint32_t)tw + x0;
        int xx = x0;
        if (xx & 1) {
            cd32x_put_px_back(xx, yy, *src++);
            ++xx;
        }
        volatile uint16_t *dst = &MARS_FRAMEBUFFER + ((WAIFU_CD32X_FB_BYTE_OFFSET + yy * WAIFU_CD32X_W + xx) >> 1);
        {
            int pairs = (x1 - xx) >> 1;
            cd32x_copy_pairs_to_back(dst, src, pairs);
            src += pairs << 1;
            xx += pairs << 1;
        }
        if (xx < x1) cd32x_put_px_back(xx, yy, *src);
    }
}

static void cd32x_init_framebuffers(WaifuCd32xVideo *video)
{
    /* Clear both boot pages to IDX_BLACK (opaque black), NOT index 0.  Index 0
       is the MD-priority "see-through" key: with MARS_VDP_PRIO_32X an index-0
       pixel defers to the MD layer, so a page left at index 0 shows the MD
       backdrop (and, once the game's first palette upload forwards the near-
       white common-palette entry 0 to the MD fade before the first real frame
       has page-flipped in, that backdrop is briefly near-white).  That was the
       ~2-frame white flash between the Sega/32X BIOS and the LOADING screen.
       IDX_BLACK maps to an opaque black CRAM entry with no priority bit, so the
       boot pages stay black regardless of the MD layer until the game draws. */
    video->current_fb = (uint16_t)(MARS_VDP_FBCTL & MARS_VDP_FS);
    cd32x_write_back_line_table();
    cd32x_clear_back_pixels(IDX_BLACK);
    cd32x_wait_fb_flip(video);
    cd32x_write_back_line_table();
    cd32x_clear_back_pixels(IDX_BLACK);
}

static void cd32x_put_px_back(int x, int y, uint8_t c)
{
    volatile uint8_t *pixels;
    if ((unsigned)x >= WAIFU_CD32X_W || (unsigned)y >= WAIFU_CD32X_H) return;
    pixels = (volatile uint8_t *)(uintptr_t)WAIFU_CD32X_FRAMEBUFFER_PIXELS;
    pixels[(uint32_t)y * WAIFU_CD32X_W + (uint32_t)x] = c;
}

static void cd32x_fill_rect_back(int x, int y, int w, int h, uint8_t c)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    uint16_t pair = (uint16_t)(((uint16_t)c << 8) | c);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WAIFU_CD32X_W) x1 = WAIFU_CD32X_W;
    if (y1 > WAIFU_CD32X_H) y1 = WAIFU_CD32X_H;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        int xx = x0;
        if (xx & 1) {
            cd32x_put_px_back(xx, yy, c);
            ++xx;
        }
        volatile uint16_t *dst = &MARS_FRAMEBUFFER + ((WAIFU_CD32X_FB_BYTE_OFFSET + yy * WAIFU_CD32X_W + xx) >> 1);
        while (xx + 1 < x1) {
            *dst++ = pair;
            xx += 2;
        }
        if (xx < x1) cd32x_put_px_back(xx, yy, c);
    }
}

static uint8_t cd32x_font_row(unsigned char ch, int row)
{
    if (row < 0 || row >= 8) return 0;
    return n2DLib_font[((uint32_t)ch * 8u) + (uint32_t)row];
}

static uint8_t cd32x_outline_row(unsigned char ch, int row)
{
    uint8_t center = cd32x_font_row(ch, row);
    uint8_t above = cd32x_font_row(ch, row - 1);
    uint8_t below = cd32x_font_row(ch, row + 1);
    uint8_t neigh = (uint8_t)(above | below |
                              (uint8_t)(center << 1) | (uint8_t)(center >> 1) |
                              (uint8_t)(above << 1) | (uint8_t)(above >> 1) |
                              (uint8_t)(below << 1) | (uint8_t)(below >> 1));
    return (uint8_t)(neigh & (uint8_t)~center);
}

static void cd32x_draw_char_scaled_both(int x, int y, char ch, int scale, uint8_t fg, uint8_t outline)
{
    unsigned char uch = (unsigned char)ch;
    if (scale == 1) {
        volatile uint8_t *pixels = (volatile uint8_t *)(uintptr_t)WAIFU_CD32X_FRAMEBUFFER_PIXELS;
        for (int yy = 0; yy < 8; ++yy) {
            int py = y + yy;
            uint8_t row = cd32x_outline_row(uch, yy);
            if ((unsigned)py >= WAIFU_CD32X_H) continue;
            for (int xx = 0; xx < 8; ++xx) {
                int px = x + xx;
                if ((row & (uint8_t)(0x80u >> xx)) && (unsigned)px < WAIFU_CD32X_W) {
                    pixels[(uint32_t)py * WAIFU_CD32X_W + (uint32_t)px] = outline;
                }
            }
        }
        for (int yy = 0; yy < 8; ++yy) {
            int py = y + yy;
            uint8_t row = cd32x_font_row(uch, yy);
            if ((unsigned)py >= WAIFU_CD32X_H) continue;
            for (int xx = 0; xx < 8; ++xx) {
                int px = x + xx;
                if ((row & (uint8_t)(0x80u >> xx)) && (unsigned)px < WAIFU_CD32X_W) {
                    pixels[(uint32_t)py * WAIFU_CD32X_W + (uint32_t)px] = fg;
                }
            }
        }
        return;
    }

    /* Match the PC-FX title overlay semantics: background pixels are
       transparent, with only a one-glyph-pixel black outline and the glyph
       foreground drawn over the already-streamed title bitmap.  This avoids
       the old solid black 8x8 cell fill around every character. */
    for (int yy = 0; yy < 8; ++yy) {
        uint8_t row = cd32x_outline_row(uch, yy);
        for (int xx = 0; xx < 8; ++xx) {
            if (row & (uint8_t)(0x80u >> xx)) {
                cd32x_fill_rect_back(x + xx * scale, y + yy * scale, scale, scale, outline);
            }
        }
    }
    for (int yy = 0; yy < 8; ++yy) {
        uint8_t row = cd32x_font_row(uch, yy);
        for (int xx = 0; xx < 8; ++xx) {
            if (row & (uint8_t)(0x80u >> xx)) {
                cd32x_fill_rect_back(x + xx * scale, y + yy * scale, scale, scale, fg);
            }
        }
    }
}

static void cd32x_draw_text_scaled_both(int x, int y, const char *text, int scale, uint8_t fg, uint8_t outline)
{
    while (*text) {
        cd32x_draw_char_scaled_both(x, y, *text++, scale, fg, outline);
        x += 8 * scale;
    }
}

static void cd32x_draw_text_centered_both(int y, const char *text, int scale, uint8_t fg, uint8_t bg)
{
    int len = 0;
    const char *p = text;
    while (*p++) ++len;
    cd32x_draw_text_scaled_both((WAIFU_CD32X_W - len * 8 * scale) / 2, y, text, scale, fg, bg);
}

static char *cd32x_append_u8(char *p, unsigned v)
{
    if (v >= 100u) {
        *p++ = (char)('0' + (v / 100u));
        v %= 100u;
        *p++ = (char)('0' + (v / 10u));
        *p++ = (char)('0' + (v % 10u));
    } else if (v >= 10u) {
        *p++ = (char)('0' + (v / 10u));
        *p++ = (char)('0' + (v % 10u));
    } else {
        *p++ = (char)('0' + v);
    }
    return p;
}

static void cd32x_draw_panel_rect_both(int x, int y, int w, int h, uint8_t fill)
{
    cd32x_fill_rect_back(x, y, w, h, fill);
    cd32x_fill_rect_back(x, y, w, 1, IDX_WHITE);
    cd32x_fill_rect_back(x, y + h - 1, w, 1, IDX_BLACK);
    cd32x_fill_rect_back(x, y, 1, h, IDX_WHITE);
    cd32x_fill_rect_back(x + w - 1, y, 1, h, IDX_BLACK);
}

static void cd32x_draw_title_logo_both(void)
{
    cd32x_draw_text_centered_both(20, "SHATTERED", 2, IDX_GOLD_HI, IDX_BLACK);
    cd32x_draw_text_centered_both(38, "DECKS", 2, IDX_WHITE, IDX_BLACK);
}

static void cd32x_overlay_begin(WaifuTextOverlayKind kind)
{
    if (g_cd32x_overlay_kind != (int)kind) {
        g_cd32x_overlay_kind = (int)kind;
        g_cd32x_overlay_base_pages_remaining = 2;
        g_cd32x_prompt_visible = -1;
        g_cd32x_prompt_pages_remaining = 0;
        g_cd32x_menu_selected = -1;
        g_cd32x_menu_has_save = -1;
        g_cd32x_menu_pages_remaining = 0;
    }
}

static int cd32x_restore_title_base_if_needed(void)
{
    if (g_cd32x_overlay_base_pages_remaining <= 0) return 0;
    cd32x_restore_title_rect_back(0, 0, WAIFU_CD32X_W, WAIFU_CD32X_H);
    cd32x_draw_title_logo_both();
    --g_cd32x_overlay_base_pages_remaining;
    return 1;
}

static void cd32x_draw_title_prompt_both(int prompt_visible, int has_save)
{
    int restored_base;
    (void)has_save;
    cd32x_overlay_begin(WAIFU_TEXT_OVERLAY_TITLE_PROMPT);
    if (prompt_visible != g_cd32x_prompt_visible) {
        g_cd32x_prompt_visible = prompt_visible;
        g_cd32x_prompt_pages_remaining = 2;
    }
    restored_base = cd32x_restore_title_base_if_needed();
    if (!restored_base && g_cd32x_prompt_pages_remaining <= 0) return;

    cd32x_restore_title_rect_back(0, 184, WAIFU_CD32X_W, 36);
    if (prompt_visible) {
        cd32x_draw_text_centered_both(190, "PUSH START", 1, IDX_WHITE, IDX_BLACK);
    }
    cd32x_draw_text_centered_both(208, "(C) 2026 GAMEBLABLA", 1, IDX_WHITE, IDX_BLACK);
    if (g_cd32x_prompt_pages_remaining > 0) --g_cd32x_prompt_pages_remaining;
}

static void cd32x_draw_menu_both(int selected, int has_save)
{
    int ox = (WAIFU_CD32X_W - 256) / 2;
    const char *help = "RANDOM DECK / FREE DUEL";
    int restored_base;
    cd32x_overlay_begin(WAIFU_TEXT_OVERLAY_MENU);
    if (selected != g_cd32x_menu_selected || has_save != g_cd32x_menu_has_save) {
        g_cd32x_menu_selected = selected;
        g_cd32x_menu_has_save = has_save;
        g_cd32x_menu_pages_remaining = 2;
    }
    restored_base = cd32x_restore_title_base_if_needed();
    if (!restored_base && g_cd32x_menu_pages_remaining <= 0) return;

    cd32x_draw_title_logo_both();
    cd32x_fill_rect_back(ox + 39, 124, 178, 75, IDX_BLACK);
    cd32x_draw_panel_rect_both(ox + 41, 126, 174, 71, IDX_UI_DARK);
    cd32x_draw_text_scaled_both(ox + 72, 139, "STORY MODE", 1, selected == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    cd32x_draw_text_scaled_both(ox + 72, 159, "BATTLE MODE", 1, selected == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    cd32x_draw_text_scaled_both(ox + 72, 179, "LOAD STORY", 1, selected == 2 ? (has_save ? IDX_GOLD_HI : IDX_DIM) : (has_save ? IDX_WHITE : IDX_DIM), IDX_BLACK);
    int ay = selected == 0 ? 143 : (selected == 1 ? 163 : 183);
    for (int r = 0; r < 7; ++r) cd32x_fill_rect_back(ox + 54, ay - 3 + r, 1 + r, 1, IDX_RED);
    if (selected == 0) help = "ENTER NAME / FIRST DREAM";
    else if (selected == 2) help = has_save ? "RESUME SAVED STORY" : "NO SAVE FILE FOUND";
    cd32x_restore_title_rect_back(ox + 43, 204, 170, 14);
    // Glitchy mess
    //cd32x_draw_text_scaled_both(ox + 55, 207, help, 1, has_save || selected != 2 ? IDX_WHITE : IDX_RED, IDX_BLACK);
    if (g_cd32x_menu_pages_remaining > 0) --g_cd32x_menu_pages_remaining;
}

WaifuCd32xVideo *waifu_cd32x_video_create(void)
{
    memset(&g_video, 0, sizeof(g_video));
    while ((MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) == 0) {
    }

    /* Blacken 32X CRAM BEFORE enabling the display: DISPMODE may reset the
       VDP palette RAM on some hardware states, so we write CRAM both before
       and after the mode switch to guarantee no white-flash window between
       the overlay becoming active and the first game palette upload.  Entry 0
       keeps its MD-priority bit so index-0 pixels show the MD backdrop
       (black).
       The first write happens while the 32X bitmap mode is still OFF (the BIOS
       hands over with the display disabled), which is the one other state in
       which CRAM is writable; the second one is after the mode switch, so it
       goes through the vblank-gated shadow flush like every later update. */
    {
        volatile uint16_t *cram = &MARS_CRAM;
        int i;
        g_cram_shadow[0] = 0x8000u;
        for (i = 1; i < 256; ++i) g_cram_shadow[i] = 0;
        for (i = 0; i < 256; ++i) cram[i] = g_cram_shadow[i];
        g_cram_dirty = 0;
    }

    MARS_VDP_DISPMODE = (uint16_t)(MARS_224_LINES | MARS_VDP_MODE_256 | MARS_VDP_PRIO_32X);

    /* Re-blacken CRAM after DISPMODE in case the mode switch reset the palette. */
    g_cram_dirty = 1;
    cd32x_cram_flush_vblank();

    cd32x_init_framebuffers(&g_video);

    g_video.current_palette_id = (WaifuFmPaletteId)-1;
    g_video.current_fade_q8 = -1;
    g_video.current_md_fade_q8 = -1;
    return &g_video;
}

void waifu_cd32x_video_destroy(WaifuCd32xVideo *video)
{
    (void)video;
}

void waifu_cd32x_video_begin_8bpp(WaifuCd32xVideo *video)
{
    (void)video;
    g_cd32x_overlay_kind = -1;
    g_cd32x_overlay_base_pages_remaining = 0;
    g_cd32x_prompt_visible = -1;
    g_cd32x_prompt_pages_remaining = 0;
    g_cd32x_menu_selected = -1;
    g_cd32x_menu_has_save = -1;
    g_cd32x_menu_pages_remaining = 0;
    MARS_VDP_DISPMODE = (uint16_t)(MARS_240_LINES | MARS_VDP_MODE_256 | MARS_VDP_PRIO_32X);
}

void waifu_cd32x_video_set_palette_rgb(WaifuCd32xVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    uint16_t *cram = g_cram_shadow;
    int i;
    if (!video || !rgb) return;
    if (fade_q8 < 0) fade_q8 = 0;
    if (fade_q8 > 256) fade_q8 = 256;
    if (palette_id == video->current_palette_id && fade_q8 == video->current_fade_q8) return;
    /* Build the new palette in the RAM shadow only.  It reaches the hardware
       from cd32x_cram_flush_vblank() during the next framebuffer flip, i.e.
       inside vblank -- never during active display.
       Palette index 0 is the see-through key for the MD plane-B story sky:
       with MARS_VDP_PRIO_32X the 32X pixel wins unless its CRAM entry has the
       priority bit set, so flag entry 0 (and only entry 0) as MD-priority.
       Where the MD planes are also transparent this shows the MD backdrop
       (black), which matches the old index-0 behavior.  The bit MUST be part
       of the single entry-0 write: rewriting it bare and OR-ing the bit in
       after the 256-entry loop left a mid-display window where index-0
       pixels showed entry 0's raw color -- the common palette's entry 0 is
       near-white, which flashed the whole sky/boot screen every rewrite. */
    cram[0] = cd32x_rgb_to_cram(rgb[0], rgb[1], rgb[2], fade_q8) | 0x8000u;
    for (i = 1; i < 256; ++i) {
        cram[i] = cd32x_rgb_to_cram(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2], fade_q8);
    }
    g_cram_dirty = 1;
    {
        /* MD CRAM channels are 3-bit, so only ~8 fade levels are visible on
           the MD layer anyway: quantize the forwarded fade so a transition
           issues a handful of MD palette rewrites instead of one per frame.
           Each rewrite is held to vblank on the Main CPU (set_palette), so
           fewer bursts also means fewer chances to slip past vblank. */
        int md_fade = fade_q8 >= 256 ? 256 : (fade_q8 & ~31);
        if (md_fade != video->current_md_fade_q8 && cd32x_request_md_palette_fade(md_fade)) {
            video->current_md_fade_q8 = md_fade;
        }
    }
    video->current_palette_id = palette_id;
    video->current_fade_q8 = fade_q8;
}

void waifu_cd32x_video_clear_black(WaifuCd32xVideo *video)
{
    (void)video;
    cd32x_write_back_line_table();
    cd32x_clear_back_pixels(0);
}

void waifu_cd32x_video_clear_back_index(uint8_t c)
{
    cd32x_clear_back_pixels(c);
}

void waifu_cd32x_video_fill_rows_index(int y0, int y1, uint8_t c)
{
    /* Full-width scanline bands are contiguous in the back page, so the 32X
       VDP auto-fill can paint them without any SH-2 framebuffer stores.  Only
       the Master may call this: the fill registers are shared VDP state and
       the Slave render jobs must not race them. */
    if (y0 < 0) y0 = 0;
    if (y1 > WAIFU_CD32X_H) y1 = WAIFU_CD32X_H;
    if (y0 >= y1) return;
    cd32x_auto_fill_back_words((uint16_t)(WAIFU_CD32X_LINE_TABLE_WORDS + y0 * (WAIFU_CD32X_W / 2)),
                               (y1 - y0) * (WAIFU_CD32X_W / 2),
                               (uint16_t)(((uint16_t)c << 8) | c));
}

void waifu_cd32x_video_draw_debug_overlay(WaifuCd32xVideo *video)
{
    char buf[12];
    char *p;
    if (!video) return;
#if defined(CD32X_DEBUG_AUTOBATTLE) || defined(WAIFU_CD32X_DEBUG_FPS)
    p = buf;
    *p++ = 'V';
    *p++ = 'B';
    *p++ = ' ';
    p = cd32x_append_u8(p, video->last_frame_vblanks ? video->last_frame_vblanks : 1u);
    *p = '\0';
    cd32x_draw_text_scaled_both(122, 5, buf, 1, IDX_GOLD_HI, IDX_BLACK);
#else
    (void)buf;
    (void)p;
#endif
}

volatile uint8_t *waifu_cd32x_video_title_upload_buffer(void)
{
    return (volatile uint8_t *)(uintptr_t)WAIFU_CD32X_FRAMEBUFFER_PIXELS;
}

void waifu_cd32x_video_commit_title_upload(void)
{
    cd32x_write_back_line_table();
}

void waifu_cd32x_video_present_8bpp(WaifuCd32xVideo *video, const uint8_t *framebuffer, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    /* The 32X CPU-visible framebuffer window always targets the current back
       page.  Common CD32X rendering uses that window as its framebuffer, so the
       normal present path only needs to refresh the palette before the vblank
       flip.  Both pages get their line tables during init/clear; keep the copy
       fallback for callers that provide a separate host-side buffer. */
    volatile uint16_t *dst16 = &MARS_FRAMEBUFFER;
    int y;
    if (!video || !framebuffer) return;
    cd32x_background_frame_end();
    waifu_cd32x_video_set_palette_rgb(video, rgb, palette_id, fade_q8);

    if (framebuffer == (const uint8_t *)(uintptr_t)WAIFU_CD32X_FRAMEBUFFER_PIXELS) return;

    dst16 += WAIFU_CD32X_LINE_TABLE_WORDS;
    for (y = 0; y < WAIFU_CD32X_H; ++y) {
        const uint8_t *src = framebuffer + y * WAIFU_CD32X_W;
        cd32x_copy_pairs_to_back(dst16 + y * (WAIFU_CD32X_W / 2), src, WAIFU_CD32X_W / 2);
    }
}

void waifu_cd32x_video_wait_vblank(WaifuCd32xVideo *video)
{
    if (!video) return;
    cd32x_wait_fb_flip(video);
}

/* Mega Drive plane-B story sky behind the 32X bitmap.  The Sub-CPU
   supervisor owns the MD VDP work (CD32X_MD_CMD_SET_BG); this side only
   latches kind+scroll opportunistically (never stealing the COMM registers
   from a CD transfer) and reports whether a hardware sky is active so the
   caller clears the sky region to palette index 0 instead of compositing a
   software sky. */
static int g_cd32x_bg_sent_word = -1;   /* last latched (kind<<12)|scroll */
static int g_cd32x_bg_applied_kind = 0; /* KIND the supervisor has actually set up */
static int g_cd32x_bg_frame_requested = 0;

static int cd32x_send_bg_word(unsigned word)
{
    if (MARS_SYS_COMM0 != 0u) return 0;
    MARS_SYS_COMM2 = (uint16_t)word;
    MARS_SYS_COMM4 = CD32X_MD_CMD_SET_BG;
    MARS_SYS_COMM0 = CD32X_COMM_READY;
    return 1;
}

/* No widescreen HUD room on this target: keep the fixed 2D UI layout. */
int waifu_platform_ui_extra_w(void) { return 0; }
int waifu_platform_arena_backdrop(void) { return 0; }
int waifu_platform_arena_backdrop_band(int x, int y, int w, int h)
{ (void)x; (void)y; (void)w; (void)h; return 0; }
void waifu_platform_ui_hud(int on) { (void)on; }
int waifu_platform_performance_tier(void) { return 0; }
int waifu_platform_glyph(int x, int y, int cell_w, unsigned char ch, unsigned char fg, unsigned char shadow)
{ (void)x; (void)y; (void)cell_w; (void)ch; (void)fg; (void)shadow; return 0; }
/* CD32X preloads the ending image through the normal loading-screen path. */
void waifu_platform_prewarm_ending(void) {}

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    unsigned word = (((unsigned)kind & 0xFu) << 12) | ((unsigned)hscroll & 0x1FFu);
    int req_kind = (int)((unsigned)kind & 0xFu);
    g_cd32x_bg_frame_requested = 1;

    /* Promote the last-SENT kind to "applied" only once the supervisor is idle
       again (COMM0 cleared): the SET_BG service builds the sky palette, uploads
       the plane-B tiles/name table and pushes the palette before it releases
       COMM0, so an idle supervisor means that whole set-up has actually landed.
       Reporting "active" merely because the request was SENT let the SH-2 start
       punching index-0 holes for the sky a dozen-plus frames before plane-B was
       really showing it, so those holes exposed the near-white MD layer -- the
       white flash seen when a sanctum scene appears without a covering fade
       (e.g. loading a save straight to the map, or B-ing back from the plaza).
       Until the kind is applied, draw_story_sky() composites the opaque
       software sky instead, which is the same dark void art, so the hand-off is
       seamless rather than a flash. */
    if (MARS_SYS_COMM0 == 0u && g_cd32x_bg_sent_word >= 0) {
        g_cd32x_bg_applied_kind = g_cd32x_bg_sent_word >> 12;
    }

    if ((int)word != g_cd32x_bg_sent_word && cd32x_send_bg_word(word)) {
        g_cd32x_bg_sent_word = (int)word;
    }

    /* Active only once the supervisor has applied THIS kind. */
    if (kind != WAIFU_BACKGROUND_NONE && g_cd32x_bg_applied_kind == req_kind) {
        return 1;
    }
    return 0;
}

/* Called from present: turn the MD sky off as soon as a frame renders without
   requesting one, so battle/menu/deck screens never show it through index-0
   pixels. */
static void cd32x_background_frame_end(void)
{
    if (!g_cd32x_bg_frame_requested &&
        g_cd32x_bg_sent_word > 0 && ((unsigned)g_cd32x_bg_sent_word >> 12) != 0u &&
        cd32x_send_bg_word(0)) {
        g_cd32x_bg_sent_word = 0;
    }
    g_cd32x_bg_frame_requested = 0;
}

int waifu_platform_text_overlay(WaifuTextOverlayKind kind, const WaifuTextOverlayParams *params)
{
    WaifuTextOverlayParams empty = {0};
    const WaifuTextOverlayParams *p = params ? params : &empty;
    switch (kind) {
    case WAIFU_TEXT_OVERLAY_TITLE_PROMPT:
        cd32x_draw_title_prompt_both(p->prompt_visible, p->has_save);
        return 1;
    case WAIFU_TEXT_OVERLAY_MENU:
        cd32x_draw_menu_both(p->selected, p->has_save);
        return 1;
    default:
        return 0;
    }
}

void waifu_platform_text_overlay_clear(void)
{
    g_cd32x_overlay_kind = -1;
    g_cd32x_overlay_base_pages_remaining = 0;
    g_cd32x_prompt_visible = -1;
    g_cd32x_prompt_pages_remaining = 0;
    g_cd32x_menu_selected = -1;
    g_cd32x_menu_has_save = -1;
    g_cd32x_menu_pages_remaining = 0;
}

int waifu_platform_text_overlay_is_hardware(void)
{
    /* CD32X page-flips the framebuffer every frame, so a screen composed only
       on entry lands in just one physical page and flashes.  The CD32X overlay
       path warms both pages after each title/menu mode change, then restores
       only the small mutable prompt/menu areas from the resident title asset. */
    return 1;
}

void waifu_platform_story_layers_begin(void) {}
int waifu_platform_story_portrait(int portrait_id, int x, int y)
{ (void)portrait_id; (void)x; (void)y; return 0; }
