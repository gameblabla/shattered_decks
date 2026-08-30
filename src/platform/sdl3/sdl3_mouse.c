/* Mouse pointer + two small frontend signals. See sdl3_mouse.h. */

#include "sdl3_mouse.h"
#include "platform.h"
#include "defines.h"

#include <SDL3/SDL.h>

/* How long after the last movement or click the pointer still counts as "in
   use". The on-screen buttons appear for that window and then get out of the
   way again, so a pad player never sees them. */
#define POINTER_IDLE_MS 4000
/* Pixels of travel (in game units) before a press counts as a drag rather than
   a click. Small enough that a deliberate flick registers, large enough that a
   shaky click does not. */
#define POINTER_DRAG_SLOP 6

static struct {
    int have;               /* the pointer is inside the presented image */
    int suppressed;
    float gx, gy;           /* game HUD space */
    int down;               /* left button held */
    int pressed, released;  /* pending edges, consumed by the core */
    int rpressed;
    int drag_x, drag_y;     /* where the current press started */
    int moved_far;          /* the press has travelled past the slop */
    Uint64 last_use_ms;
    float last_win_x, last_win_y;
    int win_valid;
    int pending_press, pending_release, pending_rpress;
    int injected;          /* a command script placed the pointer this frame */
} g_ptr;

static int g_display_reset;
static int g_options_request;

void waifu_sdl3_mouse_handle_event(const SDL_Event *ev)
{
    if (!ev) return;
    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
        /* Only real travel counts as "the player is using the mouse": a window
           focus change or a resize can synthesize a motion event at the same
           position, and that must not pop the on-screen buttons up. */
        if (!g_ptr.win_valid ||
            SDL_fabsf(ev->motion.x - g_ptr.last_win_x) > 0.5f ||
            SDL_fabsf(ev->motion.y - g_ptr.last_win_y) > 0.5f) {
            g_ptr.last_win_x = ev->motion.x;
            g_ptr.last_win_y = ev->motion.y;
            g_ptr.win_valid = 1;
            g_ptr.last_use_ms = SDL_GetTicks();
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        g_ptr.last_use_ms = SDL_GetTicks();
        if (ev->button.button == SDL_BUTTON_LEFT) g_ptr.pending_press = 1;
        else if (ev->button.button == SDL_BUTTON_RIGHT) g_ptr.pending_rpress = 1;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        g_ptr.last_use_ms = SDL_GetTicks();
        if (ev->button.button == SDL_BUTTON_LEFT) g_ptr.pending_release = 1;
        break;
    default:
        break;
    }
}

void waifu_sdl3_mouse_inject(int game_x, int game_y, int press, int release, int rclick)
{
    g_ptr.injected = 1;
    g_ptr.gx = (float)game_x;
    g_ptr.gy = (float)game_y;
    g_ptr.last_use_ms = SDL_GetTicks();
    if (g_ptr.last_use_ms == 0) g_ptr.last_use_ms = 1;
    if (press) g_ptr.pending_press = 1;
    if (release) g_ptr.pending_release = 1;
    if (rclick) g_ptr.pending_rpress = 1;
}

void waifu_sdl3_mouse_update(const WaifuSdl3Video *video, int suppressed)
{
    float wx = 0.0f, wy = 0.0f, gx = 0.0f, gy = 0.0f;

    g_ptr.suppressed = suppressed;
    if (g_ptr.injected) {
        g_ptr.have = 1;
        g_ptr.injected = 0;
    } else {
        SDL_GetMouseState(&wx, &wy);
        g_ptr.have = waifu_sdl3_video_window_to_game(video, wx, wy, &gx, &gy);
        if (g_ptr.have) { g_ptr.gx = gx; g_ptr.gy = gy; }
    }

    if (g_ptr.pending_press) {
        g_ptr.down = 1;
        g_ptr.moved_far = 0;
        g_ptr.drag_x = (int)g_ptr.gx;
        g_ptr.drag_y = (int)g_ptr.gy;
        g_ptr.pressed = 1;
        g_ptr.pending_press = 0;
    }
    if (g_ptr.down) {
        int dx = (int)g_ptr.gx - g_ptr.drag_x;
        int dy = (int)g_ptr.gy - g_ptr.drag_y;
        if (dx * dx + dy * dy >= POINTER_DRAG_SLOP * POINTER_DRAG_SLOP) g_ptr.moved_far = 1;
    }
    if (g_ptr.pending_release) {
        g_ptr.released = 1;
        g_ptr.down = 0;
        g_ptr.pending_release = 0;
    }
    if (g_ptr.pending_rpress) {
        g_ptr.rpressed = 1;
        g_ptr.pending_rpress = 0;
    }
    if (suppressed) {
        /* The frontend menu owns the mouse: drop anything queued so a click on
           a menu row is not also played into the game underneath. */
        g_ptr.pressed = g_ptr.released = g_ptr.rpressed = 0;
        g_ptr.down = 0;
    }
}

int waifu_platform_pointer(WaifuPointer *out)
{
    Uint64 now = SDL_GetTicks();
    if (!out) return 0;
    if (!g_ptr.have || g_ptr.suppressed) return 0;
    out->active = (g_ptr.last_use_ms != 0 && now - g_ptr.last_use_ms < POINTER_IDLE_MS);
    out->x = (int)g_ptr.gx;
    out->y = (int)g_ptr.gy;
    out->left_down = g_ptr.down;
    out->left_pressed = g_ptr.pressed;
    out->left_released = g_ptr.released;
    out->right_pressed = g_ptr.rpressed;
    /* A release without travel reads as a click at the press point, so a
       click-through never looks like a zero-length drag. */
    out->drag_x = g_ptr.moved_far ? g_ptr.drag_x : out->x;
    out->drag_y = g_ptr.moved_far ? g_ptr.drag_y : out->y;
    g_ptr.pressed = g_ptr.released = g_ptr.rpressed = 0;
    return 1;
}

void waifu_sdl3_display_reset_notify(void) { g_display_reset = 1; }

int waifu_platform_display_reset(void)
{
    int r = g_display_reset;
    g_display_reset = 0;
    return r;
}

void waifu_platform_open_options(void) { g_options_request = 1; }

int waifu_sdl3_options_requested(void)
{
    int r = g_options_request;
    g_options_request = 0;
    return r;
}
