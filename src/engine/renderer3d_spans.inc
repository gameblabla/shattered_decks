/* Span/scanline fillers for the 3D renderer (per-pixel hot paths).
   Compiled once per build into the active backend TU (renderer3d_generic.c
   or renderer3d_pcfx.c); renderer3d_port.h selects portable C vs PC-FX V810
   paths. renderer3d.c (shared geometry/edge walker/entry core) calls these
   across the TU once per scanline row, via renderer3d_internal.h. */

#if CFX_RENDERER_DIRECT_KRAM
extern uint8_t *cfx_game_framebuffer(void);
extern int cfx_pcfx_current_page_word_offset(void);
extern int cfx_pcfx_current_kram_page_word_offset;
extern void king_set_kram_write(uint32_t addr, int incr);
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
    /* Inline king_set_kram_write(addr, 1) plus select KRAM data register.
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
               fetches, four source-step adds, two packs, and two ST.H writes.
               V810 LD.B sign-extends: masking each low/right lane keeps palette
               indices >= 128 from overwriting its pair's high/left lane.  No
               per-pixel address rebuild and no KING reads. */
            "ld.b 0[%[src]],%[c0]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c1]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c2]\n"
            "add %[step],%[src]\n"
            "ld.b 0[%[src]],%[c3]\n"
            "add %[step],%[src]\n"
            "andi 255,%[c1],%[c1]\n"
            "andi 255,%[c3],%[c3]\n"
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
            "andi 255,%[c1],%[c1]\n"
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
            "andi 255,%[c1],%[c1]\n"
            "andi 255,%[c3],%[c3]\n"
            "andi 255,%[c5],%[c5]\n"
            "andi 255,%[c7],%[c7]\n"
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
            "andi 255,%[c1],%[c1]\n"
            "andi 255,%[c3],%[c3]\n"
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
            "andi 255,%[c1],%[c1]\n"
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

#if CFX_RENDERER_USE_I386_ASM
#if CFX_TEX_SIZE != 32
/* The shift counts below (27 for a texel column, 3/0x3e0 for a texel row)
   are the 32x32 pitch-32 tile layout written out as constants; they have no
   way to notice a different tile size, so make it a build error instead. */
#error "i386 span fillers assume a 32x32 texture tile"
#endif
/* ---- i386 span fillers ---------------------------------------------------
 *
 * Three loops share one shape, so it is described once here.
 *
 * Each can be switched back to its portable C original with
 * -DCFX_MEASURE_C_ROW, -DCFX_MEASURE_C_AFFINE or -DCFX_MEASURE_C_BOARD_FILL
 * (and cfx_board_fill can be stubbed out entirely with
 * -DCFX_MEASURE_SKIP_BOARD_FILL, every textured span with
 * -DCFX_MEASURE_SKIP_SPANS -- which answers "how much of this frame is the
 * rasterizer at all", the question that decides whether to keep tuning the
 * inner loops or to draw fewer pixels).  They are here because attributing a
 * measurement on this target is genuinely hard -- the emulator reports one
 * number for the whole game step -- and bisecting with these is what
 * established that the top-down board view goes through the constant-V row
 * filler and its packed cfx_board_fill path.  Pass them through
 * EXTRA_CORE_DEFINES; nothing defines them in a normal build.
 *
 * THE ACCUMULATOR IS PRE-SHIFTED so the texture coordinate's integer part
 * ends up in the TOP bits of a 32-bit register.  Extracting it is then a
 * single unsigned shift with no mask, and the coordinate's wraparound -- the
 * `& 31` or `(uint8_t)` cast the C code performs on every pixel -- happens
 * for free as the carry falls off bit 31.  A constant-V row, where only u
 * moves, therefore costs five instructions per pixel:
 *
 *      movl %[U],%[t] ; shrl $27,%[t]     ; texel column, no mask
 *      movb (%[row],%[t]),%b[acc]         ; fetch
 *      rorl $8,%[acc]                     ; pack
 *      addl %[dU],%[U]                    ; step
 *
 * against roughly 7.5 for what the compiler emitted, half of which touched
 * the stack because it ran out of registers.
 *
 * FOUR PIXELS PER 32-BIT STORE.  Bytes are collected in one register with
 * ROR: each fetch writes the low byte and rotates it up, so after four the
 * first pixel is back in the low byte -- which on a little-endian byte
 * framebuffer is exactly where the leftmost pixel belongs.  On a 386SX the
 * 32-bit store is split into two 16-bit bus cycles by the hardware, so it
 * costs half of what four byte stores do, and it removes three quarters of
 * the pointer arithmetic and loop branching as well.  The destination is
 * aligned to 4 first: an unaligned dword store pays an extra bus cycle every
 * single time, which would give most of the saving straight back.
 *
 * Register pressure decides the rest.  The i386 has eight registers, one is
 * the stack pointer and -fPIC reserves another for the GOT, so a loop that
 * needs an accumulator, a source base, a destination, an address temp and a
 * coordinate has nothing left for its own bounds -- hence the end pointer
 * (and, where a second coordinate is in play, its step) living in memory. */

