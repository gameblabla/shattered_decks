// ─────────────────────────────────────────────────────────────────────────────
//  msx2_story.h — story mode: the opening, the sanctum map, dialogue, the end
//
//  Everything this scene shows comes out of the cartridge.  The pictures are
//  whole baked composites (backdrop + character + an empty text box flattened
//  into one 54,272-byte GRAPHIC 7 screen, see MSX2_PORT_PLAN.md §14.2), so a
//  dialogue beat that changes speaker is one stream and nothing else; the prose
//  is a fixed-stride record table in the TEXT segment, parsed out of
//  `src/main.c` by tools/msx2/gen_msx_scenes.py so the writing cannot fork from
//  the other targets'.
//
//  The scene owns the duel *between* duels only: it hands the frame loop a
//  request to deal one and is handed the result back.  That keeps the board
//  ignorant of the story, which is the same seam every other target uses.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// What Msx2_StoryStep() reports back to the scene loop.
#define MSX2_STORY_BUSY    0xFF
#define MSX2_STORY_FIGHT   0    // deal the duel against Msx2_StoryDuelIndex()
#define MSX2_STORY_QUIT    1    // the player asked for the title screen

// Start a new run: the opening narration, then the map.
void Msx2_StoryBegin(void);

// One frame of whichever story screen is up.
u8 Msx2_StoryStep(void);

// Hand control back after a duel the scene asked for.  Beating the frontier
// advances progress; the run ends when the last opponent falls.
void Msx2_StoryDuelDone(bool won);

// The opponent the next (or last) duel is against.
u8 Msx2_StoryDuelIndex(void);
