/* Pause / options menu — see sdl3_menu.h.
 *
 * Presentation follows the game's own Egyptian gold-on-deep-blue look so the
 * menu reads as part of the product rather than a debug panel: a dimming scrim
 * over the frozen frame, a bordered panel with a gold rule under the title, one
 * highlighted row at a time, values right-aligned, and a footer that always
 * says which button does what. */

#include "sdl3_menu.h"
#include "sdl3_overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sounds.h"
#include "game_api.h"

/* --- palette (linear 0..1 RGB, matching the game's UI colours) ------------- */
static const float COL_GOLD[4]     = { 0.91f, 0.76f, 0.35f, 1.00f };
static const float COL_GOLD_DIM[4] = { 0.55f, 0.45f, 0.20f, 1.00f };
static const float COL_TEXT[4]     = { 0.94f, 0.93f, 0.89f, 1.00f };
static const float COL_TEXT_DIM[4] = { 0.55f, 0.58f, 0.66f, 1.00f };
static const float COL_PANEL_T[4]  = { 0.055f, 0.075f, 0.20f, 0.96f };
static const float COL_PANEL_B[4]  = { 0.020f, 0.030f, 0.09f, 0.96f };
static const float COL_SEL[4]      = { 0.20f, 0.26f, 0.52f, 0.85f };

#define PANEL_W      470.0f
#define PANEL_PAD     18.0f
#define ROW_H         17.0f
#define TITLE_SIZE    15.0f
#define ITEM_SIZE     10.5f
#define FOOT_SIZE      7.5f
#define MAX_VISIBLE   14

typedef enum MenuPage {
    PAGE_ROOT = 0,
    PAGE_OPTIONS,
    PAGE_VIDEO,
    PAGE_AUDIO,
    PAGE_CONTROLS,
    PAGE_QUIT,
    PAGE_COUNT
} MenuPage;

struct WaifuMenu {
    WaifuSettings *cfg;
    WaifuSdl3Video *video;
    WaifuInput *input;

    int active;
    int page;
    int cursor[PAGE_COUNT];
    int scroll;
    int open_frames;             /* drives the fade/slide-in */
    int rebind_action;           /* >= 0 while waiting for a key/button */
    int quit;
    int save_on_close;

    char toast[64];
    int toast_frames;

    /* Mouse: the panel/row rectangles the last draw produced, in overlay
       units, so hovering picks a row and a click acts on it. */
    float hit_x0, hit_x1;
    float hit_y0, hit_row_h;
    int hit_first, hit_rows;
    int mouse_row;               /* -1 = pointer not over a row */
    int mouse_click;             /* pending left click on mouse_row */
    int mouse_click_left_half;   /* click landed on the left half (decrease) */
    int mouse_wheel;             /* pending wheel steps (+down) */
    int mouse_active;            /* pointer moved recently: draw the hover */

    /* Windowed-resolution presets, filtered to what the display can show. */
    int preset_w[16], preset_h[16];
    int preset_count;
};

/* --- helpers -------------------------------------------------------------- */

static void beep(int confirm)
{
    waifu_sound_play(confirm ? WAIFU_SOUND_CONFIRM : WAIFU_SOUND_SELECT);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void apply_audio(const WaifuSettings *s)
{
    waifu_fm_audio_set_volumes(s->vol_master * 256 / 100,
                               s->vol_music * 256 / 100,
                               s->vol_sfx * 256 / 100);
}

static void build_presets(WaifuMenu *m)
{
    static const int cand[][2] = {
        { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 },
        { 2560, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 1600 },
        { 3840, 2160 }, { 1024, 768 }, { 1280, 960 }
    };
    const SDL_DisplayMode *desktop;
    int max_w = 1 << 20, max_h = 1 << 20, i;
    SDL_DisplayID *ids;
    int count = 0;

    ids = SDL_GetDisplays(&count);
    if (ids) {
        int idx = clampi(m->cfg->display_index, 0, count > 0 ? count - 1 : 0);
        if (count > 0) {
            desktop = SDL_GetDesktopDisplayMode(ids[idx]);
            if (desktop) { max_w = desktop->w; max_h = desktop->h; }
        }
        SDL_free(ids);
    }
    m->preset_count = 0;
    for (i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); ++i) {
        if (cand[i][0] > max_w || cand[i][1] > max_h) continue;
        if (m->preset_count >= (int)(sizeof(m->preset_w) / sizeof(m->preset_w[0]))) break;
        m->preset_w[m->preset_count] = cand[i][0];
        m->preset_h[m->preset_count] = cand[i][1];
        m->preset_count++;
    }
    if (m->preset_count == 0) {   /* tiny/unknown display: offer the minimum */
        m->preset_w[0] = 1280; m->preset_h[0] = 720; m->preset_count = 1;
    }
}

