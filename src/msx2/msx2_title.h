// ─────────────────────────────────────────────────────────────────────────────
//  msx2_title.h — the title screen and its menu
//
//  The shipping title is a streamed 256x212 GRB332 painting (plan §14.1); the
//  asset pipeline that bakes it is M3 and does not exist yet.  Until it does,
//  the backdrop is *drawn* out of VDP fills -- sky gradient, sun, pyramids,
//  sand -- which costs no ROM and, more usefully, gives the scene, the menu,
//  the input path and the page flip something real to be verified against now.
//  When the streamer lands, only Msx2_TitleBackdrop() changes.
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