/* Constant-V row: u advances, v does not.  U is u << 24, dU is step_u << 24,
   so u's 8-bit wrap is the carry out of bit 31.  Exact -- no approximation
   anywhere in this one. */
static void cfx_direct_row_i386(uint8_t *dst, int span, const uint8_t *row,
                                uint32_t U, uint32_t dU)
{
    while (span > 0 && ((uintptr_t)dst & 3u) != 0u) {
        *dst++ = row[U >> 27];
        U += dU;
        --span;
    }

    if (span >= 4) {
        uint32_t t, acc;
        uint8_t *const end = dst + (span & ~3);
        __asm__ volatile (
            "1:\n\t"
#define CFX_I386_ROW_PIXEL                \
            "movl %[U],%[t]\n\t"          \
            "shrl $27,%[t]\n\t"           \
            "movb (%[row],%[t]),%b[acc]\n\t" \
            "rorl $8,%[acc]\n\t"          \
            "addl %[dU],%[U]\n\t"
            CFX_I386_ROW_PIXEL
            CFX_I386_ROW_PIXEL
            CFX_I386_ROW_PIXEL
            CFX_I386_ROW_PIXEL
#undef CFX_I386_ROW_PIXEL
            "movl %[acc],(%[dst])\n\t"
            "addl $4,%[dst]\n\t"
            "cmpl %[end],%[dst]\n\t"
            "jb 1b\n\t"
            : [dst] "+r" (dst), [U] "+r" (U),
              [t] "=&r" (t), [acc] "=&q" (acc)
            : [dU] "r" (dU), [row] "r" (row), [end] "m" (end)
            : "cc", "memory");
        span &= 3;
    }

    while (span-- > 0) {
        *dst++ = row[U >> 27];
        U += dU;
    }
}

/* Affine row: both u and v advance (perspective board floor, tilted card
   faces).  One packed accumulator advances both, so the loop does one add
   rather than two:
 *
 *      bits  0-7   v      A  = v | (u << 24)
 *      bits  8-23  carry guard
 *      bits 24-31  u      dA = (uint8)step_v + ((uint8)step_u << 24)
 *
 * u sits at the top, so its 8-bit wrap is the carry off bit 31.  v keeps its
 * own low byte and its carries pile up harmlessly in the 16-bit guard, which
 * a span would have to be 65536 pixels long to overflow -- so unlike the
 * usual packed-UV trick this one is exact, not merely close.
 *
 * The texel row is (v >> 3) * 32, i.e. (A & 0xff) << 2 masked to 0x3e0, and
 * the column is A >> 27.  Only a 32x32 pitch-32 tile can be addressed this
 * way; the caller checks the pitch. */
