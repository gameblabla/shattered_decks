// ─────────────────────────────────────────────────────────────────────────────
//  msx2_input.c — joystick and keyboard, read once per frame
//
//  Both ports and the keyboard feed one latch.  The keyboard matrix is read
//  directly (Keyboard_Read) rather than through MSXgl's keyboard module: this
//  port installs its own ISR, so the module's per-frame buffer is never
//  refreshed and would report every key as released.
//
//  Everything is active-low at the hardware and active-high in the latch, which
//  is the only sign convention the rest of the port ever sees.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_input.h"

static u8 g_held;
static u8 g_pressed;

void Msx2_InputInit(void)
{
	g_held = 0;
	g_pressed = 0;
}

// Row 8 of the MSX keyboard matrix carries SPACE and all four cursor keys, so
// the common case is a single matrix read.
#define KROW_ARROWS   8
#define KROW_CONTROL  7

void Msx2_InputUpdate(void)
{
	u8 now = 0;
	u8 row;
	u8 joy;

	row = ~Keyboard_Read(KROW_ARROWS);        // now active-high
	if(row & (1 << KEY_IDX(KEY_UP)))    now |= MSX2_BTN_UP;
	if(row & (1 << KEY_IDX(KEY_DOWN)))  now |= MSX2_BTN_DOWN;
	if(row & (1 << KEY_IDX(KEY_LEFT)))  now |= MSX2_BTN_LEFT;
	if(row & (1 << KEY_IDX(KEY_RIGHT))) now |= MSX2_BTN_RIGHT;
	if(row & (1 << KEY_IDX(KEY_SPACE))) now |= MSX2_BTN_A;

	row = ~Keyboard_Read(KROW_CONTROL);
	if(row & (1 << KEY_IDX(KEY_RETURN))) now |= MSX2_BTN_A;
	if(row & (1 << KEY_IDX(KEY_ESC)))    now |= MSX2_BTN_B;

	// Both joystick ports drive the same latch: whichever one the player
	// plugged into is the one that works, with no setup screen to get wrong.
	joy = Joystick_Read(INPUT_PORT1) & Joystick_Read(INPUT_PORT2);
	now |= (u8)(JOY_GET_DIR(joy) & 0x0F);     // the direction bits already line up
	if(JOY_GET_A(joy)) now |= MSX2_BTN_A;
	if(JOY_GET_B(joy)) now |= MSX2_BTN_B;

	g_pressed = (u8)(now & ~g_held);
	g_held = now;
}

u8 Msx2_InputHeld(void)    { return g_held; }
u8 Msx2_InputPressed(void) { return g_pressed; }
