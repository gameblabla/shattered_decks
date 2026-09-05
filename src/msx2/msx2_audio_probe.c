// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio_probe.c — finding the machine's sound chips, once, at boot
//
//  WHY THIS IS A BANK AND NOT PART OF msx2_audio.c.  _CODE is a hard 32 KB and
//  the FM decoders that have to be resident already spend most of what was
//  left of it.  A probe runs once, before the title screen, and never again --
//  it is the cheapest thing in the port to move out.  It lives in cartridge
//  segment 5, linked at 0x8000 (waifu_msx2_s5_b2.c), which the caller maps
//  through the same window the streamer and the music use.
//
//  THE THREE RULES THIS FILE OBEYS, all of which follow from where it runs:
//
//   * it calls NOTHING outside itself.  While segment 5 is in the window,
//     everything _CODE put above 0x8000 is gone -- and MSXgl's own
//     Sys_CheckSlot and BIOS_InterSlotRead are up there, at 0xBD32 and 0xB3C7.
//     Calling either from here would execute probe bytes.  The slot walk and
//     the inter-slot read are therefore written out below;
//
//   * it puts the BIOS back in page 0 around the inter-slot reads.  RDSLT is a
//     BIOS entry point at 0x000C, and page 0 in this port is a cartridge code
//     bank, so the routine is simply not there until the slot register is
//     changed.  This is the same dance msx2_disk.c does, and for the same
//     reason;
//
//   * it keeps interrupts off for the whole scan.  The ISR is in RAM and would
//     survive, but it calls into the audio tick, and having the music start
//     mid-probe -- with page 0 pointing at the BIOS and the window pointing at
//     this file -- is not a state worth reasoning about.  RDSLT returns with
//     interrupts already disabled, so unlike MSXgl's wrapper the read below
//     does not turn them back on.
// ─────────────────────────────────────────────────────────────────────────────

#include "msxgl.h"
#include "msx2_audio.h"
#include "msx2_scenes.h"

// "APRLOPLL": a built-in MSX-MUSIC answers to all eight bytes at 0x4018, an
// FM-PAC to the last four at 0x401C.  One string, one probe, two start
// offsets -- the addresses line up because 0x4018 + 4 is 0x401C.
static const c8 g_probe_opll_ident[8] =
	{ 'A', 'P', 'R', 'L', 'O', 'P', 'L', 'L' };

static u8 g_probe_first;    // 0 for a built-in MSX-MUSIC, 4 for an FM-PAC
static u8 g_probe_opll_slot;

static u8 g_probe_slot_save;

