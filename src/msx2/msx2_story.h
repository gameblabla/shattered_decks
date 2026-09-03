// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story.h — story mode: the opening, the sanctum map, dialogue, the end
//
//  Everything this scene shows comes out of the cartridge.  Dialogue streams a
//  shipped GRAPHIC 7 backdrop, then blits both portrait busts at runtime so a
//  speaker change only replaces the two portrait regions.  The prose is a
//  fixed-stride record table in the TEXT segment, parsed out of `src/main.c` by
//  tools/msx2/gen_msx_scenes.py so the writing cannot fork from the other
//  targets'.
//
//  The scene owns the run between duels: name entry, frontier map, deck editor,
//  rewards and continue codes.  It hands the frame loop a request to deal one
//  and is handed the result back.  The board remains ignorant of the story,
//  which is the same seam every other target uses.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// What Msx2_StoryStep() reports back to the scene loop.
#define MSX2_STORY_BUSY    0xFF
#define MSX2_STORY_FIGHT   0    // deal the duel against Msx2_StoryDuelIndex()
#define MSX2_STORY_QUIT    1    // the player asked for the title screen

// Start a new run: the opening narration, then the map.
void Msx2_StoryBegin(void);

/* Test-only entry point used by the story soak build.  It bypasses the
   interactive name screen but still exercises the map, dialogue, rewards and
   ending transitions around the real duel loop. */
void Msx2_StoryBeginAutoplay(void);

// Start the continue-code editor used by TITLE -> LOAD STORY.
void Msx2_StoryBeginLoad(void);

// Called immediately before a story duel is dealt so the current edited deck
// is used by the rules model.
void Msx2_StoryPrepareDuelDeck(void);

// One frame of whichever story screen is up.
u8 Msx2_StoryStep(void);

// Hand control back after a duel the scene asked for.  Beating the frontier
// advances progress; the run ends when the last opponent falls.
void Msx2_StoryDuelDone(bool won);

// The opponent the next (or last) duel is against.
u8 Msx2_StoryDuelIndex(void);

// The implementations, in the page-0 bank this screen is compiled into.  The
// names above are the resident trampolines in msx2_bank.c and are what callers
// use; these exist only while that bank is mapped.  See msx2_bank.h.
void Msx2_StoryBegin_In(void);
void Msx2_StoryBeginAutoplay_In(void);
void Msx2_StoryBeginLoad_In(void);
void Msx2_StoryPrepareDuelDeck_In(void);
u8 Msx2_StoryStep_In(void);
void Msx2_StoryDuelDone_In(bool won);
u8 Msx2_StoryDuelIndex_In(void);
