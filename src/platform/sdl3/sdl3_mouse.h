#ifndef WAIFU_SDL3_MOUSE_H
#define WAIFU_SDL3_MOUSE_H

/* Mouse pointer for the PC build, plus the two other small frontend->core
 * signals that ride with it (the display was rebuilt; the title menu asked for
 * the options screen).
 *
 * The game core is cursor-driven, so the pointer is not a second input scheme
 * bolted next to the pad: main.c hit-tests the pointer against the layout it is
 * already drawing and moves the SAME cursor the pad moves, then synthesizes the
 * confirm/cancel it would have produced. That keeps one state machine and makes
 * mouse and pad interchangeable mid-action.
 *
 * Button edges are consumed by waifu_platform_pointer(): the fixed 60 Hz clock
 * can run several game steps for one real frame, and a click must land in
 * exactly one of them. */

#include "sdl3_video.h"

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Feeds one SDL event (motion, buttons, wheel). */
void waifu_sdl3_mouse_handle_event(const SDL_Event *ev);

/* Samples the pointer into game space for this frame. Call once per frame,
   after the event pump and before stepping the game. `suppressed` hides the
   pointer from the core (the frontend menu is open / a script is driving). */
void waifu_sdl3_mouse_update(const WaifuSdl3Video *video, int suppressed);

/* Places the pointer in game space and optionally presses a button, for a
   command script. An injected position replaces the real mouse for that frame
   and marks the pointer "in use", which is what makes the pointer UI (and the
   on-screen buttons) reachable from a headless capture. `press` and `release`
   are separate so a script can hold the button down across frames and perform a
   real drag; a plain click sets both. */
void waifu_sdl3_mouse_inject(int game_x, int game_y, int press, int release, int rclick);

/* Scripted wheel notches (+ away from the player, - toward). */
void waifu_sdl3_mouse_wheel_inject(int notches);

/* Set by the video layer whenever the canvas is rebuilt (resolution change). */
void waifu_sdl3_display_reset_notify(void);

/* 1 once when the game core asked for the frontend options screen. */
int waifu_sdl3_options_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_MOUSE_H */
