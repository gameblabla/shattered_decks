/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_input.h — raw IKBD keyboard + joystick.
 *
 *  TOS's own ACIA handler is replaced for the duration of the game: the cooked
 *  Bconin path cannot report two keys held at once, and a duel needs a
 *  direction and a button together.  The handler keeps a key-down bitmap and
 *  the joystick states; the game latches an edge-detected snapshot once a
 *  frame, so a press that begins and ends inside one over-budget board render
 *  is still seen.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_INPUT_H
#define WAIFU_ATARIST_INPUT_H

#include <stdint.h>

#define ATARIST_BTN_UP     0x0001
#define ATARIST_BTN_DOWN   0x0002
#define ATARIST_BTN_LEFT   0x0004
#define ATARIST_BTN_RIGHT  0x0008
#define ATARIST_BTN_A      0x0010   /* Control / Return / joystick fire */
#define ATARIST_BTN_B      0x0020   /* Alternate / Backspace */
#define ATARIST_BTN_START  0x0040   /* Space */
#define ATARIST_BTN_TAB    0x0080   /* Tab */
#define ATARIST_BTN_QUIT   0x0100   /* Escape */

typedef struct AtaristInput {
    uint16_t held;      /* buttons down right now */
    uint16_t pressed;   /* went down since the previous latch */
    uint16_t released;
} AtaristInput;

extern AtaristInput g_atarist_input;

void Atarist_InputInit(void);
void Atarist_InputShutdown(void);
/* Latch the interrupt-maintained state into g_atarist_input. */
void Atarist_InputUpdate(void);
/* Forget everything held, so a screen change does not inherit the press that
 * left the previous screen. */
void Atarist_InputFlush(void);

/* Repeat helper: true on the press edge and then every `rate` frames while the
 * button is held, which is what menu navigation wants. */
int  Atarist_InputRepeat(uint16_t button, int rate);

#endif /* WAIFU_ATARIST_INPUT_H */
