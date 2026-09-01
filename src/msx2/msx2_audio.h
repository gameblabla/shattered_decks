// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.h — audio stubs, with the RAM the real driver will need
//
//  Music and SFX are not implemented yet.  What is implemented is the *cost*:
//  the port plan budgets ~600 bytes for the Arkos AKG replayer state and ~100
//  for ayFX, and that memory is reserved here from day one so no later
//  milestone discovers it has already spent the RAM the sound driver needs.
//  Msx2_AudioTick() is the ISR-side entry point the replayer will occupy.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Reservation sizes come from the RAM budget table in MSX2_PORT_PLAN.md §3.3.
#define MSX2_AUDIO_MUSIC_STATE_BYTES  600   // Arkos AKG replayer working set
#define MSX2_AUDIO_SFX_STATE_BYTES    100   // ayFX channel-steal bookkeeping

// Track ids map 1:1 onto WaifuFmMusicTrack on the framebuffer targets.
enum Msx2MusicTrack
{
	MSX2_MUSIC_NONE = 0,
	MSX2_MUSIC_TITLE,
	MSX2_MUSIC_OPENING,
	MSX2_MUSIC_DECK_EDITOR,
	MSX2_MUSIC_BATTLE,
	MSX2_MUSIC_BOSS,
	MSX2_MUSIC_FINAL_BOSS,
	MSX2_MUSIC_RESULT,
	MSX2_MUSIC_LOST,
	MSX2_MUSIC_COUNT,
};

enum Msx2Sfx
{
	MSX2_SFX_SELECT = 0,
	MSX2_SFX_CONFIRM,
	MSX2_SFX_CARD_PLACED,
	MSX2_SFX_CARD_DESTROYED,
	MSX2_SFX_CARD_DRAWN,
	MSX2_SFX_TURN_PASSED,
	MSX2_SFX_LASER_SHOOT,
	MSX2_SFX_DIRECT_HIT,
	MSX2_SFX_YOU_LOST,
	MSX2_SFX_COUNT,
};

void Msx2_AudioInit(void);
void Msx2_MusicPlay(u8 track);
void Msx2_MusicStop(void);
void Msx2_SfxPlay(u8 sfx);
void Msx2_AudioTick(void);   // called once per frame; will move into the ISR
u8   Msx2_MusicCurrent(void);
