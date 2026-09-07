/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_types.h — the SNES fork's stand-in for MSXgl's typedefs.
 *
 *  src/msx2/msx2_duel.c is the render-free duel rules model and the only thing
 *  that made it an MSX file was MSXgl's integer typedefs.  src/snes/compat/
 *  msxgl.h is placed first on the include path so that file compiles here
 *  unmodified, exactly as src/atarist/compat/msxgl.h does for the ST.
 *
 *  816-tcc's integer model is the reason this is not just a copy of the ST
 *  shim: `int`, `short` and `long` are ALL 16 bit, and 32-bit is `long long`.
 *  So u32/i32 must come from pvsneslib's snestypes.h, never from a hand-rolled
 *  `unsigned long`.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_TYPES_H
#define WAIFU_SNES_TYPES_H

#include <snes.h>

/* snestypes.h gives u8/u16/u32 and s8/s16/s32; the rules model spells the
 * signed ones i8/i16/i32. */
typedef s8  i8;
typedef s16 i16;
typedef s32 i32;

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* WAIFU_SNES_TYPES_H */
