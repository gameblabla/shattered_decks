/* Action-based keyboard + gamepad input — see sdl3_input.h. */

#include "sdl3_input.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#define REPEAT_DELAY_FRAMES 16
#define REPEAT_RATE_FRAMES  4

/* ---- keyboard text entry (platform.h seam) --------------------------------
   The core opens a text field (the story name) and the frontend answers with
   what the player types. Two halves matter:

   * SDL's own text input is started, so the OS keyboard layout, dead keys and
     an IME all work rather than the game guessing letters from scancodes.
   * While it is on, keys that PRODUCE TEXT stop driving actions. Otherwise
     typing a name would also press buttons -- with the stock bindings, W/A/S/D
     are the d-pad, SPACE is RUN and BACKSPACE is Cancel, so "SAM" would walk
     the cursor and start the game. Keys that produce no text (Return, Escape,
     the arrows, Tab, the F-keys) keep working, which is what leaves Return
     free to accept the name and the arrows free to pick a slot. */
#define TEXT_QUEUE_MAX 32
static char g_text_queue[TEXT_QUEUE_MAX];
static int g_text_head, g_text_tail;
static int g_text_want;      /* the core has a field open */
static int g_text_suspend;   /* the frontend menu is on top of it */
static int g_text_active;    /* want && !suspend: SDL text input is running */
static SDL_Window *g_text_window;

static void text_push(char c)
{
    int next = (g_text_tail + 1) % TEXT_QUEUE_MAX;
    if (next == g_text_head) return;      /* full: drop, never overwrite */
    g_text_queue[g_text_tail] = c;
    g_text_tail = next;
}

void waifu_sdl3_text_attach(SDL_Window *window) { g_text_window = window; }

static void text_apply(void)
{
    int want = g_text_want && !g_text_suspend;
    if (want == g_text_active) return;
    g_text_active = want;
    g_text_head = g_text_tail = 0;        /* stale keystrokes never carry over */
    if (!g_text_window) return;
    if (g_text_active) SDL_StartTextInput(g_text_window);
    else               SDL_StopTextInput(g_text_window);
}

void waifu_platform_text_input(int on)
{
    g_text_want = !!on;
    text_apply();
}

/* The frontend menu opens over the game: it needs the letter keys back for its
   own navigation, and nothing typed into it belongs in the game's field. The
   core's request is remembered, so closing the menu puts the field back. */
void waifu_sdl3_text_suspend(int on)
{
    g_text_suspend = !!on;
    text_apply();
}

void waifu_sdl3_text_inject(const char *text)
{
    /* Scripted typing, so the keyboard path is reachable from a headless
       capture the same way MOUSE= reaches the pointer. Queued regardless of
       whether SDL's text input is running -- only the core's open field ever
       drains it. */
    for (; text && *text; ++text) {
        if (*text == '_') text_push(' ');
        else text_push(*text);
    }
}

int waifu_platform_text_poll(void)
{
    int c;
    if (g_text_head == g_text_tail) return 0;
    c = (unsigned char)g_text_queue[g_text_head];
    g_text_head = (g_text_head + 1) % TEXT_QUEUE_MAX;
    return c;
}

/* Does this scancode type something on the current layout? Asking SDL rather
   than listing scancodes keeps it right on a layout where, say, the key at
   QWERTY's W types something else. */
static int scancode_types_text(int sc)
{
    SDL_Keycode k;
    if (sc == SDL_SCANCODE_BACKSPACE) return 1;   /* edits the field */
    k = SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_KMOD_NONE, false);
    return k >= 0x20 && k < 0x7F;
}

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

/* ---- on-screen prompt labels (platform.h seam) -----------------------------
   A prompt that says "A" is a lie on PC: the control is rebindable, and the
   player may be holding a pad instead of a keyboard. The label therefore
   follows both the current binding AND the device last used, which the update
   below records as it scans. Kept short (a prompt line has room for a name, not
   a sentence), so the long scancode names get an abbreviation. */
static const WaifuSettings *g_prompt_cfg;
static int g_prompt_pad_last;         /* 1 = the pad moved most recently */

