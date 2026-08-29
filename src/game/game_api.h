#ifndef WAIFU_FM_GAME_API_H
#define WAIFU_FM_GAME_API_H

#include <stdint.h>

/* WAIFU_FM_WIDTH / WAIFU_FM_HEIGHT are the single authoritative framebuffer
   dimensions, defined once in the engine-layer cfx_screen_config.h. */
#include "cfx_screen_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_FM_FPS 60

typedef enum WaifuFmPaletteId {
    WAIFU_FM_PALETTE_COMMON = 0,
    WAIFU_FM_PALETTE_TITLE = 1,
    WAIFU_FM_PALETTE_ENDING = 2,
    WAIFU_FM_PALETTE_ENDING_BLACK = 3,
    WAIFU_FM_PALETTE_DIALOGUE = 4
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
/* Report how many hardware vblanks the previous frame actually took (>=1).
   Static/2-D clocks consume this wall-clock step. Moving battle cameras keep
   one displayed pose per waifu_fm_step(), so an over-budget render cannot skip
   authored poses. Platforms that never call this keep the default 1. */
void waifu_fm_set_frame_vblanks(int vblanks);
void waifu_fm_render_scripted_frame(int frame);
uint8_t *waifu_fm_framebuffer(void);
/* Which parts of the framebuffer waifu_fm_step() actually wrote.  Non-zero
   return = all of it (the default, and what every frame that clears and
   redraws reports).  Otherwise *rows is WAIFU_FM_HEIGHT mask bytes, one bit
   per 64-pixel group of that scanline.  A presenter that reads this can skip
   comparing and uploading the rest; one that ignores it is still correct.
   waifu_fm_frame_damage_clear() says the frame reached the screen. */
int waifu_fm_frame_damage(const uint8_t **rows);
void waifu_fm_frame_damage_clear(void);
/* Non-zero only when the final frame rendered a dense live board/floor. A
   presenter may use an unconditional full upload for that case; sparse and
   retained frames must continue through the row-mask comparison path even if
   their damage declaration says "full" because they cleared before drawing. */
int waifu_fm_frame_present_dense(void);
/* The subset of that mask the frame knows changed, so a presenter that
   compares before uploading can skip the comparison there.  NULL, or all
   zero, when the frame makes no such claim. */
const uint8_t *waifu_fm_frame_damage_forced(void);
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