WaifuMenu *waifu_menu_create(WaifuSettings *settings, WaifuSdl3Video *video,
                             WaifuInput *input)
{
    WaifuMenu *m = (WaifuMenu *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->cfg = settings;
    m->video = video;
    m->input = input;
    m->rebind_action = -1;
    m->save_on_close = 1;
    build_presets(m);
    apply_audio(settings);
    return m;
}

void waifu_menu_destroy(WaifuMenu *m) { free(m); }

int waifu_menu_active(const WaifuMenu *m) { return m && m->active; }

void waifu_menu_open(WaifuMenu *m)
{
    if (!m || m->active) return;
    m->active = 1;
    m->page = PAGE_ROOT;
    m->scroll = 0;
    m->open_frames = 0;
    m->rebind_action = -1;
    beep(1);
}

/* Same, but landing straight on the options list: the title screen's OPTIONS
   row should not make the player walk through the pause root first. */
void waifu_menu_open_options(WaifuMenu *m)
{
    waifu_menu_open(m);
    if (!m) return;
    m->page = PAGE_OPTIONS;
    m->cursor[PAGE_OPTIONS] = 0;
    m->scroll = 0;
}

void waifu_menu_close(WaifuMenu *m)
{
    if (!m || !m->active) return;
    m->active = 0;
    m->rebind_action = -1;
    waifu_input_capture_cancel(m->input);
    if (m->save_on_close) waifu_settings_save(m->cfg);
    waifu_sdl3_overlay_clear();
}

/* A scripted capture drives the menu without touching the player's file. */
void waifu_menu_set_persist(WaifuMenu *m, int on)
{
    if (m) m->save_on_close = on ? 1 : 0;
}

void waifu_menu_toast(WaifuMenu *m, const char *text)
{
    if (!m || !text) return;
    SDL_snprintf(m->toast, sizeof(m->toast), "%s", text);
    m->toast_frames = 120;
}

/* --- item model -----------------------------------------------------------
   Each page builds its rows into this small array every frame: label, value
   text and what the row does. Rebuilding per frame keeps the values always in
   sync with the settings with no invalidation logic. */

typedef enum RowKind {
    ROW_ACTION = 0,     /* A activates */
    ROW_CHOICE,         /* LEFT/RIGHT (and A) cycle */
    ROW_SLIDER,         /* LEFT/RIGHT adjust, drawn with a bar */
    ROW_BIND,           /* A starts capture */
    ROW_INFO            /* not selectable */
} RowKind;

typedef struct Row {
    char label[48];
    char value[64];
    int kind;
    int id;
    float frac;         /* ROW_SLIDER fill 0..1 */
} Row;

#define MAX_ROWS 40

typedef struct RowList {
    Row rows[MAX_ROWS];
    int count;
} RowList;

static Row *row_add(RowList *rl, int kind, int id, const char *label, const char *value)
{
    Row *r;
    if (rl->count >= MAX_ROWS) return &rl->rows[MAX_ROWS - 1];
    r = &rl->rows[rl->count++];
    SDL_snprintf(r->label, sizeof(r->label), "%s", label ? label : "");
    SDL_snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
    r->kind = kind;
    r->id = id;
    r->frac = 0.0f;
    return r;
}

/* Row ids per page (kept local to each build_*). */
enum { ID_RESUME = 0, ID_OPTIONS, ID_QUIT_TITLE, ID_QUIT_APP };
enum { ID_CAT_VIDEO = 0, ID_CAT_AUDIO, ID_CAT_CONTROLS, ID_BACK };
enum { ID_WINMODE = 0, ID_RESOLUTION, ID_MONITOR, ID_VSYNC, ID_FPSCAP,
       ID_ASPECT, ID_MAXASPECT, ID_RENDERSCALE, ID_SHOWFPS, ID_VBACK };
enum { ID_MASTER = 0, ID_MUSIC, ID_SFX, ID_ABACK };

static const char *winmode_name(int m)
{
    switch (m) {
    case WAIFU_WINDOW_BORDERLESS: return "BORDERLESS";
    case WAIFU_WINDOW_FULLSCREEN: return "FULLSCREEN";
    default: return "WINDOWED";
    }
}

static const char *aspect_name(int m)
{
    switch (m) {
    case WAIFU_ASPECT_PILLARBOX: return "PILLARBOX";
    case WAIFU_ASPECT_STRETCH:   return "STRETCH";
    default: return "WIDESCREEN";
    }
}

static void max_aspect_label(int x10, char *out, size_t n)
{
    if (x10 >= 35)      SDL_snprintf(out, n, "32:9");
    else if (x10 >= 23) SDL_snprintf(out, n, "21:9");
    else if (x10 >= 17) SDL_snprintf(out, n, "16:9");
    else                SDL_snprintf(out, n, "4:3");
}

/* Index of the settings' resolution inside the current choice list. */
static int current_res_index(const WaifuMenu *m)
{
    int i, count;
    if (m->cfg->window_mode == WAIFU_WINDOW_FULLSCREEN) {
        count = waifu_sdl3_video_mode_count(m->cfg->display_index);
        for (i = 0; i < count; ++i) {
            int w = 0, h = 0;
            if (waifu_sdl3_video_mode(m->cfg->display_index, i, &w, &h, NULL) &&
                w == m->cfg->fs_w && h == m->cfg->fs_h)
                return i;
        }
        return 0;
    }
    for (i = 0; i < m->preset_count; ++i)
        if (m->preset_w[i] == m->cfg->window_w && m->preset_h[i] == m->cfg->window_h)
            return i;
    return -1;
}

static void build_video(WaifuMenu *m, RowList *rl)
{
    char buf[64];
    Row *r;

    row_add(rl, ROW_CHOICE, ID_WINMODE, "DISPLAY MODE", winmode_name(m->cfg->window_mode));

    if (m->cfg->window_mode == WAIFU_WINDOW_BORDERLESS) {
        int w = 0, h = 0;
        waifu_sdl3_video_window_size(m->video, &w, &h);
        SDL_snprintf(buf, sizeof(buf), "%d X %d (DESKTOP)", w, h);
        row_add(rl, ROW_INFO, ID_RESOLUTION, "RESOLUTION", buf);
    } else if (m->cfg->window_mode == WAIFU_WINDOW_FULLSCREEN) {
        int idx = current_res_index(m), w = 0, h = 0;
        float hz = 0.0f;
        if (waifu_sdl3_video_mode(m->cfg->display_index, idx, &w, &h, &hz) && w > 0)
            SDL_snprintf(buf, sizeof(buf), "%d X %d @ %dHZ", w, h, (int)(hz + 0.5f));
        else
            SDL_snprintf(buf, sizeof(buf), "DESKTOP");
        row_add(rl, ROW_CHOICE, ID_RESOLUTION, "RESOLUTION", buf);
    } else {
        SDL_snprintf(buf, sizeof(buf), "%d X %d", m->cfg->window_w, m->cfg->window_h);
        row_add(rl, ROW_CHOICE, ID_RESOLUTION, "WINDOW SIZE", buf);
    }

    if (waifu_sdl3_video_display_count() > 1)
        row_add(rl, ROW_CHOICE, ID_MONITOR, "MONITOR",
                waifu_sdl3_video_display_name(m->cfg->display_index));

    row_add(rl, ROW_CHOICE, ID_VSYNC, "V-SYNC", m->cfg->vsync ? "ON" : "OFF");

    if (m->cfg->fps_cap <= 0) SDL_snprintf(buf, sizeof(buf), "UNLIMITED");
    else SDL_snprintf(buf, sizeof(buf), "%d FPS", m->cfg->fps_cap);
    row_add(rl, ROW_CHOICE, ID_FPSCAP, "FRAME LIMIT", buf);

    row_add(rl, ROW_CHOICE, ID_ASPECT, "SCREEN FIT", aspect_name(m->cfg->aspect_mode));

    max_aspect_label(m->cfg->max_aspect_x10, buf, sizeof(buf));
    r = row_add(rl, ROW_CHOICE, ID_MAXASPECT, "WIDEST VIEW", buf);
    if (m->cfg->aspect_mode != WAIFU_ASPECT_WIDESCREEN) r->kind = ROW_INFO;

    if (m->cfg->render_scale <= 0) SDL_snprintf(buf, sizeof(buf), "AUTO");
    else SDL_snprintf(buf, sizeof(buf), "%dX", m->cfg->render_scale);
    row_add(rl, ROW_CHOICE, ID_RENDERSCALE, "RENDER SCALE", buf);

    row_add(rl, ROW_CHOICE, ID_SHOWFPS, "SHOW FPS", m->cfg->show_fps ? "ON" : "OFF");
    row_add(rl, ROW_ACTION, ID_VBACK, "BACK", "");
}

static void build_audio(WaifuMenu *m, RowList *rl)
{
    char buf[32];
    Row *r;
    SDL_snprintf(buf, sizeof(buf), "%d", m->cfg->vol_master);
    r = row_add(rl, ROW_SLIDER, ID_MASTER, "MASTER VOLUME", buf);
    r->frac = m->cfg->vol_master / 100.0f;
    SDL_snprintf(buf, sizeof(buf), "%d", m->cfg->vol_music);
    r = row_add(rl, ROW_SLIDER, ID_MUSIC, "MUSIC VOLUME", buf);
    r->frac = m->cfg->vol_music / 100.0f;
    SDL_snprintf(buf, sizeof(buf), "%d", m->cfg->vol_sfx);
    r = row_add(rl, ROW_SLIDER, ID_SFX, "EFFECTS VOLUME", buf);
    r->frac = m->cfg->vol_sfx / 100.0f;
    row_add(rl, ROW_ACTION, ID_ABACK, "BACK", "");
}

/* Controls page rows: one per bindable action, then the pad options. */
enum { CTL_FIRST_BIND = 0,
       CTL_DEADZONE = 100, CTL_RUMBLE, CTL_DEFAULTS, CTL_CBACK };

static void build_controls(WaifuMenu *m, RowList *rl)
{
    char buf[64];
    Row *r;
    int a, pads;

    for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
        const char *key = waifu_settings_key_name(m->cfg->key[a][0]);
        const char *pad = waifu_settings_pad_name(m->cfg->pad[a][0]);
        if (m->rebind_action == a)
            SDL_snprintf(buf, sizeof(buf), "PRESS ANY KEY OR BUTTON");
        else
            SDL_snprintf(buf, sizeof(buf), "%s  /  %s", key, pad);
        row_add(rl, ROW_BIND, CTL_FIRST_BIND + a, waifu_settings_action_name(a), buf);
    }

    SDL_snprintf(buf, sizeof(buf), "%d%%", m->cfg->deadzone);
    r = row_add(rl, ROW_SLIDER, CTL_DEADZONE, "STICK DEADZONE", buf);
    r->frac = m->cfg->deadzone / 90.0f;
    row_add(rl, ROW_CHOICE, CTL_RUMBLE, "VIBRATION", m->cfg->rumble ? "ON" : "OFF");

    pads = waifu_input_pad_count(m->input);
    if (pads > 0) SDL_snprintf(buf, sizeof(buf), "%s", waifu_input_pad_name(m->input, 0));
    else SDL_snprintf(buf, sizeof(buf), "KEYBOARD ONLY");
    row_add(rl, ROW_INFO, -1, "CONTROLLER", buf);

    row_add(rl, ROW_ACTION, CTL_DEFAULTS, "RESET TO DEFAULTS", "");
    row_add(rl, ROW_ACTION, CTL_CBACK, "BACK", "");
}

