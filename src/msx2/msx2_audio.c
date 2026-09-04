// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.c — resident PSG lVGM player
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

static void Msx2_AudioStartRequested(void)
{
	u8 requested = g_music_requested;
	const Msx2MusicAsset* asset;
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
	asset = &g_msx2_music_assets[requested];
	if(asset->segment_count == 0)
		return;

	g_music_first_segment = asset->first_segment;
	g_music_segment = asset->first_segment;
	g_music_segment_count = asset->segment_count;
	g_music_loop_segment = asset->first_segment;
	// If a file has no FE marker, this is the first command after the lVGM
	// header.  Files produced by gen_msx_audio.py do contain a marker, but the
	// fallback makes the resident player safe for a hand-authored stream too.
	g_music_loop_offset = 6;
	g_audio_error = 0;
#ifdef MSX2_DEBUG_REGRESSION
	g_music_frames = 0;
	g_music_loops = 0;
#endif

	back = Msx2_Bank2Enter(g_music_segment);
	if(LVGM_Play((const void*)0x8000, asset->loop))
		g_music_active = requested;
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
	g_music_loop_offset = 6;
	g_audio_error = 0;
	g_sfx_pending = MSX2_SFX_COUNT;
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

void Msx2_SfxPlay(u8 sfx)
{
	g_sfx_pending = sfx;
}

void Msx2_AudioTick(void)
{
	u16 back;

	if(g_music_requested == MSX2_MUSIC_NONE &&
	   g_music_active == MSX2_MUSIC_NONE)
	{
		g_sfx_pending = MSX2_SFX_COUNT;
		return;
	}
	Msx2_AudioStartRequested();
	if(g_music_active != MSX2_MUSIC_NONE && LVGM_IsPlaying())
	{
		back = Msx2_Bank2Enter(g_music_segment);
		LVGM_Decode();
		PSG_Apply();
		Msx2_Bank2Leave(back);
#ifdef MSX2_DEBUG_REGRESSION
		if(g_music_frames != 0xFFFF)
			++g_music_frames;
#endif
	}
	else if(g_music_active != MSX2_MUSIC_NONE)
	{
		// A non-looping Victory/Fail recording reached FF.  Its public state is
		// now idle, so a later request for the same result can start it again.
		g_music_active = MSX2_MUSIC_NONE;
		g_music_requested = MSX2_MUSIC_NONE;
	}
	g_sfx_pending = MSX2_SFX_COUNT;
}

u8 Msx2_MusicCurrent(void)
{
	return g_music_active;
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
