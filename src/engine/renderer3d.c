#include "renderer3d_internal.h"

#if defined(WAIFU_PROFILE_RENDER)
CfxRenderer3DProfile *cfx_renderer3d_active_profile;
#endif

typedef struct {
    int16_t x, y;
    uint16_t u, v;
} CfxVertexIn;

typedef struct {
    const CfxVertexIn *vertices[3];
    int16_t section;
    int16_t section_height;
    int32_t x;
    int32_t delta_x;
    int32_t u;
    int32_t v;
    int32_t delta_u;
    int32_t delta_v;
} CfxLeftEdge;

typedef struct {
    const CfxVertexIn *vertices[3];
    int16_t section;
    int16_t section_height;
    int32_t x;
    int32_t delta_x;
} CfxRightEdge;

typedef struct {
    int16_t y_start;
    int16_t y_end;
    int32_t x;
    int32_t x_step;
    int32_t u;
    int32_t v;
    int32_t u_step;
    int32_t v_step;
} CfxQuadEdge;

/* Active 16x16 texture lookup table.  The previous renderer used this
   64 KiB table only as a texel-index LUT, then performed a second texture
   fetch per pixel.  The fast 16x16 mapper is intended to make the inner
   span loop one LUT read + one state add per pixel, so this table stores the
   final palette-index texel for the currently selected 16x16 tile.
   The texture palette is remapped below index 128 so V810/SH1 signed byte
   loads can be packed directly without zero-extension in the hot loop. */
#if CFX_RENDERER_BUILD_SINGLE_LUT
static uint8_t tex_lut[1u << 16] __attribute__((aligned(16)));
#else
static uint8_t tex_lut[1] __attribute__((aligned(16)));
#endif
#if CFX_RENDERER_MULTI_LUT
static uint8_t tex_lut_tiles[CFX_MAX_TEXTURE_TILES][1u << 16] __attribute__((aligned(16)));
const uint8_t *active_tex_lut = tex_lut_tiles[0];
static const uint8_t *tex_lut_tiles_source_atlas;
static DEFAULT_INT tex_lut_tiles_source_pitch;
static DEFAULT_INT tex_lut_tiles_source_stride;
static uint8_t tex_lut_tiles_ready;
static uint8_t tex_lut_tile_ready[CFX_MAX_TEXTURE_TILES];
#else
const uint8_t *active_tex_lut = tex_lut;
#endif
static const uint8_t *tex_lut_source_tile;
static DEFAULT_INT tex_lut_source_pitch;
static uint8_t tex_lut_ready;

#if CFX_RENDERER_DIRECT_ROW_LUT
static uint8_t tex_row_lut_tiles[CFX_MAX_TEXTURE_TILES][CFX_TEX_SIZE][256] __attribute__((aligned(16)));
static const uint8_t *tex_row_lut_source_atlas;
static DEFAULT_INT tex_row_lut_source_pitch;
static DEFAULT_INT tex_row_lut_source_stride;
static uint8_t tex_row_lut_ready;
#endif

#define CFX_DIV_CORRECTION_LIMIT 8u

static inline int32_t cfx_div_apply_sign(uint32_t q, uint8_t neg)
{
    if (!neg) {
        return (q > 0x7fffffffu) ? (int32_t)0x7fffffffu : (int32_t)q;
    }
    if (q >= 0x80000000u) {
        return (int32_t)0x80000000u;
    }
    return -(int32_t)q;
}

static inline uint32_t cfx_div_refine_u32(uint32_t un, uint32_t ud, uint32_t q)
{
    uint32_t prod;
    uint32_t corrections = 0;

    if (ud == 0u) return 0u;
    if (ud == 1u) return un;
    if (q != 0u && ud > (0xffffffffu / q)) {
        return un / ud;
    }

    prod = q * ud;
    while (prod > un) {
        if (corrections++ >= CFX_DIV_CORRECTION_LIMIT) {
            return un / ud;
        }
        --q;
        prod -= ud;
    }
    while ((uint32_t)(un - prod) >= ud) {
        if (corrections++ >= CFX_DIV_CORRECTION_LIMIT || prod > 0xffffffffu - ud) {
            return un / ud;
        }
        ++q;
        prod += ud;
    }
    return q;
}

#if CFX_RENDERER_DIV_LUT
/* Reciprocal LUTs, built once at renderer init instead of being stored in the
   image: 2 KiB of pure floor/round(K/d) data was a real cost against the CD32X
   128 KiB SH-2 staging budget.  Values are exactly the old constant tables:
   q15 = 32768/d, q24 = (1<<24 + d/2)/d, q8 = (256 + d/2)/d.  Any renderer use
   before cfx_renderer3d_init() would read zeros, so keep init first. */
static uint16_t cfx_recip_q15_u8[257];
static uint32_t cfx_recip_q24_u8[257];
static uint16_t cfx_recip_q8_u16[257];
static uint8_t cfx_floor_255_u8[257];
static uint8_t cfx_ceil_255_u8[257];

static void cfx_recip_tables_build(void)
{
    if (cfx_recip_q15_u8[1]) return;
    for (int d = 1; d <= 256; ++d) {
        cfx_recip_q15_u8[d] = (uint16_t)(32768u / (uint32_t)d);
        cfx_recip_q24_u8[d] = (uint32_t)((16777216u + (uint32_t)(d >> 1)) / (uint32_t)d);
        cfx_recip_q8_u16[d] = (uint16_t)((256u + (uint32_t)(d >> 1)) / (uint32_t)d);
        cfx_floor_255_u8[d] = (uint8_t)(255u / (uint32_t)d);
        cfx_ceil_255_u8[d] = (uint8_t)((255u + (uint32_t)d - 1u) / (uint32_t)d);
    }
}

static inline int8_t cfx_board_step_255(int positive, uint16_t denom)
{
    if (denom == 0) return 0;
    if (denom > 256) denom = 256;
    /* The old Q8 divide followed by an arithmetic >>8 is floor(255/d) for a
       positive delta and -ceil(255/d) for a negative one. */
    return positive ? (int8_t)cfx_floor_255_u8[denom]
                    : (int8_t)-(int)cfx_ceil_255_u8[denom];
}
#endif

static inline int32_t cfx_fast_div_tz_i32_u16_q15(int32_t n, uint16_t d)
{
    if (d == 0) return 0;
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_division_op();
#endif
    if (d == 1) return n;
    if (d > 256) {
        /* Fast board path only uses screen-sized spans/edges.  Clamp rather
           than falling back to DIV so the V810 hot renderer remains division-free. */
        d = 256;
    }
#if !CFX_RENDERER_DIV_LUT
    return n / (int32_t)d;
#else
    uint32_t a;
    uint8_t neg = 0;
    if (n < 0) {
        neg = 1;
        a = (uint32_t)(-n);
    } else {
        a = (uint32_t)n;
    }
    uint32_t q = (a * (uint32_t)cfx_recip_q15_u8[d]) >> 15;
    return neg ? -(int32_t)q : (int32_t)q;
#endif
}

static inline int32_t cfx_div_toward_zero(int32_t n, int16_t d)
{
    if (d == 0) return 0;
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_division_op();
#endif
    uint8_t neg = 0;
    uint32_t un = cfx_abs_i32_u32(n);
    if (n < 0) neg ^= 1;
    uint32_t ud = (d < 0) ? (uint32_t)(-(int32_t)d) : (uint32_t)d;
    if (d < 0) neg ^= 1;
    uint32_t q;
#if !CFX_RENDERER_DIV_LUT
    q = un / ud;
#else
    if (ud == 1) {
        q = un;
    } else if (ud <= 256u) {
        if ((un & 0xffffu) == 0u && (un >> 16) <= 255u) {
            q = (((un >> 16) * cfx_recip_q24_u8[ud]) >> 8);
        } else if ((un & 0xffu) == 0u && (un >> 8) <= 511u) {
            q = (((un >> 8) * cfx_recip_q24_u8[ud]) >> 16);
        } else if (un <= 131071u) {
            q = ((un * (uint32_t)cfx_recip_q15_u8[ud]) >> 15);
        } else {
            q = ((un * (uint32_t)cfx_recip_q8_u16[ud]) >> 8);
        }
    } else {
        uint32_t scaled = ud;
        uint16_t shift = 0;
        while (scaled > 256u) { scaled = (uint16_t)((scaled + 1u) >> 1); ++shift; }
        q = ((un * (uint32_t)cfx_recip_q8_u16[scaled]) >> (8 + shift));
    }
#endif

    /* Reciprocal estimate, then exact toward-zero correction.  Bad projection
       inputs can make the estimate much farther off than the normal board
       range; cap the correction loop and fall back to a real 32-bit divide so
       PC-FX cannot park in this helper for whole frames. */
    q = cfx_div_refine_u32(un, (uint32_t)ud, q);
    return cfx_div_apply_sign(q, neg);
}

static inline int32_t cfx_div_toward_zero_i32d(int32_t n, int32_t d)
{
    if (d == 0) return 0;
    uint8_t neg = 0;
    uint32_t un = cfx_abs_i32_u32(n);
    uint32_t ud = cfx_abs_i32_u32(d);
    if (n < 0) neg ^= 1;
    if (d < 0) neg ^= 1;
    uint32_t q;
#if !CFX_RENDERER_DIV_LUT
    q = un / ud;
#else
    if (ud == 1u) {
        q = un;
    } else if (ud <= 256u) {
        q = (uint32_t)cfx_div_toward_zero((int32_t)un, (int16_t)ud);
    } else {
        uint32_t scaled = ud;
        uint16_t shift = 0;
        while (scaled > 256u) { scaled = (scaled + 1u) >> 1; ++shift; }
        q = ((un * (uint32_t)cfx_recip_q8_u16[scaled]) >> (8 + shift));
    }
#endif
    q = cfx_div_refine_u32(un, ud, q);
    return cfx_div_apply_sign(q, neg);
}


