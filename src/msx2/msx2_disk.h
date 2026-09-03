// ─────────────────────────────────────────────────────────────────────────────
//  msx2_disk.h — saving to a floppy, when there is one
//
//  The cartridge has no battery, so the game's save is a continue code the
//  player writes down.  A machine with a disk drive can write it down for them.
//
//  This is deliberately the SMALLEST thing that works: the sixteen characters
//  of the continue code, in one sector, through the disk ROM's own DSKIO entry
//  reached by an inter-slot call.  No filesystem, no BDOS -- MSX-DOS lives in
//  page 1, which is where this cartridge is, and Disk BASIC's BDOS vector is a
//  jump into a ROM this game has switched out.  DSKIO through CALSLT is the
//  route a cartridge is allowed to take, and the disk ROM does the slot
//  switching itself.
//
//  The sector written is the LAST one on the disk, found from the boot record
//  rather than guessed.  It is inside the data area, so a disk with files on it
//  keeps its directory and its FAT -- but a file that happens to reach the end
//  of the disk would lose its last sector.  The screen says to use a blank one.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Is there a disk system, and did a drive answer?  Cached after the first call.
// Everything below returns FALSE when this does.
bool Msx2_DiskPresent(void);

// The continue code, to and from the disk.  `code` is the 16 characters plus
// its terminator; Msx2_DiskLoad fills it only on success.
bool Msx2_DiskSave(const c8* code);
bool Msx2_DiskLoad(c8* code);
