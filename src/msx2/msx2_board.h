// ─────────────────────────────────────────────────────────────────────────────
//  msx2_board.h — the duel screen
//
//  The duel is played on the game's own 3D arena (MSX2_PORT_PLAN.md §0.3.1).
//  The picture is captured out of `waifu_fm_headless` at an authored MSX2 pose
//  and streamed from the cartridge; the ten field cards are drawn into their
//  TRUE projected quads by the §8 span rasterizer, so they lie on a board that
//  recedes away from the player rather than standing in a grid of upright
//  rectangles.  Nothing about the arena is re-imagined here: the floor, the
//  slab sides, the perspective and the slot layout are whatever the other five
//  targets render.
//
//  The hand is the one part that is not board geometry.  It is a flat HUD strip
//  on every target, so it stays a row of five axis-aligned card blits -- which
//  also keeps the cards a player is choosing between at a readable size.
//
//  A duel opens on §4.6's baked camera move: eight whole pictures of the board
//  band, streamed one after the other, the last of which IS the resting view.
//  There is no codec and no decoder anywhere in the port.
//
//  Both pages are tracked separately.  A partial repaint reaches only the page
//  it was drawn on, so this file remembers what each page is showing and paints
//  the difference -- that, rather than a dirty-frame countdown, is what keeps a
//  double-buffered board honest when a repaint spans several frames.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// What Msx2_BoardStep() reports back to the scene loop.
#define MSX2_BOARD_BUSY   0xFF
#define MSX2_BOARD_WIN    0
#define MSX2_BOARD_LOSE   1

// Which arena to light the duel with; the four stages match the story's four
// places (STORY_SCENE_DESERT/_TEMPLE/_VOLCANO/_VOID in src/main.c).  The board
// itself is one captured board -- the stage changes the painting behind it and
// its grade, not its geometry.
u8 Msx2_BoardStageForStory(u8 story_duel_index);

// Compose the whole screen on the hidden page and show it, after playing the
// opening camera move.  This costs the best part of three seconds and is an
// entry cost, never a per-frame one.
void Msx2_BoardEnter(u8 stage);

// One frame: input, at most one rules step, and a bounded repaint.
u8 Msx2_BoardStep(void);
