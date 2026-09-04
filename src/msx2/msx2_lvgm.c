// A small resident lVGM player for the one format this port packs: 60 Hz,
// PSG-only streams with optional FD notifications and FE/FF loop markers.
//
// MSXgl's generic player dispatches every PSG command through a function
// pointer.  That path is useful for a multi-chip desktop sample, but it is a
// poor fit for this ISR: the pointer is in RAM while the 0x8000 window is
// changing, and the SDCC indirect-call shim can leave a compact PSG command
// at the same byte forever on real openMSX.  The format and callbacks are
// unchanged; the resident PSG helper and outer parser are kept explicit here
// so their code and the mapper seam are both safe in the ISR.

#include "vgm/lvgm_player.h"

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

void LVGM_Pause(void)
{
	g_LVGM_State &= (u8)~LVGM_STATE_PLAY;
	PSG_Mute();
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
	// The generator always writes PSG-only streams with no device list.
	g_LVGM_Devices = LVGM_CHIP_PSG;
	g_LVGM_PSG_Default = ((const u8*)addr)[5];
	g_LVGM_Pointer = (const u8*)addr + 6;
	g_LVGM_LoopAddr = g_LVGM_Pointer;
	g_LVGM_Wait = 0;
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

void LVGM_Decode(void)
{
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
				case LVGM_OP_NOTIFY:
					if(g_LVGM_Callback(*++g_LVGM_Pointer))
						continue;
					break;
				case LVGM_OP_LOOP:
					g_LVGM_Callback(LVGM_NOTIFY_LOOP_MARK);
					g_LVGM_LoopAddr = g_LVGM_Pointer + 1;
					break;
				case LVGM_OP_END:
					if(g_LVGM_State & LVGM_STATE_LOOP)
					{
						g_LVGM_Callback(LVGM_NOTIFY_LOOP_JUMP);
						g_LVGM_Pointer = g_LVGM_LoopAddr;
						continue;
					}
					LVGM_Stop();
					return;
			}
		}
		else
			Msx2_LvgmDecodePsg();
		++g_LVGM_Pointer;
	}
}
