#include "renderer3d_internal.h"

#if CFX_RENDERER_DIRECT_KRAM
extern uint8_t *cfx_game_framebuffer(void);
extern int cfx_pcfx_current_page_word_offset(void);
extern int cfx_pcfx_current_kram_page_word_offset;
extern void eris_king_set_kram_write(uint32_t addr, int incr);
#endif

#if CFX_RENDERER_DIRECT_KRAM
static inline __attribute__((always_inline)) uint8_t cfx_renderer3d_direct_kram_active(const CfxRenderer3DState *renderer)
{
    return (uint8_t)(renderer->framebuffer == cfx_game_framebuffer());
}

static inline __attribute__((always_inline)) void cfx_pcfx_kram_select_data_register(void)
{
    __asm__ volatile (
        "movea 14, r0, r10\n\t"
        "out.h r10, 0x600[r0]\n\t"
        :
        :
        : "r10", "memory");
}

static inline __attribute__((always_inline)) void cfx_pcfx_kram_seek_frame_word(int32_t frame_word_index)
{
    /* Inline eris_king_set_kram_write(addr, 1) plus select KRAM data register.
       The scanline rasterizer calls this once per emitted row, so avoiding the
       liberis function call matters on V810.  KING register 0x0D receives the
       write address ORed with increment<<18; register 0x0E is the data port. */
    uint32_t addr = (uint32_t)(cfx_pcfx_current_kram_page_word_offset + frame_word_index) | (1u << 18);
    __asm__ volatile (
        "movea 13, r0, r10\n\t"
        "out.h r10, 0x600[r0]\n\t"
        "out.w %[addr], 0x604[r0]\n\t"
        "movea 14, r0, r10\n\t"
        "out.h r10, 0x600[r0]\n\t"
        :
        : [addr] "r" (addr)
        : "r10", "memory");
}

static inline __attribute__((always_inline)) void cfx_pcfx_kram_write_word(uint16_t value)
{
    __asm__ volatile ("out.h %0, 0x604[r0]" : : "r" (value) : "memory");
}
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
static uint8_t tex_lut[1u << 16] __attribute__((aligned(16)));
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

static const uint16_t cfx_recip_q15_u8[257] = {
    0, 32768, 16384, 10922, 8192, 6553, 5461, 4681, 4096, 3640, 3276, 2978, 2730, 2520, 2340, 2184,
    2048, 1927, 1820, 1724, 1638, 1560, 1489, 1424, 1365, 1310, 1260, 1213, 1170, 1129, 1092, 1057,
    1024, 992, 963, 936, 910, 885, 862, 840, 819, 799, 780, 762, 744, 728, 712, 697,
    682, 668, 655, 642, 630, 618, 606, 595, 585, 574, 564, 555, 546, 537, 528, 520,
    512, 504, 496, 489, 481, 474, 468, 461, 455, 448, 442, 436, 431, 425, 420, 414,
    409, 404, 399, 394, 390, 385, 381, 376, 372, 368, 364, 360, 356, 352, 348, 344,
    341, 337, 334, 330, 327, 324, 321, 318, 315, 312, 309, 306, 303, 300, 297, 295,
    292, 289, 287, 284, 282, 280, 277, 275, 273, 270, 268, 266, 264, 262, 260, 258,
    256, 254, 252, 250, 248, 246, 244, 242, 240, 239, 237, 235, 234, 232, 230, 229,
    227, 225, 224, 222, 221, 219, 218, 217, 215, 214, 212, 211, 210, 208, 207, 206,
    204, 203, 202, 201, 199, 198, 197, 196, 195, 193, 192, 191, 190, 189, 188, 187,
    186, 185, 184, 183, 182, 181, 180, 179, 178, 177, 176, 175, 174, 173, 172, 171,
    170, 169, 168, 168, 167, 166, 165, 164, 163, 163, 162, 161, 160, 159, 159, 158,
    157, 156, 156, 155, 154, 153, 153, 152, 151, 151, 150, 149, 148, 148, 147, 146,
    146, 145, 144, 144, 143, 143, 142, 141, 141, 140, 140, 139, 138, 138, 137, 137,
    136, 135, 135, 134, 134, 133, 133, 132, 132, 131, 131, 130, 130, 129, 129, 128,
    128
};

static const uint32_t cfx_recip_q24_u8[257] = {
    0, 16777216, 8388608, 5592405, 4194304, 3355443, 2796203, 2396745,
    2097152, 1864135, 1677722, 1525201, 1398101, 1290555, 1198373, 1118481,
    1048576, 986895, 932068, 883011, 838861, 798915, 762601, 729444,
    699051, 671089, 645278, 621378, 599186, 578525, 559241, 541201,
    524288, 508400, 493448, 479349, 466034, 453438, 441506, 430185,
    419430, 409200, 399458, 390168, 381300, 372827, 364722, 356962,
    349525, 342392, 335544, 328965, 322639, 316551, 310689, 305040,
    299593, 294337, 289262, 284360, 279620, 275036, 270600, 266305,
    262144, 258111, 254200, 250406, 246724, 243148, 239675, 236299,
    233017, 229825, 226719, 223696, 220753, 217886, 215093, 212370,
    209715, 207126, 204600, 202135, 199729, 197379, 195084, 192842,
    190650, 188508, 186414, 184365, 182361, 180400, 178481, 176602,
    174763, 172961, 171196, 169467, 167772, 166111, 164483, 162886,
    161319, 159783, 158276, 156796, 155345, 153919, 152520, 151146,
    149797, 148471, 147169, 145889, 144631, 143395, 142180, 140985,
    139810, 138655, 137518, 136400, 135300, 134218, 133153, 132104,
    131072, 130056, 129056, 128070, 127100, 126144, 125203, 124276,
    123362, 122461, 121574, 120699, 119837, 118987, 118149, 117323,
    116508, 115705, 114912, 114131, 113360, 112599, 111848, 111107,
    110376, 109655, 108943, 108240, 107546, 106861, 106185, 105517,
    104858, 104206, 103563, 102928, 102300, 101680, 101068, 100462,
    99864, 99273, 98690, 98112, 97542, 96978, 96421, 95870,
    95325, 94787, 94254, 93727, 93207, 92692, 92183, 91679,
    91181, 90688, 90200, 89718, 89241, 88768, 88301, 87839,
    87381, 86929, 86480, 86037, 85598, 85164, 84733, 84308,
    83886, 83469, 83056, 82646, 82241, 81840, 81443, 81049,
    80660, 80274, 79892, 79513, 79138, 78766, 78398, 78034,
    77672, 77314, 76960, 76608, 76260, 75915, 75573, 75234,
    74898, 74565, 74235, 73908, 73584, 73263, 72944, 72629,
    72316, 72005, 71698, 71392, 71090, 70790, 70493, 70198,
    69905, 69615, 69327, 69042, 68759, 68478, 68200, 67924,
    67650, 67378, 67109, 66841, 66576, 66313, 66052, 65793,
    65536
};

