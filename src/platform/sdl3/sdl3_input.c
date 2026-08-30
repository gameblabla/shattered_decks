/* Action-based keyboard + gamepad input — see sdl3_input.h. */

#include "sdl3_input.h"

#include <stdlib.h>
#include <string.h>

#define REPEAT_DELAY_FRAMES 16
#define REPEAT_RATE_FRAMES  4

typedef struct PadSlot {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
} PadSlot;

struct WaifuInput {
    const WaifuSettings *cfg;
    PadSlot pads[WAIFU_INPUT_MAX_PADS];
    int pad_count;

    unsigned char held[WAIFU_ACT_COUNT];
    unsigned char prev[WAIFU_ACT_COUNT];
    int repeat_timer[WAIFU_ACT_COUNT];
    unsigned char repeat_fire[WAIFU_ACT_COUNT];

    unsigned char injected[WAIFU_ACT_COUNT];   /* scripted buttons for this frame */

    int capturing;
    int captured;                /* 1 = something captured this frame */
    int capture_code;
    int capture_is_pad;          /* 0 key, 1 pad, -1 cancelled */
};

WaifuInput *waifu_input_create(const WaifuSettings *settings)
{
    WaifuInput *in = (WaifuInput *)calloc(1, sizeof(*in));
    int i, count = 0;
    SDL_JoystickID *ids;
    if (!in) return NULL;
    in->cfg = settings;
    ids = SDL_GetGamepads(&count);
    if (ids) {
        for (i = 0; i < count && in->pad_count < WAIFU_INPUT_MAX_PADS; ++i) {
            SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
            if (!g) continue;
            in->pads[in->pad_count].pad = g;
            in->pads[in->pad_count].id = ids[i];
            in->pad_count++;
        }
        SDL_free(ids);
    }
    return in;
}

void waifu_input_destroy(WaifuInput *in)
{
    int i;
    if (!in) return;
    for (i = 0; i < in->pad_count; ++i)
        if (in->pads[i].pad) SDL_CloseGamepad(in->pads[i].pad);
    free(in);
}

void waifu_input_settings_changed(WaifuInput *in) { (void)in; }

static void pad_add(WaifuInput *in, SDL_JoystickID id)
{
    int i;
    SDL_Gamepad *g;
    for (i = 0; i < in->pad_count; ++i)
        if (in->pads[i].id == id) return;
    if (in->pad_count >= WAIFU_INPUT_MAX_PADS) return;
    g = SDL_OpenGamepad(id);
    if (!g) return;
    in->pads[in->pad_count].pad = g;
    in->pads[in->pad_count].id = id;
    in->pad_count++;
}

static void pad_remove(WaifuInput *in, SDL_JoystickID id)
{
    int i;
    for (i = 0; i < in->pad_count; ++i) {
        if (in->pads[i].id != id) continue;
        if (in->pads[i].pad) SDL_CloseGamepad(in->pads[i].pad);
        in->pads[i] = in->pads[in->pad_count - 1];
        in->pad_count--;
        return;
    }
}