static void cfx_fill_lut_from_tile(const CfxRenderer3DState *renderer, const uint8_t *tile, uint8_t *lut)
{
    for (uint16_t v = 0; v < 256; ++v) {
        const uint8_t *src_row = tile + (((uint16_t)(v >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK) *
            (uint16_t)renderer->tile_pitch_bytes);
        uint8_t *dst = lut + ((uint16_t)v << 8);
        for (uint16_t u = 0; u < 256; ++u) {
            dst[u] = src_row[(uint16_t)(u >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK];
        }
    }
}

#if CFX_RENDERER_DIRECT_ROW_LUT
static void cfx_renderer3d_build_direct_row_luts(const CfxRenderer3DState *renderer)
{
    if (tex_row_lut_ready &&
        tex_row_lut_source_atlas == renderer->texture_atlas &&
        tex_row_lut_source_pitch == renderer->tile_pitch_bytes &&
        tex_row_lut_source_stride == renderer->tile_stride_bytes) {
        return;
    }

    for (uint16_t tile_index = 0; tile_index < CFX_MAX_TEXTURE_TILES; ++tile_index) {
        const uint8_t *tile = renderer->texture_atlas + ((int32_t)tile_index * renderer->tile_stride_bytes);
        for (uint16_t row = 0; row < CFX_TEX_SIZE; ++row) {
            const uint8_t *src_row = tile + ((uint16_t)row * (uint16_t)renderer->tile_pitch_bytes);
            uint8_t *dst = tex_row_lut_tiles[tile_index][row];
            for (uint16_t u = 0; u < 256; ++u) {
                dst[u] = src_row[(uint16_t)(u >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK];
            }
        }
    }

    tex_row_lut_source_atlas = renderer->texture_atlas;
    tex_row_lut_source_pitch = renderer->tile_pitch_bytes;
    tex_row_lut_source_stride = renderer->tile_stride_bytes;
    tex_row_lut_ready = 1;
}

static inline const uint8_t *cfx_renderer3d_direct_row_lut(const CfxRenderer3DState *renderer, const uint8_t *tile, uint8_t v)
{
    if (!renderer->texture_atlas || renderer->tile_stride_bytes <= 0) {
        return NULL;
    }

    intptr_t byte_delta = tile - renderer->texture_atlas;
    if (byte_delta < 0 || (byte_delta % renderer->tile_stride_bytes) != 0) {
        return NULL;
    }

    intptr_t tile_index = byte_delta / renderer->tile_stride_bytes;
    if (tile_index < 0 || tile_index >= CFX_MAX_TEXTURE_TILES) {
        return NULL;
    }

    cfx_renderer3d_build_direct_row_luts(renderer);
    return tex_row_lut_tiles[tile_index][(uint16_t)(v >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK];
}
#endif

#if CFX_RENDERER_MULTI_LUT
static void cfx_renderer3d_reset_pcfx_tile_lut_cache(const CfxRenderer3DState *renderer)
{
    if (tex_lut_tiles_ready && tex_lut_tiles_source_atlas == renderer->texture_atlas &&
        tex_lut_tiles_source_pitch == renderer->tile_pitch_bytes &&
        tex_lut_tiles_source_stride == renderer->tile_stride_bytes) {
        return;
    }

    for (uint16_t tile_index = 0; tile_index < CFX_MAX_TEXTURE_TILES; ++tile_index) {
        tex_lut_tile_ready[tile_index] = 0;
    }

    tex_lut_tiles_source_atlas = renderer->texture_atlas;
    tex_lut_tiles_source_pitch = renderer->tile_pitch_bytes;
    tex_lut_tiles_source_stride = renderer->tile_stride_bytes;
    tex_lut_tiles_ready = 1;
}

static void cfx_renderer3d_select_pcfx_tile_lut(const CfxRenderer3DState *renderer, DEFAULT_INT tile_index)
{
    cfx_renderer3d_reset_pcfx_tile_lut_cache(renderer);

    if (tile_index < 0) {
        tile_index = 0;
    }
    if (tile_index >= CFX_MAX_TEXTURE_TILES) {
        tile_index = CFX_MAX_TEXTURE_TILES - 1;
    }

    if (!tex_lut_tile_ready[tile_index]) {
        const uint8_t *tile = renderer->texture_atlas + ((int32_t)tile_index * renderer->tile_stride_bytes);
        cfx_fill_lut_from_tile(renderer, tile, tex_lut_tiles[tile_index]);
        tex_lut_tile_ready[tile_index] = 1;
    }

    active_tex_lut = tex_lut_tiles[tile_index];
}

static void cfx_renderer3d_prebuild_pcfx_tile_luts(const CfxRenderer3DState *renderer)
{
    cfx_renderer3d_reset_pcfx_tile_lut_cache(renderer);
    for (uint16_t tile_index = 0; tile_index < CFX_MAX_TEXTURE_TILES; ++tile_index) {
        if (!tex_lut_tile_ready[tile_index]) {
            const uint8_t *tile = renderer->texture_atlas + ((int32_t)tile_index * renderer->tile_stride_bytes);
            cfx_fill_lut_from_tile(renderer, tile, tex_lut_tiles[tile_index]);
            tex_lut_tile_ready[tile_index] = 1;
        }
    }
    active_tex_lut = tex_lut_tiles[0];
}
#endif

static void cfx_renderer3d_build_lut_for_tile(const CfxRenderer3DState *renderer, const uint8_t *tile)
{
#if CFX_RENDERER_BUILD_SINGLE_LUT
    if (tex_lut_ready && tex_lut_source_tile == tile && tex_lut_source_pitch == renderer->tile_pitch_bytes) {
        active_tex_lut = tex_lut;
        return;
    }

    cfx_fill_lut_from_tile(renderer, tile, tex_lut);

    tex_lut_source_tile = tile;
    tex_lut_source_pitch = renderer->tile_pitch_bytes;
    tex_lut_ready = 1;
    active_tex_lut = tex_lut;
#else
    (void)renderer;
    (void)tile;
    active_tex_lut = tex_lut;
    tex_lut_ready = 1;
    tex_lut_source_tile = NULL;
    tex_lut_source_pitch = 0;
#endif
}

uint32_t cfx_renderer3d_lut_size_bytes(void)
{
#if CFX_RENDERER_MULTI_LUT
    return (uint32_t)(sizeof(tex_lut) + sizeof(tex_lut_tiles));
#elif CFX_RENDERER_DIRECT_ROW_LUT
    return (uint32_t)(sizeof(tex_lut) + sizeof(tex_row_lut_tiles));
#else
    return CFX_RENDERER_BUILD_SINGLE_LUT ? (uint32_t)sizeof(tex_lut) : 0u;
#endif
}

void cfx_renderer3d_init(CfxRenderer3D *renderer, const CfxRenderer3DConfig *config)
{
    CfxRenderer3DState *state = cfx_state(renderer);
#if CFX_RENDERER_DIV_LUT
    cfx_recip_tables_build();
#endif
    tex_lut_ready = 0;
    tex_lut_source_tile = NULL;
    active_tex_lut = tex_lut;
#if CFX_RENDERER_DIRECT_ROW_LUT
    tex_row_lut_ready = 0;
    tex_row_lut_source_atlas = NULL;
#endif
#if CFX_RENDERER_MULTI_LUT
    tex_lut_tiles_ready = 0;
    tex_lut_tiles_source_atlas = NULL;
#endif
    state->framebuffer = (uint8_t *)config->framebuffer;
    state->texture_atlas = NULL;
    state->width = config->width;
    state->height = config->height;
    state->tile_pitch_bytes = CFX_TEXTURE_TILE_PITCH_BYTES;
    state->tile_stride_bytes = CFX_TEXTURE_TILE_STRIDE_BYTES;
#if defined(WAIFU_PROFILE_RENDER)
    state->profile = NULL;
#endif
}

void cfx_renderer3d_set_framebuffer(CfxRenderer3D *renderer, void *framebuffer)
{
    cfx_state(renderer)->framebuffer = (uint8_t *)framebuffer;
}

void cfx_renderer3d_set_texture_atlas(CfxRenderer3D *renderer, const void *atlas, DEFAULT_INT tile_pitch_bytes, DEFAULT_INT tile_stride_bytes)
{
    CfxRenderer3DState *state = cfx_state(renderer);
    state->texture_atlas = (const uint8_t *)atlas;
    state->tile_pitch_bytes = tile_pitch_bytes;
    state->tile_stride_bytes = tile_stride_bytes;
    tex_lut_ready = 0;
    tex_lut_source_tile = NULL;
    active_tex_lut = tex_lut;
#if CFX_RENDERER_DIRECT_ROW_LUT
    tex_row_lut_ready = 0;
    tex_row_lut_source_atlas = NULL;
    cfx_renderer3d_build_direct_row_luts(state);
#endif
#if CFX_RENDERER_MULTI_LUT
    tex_lut_tiles_ready = 0;
    tex_lut_tiles_source_atlas = NULL;
    cfx_renderer3d_prebuild_pcfx_tile_luts(state);
#endif
}

#if defined(WAIFU_PROFILE_RENDER)
void cfx_renderer3d_set_profile(CfxRenderer3D *renderer, CfxRenderer3DProfile *profile)
{
    cfx_state(renderer)->profile = profile;
    cfx_renderer3d_active_profile = profile;
}
#endif


static inline const CfxVertexIn *cfx_find_rect_corner_common(const CfxVertexIn *v0, const CfxVertexIn *v1,
                                                             const CfxVertexIn *v2, const CfxVertexIn *v3,
                                                             int16_t x, int16_t y)
{
    if (v0->x == x && v0->y == y) return v0;
    if (v1->x == x && v1->y == y) return v1;
    if (v2->x == x && v2->y == y) return v2;
    if (v3->x == x && v3->y == y) return v3;
    return NULL;
}

static uint8_t cfx_get_axis_rect_corners(const CfxVertexIn *v0, const CfxVertexIn *v1,
                                          const CfxVertexIn *v2, const CfxVertexIn *v3,
                                          const CfxVertexIn **tl, const CfxVertexIn **tr,
                                          const CfxVertexIn **bl, int16_t *left, int16_t *right,
                                          int16_t *top, int16_t *bottom)
{
    const CfxVertexIn *vs[4] = { v0, v1, v2, v3 };
    *left = v0->x;
    *right = v0->x;
    *top = v0->y;
    *bottom = v0->y;
    for (uint16_t i = 1; i < 4; ++i) {
        if (vs[i]->x < *left) *left = vs[i]->x;
        if (vs[i]->x > *right) *right = vs[i]->x;
        if (vs[i]->y < *top) *top = vs[i]->y;
        if (vs[i]->y > *bottom) *bottom = vs[i]->y;
    }

    *tl = cfx_find_rect_corner_common(v0, v1, v2, v3, *left, *top);
    *tr = cfx_find_rect_corner_common(v0, v1, v2, v3, *right, *top);
    *bl = cfx_find_rect_corner_common(v0, v1, v2, v3, *left, *bottom);
    return (uint8_t)(*tl && *tr && *bl && *right >= *left && *bottom >= *top);
}

static uint8_t cfx_draw_axis_rect_lut(const CfxRenderer3DState *renderer,
                                      const CfxVertexIn *v0, const CfxVertexIn *v1,
                                      const CfxVertexIn *v2, const CfxVertexIn *v3)
{
    const CfxVertexIn *tl;
    const CfxVertexIn *tr;
    const CfxVertexIn *bl;
    int16_t left, right, top, bottom;
    if (!cfx_get_axis_rect_corners(v0, v1, v2, v3, &tl, &tr, &bl, &left, &right, &top, &bottom)) {
        return 0;
    }

    int16_t width = (int16_t)(right - left + 1);
    int16_t height = (int16_t)(bottom - top + 1);
    if (width <= 0 || height <= 0) {
        return 1;
    }

    int16_t span_step_u = 0;
    int16_t span_step_v = 0;
    int16_t row_step_u = 0;
    int16_t row_step_v = 0;
    if (width > 1) {
        span_step_u = (int16_t)cfx_div_toward_zero((int16_t)(tr->u - tl->u), (int16_t)(width - 1));
        span_step_v = (int16_t)cfx_div_toward_zero((int16_t)(tr->v - tl->v), (int16_t)(width - 1));
    }
    if (height > 1) {
        row_step_u = (int16_t)cfx_div_toward_zero((int16_t)(bl->u - tl->u), (int16_t)(height - 1));
        row_step_v = (int16_t)cfx_div_toward_zero((int16_t)(bl->v - tl->v), (int16_t)(height - 1));
    }

    int16_t row_u = (int16_t)tl->u;
    int16_t row_v = (int16_t)tl->v;
    int16_t span_step_linear = cfx_pack_tex_step_linear((int8_t)span_step_u, (int8_t)span_step_v);
    for (int16_t y = top; y <= bottom; ++y) {
        cfx_draw_span(renderer, y, left, width,
            cfx_pack_tex_state((uint8_t)row_u, (uint8_t)row_v),
            span_step_linear, (int8_t)span_step_u, (int8_t)span_step_v);
        row_u = (int16_t)(row_u + row_step_u);
        row_v = (int16_t)(row_v + row_step_v);
    }
    return 1;
}

#if CFX_RENDERER_DIRECT_RECT


static uint8_t cfx_draw_axis_rect_direct_tile(const CfxRenderer3DState *renderer, const uint8_t *tile,
                                              const CfxVertexIn *v0, const CfxVertexIn *v1,
                                              const CfxVertexIn *v2, const CfxVertexIn *v3)
{
    const CfxVertexIn *tl;
    const CfxVertexIn *tr;
    const CfxVertexIn *bl;
    int16_t left, right, top, bottom;
    if (!cfx_get_axis_rect_corners(v0, v1, v2, v3, &tl, &tr, &bl, &left, &right, &top, &bottom)) {
        return 0;
    }

    int16_t width = (int16_t)(right - left + 1);
    int16_t height = (int16_t)(bottom - top + 1);
    if (width <= 0 || height <= 0) {
        return 1;
    }

    int16_t span_step_u = 0;
    int16_t span_step_v = 0;
    int16_t row_step_u = 0;
    int16_t row_step_v = 0;
    if (width > 1) {
        span_step_u = (int16_t)cfx_div_toward_zero((int16_t)(tr->u - tl->u), (int16_t)(width - 1));
        span_step_v = (int16_t)cfx_div_toward_zero((int16_t)(tr->v - tl->v), (int16_t)(width - 1));
    }
    if (height > 1) {
        row_step_u = (int16_t)cfx_div_toward_zero((int16_t)(bl->u - tl->u), (int16_t)(height - 1));
        row_step_v = (int16_t)cfx_div_toward_zero((int16_t)(bl->v - tl->v), (int16_t)(height - 1));
    }

    int16_t row_u = (int16_t)tl->u;
    int16_t row_v = (int16_t)tl->v;
    for (int16_t y = top; y <= bottom; ++y) {
        cfx_draw_span_direct_tile(renderer, tile, y, left, width,
            cfx_pack_tex_state((uint8_t)row_u, (uint8_t)row_v),
            (int8_t)span_step_u, (int8_t)span_step_v);
        row_u = (int16_t)(row_u + row_step_u);
        row_v = (int16_t)(row_v + row_step_v);
    }
    return 1;
}
#endif

static int cfx_left_section(CfxLeftEdge *edge)
{
    const CfxVertexIn *v1 = edge->vertices[edge->section];
    const CfxVertexIn *v2 = edge->vertices[edge->section - 1];
    int16_t height = (int16_t)(v2->y - v1->y);
    if (height == 0) {
        return 0;
    }

    edge->section_height = height;
    edge->delta_x = cfx_div_toward_zero(cfx_int_to_fixed((int32_t)v2->x - (int32_t)v1->x), height);
    edge->x = cfx_int_to_fixed(v1->x);
    edge->u = (int32_t)v1->u << CFX_EDGE_UV_SHIFT;
    edge->v = (int32_t)v1->v << CFX_EDGE_UV_SHIFT;
    edge->delta_u = cfx_div_toward_zero(((int32_t)v2->u - (int32_t)v1->u) << CFX_EDGE_UV_SHIFT, height);
    edge->delta_v = cfx_div_toward_zero(((int32_t)v2->v - (int32_t)v1->v) << CFX_EDGE_UV_SHIFT, height);
    return height;
}

static int cfx_right_section(CfxRightEdge *edge)
{
    const CfxVertexIn *v1 = edge->vertices[edge->section];
    const CfxVertexIn *v2 = edge->vertices[edge->section - 1];
    int16_t height = (int16_t)(v2->y - v1->y);
    if (height == 0) {
        return 0;
    }

    edge->section_height = height;
    edge->delta_x = cfx_div_toward_zero(cfx_int_to_fixed((int32_t)v2->x - (int32_t)v1->x), height);
    edge->x = cfx_int_to_fixed(v1->x);
    return height;
}

#if CFX_RENDERER_QUAD_SCANLINE
static void cfx_quad_build_edge(CfxQuadEdge *edge, const CfxVertexIn *a, const CfxVertexIn *b)
{
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_edge_setup();
#endif
    int16_t dy = (int16_t)(b->y - a->y);
    if (dy == 0) {
        edge->y_start = a->y;
        edge->y_end = a->y;
        edge->x = cfx_int_to_fixed(a->x);
        edge->x_step = 0;
        edge->u = (int32_t)a->u << CFX_QUAD_UV_SHIFT;
        edge->v = (int32_t)a->v << CFX_QUAD_UV_SHIFT;
        edge->u_step = 0;
        edge->v_step = 0;
        return;
    }

    if (dy > 0) {
        edge->y_start = a->y;
        edge->y_end = b->y;
        edge->x = cfx_int_to_fixed(a->x);
        edge->u = (int32_t)a->u << CFX_QUAD_UV_SHIFT;
        edge->v = (int32_t)a->v << CFX_QUAD_UV_SHIFT;
        edge->x_step = cfx_div_toward_zero(cfx_int_to_fixed((int32_t)b->x - (int32_t)a->x), dy);
        edge->u_step = cfx_div_toward_zero(((int32_t)b->u - (int32_t)a->u) << CFX_QUAD_UV_SHIFT, dy);
        edge->v_step = cfx_div_toward_zero(((int32_t)b->v - (int32_t)a->v) << CFX_QUAD_UV_SHIFT, dy);
    } else {
        dy = (int16_t)-dy;
        edge->y_start = b->y;
        edge->y_end = a->y;
        edge->x = cfx_int_to_fixed(b->x);
        edge->u = (int32_t)b->u << CFX_QUAD_UV_SHIFT;
        edge->v = (int32_t)b->v << CFX_QUAD_UV_SHIFT;
        edge->x_step = cfx_div_toward_zero(cfx_int_to_fixed((int32_t)a->x - (int32_t)b->x), dy);
        edge->u_step = cfx_div_toward_zero(((int32_t)a->u - (int32_t)b->u) << CFX_QUAD_UV_SHIFT, dy);
        edge->v_step = cfx_div_toward_zero(((int32_t)a->v - (int32_t)b->v) << CFX_QUAD_UV_SHIFT, dy);
    }
}

static void cfx_draw_textured_quad_scanline(const CfxRenderer3DState *renderer,
                                            CfxVertexIn p0, CfxVertexIn p1,
                                            CfxVertexIn p2, CfxVertexIn p3)
{
    CfxVertexIn points[4] = { p0, p1, p2, p3 };
    CfxQuadEdge edges[4];
    int16_t min_y = points[0].y;
    int16_t max_y = points[0].y;

    for (int i = 1; i < 4; ++i) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }
    if (min_y == max_y) return;

    cfx_quad_build_edge(&edges[0], &points[0], &points[1]);
    cfx_quad_build_edge(&edges[1], &points[1], &points[2]);
    cfx_quad_build_edge(&edges[2], &points[2], &points[3]);
    cfx_quad_build_edge(&edges[3], &points[3], &points[0]);

    if (min_y < 0) {
        int16_t skip = (int16_t)-min_y;
        for (int i = 0; i < 4; ++i) {
            if (edges[i].y_start < 0 && edges[i].y_end > 0) {
                edges[i].x += edges[i].x_step * skip;
                edges[i].u += edges[i].u_step * skip;
                edges[i].v += edges[i].v_step * skip;
                edges[i].y_start = 0;
            }
        }
        min_y = 0;
    }
    if (max_y > renderer->height) max_y = (int16_t)renderer->height;

    for (int16_t y = min_y; y < max_y; ++y) {
        int16_t count = 0;
        int32_t xs_fp[2];
        int32_t us[2];
        int32_t vs[2];

        for (int i = 0; i < 4; ++i) {
            CfxQuadEdge *e = &edges[i];
            if (y >= e->y_start && y < e->y_end) {
                if (count < 2) {
                    xs_fp[count] = e->x;
                    us[count] = e->u;
                    vs[count] = e->v;
                    ++count;
                }
                e->x += e->x_step;
                e->u += e->u_step;
                e->v += e->v_step;
            }
        }

        if (count < 2) continue;
        if (xs_fp[0] > xs_fp[1]) {
            int32_t tx = xs_fp[0]; xs_fp[0] = xs_fp[1]; xs_fp[1] = tx;
            int32_t tu = us[0]; us[0] = us[1]; us[1] = tu;
            int32_t tv = vs[0]; vs[0] = vs[1]; vs[1] = tv;
        }

        int32_t dx_fp = xs_fp[1] - xs_fp[0];
        if (dx_fp <= 0) continue;
        int16_t x_start = (int16_t)((xs_fp[0] + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
        int16_t x_end = (int16_t)(xs_fp[1] >> CFX_GEOM_FIXED_SHIFT);
        int16_t span = (int16_t)(x_end - x_start + 1);
        if (span <= 0) continue;

        int32_t du_num_fp = (us[1] - us[0]) << CFX_GEOM_FIXED_SHIFT;
        int32_t dv_num_fp = (vs[1] - vs[0]) << CFX_GEOM_FIXED_SHIFT;
        int32_t du_fp = du_num_fp ? cfx_div_toward_zero(du_num_fp, dx_fp) : 0;
        int32_t dv_fp = dv_num_fp ? cfx_div_toward_zero(dv_num_fp, dx_fp) : 0;
        int32_t pixel_offset_fp = (((int32_t)x_start << CFX_GEOM_FIXED_SHIFT) + (1 << (CFX_GEOM_FIXED_SHIFT - 1))) - xs_fp[0];
        int32_t u_start = us[0] + ((du_fp * pixel_offset_fp) >> CFX_GEOM_FIXED_SHIFT);
        int32_t v_start = vs[0] + ((dv_fp * pixel_offset_fp) >> CFX_GEOM_FIXED_SHIFT);
        cfx_draw_span_quad_fp(renderer, y, x_start, span, u_start, v_start, du_fp, dv_fp);
    }
}

#endif

static inline uint16_t cfx_q8_to_q44(DEFAULT_INT q8)
{
    /* Source cube UVs are overwhelmingly exact tile endpoints.  Preserve the
       same endpoint mapping while avoiding V810 division in the common title
       cube path. */
    if (q8 <= 0) return 0;
    if (q8 >= (31 << 8)) return 255;
    int32_t scaled = cfx_div_toward_zero_i32d((int32_t)q8 * 255, (31 << 8));
    if (scaled < 0) scaled = 0;
    if (scaled > 255) scaled = 255;
    return (uint16_t)scaled;
}

static inline CfxVertexIn cfx_make_vertex(const Point2D *p)
{
    CfxVertexIn v;
    v.x = (int16_t)p->x;
    v.y = (int16_t)p->y;
    v.u = cfx_q8_to_q44(p->u);
    v.v = cfx_q8_to_q44(p->v);
    return v;
}

static inline CfxVertexIn cfx_make_vertex_endpoint(const Point2D *p)
{
    CfxVertexIn v;
    v.x = (int16_t)p->x;
    v.y = (int16_t)p->y;
    v.u = (p->u <= 0) ? 0u : 255u;
    v.v = (p->v <= 0) ? 0u : 255u;
    return v;
}

#if CFX_RENDERER_DIRECT_RECT
typedef struct {
    int16_t y_start;
    int16_t y_end;
    int32_t x;      /* 8.8 */
    int32_t u;      /* 8.8, but source coordinate uses upper byte */
    int32_t v;
    int32_t x_step;
    int32_t u_step;
    int32_t v_step;
} CfxFastQuadEdge;

static void cfx_fast_quad_build_edge(CfxFastQuadEdge *edge, const CfxVertexIn *a, const CfxVertexIn *b)
{
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_edge_setup();
#endif
    int16_t dy = (int16_t)(b->y - a->y);
    if (dy == 0) {
        edge->y_start = a->y;
        edge->y_end = a->y;
        edge->x = ((int32_t)a->x) << CFX_GEOM_FIXED_SHIFT;
        edge->u = ((int32_t)a->u) << 8;
        edge->v = ((int32_t)a->v) << 8;
        edge->x_step = edge->u_step = edge->v_step = 0;
        return;
    }

    if (dy > 0) {
        edge->y_start = a->y;
        edge->y_end = b->y;
        edge->x = ((int32_t)a->x) << CFX_GEOM_FIXED_SHIFT;
        edge->u = ((int32_t)a->u) << 8;
        edge->v = ((int32_t)a->v) << 8;
        edge->x_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)b->x - (int32_t)a->x) << CFX_GEOM_FIXED_SHIFT, (uint16_t)dy);
        edge->u_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)b->u - (int32_t)a->u) << 8, (uint16_t)dy);
        edge->v_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)b->v - (int32_t)a->v) << 8, (uint16_t)dy);
    } else {
        dy = (int16_t)-dy;
        edge->y_start = b->y;
        edge->y_end = a->y;
        edge->x = ((int32_t)b->x) << CFX_GEOM_FIXED_SHIFT;
        edge->u = ((int32_t)b->u) << 8;
        edge->v = ((int32_t)b->v) << 8;
        edge->x_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)a->x - (int32_t)b->x) << CFX_GEOM_FIXED_SHIFT, (uint16_t)dy);
        edge->u_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)a->u - (int32_t)b->u) << 8, (uint16_t)dy);
        edge->v_step = cfx_fast_div_tz_i32_u16_q15(((int32_t)a->v - (int32_t)b->v) << 8, (uint16_t)dy);
    }
}

