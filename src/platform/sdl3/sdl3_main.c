/* SDL3 frontend: hardware-accelerated (SDL_GPU) desktop build of the game.
 *
 * The game core is the same frame machine every other frontend drives
 * (game_api.h); this frontend adds a real 3D presentation on top of it:
 * the core's 3D scenes are captured through the hardware-3D seam and drawn
 * as GPU polygons, and the core's 8bpp framebuffer is palette-expanded on
 * the GPU as the 2D/UI layer above them (sdl3_video.c).
 *
 * Command scripts, --frames and --dump-every mirror the headless runner so
 * scripted scenario runs stay comparable across frontends. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <zlib.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "game_api.h"
#include "sdl3_video.h"
#include "sdl3_audio.h"
#include "sdl3_settings.h"
#include "sdl3_input.h"
#include "sdl3_menu.h"
#include "sdl3_overlay.h"

#define MAX_COMMAND_EVENTS 4096

/* One game frame of real time: the core's logic clock is fixed at
   WAIFU_FM_FPS regardless of how fast the display refreshes. */
#define STEP_NS (SDL_NS_PER_SECOND / WAIFU_FM_FPS)

/* A scripted frame's buttons: the game pad state plus the frontend-only
   MENU token, so a command file can drive the pause/options menu too. */
typedef struct ScriptButtons {
    WaifuFmInput in;
    int menu;
} ScriptButtons;

typedef struct CommandEvent {
    int start;
    int duration;
    ScriptButtons input;
} CommandEvent;