static inline int32_t cfx_fast_div_tz_i32_u16_q15(int32_t n, uint16_t d)
{
    if (d == 0) return 0;
    if (d == 1) return n;
    if (d > 256) {
        /* Fast board path only uses screen-sized spans/edges.  Clamp rather
           than falling back to DIV so the V810 hot renderer remains division-free. */
        d = 256;
    }
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
}

static const uint16_t cfx_recip_q8_u16[257] = {
    0, 256, 128, 85, 64, 51, 43, 37, 32, 28, 26, 23, 21, 20, 18, 17,
    16, 15, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 9, 9, 8,
    8, 8, 8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1
};

static inline int32_t cfx_div_toward_zero(int32_t n, int16_t d)
{
    if (d == 0) return 0;
    uint8_t neg = 0;
    uint32_t un = cfx_abs_i32_u32(n);
    if (n < 0) neg ^= 1;
    uint32_t ud = (d < 0) ? (uint32_t)(-(int32_t)d) : (uint32_t)d;
    if (d < 0) neg ^= 1;
    uint32_t q;
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
    if (tex_lut_ready && tex_lut_source_tile == tile && tex_lut_source_pitch == renderer->tile_pitch_bytes) {
        active_tex_lut = tex_lut;
        return;
    }

    cfx_fill_lut_from_tile(renderer, tile, tex_lut);

    tex_lut_source_tile = tile;
    tex_lut_source_pitch = renderer->tile_pitch_bytes;
    tex_lut_ready = 1;
    active_tex_lut = tex_lut;
}

uint32_t cfx_renderer3d_lut_size_bytes(void)
{
#if CFX_RENDERER_MULTI_LUT
    return (uint32_t)(sizeof(tex_lut) + sizeof(tex_lut_tiles));
#elif CFX_RENDERER_DIRECT_ROW_LUT
    return (uint32_t)(sizeof(tex_lut) + sizeof(tex_row_lut_tiles));
#else
    return (uint32_t)sizeof(tex_lut);
#endif
}

void cfx_renderer3d_init(CfxRenderer3D *renderer, const CfxRenderer3DConfig *config)
{
    CfxRenderer3DState *state = cfx_state(renderer);
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

static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_8(uint8_t *dst, uint16_t length, uint16_t tex_state, uint16_t tex_step)
{
    uint16_t offset = tex_state;
#if CFX_RENDERER_USE_V810_ASM
    uint32_t tmp_addr;
    uint32_t tmp_color;
    const uint8_t *lut = active_tex_lut;
    __asm__ volatile (
        "cmp 0,%[length]\n"
        "be 2f\n"
        "1:\n"
        "mov %[state],%[addr]\n"
        "andi 65535,%[addr],%[addr]\n"
        "add %[lut],%[addr]\n"
        "ld.b 0[%[addr]],%[color]\n"
        "st.b %[color],0[%[dst]]\n"
        "add 1,%[dst]\n"
        "add %[step],%[state]\n"
        "andi 65535,%[state],%[state]\n"
        "add -1,%[length]\n"
        "bne 1b\n"
        "2:\n"
        : [dst] "+r" (dst), [length] "+r" (length), [state] "+r" (offset),
          [addr] "=&r" (tmp_addr), [color] "=&r" (tmp_color)
        : [lut] "r" (lut), [step] "r" (tex_step)
        : "memory");
#else
    for (uint16_t i = 0; i < length; ++i) {
        dst[i] = active_tex_lut[offset];
        offset = (uint16_t)(offset + tex_step);
    }
#endif
}


#if CFX_RENDERER_USE_SH1_ASM
static __attribute__((noinline)) void cfx_draw_scanline_fast_pair_sh1_nowrap(uint16_t *dst, uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    const uint8_t *src = active_tex_lut + tex_state;
    int32_t step = (int32_t)tex_step_linear;
    uint16_t groups = (uint16_t)(pairs >> 1);
    uint16_t rem = (uint16_t)(pairs & 1u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            /* Four texel fetches first.  This keeps the load-use distance larger
               than the old pack-immediately loop and leaves only the required
               byte->halfword packing before the two stores. */
            "mov.b @%[src],%[c0]\n"
            "add %[step],%[src]\n"
            "mov.b @%[src],%[c1]\n"
            "add %[step],%[src]\n"
            "mov.b @%[src],%[c2]\n"
            "add %[step],%[src]\n"
            "mov.b @%[src],%[c3]\n"
            "add %[step],%[src]\n"
            "shll8 %[c0]\n"
            "or %[c1],%[c0]\n"
            "mov.w %[c0],@%[dst]\n"
            "add #2,%[dst]\n"
            "shll8 %[c2]\n"
            "or %[c3],%[c2]\n"
            "mov.w %[c2],@%[dst]\n"
            "add #2,%[dst]\n"
            "add #-1,%[groups]\n"
            "mov %[groups],r0\n"
            "cmp/eq #0,r0\n"
            "bf 1b\n"
            : [dst] "+r" (dst), [src] "+r" (src), [groups] "+r" (groups),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3)
            : [step] "r" (step)
            : "r0", "memory");
    }

    if (rem) {
        __asm__ volatile (
            "mov.b @%[src],%[c0]\n"
            "add %[step],%[src]\n"
            "mov.b @%[src],%[c1]\n"
            "shll8 %[c0]\n"
            "or %[c1],%[c0]\n"
            "mov.w %[c0],@%[dst]\n"
            : [dst] "+r" (dst), [src] "+r" (src),
              [c0] "=&r" (c0), [c1] "=&r" (c1)
            : [step] "r" (step)
            : "memory");
    }
}
#endif

#if CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_pair_pcfx_nowrap(uint16_t *dst, uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    const uint8_t *src = active_tex_lut + tex_state;
    int32_t step = (int32_t)tex_step_linear;
    uint16_t groups = (uint16_t)(pairs >> 1);
    uint16_t rem = (uint16_t)(pairs & 1u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            /* Two packed pixel-pairs per iteration.  This is intentionally
               compact instead of fully unrolled: the hot body is four byte LUT
               fetches, four source-step adds, two minimal packs without low-byte zero-extension, and two ST.H
               writes.  No per-pixel address rebuild and no KING reads. */
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c2]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c3]\n"
            "add %[step],%[src]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "st.h %[c0],0[%[dst]]\n"
            "shl 8,%[c2]\n"
            "or %[c3],%[c2]\n"
            "st.h %[c2],2[%[dst]]\n"
            "add 4,%[dst]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [dst] "+r" (dst), [src] "+r" (src), [groups] "+r" (groups),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3)
            : [step] "r" (step)
            : "memory");
    }

    if (rem) {
        __asm__ volatile (
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "st.h %[c0],0[%[dst]]\n"
            : [dst] "+r" (dst), [src] "+r" (src),
              [c0] "=&r" (c0), [c1] "=&r" (c1)
            : [step] "r" (step)
            : "memory");
    }
}
#endif

