#ifndef WAIFU_FM_GAME_API_H
#define WAIFU_FM_GAME_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_FM_WIDTH 256
#define WAIFU_FM_HEIGHT 240
#define WAIFU_FM_FPS 60

typedef struct WaifuFmInput {
    int up;
    int down;
    int left;
    int right;
    int a;      /* LCTRL */
    int b;      /* LALT */
    int start;  /* SPACE */
} WaifuFmInput;

void waifu_fm_init(void);
void waifu_fm_reset_interactive(void);
void waifu_fm_step(const WaifuFmInput *input);
void waifu_fm_render_scripted_frame(int frame);
uint8_t *waifu_fm_framebuffer(void);
const uint8_t *waifu_fm_palette_rgb(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_GAME_API_H */
