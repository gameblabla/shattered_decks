/* Persisted PC settings — see sdl3_settings.h.
 *
 * Format is one `key value` per line so the file stays diff-friendly and hand
 * editable; bindings are written as `bind.<action>.key0 <scancode>` etc.  An
 * unrecognized key is ignored, a missing one keeps its default, so an older or
 * newer file never prevents the game from starting. */

#include "sdl3_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

static char g_dir[1024];
static char g_path[1088];

static const char *const g_action_ids[WAIFU_ACT_COUNT] = {
    "up", "down", "left", "right", "confirm", "cancel",
    "start", "assist", "menu", "fullscreen", "screenshot"
};

static const char *const g_action_names[WAIFU_ACT_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "CONFIRM", "CANCEL",
    "START", "CARD CHECK", "OPEN MENU", "FULLSCREEN", "SCREENSHOT"
};

void waifu_settings_defaults(WaifuSettings *s)
{
    int a, b;
    if (!s) return;
    SDL_memset(s, 0, sizeof(*s));

    s->window_w = 1280;
    s->window_h = 720;
    s->window_mode = WAIFU_WINDOW_WINDOWED;
    s->display_index = 0;
    s->fs_w = 0;
    s->fs_h = 0;
    s->fs_hz = 0.0f;
    s->vsync = 1;
    s->fps_cap = 60;
    s->render_scale = 0;
    s->aspect_mode = WAIFU_ASPECT_WIDESCREEN;
    s->max_aspect_x10 = 36;          /* up to 32:9 */
    s->show_fps = 0;

    s->vol_master = 90;
    s->vol_music = 80;
    s->vol_sfx = 100;

    s->deadzone = 25;
    s->rumble = 1;

    for (a = 0; a < WAIFU_ACT_COUNT; ++a)
        for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
            s->key[a][b] = 0;
            s->pad[a][b] = -1;
        }

    /* Keyboard: arrows + WASD for movement, the classic CTRL/ALT/SPACE/TAB
       action set the other frontends use, plus a second modern set. */
    s->key[WAIFU_ACT_UP][0]         = SDL_SCANCODE_UP;
    s->key[WAIFU_ACT_UP][1]         = SDL_SCANCODE_W;
    s->key[WAIFU_ACT_DOWN][0]       = SDL_SCANCODE_DOWN;
    s->key[WAIFU_ACT_DOWN][1]       = SDL_SCANCODE_S;
    s->key[WAIFU_ACT_LEFT][0]       = SDL_SCANCODE_LEFT;
    s->key[WAIFU_ACT_LEFT][1]       = SDL_SCANCODE_A;
    s->key[WAIFU_ACT_RIGHT][0]      = SDL_SCANCODE_RIGHT;
    s->key[WAIFU_ACT_RIGHT][1]      = SDL_SCANCODE_D;
    s->key[WAIFU_ACT_CONFIRM][0]    = SDL_SCANCODE_LCTRL;
    s->key[WAIFU_ACT_CONFIRM][1]    = SDL_SCANCODE_RETURN;
    s->key[WAIFU_ACT_CANCEL][0]     = SDL_SCANCODE_LALT;
    s->key[WAIFU_ACT_CANCEL][1]     = SDL_SCANCODE_BACKSPACE;
    s->key[WAIFU_ACT_START][0]      = SDL_SCANCODE_SPACE;
    s->key[WAIFU_ACT_START][1]      = SDL_SCANCODE_RETURN;
    s->key[WAIFU_ACT_ASSIST][0]     = SDL_SCANCODE_TAB;
    s->key[WAIFU_ACT_ASSIST][1]     = SDL_SCANCODE_LSHIFT;
    s->key[WAIFU_ACT_MENU][0]       = SDL_SCANCODE_ESCAPE;
    s->key[WAIFU_ACT_MENU][1]       = SDL_SCANCODE_F1;
    s->key[WAIFU_ACT_FULLSCREEN][0] = SDL_SCANCODE_F11;
    s->key[WAIFU_ACT_SCREENSHOT][0] = SDL_SCANCODE_F12;

    /* Gamepad: face buttons in the console layout the game was designed for. */
    s->pad[WAIFU_ACT_UP][0]      = SDL_GAMEPAD_BUTTON_DPAD_UP;
    s->pad[WAIFU_ACT_DOWN][0]    = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    s->pad[WAIFU_ACT_LEFT][0]    = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    s->pad[WAIFU_ACT_RIGHT][0]   = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    s->pad[WAIFU_ACT_CONFIRM][0] = SDL_GAMEPAD_BUTTON_SOUTH;
    s->pad[WAIFU_ACT_CANCEL][0]  = SDL_GAMEPAD_BUTTON_EAST;
    s->pad[WAIFU_ACT_START][0]   = SDL_GAMEPAD_BUTTON_START;
    s->pad[WAIFU_ACT_ASSIST][0]  = SDL_GAMEPAD_BUTTON_WEST;
    s->pad[WAIFU_ACT_ASSIST][1]  = SDL_GAMEPAD_BUTTON_NORTH;
    s->pad[WAIFU_ACT_MENU][0]    = SDL_GAMEPAD_BUTTON_BACK;
}

