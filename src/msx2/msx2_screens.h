// ─────────────────────────────────────────────────────────────────────────────
//  msx2_screens.h — the two full-screen beats the duel steps out of itself for
//
//  Both leave the arena entirely, both compose on the hidden page, and both are
//  put away by the duel screen's own Msx2_BoardRestoreFromCutin().  They live
//  here rather than in msx2_board.c because segment 2 -- the bank the duel and
//  story screens share -- has about seven hundred bytes left in it, and only
//  the streamer itself has to be resident while the cartridge window is
//  switched: a caller may sit anywhere in _CODE.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// ── The card check screen ───────────────────────────────────────────────────
// The full-size 2-D art of one card, its name and its figures, on the same
// black stage the battle cut-in uses.  Composed once; the duel screen holds it
// until a button is pressed.
void Msx2_CardCheckCompose(u8 card, i16 atk, i16 def);

// ── The fusion cut-in ───────────────────────────────────────────────────────
// The materials, the burst that takes them, and the monster they became.
// Msx2_FusionBegin() composes the first beat; Msx2_FusionStep() advances one
// frame and returns FALSE when the beat is over and the board should come back.
#define MSX2_FUSION_MATS  3
void Msx2_FusionBegin(const u8* materials, u8 count, u8 result);
bool Msx2_FusionStep(void);