static void build_rows(WaifuMenu *m, RowList *rl, const char **title)
{
    rl->count = 0;
    switch (m->page) {
    case PAGE_OPTIONS:
        *title = "OPTIONS";
        row_add(rl, ROW_ACTION, ID_CAT_VIDEO, "VIDEO", "");
        row_add(rl, ROW_ACTION, ID_CAT_AUDIO, "AUDIO", "");
        row_add(rl, ROW_ACTION, ID_CAT_CONTROLS, "CONTROLS", "");
        row_add(rl, ROW_ACTION, ID_BACK, "BACK", "");
        break;
    case PAGE_VIDEO:
        *title = "VIDEO";
        build_video(m, rl);
        break;
    case PAGE_AUDIO:
        *title = "AUDIO";
        build_audio(m, rl);
        break;
    case PAGE_CONTROLS:
        *title = "CONTROLS";
        build_controls(m, rl);
        break;
    case PAGE_QUIT:
        *title = "QUIT GAME?";
        row_add(rl, ROW_INFO, -1, "PROGRESS SINCE THE LAST SAVE IS LOST.", "");
        row_add(rl, ROW_ACTION, ID_QUIT_TITLE, "KEEP PLAYING", "");
        row_add(rl, ROW_ACTION, ID_QUIT_APP, "QUIT TO DESKTOP", "");
        break;
    default:
        *title = "PAUSED";
        row_add(rl, ROW_ACTION, ID_RESUME, "RESUME", "");
        row_add(rl, ROW_ACTION, ID_OPTIONS, "OPTIONS", "");
        row_add(rl, ROW_ACTION, ID_QUIT_APP, "QUIT GAME", "");
        break;
    }
}