static void resolve_paths(void)
{
    const char *env;
    if (g_path[0]) return;
    env = SDL_getenv("WAIFU_SDL3_CONFIG");
    if (env && env[0]) {
        SDL_snprintf(g_path, sizeof(g_path), "%s", env);
        SDL_snprintf(g_dir, sizeof(g_dir), "%s", env);
        {   /* strip the file name to get the directory */
            size_t i = SDL_strlen(g_dir);
            while (i > 0 && g_dir[i - 1] != '/' && g_dir[i - 1] != '\\') --i;
            g_dir[i] = '\0';
        }
        if (!g_dir[0]) SDL_snprintf(g_dir, sizeof(g_dir), "./");
        return;
    }
    {
        char *pref = SDL_GetPrefPath("ShatteredDecks", "ShatteredDecks");
        if (pref) {
            SDL_snprintf(g_dir, sizeof(g_dir), "%s", pref);
            SDL_free(pref);
        } else {
            SDL_snprintf(g_dir, sizeof(g_dir), "./");
        }
    }
    SDL_snprintf(g_path, sizeof(g_path), "%ssettings.cfg", g_dir);
}

const char *waifu_settings_path(void) { resolve_paths(); return g_path; }
const char *waifu_settings_dir(void)  { resolve_paths(); return g_dir; }

/* --- parsing ------------------------------------------------------------- */

static int bind_key_index(const char *key, const char *prefix,
                          int *action, int *slot, int *is_pad)
{
    /* "bind.<action>.key0" / "bind.<action>.pad1" */
    size_t plen = SDL_strlen(prefix);
    const char *rest;
    int a;
    if (SDL_strncmp(key, prefix, plen) != 0) return 0;
    rest = key + plen;
    for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
        size_t alen = SDL_strlen(g_action_ids[a]);
        if (SDL_strncmp(rest, g_action_ids[a], alen) == 0 && rest[alen] == '.') {
            const char *tail = rest + alen + 1;
            if (SDL_strncmp(tail, "key", 3) == 0) *is_pad = 0;
            else if (SDL_strncmp(tail, "pad", 3) == 0) *is_pad = 1;
            else return 0;
            *slot = tail[3] - '0';
            if (*slot < 0 || *slot >= WAIFU_BINDS_PER_ACTION) return 0;
            *action = a;
            return 1;
        }
    }
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void apply_kv(WaifuSettings *s, const char *key, const char *val)
{
    int action, slot, is_pad;
    int iv = SDL_atoi(val);

    if (!SDL_strcmp(key, "window.width"))        s->window_w = clampi(iv, 320, 16384);
    else if (!SDL_strcmp(key, "window.height"))  s->window_h = clampi(iv, 240, 16384);
    else if (!SDL_strcmp(key, "window.mode"))    s->window_mode = clampi(iv, 0, 2);
    else if (!SDL_strcmp(key, "display.index"))  s->display_index = clampi(iv, 0, 63);
    else if (!SDL_strcmp(key, "fullscreen.width"))  s->fs_w = clampi(iv, 0, 16384);
    else if (!SDL_strcmp(key, "fullscreen.height")) s->fs_h = clampi(iv, 0, 16384);
    else if (!SDL_strcmp(key, "fullscreen.hz"))  s->fs_hz = (float)SDL_atof(val);
    else if (!SDL_strcmp(key, "video.vsync"))    s->vsync = iv ? 1 : 0;
    else if (!SDL_strcmp(key, "video.fps_cap"))  s->fps_cap = clampi(iv, 0, 1000);
    else if (!SDL_strcmp(key, "video.render_scale")) s->render_scale = clampi(iv, 0, 16);
    else if (!SDL_strcmp(key, "video.aspect"))   s->aspect_mode = clampi(iv, 0, 2);
    else if (!SDL_strcmp(key, "video.max_aspect_x10")) s->max_aspect_x10 = clampi(iv, 13, 40);
    else if (!SDL_strcmp(key, "video.show_fps")) s->show_fps = iv ? 1 : 0;
    else if (!SDL_strcmp(key, "audio.master"))   s->vol_master = clampi(iv, 0, 100);
    else if (!SDL_strcmp(key, "audio.music"))    s->vol_music = clampi(iv, 0, 100);
    else if (!SDL_strcmp(key, "audio.sfx"))      s->vol_sfx = clampi(iv, 0, 100);
    else if (!SDL_strcmp(key, "input.deadzone")) s->deadzone = clampi(iv, 0, 90);
    else if (!SDL_strcmp(key, "input.rumble"))   s->rumble = iv ? 1 : 0;
    else if (bind_key_index(key, "bind.", &action, &slot, &is_pad)) {
        if (is_pad) s->pad[action][slot] = clampi(iv, -1, SDL_GAMEPAD_BUTTON_COUNT - 1);
        else        s->key[action][slot] = clampi(iv, 0, SDL_SCANCODE_COUNT - 1);
    }
}

