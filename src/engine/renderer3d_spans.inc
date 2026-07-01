/* Span/scanline fillers for the 3D renderer (per-pixel hot paths).
   Compiled once per build into the active backend TU (renderer3d_generic.c
   or renderer3d_pcfx.c); renderer3d_port.h selects portable C vs PC-FX V810
   paths. renderer3d.c (shared geometry/edge walker/entry core) calls these
   across the TU once per scanline row, via renderer3d_internal.h. */

#if CFX_RENDERER_DIRECT_KRAM
extern uint8_t *cfx_game_framebuffer(void);
extern int cfx_pcfx_current_page_word_offset(void);
extern int cfx_pcfx_current_kram_page_word_offset;
extern void eris_king_set_kram_write(uint32_t addr, int incr);
#endif

/* Whether this render targets the live direct-VRAM (KRAM) page. The shared core
   calls this to pick the direct-VRAM rasterization path; it is always defined
   (0 on backends without a direct-VRAM path) so the core needs no DIRECT_KRAM
   ifdefs. */
uint8_t cfx_renderer3d_direct_kram_active(const CfxRenderer3DState *renderer)
{
#if CFX_RENDERER_DIRECT_KRAM
    return (uint8_t)(renderer->framebuffer == cfx_game_framebuffer());
#else
    (void)renderer;
    return 0;
#endif
}

#if CFX_RENDERER_DIRECT_KRAM
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
            "extu.b %[c0],%[c0]\n"
            "extu.b %[c1],%[c1]\n"
            "extu.b %[c2],%[c2]\n"
            "extu.b %[c3],%[c3]\n"
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
            "extu.b %[c0],%[c0]\n"
            "extu.b %[c1],%[c1]\n"
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

