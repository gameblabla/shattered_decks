/* Sega CD 32X video backend.
 *
 * This target intentionally exposes only the generic game framebuffer seam to
 * common code.  Platform-specific work stays here: 32X VDP setup, CRAM upload,
 * and 320x240 8bpp framebuffer presentation. */
#include "waifu_cd32x_video.h"
#include "cd32x_32x.h"
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


#if WAIFU_CD32X_W != 320 || WAIFU_CD32X_H != 240
#error "CD32X video backend expects WAIFU_FM_WIDTH=320 and WAIFU_FM_HEIGHT=240"
#endif
#if CD32X_TITLE_SCREEN_W != 320 || CD32X_TITLE_SCREEN_H != 240
#error "CD32X title asset must be 320x240"
#endif

struct WaifuCd32xVideo {
    uint16_t current_fb;
    WaifuFmPaletteId current_palette_id;
    int current_fade_q8;
    int current_md_fade_q8;
};

static WaifuCd32xVideo g_video;

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

static void cd32x_wait_fb_flip(WaifuCd32xVideo *video)
{
    MARS_VDP_FBCTL = (uint16_t)(video->current_fb ^ 1u);
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == video->current_fb) {
    }
    video->current_fb ^= 1u;
}

static void cd32x_write_back_line_table(void)
{
    /* Blastem's 32x_video.c is the ground truth here: framebuffer aperture
       writes always target video->back, and changing FS swaps front/back.  The
       0x24020000 aperture is overwrite mode for that same back page, not a
       CPU-addressable second page.  Therefore each page must receive its line
       table while it is the current back page, before requesting an FS flip. */
    volatile uint16_t *fb16 = &MARS_FRAMEBUFFER;
    for (int y = 0; y < WAIFU_CD32X_H; ++y) {
        fb16[y] = (uint16_t)(WAIFU_CD32X_LINE_TABLE_WORDS + y * (WAIFU_CD32X_W / 2));
    }
}

static void cd32x_clear_back_pixels(uint8_t c)
{
    volatile uint16_t *fb16 = &MARS_FRAMEBUFFER;
    uint16_t pair = (uint16_t)(((uint16_t)c << 8) | c);

    fb16 += WAIFU_CD32X_LINE_TABLE_WORDS;
    for (int i = 0; i < (WAIFU_CD32X_W * WAIFU_CD32X_H) / 2; ++i) {
        fb16[i] = pair;
    }
}

static void cd32x_init_framebuffers(WaifuCd32xVideo *video)
{
    video->current_fb = (uint16_t)(MARS_VDP_FBCTL & MARS_VDP_FS);
    cd32x_write_back_line_table();
    cd32x_clear_back_pixels(0);
    cd32x_wait_fb_flip(video);
    cd32x_write_back_line_table();
    cd32x_clear_back_pixels(0);
}