int waifu_settings_load(WaifuSettings *s)
{
    FILE *fp;
    char line[256];
    if (!s) return 0;
    waifu_settings_defaults(s);
    resolve_paths();
    fp = fopen(g_path, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line, *key, *val;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        key = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
        if (!*p) continue;
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') ++p;
        val = p;
        while (*p && *p != '\n' && *p != '\r') ++p;
        *p = '\0';
        apply_kv(s, key, val);
    }
    fclose(fp);
    return 1;
}

int waifu_settings_save(const WaifuSettings *s)
{
    FILE *fp;
    int a, b;
    if (!s) return 0;
    resolve_paths();
    SDL_CreateDirectory(g_dir);
    fp = fopen(g_path, "w");
    if (!fp) return 0;
    fprintf(fp, "# Shattered Decks - PC settings\n");
    fprintf(fp, "window.width %d\n", s->window_w);
    fprintf(fp, "window.height %d\n", s->window_h);
    fprintf(fp, "window.mode %d\n", s->window_mode);
    fprintf(fp, "display.index %d\n", s->display_index);
    fprintf(fp, "fullscreen.width %d\n", s->fs_w);
    fprintf(fp, "fullscreen.height %d\n", s->fs_h);
    fprintf(fp, "fullscreen.hz %.3f\n", (double)s->fs_hz);
    fprintf(fp, "video.vsync %d\n", s->vsync);
    fprintf(fp, "video.fps_cap %d\n", s->fps_cap);
    fprintf(fp, "video.render_scale %d\n", s->render_scale);
    fprintf(fp, "video.aspect %d\n", s->aspect_mode);
    fprintf(fp, "video.max_aspect_x10 %d\n", s->max_aspect_x10);
    fprintf(fp, "video.show_fps %d\n", s->show_fps);
    fprintf(fp, "audio.master %d\n", s->vol_master);
    fprintf(fp, "audio.music %d\n", s->vol_music);
    fprintf(fp, "audio.sfx %d\n", s->vol_sfx);
    fprintf(fp, "input.deadzone %d\n", s->deadzone);
    fprintf(fp, "input.rumble %d\n", s->rumble);
    for (a = 0; a < WAIFU_ACT_COUNT; ++a)
        for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
            fprintf(fp, "bind.%s.key%d %d\n", g_action_ids[a], b, s->key[a][b]);
            fprintf(fp, "bind.%s.pad%d %d\n", g_action_ids[a], b, s->pad[a][b]);
        }
    return fclose(fp) == 0;
}

/* --- display names -------------------------------------------------------- */

const char *waifu_settings_key_name(int scancode)
{
    /* Upper-cased into a small rotating buffer: SDL names keys in mixed case
       ("Left Ctrl") and the rest of the UI is upper-case. Rotating lets a
       caller format two names in one expression. */
    static char buf[4][40];
    static int slot = 0;
    const char *name;
    char *out;
    int i;

    if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT) return "---";
    name = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (!name || !name[0]) return "???";
    out = buf[slot];
    slot = (slot + 1) & 3;
    for (i = 0; name[i] && i < (int)sizeof(buf[0]) - 1; ++i)
        out[i] = (char)SDL_toupper((unsigned char)name[i]);
    out[i] = '\0';
    return out;
}

const char *waifu_settings_pad_name(int button)
{
    static const char *const names[] = {
        "A / CROSS", "B / CIRCLE", "X / SQUARE", "Y / TRIANGLE",
        "BACK", "GUIDE", "START",
        "LEFT STICK", "RIGHT STICK", "L SHOULDER", "R SHOULDER",
        "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
        "MISC1", "RIGHT PADDLE1", "LEFT PADDLE1", "RIGHT PADDLE2",
        "LEFT PADDLE2", "TOUCHPAD", "MISC2", "MISC3", "MISC4",
        "MISC5", "MISC6"
    };
    if (button < 0 || button >= (int)(sizeof(names) / sizeof(names[0]))) return "---";
    return names[button];
}

const char *waifu_settings_action_name(int action)
{
    if (action < 0 || action >= WAIFU_ACT_COUNT) return "";
    return g_action_names[action];
}