/* --- editing --------------------------------------------------------------- */

static void video_changed(WaifuMenu *m)
{
    waifu_sdl3_video_apply_settings(m->video);
    build_presets(m);
}

static void cycle_int(int *v, int dir, const int *list, int count)
{
    int i, idx = 0;
    for (i = 0; i < count; ++i) if (list[i] == *v) { idx = i; break; }
    idx = (idx + dir + count) % count;
    *v = list[idx];
}

static void adjust_video(WaifuMenu *m, int id, int dir)
{
    static const int caps[] = { 0, 30, 60, 75, 120, 144, 165, 240 };
    static const int aspects[] = { 13, 18, 24, 36 };
    switch (id) {
    case ID_WINMODE:
        m->cfg->window_mode = (m->cfg->window_mode + dir + 3) % 3;
        video_changed(m);
        break;
    case ID_RESOLUTION:
        if (m->cfg->window_mode == WAIFU_WINDOW_FULLSCREEN) {
            int count = waifu_sdl3_video_mode_count(m->cfg->display_index);
            int idx = current_res_index(m);
            if (count > 0) {
                idx = (idx + dir + count) % count;
                waifu_sdl3_video_mode(m->cfg->display_index, idx,
                                      &m->cfg->fs_w, &m->cfg->fs_h, &m->cfg->fs_hz);
                video_changed(m);
            }
        } else if (m->cfg->window_mode == WAIFU_WINDOW_WINDOWED) {
            int idx = current_res_index(m);
            if (idx < 0) idx = 0;
            else idx = (idx + dir + m->preset_count) % m->preset_count;
            m->cfg->window_w = m->preset_w[idx];
            m->cfg->window_h = m->preset_h[idx];
            video_changed(m);
        }
        break;
    case ID_MONITOR: {
        int count = waifu_sdl3_video_display_count();
        if (count > 0) {
            m->cfg->display_index = (m->cfg->display_index + dir + count) % count;
            m->cfg->fs_w = m->cfg->fs_h = 0;   /* mode list differs per display */
            video_changed(m);
        }
        break;
    }
    case ID_VSYNC:
        m->cfg->vsync = !m->cfg->vsync;
        video_changed(m);
        break;
    case ID_FPSCAP:
        cycle_int(&m->cfg->fps_cap, dir, caps, (int)(sizeof(caps) / sizeof(caps[0])));
        break;
    case ID_ASPECT:
        m->cfg->aspect_mode = (m->cfg->aspect_mode + dir + 3) % 3;
        video_changed(m);
        break;
    case ID_MAXASPECT:
        cycle_int(&m->cfg->max_aspect_x10, dir, aspects,
                  (int)(sizeof(aspects) / sizeof(aspects[0])));
        video_changed(m);
        break;
    case ID_RENDERSCALE:
        m->cfg->render_scale += dir;
        if (m->cfg->render_scale < 0) m->cfg->render_scale = 12;
        if (m->cfg->render_scale > 12) m->cfg->render_scale = 0;
        video_changed(m);
        break;
    case ID_SHOWFPS:
        m->cfg->show_fps = !m->cfg->show_fps;
        break;
    default:
        break;
    }
}

