#include "pad.h"
#include "io.h"

#define PAD1IN  0x4d0
#define PAD2IN  0x4d2
#define PADOUT  0x4d6

#define COM0    0xf
#define COM1    0x3f
#define COMIN   0x40

static unsigned int pad_read_port(uint16_t padin)
{
    unsigned int status = 0xc0;

    /* COM0: latch D-Pad + A/B into bits [5:0]. */
    do {
        outb(COM0, PADOUT);
    } while ((inb(padin) & COMIN) != 0);
    status |= (inb(padin) & 0x3f);

    /* COM1: latch Z/Y/X/C into bits [11:8]. */
    do {
        outb(COM1, PADOUT);
    } while ((inb(padin) & COMIN) == 0);
    status |= ((inb(padin) & 0xf) << 8);

    /* SELECT is wired onto the Left+Right lines, RUN onto Up+Down -
     * both read as "pressed" (00) together when the corresponding
     * face button is held, since the pad can't drive both at once. */
    if ((status & 0x3) == 0) {
        status |= 0x3;
        status &= ~FMT_PAD_SELECT;
    }
    if ((status & 0xc) == 0) {
        status |= 0xc;
        status &= ~FMT_PAD_RUN;
    }

    return status;
}

unsigned int fmt_pad_read(int port)
{
    if (port == 0) {
        return pad_read_port(PAD1IN);
    } else if (port == 1) {
        return pad_read_port(PAD2IN);
    }
    return 0xffffffff;
}
