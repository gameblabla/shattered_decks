// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.c — resident lVGM player, and the boot-time chip probe
//
//  The lVGM decoder is deliberately kept in the V-blank path.  A recording is
//  a banked asset, not linked code: the ISR maps its current 16K segment into
//  0x8000, lets MSXgl decode one tick into the indirect PSG register buffer,
//  applies that buffer to the AY/YM2149, and restores the resident code
//  segment before returning to the game.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_audio.h"
#include "msx2_bank.h"
#include "msx2_scenes.h"
#include "vgm/lvgm_player.h"
#include "psg.h"
#ifdef MSX2_DEBUG_REGRESSION
#include "msx2_probe.h"
#endif

#define MSX2_NEO_AUDIO_WINDOW 0x8000

// lVGM keeps this pointer private in its header even though it is the only
// address that must be retargeted when a split stream loops into an earlier
// segment.  It is a public definition in the MSXgl module.
extern const u8* g_LVGM_LoopAddr;

static volatile u8 g_music_requested;
static u8 g_music_active;
static u16 g_music_first_segment;
static u16 g_music_segment;
static u8 g_music_segment_count;
static u16 g_music_loop_segment;
static u16 g_music_loop_offset;
static u8 g_audio_error;
static u8 g_audio_chip;
// The winning chip's asset table, resolved once at boot into RAM.  In RAM and
// not in _CODE for two reasons: three chip tables is 120 bytes of an area that
// has tens to spare, and RAM is mapped whatever the 0x8000 window holds -- the
// const table this replaces was read THROUGH that window, which is what once
// made every one-shot cue read its loop flag out of the music stream.
static Msx2MusicAsset g_music_table[MSX2_MUSIC_ASSET_COUNT];
static u8 g_sfx_pending;
#ifdef MSX2_DEBUG_REGRESSION
static u16 g_music_frames;
static u16 g_music_loops;
#endif

