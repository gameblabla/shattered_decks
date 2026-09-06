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
#include "msx2_video.h"   // g_msx2_msxver, written here and read by the duel

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

// ─────────────────────────────────────────────────────────────────────────────
//  The MoonSound (YMF278B / OPL4)
//
//  It is an I/O cartridge, not a slot one, so unlike the OPLL it needs no slot
//  walk and no BIOS in page 0: it either answers on its ports or it is not
//  there.  It is found in two steps, and it takes both.
//
//  STEP ONE IS THE OPL TIMER TEST, the same one the Y8950 gets a few lines up
//  and for the same reason -- an MSX with nothing at 0xC4 floats the bus, and a
//  bus that floats to a plausible byte is what a naive read would accept.  The
//  FM half of an OPL4 is an OPL3, so its timer registers are the familiar
//  02/03/04 and its status byte carries IRQ in bit 7 and the two timer flags in
//  6 and 5.
//
//  Bits 0 and 1 of that byte are NOT OPL3 bits: they are the OPL4's own BUSY
//  and LD, and a write made microseconds earlier may still be holding either.
//  Only the three OPL3 bits are compared.
//
//  STEP TWO IS WHAT MAKES IT AN OPL4 rather than a bare OPL3 cartridge, which
//  exists and which would pass step one and then play nothing at all: these
//  recordings are almost entirely the WAVE half, and on an OPL3 there is no
//  wave half.  So the wave half is asked to load an instrument, and the LD bit
//  is watched.  Two things about that:
//
//   * the wave registers ignore every write until NEW2 is set, which is bit 1
//     of OPL3 register 0x105 -- bank 1, register 05, so port C6 then C7.  This
//     is not a courtesy: with NEW2 clear the write below does nothing and a
//     real MoonSound reports itself absent;
//
//   * a write to R#08-1F, the wave table number of one of the 24 channels,
//     sends the chip off to read that instrument's header out of the wave ROM
//     and it raises LD for about half a millisecond while it does.  The read
//     that follows is a few microseconds later, so it sees the bit up.
//
//  Channel 0's wave number is set to 0 and left there; every recording rewrites
//  all twenty-four before its first note.
// ─────────────────────────────────────────────────────────────────────────────
__sfr __at(0x7E) g_probe_moon_wave_index;
__sfr __at(0x7F) g_probe_moon_wave_data;
__sfr __at(0xC4) g_probe_moon_fm0_index;   // written: index.  read: status.
__sfr __at(0xC5) g_probe_moon_fm0_data;
__sfr __at(0xC6) g_probe_moon_fm1_index;
__sfr __at(0xC7) g_probe_moon_fm1_data;

#define MOON_MASK_TIMERS   0x60   // T1 and T2 masked, neither running
#define MOON_RESET_FLAGS   0x80   // clear the flags and drop the IRQ line
#define MOON_RUN_T1        0x21   // start timer 1, leave T2 masked
#define MOON_STATUS_LD     0x02   // the wave half is loading an instrument

// The chip wants a gap between an index write and its data write: about 10
// T-states on the FM half and 15 on the wave half, at the 3.58 MHz the Z80
// runs at.  Reaching this at all costs a CALL and a RET, which is 27 of them
// before the body has run, so the body only has to exist; `ex (sp), hl` is one
// byte and 19 T-states and comes in PAIRS, since a single one would leave HL
// holding the return address.  msx2_lvgm.c uses the same instruction in the
// player, where there is no call to pay for it.
//
// It is a function in THIS file, which is the rule the whole bank lives by: a
// call out of it would land in the middle of segment 5's own bytes.
//
// Counted T-states are enough HERE and not in the player, because the probe
// runs before Msx2_CpuFast() does: whatever machine this is, it is still a
// 3.58 MHz Z80 when these lines execute, and a call, two swaps and a return is
// eighteen microseconds.  The player has no such guarantee and waits on the
// chip's own BUSY bit instead (msx2_lvgm.c).  Polling BUSY would be circular
// in a routine whose whole job is to find out whether the chip is there.
static void Msx2_ProbeSettle(void)
{
	__asm
		ex		(sp), hl
		ex		(sp), hl
	__endasm;
}
#define MOON_SETTLE  Msx2_ProbeSettle()