static uint8_t cfx_draw_axis_rect_fast_affine_exact(
    const CfxRenderer3DState *state, const uint8_t *tile,
    const CfxVertexIn *v0, const CfxVertexIn *v1,
    const CfxVertexIn *v2, const CfxVertexIn *v3)
{
    const CfxVertexIn *tl;
    const CfxVertexIn *tr;
    const CfxVertexIn *bl;
    const CfxVertexIn *br;
    int16_t left, right, top, bottom;
    int16_t height;
    int16_t span;
    int16_t draw_top;
    int16_t draw_bottom;
    int32_t left_u_fp;
    int32_t left_v_fp;
    int32_t right_u_fp;
    int32_t right_v_fp;
    int32_t left_u_step;
    int32_t left_v_step;
    int32_t right_u_step;
    int32_t right_v_step;

    if (!cfx_get_axis_rect_corners(v0, v1, v2, v3,
                                   &tl, &tr, &bl, &left, &right, &top, &bottom)) {
        return 0;
    }
    br = cfx_find_rect_corner_common(v0, v1, v2, v3, right, bottom);
    if (!br) return 0;

    height = (int16_t)(bottom - top);
    span = (int16_t)(right - left + 1);
    if (height <= 0 || span <= 0) return 1;

    left_u_fp = (int32_t)tl->u << 8;
    left_v_fp = (int32_t)tl->v << 8;
    right_u_fp = (int32_t)tr->u << 8;
    right_v_fp = (int32_t)tr->v << 8;
    left_u_step = cfx_fast_div_tz_i32_u16_q15(
        ((int32_t)bl->u - (int32_t)tl->u) << 8, (uint16_t)height);
    left_v_step = cfx_fast_div_tz_i32_u16_q15(
        ((int32_t)bl->v - (int32_t)tl->v) << 8, (uint16_t)height);
    right_u_step = cfx_fast_div_tz_i32_u16_q15(
        ((int32_t)br->u - (int32_t)tr->u) << 8, (uint16_t)height);
    right_v_step = cfx_fast_div_tz_i32_u16_q15(
        ((int32_t)br->v - (int32_t)tr->v) << 8, (uint16_t)height);

    draw_top = top;
    draw_bottom = bottom;
    if (draw_top < 0) {
        int16_t skip = (int16_t)-draw_top;
        left_u_fp += left_u_step * skip;
        left_v_fp += left_v_step * skip;
        right_u_fp += right_u_step * skip;
        right_v_fp += right_v_step * skip;
        draw_top = 0;
    }
    if (draw_bottom > state->height) draw_bottom = state->height;

    for (int16_t y = draw_top; y < draw_bottom; ++y) {
        uint16_t denom = (uint16_t)((span > 256) ? 256 : span);
        int32_t du_fp = cfx_fast_div_tz_i32_u16_q15(right_u_fp - left_u_fp, denom);
        int32_t dv_fp = cfx_fast_div_tz_i32_u16_q15(right_v_fp - left_v_fp, denom);
        int8_t step_u = (int8_t)(du_fp >> 8);
        int8_t step_v = (int8_t)(dv_fp >> 8);
        uint16_t tex_state = cfx_pack_tex_state(
            (uint8_t)(left_u_fp >> 8), (uint8_t)(left_v_fp >> 8));
        cfx_draw_span_direct_tile(state, tile, y, left, span, tex_state, step_u, step_v);
        left_u_fp += left_u_step;
        left_v_fp += left_v_step;
        right_u_fp += right_u_step;
        right_v_fp += right_v_step;
    }
    return 1;
}

static void cfx_draw_board_span_flat(const CfxRenderer3DState *state,
                                     const uint8_t *tile, int16_t y,
                                     int16_t xs, int16_t span,
                                     uint16_t tex_state, int8_t step_u);
static void cfx_draw_board_span_tilted(const CfxRenderer3DState *state,
                                     const uint8_t *tile, int16_t y,
                                     int16_t xs, int16_t span,
                                     uint16_t tex_state, int8_t step_u,
                                     int8_t step_v);

/* A convex board cell has exactly two active boundary edges on every covered
   scanline.  The generic fallback below checks all four edges every row, which
   is needlessly expensive for the moving opening mesh.  Keep the original
   edge order and arithmetic, but carry the active pair from one row to the
   next; only rows at a vertex need the four-edge lookup. */
