#ifndef WAIFU_FM_GAME_API_H
#define WAIFU_FM_GAME_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_FM_WIDTH 256
#define WAIFU_FM_HEIGHT 240
#define WAIFU_FM_FPS 60

typedef enum WaifuFmPaletteId {
    WAIFU_FM_PALETTE_COMMON = 0,
    WAIFU_FM_PALETTE_TITLE = 1
} WaifuFmPaletteId;

typedef enum WaifuFmMusicTrack {
    WAIFU_FM_MUSIC_NONE = 0,
    WAIFU_FM_MUSIC_TITLE,
    WAIFU_FM_MUSIC_OPENING_DREAM,
    WAIFU_FM_MUSIC_DECK_EDITOR,
    WAIFU_FM_MUSIC_BOSS,
    WAIFU_FM_MUSIC_FINAL_BOSS,
    WAIFU_FM_MUSIC_RANDOM_BATTLE,
    WAIFU_FM_MUSIC_RESULTS,
    WAIFU_FM_MUSIC_LOST,
    WAIFU_FM_MUSIC_TRACK_COUNT
} WaifuFmMusicTrack;

typedef struct WaifuFmInput {
    int up;
    int down;
    int left;
    int right;
    int a;      /* LCTRL */
    int b;      /* LALT */
    int start;  /* SPACE */
    int tab;    /* TAB / button 4 */
} WaifuFmInput;

#define WAIFU_FM_MAX_DIRTY_RECTS 8

typedef struct WaifuFmDirtyRect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} WaifuFmDirtyRect;

void waifu_fm_init(void);
void waifu_fm_reset_interactive(void);
void waifu_fm_step(const WaifuFmInput *input);
void waifu_fm_render_scripted_frame(int frame);
uint8_t *waifu_fm_framebuffer(void);
uint32_t waifu_fm_frame_dirty_serial(void);
int waifu_fm_frame_dirty_full(void);
int waifu_fm_frame_dirty_rects(WaifuFmDirtyRect *out_rects, int max_rects);
const uint8_t *waifu_fm_palette_rgb(void);
const uint8_t *waifu_fm_palette_rgb_for_id(WaifuFmPaletteId id);
WaifuFmPaletteId waifu_fm_palette_id(void);
int waifu_fm_video_fade_q8(void);
int waifu_fm_audio_sample_rate(void);
int waifu_fm_audio_channels(void);
int waifu_fm_audio_samples_per_frame(void);
void waifu_fm_audio_mix_s16(int16_t *dst, int frames);
WaifuFmMusicTrack waifu_fm_audio_music_track(void);
const char *waifu_fm_audio_music_name(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_GAME_API_H */
