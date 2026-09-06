/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_blitter.c — the Blitter, where it actually wins.
 *
 *  The requirement is "use the Blitter if one is fitted, and assume one on the
 *  STE", and the honest answer to *where* is: on the big flat moves, and
 *  nowhere else.  The Blitter cannot fetch a texel per pixel, so neither the
 *  ground span nor the card quads can go near it; what it does do is move
 *  words about twice as fast as the 68000's `move.l` loop, without the loop.
 *
 *  So it is used for exactly one thing today: the ground cache copy, 9 KB on a
 *  moving frame and 35 KB on a still one.  The solid fills are NOT on it --
 *  a copy blit cannot make a colour out of nothing, and doing a solid fill
 *  needs the halftone unit, whose 16-word RAM is indexed by scanline rather
 *  than by x, so a repeating four-word bitplane pattern is not a halftone.
 *  That is a real piece of work, not an oversight to be patched in casually.
 *
 *  HOG MODE, NOT BLIT MODE.  In hog mode (`ctrl` bit 6) the Blitter takes the
 *  bus and runs the whole transfer with the CPU halted, which is what makes it
 *  a straight-line call.  Blit mode yields every 64 bus cycles and has to be
 *  restarted by the caller; it exists so interrupts stay responsive, and this
 *  port's interrupts are the VBL palette load and the raster split -- both of
 *  which would rather wait a few hundred microseconds than have the game hand
 *  the Blitter back on the wrong scanline.
 * ───────────────────────────────────────────────────────────────────────────── */

#include "atarist_blitter.h"
#include "atarist_hw.h"
#include "atarist_video.h"

/* Below this a blit is not worth 24 register writes.  Measured shape, not a
 * guess: the setup is about the same as copying 128 bytes by hand. */
#define BLIT_MIN_WORDS 64

int Atarist_BlitterUsable(const void *dst, const void *src, int32_t bytes)
{
    if (!g_atarist_has_blitter) return 0;
    if (bytes < BLIT_MIN_WORDS * 2) return 0;
    if (bytes & 1) return 0;
    /* The Blitter addresses words: an odd address is not slow here, it is a
     * bus error waiting to happen. */
    if (((uint32_t)(size_t)dst | (uint32_t)(size_t)src) & 1) return 0;
    /* One line, so the whole transfer has to fit in the 16-bit x count. */
    if ((bytes >> 1) > 65535) return 0;
    return 1;
}

void Atarist_BlitterCopy(void *dst, const void *src, int32_t bytes)
{
    uint16_t words = (uint16_t)(bytes >> 1);

    ST_BLIT_SRC_XINC = 2;
    ST_BLIT_SRC_YINC = 2;
    ST_BLIT_SRC_ADDR = (uint32_t)(size_t)src;
    ST_BLIT_DST_XINC = 2;
    ST_BLIT_DST_YINC = 2;
    ST_BLIT_DST_ADDR = (uint32_t)(size_t)dst;
    /* Every word is written whole: the masks exist to protect the edges of a
     * bitplane rectangle, and this is a flat run. */
    ST_BLIT_ENDMASK1 = 0xffffu;
    ST_BLIT_ENDMASK2 = 0xffffu;
    ST_BLIT_ENDMASK3 = 0xffffu;
    ST_BLIT_XCOUNT = words;
    ST_BLIT_YCOUNT = 1;
    ST_BLIT_HOP = 2;          /* halftone off: the operand is the source */
    ST_BLIT_OP = 3;           /* destination = source */
    ST_BLIT_SKEW = 0;
    /* Bit 7 starts it, bit 6 asks for hog mode.  The write does not return
     * until the transfer is done. */
    ST_BLIT_CTRL = 0xc0;
}