static uint8_t cfx_draw_textured_quad_fast_affine_active_edges(
    const CfxRenderer3DState *renderer, const uint8_t *tile,
    CfxFastQuadEdge *edges, int16_t min_y, int16_t max_y,
    uint8_t board_fast, uint8_t validate_boundaries)
{
    int active0 = -1;
    int active1 = -1;
    int active_count = 0;

    if (min_y == max_y) return 1;

    if (min_y < 0) {
        int16_t skip = (int16_t)-min_y;
        for (int i = 0; i < 4; ++i) {
            if (edges[i].y_start < 0 && edges[i].y_end > 0) {
                edges[i].x += edges[i].x_step * skip;
                edges[i].u += edges[i].u_step * skip;
                edges[i].v += edges[i].v_step * skip;
                edges[i].y_start = 0;
            }
        }
        min_y = 0;
    }
    if (max_y > renderer->height) max_y = (int16_t)renderer->height;
    if (min_y >= max_y) return 1;

    if (validate_boundaries) {
        int boundary_y[9];
        int boundary_count = 0;
        boundary_y[boundary_count++] = min_y;
        for (int i = 0; i < 4; ++i) {
            boundary_y[boundary_count++] = edges[i].y_start;
            boundary_y[boundary_count++] = edges[i].y_end;
        }
        /* An edge-active set can change only at one of these eight endpoints.
           Validate those few scanlines up front, so the optimized loop can
           never draw half a malformed polygon before falling back. */
        for (int b = 0; b < boundary_count; ++b) {
            int y_check = boundary_y[b];
            int count = 0;
            if (y_check < min_y || y_check >= max_y) continue;
            for (int i = 0; i < 4; ++i) {
                if (y_check >= edges[i].y_start && y_check < edges[i].y_end) ++count;
            }
            if (count != 2) return 0;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (min_y >= edges[i].y_start && min_y < edges[i].y_end) {
            if (active_count == 0) active0 = i;
            else if (active_count == 1) active1 = i;
            ++active_count;
        }
    }
    if (active_count != 2) return 0;

    for (int16_t y = min_y; y < max_y; ++y) {
        CfxFastQuadEdge *e0 = &edges[active0];
        CfxFastQuadEdge *e1 = &edges[active1];
        int32_t xs_fp[2] = {e0->x, e1->x};
        int32_t us_fp[2] = {e0->u, e1->u};
        int32_t vs_fp[2] = {e0->v, e1->v};

        e0->x += e0->x_step;
        e0->u += e0->u_step;
        e0->v += e0->v_step;
        e1->x += e1->x_step;
        e1->u += e1->u_step;
        e1->v += e1->v_step;

        if (xs_fp[0] > xs_fp[1]) {
            int32_t tx = xs_fp[0]; xs_fp[0] = xs_fp[1]; xs_fp[1] = tx;
            int32_t tu = us_fp[0]; us_fp[0] = us_fp[1]; us_fp[1] = tu;
            int32_t tv = vs_fp[0]; vs_fp[0] = vs_fp[1]; vs_fp[1] = tv;
        }

        int16_t x_start = (int16_t)((xs_fp[0] + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
        int16_t x_end = (int16_t)(xs_fp[1] >> CFX_GEOM_FIXED_SHIFT);
        int16_t span = (int16_t)(x_end - x_start + 1);
        if (span <= 0) continue;

        uint16_t denom = (uint16_t)((span > 256) ? 256 : span);
        int32_t du_fp = cfx_fast_div_tz_i32_u16_q15(us_fp[1] - us_fp[0], denom);
        int32_t dv_fp = cfx_fast_div_tz_i32_u16_q15(vs_fp[1] - vs_fp[0], denom);
        int8_t step_u = (int8_t)(du_fp >> 8);
        int8_t step_v = (int8_t)(dv_fp >> 8);
        uint16_t tex_state = cfx_pack_tex_state((uint8_t)(us_fp[0] >> 8), (uint8_t)(vs_fp[0] >> 8));
        if (board_fast) {
            cfx_draw_board_span_tilted(renderer, tile, y, x_start, span,
                                     tex_state, step_u, step_v);
        } else {
            cfx_draw_span_direct_tile(renderer, tile, y, x_start, span,
                                      tex_state, step_u, step_v);
        }

        if (y + 1 < max_y &&
            (y + 1 >= e0->y_end || y + 1 >= e1->y_end)) {
            active_count = 0;
            for (int i = 0; i < 4; ++i) {
                if (y + 1 >= edges[i].y_start && y + 1 < edges[i].y_end) {
                    if (active_count == 0) active0 = i;
                    else if (active_count == 1) active1 = i;
                    ++active_count;
                }
            }
            /* Board cells are convex, so this cannot happen for the intended
               mesh.  Refuse the specialized path before any future caller can
               silently get a partial polygon. */
            if (active_count != 2) return 0;
        }
    }
    return 1;
}

static uint8_t cfx_draw_textured_quad_fast_affine_edges(const CfxRenderer3DState *renderer,
                                                        const uint8_t *tile,
                                                        const CfxFastQuadEdge *source_edges,
                                                        int16_t min_y, int16_t max_y,
                                                        uint8_t board_fast)
{
    CfxFastQuadEdge edges[4];
    for (int i = 0; i < 4; ++i) edges[i] = source_edges[i];
    if (cfx_draw_textured_quad_fast_affine_active_edges(renderer, tile,
                                                        edges, min_y, max_y,
                                                        board_fast, 1)) {
        return 1;
    }

    {
        /* The validated walker may have advanced its private copy before
           rejecting a malformed polygon, so restart from the caller's
           original edges for the general fallback. */
        for (int i = 0; i < 4; ++i) edges[i] = source_edges[i];
        if (min_y == max_y) return 1;

        if (min_y < 0) {
            int16_t skip = (int16_t)-min_y;
            for (int i = 0; i < 4; ++i) {
                if (edges[i].y_start < 0 && edges[i].y_end > 0) {
                    edges[i].x += edges[i].x_step * skip;
                    edges[i].u += edges[i].u_step * skip;
                    edges[i].v += edges[i].v_step * skip;
                    edges[i].y_start = 0;
                }
            }
            min_y = 0;
        }
        if (max_y > renderer->height) max_y = (int16_t)renderer->height;

        for (int16_t y = min_y; y < max_y; ++y) {
            int16_t count = 0;
            int32_t xs_fp[2];
            int32_t us_fp[2];
            int32_t vs_fp[2];

            for (int i = 0; i < 4; ++i) {
                CfxFastQuadEdge *e = &edges[i];
                if (y >= e->y_start && y < e->y_end) {
                    if (count < 2) {
                        xs_fp[count] = e->x;
                        us_fp[count] = e->u;
                        vs_fp[count] = e->v;
                        ++count;
                    }
                    e->x += e->x_step;
                    e->u += e->u_step;
                    e->v += e->v_step;
                }
            }
            if (count < 2) continue;
            if (xs_fp[0] > xs_fp[1]) {
                int32_t tx = xs_fp[0]; xs_fp[0] = xs_fp[1]; xs_fp[1] = tx;
                int32_t tu = us_fp[0]; us_fp[0] = us_fp[1]; us_fp[1] = tu;
                int32_t tv = vs_fp[0]; vs_fp[0] = vs_fp[1]; vs_fp[1] = tv;
            }

            int16_t x_start = (int16_t)((xs_fp[0] + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
            int16_t x_end = (int16_t)(xs_fp[1] >> CFX_GEOM_FIXED_SHIFT);
            int16_t span = (int16_t)(x_end - x_start + 1);
            if (span <= 0) continue;

            uint16_t denom = (uint16_t)((span > 256) ? 256 : span);
            int32_t du_fp = cfx_fast_div_tz_i32_u16_q15(us_fp[1] - us_fp[0], denom);
            int32_t dv_fp = cfx_fast_div_tz_i32_u16_q15(vs_fp[1] - vs_fp[0], denom);
            int8_t step_u = (int8_t)(du_fp >> 8);
            int8_t step_v = (int8_t)(dv_fp >> 8);
            uint16_t tex_state = cfx_pack_tex_state((uint8_t)(us_fp[0] >> 8), (uint8_t)(vs_fp[0] >> 8));
            if (board_fast) {
                cfx_draw_board_span_tilted(renderer, tile, y, x_start, span,
                                         tex_state, step_u, step_v);
            } else {
                cfx_draw_span_direct_tile(renderer, tile, y, x_start, span,
                                          tex_state, step_u, step_v);
            }
        }
    }
    return 1;
}

/* The board-mesh caller can prove that all of its cells are strict convex
   before entering the cached walker.  That proof makes the endpoint scan
   above redundant for every cell, while the public quad path retains the
   validated behavior for arbitrary polygons. */
static uint8_t cfx_draw_textured_quad_fast_affine_trusted_edges(
    const CfxRenderer3DState *renderer, const uint8_t *tile,
    CfxFastQuadEdge *edges, int16_t min_y, int16_t max_y,
    uint8_t board_fast)
{
    return cfx_draw_textured_quad_fast_affine_active_edges(renderer, tile,
                                                            edges,
                                                            min_y, max_y,
                                                            board_fast, 0);
}

static uint8_t cfx_draw_textured_quad_fast_affine_direct(const CfxRenderer3DState *renderer,
                                                         const uint8_t *tile,
                                                         CfxVertexIn p0, CfxVertexIn p1,
                                                         CfxVertexIn p2, CfxVertexIn p3)
{
    CfxVertexIn points[4] = { p0, p1, p2, p3 };
    CfxFastQuadEdge edges[4];
    int16_t min_y = points[0].y;
    int16_t max_y = points[0].y;

    for (int i = 1; i < 4; ++i) {
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }
    if (min_y == max_y) return 1;

    cfx_fast_quad_build_edge(&edges[0], &points[0], &points[1]);
    cfx_fast_quad_build_edge(&edges[1], &points[1], &points[2]);
    cfx_fast_quad_build_edge(&edges[2], &points[2], &points[3]);
    cfx_fast_quad_build_edge(&edges[3], &points[3], &points[0]);
    return cfx_draw_textured_quad_fast_affine_edges(renderer, tile, edges,
                                                    min_y, max_y, 0);
}

uint8_t cfx_renderer3d_draw_quad_fast_affine(CfxRenderer3D *renderer,
                                             const Point2D *p0, const Point2D *p1,
                                             const Point2D *p2, const Point2D *p3,
                                             DEFAULT_INT tetromino_type)
{
    CfxRenderer3DState *state = cfx_state(renderer);
    if (!state->framebuffer || !state->texture_atlas) return 0;
    if (tetromino_type < 0) tetromino_type = 0;
    if (tetromino_type >= CFX_TEXTURE_TILE_COUNT) tetromino_type = CFX_TEXTURE_TILE_COUNT - 1;
    const uint8_t *tile = state->texture_atlas + ((int32_t)tetromino_type * state->tile_stride_bytes);
    CfxVertexIn v0 = cfx_make_vertex_endpoint(p0);
    CfxVertexIn v1 = cfx_make_vertex_endpoint(p1);
    CfxVertexIn v2 = cfx_make_vertex_endpoint(p2);
    CfxVertexIn v3 = cfx_make_vertex_endpoint(p3);
    if (cfx_draw_axis_rect_fast_affine_exact(state, tile, &v0, &v1, &v2, &v3)) {
        return 1;
    }
    return cfx_draw_textured_quad_fast_affine_direct(state, tile, v0, v1, v2, v3);
}

typedef struct {
    int16_t y_start;
    int16_t y_end;
    int32_t x;
    int32_t x_step;
} CfxBoardGeomEdge;

static void cfx_board_build_geom_edge(CfxBoardGeomEdge *edge,
                                      const CfxBoardPoint *a,
                                      const CfxBoardPoint *b)
{
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_edge_setup();
#endif
    int16_t dy = (int16_t)(b->y - a->y);
    if (dy == 0) {
        edge->y_start = a->y;
        edge->y_end = a->y;
        edge->x = ((int32_t)a->x) << CFX_GEOM_FIXED_SHIFT;
        edge->x_step = 0;
    } else if (dy > 0) {
        edge->y_start = a->y;
        edge->y_end = b->y;
        edge->x = ((int32_t)a->x) << CFX_GEOM_FIXED_SHIFT;
        edge->x_step = cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)b->x - (int32_t)a->x) << CFX_GEOM_FIXED_SHIFT,
            (uint16_t)dy);
    } else {
        dy = (int16_t)-dy;
        edge->y_start = b->y;
        edge->y_end = a->y;
        edge->x = ((int32_t)b->x) << CFX_GEOM_FIXED_SHIFT;
        edge->x_step = cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)a->x - (int32_t)b->x) << CFX_GEOM_FIXED_SHIFT,
            (uint16_t)dy);
    }
}

static void cfx_board_set_edge_uv(CfxFastQuadEdge *edge,
                                  const CfxBoardGeomEdge *geom,
                                  const CfxBoardPoint *a,
                                  const CfxBoardPoint *b,
                                  uint8_t au, uint8_t av,
                                  uint8_t bu, uint8_t bv)
{
    int16_t dy = (int16_t)(b->y - a->y);
    edge->y_start = geom->y_start;
    edge->y_end = geom->y_end;
    edge->x = geom->x;
    edge->x_step = geom->x_step;
    if (dy == 0) {
        edge->u = (int32_t)au << 8;
        edge->v = (int32_t)av << 8;
        edge->u_step = 0;
        edge->v_step = 0;
    } else if (dy > 0) {
        edge->u = (int32_t)au << 8;
        edge->v = (int32_t)av << 8;
        edge->u_step = (au == bu) ? 0 : cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)bu - (int32_t)au) << 8, (uint16_t)dy);
        edge->v_step = (av == bv) ? 0 : cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)bv - (int32_t)av) << 8, (uint16_t)dy);
    } else {
        dy = (int16_t)-dy;
        edge->u = (int32_t)bu << 8;
        edge->v = (int32_t)bv << 8;
        edge->u_step = (au == bu) ? 0 : cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)au - (int32_t)bu) << 8, (uint16_t)dy);
        edge->v_step = (av == bv) ? 0 : cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)av - (int32_t)bv) << 8, (uint16_t)dy);
    }
}

static void cfx_board_vertical_v_params(const CfxBoardPoint *a,
                                        const CfxBoardPoint *b,
                                        uint8_t *v_start,
                                        int32_t *v_step)
{
    int16_t dy = (int16_t)(b->y - a->y);
    if (dy == 0) {
        *v_start = 0;
        *v_step = 0;
    } else if (dy > 0) {
        *v_start = 0;
        *v_step = cfx_fast_div_tz_i32_u16_q15(255 << 8, (uint16_t)dy);
    } else {
        dy = (int16_t)-dy;
        *v_start = 255;
        *v_step = cfx_fast_div_tz_i32_u16_q15(-(255 << 8), (uint16_t)dy);
    }
}

static inline void cfx_board_set_precomputed_uv(
    CfxFastQuadEdge *edge, const CfxBoardGeomEdge *geom,
    uint8_t u_start, uint8_t v_start,
    int32_t u_step, int32_t v_step)
{
    edge->y_start = geom->y_start;
    edge->y_end = geom->y_end;
    edge->x = geom->x;
    edge->x_step = geom->x_step;
    edge->u = (int32_t)u_start << 8;
    edge->v = (int32_t)v_start << 8;
    edge->u_step = u_step;
    edge->v_step = v_step;
}

static int cfx_board_axis_cell(const CfxBoardPoint *p0,
                               const CfxBoardPoint *p1,
                               const CfxBoardPoint *p2,
                               const CfxBoardPoint *p3)
{
    return p0->y == p1->y && p1->x == p2->x &&
           p2->y == p3->y && p3->x == p0->x;
}

static int cfx_board_turn_sign(const CfxBoardPoint *a,
                               const CfxBoardPoint *b,
                               const CfxBoardPoint *c)
{
    int32_t ab_x = (int32_t)b->x - a->x;
    int32_t ab_y = (int32_t)b->y - a->y;
    int32_t bc_x = (int32_t)c->x - b->x;
    int32_t bc_y = (int32_t)c->y - b->y;
    int32_t cross = ab_x * bc_y - ab_y * bc_x;
    return (cross < 0) ? -1 : (cross > 0) ? 1 : 0;
}

static int cfx_board_mesh_strictly_convex(const CfxBoardPoint *points,
                                          DEFAULT_INT point_stride,
                                          DEFAULT_INT rows, DEFAULT_INT cols,
                                          DEFAULT_INT width, DEFAULT_INT height)
{
    /* Main's board_mesh_point_from_screen() clamps to this range.  Keeping
       the check here makes the fast proof safe for other mesh callers too;
       once coordinates are screen-bounded, every cross product fits int32. */
    for (DEFAULT_INT r = 0; r <= rows; ++r) {
        int32_t row = (int32_t)r * point_stride;
        for (DEFAULT_INT c = 0; c <= cols; ++c) {
            const CfxBoardPoint *p = &points[row + c];
            if (p->x < 0 || p->x >= width || p->y < 0 || p->y >= height)
                return 0;
        }
    }

    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            const CfxBoardPoint *p0 = &points[row0 + c];
            const CfxBoardPoint *p1 = &points[row0 + c + 1];
            const CfxBoardPoint *p2 = &points[row1 + c + 1];
            const CfxBoardPoint *p3 = &points[row1 + c];
            int sign = cfx_board_turn_sign(p0, p1, p2);
            if (sign == 0 || cfx_board_turn_sign(p1, p2, p3) != sign ||
                cfx_board_turn_sign(p2, p3, p0) != sign ||
                cfx_board_turn_sign(p3, p0, p1) != sign) {
                return 0;
            }
        }
    }
    return 1;
}

