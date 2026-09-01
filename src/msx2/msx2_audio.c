// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.c — silent stubs that hold the sound driver's RAM open.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_audio.h"

// Reserved now, used when the Arkos AKG / ayFX replayers land.  Declared here
// rather than inside the future driver so the RAM budget is spent -- and
// visible in the link map -- from the first milestone onwards.
u8 g_msx2_music_state[MSX2_AUDIO_MUSIC_STATE_BYTES];
u8 g_msx2_sfx_state[MSX2_AUDIO_SFX_STATE_BYTES];

static u8 g_music_track;
static u8 g_sfx_pending;

void Msx2_AudioInit(void)
{
	g_music_track = MSX2_MUSIC_NONE;
	g_sfx_pending = MSX2_SFX_COUNT;
}

// The track id is recorded even while playback is a stub, so the scenes can be
// written against the real interface and the driver drops in underneath.
void Msx2_MusicPlay(u8 track)
{
	g_music_track = track;
}

void Msx2_MusicStop(void)
{
	g_music_track = MSX2_MUSIC_NONE;
}

void Msx2_SfxPlay(u8 sfx)
{
	g_sfx_pending = sfx;
}

void Msx2_AudioTick(void)
{
	// The replayer will run here, wrapped in the bank save/restore the port
	// plan requires (§5.5): the ISR must never leave the 0x8000 window pointing
	// at a music segment when it returns to the streamer.
	g_sfx_pending = MSX2_SFX_COUNT;
}

u8 Msx2_MusicCurrent(void)
{
	return g_music_track;
}
