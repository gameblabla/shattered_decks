// ─────────────────────────────────────────────────────────────────────────────
//  msx2_board.h — the duel screen
//
//  The board is fifteen 40x48 card rectangles on a baked arena: five COM
//  monsters, five of the player's, and the player's hand.  Nothing about it
//  moves, which is the whole design (MSX2_PORT_PLAN.md §4.3): a Z80 cannot
//  redraw a 54 KB screen at animation rates, so the presentation is a fixed
//  view and a card that changes is one rectangle that changes.
//
//  Card art comes out of the cartridge a rectangle at a time
//  (Msx2_StreamRect), which costs about one frame per card with the display
//  running.  A slot that empties is put back with the backdrop's own bytes
//  (the SLOTS blob), and the selection cursor lives entirely inside the flat
//  ring baked around every slot, so moving it is four fills and repairs
//  nothing.
//
//  Both pages are tracked separately.  A partial repaint reaches only the page
//  it was drawn on, so this file remembers what each page is showing and
//  paints the difference -- that, rather than a dirty-frame countdown, is what
//  keeps a double-buffered board honest when a repaint spans several frames.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// What Msx2_BoardStep() reports back to the scene loop.
#define MSX2_BOARD_BUSY   0xFF
#define MSX2_BOARD_WIN    0
#define MSX2_BOARD_LOSE   1

// Which arena to bake the duel on; the four stages match the story's four
// places (STORY_SCENE_DESERT/_TEMPLE/_VOLCANO/_VOID in src/main.c).
u8 Msx2_BoardStageForStory(u8 story_duel_index);

// Compose the whole screen on the hidden page and show it.  Streams the
// backdrop and lays every card on it, so this costs the best part of a second
// and is an entry cost, never a per-frame one.
void Msx2_BoardEnter(u8 stage);

// One frame: input, at most one rules step, and a bounded repaint.
u8 Msx2_BoardStep(void);