#if CFX_RENDERER_DIRECT_KRAM && CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_pair_pcfx_kram_nowrap(uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    uint32_t c4;
    uint32_t c5;
    uint32_t c6;
    uint32_t c7;
    const uint8_t *src = active_tex_lut + tex_state;
    int32_t step = (int32_t)tex_step_linear;
    uint16_t groups = (uint16_t)(pairs >> 2);
    uint16_t rem = (uint16_t)(pairs & 3u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            /* Eight texels -> four KING halfword stores.  The longer unroll
               reduces branch/control overhead and places all byte loads before
               byte packing, which gives the accurate V810 core more distance
               between loads and their consumers. */
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c2]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c3]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c4]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c5]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c6]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c7]\n"
            "add %[step],%[src]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "out.h %[c0],0x604[r0]\n"
            "shl 8,%[c2]\n"
            "or %[c3],%[c2]\n"
            "out.h %[c2],0x604[r0]\n"
            "shl 8,%[c4]\n"
            "or %[c5],%[c4]\n"
            "out.h %[c4],0x604[r0]\n"
            "shl 8,%[c6]\n"
            "or %[c7],%[c6]\n"
            "out.h %[c6],0x604[r0]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [src] "+r" (src), [groups] "+r" (groups),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3),
              [c4] "=&r" (c4), [c5] "=&r" (c5), [c6] "=&r" (c6), [c7] "=&r" (c7)
            : [step] "r" (step)
            : "memory");
    }

    while (rem-- > 0) {
        uint8_t a = *src;
        src += step;
        uint8_t b = *src;
        src += step;
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(a, b));
    }
}

static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_pair_pcfx_kram_wrap(uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear)
{
    uint32_t state = tex_state;
    uint32_t addr;
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    const uint8_t *lut = active_tex_lut;
    int32_t step = (int32_t)tex_step_linear;
    uint16_t groups = (uint16_t)(pairs >> 1);
    uint16_t rem = (uint16_t)(pairs & 1u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c0]\n"
            "add %[step],%[state]\n"
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c1]\n"
            "add %[step],%[state]\n"
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c2]\n"
            "add %[step],%[state]\n"
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c3]\n"
            "add %[step],%[state]\n"
            "andi 65535,%[state],%[state]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "out.h %[c0],0x604[r0]\n"
            "shl 8,%[c2]\n"
            "or %[c3],%[c2]\n"
            "out.h %[c2],0x604[r0]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [state] "+r" (state), [groups] "+r" (groups),
              [addr] "=&r" (addr), [c0] "=&r" (c0), [c1] "=&r" (c1),
              [c2] "=&r" (c2), [c3] "=&r" (c3)
            : [lut] "r" (lut), [step] "r" (step)
            : "memory");
    }

    if (rem) {
        __asm__ volatile (
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c0]\n"
            "add %[step],%[state]\n"
            "mov %[state],%[addr]\n"
            "andi 65535,%[addr],%[addr]\n"
            "add %[lut],%[addr]\n"
            "ld.b 0[%[addr]],%[c1]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "out.h %[c0],0x604[r0]\n"
            : [state] "+r" (state), [addr] "=&r" (addr),
              [c0] "=&r" (c0), [c1] "=&r" (c1)
            : [lut] "r" (lut), [step] "r" (step)
            : "memory");
    }

}
#endif

static inline __attribute__((always_inline)) uint8_t cfx_span_can_linear(uint16_t pairs, uint16_t tex_state, int8_t step_u, int8_t step_v)
{
    uint32_t pixels = (uint32_t)pairs << 1;
    if (pixels <= 1) {
        return 1;
    }

    int32_t last = (int32_t)pixels - 1;
    int32_t u0 = (uint8_t)tex_state;
    int32_t v0 = (uint8_t)(tex_state >> 8);
    int32_t u1 = u0 + ((int32_t)step_u * last);
    int32_t v1 = v0 + ((int32_t)step_v * last);

    return (u1 >= 0 && u1 <= 255 && v1 >= 0 && v1 <= 255);
}

static inline __attribute__((always_inline)) void cfx_draw_scanline_safe_pair(uint16_t *dst, uint16_t pairs, uint16_t tex_state, int8_t step_u, int8_t step_v)
{
    uint8_t u = (uint8_t)tex_state;
    uint8_t v = (uint8_t)(tex_state >> 8);

    while (pairs-- > 0) {
        uint8_t c0 = cfx_fetch_texel(cfx_pack_tex_state(u, v));
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        uint8_t c1 = cfx_fetch_texel(cfx_pack_tex_state(u, v));
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        *dst++ = cfx_pack_pixel_pair(c0, c1);
    }
}

static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_pair(uint16_t *dst, uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
#if CFX_RENDERER_USE_V810_ASM
    if (cfx_span_can_linear(pairs, tex_state, step_u, step_v)) {
        cfx_draw_scanline_fast_pair_pcfx_nowrap(dst, pairs, tex_state, tex_step_linear);
    } else {
        cfx_draw_scanline_safe_pair(dst, pairs, tex_state, step_u, step_v);
    }
#elif CFX_RENDERER_USE_SH1_ASM
    if (cfx_span_can_linear(pairs, tex_state, step_u, step_v)) {
        cfx_draw_scanline_fast_pair_sh1_nowrap(dst, pairs, tex_state, tex_step_linear);
    } else {
        cfx_draw_scanline_safe_pair(dst, pairs, tex_state, step_u, step_v);
    }
#else
    (void)tex_step_linear;
    cfx_draw_scanline_safe_pair(dst, pairs, tex_state, step_u, step_v);
#endif
}

#if CFX_RENDERER_HEADLESS_BYTES
static inline void cfx_draw_span_headless_bytes(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)skip);
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    uint8_t *dst = renderer->framebuffer + ((int32_t)y * renderer->width) + xs;

    if (cfx_span_can_linear((uint16_t)((span + 1) >> 1), tex_state, step_u, step_v)) {
        const uint8_t *src = active_tex_lut + tex_state;
        int16_t step = tex_step_linear;
        for (int16_t i = 0; i < span; ++i) {
            *dst++ = *src;
            src += step;
        }
    } else {
        uint8_t u = (uint8_t)tex_state;
        uint8_t v = (uint8_t)(tex_state >> 8);
        for (int16_t i = 0; i < span; ++i) {
            *dst++ = cfx_fetch_texel(cfx_pack_tex_state(u, v));
            u = (uint8_t)(u + (uint8_t)step_u);
            v = (uint8_t)(v + (uint8_t)step_v);
        }
    }
}
#endif