void waifu_input_handle_event(WaifuInput *in, const SDL_Event *ev)
{
    if (!in || !ev) return;
    switch (ev->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_add(in, ev->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_remove(in, ev->gdevice.which);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (in->capturing && !ev->key.repeat) {
            in->capturing = 0;
            in->captured = 1;
            if (ev->key.scancode == SDL_SCANCODE_ESCAPE) {
                in->capture_is_pad = -1;
                in->capture_code = 0;
            } else {
                in->capture_is_pad = 0;
                in->capture_code = (int)ev->key.scancode;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (in->capturing) {
            in->capturing = 0;
            in->captured = 1;
            if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                in->capture_is_pad = -1;
                in->capture_code = 0;
            } else {
                in->capture_is_pad = 1;
                in->capture_code = (int)ev->gbutton.button;
            }
        }
        break;
    default:
        break;
    }
}

void waifu_input_inject(WaifuInput *in, const WaifuFmInput *game, int menu)
{
    if (!in) return;
    memset(in->injected, 0, sizeof(in->injected));
    if (game) {
        in->injected[WAIFU_ACT_UP] = (unsigned char)(game->up != 0);
        in->injected[WAIFU_ACT_DOWN] = (unsigned char)(game->down != 0);
        in->injected[WAIFU_ACT_LEFT] = (unsigned char)(game->left != 0);
        in->injected[WAIFU_ACT_RIGHT] = (unsigned char)(game->right != 0);
        in->injected[WAIFU_ACT_CONFIRM] = (unsigned char)(game->a != 0);
        in->injected[WAIFU_ACT_CANCEL] = (unsigned char)(game->b != 0);
        in->injected[WAIFU_ACT_START] = (unsigned char)(game->start != 0);
        in->injected[WAIFU_ACT_ASSIST] = (unsigned char)(game->tab != 0);
    }
    in->injected[WAIFU_ACT_MENU] = (unsigned char)(menu != 0);
}

static int axis_dir(SDL_Gamepad *g, SDL_GamepadAxis axis, int positive, int deadzone_pct)
{
    int v = (int)SDL_GetGamepadAxis(g, axis);
    int threshold = 32767 * (deadzone_pct < 5 ? 5 : deadzone_pct) / 100;
    return positive ? (v > threshold) : (v < -threshold);
}

void waifu_input_update(WaifuInput *in)
{
    const bool *keys;
    int a, b, p;
    if (!in) return;

    memcpy(in->prev, in->held, sizeof(in->prev));
    memset(in->held, 0, sizeof(in->held));

    for (a = 0; a < WAIFU_ACT_COUNT; ++a)
        if (in->injected[a]) in->held[a] = 1;
    memset(in->injected, 0, sizeof(in->injected));

    keys = SDL_GetKeyboardState(NULL);
    for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
        for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
            int sc = in->cfg->key[a][b];
            if (sc > 0 && sc < SDL_SCANCODE_COUNT && keys[sc]) in->held[a] = 1;
        }
    }

    for (p = 0; p < in->pad_count; ++p) {
        SDL_Gamepad *g = in->pads[p].pad;
        if (!g) continue;
        for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
            for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
                int btn = in->cfg->pad[a][b];
                if (btn >= 0 && SDL_GetGamepadButton(g, (SDL_GamepadButton)btn))
                    in->held[a] = 1;
            }
        }
        /* Both sticks steer the d-pad: menus and the board cursor feel the
           same whichever stick the player reaches for. */
        if (axis_dir(g, SDL_GAMEPAD_AXIS_LEFTY, 0, in->cfg->deadzone) ||
            axis_dir(g, SDL_GAMEPAD_AXIS_RIGHTY, 0, in->cfg->deadzone)) in->held[WAIFU_ACT_UP] = 1;
        if (axis_dir(g, SDL_GAMEPAD_AXIS_LEFTY, 1, in->cfg->deadzone) ||
            axis_dir(g, SDL_GAMEPAD_AXIS_RIGHTY, 1, in->cfg->deadzone)) in->held[WAIFU_ACT_DOWN] = 1;
        if (axis_dir(g, SDL_GAMEPAD_AXIS_LEFTX, 0, in->cfg->deadzone) ||
            axis_dir(g, SDL_GAMEPAD_AXIS_RIGHTX, 0, in->cfg->deadzone)) in->held[WAIFU_ACT_LEFT] = 1;
        if (axis_dir(g, SDL_GAMEPAD_AXIS_LEFTX, 1, in->cfg->deadzone) ||
            axis_dir(g, SDL_GAMEPAD_AXIS_RIGHTX, 1, in->cfg->deadzone)) in->held[WAIFU_ACT_RIGHT] = 1;
    }

    for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
        in->repeat_fire[a] = 0;
        if (!in->held[a]) {
            in->repeat_timer[a] = 0;
            continue;
        }
        if (!in->prev[a]) {
            in->repeat_fire[a] = 1;
            in->repeat_timer[a] = REPEAT_DELAY_FRAMES;
        } else if (--in->repeat_timer[a] <= 0) {
            in->repeat_fire[a] = 1;
            in->repeat_timer[a] = REPEAT_RATE_FRAMES;
        }
    }
}

int waifu_input_held(const WaifuInput *in, int action)
{
    if (!in || action < 0 || action >= WAIFU_ACT_COUNT) return 0;
    return in->held[action];
}

int waifu_input_pressed(const WaifuInput *in, int action)
{
    if (!in || action < 0 || action >= WAIFU_ACT_COUNT) return 0;
    return in->held[action] && !in->prev[action];
}

int waifu_input_repeat(const WaifuInput *in, int action)
{
    if (!in || action < 0 || action >= WAIFU_ACT_COUNT) return 0;
    return in->repeat_fire[action];
}

int waifu_input_pad_count(const WaifuInput *in) { return in ? in->pad_count : 0; }

const char *waifu_input_pad_name(const WaifuInput *in, int index)
{
    const char *name;
    if (!in || index < 0 || index >= in->pad_count) return "";
    name = SDL_GetGamepadName(in->pads[index].pad);
    return name ? name : "GAMEPAD";
}

void waifu_input_rumble(WaifuInput *in, float strength, int ms)
{
    int i;
    Uint16 mag;
    if (!in || !in->cfg->rumble) return;
    if (strength < 0.0f) strength = 0.0f;
    if (strength > 1.0f) strength = 1.0f;
    mag = (Uint16)(strength * 65535.0f);
    for (i = 0; i < in->pad_count; ++i)
        if (in->pads[i].pad) SDL_RumbleGamepad(in->pads[i].pad, mag, mag, (Uint32)ms);
}

void waifu_input_capture_begin(WaifuInput *in)
{
    if (!in) return;
    in->capturing = 1;
    in->captured = 0;
}

void waifu_input_capture_cancel(WaifuInput *in)
{
    if (!in) return;
    in->capturing = 0;
    in->captured = 0;
}

int waifu_input_capturing(const WaifuInput *in) { return in ? in->capturing : 0; }

int waifu_input_capture_result(WaifuInput *in, int *code, int *is_pad)
{
    if (!in || !in->captured) return 0;
    in->captured = 0;
    if (code) *code = in->capture_code;
    if (is_pad) *is_pad = in->capture_is_pad;
    return 1;
}
