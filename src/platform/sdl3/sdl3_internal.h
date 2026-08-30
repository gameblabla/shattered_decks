#ifndef WAIFU_SDL3_INTERNAL_H
#define WAIFU_SDL3_INTERNAL_H

/* Shared contract between the SDL3 platform TUs:
 *   sdl3_scene3d.c — implements the hw3d/hw2d capture seams (src/engine/
 *                    hw3d.h) and the platform video seams; accumulates one
 *                    frame of scene + UI geometry in CPU arrays, with every
 *                    palette index resolved to RGBA at capture time (the
 *                    8bpp framebuffer is never presented on this platform).
 *   sdl3_video.c   — owns the SDL_GPU device; consumes the frame each present.
 *
 * 3D vertex positions are premultiplied clip coordinates (ndc_x*w, ndc_y*w, w)
 * — the game's pinhole projection folded in at capture — so the hardware
 * divide by w reproduces the software projection exactly and GPU polygons
 * line up with 2D elements the game positioned from projected coordinates.
 *
 * 2D (UI) vertices are in game screen space (WAIFU_FM_WIDTH x
 * WAIFU_FM_HEIGHT units). Submission order is preserved through the run
 * list; runs before `bg_runs` were captured before the frame's first 3D
 * primitive (sky/backdrop behind the scene), the rest draw above it. */

#include <stdint.h>

#include "cfx_screen_config.h"

#define SDL3_SCENE_MAX_SCENE_VERTS 16384   /* 3D atlas-tile quads/tris */
#define SDL3_SCENE_MAX_IMAGE_VERTS 4096    /* 3D card-face quads */
#define SDL3_SCENE_MAX_LINE_VERTS  16384   /* 3D line list */

#define SDL3_UI_MAX_VERTS          262144  /* 2D quads (6 verts each) */
#define SDL3_UI_MAX_LINE_VERTS     16384   /* 2D line list */
#define SDL3_UI_MAX_GLYPH_VERTS    32768   /* hi-res text glyph quads (6 each) */
#define SDL3_UI_MAX_RUNS           4096

/* Per-frame streaming RGBA atlas for every captured image (title/ending
   screens, portraits, card faces, big art, effect buffers). */
#define SDL3_IMAGE_ATLAS_W 1024
#define SDL3_IMAGE_ATLAS_H 1024
#define SDL3_IMAGE_MAX_ENTRIES 256

/* 32x32 tile atlas, RGBA-converted (layers = game tile count). */
#define SDL3_TILE_MAX_COUNT 16

typedef struct Sdl3SceneVertex {
    float x, y, w;      /* premultiplied clip space */
    float u, v;         /* tile-relative UV; integer part = repeats */
    float tile;         /* atlas layer */
} Sdl3SceneVertex;

typedef struct Sdl3ImageVertex {
    float x, y, w;
    float u, v;         /* texels into the streaming image atlas */
    float gray;         /* 1.0 = dimmed (used-card) rendering */
} Sdl3ImageVertex;

typedef struct Sdl3LineVertex {
    float x, y, w;
    float r, g, b, a;
} Sdl3LineVertex;

typedef struct Sdl3UiVertex {
    float x, y;         /* game screen space */
    float u, v;         /* image-atlas texels; u < 0 -> solid color */
    float r, g, b, a;
} Sdl3UiVertex;

/* Environment: the story-map sky + infinite floor, rendered as one fullscreen
   ray-cast pass (sdl3_video.c: pl_env) instead of captured geometry.
     - The sky is a smooth vertical gradient between up to four palette-resolved
       stops (replacing the software's hard fill_rows bands), requested through
       waifu_platform_background_request() exactly like the PC-FX RAINBOW / CD32X
       MD sky plane.  sky_* fields are populated BEFORE the frame's clear_screen
       (which resets the rest of the frame), so they persist across the reset and
       are consumed once at present.
     - The floor is a checkerboard plane at y = floor_y sampled per fragment by
       intersecting the eye ray with the plane — infinite, with no near/far clip
       artifacts.  floor_* / camera fields are captured by waifu_hw3d_floor(). */
typedef struct Sdl3Env {
    int has_sky;                    /* sky gradient requested this frame */
    int sky_kind;                   /* WaifuBackgroundKind (procedural stars/embers) */
    int sky_hscroll;                /* parallax phase for the star/ember drift */
    int has_floor;                  /* floor plane captured this frame */
    float floor_y;                  /* plane height, world units */
    float tile_a, tile_b;           /* atlas layers for the checker */
    float texels_per_unit;          /* 32 / tile_size_world */
    float eye[3], right[3], up[3], fwd[3];  /* camera basis (orthonormal) */
    float sx, sy;                   /* focal / (W/2), focal / (H/2) */
    float sky_stops[4][4];          /* rgb + normalized screen-y per gradient stop */
    float horizon[4];               /* rgb horizon/fog color + far distance */
} Sdl3Env;

typedef enum Sdl3UiRunKind {
    SDL3_UI_RUN_TRIS = 0,
    SDL3_UI_RUN_LINES = 1,
    SDL3_UI_RUN_HIRES = 2,          /* one hi-res card (index into hires_draws) */
    SDL3_UI_RUN_GLYPH = 3           /* run of FreeType text glyph quads */
} Sdl3UiRunKind;