static void cd32x_put_px_back(int x, int y, uint8_t c)
{
    volatile uint16_t *words;
    uint32_t pix;
    uint16_t w;
    if ((unsigned)x >= WAIFU_CD32X_W || (unsigned)y >= WAIFU_CD32X_H) return;
    words = &MARS_FRAMEBUFFER;
    pix = (uint32_t)WAIFU_CD32X_FB_BYTE_OFFSET + (uint32_t)y * WAIFU_CD32X_W + (uint32_t)x;
    words += pix >> 1;
    w = *words;
    if ((x & 1) == 0) w = (uint16_t)((w & 0x00ffu) | ((uint16_t)c << 8));
    else w = (uint16_t)((w & 0xff00u) | c);
    *words = w;
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

static void cd32x_draw_title_prompt_both(int prompt_visible, int has_save)
{
    (void)prompt_visible;
    (void)has_save;
    cd32x_draw_title_logo_both();
    /* Keep the prompt resident instead of blinking it.  Without a separate
       hardware text plane, hiding it would require reloading/restoring title
       pixels under the glyph cells. */
    cd32x_draw_text_centered_both(190, "PRESS RUN TO START", 1, IDX_WHITE, IDX_BLACK);
    cd32x_draw_text_centered_both(208, "(C) 2026 GAMEBLABLA", 1, IDX_WHITE, IDX_BLACK);
}

static void cd32x_draw_menu_both(int selected, int has_save)
{
    int ox = (WAIFU_CD32X_W - 256) / 2;
    const char *help = "RANDOM DECK / FREE DUEL";
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
    /* The help line sits directly over the title-screen copyright strip.
       CD32X draws text into the 32X bitmap instead of a separate MD/PC-FX text
       layer, so clear this narrow line before the transparent glyph/outline
       renderer runs.  This removes the old copyright pixels without restoring
       solid black cells around every menu character. */
    cd32x_fill_rect_back(ox + 43, 204, 170, 14, IDX_BLACK);
    cd32x_draw_text_scaled_both(ox + 55, 207, help, 1, has_save || selected != 2 ? IDX_WHITE : IDX_RED, IDX_BLACK);
}

WaifuCd32xVideo *waifu_cd32x_video_create(void)
{
    memset(&g_video, 0, sizeof(g_video));
    while ((MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) == 0) {
    }

    MARS_VDP_DISPMODE = (uint16_t)(MARS_240_LINES | MARS_VDP_MODE_256 | MARS_VDP_PRIO_32X);

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
    MARS_VDP_DISPMODE = (uint16_t)(MARS_240_LINES | MARS_VDP_MODE_256 | MARS_VDP_PRIO_32X);
}

void waifu_cd32x_video_set_palette_rgb(WaifuCd32xVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8)
{
    volatile uint16_t *cram = &MARS_CRAM;
    int i;
    if (!video || !rgb) return;
    if (fade_q8 < 0) fade_q8 = 0;
    if (fade_q8 > 256) fade_q8 = 256;
    if (palette_id == video->current_palette_id && fade_q8 == video->current_fade_q8) return;
    for (i = 0; i < 256; ++i) {
        cram[i] = cd32x_rgb_to_cram(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2], fade_q8);
    }
    if (fade_q8 != video->current_md_fade_q8 && cd32x_request_md_palette_fade(fade_q8)) {
        video->current_md_fade_q8 = fade_q8;
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
       page.  Keep flipping, but make the current back page self-contained every
       frame: line table first, then the complete software-rendered pixels.
       Title/menu are intentionally not treated as one-shot hardware overlays on
       this target. */
    volatile uint16_t *dst16 = &MARS_FRAMEBUFFER;
    int y;
    if (!video || !framebuffer) return;
    waifu_cd32x_video_set_palette_rgb(video, rgb, palette_id, fade_q8);

    cd32x_write_back_line_table();
    dst16 += WAIFU_CD32X_LINE_TABLE_WORDS;
    for (y = 0; y < WAIFU_CD32X_H; ++y) {
        const uint8_t *src = framebuffer + y * WAIFU_CD32X_W;
        int x;
        for (x = 0; x < WAIFU_CD32X_W; x += 2) {
            dst16[y * (WAIFU_CD32X_W / 2) + (x / 2)] = (uint16_t)(((uint16_t)src[x] << 8) | src[x + 1]);
        }
    }
}

void waifu_cd32x_video_wait_vblank(WaifuCd32xVideo *video)
{
    if (!video) return;
    cd32x_wait_fb_flip(video);
}

int waifu_platform_background_request(WaifuBackgroundKind kind, int hscroll)
{
    (void)kind;
    (void)hscroll;
    return 0;
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
}

int waifu_platform_text_overlay_is_hardware(void)
{
    /* CD32X page-flips the framebuffer every frame, so a screen composed only
       on entry lands in just one physical page and flashes.  Drive the
       title/menu through the software full-redraw path instead (the title is
       kept resident in the asset arena), so present_8bpp repacks a complete
       frame into the back page every frame. */
    return 0;
}