#if CFX_RENDERER_DIRECT_KRAM
static inline __attribute__((always_inline)) void cfx_draw_scanline_safe_pair_kram(uint16_t pairs, uint16_t tex_state, int8_t step_u, int8_t step_v)
{
    uint8_t u = (uint8_t)tex_state;
    uint8_t v = (uint8_t)(tex_state >> 8);

    while (pairs-- > 0) {
        uint8_t c0 = cfx_fetch_texel(cfx_pack_tex_state(u, v));
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        uint8_t c1 = cfx_fetch_texel(cfx_pack_tex_state(u, v));
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
    }
}

static inline __attribute__((always_inline)) void cfx_draw_scanline_fast_pair_kram(uint16_t pairs, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
#if CFX_RENDERER_USE_V810_ASM
    if (cfx_span_can_linear(pairs, tex_state, step_u, step_v)) {
        cfx_draw_scanline_fast_pair_pcfx_kram_nowrap(pairs, tex_state, tex_step_linear);
    } else {
        cfx_draw_scanline_fast_pair_pcfx_kram_wrap(pairs, tex_state, tex_step_linear);
    }
#else
    cfx_draw_scanline_safe_pair_kram(pairs, tex_state, step_u, step_v);
#endif
}

static inline void cfx_draw_span_kram(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)skip);
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    cfx_pcfx_kram_seek_frame_word(((int32_t)y * (renderer->width >> 1)) + (xs >> 1));

    if (xs & 1) {
        {
            uint16_t prev_state = cfx_advance_tex_state(tex_state, (int8_t)-step_u, (int8_t)-step_v);
            uint8_t prev = cfx_fetch_texel(prev_state);
            uint8_t edge = cfx_fetch_texel(tex_state);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(prev, edge));
        }
        tex_state = cfx_advance_tex_state(tex_state, step_u, step_v);
        --span;
    }

    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_scanline_fast_pair_kram(pairs, tex_state, tex_step_linear, step_u, step_v);
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)(pairs << 1));
        span = (int16_t)(span - (int16_t)(pairs << 1));
    }

    if (span > 0) {
        {
            uint8_t edge = cfx_fetch_texel(tex_state);
            uint8_t next = cfx_fetch_texel(cfx_advance_tex_state(tex_state, step_u, step_v));
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, next));
        }
    }
}

#if CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) void cfx_draw_span_kram_nowrap_unclipped(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
    /* PC-FX title cube fast path: caller has already verified screen clipping
       and LUT linearity.  Avoid the generic span entry cost and the second
       cfx_span_can_linear() multiply/check for every emitted scanline. */
    cfx_pcfx_kram_seek_frame_word(((int32_t)y * (renderer->width >> 1)) + (xs >> 1));

    if (xs & 1) {
        uint16_t prev_state = cfx_advance_tex_state(tex_state, (int8_t)-step_u, (int8_t)-step_v);
        uint8_t prev = cfx_fetch_texel(prev_state);
        uint8_t edge = cfx_fetch_texel(tex_state);
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(prev, edge));
        tex_state = cfx_advance_tex_state(tex_state, step_u, step_v);
        --span;
    }

    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_scanline_fast_pair_pcfx_kram_nowrap(pairs, tex_state, tex_step_linear);
        if (span & 1) {
            tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)(pairs << 1));
        }
        span = (int16_t)(span - (int16_t)(pairs << 1));
    }

    if (span > 0) {
        uint8_t edge = cfx_fetch_texel(tex_state);
        uint8_t next = cfx_fetch_texel(cfx_advance_tex_state(tex_state, step_u, step_v));
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, next));
    }
}

#endif


#if CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) uint8_t cfx_fp_span_in_lut_bounds(int32_t u_fp, int32_t v_fp,
                                                                              int32_t du_fp, int32_t dv_fp,
                                                                              int16_t span)
{
    int32_t last = (int32_t)span - 1;
    int32_t end_u = u_fp + du_fp * last;
    int32_t end_v = v_fp + dv_fp * last;
    return (uint8_t)(u_fp >= 0 && u_fp <= 65535 && v_fp >= 0 && v_fp <= 65535 &&
                     end_u >= 0 && end_u <= 65535 && end_v >= 0 && end_v <= 65535);
}

static inline __attribute__((always_inline)) void cfx_draw_scanline_kram_fp_exact_v810(uint16_t pairs,
                                                                                       int32_t *u_fp_io,
                                                                                       int32_t *v_fp_io,
                                                                                       int32_t du_fp,
                                                                                       int32_t dv_fp)
{
    int32_t u_fp = *u_fp_io;
    int32_t v_fp = *v_fp_io;
    const uint8_t *lut = active_tex_lut;
    uint32_t addr0;
    uint32_t addr1;
    uint32_t tmp0;
    uint32_t tmp1;
    uint32_t c0;
    uint32_t c1;

    if (pairs) {
        __asm__ volatile (
            "1:\n"
            "mov %[v],%[addr0]\n"
            "andi 65280,%[addr0],%[addr0]\n"
            "mov %[u],%[tmp0]\n"
            "shr 8,%[tmp0]\n"
            "andi 255,%[tmp0],%[tmp0]\n"
            "or %[tmp0],%[addr0]\n"
            "add %[lut],%[addr0]\n"
            "ld.b 0[%[addr0]],%[c0]\n"
            "add %[du],%[u]\n"
            "add %[dv],%[v]\n"
            "mov %[v],%[addr1]\n"
            "andi 65280,%[addr1],%[addr1]\n"
            "mov %[u],%[tmp1]\n"
            "shr 8,%[tmp1]\n"
            "andi 255,%[tmp1],%[tmp1]\n"
            "or %[tmp1],%[addr1]\n"
            "add %[lut],%[addr1]\n"
            "ld.b 0[%[addr1]],%[c1]\n"
            "add %[du],%[u]\n"
            "add %[dv],%[v]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "out.h %[c0],0x604[r0]\n"
            "add -1,%[pairs]\n"
            "bne 1b\n"
            : [u] "+r" (u_fp), [v] "+r" (v_fp), [pairs] "+r" (pairs),
              [addr0] "=&r" (addr0), [addr1] "=&r" (addr1),
              [tmp0] "=&r" (tmp0), [tmp1] "=&r" (tmp1),
              [c0] "=&r" (c0), [c1] "=&r" (c1)
            : [du] "r" (du_fp), [dv] "r" (dv_fp), [lut] "r" (lut)
            : "memory");
    }

    *u_fp_io = u_fp;
    *v_fp_io = v_fp;
}
#endif

