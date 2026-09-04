// ─────────────────────────────────────────────────────────────────────────────
//  msx2_input.c — joystick and keyboard
//
//  Both ports and the keyboard feed one latch.  The keyboard matrix is read
//  directly rather than through MSXgl's keyboard module: this port installs its
//  own ISR, so the module's per-frame buffer is never refreshed and would
//  report every key as released.
//
//  Everything is active-low at the hardware and active-high in the latch, which
//  is the only sign convention the rest of the port ever sees.
//
//  THE MATRIX IS SCANNED AT V-BLANK, NOT WHEN THE GAME GETS ROUND TO IT.
//  A game step on this target is not one V-blank: a card name read out of the
//  cartridge, a panel repaint or a slot rasterised into its quad can take
//  several.  The scan used to happen once per game step, which meant a press
//  that began AND ended inside one long step was never seen at all -- half the
//  presses on the sanctum road were dropped, and typing a sixteen-character
//  continue code at an ordinary speed reliably lost every other letter.
//
//  So the whole matrix is scanned by the V-blank handler, sixty times a second
//  whatever the main loop is doing, and it ACCUMULATES: button edges into one
//  byte, fresh key bits into one byte per row.  The game consumes them at its
//  own pace and nothing in between is lost.
//
//  THAT SCAN IS ASSEMBLY ON PURPOSE.  SDCC allocates a C function's locals and
//  temporaries in a static overlay area it is free to share with any function
//  it believes cannot be live at the same time -- and it does not know that
//  crt0's interrupt hook calls into C, so nothing stops it sharing the
//  interrupt's frame with the main loop's.  Msx2_AudioTick(), the only other
//  thing the handler calls, has no locals at all for exactly that reason.  The
//  routine below is __naked, has no locals, calls nothing, and touches only its
//  own globals, so the question does not arise.
//
//  The joystick stays in the main loop: it sits behind the PSG, which the audio
//  tick owns, and it has no matrix row to lose.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_input.h"

static u8 g_held;
static u8 g_pressed;
static c8 g_typed;

// ── Written by the ISR, consumed by the main loop ────────────────────────────
// Plain globals rather than statics so the assembly below can name them.
// `now`   the instantaneous button state,
// `edge`  every button that has gone down since the last consume,
// `held`  the handler's own previous button scan,
// `fresh` per matrix row, every key bit that has gone down since the last
//         consume, and `prev` the handler's previous scan of that row.
#define KB_ROWS  7
volatile u8 g_msx2_kb_now;
volatile u8 g_msx2_kb_edge;
volatile u8 g_msx2_kb_held;
volatile u8 g_msx2_kb_fresh[KB_ROWS];
volatile u8 g_msx2_kb_prev[KB_ROWS];

// A SHORT QUEUE, NOT A SINGLE SLOT.
// One slot meant that two characters typed inside one game step kept the first
// and threw the second away.  Eight covers a burst arriving while a screen is
// in the middle of a repaint.
#define KB_TYPED_MAX  8
static c8 g_kb_typed[KB_TYPED_MAX];
static u8 g_kb_typed_n;

// F1 is kept apart from the letter scan: it is the only key the game reads that
// must not also be a character, because the screens that offer the disk are the
// two the player types a name and a continue code into.
#define KROW_FN      6
#define KBIT_F1      5
// X likewise shares the printable matrix with the typewriter and is also a duel
// command, so it raises both: entering a name or a code still receives the
// character while the board gets one position press.
#define KROW_X       5
#define KBIT_X       5
static u8 g_f1_press;
static u8 g_x_press;
// The joystick's previous state, which is the main loop's own edge detector.
static u8 g_joy_held;

