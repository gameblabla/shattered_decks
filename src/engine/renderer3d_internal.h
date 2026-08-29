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
#ifndef CFX_RENDERER_BUILD_SINGLE_LUT
#define CFX_RENDERER_BUILD_SINGLE_LUT 1
#endif
#ifndef CFX_RENDERER_DIRECT_GENERIC_TILE
#define CFX_RENDERER_DIRECT_GENERIC_TILE 0
#endif
#ifndef CFX_RENDERER_DIV_LUT
#define CFX_RENDERER_DIV_LUT 1
#endif
#ifndef CFX_RENDERER_PAIR_LOW_BYTE_LEFT
#define CFX_RENDERER_PAIR_LOW_BYTE_LEFT 0
#endif
#ifndef CFX_RENDERER_USE_I386_ASM
#define CFX_RENDERER_USE_I386_ASM 0
#endif
#ifndef CFX_RENDERER_SPECIALIZED_BOARD_GRID
#define CFX_RENDERER_SPECIALIZED_BOARD_GRID 0
#endif
#ifndef CFX_RENDERER_DIRECT_FLAT_ROW
#define CFX_RENDERER_DIRECT_FLAT_ROW 0
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
#if defined(WAIFU_PROFILE_RENDER)
    CfxRenderer3DProfile *profile;
#endif
} CfxRenderer3DState;

static inline CfxRenderer3DState *cfx_state(CfxRenderer3D *renderer)
{
    return (CfxRenderer3DState *)(void *)renderer->opaque;
}

#if defined(WAIFU_PROFILE_RENDER)
extern CfxRenderer3DProfile *cfx_renderer3d_active_profile;

static inline void cfx_profile_division_op(void)
{
    if (cfx_renderer3d_active_profile)
        ++cfx_renderer3d_active_profile->division_ops;
}

static inline void cfx_profile_edge_setup(void)
{
    if (cfx_renderer3d_active_profile)
        ++cfx_renderer3d_active_profile->edge_setups;
}

static inline void cfx_profile_board_span(const CfxRenderer3DState *state,
                                          int tilted)
{
    if (state->profile) {
        if (tilted) ++state->profile->tilted_spans;
        else ++state->profile->flat_spans;
    }
}

static inline void cfx_profile_emit_span(const CfxRenderer3DState *state,
                                         int span)
{
    if (state->profile && span > 0) {
        ++state->profile->scanlines;
        ++state->profile->spans;
        state->profile->pixels += (uint32_t)span;
    }
}

static inline void cfx_profile_emit_run(const CfxRenderer3DState *state,
                                        int run)
{
    if (state->profile && run > 0) {
        ++state->profile->runs;
        state->profile->run_pixels += (uint32_t)run;
    }
}

static inline void cfx_profile_emit_global_run(int run)
{
    if (cfx_renderer3d_active_profile && run > 0) {
        ++cfx_renderer3d_active_profile->runs;
        cfx_renderer3d_active_profile->run_pixels += (uint32_t)run;
    }
}
#endif

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

/* Two 8bpp pixels packed into the halfword the span fillers store.
 *
 * Which byte of that halfword is the LEFT pixel is a property of the target,
 * not a free choice.  On PC-FX the halfword is handed to KING, whose KRAM
 * word order puts the left pixel in the high byte; on CD32X the halfword is
 * stored to a byte-linear framebuffer by a big-endian SH-2, where the high
 * byte is the lower address and so is also the left pixel.  Both want the
 * same packing, which is why it was hardcoded.
 *
 * FM TOWNS is neither: a little-endian i386 storing to a byte-linear
 * framebuffer, where the LOW byte is the lower address.  With the high-byte
 * packing every horizontal pixel pair the 3D renderer emitted came out
 * swapped -- textures on the duel board were mirrored in 2-pixel columns.
 * CFX_RENDERER_PAIR_LOW_BYTE_LEFT selects the layout, so the packing follows
 * from how the target's memory is actually addressed instead of from which
 * platform was ported first. */
#if CFX_RENDERER_PAIR_LOW_BYTE_LEFT
static inline void cfx_put_even_pixel_word(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0xff00u) | (uint16_t)color);
}

static inline void cfx_put_odd_pixel_word(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0x00ffu) | ((uint16_t)color << 8));
}

static inline uint16_t cfx_pack_pixel_pair(uint8_t left, uint8_t right)
{
    return (uint16_t)(((uint16_t)right << 8) | (uint16_t)left);
}
#else
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
#endif

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

/* Direct-tile texel fetch helpers, shared by the axis-rect entry points (core)
   and the direct-tile span filler (backend). */
#if CFX_RENDERER_DIRECT_RECT
static inline uint8_t cfx_fetch_direct_texel(const CfxRenderer3DState *renderer, const uint8_t *tile, uint8_t u, uint8_t v)
{
    return tile[(((uint16_t)(v >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK) *
                 (uint16_t)renderer->tile_pitch_bytes) +
                ((uint16_t)(u >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK)];
}

static inline uint8_t cfx_fetch_direct_row_texel(const uint8_t *row, uint8_t u)
{
    return row[(uint16_t)(u >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK];
}
#endif

/* ---- span fillers: defined in the active backend TU ----------------------
   The core (renderer3d.c) walks geometry/edges and calls these once per
   scanline row; the per-pixel loop lives inside them (inlined within the
   backend), so this is the renderer's cross-TU boundary on the hot draw path. */
void cfx_draw_span(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
                   uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v);
/* Per-row emit for the quad-scanline walker, in fixed-point UV. The backend
   internally picks the direct-VRAM path or the normal span, so the core never
   references the platform's direct-VRAM (KRAM) concept. */
void cfx_draw_span_quad_fp(const CfxRenderer3DState *renderer, int16_t y, int16_t x_start, int16_t span,
                           int32_t u_start, int32_t v_start, int32_t du_fp, int32_t dv_fp);
/* 0 if this render does not target a live direct-VRAM page (always 0 on
   backends without one), letting the core choose the rasterization path with no
   platform ifdefs. */
uint8_t cfx_renderer3d_direct_kram_active(const CfxRenderer3DState *renderer);
void cfx_board_fill(uint8_t *dst, int n,
                    int32_t u, int32_t v, int32_t du, int32_t dv, const uint8_t *tile);
#if CFX_RENDERER_DIRECT_RECT
void cfx_draw_span_direct_tile(const CfxRenderer3DState *renderer, const uint8_t *tile,
                               int16_t y, int16_t xs, int16_t span,
                               uint16_t tex_state, int8_t step_u, int8_t step_v);
#if CFX_RENDERER_DIRECT_FLAT_ROW
/* The FM board walker supplies a row that has already been clipped to the
 * framebuffer.  This entry point skips the generic direct-tile wrapper's
 * repeated bounds/step dispatch and goes straight to the constant-V filler. */
void cfx_draw_span_direct_tile_flat_clipped(const CfxRenderer3DState *renderer,
                                            const uint8_t *tile, int16_t y,
                                            int16_t xs, int16_t span,
                                            uint16_t tex_state, int8_t step_u);
#endif
#endif
#if CFX_RENDERER_DIRECT_KRAM
void cfx_draw_span_kram_fp_exact(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
                                 int32_t u_fp, int32_t v_fp, int32_t du_fp, int32_t dv_fp);
#endif

#endif /* CFX_RENDERER3D_INTERNAL_H */