static void cfx_direct_affine_i386(uint8_t *dst, int span, const uint8_t *tile,
                                   uint32_t A, uint32_t dA)
{
#define CFX_I386_AFFINE_TEXEL(A, tile) \
    ((tile)[((((A) & 0xffu) << 2) & 0x3e0u) | ((A) >> 27)])

    while (span > 0 && ((uintptr_t)dst & 3u) != 0u) {
        *dst++ = CFX_I386_AFFINE_TEXEL(A, tile);
        A += dA;
        --span;
    }

    if (span >= 4) {
        uint32_t t1, t2, acc;
        uint8_t *const end = dst + (span & ~3);
        __asm__ volatile (
            "1:\n\t"
#define CFX_I386_AFFINE_PIXEL             \
            "movl %[A],%[t1]\n\t"         \
            "shll $2,%[t1]\n\t"           \
            "andl $0x3e0,%[t1]\n\t"       \
            "movl %[A],%[t2]\n\t"         \
            "shrl $27,%[t2]\n\t"          \
            "addl %[t2],%[t1]\n\t"        \
            "movb (%[tile],%[t1]),%b[acc]\n\t" \
            "rorl $8,%[acc]\n\t"          \
            "addl %[dA],%[A]\n\t"
            CFX_I386_AFFINE_PIXEL
            CFX_I386_AFFINE_PIXEL
            CFX_I386_AFFINE_PIXEL
            CFX_I386_AFFINE_PIXEL
#undef CFX_I386_AFFINE_PIXEL
            "movl %[acc],(%[dst])\n\t"
            "addl $4,%[dst]\n\t"
            "cmpl %[end],%[dst]\n\t"
            "jb 1b\n\t"
            : [dst] "+r" (dst), [A] "+r" (A),
              [t1] "=&r" (t1), [t2] "=&r" (t2), [acc] "=&q" (acc)
            : [dA] "r" (dA), [tile] "r" (tile), [end] "m" (end)
            : "cc", "memory");
        span &= 3;
    }

    while (span-- > 0) {
        *dst++ = CFX_I386_AFFINE_TEXEL(A, tile);
        A += dA;
    }
#undef CFX_I386_AFFINE_TEXEL
}
#endif /* CFX_RENDERER_USE_I386_ASM */

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
            "andi 255,%[c1],%[c1]\n"
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
#ifdef CFX_MEASURE_SKIP_SPANS
    (void)renderer;(void)y;(void)xs;(void)span;(void)tex_state;
    (void)tex_step_linear;(void)step_u;(void)step_v; return;
#endif
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

#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_emit_span(renderer, span);
    cfx_profile_emit_run(renderer, span);
#endif

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


