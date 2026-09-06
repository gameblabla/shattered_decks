// A small resident lVGM player for the formats this port packs: 60 Hz PSG,
// MSX-MUSIC (YM2413/OPLL), MSX-AUDIO (Y8950) and MoonSound (YMF278B/OPL4)
// streams with optional FD notifications and FE/FF loop markers.
//
// MSXgl's generic player dispatches every PSG command through a function
// pointer.  That path is useful for a multi-chip desktop sample, but it is a
// poor fit for this ISR: the pointer is in RAM while the 0x8000 window is
// changing, and the SDCC indirect-call shim can leave a compact PSG command
// at the same byte forever on real openMSX.  The format and callbacks are
// unchanged; the resident chip helpers and outer parser are kept explicit
// here so their code and the mapper seam are both safe in the ISR.  Which
// chip a track plays on is a property of the RECORDING, not of this file: a
// stream names its chips in the header device list and switches chunks with
// the F0/F1/F2/F7 markers, and msx2_audio.c picks the recording.

#include "vgm/lvgm_player.h"
#include "msx2_audio.h"

// ─────────────────────────────────────────────────────────────────────────────
//  The MoonSound's four I/O ports
//
//  MSXgl's system_port.h stops at the chips MSXgl itself plays, so the OPL4's
//  are named here.  They are two independent halves of one cartridge:
//
//    7E/7F   the WAVE (PCM) half -- 24 sample channels, register index then
//            data, and the half these recordings are almost entirely made of
//    C4/C5   the FM half's register bank 0 (it is an OPL3), index then data
//    C6/C7   the same for bank 1, which is where NEW2 lives
//
//  A read of C4 is the status byte, which is what the boot probe uses.
// ─────────────────────────────────────────────────────────────────────────────
#define MSX2_MOON_WAVE_INDEX   0x7E
#define MSX2_MOON_WAVE_DATA    0x7F
#define MSX2_MOON_FM0_INDEX    0xC4
#define MSX2_MOON_FM0_DATA     0xC5
#define MSX2_MOON_FM1_INDEX    0xC6
#define MSX2_MOON_FM1_DATA     0xC7

// ─────────────────────────────────────────────────────────────────────────────
//  WHY THE OPL4 IS ASKED WHETHER IT IS READY, AND THE OTHER TWO CHIPS ARE NOT
//
//  The OPLL and the Y8950 above are given a gap of counted T-states between an
//  index write and its data, which works because their datasheets state the gap
//  in CHIP clocks and the chip clock is the 3.58 MHz the Z80 runs at: a fixed
//  number of instructions is a fixed number of chip clocks, on every MSX2 ever
//  made.
//
//  The OPL4's clock is its own 33.868 MHz crystal, so its gaps are a fixed
//  TIME -- about 4.1 us on the wave half, 2.6 on the FM half -- and a fixed
//  number of instructions is only a fixed time if the CPU speed is fixed.  It
//  is not: msx2_bank.c puts a turbo R on its R800 at boot (Msx2_CpuFast), and
//  an R800 runs the two loads between those two writes in well under a
//  microsecond.  The delay that is right for a Z80 is a quarter of what the
//  chip needs there, and a register write that lands while the chip is busy is
//  simply lost -- which on this soundtrack is a note that never keys on, on the
//  one machine the port goes out of its way to run fastest.
//
//  So the chip is asked.  Bit 0 of the status byte at C4 is BUSY, and it is
//  clear again the moment the previous access has been taken.  Twenty-two
//  T-states on a Z80, where it is never actually set; as many microseconds as
//  an R800 needs, on an R800.  Reading the status has no side effects.
//
//  It does not have a bail-out count, deliberately.  The stream is bounded, the
//  mapper window is bounded, and the parser checks both -- but BUSY clearing is
//  a property of a chip that answered a two-stage probe at boot, and a bound
//  here could only ever fire on a cartridge that had stopped responding
//  mid-song.  A count would cost seven T-states in the busiest loop this port
//  has to buy a check against hardware failure.
// ─────────────────────────────────────────────────────────────────────────────

