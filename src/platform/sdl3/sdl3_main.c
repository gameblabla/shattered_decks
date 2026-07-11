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

#define MAX_COMMAND_EVENTS 4096

typedef struct CommandEvent {
    int start;
    int duration;
    WaifuFmInput input;
} CommandEvent;

typedef struct GamepadState {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
} GamepadState;

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

static void apply_gamepad_button(WaifuFmInput *input, SDL_GamepadButton button, int down)
{
    switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: input->up = down; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: input->down = down; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: input->left = down; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: input->right = down; break;
        case SDL_GAMEPAD_BUTTON_SOUTH: input->a = down; break;
        case SDL_GAMEPAD_BUTTON_EAST: input->b = down; break;
        case SDL_GAMEPAD_BUTTON_START: input->start = down; break;
        case SDL_GAMEPAD_BUTTON_BACK: input->tab = down; break;
        default: break;
    }
}

static void poll_input(WaifuFmInput *input, GamepadState *pad,
                       WaifuSdl3Video *video, int *running)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_EVENT_QUIT:
                *running = 0;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const int down = (ev.type == SDL_EVENT_KEY_DOWN);
                switch (ev.key.key) {
                    case SDLK_ESCAPE:
                        if (down) *running = 0;
                        break;
                    case SDLK_F11:
                        if (down && !ev.key.repeat) waifu_sdl3_video_toggle_fullscreen(video);
                        break;
                    case SDLK_UP: input->up = down; break;
                    case SDLK_DOWN: input->down = down; break;
                    case SDLK_LEFT: input->left = down; break;
                    case SDLK_RIGHT: input->right = down; break;
                    case SDLK_LCTRL: input->a = down; break;
                    case SDLK_LALT: input->b = down; break;
                    case SDLK_SPACE: input->start = down; break;
                    case SDLK_TAB: input->tab = down; break;
                    default: break;
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad->pad) {
                    pad->pad = SDL_OpenGamepad(ev.gdevice.which);
                    if (pad->pad) pad->id = ev.gdevice.which;
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad->pad && ev.gdevice.which == pad->id) {
                    SDL_CloseGamepad(pad->pad);
                    pad->pad = NULL;
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                apply_gamepad_button(input, (SDL_GamepadButton)ev.gbutton.button,
                                     ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            default:
                break;
        }
    }
}

/* Command-file parsing mirrors the headless runner / SDL 1.2 frontend:
   `<frame> <hold_frames> <BUTTON...>` per line, `#` comments. */
static void input_or_button(WaifuFmInput *in, const char *tok)
{
    char buf[32];
    int i = 0;
    while (*tok && i < (int)sizeof(buf) - 1) {
        if (*tok != ',' && *tok != '|' && *tok != '+') buf[i++] = (char)toupper((unsigned char)*tok);
        tok++;
    }
    buf[i] = '\0';
    if (!strcmp(buf, "UP")) in->up = 1;
    else if (!strcmp(buf, "DOWN")) in->down = 1;
    else if (!strcmp(buf, "LEFT")) in->left = 1;
    else if (!strcmp(buf, "RIGHT")) in->right = 1;
    else if (!strcmp(buf, "A") || !strcmp(buf, "LCTRL") || !strcmp(buf, "CTRL")) in->a = 1;
    else if (!strcmp(buf, "B") || !strcmp(buf, "LALT") || !strcmp(buf, "ALT")) in->b = 1;
    else if (!strcmp(buf, "START") || !strcmp(buf, "RUN") || !strcmp(buf, "SPACE")) in->start = 1;
    else if (!strcmp(buf, "TAB") || !strcmp(buf, "BUTTON4") || !strcmp(buf, "BTN4") || !strcmp(buf, "4")) in->tab = 1;
}

