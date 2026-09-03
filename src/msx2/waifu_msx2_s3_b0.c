// ─────────────────────────────────────────────────────────────────────────────
//  waifu_msx2_s3_b0.c — the modal-screen code bank
//
//  Cartridge segment 3, mapped at 0x0000 only while one of the screens below is
//  actually on the display.  msx2_bank.h explains why that is safe and what the
//  one rule is; MSXgl's contract is the filename, and it compiles exactly one
//  file per (segment, bank), so the modules that live here are #included.
//
//  What belongs here: a screen that takes the whole display, runs, and gives it
//  back.  What does not: anything the duel or story screens call while their
//  own bank has to stay mapped.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_screens.c"
#include "msx2_disk.c"
