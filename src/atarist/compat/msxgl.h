/* Atari ST fork shim.
 *
 * src/msx2/msx2_duel.c is the render-free duel rules model, and the only thing
 * that made it an MSX file was MSXgl's integer typedefs.  This stand-in is
 * placed first on the ST include path so that source compiles unmodified and
 * both forks keep computing damage the same way.
 *
 * Nothing here may grow into an MSXgl emulation layer: if an ST module wants a
 * type it should use <stdint.h> directly.
 */
#ifndef WAIFU_ATARIST_MSXGL_SHIM_H
#define WAIFU_ATARIST_MSXGL_SHIM_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;

#ifndef bool
typedef unsigned char bool_t;
#define bool bool_t
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* WAIFU_ATARIST_MSXGL_SHIM_H */