static void input_or_button_list(WaifuFmInput *in, const char *tok)
{
    char part[32];
    int n = 0;
    const char *p = tok;
    while (1) {
        int delim = (*p == '\0' || *p == ',' || *p == '|' || *p == '+');
        if (delim) {
            if (n > 0) {
                part[n] = '\0';
                input_or_button(in, part);
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

static WaifuFmInput input_for_frame_from_events(int frame, const CommandEvent *events, int count)
{
    WaifuFmInput in;
    int i;
    memset(&in, 0, sizeof(in));
    for (i = 0; i < count; ++i) {
        if (frame >= events[i].start && frame < events[i].start + events[i].duration) {
            in.up |= events[i].input.up;
            in.down |= events[i].input.down;
            in.left |= events[i].input.left;
            in.right |= events[i].input.right;
            in.a |= events[i].input.a;
            in.b |= events[i].input.b;
            in.start |= events[i].input.start;
            in.tab |= events[i].input.tab;
        }
    }
    return in;
}

static WaifuFmInput merge_inputs(const WaifuFmInput *a, const WaifuFmInput *b)
{
    WaifuFmInput in;
    in.up = a->up | b->up;
    in.down = a->down | b->down;
    in.left = a->left | b->left;
    in.right = a->right | b->right;
    in.a = a->a | b->a;
    in.b = a->b | b->b;
    in.start = a->start | b->start;
    in.tab = a->tab | b->tab;
    return in;
}

int main(int argc, char **argv)
{
    WaifuSdl3Video *video = NULL;
    WaifuSdl3Audio *audio = NULL;
    GamepadState pad = { NULL, 0 };
    WaifuFmInput input;
    CommandEvent events[MAX_COMMAND_EVENTS];
    int event_count = 0;
    int running = 1;
    int frame = 0;
    int max_frames = -1;
    int dump_every = 0;
    int no_delay = 0;
    int fullscreen = 0;
    int scale = 3;
    int vsync = 1;
    const char *commands_path = NULL;
    const char *out_dir = NULL;
    uint8_t *dump_scratch = NULL;
    Uint64 next_frame_ns;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--commands") && i + 1 < argc) commands_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-delay")) no_delay = 1;
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--no-vsync")) vsync = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--commands file] [--frames n] [--out dir] [--dump-every n]\n"
                            "          [--no-delay] [--no-vsync] [--fullscreen] [--scale n]\n", argv[0]);
            return 1;
        }
    }
    if (dump_every < 0) dump_every = 0;
    if (scale < 1) scale = 1;

    if (commands_path) {
        event_count = load_command_file(commands_path, events, MAX_COMMAND_EVENTS);
        if (event_count < 0) return 1;
    }
    if (out_dir && dump_every > 0) ensure_dir(out_dir);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    video = waifu_sdl3_video_create("Shattered Decks - SDL3", scale, fullscreen, vsync);
    if (!video) {
        fprintf(stderr, "SDL3 GPU video init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    memset(&input, 0, sizeof(input));
    waifu_fm_init();
    waifu_fm_reset_interactive();
    audio = waifu_sdl3_audio_create();

    next_frame_ns = SDL_GetTicksNS();

    while (running) {
        WaifuFmInput effective;

        poll_input(&input, &pad, video, &running);
        if (commands_path) {
            WaifuFmInput script_input = input_for_frame_from_events(frame, events, event_count);
            effective = merge_inputs(&input, &script_input);
        } else {
            effective = input;
        }

        waifu_sdl3_audio_lock(audio);
        waifu_fm_step(&effective);
        waifu_sdl3_audio_unlock(audio);

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

        /* 60 FPS pacing on the nanosecond clock; drift-free because the
           deadline advances by exactly one frame period each iteration. */
        next_frame_ns += SDL_NS_PER_SECOND / WAIFU_FM_FPS;
        if (!no_delay) {
            Uint64 now = SDL_GetTicksNS();
            if (next_frame_ns > now) SDL_DelayNS(next_frame_ns - now);
            else if (now - next_frame_ns > SDL_NS_PER_SECOND / 4) next_frame_ns = now;
        } else {
            next_frame_ns = SDL_GetTicksNS();
        }
    }

    waifu_sdl3_audio_destroy(audio);
    if (pad.pad) SDL_CloseGamepad(pad.pad);
    waifu_sdl3_video_destroy(video);
    free(dump_scratch);
    SDL_Quit();
    return 0;
}