static inline void cfx_draw_span_kram_fp_exact(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
                                               int32_t u_fp, int32_t v_fp, int32_t du_fp, int32_t dv_fp)
{
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        u_fp += du_fp * skip;
        v_fp += dv_fp * skip;
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    /* KING-only fixed-point span path.

       Do not read or update the CPU foreground mirror and do not stage a row.
       KING KRAM is a 16-bit port, so odd visible edges are emitted as complete
       halfwords.  For odd 16-bit KRAM edges, duplicate the edge texel into the
       non-visible byte instead of writing transparent index 0.  Writing 0 at
       these halfword boundaries can punch transient holes into adjacent KING
       texels when the span edge lands on an odd byte. */
#if defined(PLATFORM) && PLATFORM == NECPCFX
    const int32_t row_word = (int32_t)y << 7;
#else
    const int32_t row_word = (int32_t)y * (renderer->width >> 1);
#endif
    cfx_pcfx_kram_seek_frame_word(row_word + (xs >> 1));

    if (xs & 1) {
        uint8_t edge = cfx_fetch_texel_fp(u_fp, v_fp);
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        u_fp += du_fp;
        v_fp += dv_fp;
        --span;
    }

#if CFX_RENDERER_USE_V810_ASM
    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_scanline_kram_fp_exact_v810(pairs, &u_fp, &v_fp, du_fp, dv_fp);
        span = (int16_t)(span - (int16_t)(pairs << 1));
        if (span > 0) {
            uint8_t edge = cfx_fetch_texel_fp(u_fp, v_fp);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
        return;
    }
#endif

    while (span >= 2) {
        uint8_t c0 = cfx_fetch_texel_fp(u_fp, v_fp);
        u_fp += du_fp;
        v_fp += dv_fp;
        uint8_t c1 = cfx_fetch_texel_fp(u_fp, v_fp);
        u_fp += du_fp;
        v_fp += dv_fp;
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
        span = (int16_t)(span - 2);
    }

    if (span > 0) {
        uint8_t edge = cfx_fetch_texel_fp(u_fp, v_fp);
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
    }
}
#endif

#if CFX_RENDERER_MULTI_LUT
static inline void cfx_put_even_pixel_word_pcfx(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0x00ffu) | ((uint16_t)color << 8));
}

static inline void cfx_put_odd_pixel_word_pcfx(uint16_t *word, uint8_t color)
{
    *word = (uint16_t)((*word & 0xff00u) | (uint16_t)color);
}

static inline void cfx_draw_span_pcfx_exact_words(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)skip);
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    /* Keep the PC-FX/KING byte order fixed: even/left pixel in the high byte,
       odd/right pixel in the low byte.  The earlier all-pair expansion kept
       every write as a full 16-bit store, but it also drew one texel outside
       odd/even triangle edges.  At steep title-cube angles that outside texel
       could be sampled after a 16-bit UV wrap, giving the visible bottom-edge
       texture wrap at the 2400-frame title capture.

       This exact path preserves the fast LUT pair loop for the interior and
       uses at most two read/modify/write halfwords from the CPU-side staging
       framebuffer for the edge pixels.  It still never reads KING KRAM, and
       all stores remain halfword stores. */
    uint16_t *dst = (uint16_t *)renderer->framebuffer + ((int32_t)y * (renderer->width >> 1)) + (xs >> 1);

    if (xs & 1) {
        cfx_put_odd_pixel_word_pcfx(dst, cfx_fetch_texel(tex_state));
        tex_state = cfx_advance_tex_state(tex_state, step_u, step_v);
        ++dst;
        --span;
    }

    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_scanline_fast_pair(dst, pairs, tex_state, tex_step_linear, step_u, step_v);
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)(pairs << 1));
        dst += pairs;
        span = (int16_t)(span - (int16_t)(pairs << 1));
    }

    if (span > 0) {
        cfx_put_even_pixel_word_pcfx(dst, cfx_fetch_texel(tex_state));
    }
}
#endif

static inline void cfx_draw_span(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
{
#if CFX_RENDERER_DIRECT_KRAM
    if (cfx_renderer3d_direct_kram_active(renderer)) {
        cfx_draw_span_kram(renderer, y, xs, span, tex_state, tex_step_linear, step_u, step_v);
        return;
    }
#endif
#if CFX_RENDERER_MULTI_LUT
    cfx_draw_span_pcfx_exact_words(renderer, y, xs, span, tex_state, tex_step_linear, step_u, step_v);
    return;
#elif CFX_RENDERER_HEADLESS_BYTES
    cfx_draw_span_headless_bytes(renderer, y, xs, span, tex_state, tex_step_linear, step_u, step_v);
    return;
#endif
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)skip);
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    uint16_t *dst = (uint16_t *)renderer->framebuffer + ((int32_t)y * (renderer->width >> 1)) + (xs >> 1);

    if (xs & 1) {
        cfx_put_odd_pixel_word(dst, cfx_fetch_texel(tex_state));
        tex_state = cfx_advance_tex_state(tex_state, step_u, step_v);
        ++dst;
        --span;
    }

    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_scanline_fast_pair(dst, pairs, tex_state, tex_step_linear, step_u, step_v);
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)(pairs << 1));
        dst += pairs;
        span = (int16_t)(span - (int16_t)(pairs << 1));
    }

    if (span > 0) {
        cfx_put_even_pixel_word(dst, cfx_fetch_texel(tex_state));
    }
}



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

#if CFX_RENDERER_DIRECT_ROW_LUT
static inline __attribute__((always_inline)) uint8_t cfx_u_span_can_linear(uint16_t pixels, uint8_t u, int8_t step_u)
{
    if (pixels <= 1) return 1;
    int32_t last = (int32_t)pixels - 1;
    int32_t end = (int32_t)u + ((int32_t)step_u * last);
    return (uint8_t)(end >= 0 && end <= 255);
}

#if CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) void cfx_draw_row_lut_pair_pcfx_nowrap(uint16_t *dst, uint16_t pairs, const uint8_t *src, int16_t step)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    uint16_t groups = (uint16_t)(pairs >> 1);
    uint16_t rem = (uint16_t)(pairs & 1u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c2]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c3]\n"
            "add %[step],%[src]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "st.h %[c0],0[%[dst]]\n"
            "shl 8,%[c2]\n"
            "or %[c3],%[c2]\n"
            "st.h %[c2],2[%[dst]]\n"
            "add 4,%[dst]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [dst] "+r" (dst), [src] "+r" (src), [groups] "+r" (groups),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3)
            : [step] "r" ((int32_t)step)
            : "memory");
    }

    if (rem) {
        __asm__ volatile (
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "st.h %[c0],0[%[dst]]\n"
            : [dst] "+r" (dst), [src] "+r" (src),
              [c0] "=&r" (c0), [c1] "=&r" (c1)
            : [step] "r" ((int32_t)step)
            : "memory");
    }
}
#endif

