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

// Map `segment` at 0x0000 and return whatever was there, for Msx2_Bank0Leave.
u16  Msx2_Bank0Enter(u16 segment);
void Msx2_Bank0Leave(u16 segment);
u16  Msx2_Bank0Current(void);
