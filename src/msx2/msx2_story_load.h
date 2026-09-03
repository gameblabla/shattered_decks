// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story_load.h — LOAD STORY: where is the save?
//
//  The cartridge has no battery, so a run is carried between sessions either on
//  paper -- the sixteen-character continue code -- or, on a machine with a
//  drive, in one sector of a floppy.  Both are the same sixteen characters;
//  only where they come from differs, and that is a question worth asking out
//  loud rather than a keystroke buried on a screen the player has already had
//  to reach.
//
//  It is its own module, and in _CODE rather than in the story bank, because
//  cartridge segment 2 -- msx2_story.c and msx2_board.c -- is full to within a
//  few hundred bytes and a whole screen does not fit in what is left.  The
//  surface is deliberately four calls wide: this file knows nothing about the
//  story's phases and the story knows nothing about this screen's cursor.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

#define MSX2_LOADPICK_BUSY      0
#define MSX2_LOADPICK_QUIT      1   // back to the title
#define MSX2_LOADPICK_PASSWORD  2   // the player will type it
#define MSX2_LOADPICK_LOADED    3   // `code` now holds what the disk had

// Put the screen up.  `code` is the caller's continue-code buffer, which a
// successful disk read fills in place; it must outlive the screen.
void Msx2_StoryLoadPickEnter(c8* code);

// One frame.  Returns one of the four answers above.
u8   Msx2_StoryLoadPickStep(void);

// The caller parsed what the disk gave back and it was not a continue code.
// Says so on the screen and stays there.
void Msx2_StoryLoadPickRefused(void);