const LVGM_Header* g_LVGM_Header;
const u8* g_LVGM_Pointer;
const u8* g_LVGM_LoopAddr;
u8 g_LVGM_Devices;
u8 g_LVGM_PSG_Default;
u8 g_LVGM_Wait;
u8 g_LVGM_State;
u8 g_LVGM_CurChip;
void (*g_LVGM_Decode)(void);
LVGM_NotifyCB g_LVGM_Callback;

const u8 g_LVGM_Ident[4] = { 'l', 'V', 'G', 'M' };

static const u8 g_msx2_lvgm_reg_table[13] =
    { 0, 1, 3, 5, 6, 6, 8, 8, 9, 9, 10, 10, 13 };

// lVGM's OPLL block opcodes name a run of registers by index: 4x/5x copy or
// fill Cnt[x] registers starting at Reg[x].
static const u8 g_msx2_opll_cnt[7] = { 8, 9, 9, 9, 3, 3, 3 };
static const u8 g_msx2_opll_reg[7] =
    { 0x00, 0x10, 0x20, 0x30, 0x16, 0x26, 0x36 };

// -----------------------------------------------------------------------------
//  Writing an FM register takes time the decoder does not otherwise spend
//
//  Both chips latch a register index on one port and its value on another, and
//  both demand a gap: 12 chip clocks after the index write and 84 after the
//  data write, the chip clock being the same 3.58 MHz the Z80 runs at.  A
//  single lVGM opcode can write eighteen consecutive registers from one tight
//  loop, which does NOT spend 84 T-states per iteration, so the wait is
//  written out here rather than assumed.  `ex (sp), hl` is one byte and 19
//  T-states and leaves HL, SP and the stack contents exactly as it found them,
//  which is what makes it usable as a delay inside a compiled function.
// -----------------------------------------------------------------------------

static void Msx2_LvgmOpllWrite(u8 reg, u8 val)
{
	g_MSXMusic_IndexPort = reg;
	__asm
		ex (sp), hl
	__endasm;
	g_MSXMusic_DataPort = val;
	__asm
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
	__endasm;
}

static void Msx2_LvgmOpl1Write(u8 reg, u8 val)
{
	g_MSXAudio_IndexPort = reg;
	__asm
		ex (sp), hl
	__endasm;
	g_MSXAudio_DataPort = val;
	__asm
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
		ex (sp), hl
	__endasm;
}

// Key-off every voice and take the chip out of rhythm mode.  A tune stopped
// mid-note otherwise holds that note for ever: unlike the PSG, whose whole
// register file this port rewrites every frame, an FM voice sustains until
// something keys it off.  (The two routines were once one, parameterised by
// base register: SDCC made the shared version and its two call sites add up to
// slightly MORE than the two copies, so they are copies.)
static void Msx2_LvgmOpllMute(void)
{
	u8 i;
	Msx2_LvgmOpllWrite(0x0E, 0x00);   // rhythm mode off
	for(i = 0; i < 9; ++i)
		Msx2_LvgmOpllWrite((u8)(0x20 + i), 0x00);
}

static void Msx2_LvgmOpl1Mute(void)
{
	u8 i;
	Msx2_LvgmOpl1Write(0xBD, 0x00);   // rhythm mode off
	for(i = 0; i < 9; ++i)
		Msx2_LvgmOpl1Write((u8)(0xB0 + i), 0x00);
}