static DEFAULT_INT cfx_board_clamp_tile(DEFAULT_INT tile)
{
    if (tile < 0) return 0;
    if (tile >= CFX_TEXTURE_TILE_COUNT) return CFX_TEXTURE_TILE_COUNT - 1;
    return tile;
}

static uint8_t cfx_draw_board_mesh_axis(const CfxRenderer3DState *state,
                                        const CfxBoardPoint *points,
                                        DEFAULT_INT point_stride,
                                        DEFAULT_INT rows, DEFAULT_INT cols,
                                        DEFAULT_INT even_tile,
                                        DEFAULT_INT odd_tile)
{
    const DEFAULT_INT even = cfx_board_clamp_tile(even_tile);
    const DEFAULT_INT odd = cfx_board_clamp_tile(odd_tile);
    const uint8_t *even_src = state->texture_atlas + ((int32_t)even * state->tile_stride_bytes);
    const uint8_t *odd_src = state->texture_atlas + ((int32_t)odd * state->tile_stride_bytes);
    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            const CfxBoardPoint *p0 = &points[row0 + c];
            const CfxBoardPoint *p1 = &points[row0 + c + 1];
            const CfxBoardPoint *p3 = &points[row1 + c];
            int16_t left = p0->x < p1->x ? p0->x : p1->x;
            int16_t right = p0->x > p1->x ? p0->x : p1->x;
            int16_t top = p0->y < p3->y ? p0->y : p3->y;
            int16_t bottom = p0->y > p3->y ? p0->y : p3->y;
            int16_t height;
            int16_t span;
            int16_t draw_top;
            int16_t draw_bottom;
            int32_t left_u_fp;
            int32_t left_v_fp;
            int32_t right_u_fp;
            int32_t right_v_fp;
            int32_t left_u_step;
            int32_t left_v_step;
            int32_t right_u_step;
            int32_t right_v_step;
            uint16_t denom;
            int8_t step_u;
            int8_t step_v;
            const uint8_t *tile;
            uint8_t left_u = (p0->x <= p1->x) ? 0u : 255u;
            uint8_t top_v = (p0->y <= p3->y) ? 0u : 255u;
            uint8_t right_u = (uint8_t)(255u - left_u);
            uint8_t bottom_v = (uint8_t)(255u - top_v);

            height = (int16_t)(bottom - top);
            span = (int16_t)(right - left + 1);
            if (height <= 0 || span <= 0) continue;
            left_u_fp = (int32_t)left_u << 8;
            left_v_fp = (int32_t)top_v << 8;
            right_u_fp = (int32_t)right_u << 8;
            right_v_fp = (int32_t)top_v << 8;
            left_u_step = 0;
            left_v_step = cfx_fast_div_tz_i32_u16_q15(
                ((int32_t)bottom_v - (int32_t)top_v) << 8,
                (uint16_t)height);
            right_u_step = left_u_step;
            right_v_step = left_v_step;

            draw_top = top;
            draw_bottom = bottom;
            if (draw_top < 0) {
                int16_t skip = (int16_t)-draw_top;
                left_u_fp += left_u_step * skip;
                left_v_fp += left_v_step * skip;
                right_u_fp += right_u_step * skip;
                right_v_fp += right_v_step * skip;
                draw_top = 0;
            }
            if (draw_bottom > state->height) draw_bottom = (int16_t)state->height;
            denom = (uint16_t)((span > 256) ? 256 : span);
            step_u = (int8_t)(cfx_fast_div_tz_i32_u16_q15(
                right_u_fp - left_u_fp, denom) >> 8);
            step_v = (int8_t)(cfx_fast_div_tz_i32_u16_q15(
                right_v_fp - left_v_fp, denom) >> 8);
            tile = ((r + c) & 1) ? odd_src : even_src;
            for (int16_t y = draw_top; y < draw_bottom; ++y) {
        cfx_draw_span_direct_tile(state, tile, y, left, span,
            cfx_pack_tex_state((uint8_t)(left_u_fp >> 8),
                               (uint8_t)(left_v_fp >> 8)),
            step_u, step_v);
                left_u_fp += left_u_step;
                left_v_fp += left_v_step;
                right_u_fp += right_u_step;
                right_v_fp += right_v_step;
            }
        }
    }
    return 1;
}

#define CFX_BOARD_MESH_MAX_COLS 64

#if defined(__GNUC__)
#define CFX_BOARD_MESH_COLD __attribute__((noinline, cold))
#else
#define CFX_BOARD_MESH_COLD
#endif

static int cfx_board_rows_uniform_trapezoid(const CfxBoardPoint *points,
                                            DEFAULT_INT point_stride,
                                            DEFAULT_INT rows, DEFAULT_INT cols)
{
    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        int16_t top_y = points[row0].y;
        int16_t bottom_y = points[row1].y;
        for (DEFAULT_INT c = 1; c <= cols; ++c) {
            if (points[row0 + c].y != top_y || points[row1 + c].y != bottom_y)
                return 0;
        }
    }
    return 1;
}

/* The Marty board's ground cells use a 32x32 tile and constant V on each
   scanline.  Feed that row directly to the backend's packed i386 board-fill
   loop: it emits four pixels per store and avoids the divide/run bookkeeping
   that used to dominate this otherwise simple constant-V case.  The incoming
   coordinates are 8-bit texture positions (0..255); cfx_board_fill's Q8
   interface therefore receives them multiplied by 32. */
static void cfx_draw_board_span_flat(const CfxRenderer3DState *state,
                                     const uint8_t *tile, int16_t y,
                                     int16_t xs, int16_t span,
                                     uint16_t tex_state, int8_t step_u)
{
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_board_span(state, 0);
#endif
#if defined(WAIFU_FM_FMTOWNS) && defined(__i386__) && (CFX_TEX_SIZE == 32) && \
    !defined(CFX_MEASURE_SKIP_SPANS) && !defined(CFX_MEASURE_C_ROW)
    if (span <= 0 || y < 0 || y >= state->height) return;
    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) return;
        tex_state = cfx_pack_tex_state(
            (uint8_t)((uint8_t)tex_state + (int)step_u * skip),
            (uint8_t)(tex_state >> 8));
        span = (int16_t)(span - skip);
        xs = 0;
    }
    if (xs >= state->width) return;
    if ((int32_t)xs + span > state->width)
        span = (int16_t)(state->width - xs);
    if (span <= 0) return;
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_emit_span(state, span);
#endif
#if CFX_RENDERER_DIRECT_FLAT_ROW
    if (step_u == 5 && (uint8_t)tex_state == 0u && span <= 48)
        cfx_draw_span_direct_tile_flat_step5_pattern(state, tile, y, xs, span,
                                                     tex_state);
    else
        cfx_draw_span_direct_tile_flat_clipped(state, tile, y, xs, span,
                                               tex_state, step_u);
#else
    cfx_board_fill(state->framebuffer + ((int32_t)y * state->width) + xs,
                   span,
                   (int32_t)(uint8_t)tex_state << 5,
                   (int32_t)(uint8_t)(tex_state >> 8) << 5,
                   (int32_t)step_u << 5, 0, tile);
#endif
#else
    cfx_draw_span_direct_tile(state, tile, y, xs, span, tex_state, step_u, 0);
#endif
}

static void cfx_draw_board_span_tilted(const CfxRenderer3DState *state,
                                     const uint8_t *tile, int16_t y,
                                     int16_t xs, int16_t span,
                                     uint16_t tex_state, int8_t step_u,
                                     int8_t step_v)
{
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_board_span(state, 1);
#endif
#if defined(WAIFU_FM_FMTOWNS) && defined(__i386__) && (CFX_TEX_SIZE == 32) && \
    !defined(CFX_MEASURE_SKIP_SPANS) && !defined(CFX_MEASURE_C_ROW)
    /* Tilted board spans average only 1.25--1.34 pixels per texture-boundary
       run in the measured turn poses.  The run splitter therefore spends more
       time finding boundaries than it saves in repeated-color stores.  Reuse
       the exact packed-accumulator affine filler: it preserves the same
       uint8_t UV wrapping and 4-pixel stores, but advances one pixel without
       the per-run lookup/branch machinery. */
    cfx_draw_span_direct_tile(state, tile, y, xs, span, tex_state, step_u, step_v);
#else
    cfx_draw_span_direct_tile(state, tile, y, xs, span, tex_state, step_u, step_v);
#endif
}

/* The projected ground plane keeps every screen row of a board mesh on one
   horizontal scanline boundary.  In that common case the old per-cell loop
   copied and advanced each shared column edge once for its left cell and once
   for its right cell.  Walk one row at a time instead: each column edge is
   stepped once, while every cell retains its own U endpoint and tile. */