#if CFX_RENDERER_DIRECT_KRAM && CFX_RENDERER_USE_V810_ASM
static inline __attribute__((always_inline)) void cfx_draw_row_lut_pair_pcfx_kram_nowrap(uint16_t pairs, const uint8_t *src, int16_t step)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t c2;
    uint32_t c3;
    uint32_t c4;
    uint32_t c5;
    uint32_t c6;
    uint32_t c7;
    uint16_t groups = (uint16_t)(pairs >> 2);
    uint16_t rem = (uint16_t)(pairs & 3u);

    if (groups) {
        __asm__ volatile (
            "1:\n"
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c2]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c3]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c4]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c5]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c6]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c7]\n"
            "add %[step],%[src]\n"
            "shl 8,%[c0]\n"
            "or %[c1],%[c0]\n"
            "out.h %[c0],0x604[r0]\n"
            "shl 8,%[c2]\n"
            "or %[c3],%[c2]\n"
            "out.h %[c2],0x604[r0]\n"
            "shl 8,%[c4]\n"
            "or %[c5],%[c4]\n"
            "out.h %[c4],0x604[r0]\n"
            "shl 8,%[c6]\n"
            "or %[c7],%[c6]\n"
            "out.h %[c6],0x604[r0]\n"
            "add -1,%[groups]\n"
            "bne 1b\n"
            : [src] "+r" (src), [groups] "+r" (groups),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3),
              [c4] "=&r" (c4), [c5] "=&r" (c5), [c6] "=&r" (c6), [c7] "=&r" (c7)
            : [step] "r" ((int32_t)step)
            : "memory");
    }

    while (rem-- > 0) {
        uint8_t a = *src;
        src += step;
        uint8_t b = *src;
        src += step;
        cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(a, b));
    }
}

#endif

static inline __attribute__((always_inline)) void cfx_draw_span_direct_tile_row_lut(
    uint16_t *dst, int16_t xs, int16_t span, const uint8_t *row, uint8_t u, int8_t step_u
#if CFX_RENDERER_DIRECT_KRAM
    , uint8_t direct_kram
#endif
)
{
#if CFX_RENDERER_DIRECT_KRAM
    if (direct_kram) {
        if (xs & 1) {
            {
                uint8_t edge = row[u];
                cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
            }
            u = (uint8_t)(u + (uint8_t)step_u);
            --span;
        }
        if (span >= 2) {
            uint16_t pairs = (uint16_t)(span >> 1);
#if CFX_RENDERER_USE_V810_ASM
            if (cfx_u_span_can_linear((uint16_t)(pairs << 1), u, step_u)) {
                cfx_draw_row_lut_pair_pcfx_kram_nowrap(pairs, row + u, (int16_t)step_u);
                u = (uint8_t)(u + (uint8_t)((int16_t)step_u * (int16_t)(pairs << 1)));
                span = (int16_t)(span - (int16_t)(pairs << 1));
            } else
#endif
            {
                while (pairs-- > 0) {
                    uint8_t c0 = row[u];
                    u = (uint8_t)(u + (uint8_t)step_u);
                    uint8_t c1 = row[u];
                    u = (uint8_t)(u + (uint8_t)step_u);
                    cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
                    span = (int16_t)(span - 2);
                }
            }
        }
        if (span > 0) {
            {
            uint8_t edge = row[u];
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
        }
        return;
    }
#endif
    if (xs & 1) {
        cfx_put_odd_pixel_word(dst, row[u]);
        u = (uint8_t)(u + (uint8_t)step_u);
        ++dst;
        --span;
    }

    if (span >= 2) {
        uint16_t pairs = (uint16_t)(span >> 1);
#if CFX_RENDERER_USE_V810_ASM
        if (cfx_u_span_can_linear((uint16_t)(pairs << 1), u, step_u)) {
            cfx_draw_row_lut_pair_pcfx_nowrap(dst, pairs, row + u, (int16_t)step_u);
            u = (uint8_t)(u + (uint8_t)((int16_t)step_u * (int16_t)(pairs << 1)));
            dst += pairs;
            span = (int16_t)(span - (int16_t)(pairs << 1));
        } else
#endif
        {
            while (pairs-- > 0) {
                uint8_t c0 = row[u];
                u = (uint8_t)(u + (uint8_t)step_u);
                uint8_t c1 = row[u];
                u = (uint8_t)(u + (uint8_t)step_u);
                *dst++ = cfx_pack_pixel_pair(c0, c1);
                span = (int16_t)(span - 2);
            }
        }
    }

    if (span > 0) {
        cfx_put_even_pixel_word(dst, row[u]);
    }
}
#endif

static inline __attribute__((always_inline)) void cfx_draw_span_direct_tile_row(
    const CfxRenderer3DState *renderer, const uint8_t *tile, int16_t y, int16_t xs, int16_t span,
    uint16_t tex_state, int8_t step_u)
{
    uint8_t u = (uint8_t)tex_state;
    const uint8_t v = (uint8_t)(tex_state >> 8);
    const uint8_t *row = tile + (((uint16_t)(v >> CFX_FIXED_POINT_SHIFT) & CFX_TEX_MASK) *
                                 (uint16_t)renderer->tile_pitch_bytes);
    uint16_t *dst = (uint16_t *)renderer->framebuffer + ((int32_t)y * (renderer->width >> 1)) + (xs >> 1);
#if CFX_RENDERER_DIRECT_KRAM
    uint8_t direct_kram = cfx_renderer3d_direct_kram_active(renderer);
    if (direct_kram) {
        cfx_pcfx_kram_seek_frame_word(((int32_t)y * (renderer->width >> 1)) + (xs >> 1));
    }
#endif

#if CFX_RENDERER_DIRECT_ROW_LUT
    const uint8_t *row_lut = cfx_renderer3d_direct_row_lut(renderer, tile, v);
    if (row_lut) {
        cfx_draw_span_direct_tile_row_lut(dst, xs, span, row_lut, u, step_u
#if CFX_RENDERER_DIRECT_KRAM
            , direct_kram
#endif
        );
        return;
    }
#endif

#if CFX_RENDERER_DIRECT_KRAM
    if (direct_kram) {
        if (xs & 1) {
            {
            uint8_t edge = cfx_fetch_direct_row_texel(row, u);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
            u = (uint8_t)(u + (uint8_t)step_u);
            --span;
        }
        while (span >= 4) {
            const uint8_t c0 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            const uint8_t c1 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            const uint8_t c2 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            const uint8_t c3 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c2, c3));
            span = (int16_t)(span - 4);
        }
        if (span >= 2) {
            const uint8_t c0 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            const uint8_t c1 = cfx_fetch_direct_row_texel(row, u);
            u = (uint8_t)(u + (uint8_t)step_u);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
            span = (int16_t)(span - 2);
        }
        if (span > 0) {
            {
            uint8_t edge = cfx_fetch_direct_row_texel(row, u);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
        }
        return;
    }
#endif

    if (xs & 1) {
        cfx_put_odd_pixel_word(dst, cfx_fetch_direct_row_texel(row, u));
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) cfx_pcfx_kram_write_word(*dst);
#endif
        u = (uint8_t)(u + (uint8_t)step_u);
        ++dst;
        --span;
    }

    while (span >= 4) {
        const uint8_t c0 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        const uint8_t c1 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        const uint8_t c2 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        const uint8_t c3 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        dst[0] = cfx_pack_pixel_pair(c0, c1);
        dst[1] = cfx_pack_pixel_pair(c2, c3);
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) {
            cfx_pcfx_kram_write_word(dst[0]);
            cfx_pcfx_kram_write_word(dst[1]);
        }
#endif
        dst += 2;
        span = (int16_t)(span - 4);
    }

    if (span >= 2) {
        const uint8_t c0 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        const uint8_t c1 = cfx_fetch_direct_row_texel(row, u);
        u = (uint8_t)(u + (uint8_t)step_u);
        {
            uint16_t packed = cfx_pack_pixel_pair(c0, c1);
            *dst++ = packed;
#if CFX_RENDERER_DIRECT_KRAM
            if (direct_kram) cfx_pcfx_kram_write_word(packed);
#endif
        }
        span = (int16_t)(span - 2);
    }

    if (span > 0) {
        cfx_put_even_pixel_word(dst, cfx_fetch_direct_row_texel(row, u));
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) cfx_pcfx_kram_write_word(*dst);
#endif
    }
}

