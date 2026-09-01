// ─────────────────────────────────────────────────────────────────────────────
//  msx2_stream.h — cartridge segment → VRAM / RAM streaming
//
//  A full-screen GRAPHIC 7 picture is 54,272 bytes and the card set is 158 KB.
//  Nothing that size can be linked into the address space the Z80 sees, so
//  every asset lives in whole 16 KB NEO segments (tools/msx2/gen_msx_scenes.py
//  places them, tools/msx2/pack_msx_rom.py writes them into the ROM) and is
//  pushed at the VDP through the 0x8000 window.
//
//  TWO RULES, both of them load-bearing:
//
//  1. While the window holds asset data, none of the code that normally lives
//     at 0x8000-0xBFFF exists.  Every copy here therefore runs with interrupts
//     off and touches nothing but ports -- the ISR itself is in that window.
//     This whole module is compiled into the page-0 code bank
//     (waifu_msx2_s2_b0.c), which the mapper never switches, so residency is a
//     property of where the file is compiled rather than of link order luck;
//     pack_msx_rom.py still checks it on every build.
//  2. A full-screen stream blanks the display.  In GRAPHIC 7 with the screen
//     on, the VDP needs ~29 T-states between VRAM accesses and `OTIR` gives it
//     21, so bytes would be dropped.  Msx2_StreamRect, which has to run with
//     the picture up, pays for that instead: its inner loop is padded to 32
//     T-states per byte, which is about 1.8 KB per frame -- one card.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Stream one full-screen scene into a GRAPHIC 7 page.  Blanks the display for
// the duration and restores it; the caller decides which page to show after.
void Msx2_StreamScene(u16 segment, u8 page);

// Copy a w x h rectangle out of the cartridge into the current draw page, with
// the display running.  `segment`/`offset` address the first byte of the
// rectangle; rows are contiguous and w bytes apart, which is how every baked
// sprite in this port is stored.  Interrupts come back between rows.
void Msx2_StreamRect(u16 segment, u16 offset, u8 x, u8 y, u8 w, u8 h);

// Copy bytes out of a cartridge segment into RAM -- the string table, which is
// far too big to link.  `len` is a byte count, and the copy must not run off
// the end of the segment.
void Msx2_RomRead(u16 segment, u16 offset, u8* dst, u8 len);