// The MoonSound, silenced the way its own recordings start.
//
// DAMP rather than key-off: bit 6 of R#68+n runs the wave channel's envelope
// down to zero at the damp rate instead of releasing it, which is what stops a
// held sample immediately rather than letting it ring out over the next screen.
// Every one of these recordings opens by writing 0x40 into all twenty-four of
// those registers, so this is the state the next track expects to start from.
//
// The FM half is an OPL3 and gets the same key-off the other two chips get, in
// both register banks.  These recordings do not use it -- they set it up and
// key it off at bar one -- but a chip left keyed on by a track that did would
// hold that note under the whole menu.
static void Msx2_LvgmOpl4Mute(void) __naked
{
	// Four runs of consecutive registers, each (index port, first register,
	// count, value).  Written as a table rather than as four unrolled loops
	// because every one of the ten register writes below needs its own BUSY
	// poll in front of it, and ten copies of the poll is most of what the two
	// resident code areas have left.  The data port is always the index port
	// plus one, on both halves of the chip.
	__asm
		ld		hl, #2$
	1$:
		ld		c, (hl)			; index port -- a zero ends the table
		inc		hl
		ld		a, c
		or		a, a
		ret		Z
		ld		e, (hl)			; first register
		inc		hl
		ld		b, (hl)			; how many of them
		inc		hl
		ld		d, (hl)			; the value they all take
		inc		hl
		push	hl
	3$:
		in		a, (#0xC4)
		rra
		jr		C, 3$
		ld		a, e
		out		(c), a
		inc		c				; ... and the data port is the next one along
	4$:
		in		a, (#0xC4)
		rra
		jr		C, 4$
		ld		a, d
		out		(c), a
		dec		c
		inc		e
		djnz	3$
		pop		hl
		jr		1$
	2$:
		.db		0x7E, 0x68, 24, 0x40	; DAMP the 24 wave channels
		.db		0xC4, 0xB0,  9, 0x00	; key-off, FM register bank 0
		.db		0xC6, 0xB0,  9, 0x00	; key-off, FM register bank 1
		.db		0xC4, 0xBD,  1, 0x00	; and out of rhythm mode
		.db		0x00
	__endasm;
}

void LVGM_Pause(void)
{
	g_LVGM_State &= (u8)~LVGM_STATE_PLAY;
	PSG_Mute();
	// Only the chips this recording actually drives.  A machine that has an
	// OPLL but is playing the PSG title theme must not have its FM voices
	// cleared out from under a track that never touched them.
	if(g_LVGM_Devices & LVGM_CHIP_MSXMUSIC)
		Msx2_LvgmOpllMute();
	if(g_LVGM_Devices & LVGM_CHIP_MSXAUDIO)
		Msx2_LvgmOpl1Mute();
	if(g_LVGM_Devices & LVGM_CHIP_MOONSOUND)
		Msx2_LvgmOpl4Mute();
}

bool LVGM_Play(const void* addr, bool loop)
{
	g_LVGM_Header = (const LVGM_Header*)addr;
	// gen_msx_audio.py rejects every non-lVGM input and emits the fixed
	// five-byte header used here.  Keep this hot path small; the ROM never
	// accepts hand-authored streams at runtime.
	if(g_LVGM_Header->Ident[0] != 'l' || g_LVGM_Header->Ident[1] != 'V' ||
	   g_LVGM_Header->Ident[2] != 'G' || g_LVGM_Header->Ident[3] != 'M')
		return FALSE;

	g_LVGM_State = (g_LVGM_Header->Option & LVGM_OPTION_50HZ) ?
		LVGM_STATE_50HZ : 0;
	if(loop)
		g_LVGM_State |= LVGM_STATE_LOOP;

	// THE HEADER IS NOT A FIXED SIX BYTES ANY MORE.
	// A PSG recording has no device list and one common-value byte; an OPLL or
	// MSX-AUDIO one has a device list and no common value.  The window offset
	// of the first command therefore varies, and msx2_audio.c needs it too --
	// it is the fallback loop address for a stream with no FE marker -- so it
	// is computed once here and published rather than hardcoded twice.
	{
		const u8* p = (const u8*)addr + 5;
		g_LVGM_Devices = (g_LVGM_Header->Option & LVGM_OPTION_DEVICE) ?
			*p++ : LVGM_CHIP_PSG;
		g_LVGM_PSG_Default = (g_LVGM_Devices & LVGM_CHIP_PSG) ? *p++ : 0;
		g_LVGM_Pointer = p;
	}
	g_LVGM_LoopAddr = g_LVGM_Pointer;
	g_LVGM_Wait = 0;
	// lVGM's own default: a stream that never emits a chip marker is PSG data.
	g_LVGM_CurChip = LVGM_CHIP_PSG;
	LVGM_Pause();
	g_LVGM_State |= LVGM_STATE_PLAY;
	return TRUE;
}

void LVGM_Stop(void)
{
	LVGM_Pause();
	g_LVGM_Pointer = (const u8*)g_LVGM_Header + 2;
	g_LVGM_Wait = 0;
}

static void Msx2_LvgmDecodePsg(void)
{
	u8 op = *g_LVGM_Pointer & 0xF0;

	switch(op)
	{
		case 0x00:
		{
			u8 reg = *g_LVGM_Pointer;
			PSG_SetRegister(reg, *++g_LVGM_Pointer);
			break;
		}
		case 0xD0:
		{
			u8 reg = *g_LVGM_Pointer & 0x0F;
			PSG_SetRegister(reg, g_LVGM_PSG_Default);
			break;
		}
		default:
		{
			PSG_SetRegister(g_msx2_lvgm_reg_table[*g_LVGM_Pointer >> 4],
			                *g_LVGM_Pointer);
		}
	}
}

// An OPLL chunk.  Registers 00-38 are written singly by the 0x/1x/2x/3x forms
// and cleared by their 8x/9x/Ax/Bx counterparts.  The four forms in between
// all say the same thing -- write a RUN of consecutive registers -- and differ
// only in where the run starts and whether the values come one per register or
// one for all of them, so they share the single loop at the bottom rather than
// getting a copy of it each.  That is not tidiness: four copies of the loop
// cost about a hundred bytes of _CODE, and _CODE is what this port is out of.
//
// Every form leaves the pointer on its LAST consumed byte, because the outer
// parser advances it once more.
// ITS LOCALS ARE FILE STATICS, and that is a hard requirement rather than a
// micro-optimisation.  Six locals is enough for SDCC to build an IX frame, and
// the frame is set up by a CALL to ___sdcc_enter_ix -- a library helper that
// links into _CODE just under 0xBC00.  This function runs inside the V-blank
// handler with the music segment mapped over 0x8000, so that helper is not
// there: the call lands in the middle of the recording, and the machine dies a
// few seconds later with a stack pointer made of music.  Nothing on this path
// may have a frame.  (The decoder is not re-entrant either way: it runs only
// from the ISR, with interrupts off.)
static u8 g_opll_op, g_opll_high, g_opll_cnt, g_opll_reg, g_opll_val;
static bool g_opll_fill;

static void Msx2_LvgmDecodeOpll(void)
{
	g_opll_op = *g_LVGM_Pointer;
	g_opll_high = (u8)(g_opll_op >> 4);

	if(g_opll_high <= 0x3)                   // rr nn | R#rr = nn
	{
		Msx2_LvgmOpllWrite(g_opll_op, *++g_LVGM_Pointer);
		return;
	}
	if(g_opll_high >= 0x8)                   // 8x-Bx | R#(op & 7F) = 0
	{
		Msx2_LvgmOpllWrite((u8)(g_opll_op & 0x7F), 0);
		return;
	}

	if(g_opll_high <= 0x5)                   // 4x nn[] / 5x nn | a run this table names
	{
		g_opll_cnt = g_msx2_opll_cnt[g_opll_op & 0x0F];
		g_opll_reg = g_msx2_opll_reg[g_opll_op & 0x0F];
		g_opll_fill = (g_opll_high == 0x5);
	}
	else                              // 6n rr vv / 7n rr vv[] | a run from rr
	{
		g_opll_cnt = (u8)((g_opll_op & 0x0F) + 3);
		g_opll_reg = *++g_LVGM_Pointer;
		g_opll_fill = (g_opll_high == 0x6);
	}

	// Two loops, not one with the choice inside it: hoisting the test out is
	// what makes the shared setup above pay for itself at all.
	if(g_opll_fill)
	{
		g_opll_val = *++g_LVGM_Pointer;
		while(g_opll_cnt--)
			Msx2_LvgmOpllWrite(g_opll_reg++, g_opll_val);
	}
	else
	{
		while(g_opll_cnt--)
			Msx2_LvgmOpllWrite(g_opll_reg++, *++g_LVGM_Pointer);
	}
}

// An MSX-AUDIO chunk is the plain form: a register byte and its value.  The
// Y8950's registers stop at C8, so a register byte can never collide with the
// outer parser's Ex waits or Fx markers.
static void Msx2_LvgmDecodeOpl1(void)
{
	u8 reg = *g_LVGM_Pointer;
	Msx2_LvgmOpl1Write(reg, *++g_LVGM_Pointer);
}

// ─────────────────────────────────────────────────────────────────────────────
//  A MoonSound chunk, which is where this port's V-blank budget actually goes
//
//  Every command is three bytes -- port, register, value -- and one busy frame
//  of the boss theme is over 180 of them.  Two I/O writes each, each waiting on
//  the chip's BUSY bit first, is why this one decoder is written out in
//  assembly while the other three are C: at ~140 T-states a write it is already
//  four tenths of the worst frame in the soundtrack, and the C the other
//  decoders compile to is half as fast again.  It also consumes the WHOLE run
//  rather than one command, so the outer parser's per-command overhead is paid
//  once for a hundred writes instead of a hundred times.
//
//  A is the poll's own register, so the byte on its way to the chip waits in E
//  between the poll and the write.
//
//  The run ends on anything that is not a port number: a wait (Ex), a marker
//  (Fx), or a byte that is neither, which cannot be a valid command and would
//  otherwise be played into the chip as one.  On that byte the pointer is left
//  one BEFORE it, because the outer parser advances once more -- the same
//  convention the other three decoders keep.
//
//  It also stops after 256 writes, which is what `djnz` gives for free.  That
//  is not a music limit (nothing here writes that many in a frame); it is the
//  bound that keeps a stream which has walked out of its own segment from
//  running the ISR off the end of the mapper window, and LVGM_Decode() checks
//  the pointer against the window when it comes back.
// ─────────────────────────────────────────────────────────────────────────────
static void Msx2_LvgmDecodeOpl4(void) __naked
{
	__asm
		ld		hl, (_g_LVGM_Pointer)
		ld		b, #0			; 256 writes, then hand the frame back
	1$:
		ld		a, (hl)			; the port: 0 and 1 the FM banks, 2 the wave
		sub		a, #2
		jr		NZ, 3$

		; ---- port 2, the wave half at 7E/7F: almost every command --------
		inc		hl
		ld		e, (hl)
		inc		hl
	6$:
		in		a, (#0xC4)
		rra
		jr		C, 6$
		ld		a, e
		out		(#0x7E), a		; MSX2_MOON_WAVE_INDEX
		ld		e, (hl)
		inc		hl
	7$:
		in		a, (#0xC4)
		rra
		jr		C, 7$
		ld		a, e
		out		(#0x7F), a		; MSX2_MOON_WAVE_DATA
		djnz	1$
		jr		2$				; 256 writes: come back for the rest

		; ---- the terminator, and the two FM banks ------------------------
		; The FM half is forty writes in the whole soundtrack -- an init and a
		; key-off at bar one -- so unlike the wave half it does not get its own
		; unrolled path: the index port comes out of the port number (0 -> C4,
		; 1 -> C6) and the write goes through BC.
	3$:
		add		a, #2			; the port byte again
		cp		a, #2
		jr		NC, 2$			; not a port at all: the run ends here
		add		a, a
		add		a, #0xC4
		ld		c, a
		inc		hl
		ld		e, (hl)
		inc		hl
	8$:
		in		a, (#0xC4)
		rra
		jr		C, 8$
		ld		a, e
		out		(c), a			; the index port
		inc		c
		ld		e, (hl)
		inc		hl
	9$:
		in		a, (#0xC4)
		rra
		jr		C, 9$
		ld		a, e
		out		(c), a			; ... and the data port beside it
		djnz	1$

		; Both exits leave HL on the byte the parser should look at next, and
		; the parser increments before it looks: step back over it.
	2$:
		dec		hl
		ld		(_g_LVGM_Pointer), hl
		ret
	__endasm;
}

// A FRAME HAS TO END.
// The parser below runs inside the V-blank handler, with interrupts off and
// the music segment mapped over the resident code at 0x8000, and it only hands
// the frame back when it meets a wait (0xEx) or the end of the song.  A
// recording that never presents one -- a stream packed wrong, a pointer that
// walked out of its segment, a segment that failed to map -- therefore does not
// play badly: it never returns, and the machine is dead with interrupts
// disabled.  A frame of this music is a couple of dozen commands, so a budget
// this size cannot be reached by a healthy song, and reaching it stops the
// track.  Silence is recoverable; a wedged V-blank is not.
#define LVGM_FRAME_COMMAND_BUDGET  512

void LVGM_Decode(void)
{
	u16 budget = LVGM_FRAME_COMMAND_BUDGET;

	if(!(g_LVGM_State & LVGM_STATE_PLAY))
		return;
	if(g_LVGM_Wait != 0)
	{
		--g_LVGM_Wait;
		return;
	}

	while(TRUE)
	{
		u8 op = *g_LVGM_Pointer & 0xF0;

		if(--budget == 0)
		{
			// Whatever this stream is, it is not a frame of music.
			LVGM_Stop();
			return;
		}
		if(op == 0xE0)
		{
			g_LVGM_Wait = (u8)(g_LVGM_Wait + (*g_LVGM_Pointer & 0x0F));
			++g_LVGM_Pointer;
			return;
		}
		if(op == 0xF0)
		{
			switch(*g_LVGM_Pointer)
			{
				// Which chip the bytes that follow belong to.  A recording
				// that names no chip is PSG data, which is what LVGM_Play
				// leaves in g_LVGM_CurChip.
				case LVGM_OP_PSG:
					g_LVGM_CurChip = LVGM_CHIP_PSG;
					break;
				case LVGM_OP_OPLL:
					g_LVGM_CurChip = LVGM_CHIP_MSXMUSIC;
					break;
				case LVGM_OP_OPL1:
					g_LVGM_CurChip = LVGM_CHIP_MSXAUDIO;
					break;
				case LVGM_OP_OPL4:
					g_LVGM_CurChip = LVGM_CHIP_MOONSOUND;
					break;

				case LVGM_OP_NOTIFY:
					if(Msx2_LvgmNotify(*++g_LVGM_Pointer))
						continue;
					// The callback refused the marker: the only refusal it has
					// is a segment end with no segment after it, and carrying
					// on would parse whatever lies past the recording as if it
					// were music, for ever.  Stop the track instead.
					LVGM_Stop();
					return;
				case LVGM_OP_LOOP:
					Msx2_LvgmNotify(LVGM_NOTIFY_LOOP_MARK);
					g_LVGM_LoopAddr = g_LVGM_Pointer + 1;
					break;
				case LVGM_OP_END:
					if(g_LVGM_State & LVGM_STATE_LOOP)
					{
						Msx2_LvgmNotify(LVGM_NOTIFY_LOOP_JUMP);
						g_LVGM_Pointer = g_LVGM_LoopAddr;
						continue;
					}
					LVGM_Stop();
					return;
			}
		}
		else if(g_LVGM_CurChip == LVGM_CHIP_MSXMUSIC)
			Msx2_LvgmDecodeOpll();
		else if(g_LVGM_CurChip == LVGM_CHIP_MSXAUDIO)
			Msx2_LvgmDecodeOpl1();
		else if(g_LVGM_CurChip == LVGM_CHIP_MOONSOUND)
		{
			Msx2_LvgmDecodeOpl4();
			// The only thing that can leave the pointer above the mapper
			// window is a stream that ran past its own segment without the
			// FD 00 that would have mapped the next one.  Past the window is
			// RAM, and a run of RAM read as OPL4 commands is a chip full of
			// noise; the budget below would stop it eventually, but only
			// after a few hundred thousand register writes.
			if((u16)g_LVGM_Pointer >= 0xC000)
			{
				LVGM_Stop();
				return;
			}
		}
		else
			Msx2_LvgmDecodePsg();
		++g_LVGM_Pointer;
	}
}
