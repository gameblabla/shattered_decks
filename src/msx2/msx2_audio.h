// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.h — lVGM playback on whichever sound chip the machine has,
//  with the mapper-safe ISR seam
//
//  The recordings are MSXgl lVGM streams and the cartridge carries three sets
//  of them: PSG (AY-3-8910), MSX-MUSIC (YM2413/OPLL) and MSX-AUDIO (Y8950).
//  Msx2_AudioInit() probes for the two FM chips once at boot and every track
//  afterwards is looked up in the winning set, falling back to the PSG
//  recording for any track that set does not have.
//
//  Their data lives in banked cartridge segments; the resident ISR maps one
//  segment, decodes one frame, applies the indirect PSG buffer, and restores
//  the code segment before it returns.  Msx2_MusicPlay() only queues a track
//  transition for that ISR.
//
//  Sound effects stay on the PSG whatever the music is playing on.  With an FM
//  chip driving the tune the PSG is idle, so a cue no longer has to borrow a
//  voice from the music -- it simply costs nothing.
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

// Which chip the music is coming out of.  Fixed at boot by Msx2_AudioInit().
enum Msx2AudioChip
{
	MSX2_CHIP_PSG = 0,        // AY-3-8910 / YM2149, always present
	MSX2_CHIP_MSXMUSIC,       // YM2413, internal or an FM-PAC
	MSX2_CHIP_MSXAUDIO,       // Y8950
	MSX2_CHIP_COUNT,
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
u8   Msx2_AudioChip(void);   // see <Msx2AudioChip>

// Defined in the boot bank (msx2_audio_probe.c): probe the chips, fill `table`
// with MSX2_MUSIC_ASSET_COUNT resolved records, and return the winning chip.
// Call it through Msx2_AudioSetup() in msx2_bank.c, never directly -- it only
// exists while segment 5 is mapped at 0x8000.
struct Msx2MusicAsset;
u8   Msx2_AudioSetup_In(struct Msx2MusicAsset* table);

// The resident lVGM parser calls this directly from the V-blank path.  Keep
// the callback in fixed code; the generic function-pointer dispatch is not
// safe while the music stream is mapped through the 0x8000 window.
bool Msx2_LvgmNotify(u8 id);

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_AudioRegressionStamp(void);
#endif