static void adjust_audio(WaifuMenu *m, int id, int dir)
{
    int *v = (id == ID_MASTER) ? &m->cfg->vol_master
           : (id == ID_MUSIC)  ? &m->cfg->vol_music
                               : &m->cfg->vol_sfx;
    *v = clampi(*v + dir * 5, 0, 100);
    apply_audio(m->cfg);
}

static void adjust_controls(WaifuMenu *m, int id, int dir)
{
    if (id == CTL_DEADZONE) {
        m->cfg->deadzone = clampi(m->cfg->deadzone + dir * 5, 0, 90);
    } else if (id == CTL_RUMBLE) {
        m->cfg->rumble = !m->cfg->rumble;
        if (m->cfg->rumble) waifu_input_rumble(m->input, 0.6f, 180);
    }
}

/* Clears any other action that already owns a key/button, so a binding is
   never ambiguous. */
static void unbind_elsewhere(WaifuSettings *s, int action, int code, int is_pad)
{
    int a, b;
    for (a = 0; a < WAIFU_ACT_COUNT; ++a) {
        if (a == action) continue;
        for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
            if (is_pad) { if (s->pad[a][b] == code) s->pad[a][b] = -1; }
            else        { if (s->key[a][b] == code) s->key[a][b] = 0; }
        }
    }
}

static void activate(WaifuMenu *m, const Row *row)
{
    switch (m->page) {
    case PAGE_ROOT:
        if (row->id == ID_RESUME) { beep(1); waifu_menu_close(m); }
        else if (row->id == ID_OPTIONS) { beep(1); m->page = PAGE_OPTIONS; m->scroll = 0; }
        else if (row->id == ID_QUIT_APP) { beep(1); m->page = PAGE_QUIT; m->cursor[PAGE_QUIT] = 1; m->scroll = 0; }
        break;
    case PAGE_QUIT:
        beep(1);
        if (row->id == ID_QUIT_APP) m->quit = 1;
        else m->page = PAGE_ROOT;
        m->scroll = 0;
        break;
    case PAGE_OPTIONS:
        beep(1);
        if (row->id == ID_CAT_VIDEO) m->page = PAGE_VIDEO;
        else if (row->id == ID_CAT_AUDIO) m->page = PAGE_AUDIO;
        else if (row->id == ID_CAT_CONTROLS) m->page = PAGE_CONTROLS;
        else m->page = PAGE_ROOT;
        m->scroll = 0;
        break;
    case PAGE_VIDEO:
        if (row->id == ID_VBACK) { beep(1); m->page = PAGE_OPTIONS; m->scroll = 0; }
        else adjust_video(m, row->id, 1);
        break;
    case PAGE_AUDIO:
        if (row->id == ID_ABACK) { beep(1); m->page = PAGE_OPTIONS; m->scroll = 0; }
        break;
    case PAGE_CONTROLS:
        if (row->id == CTL_CBACK) { beep(1); m->page = PAGE_OPTIONS; m->scroll = 0; }
        else if (row->id == CTL_DEFAULTS) {
            WaifuSettings def;
            int a, b;
            waifu_settings_defaults(&def);
            for (a = 0; a < WAIFU_ACT_COUNT; ++a)
                for (b = 0; b < WAIFU_BINDS_PER_ACTION; ++b) {
                    m->cfg->key[a][b] = def.key[a][b];
                    m->cfg->pad[a][b] = def.pad[a][b];
                }
            m->cfg->deadzone = def.deadzone;
            m->cfg->rumble = def.rumble;
            beep(1);
            waifu_menu_toast(m, "CONTROLS RESET");
        } else if (row->id == CTL_RUMBLE) {
            adjust_controls(m, row->id, 1);
        } else if (row->id >= CTL_FIRST_BIND && row->id < CTL_FIRST_BIND + WAIFU_ACT_COUNT) {
            m->rebind_action = row->id - CTL_FIRST_BIND;
            waifu_input_capture_begin(m->input);
            beep(1);
        }
        break;
    default:
        break;
    }
}

/* --- drawing --------------------------------------------------------------- */

static void draw_text_shadow(float x, float y, float size, const char *s, const float c[4])
{
    waifu_sdl3_overlay_text(x + size * 0.09f, y + size * 0.09f, size, s, 0.0f, 0.0f, 0.0f, c[3] * 0.75f);
    waifu_sdl3_overlay_text(x, y, size, s, c[0], c[1], c[2], c[3]);
}