#if CFX_RENDERER_USE_V810_ASM && CFX_RENDERER_DIRECT_RECT
/* Compact affine direct-tile inner loop for the perspective board floor
   (32x32 source, pitch 32).  This is the hot per-pixel path: the previous C
   loop recomputed a full 2-D texel address per pixel and, together with the
   branch-heavy span/triangle code around it, overran the 1 KiB V810 I-cache.
   Here the whole pixel-pair body is a handful of fixed instructions with no
   branches, no KRAM probe and no LUT, so it stays resident in I-cache.

   Pixel-identical to cfx_fetch_direct_texel(): texel offset is
   ((v>>3)&31)*32 + ((u>>3)&31), expressed as ((v<<2)&0x3e0) | ((u>>3)&0x1f).
   u/v run as 32-bit accumulators; the &0x3e0 / &0x1f masks make the high bits
   irrelevant, so no per-step 8-bit wrap is needed.  Two pixels per ST.H, left
   pixel in the high byte to match the KING byte order. */
static inline __attribute__((always_inline)) void cfx_draw_board_pair_nowrap(
    uint16_t *dst, uint16_t pairs, uint8_t *up, uint8_t *vp,
    int8_t step_u, int8_t step_v, const uint8_t *tile)
{
    uint32_t u = *up;
    uint32_t v = *vp;
    uint32_t su = (uint32_t)(int32_t)step_u;
    uint32_t sv = (uint32_t)(int32_t)step_v;
    uint32_t c0, c1, ru, rv;
    if (pairs) {
        __asm__ volatile (
            "1:\n"
            "mov %[v],%[rv]\n" "shl 2,%[rv]\n" "andi 0x3e0,%[rv],%[rv]\n"
            "mov %[u],%[ru]\n" "shr 3,%[ru]\n" "andi 0x1f,%[ru],%[ru]\n"
            "add %[rv],%[ru]\n" "add %[tile],%[ru]\n" "ld.b 0[%[ru]],%[c0]\n"
            "add %[su],%[u]\n" "add %[sv],%[v]\n"
            "mov %[v],%[rv]\n" "shl 2,%[rv]\n" "andi 0x3e0,%[rv],%[rv]\n"
            "mov %[u],%[ru]\n" "shr 3,%[ru]\n" "andi 0x1f,%[ru],%[ru]\n"
            "add %[rv],%[ru]\n" "add %[tile],%[ru]\n" "ld.b 0[%[ru]],%[c1]\n"
            "add %[su],%[u]\n" "add %[sv],%[v]\n"
            "shl 8,%[c0]\n" "or %[c1],%[c0]\n" "st.h %[c0],0[%[dst]]\n"
            "add 2,%[dst]\n" "add -1,%[pairs]\n" "bne 1b\n"
            : [dst] "+r" (dst), [pairs] "+r" (pairs), [u] "+r" (u), [v] "+r" (v),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [ru] "=&r" (ru), [rv] "=&r" (rv)
            : [su] "r" (su), [sv] "r" (sv), [tile] "r" (tile)
            : "memory");
    }
    *up = (uint8_t)u;
    *vp = (uint8_t)v;
}
#endif

static inline void cfx_draw_span_direct_tile(const CfxRenderer3DState *renderer, const uint8_t *tile,
                                             int16_t y, int16_t xs, int16_t span,
                                             uint16_t tex_state, int8_t step_u, int8_t step_v)
{
    if (span <= 0 || y < 0 || y >= renderer->height) {
        return;
    }

    if (xs < 0) {
        int16_t skip = (int16_t)-xs;
        if (skip >= span) {
            return;
        }
        tex_state = cfx_advance_tex_state_n(tex_state, step_u, step_v, (uint16_t)skip);
        span = (int16_t)(span - skip);
        xs = 0;
    }

    if (xs >= renderer->width) {
        return;
    }

    if ((int32_t)xs + span > renderer->width) {
        span = (int16_t)(renderer->width - xs);
    }

    if (span <= 0) {
        return;
    }

    /* Axis-aligned block faces have a constant V across each row.  Keep this
       path separate so the PC-FX/V810 hot loop only shifts U, loads from a
       single 16-byte source row, packs two pixels, and stores one KING word.
       The old direct path recomputed the 2-D texel address for every pixel. */
    if (step_v == 0) {
        cfx_draw_span_direct_tile_row(renderer, tile, y, xs, span, tex_state, step_u);
        return;
    }

    uint8_t u = (uint8_t)tex_state;
    uint8_t v = (uint8_t)(tex_state >> 8);
    uint16_t *dst = (uint16_t *)renderer->framebuffer + ((int32_t)y * (renderer->width >> 1)) + (xs >> 1);
#if CFX_RENDERER_DIRECT_KRAM
    uint8_t direct_kram = cfx_renderer3d_direct_kram_active(renderer);
    if (direct_kram) {
        cfx_pcfx_kram_seek_frame_word(((int32_t)y * (renderer->width >> 1)) + (xs >> 1));
    }
#endif

#if CFX_RENDERER_DIRECT_KRAM
    if (direct_kram) {
        if (xs & 1) {
            {
            uint8_t edge = cfx_fetch_direct_texel(renderer, tile, u, v);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
            u = (uint8_t)(u + (uint8_t)step_u);
            v = (uint8_t)(v + (uint8_t)step_v);
            --span;
        }
        while (span >= 2) {
            uint8_t c0 = cfx_fetch_direct_texel(renderer, tile, u, v);
            u = (uint8_t)(u + (uint8_t)step_u);
            v = (uint8_t)(v + (uint8_t)step_v);
            uint8_t c1 = cfx_fetch_direct_texel(renderer, tile, u, v);
            u = (uint8_t)(u + (uint8_t)step_u);
            v = (uint8_t)(v + (uint8_t)step_v);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(c0, c1));
            span = (int16_t)(span - 2);
        }
        if (span > 0) {
            {
            uint8_t edge = cfx_fetch_direct_texel(renderer, tile, u, v);
            cfx_pcfx_kram_write_word(cfx_pack_pixel_pair(edge, edge));
        }
        }
        return;
    }
#endif

    if (xs & 1) {
        cfx_put_odd_pixel_word(dst, cfx_fetch_direct_texel(renderer, tile, u, v));
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) cfx_pcfx_kram_write_word(*dst);
#endif
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        ++dst;
        --span;
    }