bool Msx2_LvgmNotify(u8 id)
{
	if(id == LVGM_NOTIFY_SEG_END)
	{
		u16 next = (u16)(g_music_segment + 1);
		if(next >= (u16)(g_music_first_segment + g_music_segment_count))
		{
			g_audio_error = 1;
			return FALSE;
		}
		// The decoder is currently executing from the old stream segment.  The
		// callback itself and every callee it uses are resident below 0x8000.
		Msx2_Bank2Leave(next);
		g_music_segment = next;
		g_LVGM_Pointer = (const u8*)0x8000;
		return TRUE; // LVGM_Decode restarts its parser at the new segment.
	}

	if(id == LVGM_NOTIFY_LOOP_MARK)
	{
		// LVGM's own loop pointer is an address, not a segment number.  Capture
		// both parts while the marker is visible; the final FF callback can then
		// remap a split recording before the library assigns that pointer.
		g_music_loop_segment = g_music_segment;
		g_music_loop_offset =
		    (u16)(g_LVGM_Pointer - (const u8*)0x8000 + 1);
	}
	else if(id == LVGM_NOTIFY_LOOP_JUMP)
	{
		// LVGM increments the pointer once after handling FF.  Assigning the
		// loop address here preserves that existing decoder convention while
		// making its address valid in the segment being selected.
		Msx2_Bank2Leave(g_music_loop_segment);
		g_music_segment = g_music_loop_segment;
		g_LVGM_LoopAddr =
		    (const u8*)(0x8000 + g_music_loop_offset);
#ifdef MSX2_DEBUG_REGRESSION
		++g_music_loops;
#endif
	}
	return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sound effects, on the PSG's third channel
//
//  There is one PSG and the music is already using it, so an effect BORROWS
//  channel C for as long as it lasts and hands it back.  That is the whole
//  design, and it is the one the Game Boy Yu-Gi-Oh games use for the same
//  reason: a menu blip or a card going up in smoke is short, it wants to be
//  heard over the tune rather than instead of it, and a third of the music is
//  a cheaper thing to lose for eight frames than a whole voice.
//
//  An effect is a run of STEPS, and the step table is in msx2_audio.h.
//
//  HANDING THE CHANNEL BACK.  lVGM writes register CHANGES, not a whole frame
//  of registers, so a channel an effect scribbles on stays scribbled until the
//  tune happens to move it again -- which for a held note is never.  So the
//  music's own channel C is captured when a cue takes the channel and written
//  back when the cue ends.  The one thing that is knowingly not handled is the
//  tune changing channel C *while* a cue holds it: that change is then undone
//  by the restore and stands wrong until the tune's next write to C.  Tracking
//  it costs four more comparisons a frame in resident code, and a cue is at
//  most two fifths of a second.
// ─────────────────────────────────────────────────────────────────────────────

// The mixer bits belonging to channel C.  g_PSG_Regs holds the register as the
// chip wants it, where a SET bit is a generator switched OFF (PSG_SetMixer
// stores the complement), so these two are cleared to make a sound.
#define SFX_MIX_C   0x24u   // tone C (bit 2) and noise C (bit 5)

// The cue table itself is in msx2_audio.h and in the boot bank: it is const
// data, and const data is _CODE, which is the area this port is out of.  These
// are the RAM copies Msx2_AudioSetup() fills; see the header for why.
static Msx2SfxStep g_sfx_steps[MSX2_SFX_STEP_COUNT];
static u8 g_sfx_first[MSX2_SFX_COUNT + 1];

#define SFX_IDLE   0xFFu

static u8  g_sfx_idx;      // step being played, or SFX_IDLE
static u8  g_sfx_end;      // one past the last step of the running cue
static u8  g_sfx_left;     // ticks left in this step
static u8  g_sfx_restore;  // a cue ended: give the music its channel back
static u16 g_sfx_period;
static i16 g_sfx_slide;
static u8  g_sfx_noise;
static u8  g_sfx_vol16;    // volume in sixteenths, so `decay` can be gentle
static u8  g_sfx_decay;
// The music's channel C, as it was when the cue took it.
static u16 g_sfx_bg_tone;
static u8  g_sfx_bg_vol;
static u8  g_sfx_bg_mix;
static u8  g_sfx_bg_noise;

static void Msx2_SfxLoadStep(void)
{
	// One address computation for the six fields: the step index is a byte and
	// the struct is eight bytes wide, but SDCC recomputes base + idx * 8 for
	// every member access unless it is handed a pointer.
	const Msx2SfxStep* step = &g_sfx_steps[g_sfx_idx];

	g_sfx_period = step->period;
	g_sfx_slide  = step->slide;
	g_sfx_noise  = step->noise;
	g_sfx_vol16  = (u8)(step->vol << 4);
	g_sfx_decay  = step->decay;
	g_sfx_left   = step->len;
}

// Called from the V-blank, after the music has decoded its frame and after the
// bank window has been given back to the code segment.
static void Msx2_SfxTick(void)
{
	if(g_sfx_restore)
	{
		g_PSG_Regs.Tone[2] = g_sfx_bg_tone;
		g_PSG_Regs.Volume[2] = g_sfx_bg_vol;
		g_PSG_Regs.Noise = g_sfx_bg_noise;
		g_PSG_Regs.Mixer =
			(u8)((g_PSG_Regs.Mixer & (u8)~SFX_MIX_C) | g_sfx_bg_mix);
		g_sfx_restore = 0;
	}

	if(g_sfx_pending < MSX2_SFX_COUNT)
	{
		if(g_sfx_idx == SFX_IDLE)
		{
			g_sfx_bg_tone = g_PSG_Regs.Tone[2];
			g_sfx_bg_vol = g_PSG_Regs.Volume[2];
			g_sfx_bg_noise = g_PSG_Regs.Noise;
			g_sfx_bg_mix = (u8)(g_PSG_Regs.Mixer & SFX_MIX_C);
		}
		g_sfx_idx = g_sfx_first[g_sfx_pending];
		g_sfx_end = g_sfx_first[g_sfx_pending + 1];
		Msx2_SfxLoadStep();
	}
	g_sfx_pending = MSX2_SFX_COUNT;
	if(g_sfx_idx == SFX_IDLE)
		return;

	g_PSG_Regs.Tone[2] = g_sfx_period;
	g_PSG_Regs.Volume[2] = (u8)(g_sfx_vol16 >> 4);
	if(g_sfx_noise != 0)
		g_PSG_Regs.Noise = g_sfx_noise;
	g_PSG_Regs.Mixer = (u8)((g_PSG_Regs.Mixer & (u8)~SFX_MIX_C)
		| ((g_sfx_period != 0) ? 0u : 0x04u)
		| ((g_sfx_noise != 0) ? 0u : 0x20u));

	g_sfx_period = (u16)(g_sfx_period + g_sfx_slide);
	g_sfx_vol16 = (g_sfx_vol16 > g_sfx_decay) ? (u8)(g_sfx_vol16 - g_sfx_decay)
	                                          : 0;
	if(--g_sfx_left == 0)
	{
		++g_sfx_idx;
		if(g_sfx_idx >= g_sfx_end)
		{
			g_sfx_idx = SFX_IDLE;
			g_sfx_restore = 1;
		}
		else
			Msx2_SfxLoadStep();
	}
}

static void Msx2_AudioStartRequested(void)
{
	u8 requested = g_music_requested;
	const Msx2MusicAsset* asset;
	u8 loop;
	u16 back;

	if(requested == g_music_active)
		return;

	if(g_music_active != MSX2_MUSIC_NONE)
	{
		back = Msx2_Bank2Enter(g_music_segment);
		LVGM_Stop();
		Msx2_Bank2Leave(back);
		g_music_active = MSX2_MUSIC_NONE;
	}

	if(requested >= MSX2_MUSIC_ASSET_COUNT)
	{
		g_music_requested = MSX2_MUSIC_NONE;
		return;
	}
	// Already resolved: the boot bank filled this table with the winning
	// chip's records, standing the PSG recording in wherever that chip has no
	// rendition of a track.
	asset = &g_music_table[requested];
	if(asset->segment_count == 0)
		return;

	// THE TABLE IS IN THE WINDOW THIS FUNCTION IS ABOUT TO MOVE.
	// g_msx2_music_assets is const data, so the linker puts it in the code
	// area -- and the code area runs past 0x8000, which is the NEO window the
	// recording is mapped through.  Reading asset->loop as an argument to
	// LVGM_Play() therefore read a byte of the music stream instead of the
	// flag: every one-shot cue (Victory, Fail) started with LVGM_STATE_LOOP
	// set and played for ever.  Every field this function needs is taken here,
	// while the window still holds the code segment.
	loop = asset->loop;
	g_music_first_segment = asset->first_segment;
	g_music_segment = asset->first_segment;
	g_music_segment_count = asset->segment_count;
	g_music_loop_segment = asset->first_segment;
	// Filled in from LVGM_Play() below, which is the only thing that knows how
	// long this stream's header is: a PSG recording has a common-value byte
	// and no device list, an FM one has a device list and no common value.  It
	// is the fallback loop address for a stream with no FE marker; every file
	// gen_msx_audio.py produces does have one.
	g_music_loop_offset = 0;
	g_audio_error = 0;
#ifdef MSX2_DEBUG_REGRESSION
	g_music_frames = 0;
	g_music_loops = 0;
#endif

	back = Msx2_Bank2Enter(g_music_segment);
	if(LVGM_Play((const void*)0x8000, loop))
	{
		g_music_active = requested;
		g_music_loop_offset = (u16)(g_LVGM_Pointer - (const u8*)0x8000);
	}
	else
	{
		g_audio_error = 1;
		g_music_active = MSX2_MUSIC_NONE;
		g_music_requested = MSX2_MUSIC_NONE;
	}
	Msx2_Bank2Leave(back);
}

void Msx2_AudioInit(void)
{
	g_music_requested = MSX2_MUSIC_NONE;
	g_music_active = MSX2_MUSIC_NONE;
	g_music_first_segment = 0;
	g_music_segment = 0;
	g_music_segment_count = 0;
	g_music_loop_segment = 0;
	g_music_loop_offset = 0;
	g_audio_error = 0;
	// Nothing is playing yet, so LVGM_Pause() must not go poking at FM chips
	// this machine may not have.
	g_LVGM_Devices = 0;
	// The probe and the tables are a boot bank, not resident code:
	// msx2_audio_probe.c says why, and Msx2_AudioSetup() is the trampoline.
	g_audio_chip = Msx2_AudioSetup(g_music_table, g_sfx_steps, g_sfx_first);
	g_sfx_pending = MSX2_SFX_COUNT;
	g_sfx_idx = SFX_IDLE;
	g_sfx_restore = 0;
	// Silence channel C and leave its generators switched off, so the first
	// effect hands back something the tune can live with even if it never
	// touched the channel itself.
	g_PSG_Regs.Volume[2] = 0;
	g_PSG_Regs.Mixer |= SFX_MIX_C;
	LVGM_SetNotifyCallback(Msx2_LvgmNotify);
}

// The main loop only changes this byte.  Starting/stopping the player is
// deferred to the ISR so LVGM's PSG writes and the bank window share one
// serialized context.
void Msx2_MusicPlay(u8 track)
{
	g_music_requested = (track < MSX2_MUSIC_COUNT) ? track : MSX2_MUSIC_NONE;
}

void Msx2_MusicStop(void)
{
	g_music_requested = MSX2_MUSIC_NONE;
}

// Like Msx2_MusicPlay, this only leaves a byte for the V-blank: the PSG buffer
// belongs to the ISR, and a cue asked for from the main loop would otherwise
// race the tick that is applying it.  A second request in the same frame wins;
// that is deliberate, since the newer cue is the one the player just caused.
void Msx2_SfxPlay(u8 sfx)
{
	if(sfx < MSX2_SFX_COUNT)
		g_sfx_pending = sfx;
}

void Msx2_AudioTick(void)
{
	u16 back;

	if((g_music_requested != MSX2_MUSIC_NONE) ||
	   (g_music_active != MSX2_MUSIC_NONE))
	{
		Msx2_AudioStartRequested();
		if(g_music_active != MSX2_MUSIC_NONE && LVGM_IsPlaying())
		{
			back = Msx2_Bank2Enter(g_music_segment);
			LVGM_Decode();
			Msx2_Bank2Leave(back);
#ifdef MSX2_DEBUG_REGRESSION
			if(g_music_frames != 0xFFFF)
				++g_music_frames;
#endif
		}
		else if(g_music_active != MSX2_MUSIC_NONE)
		{
			// A non-looping Victory/Fail recording reached FF.  Its public
			// state is now idle, so a later request for the same result can
			// start it again.
			g_music_active = MSX2_MUSIC_NONE;
			g_music_requested = MSX2_MUSIC_NONE;
		}
	}

	// AFTER the window has gone back to the code segment, and after the music
	// has had its say, so an effect wins the channel for the frame it is on --
	// and only ever writes the register buffer, never the chip.
	Msx2_SfxTick();
	PSG_Apply();
}

u8 Msx2_MusicCurrent(void)
{
	return g_music_active;
}

u8 Msx2_AudioChip(void)
{
	return g_audio_chip;
}

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_AudioRegressionStamp(void)
{
	g_msx2_regression_diag.music_track = g_music_active;
	g_msx2_regression_diag.music_segment = g_music_segment;
	if(g_music_active != MSX2_MUSIC_NONE && g_LVGM_Pointer != 0)
		g_msx2_regression_diag.music_pointer =
			(u16)(g_LVGM_Pointer - (const u8*)0x8000);
	else
		g_msx2_regression_diag.music_pointer = 0;
	g_msx2_regression_diag.music_frames = g_music_frames;
	g_msx2_regression_diag.music_loops = g_music_loops;
	g_msx2_regression_diag.music_errors = g_audio_error;
}
#endif