static void draw_page(WaifuMenu *m, const RowList *rl, const char *title, float alpha)
{
    const float sw = waifu_sdl3_overlay_width();
    float panel_w = PANEL_W;
    float panel_h, x, y, rowy;
    int visible = rl->count < MAX_VISIBLE ? rl->count : MAX_VISIBLE;
    int cursor = m->cursor[m->page];
    int i;
    float top[4], bot[4], sel[4], gold[4], text[4], dim[4];

    if (panel_w > sw - 40.0f) panel_w = sw - 40.0f;
    panel_h = PANEL_PAD * 2.0f + 30.0f + visible * ROW_H + 16.0f;
    x = (sw - panel_w) * 0.5f;
    y = (WAIFU_OVERLAY_H - panel_h) * 0.5f;

    memcpy(top, COL_PANEL_T, sizeof(top));
    memcpy(bot, COL_PANEL_B, sizeof(bot));
    memcpy(sel, COL_SEL, sizeof(sel));
    memcpy(gold, COL_GOLD, sizeof(gold));
    memcpy(text, COL_TEXT, sizeof(text));
    memcpy(dim, COL_TEXT_DIM, sizeof(dim));
    top[3] *= alpha; bot[3] *= alpha; sel[3] *= alpha;
    gold[3] *= alpha; text[3] *= alpha; dim[3] *= alpha;

    /* Scrim over the frozen frame. */
    waifu_sdl3_overlay_rect(0.0f, 0.0f, sw, WAIFU_OVERLAY_H, 0.0f, 0.0f, 0.02f, 0.72f * alpha);

    /* Panel + double gold border (outer thin, inner hairline). */
    waifu_sdl3_overlay_vgrad(x, y, panel_w, panel_h, top, bot);
    waifu_sdl3_overlay_frame(x, y, panel_w, panel_h, 1.6f, gold[0], gold[1], gold[2], gold[3]);
    waifu_sdl3_overlay_frame(x + 3.5f, y + 3.5f, panel_w - 7.0f, panel_h - 7.0f, 0.7f,
                             COL_GOLD_DIM[0], COL_GOLD_DIM[1], COL_GOLD_DIM[2], gold[3]);

    draw_text_shadow(x + PANEL_PAD, y + PANEL_PAD - 2.0f, TITLE_SIZE, title, gold);
    waifu_sdl3_overlay_rect(x + PANEL_PAD, y + PANEL_PAD + TITLE_SIZE + 3.0f,
                            panel_w - PANEL_PAD * 2.0f, 0.8f,
                            gold[0], gold[1], gold[2], gold[3] * 0.7f);

    rowy = y + PANEL_PAD + 30.0f;
    m->hit_x0 = x + PANEL_PAD - 6.0f;
    m->hit_x1 = x + panel_w - PANEL_PAD + 6.0f;
    m->hit_y0 = rowy;
    m->hit_row_h = ROW_H;
    m->hit_first = m->scroll;
    m->hit_rows = visible;
    for (i = 0; i < visible; ++i) {
        int idx = m->scroll + i;
        const Row *r;
        float ry = rowy + i * ROW_H;
        float ty = ry + (ROW_H - ITEM_SIZE) * 0.5f - 1.0f;
        float vx;
        const float *lab_col;
        if (idx < 0 || idx >= rl->count) break;
        r = &rl->rows[idx];
        lab_col = (r->kind == ROW_INFO) ? dim : text;

        if (idx == cursor) {
            waifu_sdl3_overlay_rect(x + PANEL_PAD - 6.0f, ry, panel_w - PANEL_PAD * 2.0f + 12.0f,
                                    ROW_H - 2.0f, sel[0], sel[1], sel[2], sel[3]);
            waifu_sdl3_overlay_rect(x + PANEL_PAD - 6.0f, ry, 2.0f, ROW_H - 2.0f,
                                    gold[0], gold[1], gold[2], gold[3]);
            lab_col = gold;
        }
        draw_text_shadow(x + PANEL_PAD + 2.0f, ty, ITEM_SIZE, r->label, lab_col);

        if (r->kind == ROW_SLIDER) {
            float bw = 90.0f, bh = 4.0f;
            float bx = x + panel_w - PANEL_PAD - bw - 34.0f;
            float by = ry + (ROW_H - bh) * 0.5f - 1.0f;
            waifu_sdl3_overlay_rect(bx, by, bw, bh, 0.12f, 0.14f, 0.24f, alpha);
            waifu_sdl3_overlay_rect(bx, by, bw * r->frac, bh, gold[0], gold[1], gold[2], gold[3]);
            waifu_sdl3_overlay_frame(bx, by, bw, bh, 0.5f,
                                     COL_GOLD_DIM[0], COL_GOLD_DIM[1], COL_GOLD_DIM[2], alpha);
            vx = x + panel_w - PANEL_PAD - waifu_sdl3_overlay_text_width(ITEM_SIZE, r->value);
            draw_text_shadow(vx, ty, ITEM_SIZE, r->value, text);
        } else if (r->value[0]) {
            const float *vc = (m->rebind_action >= 0 &&
                               r->id - CTL_FIRST_BIND == m->rebind_action &&
                               m->page == PAGE_CONTROLS) ? gold : text;
            vx = x + panel_w - PANEL_PAD - waifu_sdl3_overlay_text_width(ITEM_SIZE, r->value);
            draw_text_shadow(vx, ty, ITEM_SIZE, r->value, vc);
        }
    }

    /* Scroll indicators. */
    if (m->scroll > 0)
        draw_text_shadow(x + panel_w - PANEL_PAD - 6.0f, rowy - 9.0f, 8.0f, "^", dim);
    if (m->scroll + visible < rl->count)
        draw_text_shadow(x + panel_w - PANEL_PAD - 6.0f, rowy + visible * ROW_H + 1.0f, 8.0f, "v", dim);

    /* Footer hint. */
    {
        const char *hint;
        if (m->rebind_action >= 0)
            hint = "PRESS A KEY OR BUTTON   -   ESC CANCELS";
        else if (m->page == PAGE_ROOT)
            hint = "UP/DOWN SELECT   -   CONFIRM ACCEPT   -   CANCEL RESUME";
        else if (m->page == PAGE_OPTIONS || m->page == PAGE_QUIT)
            hint = "UP/DOWN SELECT   -   CONFIRM ACCEPT   -   CANCEL BACK";
        else
            hint = "UP/DOWN SELECT   -   LEFT/RIGHT CHANGE   -   CANCEL BACK";
        float fw = waifu_sdl3_overlay_text_width(FOOT_SIZE, hint);
        draw_text_shadow(x + (panel_w - fw) * 0.5f, y + panel_h - PANEL_PAD + 2.0f,
                         FOOT_SIZE, hint, dim);
    }
}

