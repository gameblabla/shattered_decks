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
//  own bank has to stay mapped -- and nothing that needs the BIOS, which lives
//  at 0x0000 in another slot and so is exactly what this bank displaces.  That
//  is why the disk layer is in _CODE (msx2_disk.c) and not here.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_screens.c"
#include "msx2_probe_bank.c"
#include "msx2_entropy_bank.c"
