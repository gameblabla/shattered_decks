// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.h — PSG lVGM playback, with the mapper-safe ISR seam
//
//  The recordings are MSXgl lVGM PSG streams.  Their data lives in banked
//  cartridge segments; the resident ISR maps one segment, decodes one frame,
//  applies the indirect PSG buffer, and restores the code segment before it
//  returns.  Msx2_MusicPlay() only queues a track transition for that ISR.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Track ids map 1:1 onto WaifuFmMusicTrack on the framebuffer targets.
enum Msx2MusicTrack
{
	MSX2_MUSIC_NONE = 0,
	MSX2_MUSIC_TITLE,
	MSX2_MUSIC_OPENING,
	MSX2_MUSIC_OVERWORLD,
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
void Msx2_AudioTick(void);   // called once per V-blank from the resident ISR
u8   Msx2_MusicCurrent(void);

// The resident lVGM parser calls this directly from the V-blank path.  Keep
// the callback in fixed code; the generic function-pointer dispatch is not
// safe while the music stream is mapped through the 0x8000 window.
bool Msx2_LvgmNotify(u8 id);

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_AudioRegressionStamp(void);
#endif