static uint8_t CFX_BOARD_MESH_COLD cfx_draw_board_mesh_trapezoid_rows(
    const CfxRenderer3DState *state, const CfxBoardPoint *points,
    DEFAULT_INT point_stride, DEFAULT_INT rows, DEFAULT_INT cols,
    DEFAULT_INT even_tile, DEFAULT_INT odd_tile)
{
    CfxBoardGeomEdge vertical[CFX_BOARD_MESH_MAX_COLS + 1];
    const DEFAULT_INT even = cfx_board_clamp_tile(even_tile);
    const DEFAULT_INT odd = cfx_board_clamp_tile(odd_tile);
    const uint8_t *even_src = state->texture_atlas + ((int32_t)even * state->tile_stride_bytes);
    const uint8_t *odd_src = state->texture_atlas + ((int32_t)odd * state->tile_stride_bytes);

    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        int16_t top = points[row0].y;
        int16_t bottom = points[row1].y;
        int16_t min_y = top < bottom ? top : bottom;
        int16_t max_y = top > bottom ? top : bottom;
        int16_t draw_top = min_y;
        int16_t draw_bottom = max_y;
        int16_t height = (int16_t)(max_y - min_y);
        int32_t v_fp;
        int32_t v_step;
        uint8_t top_v;
        uint8_t bottom_v;

        if (height <= 0) continue;
        for (DEFAULT_INT c = 0; c <= cols; ++c)
            cfx_board_build_geom_edge(&vertical[c], &points[row0 + c], &points[row1 + c]);

        top_v = (top <= bottom) ? 0u : 255u;
        bottom_v = (uint8_t)(255u - top_v);
        v_fp = (int32_t)top_v << 8;
        v_step = cfx_fast_div_tz_i32_u16_q15(
            ((int32_t)bottom_v - (int32_t)top_v) << 8, (uint16_t)height);

        if (draw_top < 0) {
            int16_t skip = (int16_t)-draw_top;
            for (DEFAULT_INT c = 0; c <= cols; ++c)
                vertical[c].x += vertical[c].x_step * skip;
            v_fp += v_step * skip;
            draw_top = 0;
        }
        if (draw_bottom > state->height) draw_bottom = (int16_t)state->height;

        for (int16_t y = draw_top; y < draw_bottom; ++y) {
            for (DEFAULT_INT c = 0; c < cols; ++c) {
                const CfxBoardPoint *p0 = &points[row0 + c];
                const CfxBoardPoint *p1 = &points[row0 + c + 1];
                int32_t xs0 = vertical[c + 1].x;
                int32_t xs1 = vertical[c].x;
                int32_t us0 = (int32_t)((p0->x <= p1->x) ? 255u : 0u) << 8;
                int32_t us1 = (int32_t)((p0->x <= p1->x) ? 0u : 255u) << 8;
                int16_t x_start;
                int16_t x_end;
                int16_t span;
                uint16_t denom;
                int8_t step_u;

                if (xs0 > xs1) {
                    int32_t t = xs0;
                    xs0 = xs1;
                    xs1 = t;
                    t = us0;
                    us0 = us1;
                    us1 = t;
                }
                x_start = (int16_t)((xs0 + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
                x_end = (int16_t)(xs1 >> CFX_GEOM_FIXED_SHIFT);
                span = (int16_t)(x_end - x_start + 1);
                if (span <= 0) continue;

                denom = (uint16_t)((span > 256) ? 256 : span);
                step_u = cfx_board_step_255(us1 > us0, denom);
                cfx_draw_board_span_flat(state,
                    ((r + c) & 1) ? odd_src : even_src, y, x_start, span,
                    cfx_pack_tex_state((uint8_t)(us0 >> 8), (uint8_t)(v_fp >> 8)),
                    step_u);
            }
            for (DEFAULT_INT c = 0; c <= cols; ++c)
                vertical[c].x += vertical[c].x_step;
            v_fp += v_step;
        }
    }
    return 1;
}

static uint8_t cfx_draw_board_mesh_trapezoid(const CfxRenderer3DState *state,
                                             const CfxBoardPoint *points,
                                             DEFAULT_INT point_stride,
                                             DEFAULT_INT rows, DEFAULT_INT cols,
                                             DEFAULT_INT even_tile,
                                             DEFAULT_INT odd_tile)
{
    CfxBoardGeomEdge vertical[CFX_BOARD_MESH_MAX_COLS + 1];
    const DEFAULT_INT even = cfx_board_clamp_tile(even_tile);
    const DEFAULT_INT odd = cfx_board_clamp_tile(odd_tile);
    const uint8_t *even_src = state->texture_atlas + ((int32_t)even * state->tile_stride_bytes);
    const uint8_t *odd_src = state->texture_atlas + ((int32_t)odd * state->tile_stride_bytes);

    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c <= cols; ++c) {
            cfx_board_build_geom_edge(&vertical[c], &points[row0 + c], &points[row1 + c]);
        }

        for (DEFAULT_INT c = 0; c < cols; ++c) {
            const CfxBoardPoint *p0 = &points[row0 + c];
            const CfxBoardPoint *p1 = &points[row0 + c + 1];
            const CfxBoardPoint *p2 = &points[row1 + c + 1];
            const CfxBoardPoint *p3 = &points[row1 + c];
            CfxFastQuadEdge left;
            CfxFastQuadEdge right;
            int16_t min_y = p0->y;
            int16_t max_y = p0->y;
            int16_t draw_top;
            int16_t draw_bottom;
            const uint8_t *tile = ((r + c) & 1) ? odd_src : even_src;

            if (p1->y < min_y) min_y = p1->y;
            if (p2->y < min_y) min_y = p2->y;
            if (p3->y < min_y) min_y = p3->y;
            if (p1->y > max_y) max_y = p1->y;
            if (p2->y > max_y) max_y = p2->y;
            if (p3->y > max_y) max_y = p3->y;
            if (min_y == max_y) continue;

            /* The old quad walker sees the right edge before the left edge.
               Keep that order, including the endpoint UVs, so its x-order
               swap and all subsequent truncation remain unchanged. */
            cfx_board_set_edge_uv(&right, &vertical[c + 1], p1, p2,
                                  255, 0, 255, 255);
            cfx_board_set_edge_uv(&left, &vertical[c], p3, p0,
                                  0, 255, 0, 0);

            draw_top = min_y;
            draw_bottom = max_y;
            if (draw_top < 0) {
                int16_t skip = (int16_t)-draw_top;
                if (right.y_start < 0 && right.y_end > 0) {
                    right.x += right.x_step * skip;
                    right.u += right.u_step * skip;
                    right.v += right.v_step * skip;
                    right.y_start = 0;
                }
                if (left.y_start < 0 && left.y_end > 0) {
                    left.x += left.x_step * skip;
                    left.u += left.u_step * skip;
                    left.v += left.v_step * skip;
                    left.y_start = 0;
                }
                draw_top = 0;
            }
            if (draw_bottom > state->height) draw_bottom = (int16_t)state->height;

            for (int16_t y = draw_top; y < draw_bottom; ++y) {
                int32_t xs0 = right.x;
                int32_t xs1 = left.x;
                int32_t us0 = right.u;
                int32_t us1 = left.u;
                int32_t vs0 = right.v;
                int32_t vs1 = left.v;
                int16_t x_start;
                int16_t x_end;
                int16_t span;
                uint16_t denom;
                int8_t step_u;

                right.x += right.x_step;
                right.u += right.u_step;
                right.v += right.v_step;
                left.x += left.x_step;
                left.u += left.u_step;
                left.v += left.v_step;

                if (xs0 > xs1) {
                    int32_t t;
                    t = xs0; xs0 = xs1; xs1 = t;
                    t = us0; us0 = us1; us1 = t;
                    t = vs0; vs0 = vs1; vs1 = t;
                }
                x_start = (int16_t)((xs0 + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
                x_end = (int16_t)(xs1 >> CFX_GEOM_FIXED_SHIFT);
                span = (int16_t)(x_end - x_start + 1);
                if (span <= 0) continue;

                denom = (uint16_t)((span > 256) ? 256 : span);
                /* The two horizontal boundaries have equal y endpoints, so
                   both vertical edges carry the same V at every scanline.
                   U is always an endpoint pair (0,255); spell out the exact
                   truncation of (delta << 8) / denom, including its negative
                   arithmetic-shift case, instead of dividing on every row. */
                step_u = (us1 == us0) ? 0 : cfx_board_step_255(us1 > us0, denom);
                cfx_draw_board_span_flat(state, tile, y, x_start, span,
                    cfx_pack_tex_state((uint8_t)(us0 >> 8),
                                       (uint8_t)(vs0 >> 8)),
                    step_u);
            }
        }
    }
    return 1;
}

#if CFX_RENDERER_SPECIALIZED_BOARD_GRID
/* FM TOWNS only: the in-game board is a fixed 5x4 grid.  The generic moving
   mesh path rebuilds horizontal edge metadata for every cell and then walks
   each cell independently.  That repeats the same projected grid edge for
   its neighbour and makes the scanline loop pay a call/branch transition at
   every cell.  Keep one mutable state per unique edge and visit cells in the
   same row-major order as the reference renderer, but make the scanline the
   outer loop.  The edge state is advanced lazily, so shared edges advance once
   even when two adjacent cells use them.

   This is deliberately bounded to the shipped board dimensions.  It does not
   become a general mesh cache or a moving-camera cache: all edge state is
   rebuilt from the caller's current projected points for every invocation. */
#define CFX_SPECIAL_BOARD_ROWS 4
#define CFX_SPECIAL_BOARD_COLS 5
#define CFX_SPECIAL_BOARD_VERTICAL_COUNT \
    (CFX_SPECIAL_BOARD_ROWS * (CFX_SPECIAL_BOARD_COLS + 1))
#define CFX_SPECIAL_BOARD_HORIZONTAL_COUNT \
    ((CFX_SPECIAL_BOARD_ROWS + 1) * CFX_SPECIAL_BOARD_COLS)
#define CFX_SPECIAL_BOARD_EDGE_COUNT \
    (CFX_SPECIAL_BOARD_VERTICAL_COUNT + CFX_SPECIAL_BOARD_HORIZONTAL_COUNT)

typedef struct {
    int16_t y_start;
    int16_t y_end;
    int32_t x;
    int32_t x_step;
    /* Horizontal edges use tex/tex_step for U; vertical edges use it for V. */
    int32_t tex;
    int32_t tex_step;
    int16_t last_y;
    int32_t base_x;
    int32_t base_tex;
} CfxBoardGridEdge;

typedef struct {
    uint8_t edge[4]; /* top, right, bottom, left */
    int16_t min_y;
    int16_t max_y;
    int8_t active0;
    int8_t active1;
} CfxBoardGridCell;

static void cfx_board_grid_edge_from_geom(CfxBoardGridEdge *dst,
                                           const CfxBoardGeomEdge *geom,
                                           int32_t tex, int32_t tex_step)
{
    dst->y_start = geom->y_start;
    dst->y_end = geom->y_end;
    dst->x = geom->x;
    dst->x_step = geom->x_step;
    dst->tex = tex;
    dst->tex_step = tex_step;
    dst->last_y = geom->y_start;
    dst->base_x = geom->x;
    dst->base_tex = tex;
}

static void cfx_board_grid_reset(CfxBoardGridEdge *edge)
{
    edge->x = edge->base_x;
    edge->tex = edge->base_tex;
    edge->last_y = edge->y_start;
}

static void cfx_board_grid_advance(CfxBoardGridEdge *edge, int16_t y)
{
    if (edge->last_y < y) {
        int32_t count = (int32_t)y - edge->last_y;
        edge->x += edge->x_step * count;
        edge->tex += edge->tex_step * count;
        edge->last_y = y;
    }
}

static int cfx_board_grid_select_active(const CfxBoardGridCell *cell,
                                        const CfxBoardGridEdge *edges,
                                        int16_t y,
                                        int8_t *active0, int8_t *active1)
{
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        const CfxBoardGridEdge *edge = &edges[cell->edge[i]];
        if (y >= edge->y_start && y < edge->y_end) {
            if (count == 0) *active0 = (int8_t)i;
            else if (count == 1) *active1 = (int8_t)i;
            ++count;
        }
    }
    return count == 2;
}

static void cfx_board_grid_values(CfxBoardGridEdge *edge, int local_edge,
                                  int16_t y, int32_t *x, int32_t *u,
                                  int32_t *v)
{
    cfx_board_grid_advance(edge, y);
    *x = edge->x;
    if (local_edge == 0) {
        *u = edge->tex;
        *v = 0;
    } else if (local_edge == 1) {
        *u = 255 << 8;
        *v = edge->tex;
    } else if (local_edge == 2) {
        *u = edge->tex;
        *v = 255 << 8;
    } else {
        *u = 0;
        *v = edge->tex;
    }
}

static uint8_t cfx_draw_board_mesh_specialized_grid(
    const CfxRenderer3DState *state, const CfxBoardPoint *points,
    DEFAULT_INT point_stride, DEFAULT_INT rows, DEFAULT_INT cols,
    DEFAULT_INT even_tile, DEFAULT_INT odd_tile)
{
    CfxBoardGridEdge edges[CFX_SPECIAL_BOARD_EDGE_COUNT];
    CfxBoardGridCell cells[CFX_SPECIAL_BOARD_ROWS * CFX_SPECIAL_BOARD_COLS];
    const DEFAULT_INT even = cfx_board_clamp_tile(even_tile);
    const DEFAULT_INT odd = cfx_board_clamp_tile(odd_tile);
    const uint8_t *even_src = state->texture_atlas + ((int32_t)even * state->tile_stride_bytes);
    const uint8_t *odd_src = state->texture_atlas + ((int32_t)odd * state->tile_stride_bytes);

    if (rows != CFX_SPECIAL_BOARD_ROWS || cols != CFX_SPECIAL_BOARD_COLS)
        return 0;

    /* Build each vertical edge once.  Its texture component is V; U is a
       cell-side constant supplied by cfx_board_grid_values(). */
    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c <= cols; ++c) {
            CfxBoardGeomEdge geom;
            uint8_t v_start;
            int32_t v_step;
            cfx_board_build_geom_edge(&geom, &points[row0 + c], &points[row1 + c]);
            cfx_board_vertical_v_params(&points[row0 + c], &points[row1 + c],
                                        &v_start, &v_step);
            cfx_board_grid_edge_from_geom(
                &edges[r * (cols + 1) + c], &geom,
                (int32_t)v_start << 8, v_step);
        }
    }

    /* Horizontal edges are shared by the cells above and below.  The physical
       mapping is U=0..255 from left to right; the two callers differ only in
       their constant V (0 for top, 255 for bottom). */
    for (DEFAULT_INT r = 0; r <= rows; ++r) {
        int32_t row = (int32_t)r * point_stride;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            CfxBoardGeomEdge geom;
            CfxFastQuadEdge uv;
            cfx_board_build_geom_edge(&geom, &points[row + c], &points[row + c + 1]);
            cfx_board_set_edge_uv(&uv, &geom, &points[row + c], &points[row + c + 1],
                                  0, 0, 255, 0);
            cfx_board_grid_edge_from_geom(&edges[CFX_SPECIAL_BOARD_VERTICAL_COUNT + r * cols + c],
                                          &geom, uv.u, uv.u_step);
        }
    }

    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            CfxBoardGridCell *cell = &cells[r * cols + c];
            const CfxBoardPoint *p0 = &points[row0 + c];
            const CfxBoardPoint *p1 = &points[row0 + c + 1];
            const CfxBoardPoint *p2 = &points[row1 + c + 1];
            const CfxBoardPoint *p3 = &points[row1 + c];
            cell->edge[0] = (uint8_t)(CFX_SPECIAL_BOARD_VERTICAL_COUNT + r * cols + c);
            cell->edge[1] = (uint8_t)((r * (cols + 1)) + c + 1);
            cell->edge[2] = (uint8_t)(CFX_SPECIAL_BOARD_VERTICAL_COUNT + (r + 1) * cols + c);
            cell->edge[3] = (uint8_t)((r * (cols + 1)) + c);
            cell->min_y = p0->y;
            cell->max_y = p0->y;
            if (p1->y < cell->min_y) cell->min_y = p1->y;
            if (p2->y < cell->min_y) cell->min_y = p2->y;
            if (p3->y < cell->min_y) cell->min_y = p3->y;
            if (p1->y > cell->max_y) cell->max_y = p1->y;
            if (p2->y > cell->max_y) cell->max_y = p2->y;
            if (p3->y > cell->max_y) cell->max_y = p3->y;
            cell->active0 = -1;
            cell->active1 = -1;
        }
    }

    /* Visit one projected board row at a time.  The previous global-Y walk
       checked all 20 cells for every screen row, even though only one logical
       board row is active there.  Keeping the authored cell order but making
       the row range the outer loop removes those 15 inactive-cell checks from
       every scanline without changing edge stepping or emitted spans. */
    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int16_t row_min_y = (int16_t)state->height;
        int16_t row_max_y = 0;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            CfxBoardGridCell *cell = &cells[r * cols + c];
            if (cell->min_y < row_min_y) row_min_y = cell->min_y;
            if (cell->max_y > row_max_y) row_max_y = cell->max_y;
            cell->active0 = -1;
            cell->active1 = -1;
        }
        if (row_min_y < 0) row_min_y = 0;
        if (row_max_y > state->height) row_max_y = (int16_t)state->height;
        for (DEFAULT_INT c = 0; c <= cols; ++c)
            cfx_board_grid_reset(&edges[r * (cols + 1) + c]);
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            cfx_board_grid_reset(&edges[CFX_SPECIAL_BOARD_VERTICAL_COUNT + r * cols + c]);
            cfx_board_grid_reset(&edges[CFX_SPECIAL_BOARD_VERTICAL_COUNT + (r + 1) * cols + c]);
        }
        /* A moving FM board is texture-detail limited at this resolution.  The
           paired path below still visits every authored camera pose, but emits
           one nearest-neighbour sample for a 2x2 framebuffer block.  Advance
           the edge state by the same two scanlines so the next pose starts
           from the exact projected geometry; this is spatial LOD, never frame
           dropping.  Keep the final unpaired row exact. */
        for (int16_t y = row_min_y; y < row_max_y; ) {
            int block_rows = (y + 1 < row_max_y && y + 1 < state->height) ? 2 : 1;
            for (DEFAULT_INT c = 0; c < cols; ++c) {
                CfxBoardGridCell *cell = &cells[r * cols + c];
                int32_t xs0, xs1, us0, us1, vs0, vs1;
                int16_t x_start, x_end, span;
                uint16_t denom;
                int8_t step_u, step_v;

                if (y < cell->min_y || y >= cell->max_y) continue;
                if (cell->active0 < 0 &&
                    !cfx_board_grid_select_active(cell, edges, y,
                                                  &cell->active0, &cell->active1))
                    return 0;

                cfx_board_grid_values(&edges[cell->edge[(int)cell->active0]],
                                      cell->active0, y, &xs0, &us0, &vs0);
                cfx_board_grid_values(&edges[cell->edge[(int)cell->active1]],
                                      cell->active1, y, &xs1, &us1, &vs1);
                if (xs0 > xs1) {
                    int32_t t;
                    t = xs0; xs0 = xs1; xs1 = t;
                    t = us0; us0 = us1; us1 = t;
                    t = vs0; vs0 = vs1; vs1 = t;
                }
                x_start = (int16_t)((xs0 + ((1 << CFX_GEOM_FIXED_SHIFT) - 1)) >> CFX_GEOM_FIXED_SHIFT);
                x_end = (int16_t)(xs1 >> CFX_GEOM_FIXED_SHIFT);
                span = (int16_t)(x_end - x_start + 1);
                if (span > 0) {
                    denom = (uint16_t)((span > 256) ? 256 : span);
                    step_u = (int8_t)(cfx_fast_div_tz_i32_u16_q15(us1 - us0, denom) >> 8);
                    step_v = (vs1 == vs0) ? 0 :
                        (int8_t)(cfx_fast_div_tz_i32_u16_q15(vs1 - vs0, denom) >> 8);
                    if (block_rows) {
#if defined(WAIFU_PROFILE_RENDER)
                        cfx_profile_board_span(state, 1);
#endif
                        cfx_draw_board_span_direct_block2x2(
                            state, ((r + c) & 1) ? odd_src : even_src, y,
                            x_start, span,
                            cfx_pack_tex_state((uint8_t)(us0 >> 8), (uint8_t)(vs0 >> 8)),
                            step_u, step_v);
                    } else {
                        cfx_draw_board_span_tilted(
                            state, ((r + c) & 1) ? odd_src : even_src, y,
                            x_start, span,
                            cfx_pack_tex_state((uint8_t)(us0 >> 8), (uint8_t)(vs0 >> 8)),
                            step_u, step_v);
                    }
                }

                if (y + 1 < cell->max_y &&
                    (y + 1 >= edges[cell->edge[(int)cell->active0]].y_end ||
                     y + 1 >= edges[cell->edge[(int)cell->active1]].y_end)) {
                    cell->active0 = -1;
                    cell->active1 = -1;
                    if (!cfx_board_grid_select_active(cell, edges, (int16_t)(y + 1),
                                                      &cell->active0, &cell->active1))
                        return 0;
                }
            }
            y = (int16_t)(y + block_rows);
        }
    }
    return 1;
}
#endif

