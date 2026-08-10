#ifndef FMT_MBV_BLIT_H
#define FMT_MBV_BLIT_H

#include <stdint.h>

/*
 * Getting a decoded 8bpp frame into single-page VRAM, fast.
 *
 * libfmt's fmt_put_image() walks the image one byte at a time and runs the
 * VRAM1Trans address swizzle (libfmt.c) on every one of them.  At 256x240
 * that is 61440 swizzle computations and 61440 single-byte VRAM writes per
 * frame, on a 16MHz 386SX that also has to feed a DAC every 62.5us.  Far too
 * slow for video.
 *
 * The swizzle is not arbitrary, though:
 *
 *   trans(off) = ((off & 4) << 16) | ((off & 0x7fff8) >> 1) | (off & 3)
 *
 * Bits 0-1 pass straight through and bit 2 selects a bank, so each aligned
 * four-byte group of the source lands as one contiguous four-byte group in
 * VRAM.  Writing q = off >> 3, the group at off = 8q goes to 4q, and the group
 * at off = 8q+4 goes to 0x40000 + 4q.  In other words the source's even and
 * odd four-byte groups are two plain, dense, *linear* arrays 256KB apart.
 *
 * So the whole frame is a single loop over 32-bit words alternating between
 * two moving pointers - no address arithmetic per pixel, no byte writes, and
 * a quarter as many bus transactions.  tests/mbv_blit_test.c checks this
 * against libfmt.c's byte-at-a-time transform for every offset in a page.
 *
 * With FMT_MBV_DAC_TICK defined the loop services the DAC every 64 bytes,
 * which at any plausible VRAM speed is well inside one sample period.
 */

#ifdef FMT_MBV_DAC_TICK
#include "dacout.h"
#define MBV_BLIT_TICK() fmt_dac_tick()
#else
#define MBV_BLIT_TICK() ((void)0)
#endif

/* Distance between the two banks the swizzle interleaves - bit 2 of the
 * source offset moved up to bit 18. */
#define MBV_VRAM_BANK_STRIDE  0x40000u

typedef uint32_t mbv_blit_u32 __attribute__((may_alias));

/*
 * Copies `bytes` bytes of an 8bpp image into single-page VRAM starting at
 * VRAM byte offset `base`.  Both must be multiples of 8, and `src` must be
 * 4-byte aligned.
 */
static inline void fmt_mbv_blit(volatile uint8_t *vram, uint32_t base,
                                const uint8_t *src, uint32_t bytes)
{
    volatile mbv_blit_u32 *even = (volatile mbv_blit_u32 *)(vram + (base >> 1));
    volatile mbv_blit_u32 *odd  = (volatile mbv_blit_u32 *)
                                  (vram + MBV_VRAM_BANK_STRIDE + (base >> 1));
    const mbv_blit_u32 *s = (const mbv_blit_u32 *)src;
    uint32_t pairs = bytes >> 3;

    while (pairs >= 8u) {
        unsigned i;
        for (i = 0; i < 8u; i++) {
            even[i] = s[0];
            odd[i]  = s[1];
            s += 2;
        }
        even += 8;
        odd  += 8;
        pairs -= 8u;
        MBV_BLIT_TICK();
    }
    while (pairs--) {
        *even++ = s[0];
        *odd++  = s[1];
        s += 2;
    }
}

#endif
