/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_blitter.h — Blitter-accelerated block moves.
 *
 *  Optional on an ST and assumed on an STE, which is why every entry point is
 *  guarded: `Atarist_BlitterUsable()` answers whether this particular transfer
 *  is worth handing over, and the callers keep their CPU path for when it is
 *  not.  See atarist_blitter.c for what the Blitter is and is not used for.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_BLITTER_H
#define WAIFU_ATARIST_BLITTER_H

#include <stdint.h>
#include <stddef.h>

/* Non-zero when a Blitter is fitted and this transfer suits it: word aligned,
 * an even byte count, big enough to pay for the setup, and short enough to fit
 * one line of the Blitter's 16-bit x count. */
int  Atarist_BlitterUsable(const void *dst, const void *src, int32_t bytes);
/* Straight block move.  Returns when the transfer is complete. */
void Atarist_BlitterCopy(void *dst, const void *src, int32_t bytes);

#endif /* WAIFU_ATARIST_BLITTER_H */
