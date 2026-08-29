#ifndef CFX_RENDERER3D_H
#define CFX_RENDERER3D_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"
#include "defines.h"

#define CFX_RENDERER3D_STORAGE_WORDS 8

#define CFX_TEXTURE_TILE_SIZE 32
#define CFX_TEXTURE_TILE_COUNT 7
#define CFX_TEXTURE_TILE_PITCH_BYTES CFX_TEXTURE_TILE_SIZE
#define CFX_TEXTURE_TILE_STRIDE_BYTES (CFX_TEXTURE_TILE_SIZE * CFX_TEXTURE_TILE_SIZE)
#define CFX_TEXTURE_ATLAS_WIDTH CFX_TEXTURE_TILE_SIZE
#define CFX_TEXTURE_ATLAS_HEIGHT (CFX_TEXTURE_TILE_SIZE * CFX_TEXTURE_TILE_COUNT)

typedef struct CfxRenderer3D {
    uintptr_t opaque[CFX_RENDERER3D_STORAGE_WORDS];
} CfxRenderer3D;

typedef struct CfxBoardPoint {
    int16_t x;
    int16_t y;
} CfxBoardPoint;

#if defined(WAIFU_PROFILE_RENDER)
/* Profiling-only counters.  The pointer is absent from shipping renderer
   state unless a measurement build opts in, so these counters cannot change
   the FM TOWNS payload or hot-loop ABI. */
typedef struct CfxRenderer3DProfile {
    uint32_t board_path;
    uint32_t cells;
    uint32_t scanlines;
    uint32_t spans;
    uint32_t pixels;
    uint32_t flat_spans;
    uint32_t tilted_spans;
    uint32_t runs;
    uint32_t run_pixels;
    uint32_t edge_setups;
    uint32_t division_ops;
} CfxRenderer3DProfile;

enum {
    CFX_PROFILE_BOARD_AXIS = 1,
    CFX_PROFILE_BOARD_TRAPEZOID_ROWS = 2,
    CFX_PROFILE_BOARD_TRAPEZOID = 3,
    CFX_PROFILE_BOARD_CACHED_EDGES = 4,
    CFX_PROFILE_BOARD_FALLBACK = 5,
    CFX_PROFILE_BOARD_SPECIALIZED_GRID = 6
};
#endif

typedef struct {
    void *framebuffer;
    DEFAULT_INT width;
    DEFAULT_INT height;
} CfxRenderer3DConfig;

void cfx_renderer3d_init(CfxRenderer3D *renderer, const CfxRenderer3DConfig *config);
void cfx_renderer3d_set_framebuffer(CfxRenderer3D *renderer, void *framebuffer);
void cfx_renderer3d_set_texture_atlas(CfxRenderer3D *renderer, const void *atlas, DEFAULT_INT tile_pitch_bytes, DEFAULT_INT tile_stride_bytes);
void cfx_renderer3d_draw_face_list(CfxRenderer3D *renderer, FaceToDraw *faces, DEFAULT_INT face_count);
void cfx_renderer3d_draw_quad(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type);
void cfx_renderer3d_draw_quad_board(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type);
void cfx_renderer3d_draw_quad_board_band(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type, DEFAULT_INT y0, DEFAULT_INT y1);
void cfx_renderer3d_draw_quad_offscreen_direct(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type);
/* Division-free affine quad path for cached 3D board/background quads.
   It is intentionally approximate but stable; the generic renderer remains
   available for objects that need the old triangle path. */
uint8_t cfx_renderer3d_draw_quad_fast_affine(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type);
uint8_t cfx_renderer3d_draw_board_mesh_fast_affine(
    CfxRenderer3D *renderer, const CfxBoardPoint *points,
    DEFAULT_INT point_stride, DEFAULT_INT rows, DEFAULT_INT cols,
    DEFAULT_INT even_tile, DEFAULT_INT odd_tile);
uint32_t cfx_renderer3d_lut_size_bytes(void);
#if defined(WAIFU_PROFILE_RENDER)
void cfx_renderer3d_set_profile(CfxRenderer3D *renderer, CfxRenderer3DProfile *profile);
#endif

#endif
