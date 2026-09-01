// ─────────────────────────────────────────────────────────────────────────────
//  msx2_title.h — the title screen and its menu
//
//  The backdrop is the shipping 256x212 GRB332 painting (plan §14.1): the same
//  picture the other targets show, dithered offline by
//  tools/msx2/gen_msx_scenes.py and streamed out of the cartridge.  The logo,
//  the attract prompt and the menu are drawn over it, which is why they are all
//  outlined or shadowed -- a colour that reads on the bright sky does not read
//  on the dark rock, and vice versa.
//
//  Layout is deliberately identical to the framebuffer targets' title screen in
//  src/main.c (same 256x212 canvas, same panel and row geometry), so the two
//  read as the same game rather than as a coincidence.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// What Msx2_TitleStep() reports back to the scene loop.
#define MSX2_TITLE_BUSY     0xFF
#define MSX2_TITLE_STORY    0
#define MSX2_TITLE_BATTLE   1
#define MSX2_TITLE_LOAD     2
#define MSX2_TITLE_ROWS     3

// Composes the whole screen on the hidden page and flips to it.  Costs several
// hundred VDP commands, so it is an entry cost, never a per-frame one.
void Msx2_TitleEnter(void);

// One frame of the title: cursor movement, the blinking prompt, selection.
// Returns MSX2_TITLE_BUSY until the player commits to a row.
u8 Msx2_TitleStep(void);

// Which row the cursor is on -- stamped into the probe so a headless run can
// see the menu being driven.
u8 Msx2_TitleCursor(void);