static int ensure_dir(const char *path)
{
#ifdef _WIN32
    return mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

/* Minimal RGBA8 PNG writer (zlib-compressed, filter 0 rows) for GPU frame
   dumps — the game has no 8bpp presentation surface on this platform, so
   dumps read back the rendered offscreen target instead. */
static void png_chunk(FILE *fp, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    uint32_t crc;
    hdr[0] = (uint8_t)(len >> 24); hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, fp);
    if (len) fwrite(data, 1, len, fp);
    crc = (uint32_t)crc32(0L, Z_NULL, 0);
    crc = (uint32_t)crc32(crc, (const Bytef *)type, 4);
    if (len) crc = (uint32_t)crc32(crc, data, len);
    {
        uint8_t cb[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
        fwrite(cb, 1, 4, fp);
    }
}

static int write_png_rgba(const char *path, const uint8_t *rgba, int w, int h)
{
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    uint8_t ihdr[13];
    size_t raw_len = (size_t)h * ((size_t)w * 4 + 1);
    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uint8_t *z = (uint8_t *)malloc(zlen);
    FILE *fp = NULL;
    int y, ok = 0;

    if (raw && z) {
        for (y = 0; y < h; ++y) {
            uint8_t *row = raw + (size_t)y * ((size_t)w * 4 + 1);
            row[0] = 0;
            memcpy(row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
        }
        if (compress2(z, &zlen, raw, (uLong)raw_len, 6) == Z_OK &&
            (fp = fopen(path, "wb")) != NULL) {
            fwrite(sig, 1, 8, fp);
            ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
            ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
            ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
            ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
            ihdr[8] = 8;   /* bit depth */
            ihdr[9] = 6;   /* RGBA */
            ihdr[10] = ihdr[11] = ihdr[12] = 0;
            png_chunk(fp, "IHDR", ihdr, 13);
            png_chunk(fp, "IDAT", z, (uint32_t)zlen);
            png_chunk(fp, "IEND", NULL, 0);
            ok = (fclose(fp) == 0);
            fp = NULL;
        }
    }
    if (fp) fclose(fp);
    free(raw);
    free(z);
    return ok ? 0 : -1;
}

static int dump_gpu_frame_png(WaifuSdl3Video *video, uint8_t *scratch,
                              const char *out_dir, int frame)
{
    char path[512];
    int w = waifu_sdl3_video_render_width(video);
    int h = waifu_sdl3_video_render_height(video);
    if (!waifu_sdl3_video_read_frame(video, scratch)) return -1;
    snprintf(path, sizeof(path), "%s/frame_%05d.png", out_dir, frame);
    return write_png_rgba(path, scratch, w, h);
}

/* Command-file parsing mirrors the headless runner / SDL 1.2 frontend:
   `<frame> <hold_frames> <BUTTON...>` per line, `#` comments. */
static void input_or_button(ScriptButtons *sb, const char *tok)
{
    char buf[32];
    int i = 0;
    while (*tok && i < (int)sizeof(buf) - 1) {
        if (*tok != ',' && *tok != '|' && *tok != '+') buf[i++] = (char)toupper((unsigned char)*tok);
        tok++;
    }
    buf[i] = '\0';
    if (!strcmp(buf, "UP")) sb->in.up = 1;
    else if (!strcmp(buf, "DOWN")) sb->in.down = 1;
    else if (!strcmp(buf, "LEFT")) sb->in.left = 1;
    else if (!strcmp(buf, "RIGHT")) sb->in.right = 1;
    else if (!strcmp(buf, "A") || !strcmp(buf, "LCTRL") || !strcmp(buf, "CTRL")) sb->in.a = 1;
    else if (!strcmp(buf, "B") || !strcmp(buf, "LALT") || !strcmp(buf, "ALT")) sb->in.b = 1;
    else if (!strcmp(buf, "START") || !strcmp(buf, "RUN") || !strcmp(buf, "SPACE")) sb->in.start = 1;
    else if (!strcmp(buf, "TAB") || !strcmp(buf, "BUTTON4") || !strcmp(buf, "BTN4") || !strcmp(buf, "4")) sb->in.tab = 1;
    else if (!strcmp(buf, "MENU") || !strcmp(buf, "ESC") || !strcmp(buf, "ESCAPE")) sb->menu = 1;
}

static void input_or_button_list(ScriptButtons *sb, const char *tok)
{
    char part[32];
    int n = 0;
    const char *p = tok;
    while (1) {
        int delim = (*p == '\0' || *p == ',' || *p == '|' || *p == '+');
        if (delim) {
            if (n > 0) {
                part[n] = '\0';
                input_or_button(sb, part);
                n = 0;
            }
            if (*p == '\0') break;
        } else if (n < (int)sizeof(part) - 1) {
            part[n++] = *p;
        }
        ++p;
    }
}

static int load_command_file(const char *path, CommandEvent *events, int max_events)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int count = 0;
    if (!fp) {
        fprintf(stderr, "could not open command file: %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *tok;
        CommandEvent ev;
        memset(&ev, 0, sizeof(ev));
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        tok = strtok(p, " \t\r\n");
        if (!tok) continue;
        ev.start = atoi(tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok) continue;
        ev.duration = atoi(tok);
        if (ev.duration < 1) ev.duration = 1;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL) input_or_button_list(&ev.input, tok);
        if (count < max_events) events[count++] = ev;
    }
    fclose(fp);
    return count;
}

static ScriptButtons input_for_frame_from_events(int frame, const CommandEvent *events, int count)
{
    ScriptButtons sb;
    int i;
    memset(&sb, 0, sizeof(sb));
    for (i = 0; i < count; ++i) {
        if (frame >= events[i].start && frame < events[i].start + events[i].duration) {
            sb.in.up |= events[i].input.in.up;
            sb.in.down |= events[i].input.in.down;
            sb.in.left |= events[i].input.in.left;
            sb.in.right |= events[i].input.in.right;
            sb.in.a |= events[i].input.in.a;
            sb.in.b |= events[i].input.in.b;
            sb.in.start |= events[i].input.in.start;
            sb.in.tab |= events[i].input.in.tab;
            sb.menu |= events[i].input.menu;
        }
    }
    return sb;
}

/* Screenshots land next to the settings file so they survive a reinstall and
   never need write access to the game directory. */
static void save_screenshot(WaifuSdl3Video *video, WaifuMenu *menu)
{
    char path[1200];
    int w = waifu_sdl3_video_render_width(video);
    int h = waifu_sdl3_video_render_height(video);
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    SDL_Time now = 0;
    SDL_DateTime dt;

    if (!rgba) return;
    if (waifu_sdl3_video_read_frame(video, rgba)) {
        SDL_GetCurrentTime(&now);
        if (SDL_TimeToDateTime(now, &dt, true)) {
            snprintf(path, sizeof(path), "%sshot_%04d%02d%02d_%02d%02d%02d.png",
                     waifu_settings_dir(), dt.year, dt.month, dt.day,
                     dt.hour, dt.minute, dt.second);
        } else {
            snprintf(path, sizeof(path), "%sshot.png", waifu_settings_dir());
        }
        if (write_png_rgba(path, rgba, w, h) == 0)
            waifu_menu_toast(menu, "SCREENSHOT SAVED");
        else
            waifu_menu_toast(menu, "SCREENSHOT FAILED");
    }
    free(rgba);
}

static WaifuFmInput input_from_actions(const WaifuInput *in)
{
    WaifuFmInput out;
    memset(&out, 0, sizeof(out));
    out.up    = waifu_input_held(in, WAIFU_ACT_UP);
    out.down  = waifu_input_held(in, WAIFU_ACT_DOWN);
    out.left  = waifu_input_held(in, WAIFU_ACT_LEFT);
    out.right = waifu_input_held(in, WAIFU_ACT_RIGHT);
    out.a     = waifu_input_held(in, WAIFU_ACT_CONFIRM);
    out.b     = waifu_input_held(in, WAIFU_ACT_CANCEL);
    out.start = waifu_input_held(in, WAIFU_ACT_START);
    out.tab   = waifu_input_held(in, WAIFU_ACT_ASSIST);
    return out;
}

int main(int argc, char **argv)
{
    WaifuSettings settings;
    WaifuSdl3Video *video = NULL;
    WaifuSdl3Audio *audio = NULL;
    WaifuInput *input = NULL;
    WaifuMenu *menu = NULL;
    CommandEvent events[MAX_COMMAND_EVENTS];
    int event_count = 0;
    int running = 1;
    int frame = 0;
    int max_frames = -1;
    int dump_every = 0;
    int no_delay = 0;
    int cli_fullscreen = 0;
    int cli_scale = 0;
    int cli_novsync = 0;
    const char *commands_path = NULL;
    const char *out_dir = NULL;
    uint8_t *dump_scratch = NULL;
    Uint64 next_frame_ns;
    Uint64 fps_window_ns;
    Uint64 last_tick_ns;
    Uint64 frame_delta_ns = 0;
    Uint64 step_accum_ns = 0;
    int fixed_step;
    int fps_window_frames = 0;
    float fps_value = 0.0f;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--commands") && i + 1 < argc) commands_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-delay")) no_delay = 1;
        else if (!strcmp(argv[i], "--fullscreen")) cli_fullscreen = 1;
        else if (!strcmp(argv[i], "--no-vsync")) cli_novsync = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) cli_scale = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--commands file] [--frames n] [--out dir] [--dump-every n]\n"
                            "          [--no-delay] [--no-vsync] [--fullscreen] [--scale n]\n", argv[0]);
            return 1;
        }
    }
    if (dump_every < 0) dump_every = 0;

    /* Player settings first: the window opens at the size/mode last saved. The
       command-line switches stay as scripted-capture overrides on top. */
    waifu_settings_load(&settings);
    if (cli_fullscreen) settings.window_mode = WAIFU_WINDOW_BORDERLESS;
    if (cli_novsync) settings.vsync = 0;
    if (no_delay) settings.fps_cap = 0;
    if (cli_scale > 0) {
        settings.window_h = WAIFU_FM_HEIGHT * cli_scale;
        settings.window_w = (settings.window_h * 16 + 4) / 9;
        settings.window_mode = WAIFU_WINDOW_WINDOWED;
    }

    if (commands_path) {
        event_count = load_command_file(commands_path, events, MAX_COMMAND_EVENTS);
        if (event_count < 0) return 1;
    }
    if (out_dir && dump_every > 0) ensure_dir(out_dir);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    video = waifu_sdl3_video_create("Shattered Decks", &settings);
    if (!video) {
        fprintf(stderr, "SDL3 GPU video init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    input = waifu_input_create(&settings);
    menu = waifu_menu_create(&settings, video, input);
    if (!input || !menu) {
        fprintf(stderr, "frontend init failed\n");
        waifu_sdl3_video_destroy(video);
        SDL_Quit();
        return 1;
    }

    if (commands_path) waifu_menu_set_persist(menu, 0);

    waifu_fm_init();
    waifu_fm_reset_interactive();
    audio = waifu_sdl3_audio_create();

    /* One game step per presented frame is what a scripted capture needs (its
       command file counts frames) and what --no-delay means. Interactive play
       runs the accumulator instead. */
    fixed_step = (commands_path != NULL) || no_delay;

    next_frame_ns = SDL_GetTicksNS();
    fps_window_ns = next_frame_ns;
    last_tick_ns = next_frame_ns;

    while (running) {
        WaifuFmInput effective;
        SDL_Event ev;
        int win_w = 0, win_h = 0;

        while (SDL_PollEvent(&ev)) {
            waifu_input_handle_event(input, &ev);
            waifu_menu_handle_event(menu, &ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = 0;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                waifu_sdl3_video_note_window_size(video, &settings);
                break;
            default:
                break;
            }
        }
        if (commands_path) {
            ScriptButtons sb = input_for_frame_from_events(frame, events, event_count);
            waifu_input_inject(input, &sb.in, sb.menu);
        }
        {
            Uint64 now = SDL_GetTicksNS();
            frame_delta_ns = now - last_tick_ns;
            last_tick_ns = now;
            /* Clamp a stall (window drag, alt-tab) so it never replays as a
               burst of game frames. */
            if (frame_delta_ns > SDL_NS_PER_SECOND / 4) frame_delta_ns = STEP_NS;
        }
        waifu_input_update(input);
        /* The overlay draws into the present target (canvas sized), so its
           virtual space must carry the CANVAS aspect, not the window's — they
           differ in the pillarbox/stretch fit modes. */
        win_w = waifu_sdl3_video_render_width(video);
        win_h = waifu_sdl3_video_render_height(video);

        /* Frontend hotkeys work in and out of the menu, except while a
           rebinding capture is armed (the input layer swallows those). */
        if (!waifu_input_capturing(input)) {
            if (waifu_input_pressed(input, WAIFU_ACT_FULLSCREEN)) {
                waifu_sdl3_video_toggle_fullscreen(video, &settings);
                if (!commands_path) waifu_settings_save(&settings);
            }
            if (waifu_input_pressed(input, WAIFU_ACT_SCREENSHOT))
                save_screenshot(video, menu);
        }

        if (waifu_menu_active(menu)) {
            /* The game is frozen: no step, no captured geometry. The presenter
               keeps showing the last canvas and composites the menu over it. */
            waifu_sdl3_audio_lock(audio);
            if (!waifu_menu_update(menu, win_w, win_h)) running = 0;
            waifu_sdl3_audio_unlock(audio);
        } else {
            if (waifu_input_pressed(input, WAIFU_ACT_MENU)) {
                waifu_menu_open(menu);
                step_accum_ns = 0;
            } else {
                int steps = 1;
                effective = input_from_actions(input);
                /* The game logic is a fixed 60 Hz clock. Presentation is not:
                   vsync on a 120/144 Hz display, or an uncapped limiter, would
                   otherwise run the game at the refresh rate. So accumulate real
                   time and step only whole game frames, catching up at most a
                   few (a long stall must not turn into a burst of gameplay).
                   Scripted runs keep one step per iteration so a command file's
                   frame numbers stay exact. */
                if (!fixed_step) {
                    steps = 0;
                    step_accum_ns += frame_delta_ns;
                    while (step_accum_ns >= STEP_NS && steps < 4) {
                        step_accum_ns -= STEP_NS;
                        ++steps;
                    }
                    if (step_accum_ns > STEP_NS * 4) step_accum_ns = 0;
                }
                waifu_sdl3_audio_lock(audio);
                while (steps-- > 0) waifu_fm_step(&effective);
                waifu_sdl3_audio_unlock(audio);
            }
            waifu_menu_draw_hud(menu, win_w, win_h, settings.show_fps ? fps_value : -1.0f);
        }

        if (!waifu_sdl3_video_present(video, waifu_fm_video_fade_q8())) {
            fprintf(stderr, "GPU present failed: %s\n", SDL_GetError());
            running = 0;
        }

        if (out_dir && dump_every > 0 && (frame % dump_every) == 0) {
            if (!dump_scratch) {
                dump_scratch = (uint8_t *)malloc((size_t)waifu_sdl3_video_render_width(video) *
                                                 (size_t)waifu_sdl3_video_render_height(video) * 4);
            }
            if (dump_scratch) dump_gpu_frame_png(video, dump_scratch, out_dir, frame);
        }

        ++frame;
        if (max_frames >= 0 && frame >= max_frames) running = 0;

        /* Measured frame rate over a rolling half second (the FPS readout). */
        ++fps_window_frames;
        {
            Uint64 now = SDL_GetTicksNS();
            if (now - fps_window_ns >= SDL_NS_PER_SECOND / 2) {
                fps_value = (float)fps_window_frames * (float)SDL_NS_PER_SECOND /
                            (float)(now - fps_window_ns);
                fps_window_frames = 0;
                fps_window_ns = now;
            }
        }

        /* Frame pacing on the nanosecond clock; drift-free because the deadline
           advances by exactly one frame period each iteration. A frame limit of
           0 (or --no-delay) runs free, letting vsync alone pace the loop. */
        {
            int cap = settings.fps_cap > 0 ? settings.fps_cap : 0;
            if (no_delay || cap <= 0) {
                next_frame_ns = SDL_GetTicksNS();
            } else {
                Uint64 now;
                next_frame_ns += SDL_NS_PER_SECOND / (Uint64)cap;
                now = SDL_GetTicksNS();
                if (next_frame_ns > now) SDL_DelayNS(next_frame_ns - now);
                else if (now - next_frame_ns > SDL_NS_PER_SECOND / 4) next_frame_ns = now;
            }
        }
    }

    /* A scripted capture must not rewrite the player's settings file. */
    if (!commands_path) waifu_settings_save(&settings);
    waifu_sdl3_audio_destroy(audio);
    waifu_menu_destroy(menu);
    waifu_input_destroy(input);
    waifu_sdl3_video_destroy(video);
    free(dump_scratch);
    SDL_Quit();
    return 0;
}
