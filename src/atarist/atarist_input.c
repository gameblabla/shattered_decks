#include "atarist_input.h"
#include "atarist_hw.h"
#include "atarist_probe.h"

AtaristInput g_atarist_input;

/* Written by the ACIA interrupt, read by the main loop. */
volatile uint8_t g_atarist_keydown[128];
volatile uint8_t g_atarist_joy[2];
volatile uint8_t g_atarist_ikbd_state;   /* 0 = expecting a scancode */

void Atarist_AciaWrapper(void);

static uint32_t g_old_acia_vec;
static uint8_t  g_old_ierb;
static uint8_t  g_old_imrb;
static uint16_t g_repeat_timer[10];

/* ST scancodes. */
#define SC_ESC    0x01
#define SC_BSPACE 0x0e
#define SC_TAB    0x0f
#define SC_RETURN 0x1c
#define SC_CTRL   0x1d
#define SC_ALT    0x38
#define SC_SPACE  0x39
#define SC_UP     0x48
#define SC_LEFT   0x4b
#define SC_RIGHT  0x4d
#define SC_DOWN   0x50

/* Called from the ACIA interrupt wrapper.  With the mouse turned off the only
 * multi-byte packets left are the two joystick reports, so the whole protocol
 * is one state byte. */
void Atarist_AciaPoll(void)
{
    for (;;) {
        uint8_t b;
        /* BOTH ACIAs SHARE MFP CHANNEL 6.
         * A handler that services only the keyboard leaves a MIDI byte sitting
         * in the other ACIA with its interrupt line still asserted, the MFP
         * re-enters at level 6 immediately, and the machine locks up inside
         * this handler with the VBL never running again -- which is precisely
         * how this port first froze, a minute into the bring-up screen.  The
         * MIDI byte is read and dropped; the port has no use for it, but the
         * chip has to be told. */
        if (ST_R8(0xfffffc04) & 0x01) { (void)ST_R8(0xfffffc06); continue; }
        if (!(ST_ACIA_CTRL & 0x01)) break;   /* receive register empty */
        b = ST_ACIA_DATA;
        if (g_atarist_ikbd_state) {
            g_atarist_joy[g_atarist_ikbd_state - 1] = b;
            g_atarist_ikbd_state = 0;
        } else if (b == 0xfe) {
            g_atarist_ikbd_state = 1;
        } else if (b == 0xff) {
            g_atarist_ikbd_state = 2;
        } else if (b) {
            g_atarist_keydown[b & 0x7f] = (uint8_t)((b & 0x80) ? 0 : 1);
        }
    }
}

static void ikbd_send(uint8_t cmd)
{
    /* Bit 1 of the ACIA status is "transmit data register empty". */
    while (!(ST_ACIA_CTRL & 0x02)) { }
    ST_ACIA_DATA = cmd;
}

void Atarist_InputInit(void)
{
    int i;
    for (i = 0; i < 128; ++i) g_atarist_keydown[i] = 0;
    g_atarist_joy[0] = g_atarist_joy[1] = 0;
    g_atarist_ikbd_state = 0;

    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    g_old_acia_vec = ST_VEC_MFP_ACIA;
    g_old_ierb = ST_MFP_IERB;
    g_old_imrb = ST_MFP_IMRB;
    ST_VEC_MFP_ACIA = (uint32_t)Atarist_AciaWrapper;
    ST_MFP_IERB = (uint8_t)(g_old_ierb | ST_MFP_IERB_ACIA);
    ST_MFP_IMRB = (uint8_t)(g_old_imrb | ST_MFP_IERB_ACIA);
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");

    ikbd_send(0x12);   /* disable mouse reporting */
    ikbd_send(0x1a);   /* disable joystick reporting ... */
    ikbd_send(0x14);   /* ... then re-enable it in event mode */
}

void Atarist_InputShutdown(void)
{
    ikbd_send(0x1a);   /* joysticks off */
    ikbd_send(0x08);   /* relative mouse reporting back on for the desktop */
    __asm__ volatile("move.w #0x2700,%%sr" : : : "cc");
    ST_VEC_MFP_ACIA = g_old_acia_vec;
    ST_MFP_IERB = g_old_ierb;
    ST_MFP_IMRB = g_old_imrb;
    __asm__ volatile("move.w #0x2300,%%sr" : : : "cc");
}

void Atarist_InputFlush(void)
{
    int i;
    for (i = 0; i < 128; ++i) g_atarist_keydown[i] = 0;
    g_atarist_joy[0] = g_atarist_joy[1] = 0;
    g_atarist_input.held = 0;
    g_atarist_input.pressed = 0;
    g_atarist_input.released = 0;
    for (i = 0; i < 10; ++i) g_repeat_timer[i] = 0;
}

void Atarist_InputUpdate(void)
{
    uint16_t held = 0;
    uint16_t prev = g_atarist_input.held;
    uint8_t joy = (uint8_t)(g_atarist_joy[0] | g_atarist_joy[1]);

    if (g_atarist_keydown[SC_UP]    || (joy & 0x01)) held |= ATARIST_BTN_UP;
    if (g_atarist_keydown[SC_DOWN]  || (joy & 0x02)) held |= ATARIST_BTN_DOWN;
    if (g_atarist_keydown[SC_LEFT]  || (joy & 0x04)) held |= ATARIST_BTN_LEFT;
    if (g_atarist_keydown[SC_RIGHT] || (joy & 0x08)) held |= ATARIST_BTN_RIGHT;
    if (g_atarist_keydown[SC_CTRL] || g_atarist_keydown[SC_RETURN] ||
        (joy & 0x80)) held |= ATARIST_BTN_A;
    if (g_atarist_keydown[SC_ALT] || g_atarist_keydown[SC_BSPACE])
        held |= ATARIST_BTN_B;
    if (g_atarist_keydown[SC_SPACE]) held |= ATARIST_BTN_START;
    if (g_atarist_keydown[SC_TAB])   held |= ATARIST_BTN_TAB;
    if (g_atarist_keydown[SC_ESC])   held |= ATARIST_BTN_QUIT;

    /* Scripted input.  The headless harness has no way to press a key -- the
     * MCP server exposes memory and screenshots, not the IKBD -- so a queue
     * inside the probe struct stands in for the player.  It is OR-ed in, never
     * substituted, so a scripted run and a played run take the same path. */
    held |= Atarist_ProbeScriptedButtons();

    g_atarist_input.pressed  = (uint16_t)(held & ~prev);
    g_atarist_input.released = (uint16_t)(prev & ~held);
    g_atarist_input.held     = held;
}

int Atarist_InputRepeat(uint16_t button, int rate)
{
    int slot = 0;
    uint16_t b = button;
    while (b > 1u) { b >>= 1; ++slot; }
    if (slot >= 10) slot = 9;

    if (!(g_atarist_input.held & button)) {
        g_repeat_timer[slot] = 0;
        return 0;
    }
    if (g_atarist_input.pressed & button) {
        g_repeat_timer[slot] = (uint16_t)rate;
        return 1;
    }
    if (g_repeat_timer[slot]) {
        --g_repeat_timer[slot];
        return 0;
    }
    g_repeat_timer[slot] = (uint16_t)(rate / 2 > 0 ? rate / 2 : 1);
    return 1;
}
