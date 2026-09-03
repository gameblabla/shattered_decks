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
static c8 g_typed;
// Rows 0..5 of the matrix carry the digits and the whole alphabet.  The
// previous scan is kept so a held key types once, exactly like the latch above.
static u8 g_key_held[6];
// F1, kept apart from the letter scan: it is the only key the game reads that
// must not also be a character, because the screens that offer the disk are
// the two the player types a name and a continue code into.
#define KROW_FN      6
#define KBIT_F1      5
static u8 g_f1_held;
static u8 g_f1_press;

void Msx2_InputInit(void)
{
	u8 i;
	g_held = 0;
	g_pressed = 0;
	g_typed = 0;
	g_f1_held = 0;
	g_f1_press = 0;
	for(i = 0; i < 6; ++i)
		g_key_held[i] = 0;
}

// The MSX keyboard matrix, in the order the rows report it:
//   row 0: 0 1 2 3 4 5 6 7      row 3: C D E F G H I J
//   row 1: 8 9 - = \ [ ] ;      row 4: K L M N O P Q R
//   row 2: ' ` , . / _ A B      row 5: S T U V W X Y Z
// so the printable characters this game needs are one flat table.
static const c8 g_key_char[6][8] =
{
	{ '0', '1', '2', '3', '4', '5', '6', '7' },
	{ '8', '9',   0,   0,   0,   0,   0,   0 },
	{   0,   0,   0,   0,   0,   0, 'A', 'B' },
	{ 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J' },
	{ 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R' },
	{ 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' },
};

// Scan the printable rows and report the first key that went down this frame.
static void Msx2_InputScanTyped(void)
{
	u8 r;
	g_typed = 0;
	for(r = 0; r < 6; ++r)
	{
		u8 now = (u8)~Keyboard_Read(r);
		u8 fresh = (u8)(now & ~g_key_held[r]);
		g_key_held[r] = now;
		if((fresh != 0) && (g_typed == 0))
		{
			u8 b;
			for(b = 0; b < 8; ++b)
				if((fresh & (u8)(1 << b)) && (g_key_char[r][b] != 0))
				{
					g_typed = g_key_char[r][b];
					break;
				}
		}
	}
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
	if(row & (1 << KEY_IDX(KEY_RETURN))) now |= (MSX2_BTN_A | MSX2_BTN_ENTER);
	if(row & (1 << KEY_IDX(KEY_ESC)))    now |= MSX2_BTN_B;
	if(row & (1 << KEY_IDX(KEY_BS)))     now |= (MSX2_BTN_B | MSX2_BTN_DEL);

	// Both joystick ports drive the same latch: whichever one the player
	// plugged into is the one that works, with no setup screen to get wrong.
	joy = Joystick_Read(INPUT_PORT1) & Joystick_Read(INPUT_PORT2);
	now |= (u8)(JOY_GET_DIR(joy) & 0x0F);     // the direction bits already line up
	if(JOY_GET_A(joy)) now |= MSX2_BTN_A;
	if(JOY_GET_B(joy)) now |= MSX2_BTN_B;

	g_pressed = (u8)(now & ~g_held);
	g_held = now;

	Msx2_InputScanTyped();

	{
		u8 f1 = (u8)((~Keyboard_Read(KROW_FN)) & (u8)(1 << KBIT_F1));
		g_f1_press = (u8)(f1 && !g_f1_held);
		g_f1_held = f1;
	}
}

u8 Msx2_InputHeld(void)    { return g_held; }
u8 Msx2_InputPressed(void) { return g_pressed; }
c8 Msx2_InputTyped(void)   { return g_typed; }
bool Msx2_InputDiskKey(void) { return g_f1_press ? TRUE : FALSE; }
