#include "waifu_pcfx_input.h"
#include "pcfx.h"
#include <string.h>

struct WaifuPcfxInput {
    uint32_t previous_raw;
    uint32_t current_raw;
};

static WaifuPcfxInput g_input;

WaifuPcfxInput *waifu_pcfx_input_create(void)
{
    memset(&g_input, 0, sizeof(g_input));
    eris_pad_init(0);
    return &g_input;
}

void waifu_pcfx_input_destroy(WaifuPcfxInput *input)
{
    (void)input;
}

void waifu_pcfx_input_poll(WaifuPcfxInput *input, WaifuFmInput *out)
{
    if (!input || !out) return;
    memset(out, 0, sizeof(*out));
    input->previous_raw = input->current_raw;
    (void)eris_pad_type(0);
    input->current_raw = (uint32_t)eris_pad_read(0);

    /* Button mapping follows Cascade FX's known-good PC-FX pad decode. */
    out->left  = (input->current_raw & (1u << 11)) != 0;
    out->right = (input->current_raw & (1u << 9)) != 0;
    out->up    = (input->current_raw & (1u << 8)) != 0;
    out->down  = (input->current_raw & (1u << 10)) != 0;
    out->a     = (input->current_raw & (1u << 0)) != 0;
    out->b     = (input->current_raw & (1u << 1)) != 0;
    out->tab   = (input->current_raw & (1u << 2)) != 0;
    out->start = (input->current_raw & (1u << 7)) != 0;
}
