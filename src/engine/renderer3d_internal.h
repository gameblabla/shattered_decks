#ifndef CFX_RENDERER3D_INTERNAL_H
#define CFX_RENDERER3D_INTERNAL_H

/* Shared internals for the 3D renderer split across translation units:
 *   - renderer3d.c          : platform-agnostic geometry, edge walking, entry
 *                             points and public API; calls the span fillers.
 *   - renderer3d_generic.c  : portable C span/scanline fillers (host/exotic).
 *   - renderer3d_pcfx.c     : PC-FX V810/KING span/scanline fillers.
 * Exactly one backend is linked per build (chosen by the Makefile).
 *
 * Everything here is platform-independent: the struct/layout the walkers and
 * fillers share, plus small pure inline helpers. The hot per-pixel work lives
 * inside each backend's span fillers (inlined within that TU); the walkers call
 * those fillers once per scanline row, so this header carries no per-pixel
 * indirection. */

#include "renderer3d.h"
#include "renderer3d_port.h"
#include <stddef.h>

#ifndef CFX_RENDERER_DIRECT_KRAM
#define CFX_RENDERER_DIRECT_KRAM 0
#endif
#ifndef CFX_RENDERER_DIRECT_ROW_LUT
#define CFX_RENDERER_DIRECT_ROW_LUT 0
#endif
#ifndef CFX_RENDERER_QUAD_SCANLINE
#define CFX_RENDERER_QUAD_SCANLINE 0
#endif
#ifndef CFX_RENDERER_DIRECT_GENERIC_TILE
#define CFX_RENDERER_DIRECT_GENERIC_TILE 0
#endif

#define CFX_TEX_SIZE CFX_TEXTURE_TILE_SIZE
#define CFX_TEX_MASK (CFX_TEX_SIZE - 1)
#define CFX_FIXED_POINT_SHIFT ((CFX_TEX_SIZE == 32) ? 3 : 4)
#define CFX_GEOM_FIXED_SHIFT 8
#define CFX_QUAD_UV_SHIFT 8
#define CFX_EDGE_UV_SHIFT 16
#define CFX_MAX_TEXTURE_TILES 7

typedef struct {
    uint8_t *framebuffer;
    const uint8_t *texture_atlas;
    DEFAULT_INT width;
    DEFAULT_INT height;
    DEFAULT_INT tile_pitch_bytes;
    DEFAULT_INT tile_stride_bytes;
} CfxRenderer3DState;

static inline CfxRenderer3DState *cfx_state(CfxRenderer3D *renderer)
{
    return (CfxRenderer3DState *)(void *)renderer->opaque;
}

/* The currently-selected 16x16 -> palette-index texel LUT, shared between the
   LUT builders (renderer3d.c) and the span fillers (backend). It is plain
   global state; with V810 -msda=0 an extern global is absolute-addressed, so a
   per-pixel read costs the same as a file-static would. */
extern const uint8_t *active_tex_lut;

/* ---- pure inline helpers (no globals, no platform code) ------------------ */

static inline int32_t cfx_abs_i32(int32_t v) { return v < 0 ? -v : v; }

static inline uint32_t cfx_abs_i32_u32(int32_t v)
{
    return (v < 0) ? ((uint32_t)(-(v + 1)) + 1u) : (uint32_t)v;
}

static inline int32_t cfx_int_to_fixed(int32_t x) { return x << CFX_GEOM_FIXED_SHIFT; }
static inline uint16_t cfx_pack_tex_state(uint8_t u, uint8_t v) { return (uint16_t)(((uint16_t)v << 8) | (uint16_t)u); }
static inline int16_t cfx_pack_tex_step_linear(int8_t du, int8_t dv)
{
    return (int16_t)(((int16_t)dv << 8) + (int16_t)du);
}

static inline uint16_t cfx_advance_tex_state(uint16_t state, int8_t du, int8_t dv)
{
    uint8_t u = (uint8_t)((uint8_t)state + (uint8_t)du);
    uint8_t v = (uint8_t)((uint8_t)(state >> 8) + (uint8_t)dv);
    return cfx_pack_tex_state(u, v);
}

static inline uint16_t cfx_advance_tex_state_n(uint16_t state, int8_t du, int8_t dv, uint16_t count)
{
    uint8_t u = (uint8_t)((uint8_t)state + (uint8_t)((int16_t)du * (int16_t)count));
    uint8_t v = (uint8_t)((uint8_t)(state >> 8) + (uint8_t)((int16_t)dv * (int16_t)count));
    return cfx_pack_tex_state(u, v);
}

static inline void cfx_put_even_pixel_word(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0x00ffu) | ((uint16_t)color << 8));
}

static inline void cfx_put_odd_pixel_word(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0xff00u) | (uint16_t)color);
}

static inline uint16_t cfx_pack_pixel_pair(uint8_t left, uint8_t right)
{
    return (uint16_t)(((uint16_t)left << 8) | (uint16_t)right);
}

static inline uint8_t cfx_fetch_texel(uint16_t state)
{
    return active_tex_lut[state];
}

static inline uint8_t cfx_clamp_u8_i32(int32_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline uint8_t cfx_fetch_texel_fp(int32_t u_fp, int32_t v_fp)
{
    return cfx_fetch_texel(cfx_pack_tex_state(
        cfx_clamp_u8_i32(u_fp >> CFX_QUAD_UV_SHIFT),
        cfx_clamp_u8_i32(v_fp >> CFX_QUAD_UV_SHIFT)));
}

/* ---- span fillers: defined in the active backend TU ----------------------
   The core (renderer3d.c) walks geometry/edges and calls these once per
   scanline row; the per-pixel loop lives inside them (inlined within the
   backend), so this is the renderer's cross-TU boundary on the hot draw path. */
void cfx_draw_span(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
                   uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v);
void cfx_board_fill(uint8_t *dst, int n,
                    int32_t u, int32_t v, int32_t du, int32_t dv, const uint8_t *tile);
#if CFX_RENDERER_DIRECT_KRAM
void cfx_draw_span_kram_fp_exact(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
                                 int32_t u_fp, int32_t v_fp, int32_t du_fp, int32_t dv_fp);
#endif

#endif /* CFX_RENDERER3D_INTERNAL_H */
