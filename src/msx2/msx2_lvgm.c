// A small resident lVGM player for the formats this port packs: 60 Hz PSG,
// MSX-MUSIC (YM2413/OPLL) and MSX-AUDIO (Y8950) streams with optional FD
// notifications and FE/FF loop markers.
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
// the F0/F1/F2 markers, and msx2_audio.c picks the recording.

#include "vgm/lvgm_player.h"
#include "msx2_audio.h"

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
		else
			Msx2_LvgmDecodePsg();
		++g_LVGM_Pointer;
	}
}
