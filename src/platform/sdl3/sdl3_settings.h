#ifndef WAIFU_SDL3_SETTINGS_H
#define WAIFU_SDL3_SETTINGS_H

/* Persisted player settings for the PC (SDL3) build.
 *
 * The whole options menu edits this one struct; the frontend applies it to the
 * window, the GPU swapchain, the mixer and the input layer.  It is stored as a
 * flat `key value` text file under the platform preferences directory
 * (SDL_GetPrefPath), or wherever WAIFU_SDL3_CONFIG points, so a user can hand
 * edit it and an unknown//corrupt key simply falls back to the default.
 *
 * Nothing here reaches the game core: console targets have no settings file
 * and their behaviour is unchanged. */

#ifdef __cplusplus
extern "C" {
#endif

/* Logical actions.  Everything the game core consumes plus the frontend-only
   system actions, so both keyboard and gamepad bindings live in one table. */
typedef enum WaifuAction {
    WAIFU_ACT_UP = 0,
    WAIFU_ACT_DOWN,
    WAIFU_ACT_LEFT,
    WAIFU_ACT_RIGHT,
    WAIFU_ACT_CONFIRM,      /* game "A" */
    WAIFU_ACT_CANCEL,       /* game "B" */
    WAIFU_ACT_START,        /* game "START"/RUN */
    WAIFU_ACT_ASSIST,       /* game "TAB"/button 4 (card check / storage) */
    WAIFU_ACT_MENU,         /* frontend: open the pause / options menu */
    WAIFU_ACT_FULLSCREEN,   /* frontend: toggle fullscreen */
    WAIFU_ACT_SCREENSHOT,   /* frontend: save a PNG next to the config */
    WAIFU_ACT_COUNT
} WaifuAction;

#define WAIFU_BINDS_PER_ACTION 2

/* Window presentation mode. */
typedef enum WaifuWindowMode {
    WAIFU_WINDOW_WINDOWED = 0,
    WAIFU_WINDOW_BORDERLESS,        /* desktop-sized borderless fullscreen */
    WAIFU_WINDOW_FULLSCREEN         /* exclusive mode at fs_w x fs_h @ fs_hz */
} WaifuWindowMode;

/* How the game view uses a wider-than-native display. */
typedef enum WaifuAspectMode {
    WAIFU_ASPECT_WIDESCREEN = 0,    /* HUD + scenes spread to the real edges */
    WAIFU_ASPECT_PILLARBOX,         /* keep the 256x240 column, black bars */
    WAIFU_ASPECT_STRETCH            /* stretch the column to fill (no bars) */
} WaifuAspectMode;

typedef struct WaifuSettings {
    /* --- video --- */
    int window_w, window_h;         /* windowed size */
    int window_mode;                /* WaifuWindowMode */
    int display_index;              /* 0-based display for fullscreen */
    int fs_w, fs_h;                 /* exclusive fullscreen mode (0 = desktop) */
    float fs_hz;                    /* refresh rate of that mode (0 = any) */
    int vsync;                      /* 1 = vsync, 0 = immediate */
    int fps_cap;                    /* 0 = uncapped, else frames per second */
    int render_scale;               /* 0 = auto (fit), else forced canvas N */
    int aspect_mode;                /* WaifuAspectMode */
    int max_aspect_x10;             /* widest canvas aspect * 10 (e.g. 36 = 32:9) */
    int show_fps;

    /* --- audio (0..100) --- */
    int vol_master, vol_music, vol_sfx;

    /* --- input --- */
    int deadzone;                   /* analog stick deadzone, 0..100 */
    int rumble;                     /* 0/1 */
    int key[WAIFU_ACT_COUNT][WAIFU_BINDS_PER_ACTION];   /* SDL_Scancode, 0 = unbound */
    int pad[WAIFU_ACT_COUNT][WAIFU_BINDS_PER_ACTION];   /* SDL_GamepadButton, -1 = unbound */
} WaifuSettings;

/* Fills `s` with the shipping defaults (never fails). */
void waifu_settings_defaults(WaifuSettings *s);

/* Loads the settings file over the defaults.  Returns 1 if a file was read. */
int waifu_settings_load(WaifuSettings *s);

/* Writes the settings file (creating the preferences directory).  Returns 1
   on success. */
int waifu_settings_save(const WaifuSettings *s);

/* Absolute path of the settings file (stable string, valid for the process). */
const char *waifu_settings_path(void);

/* Directory the settings file lives in — also where screenshots are written. */
const char *waifu_settings_dir(void);

/* Display names for the binding UI. "---" when unbound. */
const char *waifu_settings_key_name(int scancode);
const char *waifu_settings_pad_name(int button);
const char *waifu_settings_action_name(int action);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_SDL3_SETTINGS_H */
