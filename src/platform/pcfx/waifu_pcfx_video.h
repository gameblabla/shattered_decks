#ifndef WAIFU_PCFX_VIDEO_H
#define WAIFU_PCFX_VIDEO_H

#include <stdint.h>
#include "game_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuPcfxVideo WaifuPcfxVideo;

typedef enum WaifuPcfxVideoMode {
    WAIFU_PCFX_VIDEO_MODE_KING_8BPP = 0,
    WAIFU_PCFX_VIDEO_MODE_TITLE_HICOLOR = 1
} WaifuPcfxVideoMode;

typedef enum WaifuPcfxVdcBackground {
    WAIFU_PCFX_VDC_BG_NONE = 0,
    WAIFU_PCFX_VDC_BG_TITLE,
    WAIFU_PCFX_VDC_BG_GAME_3D
} WaifuPcfxVdcBackground;

typedef enum WaifuPcfxSanctumBackdrop {
    WAIFU_PCFX_SANCTUM_BACKDROP_DESERT = 0,
    WAIFU_PCFX_SANCTUM_BACKDROP_STONE = 1,
    WAIFU_PCFX_SANCTUM_BACKDROP_EMBER = 2,
    WAIFU_PCFX_SANCTUM_BACKDROP_SKY = 3
} WaifuPcfxSanctumBackdrop;

typedef enum WaifuPcfxSanctumOverlay {
    WAIFU_PCFX_SANCTUM_OVERLAY_MENU = 0,
    WAIFU_PCFX_SANCTUM_OVERLAY_SAVE = 1,
    WAIFU_PCFX_SANCTUM_OVERLAY_SAVE_DEVICE = 2
} WaifuPcfxSanctumOverlay;

WaifuPcfxVideo *waifu_pcfx_video_create(void);
void waifu_pcfx_video_destroy(WaifuPcfxVideo *video);
void waifu_pcfx_video_begin_8bpp(WaifuPcfxVideo *video);
void waifu_pcfx_video_use_vdc_background(WaifuPcfxVideo *video, WaifuPcfxVdcBackground bg);
void waifu_pcfx_video_request_vdc_background(WaifuPcfxVdcBackground bg);
void waifu_pcfx_video_request_rainbow_backdrop(WaifuPcfxSanctumBackdrop backdrop);
void waifu_pcfx_video_request_rainbow_hscroll(int hscroll);
void waifu_pcfx_video_request_sanctum(WaifuPcfxSanctumBackdrop backdrop, WaifuPcfxSanctumOverlay overlay, int value, int blink_visible);
void waifu_pcfx_video_set_palette_rgb(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id);
void waifu_pcfx_video_set_palette_rgb_fade(WaifuPcfxVideo *video, const uint8_t *rgb, WaifuFmPaletteId palette_id, int fade_q8);
void waifu_pcfx_video_clear_black(WaifuPcfxVideo *video);
void waifu_pcfx_video_present_8bpp(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb, WaifuFmPaletteId palette_id);
void waifu_pcfx_video_present_title_hicolor_stub(WaifuPcfxVideo *video, const uint8_t *framebuffer, const uint8_t *rgb);
void waifu_pcfx_video_wait_vblank(WaifuPcfxVideo *video);
void waifu_pcfx_video_overlay_title_prompt(int visible, int has_save);
void waifu_pcfx_video_overlay_menu(int selected, int has_save);
void waifu_pcfx_video_overlay_load_menu(int selected, int internal_has_save, int external_has_save);
void waifu_pcfx_video_overlay_ending_story(const char *name, int page, int prompt_visible, int visible_chars);
void waifu_pcfx_video_overlay_ending_credits(void);
void waifu_pcfx_video_overlay_clear(void);

/* In-duel HUD text on the VDC hardware overlay layer.  Duel draw code just calls
   hud_print() for each HUD string (framebuffer pixel coords, snapped to the 8x8
   tile grid); the next present composites them in front of KING BG0 without
   touching the KING framebuffer and clears the list for the next frame.  Frames
   that print no HUD text tear the overlay back down after a short grace period.
   color is one of WAIFU_PCFX_HUD_COLOR_*.  hud_print_centered() horizontally
   centers on the visible width using the 8px HUD glyph advance. */
#define WAIFU_PCFX_HUD_COLOR_WHITE 0
#define WAIFU_PCFX_HUD_COLOR_GOLD  1
#define WAIFU_PCFX_HUD_COLOR_GREEN 2
#define WAIFU_PCFX_HUD_COLOR_RED   3
void waifu_pcfx_video_hud_print(int x, int y, const char *str, int color);
void waifu_pcfx_video_hud_print_centered(int y, const char *str, int color);
void waifu_pcfx_video_hud_def_icon(int x, int y, int color);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_PCFX_VIDEO_H */
