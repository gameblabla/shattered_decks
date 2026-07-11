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
#define SDL3_SCENE_MAX_FLOORS      8

#define SDL3_UI_MAX_VERTS          262144  /* 2D quads (6 verts each) */
#define SDL3_UI_MAX_LINE_VERTS     16384   /* 2D line list */
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

typedef struct Sdl3FloorVertex {
    float x, y, w;
    float wx, wz;       /* world-space XZ, for the checker pattern */
} Sdl3FloorVertex;

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

/* Up to 12 vertices per floor: the plane rectangle is clipped against the
   camera near plane on the CPU (so no vertex is ever behind the eye — a
   whole-plane quad crossing w=0 does not rasterize reliably) and the
   resulting polygon fan-triangulated. */
#define SDL3_FLOOR_VERTS_PER_DRAW 12

typedef struct Sdl3FloorDraw {
    int first_vertex;               /* into floor_verts */
    int vertex_count;
    float tile_a, tile_b;           /* atlas layers for the checker */
    float texels_per_unit;          /* 32 / tile_size_world */
} Sdl3FloorDraw;

typedef enum Sdl3UiRunKind {
    SDL3_UI_RUN_TRIS = 0,
    SDL3_UI_RUN_LINES = 1
} Sdl3UiRunKind;

typedef struct Sdl3UiRun {
    int kind;                       /* Sdl3UiRunKind */
    int first;                      /* vertex offset in the kind's array */
    int count;                      /* vertex count */
} Sdl3UiRun;

typedef struct Sdl3SceneFrame {
    int has_content;                /* anything captured since last reset */
    int cleared;                    /* clear_screen captured this frame: the
                                       persistent canvas restarts from bg_rgba
                                       instead of keeping prior content */

    float bg_rgba[4];               /* frame background (last clear color) */

    /* --- 3D scene --- */
    Sdl3SceneVertex scene_verts[SDL3_SCENE_MAX_SCENE_VERTS];
    int scene_vert_count;

    Sdl3FloorVertex floor_verts[SDL3_SCENE_MAX_FLOORS * SDL3_FLOOR_VERTS_PER_DRAW];
    Sdl3FloorDraw floors[SDL3_SCENE_MAX_FLOORS];
    int floor_count;

    Sdl3ImageVertex image_verts[SDL3_SCENE_MAX_IMAGE_VERTS];
    int image_vert_count;

    Sdl3LineVertex line_verts[SDL3_SCENE_MAX_LINE_VERTS];
    int line_vert_count;

    /* --- 2D UI layer --- */
    Sdl3UiVertex ui_verts[SDL3_UI_MAX_VERTS];
    int ui_vert_count;
    Sdl3UiVertex ui_line_verts[SDL3_UI_MAX_LINE_VERTS];
    int ui_line_vert_count;
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

/* Owned by sdl3_scene3d.c. */
Sdl3SceneFrame *waifu_sdl3_scene_frame(void);

/* Resets the per-frame capture state (called by the video present after the
   frame's geometry has been uploaded). The background color and converted
   tile atlas persist across frames. */
void waifu_sdl3_scene_frame_reset(void);

#endif /* WAIFU_SDL3_INTERNAL_H */