// Select the primary slot the BIOS booted from in page 0, so that 0x000C is
// RDSLT and not this cartridge.  Page 3 (the stack and EXPTBL) is untouched.
static void Msx2_ProbeBiosIn(void)
{
	__asm
		di
		in		a, (#0xA8)
		ld		(_g_probe_slot_save), a
		and		a, #0xFC
		ld		hl, #_g_EXPTBL
		or		a, (hl)
		and		a, #0x03
		ld		b, a
		ld		a, (_g_probe_slot_save)
		and		a, #0xFC
		or		a, b
		out		(#0xA8), a
	__endasm;
}

// Interrupts stay off across the whole probe: Msx2_AudioSetup() in
// msx2_bank.c owns them, because everything in this file runs with the upper
// half of _CODE -- the resident lVGM decoder the V-blank handler calls --
// swapped out from under it.
static void Msx2_ProbeBiosOut(void)
{
	__asm
		ld		a, (_g_probe_slot_save)
		out		(#0xA8), a
	__endasm;
}

// RDSLT and WRSLT, the BIOS's inter-slot accessors, with the same argument
// registers MSXgl's bios.c uses (slot in A, address in DE, value on the
// stack).  Neither turns interrupts back on, unlike the engine's wrappers:
// the whole scan runs with them off.
static u8 Msx2_ProbeSlotRead(u8 slot, u16 addr) __NAKED
{
	slot; addr;
	__asm
		ex		de, hl
		call	#0x000C     // RDSLT
		ret
	__endasm;
}

// NOT __NAKED, unlike the read above, and that is not a style choice: `value`
// is the one argument SDCC passes on the stack, and with sdcccall(1) it is the
// CALLEE that takes it back off.  A naked body ending in a bare RET leaves the
// byte behind, every caller drifts the stack by one, and the eventual return
// lands somewhere arbitrary -- which is what silently turned "an FM-PAC is
// present" into a machine that reported no FM chip at all.  Letting SDCC write
// the epilogue is what pops it; this is exactly MSXgl's BIOS_InterSlotWrite.
static void Msx2_ProbeSlotWrite(u8 slot, u16 addr, u8 value)
{
	slot;  // A
	addr;  // DE
	value; // (SP+4)

	__asm
		push	iy
		ld		iy, #4
		add		iy, sp
		ld		l, e
		ld		h, d
		ld		e, 0(iy)
		call	#0x0014     // WRSLT
		pop		iy
	__endasm;
}

// Does `slot` hold the OPLL ident, from g_probe_first onwards?
static u8 g_probe_i;

static bool Msx2_ProbeOpllSlot(u8 slot)
{
	u8 i;
	for(i = g_probe_first; i < 8; ++i)
		if(g_probe_opll_ident[i] !=
		   Msx2_ProbeSlotRead(slot, (u16)(0x4018 + i)))
			return FALSE;
	return TRUE;
}

// MSXgl's Sys_CheckSlot, open-coded: every primary slot, and every secondary
// slot of an expanded one.  EXPTBL is a BIOS work area in page 3 RAM, which is
// mapped throughout.
static bool Msx2_ProbeScanOpll(void)
{
	u8 primary, secondary, id;

	for(primary = 0; primary < 4; ++primary)
	{
		if(g_EXPTBL[primary] & SLOT_EXP)
		{
			for(secondary = 0; secondary < 4; ++secondary)
			{
				id = (u8)SLOTEX(primary, secondary);
				if(Msx2_ProbeOpllSlot(id))
				{
					g_probe_opll_slot = id;
					return TRUE;
				}
			}
		}
		else if(Msx2_ProbeOpllSlot(primary))
		{
			g_probe_opll_slot = primary;
			return TRUE;
		}
	}
	return FALSE;
}

static bool g_probe_found;

static bool Msx2_ProbeOpll(void)
{
	Msx2_ProbeBiosIn();
	g_probe_first = 0;                 // a built-in MSX-MUSIC
	g_probe_found = Msx2_ProbeScanOpll();
	if(!g_probe_found)
	{
		g_probe_first = 4;             // ... or an FM-PAC
		g_probe_found = Msx2_ProbeScanOpll();
		if(g_probe_found)
			// An FM-PAC powers up with its OPLL switched off; bit 0 of
			// 0x7FF6 is the switch, and until it is set the chip ignores
			// every register write.
			Msx2_ProbeSlotWrite(
				g_probe_opll_slot, 0x7FF6,
				(u8)(Msx2_ProbeSlotRead(g_probe_opll_slot, 0x7FF6) | 0x01));
	}
	Msx2_ProbeBiosOut();
	return g_probe_found;
}

// The Y8950 is found by its timer, which is a property of the chip and not of
// the bus: mask every status source, reset the flags, start timer 1 at its
// shortest preset, and see the IRQ and T1 bits come up.  Reading the port
// alone cannot do this -- a machine with nothing at 0xC0 floats the bus, and
// on some machines that floats to a value a naive probe accepts.  There is no
// BIOS in this one, so it needs no page-0 dance.
//
// REGISTER 4 IS MASKS AS WELL AS TIMERS, and masking only the two timers is
// not enough: an idle Y8950 holds BUF_RDY, the ADPCM "send me a byte" flag,
// and with that unmasked the chip re-raises its IRQ the instant the reset
// clears it.  The status then reads 0x8E, the presence check sees a set bit it
// did not expect, and a machine that has the chip is reported as not having
// it.  So both ADPCM sources are masked too.
#define MSXAUDIO_MASK_ALL   0x78   // T1, T2, EOS and BUF_RDY all masked
#define MSXAUDIO_RESET      0x80   // clear the flags and drop the IRQ line
#define MSXAUDIO_RUN_T1     0x39   // MSXAUDIO_MASK_ALL, minus T1, plus start
// Bits 1 and 2 read as 1 on a real MSX-AUDIO whatever else is going on, so
// they are the only ones a quiet chip may have set.
#define MSXAUDIO_QUIET(s)   (((s) & 0xF9) == 0x00)

static bool Msx2_ProbeMsxAudio(void)
{
	u8 status;
	u16 spin;

	g_MSXAudio_IndexPort = 0x04;   // timer control and status masks
	g_MSXAudio_DataPort = MSXAUDIO_MASK_ALL;
	g_MSXAudio_IndexPort = 0x04;
	g_MSXAudio_DataPort = MSXAUDIO_RESET;
	if(!MSXAUDIO_QUIET(g_MSXAudio_IndexPort))
		return FALSE;              // a real Y8950 now reads a clear status

	g_MSXAudio_IndexPort = 0x02;   // timer 1 preset
	g_MSXAudio_DataPort = 0xFF;    // ... at its shortest, 80 us
	g_MSXAudio_IndexPort = 0x04;
	g_MSXAudio_DataPort = MSXAUDIO_RUN_T1;
	// Comfortably longer than 80 us at 3.58 MHz whatever this compiles to,
	// and bounded, so a machine that never raises the flag still leaves.
	for(spin = 0; spin < 400; ++spin)
		;
	status = g_MSXAudio_IndexPort;

	g_MSXAudio_IndexPort = 0x04;   // stop the timer again, whatever we saw
	g_MSXAudio_DataPort = MSXAUDIO_MASK_ALL;
	g_MSXAudio_IndexPort = 0x04;
	g_MSXAudio_DataPort = MSXAUDIO_RESET;
	return (status & 0xE0) == 0xC0;
}

// Where every recording is, one table per chip.  They are here rather than in
// _CODE because only one of them is ever consulted on a given machine, and the
// resolve step below turns that one into a flat RAM table the resident player
// indexes directly -- which also means the hot path no longer reads const data
// through a window it is about to change, a trap that once made every one-shot
// cue loop for ever.
static const Msx2MusicAsset g_probe_music_psg[MSX2_MUSIC_ASSET_COUNT] =
	MSX2_MUSIC_TABLE_PSG;
static const Msx2MusicAsset g_probe_music_opll[MSX2_MUSIC_ASSET_COUNT] =
	MSX2_MUSIC_TABLE_OPLL;
static const Msx2MusicAsset g_probe_music_msxaudio[MSX2_MUSIC_ASSET_COUNT] =
	MSX2_MUSIC_TABLE_MSXAUDIO;

// MSX-AUDIO first: it is the better chip of the two, and a machine that has
// both is choosing between two complete recordings, not between a recording
// and silence.
//
// `table` is filled with the winning chip's records, with the PSG record
// standing in wherever that chip has no rendition of a track -- there is no FM
// title theme, for one.  Resolving the fallback here rather than at play time
// is what keeps it out of the resident player entirely.
//
// NOTHING IN THIS FILE MAY CALL OUT OF IT, and that includes the calls SDCC
// makes on your behalf: ___sdcc_enter_ix for a stack frame and ___memcpy for a
// struct assignment both live in _CODE above 0x8000, which is precisely the
// memory segment 5 is standing in.  Either one is a CALL into the middle of
// this bank's own bytes.  So every local that would force a frame is a file
// static, and the table copy below is spelled out a byte at a time.
static const Msx2MusicAsset* g_probe_chosen;
static u8 g_probe_chip;

u8 Msx2_AudioSetup_In(Msx2MusicAsset* table)
{
	u8* out;
	const u8* in;
	u8 n;

	if(Msx2_ProbeMsxAudio())
	{
		g_probe_chip = MSX2_CHIP_MSXAUDIO;
		g_probe_chosen = g_probe_music_msxaudio;
	}
	else if(Msx2_ProbeOpll())
	{
		g_probe_chip = MSX2_CHIP_MSXMUSIC;
		g_probe_chosen = g_probe_music_opll;
	}
	else
	{
		g_probe_chip = MSX2_CHIP_PSG;
		g_probe_chosen = g_probe_music_psg;
	}

	for(g_probe_i = 0; g_probe_i < MSX2_MUSIC_ASSET_COUNT; ++g_probe_i)
	{
		in = (const u8*)(g_probe_chosen[g_probe_i].segment_count
		                 ? &g_probe_chosen[g_probe_i]
		                 : &g_probe_music_psg[g_probe_i]);
		out = (u8*)&table[g_probe_i];
		for(n = 0; n < sizeof(Msx2MusicAsset); ++n)
			*out++ = *in++;
	}
	return g_probe_chip;
}
