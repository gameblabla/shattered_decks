// ─────────────────────────────────────────────────────────────────────────────
//  msx2_bank.h — the switchable page-0 code window
//
//  The port has two fixed code areas and had run out of both: _CODE spans
//  0x4000-0xBFFF (cartridge segments 0 and 1, 32 KB) and cartridge segment 2
//  sits at 0x0000-0x3FFF (16 KB, the duel and story screens).  Neither can
//  grow, and the duel screen still has whole features to gain.
//
//  So page 0 became a WINDOW.  Segment 2 holds the duel screen, segment 4 the
//  story screens -- the two biggest things in the port, which never run at the
//  same time -- and segment 3 the modal screens that either of them can step
//  out into: the card check, the fusion cut-in, the disk save.  Two things make
//  this legal, and both are why the ISR moved to RAM page 3
//  (project_config.js):
//
//   * the interrupt handler is no longer at 0x0038 in segment 2, so swapping
//     the window no longer takes the handler with it;
//   * every trampoline below is compiled into _CODE, which the window is not.
//     A caller in segment 2 calls a trampoline in _CODE, the trampoline maps
//     segment 3 and calls into it, and the return path unwinds through code
//     that was mapped the whole time.  Segment 2's caller only resumes after
//     its own bank is back.
//
//  THE ONE RULE: code in a page-0 bank may call _CODE and its own bank, and no
//  other bank -- those do not exist while it runs.  The trampolines are what
//  enforce it: they are the only way in, and each one leaves the window as it
//  found it.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_BANK0_DUEL   2   // the duel screen      (waifu_msx2_s2_b0.c)
#define MSX2_BANK0_MODAL  3   // modal screens        (waifu_msx2_s3_b0.c)
#define MSX2_BANK0_STORY  4   // the story screens    (waifu_msx2_s4_b0.c)

// A bank in the OTHER window.  Segment 5 is linked at 0x8000 and holds code
// that runs once at boot (waifu_msx2_s5_b2.c); it is mapped through the same
// Msx2_Bank2Enter/Leave pair the streamer and the music use, so nothing in it
// may be reachable from the ISR.
#define MSX2_BANK2_BOOT   5

void Msx2_VideoBakeFont_In(void);

// Put a turbo R on its R800, from _CODE, with the BIOS briefly in page 0 --
// and put this port's own interrupt vector back afterwards, which is the part
// that is not optional (msx2_bank.c).  A no-op on every other machine.
void Msx2_CpuFast(void);

// Map `segment` at 0x0000 and return whatever was there, for Msx2_Bank0Leave.
u16  Msx2_Bank0Enter(u16 segment);
void Msx2_Bank0Leave(u16 segment);
u16  Msx2_Bank0Current(void);

// The 0x8000 window is shared by the streamer and the resident audio ISR.
// Both users save and restore this shadow rather than assuming that segment 1
// is still mapped after another banked operation has returned.
u16  Msx2_Bank2Enter(u16 segment);
void Msx2_Bank2Leave(u16 segment);

// Probe the machine's sound chips and resolve its music and sound-effect
// tables.  Returns a <Msx2AudioChip>; see msx2_audio_probe.c for why none of
// the three is in _CODE.
#include "msx2_audio.h"
struct Msx2MusicAsset;
u8   Msx2_AudioSetup(struct Msx2MusicAsset* table, Msx2SfxStep* steps,
                     u8* first);

#ifdef MSX2_DEBUG_REGRESSION
void Msx2_BoardRegressionFixture(u8 fixture);
void Msx2_BoardRegressionStamp(void);
void Msx2_StoryRegressionFixture(u8 fixture);
void Msx2_StoryRegressionStamp(void);
#endif