void cfx_draw_span_kram_fp_exact(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span,
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

void cfx_draw_span(const CfxRenderer3DState *renderer, int16_t y, int16_t xs, int16_t span, uint16_t tex_state, int16_t tex_step_linear, int8_t step_u, int8_t step_v)
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


void cfx_board_fill(uint8_t *dst, int n,
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
#elif CFX_RENDERER_USE_SH1_ASM
    {
        uint32_t mask_u = 0x1fu;
        uint32_t mask_v = 0x3e0u;
        if (((uintptr_t)dst & 1u) != 0u) {
            *dst++ = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
            u += du;
            v += dv;
            --n;
        }

        if (n >= 2) {
            uint32_t c0, c1, c2, c3, ru, rv;
            uint16_t *dst16 = (uint16_t *)dst;
            int pairs = n >> 1;
            int pair_groups = pairs >> 1;
            int pair_group_count = pair_groups;

            if (pair_groups > 0) {
                __asm__ volatile (
                    "1:\n"
                    "mov %[v],%[rv]\n"
                    "shlr2 %[rv]\n"
                    "shlr %[rv]\n"
                    "and %[mask_v],%[rv]\n"
                    "mov %[u],%[ru]\n"
                    "shlr8 %[ru]\n"
                    "and %[mask_u],%[ru]\n"
                    "add %[rv],%[ru]\n"
                    "add %[tile],%[ru]\n"
                    "mov.b @%[ru],%[c0]\n"
                    "add %[du],%[u]\n"
                    "add %[dv],%[v]\n"

                    "mov %[v],%[rv]\n"
                    "shlr2 %[rv]\n"
                    "shlr %[rv]\n"
                    "and %[mask_v],%[rv]\n"
                    "mov %[u],%[ru]\n"
                    "shlr8 %[ru]\n"
                    "and %[mask_u],%[ru]\n"
                    "add %[rv],%[ru]\n"
                    "add %[tile],%[ru]\n"
                    "mov.b @%[ru],%[c1]\n"
                    "add %[du],%[u]\n"
                    "add %[dv],%[v]\n"

                    "mov %[v],%[rv]\n"
                    "shlr2 %[rv]\n"
                    "shlr %[rv]\n"
                    "and %[mask_v],%[rv]\n"
                    "mov %[u],%[ru]\n"
                    "shlr8 %[ru]\n"
                    "and %[mask_u],%[ru]\n"
                    "add %[rv],%[ru]\n"
                    "add %[tile],%[ru]\n"
                    "mov.b @%[ru],%[c2]\n"
                    "add %[du],%[u]\n"
                    "add %[dv],%[v]\n"

                    "mov %[v],%[rv]\n"
                    "shlr2 %[rv]\n"
                    "shlr %[rv]\n"
                    "and %[mask_v],%[rv]\n"
                    "mov %[u],%[ru]\n"
                    "shlr8 %[ru]\n"
                    "and %[mask_u],%[ru]\n"
                    "add %[rv],%[ru]\n"
                    "add %[tile],%[ru]\n"
                    "mov.b @%[ru],%[c3]\n"
                    "add %[du],%[u]\n"
                    "add %[dv],%[v]\n"

                    "extu.b %[c0],%[c0]\n"
                    "extu.b %[c1],%[c1]\n"
                    "extu.b %[c2],%[c2]\n"
                    "extu.b %[c3],%[c3]\n"
                    "shll8 %[c0]\n"
                    "or %[c1],%[c0]\n"
                    "mov.w %[c0],@%[dst]\n"
                    "add #2,%[dst]\n"
                    "shll8 %[c2]\n"
                    "or %[c3],%[c2]\n"
                    "mov.w %[c2],@%[dst]\n"
                    "add #2,%[dst]\n"
                    "add #-1,%[pairs]\n"
                    "tst %[pairs],%[pairs]\n"
                    "bf 1b\n"
                    : [dst] "+r" (dst16), [pairs] "+r" (pair_groups), [u] "+r" (u), [v] "+r" (v),
                      [c0] "=&r" (c0), [c1] "=&r" (c1), [c2] "=&r" (c2), [c3] "=&r" (c3),
                      [ru] "=&r" (ru), [rv] "=&r" (rv)
                    : [du] "r" (du), [dv] "r" (dv), [tile] "r" (tile),
                      [mask_u] "r" (mask_u), [mask_v] "r" (mask_v)
                    : "memory");

                n -= pair_group_count << 2;
            }

            if (pairs & 1) {
                uint8_t p0 = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
                u += du;
                v += dv;
                uint8_t p1 = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
                u += du;
                v += dv;
                *dst16++ = cfx_pack_pixel_pair(p0, p1);
                n -= 2;
            }

            dst = (uint8_t *)dst16;
        }

        if (n > 0) {
            *dst = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
        }
    }
#else
    while (n-- > 0) {
        *dst++ = tile[(((uint32_t)v >> 3) & 0x3e0) | (((uint32_t)u >> 8) & 0x1f)];
        u += du; v += dv;
    }
#endif
}

/* Direct-tile (axis-aligned) span fillers + row-LUT helpers. */
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

#if (CFX_RENDERER_USE_V810_ASM || CFX_RENDERER_USE_SH1_ASM) && CFX_RENDERER_DIRECT_RECT
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
#if CFX_RENDERER_USE_V810_ASM
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
#elif CFX_RENDERER_USE_SH1_ASM
        uint32_t mask_u = 0x1fu;
        uint32_t mask_v = 0x3e0u;
        __asm__ volatile (
            "1:\n"
            "mov %[v],%[rv]\n"
            "shll2 %[rv]\n"
            "and %[mask_v],%[rv]\n"
            "mov %[u],%[ru]\n"
            "shlr2 %[ru]\n"
            "shlr %[ru]\n"
            "and %[mask_u],%[ru]\n"
            "add %[rv],%[ru]\n"
            "add %[tile],%[ru]\n"
            "mov.b @%[ru],%[c0]\n"
            "add %[su],%[u]\n"
            "add %[sv],%[v]\n"
            "mov %[v],%[rv]\n"
            "shll2 %[rv]\n"
            "and %[mask_v],%[rv]\n"
            "mov %[u],%[ru]\n"
            "shlr2 %[ru]\n"
            "shlr %[ru]\n"
            "and %[mask_u],%[ru]\n"
            "add %[rv],%[ru]\n"
            "add %[tile],%[ru]\n"
            "mov.b @%[ru],%[c1]\n"
            "add %[su],%[u]\n"
            "add %[sv],%[v]\n"
            "extu.b %[c0],%[c0]\n"
            "extu.b %[c1],%[c1]\n"
            "shll8 %[c0]\n"
            "or %[c1],%[c0]\n"
            "mov.w %[c0],@%[dst]\n"
            "add #2,%[dst]\n"
            "add #-1,%[pairs]\n"
            "tst %[pairs],%[pairs]\n"
            "bf 1b\n"
            : [dst] "+r" (dst), [pairs] "+r" (pairs), [u] "+r" (u), [v] "+r" (v),
              [c0] "=&r" (c0), [c1] "=&r" (c1), [ru] "=&r" (ru), [rv] "=&r" (rv)
            : [su] "r" (su), [sv] "r" (sv), [tile] "r" (tile),
              [mask_u] "r" (mask_u), [mask_v] "r" (mask_v)
            : "memory");
#endif
    }
    *up = (uint8_t)u;
    *vp = (uint8_t)v;
}
#endif

void cfx_draw_span_direct_tile(const CfxRenderer3DState *renderer, const uint8_t *tile,
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

#if (CFX_RENDERER_USE_V810_ASM || CFX_RENDERER_USE_SH1_ASM) && CFX_RENDERER_DIRECT_RECT
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

/* Per-row emit for the textured-quad scanline walker: chooses the direct-VRAM
   exact-FP path on platforms with a live KRAM page, else the normal LUT span.
   Keeps the DIRECT_KRAM choice out of the shared core (renderer3d.c). */
void cfx_draw_span_quad_fp(const CfxRenderer3DState *renderer, int16_t y, int16_t x_start, int16_t span,
                           int32_t u_start, int32_t v_start, int32_t du_fp, int32_t dv_fp)
{
#if CFX_RENDERER_DIRECT_KRAM
    if (cfx_renderer3d_direct_kram_active(renderer)) {
        cfx_draw_span_kram_fp_exact(renderer, y, x_start, span, u_start, v_start, du_fp, dv_fp);
        return;
    }
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