static uint8_t CFX_BOARD_MESH_COLD cfx_draw_board_mesh_cached(const CfxRenderer3DState *state,
                                          const CfxBoardPoint *points,
                                          DEFAULT_INT point_stride,
                                          DEFAULT_INT rows, DEFAULT_INT cols,
                                          DEFAULT_INT even_tile,
                                          DEFAULT_INT odd_tile,
                                          uint8_t trusted_convex)
{
    CfxBoardGeomEdge vertical[CFX_BOARD_MESH_MAX_COLS + 1];
    CfxBoardGeomEdge top_edges[CFX_BOARD_MESH_MAX_COLS];
    CfxBoardGeomEdge bottom_edges[CFX_BOARD_MESH_MAX_COLS];
    uint8_t vertical_v_start[CFX_BOARD_MESH_MAX_COLS + 1];
    int32_t vertical_v_step[CFX_BOARD_MESH_MAX_COLS + 1];
    const DEFAULT_INT even = cfx_board_clamp_tile(even_tile);
    const DEFAULT_INT odd = cfx_board_clamp_tile(odd_tile);
    const uint8_t *even_src = state->texture_atlas + ((int32_t)even * state->tile_stride_bytes);
    const uint8_t *odd_src = state->texture_atlas + ((int32_t)odd * state->tile_stride_bytes);

    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c <= cols; ++c) {
            cfx_board_build_geom_edge(&vertical[c], &points[row0 + c], &points[row1 + c]);
            cfx_board_vertical_v_params(&points[row0 + c], &points[row1 + c],
                                        &vertical_v_start[c], &vertical_v_step[c]);
        }
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            cfx_board_build_geom_edge(&top_edges[c],
                &points[row0 + c], &points[row0 + c + 1]);
            cfx_board_build_geom_edge(&bottom_edges[c],
                &points[row1 + c + 1], &points[row1 + c]);
        }

        for (DEFAULT_INT c = 0; c < cols; ++c) {
            const CfxBoardPoint *p0 = &points[row0 + c];
            const CfxBoardPoint *p1 = &points[row0 + c + 1];
            const CfxBoardPoint *p2 = &points[row1 + c + 1];
            const CfxBoardPoint *p3 = &points[row1 + c];
            CfxFastQuadEdge edges[4];
            int16_t min_y = p0->y;
            int16_t max_y = p0->y;
            const uint8_t *tile = ((r + c) & 1) ? odd_src : even_src;

            if (p1->y < min_y) min_y = p1->y;
            if (p2->y < min_y) min_y = p2->y;
            if (p3->y < min_y) min_y = p3->y;
            if (p1->y > max_y) max_y = p1->y;
            if (p2->y > max_y) max_y = p2->y;
            if (p3->y > max_y) max_y = p3->y;

            cfx_board_set_edge_uv(&edges[0], &top_edges[c], p0, p1, 0, 0, 255, 0);
            cfx_board_set_precomputed_uv(&edges[1], &vertical[c + 1],
                                         255, vertical_v_start[c + 1],
                                         0, vertical_v_step[c + 1]);
            cfx_board_set_edge_uv(&edges[2], &bottom_edges[c], p2, p3, 255, 255, 0, 255);
            cfx_board_set_precomputed_uv(&edges[3], &vertical[c],
                                         0, vertical_v_start[c],
                                         0, vertical_v_step[c]);
            if (trusted_convex) {
                cfx_draw_textured_quad_fast_affine_trusted_edges(state, tile, edges,
                                                                 min_y, max_y, 1);
            } else {
                cfx_draw_textured_quad_fast_affine_edges(state, tile, edges,
                                                         min_y, max_y, 1);
            }
        }
    }
    return 1;
}

#else
uint8_t cfx_renderer3d_draw_quad_fast_affine(CfxRenderer3D *renderer,
                                             const Point2D *p0, const Point2D *p1,
                                             const Point2D *p2, const Point2D *p3,
                                             DEFAULT_INT tetromino_type)
{
    cfx_renderer3d_draw_quad(renderer, p0, p1, p2, p3, tetromino_type);
    return 1;
}

#endif

static uint8_t CFX_BOARD_MESH_COLD cfx_draw_board_mesh_fallback(CfxRenderer3D *renderer,
                                            const CfxBoardPoint *points,
                                            DEFAULT_INT point_stride,
                                            DEFAULT_INT rows, DEFAULT_INT cols,
                                            DEFAULT_INT even_tile,
                                            DEFAULT_INT odd_tile)
{
    const DEFAULT_INT uvmax = (DEFAULT_INT)((CFX_TEXTURE_TILE_SIZE - 1) << 8);
    for (DEFAULT_INT r = 0; r < rows; ++r) {
        int32_t row0 = (int32_t)r * point_stride;
        int32_t row1 = (int32_t)(r + 1) * point_stride;
        for (DEFAULT_INT c = 0; c < cols; ++c) {
            Point2D p0 = {(DEFAULT_INT)points[row0 + c].x,
                          (DEFAULT_INT)points[row0 + c].y, 0, 0};
            Point2D p1 = {(DEFAULT_INT)points[row0 + c + 1].x,
                          (DEFAULT_INT)points[row0 + c + 1].y, uvmax, 0};
            Point2D p2 = {(DEFAULT_INT)points[row1 + c + 1].x,
                          (DEFAULT_INT)points[row1 + c + 1].y, uvmax, uvmax};
            Point2D p3 = {(DEFAULT_INT)points[row1 + c].x,
                          (DEFAULT_INT)points[row1 + c].y, 0, uvmax};
            cfx_renderer3d_draw_quad_fast_affine(renderer, &p0, &p1, &p2, &p3,
                ((r + c) & 1) ? odd_tile : even_tile);
        }
    }
    return 1;
}

uint8_t cfx_renderer3d_draw_board_mesh_fast_affine(
    CfxRenderer3D *renderer, const CfxBoardPoint *points,
    DEFAULT_INT point_stride, DEFAULT_INT rows, DEFAULT_INT cols,
    DEFAULT_INT even_tile, DEFAULT_INT odd_tile)
{
    CfxRenderer3DState *state = cfx_state(renderer);
    if (!state->framebuffer || !state->texture_atlas || !points) return 0;
    if (rows < 0 || cols < 0 || point_stride <= cols) return 0;
    if (rows == 0 || cols == 0) return 1;
#if defined(WAIFU_PROFILE_RENDER)
    if (state->profile) {
        state->profile->cells += (uint32_t)rows * (uint32_t)cols;
        state->profile->board_path = CFX_PROFILE_BOARD_FALLBACK;
    }
#endif
#if CFX_RENDERER_DIRECT_RECT
    if (cols > CFX_BOARD_MESH_MAX_COLS) {
        return cfx_draw_board_mesh_fallback(renderer, points, point_stride,
                                            rows, cols, even_tile, odd_tile);
    }
    {
        int all_axis = 1;
        int all_trapezoid = 1;
        for (DEFAULT_INT r = 0; r < rows && (all_axis || all_trapezoid); ++r) {
            int32_t row0 = (int32_t)r * point_stride;
            int32_t row1 = (int32_t)(r + 1) * point_stride;
            for (DEFAULT_INT c = 0; c < cols; ++c) {
                const CfxBoardPoint *p0 = &points[row0 + c];
                const CfxBoardPoint *p1 = &points[row0 + c + 1];
                const CfxBoardPoint *p2 = &points[row1 + c + 1];
                const CfxBoardPoint *p3 = &points[row1 + c];
                if (!cfx_board_axis_cell(p0, p1, p2, p3)) {
                    all_axis = 0;
                }
                if (p0->y != p1->y || p2->y != p3->y ||
                    (p0->y == p3->y) || (p1->y == p2->y) ||
                    ((p0->y < p3->y) != (p1->y < p2->y))) {
                    all_trapezoid = 0;
                }
            }
        }
        if (all_axis) {
#if defined(WAIFU_PROFILE_RENDER)
            if (state->profile) state->profile->board_path = CFX_PROFILE_BOARD_AXIS;
#endif
            return cfx_draw_board_mesh_axis(state, points, point_stride,
                                            rows, cols, even_tile, odd_tile);
        }
        if (all_trapezoid) {
            if (cfx_board_rows_uniform_trapezoid(points, point_stride, rows, cols)) {
#if defined(WAIFU_PROFILE_RENDER)
                if (state->profile) state->profile->board_path = CFX_PROFILE_BOARD_TRAPEZOID_ROWS;
#endif
                return cfx_draw_board_mesh_trapezoid_rows(state, points, point_stride,
                                                          rows, cols, even_tile, odd_tile);
            }
#if defined(WAIFU_PROFILE_RENDER)
            if (state->profile) state->profile->board_path = CFX_PROFILE_BOARD_TRAPEZOID;
#endif
            return cfx_draw_board_mesh_trapezoid(state, points, point_stride,
                                                 rows, cols, even_tile, odd_tile);
        }
        }
        {
            uint8_t trusted_convex = (uint8_t)cfx_board_mesh_strictly_convex(
                points, point_stride, rows, cols, state->width, state->height);
#if CFX_RENDERER_SPECIALIZED_BOARD_GRID
            if (trusted_convex &&
                cfx_draw_board_mesh_specialized_grid(state, points, point_stride,
                                                     rows, cols, even_tile, odd_tile)) {
#if defined(WAIFU_PROFILE_RENDER)
                if (state->profile) state->profile->board_path = CFX_PROFILE_BOARD_SPECIALIZED_GRID;
#endif
                return 1;
            }
#endif
#if defined(WAIFU_PROFILE_RENDER)
            if (state->profile) state->profile->board_path = CFX_PROFILE_BOARD_CACHED_EDGES;
#endif
            return cfx_draw_board_mesh_cached(state, points, point_stride, rows, cols,
                                              even_tile, odd_tile, trusted_convex);
        }
#else
    return cfx_draw_board_mesh_fallback(renderer, points, point_stride,
                                        rows, cols, even_tile, odd_tile);
#endif
}

static void cfx_draw_textured_triangle(const CfxRenderer3DState *renderer,
#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
                                       const uint8_t *direct_tile,
#endif
                                       CfxVertexIn p1, CfxVertexIn p2, CfxVertexIn p3)
{
    if (p1.y > p2.y) { CfxVertexIn t = p1; p1 = p2; p2 = t; }
    if (p1.y > p3.y) { CfxVertexIn t = p1; p1 = p3; p3 = t; }
    if (p2.y > p3.y) { CfxVertexIn t = p2; p2 = p3; p3 = t; }

    int16_t y1 = p1.y;
    int16_t y2 = p2.y;
    int16_t y3 = p3.y;
    int16_t total_height = (int16_t)(y3 - y1);
    if (total_height == 0) {
        return;
    }

    int16_t temp = (int16_t)cfx_div_toward_zero((y2 - y1) << CFX_GEOM_FIXED_SHIFT, total_height);
    int32_t longest = (int32_t)temp * ((int32_t)p3.x - (int32_t)p1.x) + (((int32_t)p1.x - (int32_t)p2.x) << CFX_GEOM_FIXED_SHIFT);
    if (longest == 0) {
        return;
    }

    CfxLeftEdge left;
    CfxRightEdge right;

    if (longest < 0) {
        right.vertices[0] = &p3; right.vertices[1] = &p2; right.vertices[2] = &p1;
        right.section = 2;
        left.vertices[0] = &p3; left.vertices[1] = &p1; left.vertices[2] = &p1;
        left.section = 1;
        if (cfx_left_section(&left) <= 0) {
            return;
        }
        if (cfx_right_section(&right) <= 0) {
            --right.section;
            if (cfx_right_section(&right) <= 0) {
                return;
            }
        }
    } else {
        left.vertices[0] = &p3; left.vertices[1] = &p2; left.vertices[2] = &p1;
        left.section = 2;
        right.vertices[0] = &p3; right.vertices[1] = &p1; right.vertices[2] = &p1;
        right.section = 1;
        if (cfx_right_section(&right) <= 0) {
            return;
        }
        if (cfx_left_section(&left) <= 0) {
            --left.section;
            if (cfx_left_section(&left) <= 0) {
                return;
            }
        }
    }

    int32_t num_u = (int32_t)temp * ((int32_t)p3.u - (int32_t)p1.u) + cfx_int_to_fixed((int32_t)p1.u - (int32_t)p2.u);
    int32_t num_v = (int32_t)temp * ((int32_t)p3.v - (int32_t)p1.v) + cfx_int_to_fixed((int32_t)p1.v - (int32_t)p2.v);
    int8_t span_step_u = (int8_t)(int16_t)cfx_div_toward_zero_i32d(num_u, longest);
    int8_t span_step_v = (int8_t)(int16_t)cfx_div_toward_zero_i32d(num_v, longest);
    int16_t span_step_linear = cfx_pack_tex_step_linear(span_step_u, span_step_v);

    for (;;) {
        int16_t xs = (int16_t)(left.x >> CFX_GEOM_FIXED_SHIFT);
        int16_t xe = (int16_t)(right.x >> CFX_GEOM_FIXED_SHIFT);
        int16_t span = (int16_t)(xe - xs + 1);
        uint16_t tex_state = cfx_pack_tex_state((uint8_t)(left.u >> CFX_EDGE_UV_SHIFT), (uint8_t)(left.v >> CFX_EDGE_UV_SHIFT));

#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
        if (direct_tile) {
            cfx_draw_span_direct_tile(renderer, direct_tile, y1, xs, span,
                                      tex_state, span_step_u, span_step_v);
        } else
#endif
        {
            cfx_draw_span(renderer, y1, xs, span, tex_state, span_step_linear, span_step_u, span_step_v);
        }
        ++y1;

        if (--left.section_height <= 0) {
            if (--left.section <= 0) {
                return;
            }
            if (cfx_left_section(&left) <= 0) {
                return;
            }
        } else {
            left.x += left.delta_x;
            left.u += left.delta_u;
            left.v += left.delta_v;
        }

        if (--right.section_height <= 0) {
            if (--right.section <= 0) {
                return;
            }
            if (cfx_right_section(&right) <= 0) {
                return;
            }
        } else {
            right.x += right.delta_x;
        }
    }
}