#if CFX_RENDERER_USE_I386_ASM
/* ---- i386: one packed UV accumulator, four pixels per 32-bit store -------
 *
 * This is the FM TOWNS duel board's hot loop: the top-down board view is
 * ~50000 pixels of it per frame and measured 26.8 ms of game step, i.e. the
 * whole 3-D cost of a duel is essentially this function.
 *
 * What the compiler produced from the portable C loop below, at -Os for
 * -march=i386, was 14 instructions per pixel:
 *
 *      cmpl %ecx,%eax ; je ... ; incl %eax          <- 2 branches per pixel
 *      movl %esi,%edx ; shrl $3,%edx  ; andl $992,%edx
 *      movl %ebx,%ebp ; shrl $8,%ebp  ; andl $31,%ebp
 *      orl  %ebp,%edx ; movb (%edi,%edx),%dl ; movb %dl,-1(%eax)
 *      addl 36(%esp),%ebx ; addl 40(%esp),%esi      <- du/dv re-read from
 *      jmp  ...                                        the stack per pixel
 *
 * Three things are wrong with it on a 16 MHz 386SX behind a 16-bit bus,
 * where instruction fetch is itself a bus cost: du and dv are read from
 * memory for every pixel, the loop spends two branches per pixel, and each
 * pixel is its own byte store.
 *
 * PACKED ACCUMULATOR.  u and v are both Q8 and only their low 13 bits can
 * matter -- the texel index is ((v>>8)&31)*32 + ((u>>8)&31), so everything
 * above bit 12 of either is masked away.  Two 13-bit quantities fit in one
 * 32-bit register, which turns two adds (and two stack reads) into one:
 *
 *      bits  0-7   v fraction        A     = (v & 0x1fff) | (u << 19)
 *      bits  8-12  v integer         dA    = (dv & 0x1fff) + (du << 19)
 *      bits 13-18  carry guard       v row = (A >> 3) & 0x3e0
 *      bits 19-26  u fraction        u col = (A >> 27)
 *      bits 27-31  u integer
 *
 * u sits at the top so its wraparound falls off bit 31 and is discarded --
 * exactly the mod-32 texture repeat the C code gets from its mask -- and
 * because its integer part is the top 5 bits, extracting it is a single
 * shift with no mask at all.  dv is reduced mod 8192 so it stays positive
 * and can carry at most once per pixel, which is what makes the guard work:
 * v's carries pile up in bits 13-18 and only leak into u's fraction after 64
 * of them, so across a 256-pixel span u drifts by at most 4/256 of a texel.
 * That is the one place this is not bit-identical to the C loop, and it is
 * far below a visible pixel.
 *
 * Per pixel that leaves 9 instructions, including the dA add from a retained
 * register (the i386 has 8 registers, -fPIC reserves one for the GOT, and the
 * loop needs the accumulator, the tile base, the destination, two address
 * temps and the pixel accumulator).  The four pixel
 * bytes are collected in one register with ROR and written as a single
 * 32-bit store: a dword store is two bus cycles on the SX's 16-bit bus
 * against four for four byte stores, and it removes three quarters of the
 * loop's pointer arithmetic and branching as well.  The destination is
 * aligned to 4 first, since an unaligned dword store costs an extra bus
 * cycle on every one of them. */
/* One pixel.  The ROR walks the newest byte up to the top of the register;
   after four of them the first pixel is back in the low byte, which is where
   the leftmost pixel belongs in a little-endian byte framebuffer. */
#define CFX_I386_BOARD_PIXEL              \
    "movl %[A],%[t1]\n\t"                 \
    "shrl $3,%[t1]\n\t"                   \
    "andl $0x3e0,%[t1]\n\t"               \
    "movl %[A],%[t2]\n\t"                 \
    "shrl $27,%[t2]\n\t"                  \
    "addl %[t2],%[t1]\n\t"                \
    "movb (%[tile],%[t1]),%b[acc]\n\t"    \
    "rorl $8,%[acc]\n\t"                  \
    "addl %[dA],%[A]\n\t"

static void cfx_board_fill_i386(uint8_t *dst, int n, uint32_t A, uint32_t dA,
                                const uint8_t *tile)
{
    /* Head: byte pixels until the destination is 4-aligned. */
    while (n > 0 && ((uintptr_t)dst & 3u) != 0u) {
        *dst++ = tile[((A >> 3) & 0x3e0u) | (A >> 27)];
        A += dA;
        --n;
    }

    if (n >= 4) {
        /* Six registers are occupied by the loop state: the i386 has eight,
           -fPIC reserves one for the GOT and one is the stack pointer.  Keep
           dA in a register; only the end pointer remains a memory operand. */
        uint32_t t1, t2, acc;
        uint8_t *const end = dst + (n & ~3);
        __asm__ volatile (
            "1:\n\t"
            CFX_I386_BOARD_PIXEL
            CFX_I386_BOARD_PIXEL
            CFX_I386_BOARD_PIXEL
            CFX_I386_BOARD_PIXEL
            "movl %[acc],(%[dst])\n\t"
            "addl $4,%[dst]\n\t"
            "cmpl %[end],%[dst]\n\t"
            "jb 1b\n\t"
            : [dst] "+r" (dst), [A] "+r" (A),
              [t1] "=&r" (t1), [t2] "=&r" (t2), [acc] "=&q" (acc)
            : [dA] "r" (dA), [tile] "r" (tile), [end] "m" (end)
            : "cc", "memory");
        n &= 3;
    }

    while (n-- > 0) {
        *dst++ = tile[((A >> 3) & 0x3e0u) | (A >> 27)];
        A += dA;
    }
}
#endif /* CFX_RENDERER_USE_I386_ASM */

