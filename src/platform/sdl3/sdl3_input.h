#ifndef WAIFU_SDL3_INPUT_H
#define WAIFU_SDL3_INPUT_H

/* Action-based input for the PC build.
 *
 * Keyboard and every connected gamepad are folded into one logical action set
 * (sdl3_settings.h) that the frontend samples once per frame: `held` for the
 * game core, `pressed` edges for the menus and the frontend hotkeys.  Any
 * number of pads may be connected and all of them drive player 1 — a couch
 * player can pick up whichever controller is nearest, which is what a shipped
 * single-player game does.
 *
 * Analog sticks emulate the d-pad through a configurable deadzone, and the
 * menu layer gets auto-repeat on the four directions. */

#include "sdl3_settings.h"
#include "game_api.h"

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_INPUT_MAX_PADS 8

typedef struct WaifuInput WaifuInput;

WaifuInput *waifu_input_create(const WaifuSettings *settings);
void waifu_input_destroy(WaifuInput *in);

/* Re-reads the settings pointer's bindings/deadzone (call after the options
   menu edits them). The pointer itself is retained, so this is only needed to
   reopen pads after a rumble/deadzone change; bindings are read live. */
void waifu_input_settings_changed(WaifuInput *in);

/* Feeds one SDL event (device add/remove, key/button edges for rebinding). */
void waifu_input_handle_event(WaifuInput *in, const SDL_Event *ev);

/* Merges a scripted button set into the NEXT waifu_input_update() (command
   files, regression captures). `menu` presses the frontend MENU action. The
   injection lasts exactly one frame, so live input keeps working alongside. */
void waifu_input_inject(WaifuInput *in, const WaifuFmInput *game, int menu);

/* Samples keyboard + pads into this frame's action state. Call once per frame
   after the event pump. */
void waifu_input_update(WaifuInput *in);

int waifu_input_held(const WaifuInput *in, int action);
int waifu_input_pressed(const WaifuInput *in, int action);
/* Direction edge with menu auto-repeat (initial delay then a fast repeat). */
int waifu_input_repeat(const WaifuInput *in, int action);

/* Number of gamepads currently open, and a display name for one of them. */
int waifu_input_pad_count(const WaifuInput *in);
const char *waifu_input_pad_name(const WaifuInput *in, int index);

/* Rumble every open pad (no-op when disabled in the settings). */
void waifu_input_rumble(WaifuInput *in, float strength, int ms);

/* --- rebinding ------------------------------------------------------------
   While capture is armed, the next key or gamepad button press is recorded
   instead of acting.  waifu_input_capture_result() returns 1 once something
   was captured, writing the scancode (is_pad = 0) or gamepad button
   (is_pad = 1); ESC / pad BACK cancels and reports is_pad = -1. */
void waifu_input_capture_begin(WaifuInput *in);
void waifu_input_capture_cancel(WaifuInput *in);
int waifu_input_capturing(const WaifuInput *in);
int waifu_input_capture_result(WaifuInput *in, int *code, int *is_pad);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_INPUT_H */