static void draw_toast(WaifuMenu *m)
{
    float a, w;
    if (m->toast_frames <= 0) return;
    a = m->toast_frames > 30 ? 1.0f : m->toast_frames / 30.0f;
    w = waifu_sdl3_overlay_text_width(10.0f, m->toast);
    waifu_sdl3_overlay_rect(14.0f, WAIFU_OVERLAY_H - 34.0f, w + 16.0f, 20.0f,
                            0.02f, 0.03f, 0.09f, 0.8f * a);
    waifu_sdl3_overlay_frame(14.0f, WAIFU_OVERLAY_H - 34.0f, w + 16.0f, 20.0f, 1.0f,
                             COL_GOLD[0], COL_GOLD[1], COL_GOLD[2], a);
    waifu_sdl3_overlay_text(22.0f, WAIFU_OVERLAY_H - 28.0f, 10.0f, m->toast,
                            COL_TEXT[0], COL_TEXT[1], COL_TEXT[2], a);
    m->toast_frames--;
}

/* --- mouse ------------------------------------------------------------------
   A desktop release is expected to be playable with the pointer in its menus:
   hovering moves the selection, the left button activates (and, on a slider or
   a choice, the half of the row you click decides the direction), and the wheel
   scrolls a long page. */

void waifu_menu_handle_event(WaifuMenu *m, const SDL_Event *ev)
{
    float ox = 0.0f, oy = 0.0f;
    if (!m || !m->active || !ev) return;

    switch (ev->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!waifu_sdl3_video_window_to_overlay(m->video, ev->motion.x, ev->motion.y, &ox, &oy)) {
            m->mouse_row = -1;
            return;
        }
        m->mouse_active = 1;
        m->mouse_row = -1;
        if (m->hit_rows > 0 && ox >= m->hit_x0 && ox < m->hit_x1 && oy >= m->hit_y0) {
            int idx = (int)((oy - m->hit_y0) / m->hit_row_h);
            if (idx >= 0 && idx < m->hit_rows) {
                m->mouse_row = m->hit_first + idx;
                m->mouse_click_left_half = (ox < (m->hit_x0 + m->hit_x1) * 0.5f);
            }
        }
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && m->mouse_row >= 0 &&
            ev->button.button == SDL_BUTTON_LEFT)
            m->mouse_click = 1;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        m->mouse_wheel -= (int)ev->wheel.y;
        break;
    default:
        break;
    }
}

/* --- frame ----------------------------------------------------------------- */