WaifuInput *waifu_input_create(const WaifuSettings *settings)
{
    WaifuInput *in = (WaifuInput *)calloc(1, sizeof(*in));
    int i, count = 0;
    SDL_JoystickID *ids;
    if (!in) return NULL;
    in->cfg = settings;
    g_prompt_cfg = settings;
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
    case SDL_EVENT_TEXT_INPUT:
        if (g_text_active) {
            const char *t = ev->text.text;
            /* The field is plain ASCII; anything else (an accented letter, an
               IME commit) has no glyph in the game's font, so drop it rather
               than write a byte the name cannot hold. */
            for (; t && *t; ++t)
                if ((unsigned char)*t >= 0x20 && (unsigned char)*t < 0x7F) text_push(*t);
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (g_text_active && ev->key.scancode == SDL_SCANCODE_BACKSPACE) text_push('\b');
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
            if (sc <= 0 || sc >= SDL_SCANCODE_COUNT || !keys[sc]) continue;
            /* A text field is open: the letter keys belong to it, not to the
               buttons they are bound to. */
            if (g_text_active && scancode_types_text(sc)) continue;
            in->held[a] = 1;
            g_prompt_pad_last = 0;
        }
    }

    for (p = 0; p < in->pad_count; ++p) {
        SDL_Gamepad *g = in->pads[p].pad;
        if (!g) continue;
        for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
            for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
                int btn = in->cfg->pad[a][b];
                if (btn >= 0 && SDL_GetGamepadButton(g, (SDL_GamepadButton)btn)) {
                    in->held[a] = 1;
                    g_prompt_pad_last = 1;
                }
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

/* Short pad-face names. waifu_settings_pad_name() spells them out for the
   rebinding screen ("A / CROSS"), which is too long to sit in a prompt. */
static const char *prompt_pad_short(int button)
{
    static const char *const names[] = {
        "A", "B", "X", "Y", "BACK", "GUIDE", "START",
        "LSTICK", "RSTICK", "L1", "R1",
        "UP", "DOWN", "LEFT", "RIGHT"
    };
    if (button < 0 || button >= (int)(sizeof(names) / sizeof(names[0]))) return NULL;
    return names[button];
}

/* Abbreviations for the keys long enough to push a prompt off its line. */
static const char *prompt_key_short(const char *name)
{
    static const struct { const char *full, *shortname; } k[] = {
        { "LEFT CTRL", "LCTRL" },   { "RIGHT CTRL", "RCTRL" },
        { "LEFT ALT", "LALT" },     { "RIGHT ALT", "RALT" },
        { "LEFT SHIFT", "LSHIFT" }, { "RIGHT SHIFT", "RSHIFT" },
        { "RETURN", "ENTER" },      { "BACKSPACE", "BKSP" },
        { "ESCAPE", "ESC" }
    };
    int i;
    for (i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
        if (!SDL_strcmp(name, k[i].full)) return k[i].shortname;
    return name;
}

const char *waifu_platform_prompt_label(int action)
{
    static const int map[] = {
        WAIFU_ACT_CONFIRM, WAIFU_ACT_CANCEL, WAIFU_ACT_START, WAIFU_ACT_ASSIST
    };
    int act, b;
    if (!g_prompt_cfg) return NULL;
    if (action < 0 || action >= (int)(sizeof(map) / sizeof(map[0]))) return NULL;
    act = map[action];
    if (g_prompt_pad_last) {
        for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
            const char *n = prompt_pad_short(g_prompt_cfg->pad[act][b]);
            if (n) return n;
        }
    }
    for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
        int sc = g_prompt_cfg->key[act][b];
        if (sc > 0 && sc < SDL_SCANCODE_COUNT)
            return prompt_key_short(waifu_settings_key_name(sc));
    }
    /* Bound to a pad button only, whatever the player last touched. */
    for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
        const char *n = prompt_pad_short(g_prompt_cfg->pad[act][b]);
        if (n) return n;
    }
    return NULL;
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