#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
/* ---- Compact affine board triangle rasterizer ---------------------------
   One small self-contained routine for the battle board (floor + slab walls).
   The whole hot path fits the V810 1KB I-cache, so re-rendering the board's
   ~68 triangles every animation frame no longer thrashes the cache against the
   2.4KB generic triangle/span machinery -- that I-cache miss storm was the
   board's real per-frame cost.  Perspective lives in the projected vertices;
   texturing is affine (per-triangle constant gradient, what the board used
   before).  u/v are Q8 texels (cell corners 0 and 31<<8).  Clips per scanline
   (Y range + X clamp), so off-screen wall quads need no separate clip path.
   32x32 pitch-32 tiles only. */
#define CFX_BOARD_GCLAMP (64 << 8)
static inline int32_t cfx_board_clampg(int32_t g)
{
    if (g > CFX_BOARD_GCLAMP) return CFX_BOARD_GCLAMP;
    if (g < -CFX_BOARD_GCLAMP) return -CFX_BOARD_GCLAMP;
    return g;
}


static void cfx_board_tri(uint8_t *fb, int W, int H, int ylo, const uint8_t *tile,
    int x0, int y0, int32_t U0, int32_t V0,
    int x1, int y1, int32_t U1, int32_t V1,
    int x2, int y2, int32_t U2, int32_t V2)
{
    int t; int32_t tU;
    /* GENEROUS bound (well outside the 256x240 screen) chosen so the int32 edge /
       gradient math stays in range: real on- and moderately off-screen geometry
       (slab walls whose bottom corners fall below the screen on a low/perspective
       camera) is far inside +-8192, so it is NOT distorted; only pathological
       near-singularity projections get bounded.  No 64-bit math (slow on V810):
       the edges are MARCHED continuously (so the accumulator stays inside the
       corner x range and can't overflow), and the loop breaks once it drops below
       the screen, so an off-screen bottom corner draws the correct on-screen slab
       edge instead of garbage. */
    if (x0 < -8192) x0 = -8192; else if (x0 > 8192) x0 = 8192;
    if (x1 < -8192) x1 = -8192; else if (x1 > 8192) x1 = 8192;
    if (x2 < -8192) x2 = -8192; else if (x2 > 8192) x2 = 8192;
    if (y0 < -8192) y0 = -8192; else if (y0 > 8192) y0 = 8192;
    if (y1 < -8192) y1 = -8192; else if (y1 > 8192) y1 = 8192;
    if (y2 < -8192) y2 = -8192; else if (y2 > 8192) y2 = 8192;
    if (y0 > y1) { t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; tU=U0;U0=U1;U1=tU; tU=V0;V0=V1;V1=tU; }
    if (y0 > y2) { t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; tU=U0;U0=U2;U2=tU; tU=V0;V0=V2;V2=tU; }
    if (y1 > y2) { t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; tU=U1;U1=U2;U2=tU; tU=V1;V1=V2;V2=tU; }
    if (y2 <= y0) return;
    {
        int dy02 = y2 - y0, dy01 = y1 - y0, dy12 = y2 - y1;
        int det = (x1 - x0) * dy02 - (x2 - x0) * dy01;
        int gUx, gVx, half, y;
        int32_t xl, dxl, xs, dxs;
        int32_t ul, dul, vl, dvl, us, dus, vs, dvs;
        if (det == 0) return;

        /* PC-FX hot path: keep V810 DIV out of the board renderer and keep MUL
           out of the scanline loop.  The old version evaluated

               U = U0 + (x - x0) * gUx + (y - y0) * gUy

           for every row, which costs 4 MUL per emitted scanline on top of 7
           hardware divides per triangle.  This is the same affine idea as the
           envmap triangle code: do setup once, then march x/u/v edges with ADDs
           and fill spans with ADDs.  cfx_div_toward_zero_i32d still uses the
           reciprocal path for normal board geometry, but falls back to exact
           division if projection inputs make the correction too large. */
        gUx = cfx_board_clampg(cfx_div_toward_zero_i32d((U1 - U0) * dy02 - (U2 - U0) * dy01, det));
        gVx = cfx_board_clampg(cfx_div_toward_zero_i32d((V1 - V0) * dy02 - (V2 - V0) * dy01, det));
        xl = (int32_t)x0 << 16;
        dxl = cfx_div_toward_zero_i32d((int32_t)(x2 - x0) << 16, dy02);
        ul = U0 << 16; vl = V0 << 16;
        dul = cfx_div_toward_zero_i32d((U2 - U0) << 16, dy02);
        dvl = cfx_div_toward_zero_i32d((V2 - V0) << 16, dy02);
        y = y0;
        for (half = 0; half < 2; ++half) {
            int yend;
            if (half == 0) {
                if (dy01 == 0) continue;
                yend = y1;
                xs = (int32_t)x0 << 16;
                us = U0 << 16; vs = V0 << 16;
                dxs = cfx_div_toward_zero_i32d((int32_t)(x1 - x0) << 16, dy01);
                dus = cfx_div_toward_zero_i32d((U1 - U0) << 16, dy01);
                dvs = cfx_div_toward_zero_i32d((V1 - V0) << 16, dy01);
            } else {
                if (dy12 == 0) break;
                yend = y2;
                xs = (int32_t)x1 << 16;
                us = U1 << 16; vs = V1 << 16;
                dxs = cfx_div_toward_zero_i32d((int32_t)(x2 - x1) << 16, dy12);
                dus = cfx_div_toward_zero_i32d((U2 - U1) << 16, dy12);
                dvs = cfx_div_toward_zero_i32d((V2 - V1) << 16, dy12);
            }
            for (; y < yend; ) {
                int block_rows = 0;
#if defined(WAIFU_FM_FMTOWNS) && CFX_RENDERER_USE_I386_ASM && \
    !defined(CFX_MEASURE_SKIP_SPANS) && !defined(CFX_MEASURE_C_BOARD_FILL)
                block_rows = (y + 1 < yend && y + 1 < H);
#endif
                if (y >= H) return;            /* below screen/band: nothing left to draw */
                if (y >= ylo) {
                    int xa = (int)(xl >> 16);
                    int xb = (int)(xs >> 16);
                    int use_long_left = (xa < xb);
                    int edge_left = use_long_left ? xa : xb;
                    int edge_right = use_long_left ? xb : xa;
                    int left = edge_left;
                    int right = edge_right;
                    if (left < 0) left = 0;
                    if (right >= W) right = W - 1;
                    if (left <= right) {
                        int32_t U = (use_long_left ? ul : us) >> 16;
                        int32_t V = (use_long_left ? vl : vs) >> 16;
                        if (left != edge_left) {
                            U += (int32_t)(left - edge_left) * gUx;
                            V += (int32_t)(left - edge_left) * gVx;
                        }
#if defined(WAIFU_FM_FMTOWNS) && CFX_RENDERER_USE_I386_ASM && \
    !defined(CFX_MEASURE_SKIP_SPANS) && !defined(CFX_MEASURE_C_BOARD_FILL)
                        if (block_rows) {
                            cfx_board_fill_block2x2(
                                fb + ((W == 256) ? ((int32_t)y << 8) : (int32_t)y * W) + left,
                                right - left + 1, U, V, gUx, gVx, tile);
                        } else
#endif
                        cfx_board_fill(fb + ((W == 256) ? ((int32_t)y << 8) : (int32_t)y * W) + left,
                                       right - left + 1, U, V, gUx, gVx, tile);
                    }
                }
                if (block_rows) {
                    xl += dxl * block_rows; ul += dul * block_rows; vl += dvl * block_rows;
                    xs += dxs * block_rows; us += dus * block_rows; vs += dvs * block_rows;
                    y += block_rows;
                } else {
                    xl += dxl; ul += dul; vl += dvl;
                    xs += dxs; us += dus; vs += dvs;
                    ++y;
                }
            }
        }
    }
}
#endif

void cfx_renderer3d_draw_quad_board(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type)
{
#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
    cfx_renderer3d_draw_quad_board_band(renderer, p0, p1, p2, p3, tetromino_type,
                                        0, (DEFAULT_INT)cfx_state(renderer)->height);
#else
    cfx_renderer3d_draw_quad(renderer, p0, p1, p2, p3, tetromino_type);
#endif
}

/* Band-clipped board quad: fills only scanlines in [y0, y1).  Two CPUs can
   rasterize the same quad list into disjoint bands with no write overlap and
   painter order preserved inside each band (CD32X story scenes); the plain
   quad entry point above is this with the full framebuffer height. */
void cfx_renderer3d_draw_quad_board_band(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type, DEFAULT_INT y0, DEFAULT_INT y1)
{
#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
    CfxRenderer3DState *state = cfx_state(renderer);
    if (!state->framebuffer || !state->texture_atlas) {
        return;
    }

    if (tetromino_type < 0) {
        tetromino_type = 0;
    } else if (tetromino_type >= CFX_TEXTURE_TILE_COUNT) {
        tetromino_type = CFX_TEXTURE_TILE_COUNT - 1;
    }
    if (y0 < 0) y0 = 0;
    if (y1 > (DEFAULT_INT)state->height) y1 = (DEFAULT_INT)state->height;
    if (y0 >= y1) return;

    {
        const uint8_t *tile = state->texture_atlas + ((int32_t)tetromino_type * state->tile_stride_bytes);
        uint8_t *fb = state->framebuffer;
        int W = (int)state->width;
        cfx_board_tri(fb, W, (int)y1, (int)y0, tile,
                      (int)p0->x, (int)p0->y, (int32_t)p0->u, (int32_t)p0->v,
                      (int)p1->x, (int)p1->y, (int32_t)p1->u, (int32_t)p1->v,
                      (int)p2->x, (int)p2->y, (int32_t)p2->u, (int32_t)p2->v);
        cfx_board_tri(fb, W, (int)y1, (int)y0, tile,
                      (int)p0->x, (int)p0->y, (int32_t)p0->u, (int32_t)p0->v,
                      (int)p2->x, (int)p2->y, (int32_t)p2->u, (int32_t)p2->v,
                      (int)p3->x, (int)p3->y, (int32_t)p3->u, (int32_t)p3->v);
    }
#else
    (void)y0; (void)y1;
    cfx_renderer3d_draw_quad(renderer, p0, p1, p2, p3, tetromino_type);
#endif
}

void cfx_renderer3d_draw_quad(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type)
{
    CfxRenderer3DState *state = cfx_state(renderer);
    if (!state->framebuffer || !state->texture_atlas) {
        return;
    }

    if (tetromino_type < 0) {
        tetromino_type = 0;
    }

    const uint8_t *tile = state->texture_atlas + ((int32_t)tetromino_type * state->tile_stride_bytes);
    CfxVertexIn v0 = cfx_make_vertex(p0);
    CfxVertexIn v1 = cfx_make_vertex(p1);
    CfxVertexIn v2 = cfx_make_vertex(p2);
    CfxVertexIn v3 = cfx_make_vertex(p3);

#if CFX_RENDERER_DIRECT_RECT
    /* Most in-game block front faces are unrotated screen-aligned rectangles.
       Draw those directly from the 16x16 source tile so Loopy does not rebuild
       the 64 KiB expanded texture LUT every time adjacent cells have different
       tetromino colors. */
    if (cfx_draw_axis_rect_direct_tile(state, tile, &v0, &v1, &v2, &v3)) {
        return;
    }
#endif

#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
    /* The locked-board cache is rendered into an offscreen CPU bitmap.  For
       non-axis side/top/bottom faces, sample the 32x32 source tile directly
       instead of rebuilding the single expanded 64 KiB texture LUT whenever the
       tetromino color changes.  Keep the direct-KING live-piece path on the
       older LUT/FP-exact path to avoid touching foreground timing/edges. */
    if (!cfx_renderer3d_direct_kram_active(state))
    {
        cfx_draw_textured_triangle(state, tile, v0, v1, v2);
        cfx_draw_textured_triangle(state, tile, v0, v2, v3);
        return;
    }
#endif

#if CFX_RENDERER_MULTI_LUT
    cfx_renderer3d_select_pcfx_tile_lut(state, tetromino_type);
#endif

#if !CFX_RENDERER_MULTI_LUT
#if CFX_RENDERER_BUILD_SINGLE_LUT
    cfx_renderer3d_build_lut_for_tile(state, tile);
#else
    /* CD32X keeps the large 64 KiB expanded texture LUT out of SDRAM.
       Its direct-tile paths should have handled board quads above; if a
       later path reaches the LUT-only fallback, skip the draw rather than
       sampling an absent cache. */
    return;
#endif
#else
    (void)tile;
#endif

#if CFX_RENDERER_QUAD_SCANLINE
    if (cfx_renderer3d_direct_kram_active(state)) {
        cfx_draw_textured_quad_scanline(state, v0, v1, v2, v3);
        return;
    }
#endif

    if (cfx_draw_axis_rect_lut(state, &v0, &v1, &v2, &v3)) {
        return;
    }

    cfx_draw_textured_triangle(state,
#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
                               NULL,
#endif
                               v0, v1, v2);
    cfx_draw_textured_triangle(state,
#if CFX_RENDERER_DIRECT_GENERIC_TILE && CFX_RENDERER_DIRECT_RECT
                               NULL,
#endif
                               v0, v2, v3);
}

void cfx_renderer3d_draw_face_list(CfxRenderer3D *renderer, FaceToDraw *faces, DEFAULT_INT face_count)
{
    for (DEFAULT_INT i = 0; i < face_count; ++i) {
        FaceToDraw *face = faces + i;
        cfx_renderer3d_draw_quad(renderer,
            &face->projected_vertices[0],
            &face->projected_vertices[1],
            &face->projected_vertices[2],
            &face->projected_vertices[3],
            face->tetromino_type);
    }
}
