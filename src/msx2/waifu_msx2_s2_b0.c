// ─────────────────────────────────────────────────────────────────────────────
//  waifu_msx2_s2_b0.c — the page-0 code bank
//
//  NEO-16 maps three 16 KB banks, and MSXgl's crt0 uses the first of them --
//  cartridge segment 2 at 0x0000-0x3FFF -- for the 39-byte interrupt hook and
//  nothing else.  The filename is MSXgl's contract for "compile this into
//  segment 2, linked at bank 0" (engine/script/js/build.js), and the ~16 KB
//  behind that hook is the port's second code budget: _CODE at 0x4000-0xBFFF
//  is a hard 32 KB, and the duel screen does not fit inside what is left of it.
//
//  Two properties make this the right bank for exactly this code:
//
//   * it is never switched, so a routine here still exists while the 0x8000
//     window holds picture data.  That is a requirement for the streamer, not
//     a nicety -- the ISR and everything else above 0x8000 is gone during a
//     copy -- and putting the streamer here makes residency a property of the
//     file rather than of link order;
//   * it costs nothing at runtime: all three banks are mapped at once, so a
//     call from _CODE into here is an ordinary CALL.
//
//  The duel screen is here because _CODE has no room for it, and it is here
//  ALONE: the story screens used to share this bank and the two together came
//  within a couple of hundred bytes of filling it, which is not a budget you
//  can add a feature to.  They are in segment 4 now (waifu_msx2_s4_b0.c).  The
//  split costs nothing because the two never run at the same time and never
//  call each other -- the scene loop in msx2_main.c is the only thing that
//  knows both -- so each simply gets its own sixteen kilobytes.
//
//  MSXgl compiles exactly one file per (segment, bank), so the modules that
//  live here are #included rather than listed in ProjModules.  They are
//  ordinary .c files and are written as such; this file is the placement.
// ─────────────────────────────────────────────────────────────────────────────

// The battle cut-in's blade, burst, burn and damage figure.  It lives in this
// bank rather than in _CODE for room: msx2_board.c is its only caller, a call
// inside a bank is an ordinary CALL, and _CODE was thirty bytes over the line
// the day the PSG effects went in.  It reaches the sprite layer and the video
// layer, both of which are in _CODE, which is what a bank is allowed to do.
#include "msx2_battle_fx.c"
#include "msx2_board.c"