#if CFX_RENDERER_USE_V810_ASM && CFX_RENDERER_DIRECT_RECT
    /* Fast affine inner loop for the perspective board floor.  Only the 32x32,
       pitch-32 tiles used by the board match the hardcoded shift layout; any
       other geometry falls through to the portable C loop below. */
    if (span >= 2 && CFX_TEX_SIZE == 32 && renderer->tile_pitch_bytes == 32) {
        uint16_t pairs = (uint16_t)(span >> 1);
        cfx_draw_board_pair_nowrap(dst, pairs, &u, &v, step_u, step_v, tile);
        dst += pairs;
        span = (int16_t)(span - (int16_t)(pairs << 1));
    } else
#endif
    while (span >= 2) {
        uint8_t c0 = cfx_fetch_direct_texel(renderer, tile, u, v);
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        uint8_t c1 = cfx_fetch_direct_texel(renderer, tile, u, v);
        u = (uint8_t)(u + (uint8_t)step_u);
        v = (uint8_t)(v + (uint8_t)step_v);
        {
            uint16_t packed = cfx_pack_pixel_pair(c0, c1);
            *dst++ = packed;
#if CFX_RENDERER_DIRECT_KRAM
            if (direct_kram) cfx_pcfx_kram_write_word(packed);
#endif
        }
        span = (int16_t)(span - 2);
    }

    if (span > 0) {
        cfx_put_even_pixel_word(dst, cfx_fetch_direct_texel(renderer, tile, u, v));
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) cfx_pcfx_kram_write_word(*dst);
#endif
    }
}

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

#if CFX_RENDERER_DIRECT_KRAM
    const uint8_t direct_kram = cfx_renderer3d_direct_kram_active(renderer);
#else
    const uint8_t direct_kram = 0;
#endif


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
#if CFX_RENDERER_DIRECT_KRAM
        if (direct_kram) {
            cfx_draw_span_kram_fp_exact(renderer, y, x_start, span, u_start, v_start, du_fp, dv_fp);
        } else
#endif
        {
            int8_t du = (int8_t)(du_fp >> CFX_QUAD_UV_SHIFT);
            int8_t dv = (int8_t)(dv_fp >> CFX_QUAD_UV_SHIFT);
            uint16_t tex_state = cfx_pack_tex_state(cfx_clamp_u8_i32(u_start >> CFX_QUAD_UV_SHIFT),
                                                    cfx_clamp_u8_i32(v_start >> CFX_QUAD_UV_SHIFT));
            int16_t tex_step_linear = cfx_pack_tex_step_linear(du, dv);
            cfx_draw_span(renderer, y, x_start, span, tex_state, tex_step_linear, du, dv);
        }
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
        cfx_draw_span_direct_tile(renderer, tile, y, x_start, span, tex_state, step_u, step_v);
    }
    return 1;
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
    return cfx_draw_textured_quad_fast_affine_direct(state, tile, v0, v1, v2, v3);
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

static inline __attribute__((always_inline)) void cfx_board_fill(uint8_t *dst, int n,
        int32_t u, int32_t v, int32_t du, int32_t dv, const uint8_t *tile)
{
    if (n <= 0) return;
#if CFX_RENDERER_USE_V810_ASM
    {
        uint32_t c0, ru, rv;
        __asm__ volatile (
            "1:\n"
            "mov %[v],%[rv]\n" "shr 3,%[rv]\n" "andi 0x3e0,%[rv],%[rv]\n"
            "mov %[u],%[ru]\n" "shr 8,%[ru]\n" "andi 0x1f,%[ru],%[ru]\n"
            "add %[rv],%[ru]\n" "add %[tile],%[ru]\n" "ld.b 0[%[ru]],%[c0]\n"
            "st.b %[c0],0[%[dst]]\n"
            "add %[du],%[u]\n" "add %[dv],%[v]\n"
            "add 1,%[dst]\n" "add -1,%[n]\n" "bne 1b\n"
            : [dst] "+r" (dst), [n] "+r" (n), [u] "+r" (u), [v] "+r" (v),
              [c0] "=&r" (c0), [ru] "=&r" (ru), [rv] "=&r" (rv)
            : [du] "r" (du), [dv] "r" (dv), [tile] "r" (tile)
            : "memory");
    }
#else
    while (n-- > 0) {
        *dst++ = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
        u += du; v += dv;
    }
#endif
}

static void cfx_board_tri(uint8_t *fb, int W, int H, const uint8_t *tile,
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
            for (; y < yend; ++y) {
                if (y >= H) return;            /* below screen: nothing left to draw */
                if (y >= 0) {
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
                        cfx_board_fill(fb + ((W == 256) ? ((int32_t)y << 8) : (int32_t)y * W) + left,
                                       right - left + 1, U, V, gUx, gVx, tile);
                    }
                }
                xl += dxl; ul += dul; vl += dvl;
                xs += dxs; us += dus; vs += dvs;
            }
        }
    }
}
#endif

void cfx_renderer3d_draw_quad_board(CfxRenderer3D *renderer, const Point2D *p0, const Point2D *p1, const Point2D *p2, const Point2D *p3, DEFAULT_INT tetromino_type)
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

    {
        const uint8_t *tile = state->texture_atlas + ((int32_t)tetromino_type * state->tile_stride_bytes);
        uint8_t *fb = state->framebuffer;
        int W = (int)state->width, H = (int)state->height;
        cfx_board_tri(fb, W, H, tile,
                      (int)p0->x, (int)p0->y, (int32_t)p0->u, (int32_t)p0->v,
                      (int)p1->x, (int)p1->y, (int32_t)p1->u, (int32_t)p1->v,
                      (int)p2->x, (int)p2->y, (int32_t)p2->u, (int32_t)p2->v);
        cfx_board_tri(fb, W, H, tile,
                      (int)p0->x, (int)p0->y, (int32_t)p0->u, (int32_t)p0->v,
                      (int)p2->x, (int)p2->y, (int32_t)p2->u, (int32_t)p2->v,
                      (int)p3->x, (int)p3->y, (int32_t)p3->u, (int32_t)p3->v);
    }
#else
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
#if CFX_RENDERER_DIRECT_KRAM
    if (!cfx_renderer3d_direct_kram_active(state))
#endif
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
    cfx_renderer3d_build_lut_for_tile(state, tile);
#else
    (void)tile;
#endif

#if CFX_RENDERER_QUAD_SCANLINE && CFX_RENDERER_DIRECT_KRAM
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
