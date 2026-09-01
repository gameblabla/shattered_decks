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
#define MSX2_BTN_A      0x10   // space / trigger A
#define MSX2_BTN_B      0x20   // escape / trigger B

void Msx2_InputInit(void);
void Msx2_InputUpdate(void);

u8   Msx2_InputHeld(void);      // buttons currently down
u8   Msx2_InputPressed(void);   // buttons that went down this frame