int waifu_menu_update(WaifuMenu *m, int px_w, int px_h)
{
    RowList rl;
    const char *title = "";
    int cursor, moved = 0;
    float alpha;

    if (!m || !m->active) return 1;

    /* A rebind swallows the frame's input while the capture is armed. */
    if (m->rebind_action >= 0) {
        int code = 0, is_pad = 0;
        if (waifu_input_capture_result(m->input, &code, &is_pad)) {
            if (is_pad >= 0) {
                unbind_elsewhere(m->cfg, m->rebind_action, code, is_pad);
                if (is_pad) m->cfg->pad[m->rebind_action][0] = code;
                else        m->cfg->key[m->rebind_action][0] = code;
                beep(1);
            }
            m->rebind_action = -1;
        } else if (!waifu_input_capturing(m->input)) {
            m->rebind_action = -1;
        }
    }

    build_rows(m, &rl, &title);
    if (rl.count <= 0) return 1;

    cursor = clampi(m->cursor[m->page], 0, rl.count - 1);

    /* Pointer first: hovering a selectable row takes the selection, so the
       keyboard/pad and the mouse never disagree about what is highlighted. */
    if (m->rebind_action < 0) {
        if (m->mouse_wheel) {
            m->scroll += m->mouse_wheel;
            m->mouse_wheel = 0;
        }
        if (m->mouse_row >= 0 && m->mouse_row < rl.count &&
            rl.rows[m->mouse_row].kind != ROW_INFO && m->mouse_active) {
            if (m->mouse_row != cursor) beep(0);
            cursor = m->mouse_row;
            m->cursor[m->page] = cursor;
        }
        if (m->mouse_click) {
            const Row *r = &rl.rows[cursor];
            m->mouse_click = 0;
            if (m->mouse_row == cursor) {
                if (r->kind == ROW_CHOICE || r->kind == ROW_SLIDER) {
                    int dir = m->mouse_click_left_half ? -1 : 1;
                    if (m->page == PAGE_VIDEO) adjust_video(m, r->id, dir);
                    else if (m->page == PAGE_AUDIO) adjust_audio(m, r->id, dir);
                    else if (m->page == PAGE_CONTROLS) adjust_controls(m, r->id, dir);
                    beep(0);
                } else {
                    activate(m, r);
                }
            }
        }
    }

    if (!m->active) return m->quit ? 0 : 1;

    if (m->rebind_action < 0) {
        if (waifu_input_repeat(m->input, WAIFU_ACT_DOWN)) { cursor++; moved = 1; }
        if (waifu_input_repeat(m->input, WAIFU_ACT_UP))   { cursor--; moved = 1; }
        if (cursor < 0) cursor = rl.count - 1;
        if (cursor >= rl.count) cursor = 0;
        /* Skip over non-selectable rows in the direction of travel. */
        if (moved) {
            int guard = rl.count;
            int dir = waifu_input_repeat(m->input, WAIFU_ACT_DOWN) ? 1 : -1;
            while (rl.rows[cursor].kind == ROW_INFO && guard-- > 0) {
                cursor += dir;
                if (cursor < 0) cursor = rl.count - 1;
                if (cursor >= rl.count) cursor = 0;
            }
            beep(0);
        }
        m->cursor[m->page] = cursor;

        if (waifu_input_repeat(m->input, WAIFU_ACT_LEFT) ||
            waifu_input_repeat(m->input, WAIFU_ACT_RIGHT)) {
            int dir = waifu_input_repeat(m->input, WAIFU_ACT_RIGHT) ? 1 : -1;
            const Row *r = &rl.rows[cursor];
            if (r->kind == ROW_CHOICE || r->kind == ROW_SLIDER) {
                if (m->page == PAGE_VIDEO) adjust_video(m, r->id, dir);
                else if (m->page == PAGE_AUDIO) adjust_audio(m, r->id, dir);
                else if (m->page == PAGE_CONTROLS) adjust_controls(m, r->id, dir);
                beep(0);
            }
        }

        if (waifu_input_pressed(m->input, WAIFU_ACT_CONFIRM) ||
            waifu_input_pressed(m->input, WAIFU_ACT_START))
            activate(m, &rl.rows[cursor]);

        if (waifu_input_pressed(m->input, WAIFU_ACT_CANCEL) ||
            waifu_input_pressed(m->input, WAIFU_ACT_MENU)) {
            beep(0);
            if (m->page == PAGE_ROOT) waifu_menu_close(m);
            else if (m->page == PAGE_OPTIONS || m->page == PAGE_QUIT) m->page = PAGE_ROOT;
            else { m->page = PAGE_OPTIONS; }
            m->scroll = 0;
        }
    } else if (waifu_input_pressed(m->input, WAIFU_ACT_MENU)) {
        m->rebind_action = -1;
        waifu_input_capture_cancel(m->input);
    }

    if (!m->active) return m->quit ? 0 : 1;   /* closed this frame */

    /* Rebuild after the edit so the drawn values are this frame's. */
    build_rows(m, &rl, &title);
    cursor = clampi(m->cursor[m->page], 0, rl.count - 1);
    m->cursor[m->page] = cursor;
    if (cursor < m->scroll) m->scroll = cursor;
    if (cursor >= m->scroll + MAX_VISIBLE) m->scroll = cursor - MAX_VISIBLE + 1;
    if (m->scroll > rl.count - MAX_VISIBLE) m->scroll = rl.count - MAX_VISIBLE;
    if (m->scroll < 0) m->scroll = 0;

    if (m->open_frames < 8) m->open_frames++;
    alpha = m->open_frames / 8.0f;

    waifu_sdl3_overlay_begin(px_w, px_h);
    draw_page(m, &rl, title, alpha);
    draw_toast(m);
    return m->quit ? 0 : 1;
}

void waifu_menu_draw_hud(WaifuMenu *m, int px_w, int px_h, float fps)
{
    char buf[32];
    if (!m) return;
    if (fps < 0.0f && m->toast_frames <= 0) {
        waifu_sdl3_overlay_clear();
        return;
    }
    waifu_sdl3_overlay_begin(px_w, px_h);
    if (fps >= 0.0f) {
        float w;
        SDL_snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f));
        w = waifu_sdl3_overlay_text_width(10.0f, buf);
        waifu_sdl3_overlay_rect(waifu_sdl3_overlay_width() - w - 18.0f, 6.0f, w + 12.0f, 16.0f,
                                0.0f, 0.0f, 0.0f, 0.45f);
        waifu_sdl3_overlay_text(waifu_sdl3_overlay_width() - w - 12.0f, 10.0f, 10.0f, buf,
                                COL_GOLD[0], COL_GOLD[1], COL_GOLD[2], 1.0f);
    }
    draw_toast(m);
}
