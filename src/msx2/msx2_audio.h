// ─────────────────────────────────────────────────────────────────────────────
//  msx2_audio.h — lVGM playback on whichever sound chip the machine has,
//  with the mapper-safe ISR seam
//
//  The recordings are MSXgl lVGM streams and the cartridge carries four sets
//  of them: PSG (AY-3-8910), MSX-MUSIC (YM2413/OPLL), MSX-AUDIO (Y8950) and
//  MoonSound (YMF278B/OPL4).  Msx2_AudioInit() probes for the three expansion
//  chips once at boot and every track afterwards is looked up in the winning
//  set, falling back to the PSG recording for any track that set does not
//  have.
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
	MSX2_CHIP_MOONSOUND,      // YMF278B, a MoonSound or an OPL4 clone
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

// ─────────────────────────────────────────────────────────────────────────────
//  The sound-effect cue table, which lives in the boot bank and in RAM
//
//  A cue is a run of STEPS.  A step holds the PSG's channel C for `len`
//  V-blanks with a tone period that walks by `slide` every tick and a volume
//  that falls by `decay`, plus an optional noise period -- which covers every
//  cue this game has: a blip is one step, a two-note confirm is two, an
//  explosion is three widening bands of noise.  Nothing here is a general sound
//  driver; it is the smallest thing that covers the list.
//
//  WHY IT IS DECLARED HERE AND FILLED FROM SEGMENT 5.  The steps are 146 bytes
//  of constant data, and constant data goes into _CODE -- the one 32 KB area
//  this port is genuinely out of, to the point where the MSX2+ cartridge had
//  eighteen bytes left in it.  A boot bank has fifteen KILOBYTES left.  So the
//  table is a const in msx2_audio_probe.c, copied byte for byte into RAM at
//  boot beside the music table that is already there for the same reason, and
//  the resident player indexes the RAM copy with exactly the code it used to
//  index the ROM one.  RAM is mapped whatever the 0x8000 window holds, which
//  makes the copy strictly safer as well as cheaper.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct
{
	u16 period;   // tone period, or 0 for a step with no tone at all
	i16 slide;    // added to the period every tick: a rise, a fall, a zap
	u8  noise;    // noise period 1..31, or 0 for a step with no noise
	u8  vol;      // 0..15 at the start of the step
	u8  decay;    // sixteenths of a volume step lost per tick
	u8  len;      // ticks
} Msx2SfxStep;

#define MSX2_SFX_STEP_COUNT 17

// The cues, one run of steps each, in the order of enum Msx2Sfx.  Periods are
// the PSG's own: frequency is 111861 / period, so 190 is a 589 Hz blip and 900
// is the 124 Hz thump under a card landing.  No step may slide its period out
// of the chip's twelve bits over its own length -- the walk does not clamp, and
// none of these comes near it.
#define MSX2_SFX_STEPS \
{ \
	/* SELECT: the cursor moved.  One bright click, gone in four frames. */ \
	{  190,    0,  0, 10, 56,  4 }, \
	/* CONFIRM: C5 then G5, the two-note "yes" of a menu. */ \
	{  213,    0,  0, 12,  0,  3 }, \
	{  142,    0,  0, 12, 24,  8 }, \
	/* CARD_PLACED: a clack -- a short noise transient over a low tone. */ \
	{  900,    0,  6, 14,  0,  2 }, \
	{ 1100,    0, 10, 10, 48,  5 }, \
	/* CARD_DESTROYED: three widening bands of noise, each quieter, which is \
	   a falling explosion without an envelope generator to spend on it. */ \
	{    0,    0,  2, 15,  4,  6 }, \
	{    0,    0,  6, 11,  6,  8 }, \
	{    0,    0, 14,  7,  8, 10 }, \
	/* CARD_DRAWN: paper.  Fine noise, very short. */ \
	{    0,    0,  3,  8, 40,  5 }, \
	/* TURN_PASSED: two notes down, the handover. */ \
	{  250,    0,  0, 11,  0,  5 }, \
	{  375,    0,  0, 11, 22, 10 }, \
	/* LASER_SHOOT: one tone falling fast over a quarter of a second. */ \
	{   90,   18,  0, 13,  8, 16 }, \
	/* DIRECT_HIT: an impact -- noise, then a tone dropping away under it. */ \
	{    0,    0,  3, 15,  0,  3 }, \
	{  700,   90,  8, 14, 12, 12 }, \
	/* YOU_LOST: three notes down, the last one held and fading. */ \
	{  320,    0,  0, 12,  0,  8 }, \
	{  400,    0,  0, 12,  0,  8 }, \
	{  505,    0,  0, 12, 10, 24 } \
}

// Where each cue's steps start.  One entry past the end, so a cue's run is
// [first[id], first[id + 1]).
#define MSX2_SFX_FIRST \
	{ 0, 1, 3, 5, 8, 9, 11, 12, 14, 17 }

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
u8   Msx2_AudioSetup_In(struct Msx2MusicAsset* table, Msx2SfxStep* steps,
                        u8* first);

// The resident lVGM parser calls this directly from the V-blank path.  Keep
// the callback in fixed code; the generic function-pointer dispatch is not
// safe while the music stream is mapped through the 0x8000 window.
bool Msx2_LvgmNotify(u8 id);

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_AudioRegressionStamp(void);
#endif
