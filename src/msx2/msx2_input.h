// ─────────────────────────────────────────────────────────────────────────────
//  msx2_input.h — joystick and keyboard, read once per frame
//
//  The game only ever sees the latched state from the last frame boundary, so
//  no scene can read a half-scanned matrix, and a scripted/self-driving build
//  can substitute its own input by writing the same latch.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_BTN_UP     0x01
#define MSX2_BTN_DOWN   0x02
#define MSX2_BTN_LEFT   0x04
#define MSX2_BTN_RIGHT  0x08
#define MSX2_BTN_A      0x10   // space / return / trigger A
#define MSX2_BTN_B      0x20   // escape / backspace / trigger B
// RETURN and BACKSPACE also raise A and B, so every screen that only knows
// about the joystick keeps working; the two extra bits let the screens that
// want a typewriter tell "accept" from "another letter".
#define MSX2_BTN_ENTER  0x40   // return only
#define MSX2_BTN_DEL    0x80   // backspace / delete only

void Msx2_InputInit(void);
void Msx2_InputUpdate(void);

u8   Msx2_InputHeld(void);      // buttons currently down
u8   Msx2_InputPressed(void);   // buttons that went down this frame

// A letter or digit typed on the MSX keyboard this frame, or 0.  Name and
// continue-code entry accept this as well as the on-screen grid, so a machine
// with a keyboard types and a machine with only a joystick still plays.
c8   Msx2_InputTyped(void);