void cfx_board_fill(uint8_t *dst, int n,
        int32_t u, int32_t v, int32_t du, int32_t dv, const uint8_t *tile)
{
#if defined(CFX_MEASURE_SKIP_BOARD_FILL) || defined(CFX_MEASURE_SKIP_SPANS)
    (void)dst;(void)n;(void)u;(void)v;(void)du;(void)dv;(void)tile; return;
#endif
    if (n <= 0) return;
#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_emit_global_run(n);
#endif
#if CFX_RENDERER_USE_I386_ASM && !defined(CFX_MEASURE_C_BOARD_FILL)
    cfx_board_fill_i386(dst, n,
                        ((uint32_t)v & 0x1fffu) | ((uint32_t)u << 19),
                        ((uint32_t)dv & 0x1fffu) + ((uint32_t)du << 19),
                        tile);
#elif CFX_RENDERER_USE_V810_ASM
    {
        /* Keep this scalar loop compact.  A copied two-texel Maka variant
           removed 1.6% of the moving-board instruction count, but grew this
           function enough to shift the adjacent hot span dispatcher across
           direct-mapped V810 I-cache sets.  The measured render-cost proxy
           regressed by 4.4k cycles/field, so code placement wins here. */
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
            "andi 255,%[c1],%[c1]\n"
            "andi 255,%[c3],%[c3]\n"
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
            "andi 255,%[c1],%[c1]\n"
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
            "andi 255,%[c1],%[c1]\n"
            "andi 255,%[c3],%[c3]\n"
            "andi 255,%[c5],%[c5]\n"
            "andi 255,%[c7],%[c7]\n"
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

/* Out-of-line: this is the constant-V (axis-aligned block face) row path, which
   the perspective board floor never takes (floor rows have step_v != 0).  Keeping
   it out of cfx_draw_span_direct_tile stops its ~200 bytes from fragmenting the
   floor's hot per-scanline path in the tiny 1 KB V810 icache. */
#if defined(WAIFU_FM_PCFX)
/* Aligned to a 1 KB boundary (V810 icache set 0).  The affine quad walker's hot
   scanline loop lands in the middle cache sets; pinning this #1-thrashing row
   filler to set 0 makes it alias only the walker's once-per-quad cold prologue
   instead of its per-scanline loop, so the two stop evicting each other.
   Measured: in-duel icache misses/field 13951 -> ~12500, render-overhead
   (icache + DRAM + MUL) cyc/field ~92300 -> ~88800. */
static __attribute__((noinline, aligned(1024))) void cfx_draw_span_direct_tile_row(
#else
static inline __attribute__((always_inline)) void cfx_draw_span_direct_tile_row(
#endif
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

#if CFX_RENDERER_USE_I386_ASM && !defined(CFX_MEASURE_C_ROW)
    /* This is the FM TOWNS duel's most expensive loop by a wide margin.  The
       top-down board view is a grid of screen-aligned quads, so every one of
       its scanlines is a constant-V row and lands here; that view measured
       26.8 ms of game step against 1.0 ms for the hand view, and the
       difference is this function.
       Nothing here needs the pair machinery the other targets use.  Those
       exist because PC-FX writes KRAM through a 16-bit port and CD32X is
       big-endian; an i386 writing a byte-linear framebuffer can simply
       address pixels, which drops the odd-edge read-modify-write entirely. */
    {
        uint8_t *b = renderer->framebuffer + ((int32_t)y * renderer->width) + xs;
        cfx_direct_row_i386(b, span, row,
                            (uint32_t)u << 24, (uint32_t)(uint8_t)step_u << 24);
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

#if CFX_RENDERER_USE_V810_ASM && !CFX_RENDERER_DIRECT_KRAM
    /* Tight V810 form of the four-pixel row body (this is the #1 in-duel icache
       thrasher).  Two shrinks vs the compiler's C loop: u is an 8-bit
       accumulator so (u>>3) is already 0..31 -- the per-pixel CFX_TEX_MASK &31
       is dropped -- and the two trailing st.h fill the loop branch's flag-read
       slot so bne no longer stalls on the just-decremented count.  All four
       texels come from the single source row (same DRAM page); pixel-identical
       to the C body: left texel in the high byte, u advances per pixel. */
    if (span >= 4) {
        uint16_t iters = (uint16_t)((uint16_t)span >> 2);
        uint32_t uu = u;
        uint32_t su = (uint32_t)(uint8_t)step_u;
        uint32_t a0, a1, a2, a3, ta;
        __asm__ volatile (
            "1:\n"
            "mov %[u],%[ta]\n" "shr 3,%[ta]\n" "add %[row],%[ta]\n" "ld.b 0[%[ta]],%[a0]\n"
            "add %[su],%[u]\n" "andi 255,%[u],%[u]\n"
            "mov %[u],%[ta]\n" "shr 3,%[ta]\n" "add %[row],%[ta]\n" "ld.b 0[%[ta]],%[a1]\n"
            "add %[su],%[u]\n" "andi 255,%[u],%[u]\n"
            "mov %[u],%[ta]\n" "shr 3,%[ta]\n" "add %[row],%[ta]\n" "ld.b 0[%[ta]],%[a2]\n"
            "add %[su],%[u]\n" "andi 255,%[u],%[u]\n"
            "mov %[u],%[ta]\n" "shr 3,%[ta]\n" "add %[row],%[ta]\n" "ld.b 0[%[ta]],%[a3]\n"
            "add %[su],%[u]\n" "andi 255,%[u],%[u]\n"
            "andi 255,%[a1],%[a1]\n" "andi 255,%[a3],%[a3]\n"
            "shl 8,%[a0]\n" "or %[a1],%[a0]\n"
            "shl 8,%[a2]\n" "or %[a3],%[a2]\n"
            "add 4,%[dst]\n" "add -1,%[iters]\n"
            "st.h %[a0],-4[%[dst]]\n" "st.h %[a2],-2[%[dst]]\n"
            "bne 1b\n"
            : [dst] "+r" (dst), [iters] "+r" (iters), [u] "+r" (uu),
              [a0] "=&r" (a0), [a1] "=&r" (a1), [a2] "=&r" (a2), [a3] "=&r" (a3),
              [ta] "=&r" (ta)
            : [su] "r" (su), [row] "r" (row)
            : "memory");
        u = (uint8_t)uu;
        span &= 3;
    }
#else
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
#endif

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
            "andi 255,%[c1],%[c1]\n"
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
#ifdef CFX_MEASURE_SKIP_SPANS
    (void)renderer;(void)tile;(void)y;(void)xs;(void)span;(void)tex_state;
    (void)step_u;(void)step_v; return;
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

#if defined(WAIFU_PROFILE_RENDER)
    cfx_profile_emit_span(renderer, span);
    cfx_profile_emit_run(renderer, span);
#endif

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
#if CFX_RENDERER_USE_I386_ASM && !defined(CFX_MEASURE_C_AFFINE)
    /* Byte-addressed, one packed accumulator -- see cfx_direct_affine_i386().
       Only the 32x32 pitch-32 tiles the board and card faces use match its
       hardcoded row shift; anything else falls through to the portable C
       below, exactly as the V810/SH-2 fast paths do. */
    if (renderer->tile_pitch_bytes == 32) {
        uint8_t *b = renderer->framebuffer + ((int32_t)y * renderer->width) + xs;
        cfx_direct_affine_i386(b, span, tile,
                               (uint32_t)v | ((uint32_t)u << 24),
                               (uint32_t)(uint8_t)step_v + ((uint32_t)(uint8_t)step_u << 24));
        return;
    }
#endif
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