void Msx2_InputInit(void)
{
	u8 i;
	g_held = 0;
	g_pressed = 0;
	g_typed = 0;
	g_f1_press = 0;
	g_x_press = 0;
	g_joy_held = 0;
	g_kb_typed_n = 0;
	g_msx2_kb_now = 0;
	g_msx2_kb_edge = 0;
	g_msx2_kb_held = 0;
	for(i = 0; i < KB_ROWS; ++i)
	{
		g_msx2_kb_fresh[i] = 0;
		g_msx2_kb_prev[i] = 0;
	}
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

// Called from the V-blank handler, and from nowhere else.  Rows 8 (the cursor
// keys and SPACE) and 7 (RETURN, ESC, BACKSPACE) are decoded straight into the
// button latch; rows 0..6 are kept as raw fresh-key bits for the main loop to
// turn into characters, which is arithmetic that does not have to happen here.
void Msx2_InputLatch(void) __naked
{
__asm
	; ---- row 8: SPACE b0, LEFT b4, UP b5, DOWN b6, RIGHT b7 ----------------
	in	a, (#0xAA)
	and	#0xF0
	or	#8
	out	(#0xAA), a
	in	a, (#0xA9)
	cpl				; active-low at the hardware, active-high here
	ld	c, a
	ld	b, #0			; B builds the latch
	bit	0, c
	jr	Z, 1$
	set	4, b			; SPACE  -> MSX2_BTN_A
1$:
	bit	4, c
	jr	Z, 2$
	set	2, b			; LEFT
2$:
	bit	5, c
	jr	Z, 3$
	set	0, b			; UP
3$:
	bit	6, c
	jr	Z, 4$
	set	1, b			; DOWN
4$:
	bit	7, c
	jr	Z, 5$
	set	3, b			; RIGHT
5$:
	; ---- row 7: ESC b2, BS b5, RETURN b7 -----------------------------------
	in	a, (#0xAA)
	and	#0xF0
	or	#7
	out	(#0xAA), a
	in	a, (#0xA9)
	cpl
	ld	c, a
	bit	7, c
	jr	Z, 6$
	set	4, b			; RETURN -> A ...
	set	6, b			;        ... and ENTER
6$:
	bit	2, c
	jr	Z, 7$
	set	5, b			; ESC    -> B
7$:
	bit	5, c
	jr	Z, 8$
	set	5, b			; BS     -> B ...
	set	7, b			;        ... and DEL
8$:
	; ---- edge |= now & ~held ; held = now ----------------------------------
	ld	a, (_g_msx2_kb_held)
	cpl
	and	b
	ld	hl, #_g_msx2_kb_edge
	or	(hl)
	ld	(hl), a
	ld	a, b
	ld	(_g_msx2_kb_held), a
	ld	(_g_msx2_kb_now), a

	; ---- rows 0..6: fresh[r] |= now & ~prev[r] ; prev[r] = now -------------
	ld	hl, #_g_msx2_kb_prev
	ld	de, #_g_msx2_kb_fresh
	ld	c, #0
9$:
	in	a, (#0xAA)
	and	#0xF0
	or	c
	out	(#0xAA), a
	in	a, (#0xA9)
	cpl
	ld	b, a			; B = this row, active-high
	ld	a, (hl)			; previous scan of it
	cpl
	and	b			; the bits that have just gone down
	ex	de, hl
	or	(hl)
	ld	(hl), a
	ex	de, hl
	ld	(hl), b
	inc	hl
	inc	de
	inc	c
	ld	a, c
	cp	#7
	jr	C, 9$
	ret
__endasm;
}

// Turn the ISR's fresh-key bits into typed characters and the two keys that are
// not characters.  Short enough to run with interrupts held off, which is what
// makes taking and clearing the accumulators one operation.
static void Msx2_InputConsumeRows(void)
{
	u8 r;

	for(r = 0; r < KB_ROWS; ++r)
	{
		u8 fresh = g_msx2_kb_fresh[r];
		u8 b;

		if(fresh == 0)
			continue;
		g_msx2_kb_fresh[r] = 0;
		if(r == KROW_FN)
		{
			if(fresh & (u8)(1 << KBIT_F1))
				g_f1_press = 1;
			continue;
		}
		if((r == KROW_X) && (fresh & (u8)(1 << KBIT_X)))
			g_x_press = 1;
		for(b = 0; b < 8; ++b)
			if((fresh & (u8)(1 << b)) && (g_key_char[r][b] != 0) &&
			   (g_kb_typed_n < KB_TYPED_MAX))
				g_kb_typed[g_kb_typed_n++] = g_key_char[r][b];
	}
}

void Msx2_InputUpdate(void)
{
	u8 edge;
	u8 now;
	u8 joy;
	u8 joy_now = 0;

	g_f1_press = 0;
	g_x_press = 0;

	// One consume, with the handler held off for it: an edge arriving between
	// reading an accumulator and clearing it would otherwise be thrown away.
	__asm di __endasm;
	edge = g_msx2_kb_edge;
	now = g_msx2_kb_now;
	g_msx2_kb_edge = 0;
	Msx2_InputConsumeRows();
	__asm ei __endasm;

	// One character a step, in the order they were typed: the screens that take
	// text add exactly one per step anyway, and holding the rest keeps a fast
	// typist's letters in the queue rather than on the floor.
	if(g_kb_typed_n != 0)
	{
		u8 i;
		g_typed = g_kb_typed[0];
		--g_kb_typed_n;
		for(i = 0; i < g_kb_typed_n; ++i)
			g_kb_typed[i] = g_kb_typed[i + 1];
	}
	else
		g_typed = 0;

	// Both joystick ports drive the same latch: whichever one the player
	// plugged into is the one that works, with no setup screen to get wrong.
	joy = Joystick_Read(INPUT_PORT1) & Joystick_Read(INPUT_PORT2);
	joy_now |= (u8)(JOY_GET_DIR(joy) & 0x0F);  // the direction bits already line up
	if(JOY_GET_A(joy)) joy_now |= MSX2_BTN_A;
	if(JOY_GET_B(joy)) joy_now |= MSX2_BTN_B;

	g_pressed = (u8)(edge | (u8)(joy_now & ~g_joy_held));
	g_joy_held = joy_now;
	g_held = (u8)(now | joy_now);
}

u8 Msx2_InputHeld(void)    { return g_held; }
u8 Msx2_InputPressed(void) { return g_pressed; }
void Msx2_InputConsume(u8 mask) { g_pressed &= (u8)~mask; }
c8 Msx2_InputTyped(void)   { return g_typed; }
bool Msx2_InputDiskKey(void) { return g_f1_press ? TRUE : FALSE; }
bool Msx2_InputXKey(void) { return g_x_press ? TRUE : FALSE; }