static bool Msx2_ProbeMoonSound(void)
{
	u8 status;
	u16 spin;

	g_probe_moon_fm0_index = 0x04;
	MOON_SETTLE;
	g_probe_moon_fm0_data = MOON_MASK_TIMERS;
	MOON_SETTLE;
	g_probe_moon_fm0_index = 0x04;
	MOON_SETTLE;
	g_probe_moon_fm0_data = MOON_RESET_FLAGS;
	MOON_SETTLE;
	if((g_probe_moon_fm0_index & 0xE0) != 0x00)
		return FALSE;              // a real OPL3 now reads a clear status

	g_probe_moon_fm0_index = 0x02; // timer 1 preset
	MOON_SETTLE;
	g_probe_moon_fm0_data = 0xFF;  // ... at its shortest, 80 us
	MOON_SETTLE;
	g_probe_moon_fm0_index = 0x04;
	MOON_SETTLE;
	g_probe_moon_fm0_data = MOON_RUN_T1;
	// Comfortably longer than 80 us at 3.58 MHz whatever this compiles to,
	// and bounded, so a machine that never raises the flag still leaves.
	for(spin = 0; spin < 400; ++spin)
		;
	status = g_probe_moon_fm0_index;

	g_probe_moon_fm0_index = 0x04; // stop the timer again, whatever we saw
	MOON_SETTLE;
	g_probe_moon_fm0_data = MOON_MASK_TIMERS;
	MOON_SETTLE;
	g_probe_moon_fm0_index = 0x04;
	MOON_SETTLE;
	g_probe_moon_fm0_data = MOON_RESET_FLAGS;
	MOON_SETTLE;
	if((status & 0xE0) != 0xC0)
		return FALSE;

	g_probe_moon_fm1_index = 0x05; // R#105: NEW and NEW2
	MOON_SETTLE;
	g_probe_moon_fm1_data = 0x03;
	MOON_SETTLE;
	g_probe_moon_wave_index = 0x08; // wave table number, channel 0
	MOON_SETTLE;
	g_probe_moon_wave_data = 0x00;
	MOON_SETTLE;
	return (g_probe_moon_fm0_index & MOON_STATUS_LD) != 0;
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
static const Msx2MusicAsset g_probe_music_moonsound[MSX2_MUSIC_ASSET_COUNT] =
	MSX2_MUSIC_TABLE_MOONSOUND;

// The sound-effect cues, here for the same reason the music tables are: they
// are const data, const data is _CODE, and _CODE is the area this cartridge
// runs out of first.  msx2_audio.h carries the table and the argument.
static const Msx2SfxStep g_probe_sfx_steps[MSX2_SFX_STEP_COUNT] =
	MSX2_SFX_STEPS;
static const u8 g_probe_sfx_first[MSX2_SFX_COUNT + 1] = MSX2_SFX_FIRST;

// MoonSound first, then MSX-AUDIO: best chip wins, and a machine that has more
// than one is choosing between complete recordings, not between a recording
// and silence.  The OPL4 goes first for the obvious reason -- it is a sampler
// and the other three are not -- and because its probe is the cheapest of the
// three: it is I/O-mapped, so unlike the OPLL it costs no slot walk.
//
// `table` is filled with the winning chip's records, with the PSG record
// standing in wherever that chip has no rendition of a track.  Resolving the
// fallback here rather than at play time
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

u8 Msx2_AudioSetup_In(Msx2MusicAsset* table, Msx2SfxStep* steps, u8* first)
{
	u8* out;
	const u8* in;
	u8 n;

	// THE MACHINE'S OWN VERSION BYTE, TAKEN WHILE THE BIOS IS REACHABLE.
	// It is read here rather than anywhere more obvious because this is the
	// only routine in the port that already puts the BIOS back in page 0:
	// 0x002D of the main ROM is otherwise this cartridge's own segment 2.
	// The duel screen uses it to decide how much of the board a camera move
	// may draw -- an R800 has the cycles for every line of it (msx2_floor.c).
	Msx2_ProbeBiosIn();
	g_msx2_msxver = Msx2_ProbeSlotRead(g_EXPTBL[0], R_MSXVER);
	Msx2_ProbeBiosOut();

	if(Msx2_ProbeMoonSound())
	{
		g_probe_chip = MSX2_CHIP_MOONSOUND;
		g_probe_chosen = g_probe_music_moonsound;
	}
	else if(Msx2_ProbeMsxAudio())
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

	// The cues, byte for byte for the same reason: ___memcpy is in _CODE above
	// 0x8000, which is where this file is standing.
	in = (const u8*)g_probe_sfx_steps;
	out = (u8*)steps;
	for(g_probe_i = 0; g_probe_i < sizeof(g_probe_sfx_steps); ++g_probe_i)
		*out++ = *in++;
	in = g_probe_sfx_first;
	out = first;
	for(g_probe_i = 0; g_probe_i < sizeof(g_probe_sfx_first); ++g_probe_i)
		*out++ = *in++;
	return g_probe_chip;
}
