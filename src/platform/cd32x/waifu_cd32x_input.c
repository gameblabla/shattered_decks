#include "waifu_cd32x_input.h"
#include "cd32x_32x.h"
#include <string.h>

struct WaifuCd32xInput {
    uint16_t previous_raw;
    uint16_t current_raw;
};

static WaifuCd32xInput g_input;

WaifuCd32xInput *waifu_cd32x_input_create(void)
{
    memset(&g_input, 0, sizeof(g_input));
    return &g_input;
}

void waifu_cd32x_input_destroy(WaifuCd32xInput *input)
{
    (void)input;
}

void waifu_cd32x_input_poll(WaifuCd32xInput *input, WaifuFmInput *out)
{
    uint16_t buttons;
    if (!input || !out) return;
    memset(out, 0, sizeof(*out));
    input->previous_raw = input->current_raw;
    input->current_raw = (uint16_t)(MARS_SYS_COMM8 & SEGA_CTRL_BUTTONS);
    buttons = input->current_raw;

    out->up = (buttons & SEGA_CTRL_UP) != 0;
    out->down = (buttons & SEGA_CTRL_DOWN) != 0;
    out->left = (buttons & SEGA_CTRL_LEFT) != 0;
    out->right = (buttons & SEGA_CTRL_RIGHT) != 0;
    out->a = (buttons & SEGA_CTRL_A) != 0;
    out->b = (buttons & (SEGA_CTRL_B | SEGA_CTRL_C)) != 0;
    out->tab = (buttons & SEGA_CTRL_X) != 0;
    out->start = (buttons & SEGA_CTRL_START) != 0;
}
