#ifndef FMT_PAD_H
#define FMT_PAD_H

#include <stdint.h>

/*
 * FM TOWNS 6-button pad reader, ported from FMTOWNS_PAD6/PAD6.c (same
 * COM0/COM1 strobe protocol, same PAD1IN/PAD2IN/PADOUT ports) onto this
 * project's outb()/inb() (src/boot/io.h) instead of the original's
 * _outb()/_inb() intrinsics.
 *
 * Bit layout of fmt_pad_read()'s return value (0 = pressed, 1 =
 * released, matching the pad's own idle-high wiring):
 *   0: Up     1: Down   2: Left   3: Right
 *   4: A      5: B      6: RUN    7: SELECT
 *   8: Z      9: Y     10: X     11: C
 */
#define FMT_PAD_UP      0x001
#define FMT_PAD_DOWN    0x002
#define FMT_PAD_LEFT    0x004
#define FMT_PAD_RIGHT   0x008
#define FMT_PAD_A       0x010
#define FMT_PAD_B       0x020
#define FMT_PAD_RUN     0x040
#define FMT_PAD_SELECT  0x080
#define FMT_PAD_Z       0x100
#define FMT_PAD_Y       0x200
#define FMT_PAD_X       0x400
#define FMT_PAD_C       0x800

/* port: 0 = PAD1, 1 = PAD2. Returns the raw 12-bit active-low status
 * word above, or 0xffffffff for any other port. */
unsigned int fmt_pad_read(int port);

#endif
