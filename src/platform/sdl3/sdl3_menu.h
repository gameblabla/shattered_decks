#ifndef WAIFU_SDL3_MENU_H
#define WAIFU_SDL3_MENU_H

/* Pause / options menu for the PC build.
 *
 * Lives entirely in the frontend: it freezes the game (the caller stops
 * stepping the core while it is active), draws itself through the immediate
 * overlay layer (sdl3_overlay.h) on top of the last presented frame, and edits
 * the settings struct in place — applying video changes to the window at once
 * and writing the file when it closes. The game core knows nothing about it. */

#include "sdl3_settings.h"
#include "sdl3_input.h"
#include "sdl3_video.h"

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WaifuMenu WaifuMenu;

WaifuMenu *waifu_menu_create(WaifuSettings *settings, WaifuSdl3Video *video,
                             WaifuInput *input);
void waifu_menu_destroy(WaifuMenu *menu);

int waifu_menu_active(const WaifuMenu *menu);
void waifu_menu_open(WaifuMenu *menu);
void waifu_menu_close(WaifuMenu *menu);

/* Turns off writing the settings file when the menu closes (scripted runs). */
void waifu_menu_set_persist(WaifuMenu *menu, int on);

/* Feeds one SDL event to the menu (mouse motion / clicks / wheel). Ignored
   while the menu is closed. */
void waifu_menu_handle_event(WaifuMenu *menu, const SDL_Event *ev);

/* One frame of menu: consumes input, applies changes, emits the overlay.
   Returns 0 when the player chose to quit the game. */
int waifu_menu_update(WaifuMenu *menu, int px_w, int px_h);

/* Small always-on overlay elements (FPS counter, transient toasts). Call once
   per frame when the menu is NOT active; `fps` < 0 hides the counter. */
void waifu_menu_draw_hud(WaifuMenu *menu, int px_w, int px_h, float fps);

/* Shows a short message in the corner (e.g. "SCREENSHOT SAVED"). */
void waifu_menu_toast(WaifuMenu *menu, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_MENU_H */
