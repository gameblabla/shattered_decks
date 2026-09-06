// ─────────────────────────────────────────────────────────────────────────────
//  waifu_msx2_s5_b2.c — the boot-time code bank
//
//  Cartridge segment 5, linked at 0x8000 and mapped through the SAME window the
//  streamer and the music player use (msx2_bank.h).  MSXgl's contract is the
//  filename: _s5 is the segment, _b2 is the 0x8000 bank.
//
//  What belongs here: code that runs once, at boot, from the main loop, and is
//  otherwise pure weight in a 32 KB _CODE that has a few hundred bytes left.
//  What does not: anything reachable from the V-blank ISR, and anything the
//  streamer calls -- while this segment is in the window, the resident code
//  above 0x8000 does not exist, which is exactly the property that makes the
//  window usable and exactly the trap it sets.  pack_msx_rom.py enforces the
//  resident half of that rule.
//
//  Callers come in through the trampoline in msx2_bank.c, never directly.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef MSX2_ASCII16X

#include "msx2_audio_probe.c"

#endif