typedef struct Sdl3UiRun {
    int kind;                       /* Sdl3UiRunKind */
    int first;                      /* vertex offset in the kind's array (TRIS/LINES);
                                       hires_draws index (HIRES) */
    int count;                      /* vertex count (TRIS/LINES); unused (HIRES) */
    int hud;                        /* 1 = widescreen HUD run: render full-width */
} Sdl3UiRun;

/* Full-resolution card art (PC only): a card face/big-art draw substituted for
   the 8bpp blit. Vertices are Sdl3ImageVertex (clip space + 0..1 UV) in
   hires_verts; each draw names the card + framing so the video layer can bind
   the per-card mipmapped texture. group 0 = 3D board card (drawn with the 3D
   solids, full-width viewport); group 1 = 2D card (hand/detail), interleaved in
   the UI run list so draw order (frames, cursors, text over the art) is kept. */
#define SDL3_HIRES_MAX_DRAWS 64
typedef struct Sdl3HiresDraw {
    int first_vertex;               /* into hires_verts (6 per quad) */
    int card_id;
    int kind;                       /* WAIFU_HIRES_FACE / WAIFU_HIRES_BIG */
    int group;                      /* 0 = 3D solid, 1 = 2D UI */
} Sdl3HiresDraw;

typedef struct Sdl3SceneFrame {
    int has_content;                /* anything captured since last reset */
    int cleared;                    /* clear_screen captured this frame: the
                                       persistent canvas restarts from bg_rgba
                                       instead of keeping prior content */

    float bg_rgba[4];               /* frame background (last clear color) */

    /* --- 3D scene --- */
    Sdl3SceneVertex scene_verts[SDL3_SCENE_MAX_SCENE_VERTS];
    int scene_vert_count;

    Sdl3Env env;                    /* story-map sky gradient + infinite floor */

    Sdl3ImageVertex image_verts[SDL3_SCENE_MAX_IMAGE_VERTS];
    int image_vert_count;

    Sdl3LineVertex line_verts[SDL3_SCENE_MAX_LINE_VERTS];
    int line_vert_count;

    /* --- hi-res card art (PC only) --- */
    Sdl3ImageVertex hires_verts[SDL3_HIRES_MAX_DRAWS * 6];
    Sdl3HiresDraw hires_draws[SDL3_HIRES_MAX_DRAWS];
    int hires_draw_count;
    int full_image;                 /* 0 none, 1 title, 2 ending: draw the 16:9
                                       source across the full canvas (PC) */

    /* --- 2D UI layer --- */
    Sdl3UiVertex ui_verts[SDL3_UI_MAX_VERTS];
    int ui_vert_count;
    Sdl3UiVertex ui_line_verts[SDL3_UI_MAX_LINE_VERTS];
    int ui_line_vert_count;
    /* Hi-res text glyph quads: same Sdl3UiVertex layout as the 2D UI but the uv
       is normalized (0..1) into the persistent glyph atlas, sampled linearly by
       the glyph pipeline. Interleaved in the run list to keep draw order. */
    Sdl3UiVertex ui_glyph_verts[SDL3_UI_MAX_GLYPH_VERTS];
    int ui_glyph_vert_count;
    Sdl3UiRun ui_runs[SDL3_UI_MAX_RUNS];
    int ui_run_count;
    int bg_runs;                    /* runs captured before the first 3D
                                       primitive (-1 = no 3D this frame) */

    /* --- textures --- */
    uint32_t image_atlas[SDL3_IMAGE_ATLAS_W * SDL3_IMAGE_ATLAS_H]; /* RGBA8 */
    int image_atlas_used_h;

    uint32_t tile_atlas_rgba[SDL3_TILE_MAX_COUNT * 32 * 32];
    int tile_count;
    uint32_t tile_atlas_serial;     /* bumped when tile_atlas_rgba changes */
} Sdl3SceneFrame;

/* Capture-limit drop counters (sdl3_scene3d.c): a nonzero value means some
   primitive was silently dropped this frame — the visible symptom is a piece
   of the picture missing, e.g. a card's drop shadow drawn with no card in it.
   Reported per frame under WAIFU_SDL3_DEBUG. */
/* Pushes a caller-owned RGBA8 image into this frame's image atlas and draws it
   as a UI quad over the game-space rect (dx,dy,dw,dh). Used by the true-colour
   effects (sdl3_fire.c) that have no 8bpp palette form. Returns 1 when it was
   taken, 0 when the atlas had no room. */
int waifu_sdl3_push_rgba_image(const uint32_t *rgba, int sw, int sh,
                               int dx, int dy, int dw, int dh);

extern int g_sdl3_drop_atlas, g_sdl3_drop_run, g_sdl3_drop_vert, g_sdl3_drop_hires;

/* Owned by sdl3_scene3d.c. */
Sdl3SceneFrame *waifu_sdl3_scene_frame(void);

/* Resets the per-frame capture state (called by the video present after the
   frame's geometry has been uploaded). The background color and converted
   tile atlas persist across frames. */
void waifu_sdl3_scene_frame_reset(void);

/* Drops the persistent sky request after the environment pass consumed it. */
void waifu_sdl3_env_consume_sky(void);

/* Set by the video layer each present: the extra game-x HUD width the current
   widescreen canvas affords (0 = none). Read back by waifu_platform_ui_extra_w. */
void waifu_sdl3_set_ui_extra_w(int extra);

#endif /* WAIFU_SDL3_INTERNAL_H */
