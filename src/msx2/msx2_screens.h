// ─────────────────────────────────────────────────────────────────────────────
//  msx2_screens.h — the two full-screen beats the duel steps out of itself for
//
//  Both leave the arena entirely, both compose on the hidden page, and both are
//  put away by the duel screen's own Msx2_BoardRestoreFromCutin().
//
//  Both are compiled into the modal-screen code bank (waifu_msx2_s3_b0.c), so
//  each has two names: the plain one is the resident trampoline in msx2_bank.c
//  and is what callers use, and the _In one is the implementation, which only
//  exists while segment 3 is mapped.  See msx2_bank.h for the rule that governs
//  what such an implementation is allowed to call.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// ── The card check screen ───────────────────────────────────────────────────
// The full-size 2-D art of one card, its name and its figures, on the same
// black stage the battle cut-in uses.  Composed once; the duel screen holds it
// until a button is pressed.
void Msx2_CardCheckCompose(u8 card, i16 atk, i16 def);
void Msx2_CardCheckCompose_In(u8 card, i16 atk, i16 def);

// ── The fusion cut-in ───────────────────────────────────────────────────────
// The materials, the burst that takes them, and the monster they became.
// Msx2_FusionBegin() composes the first beat; Msx2_FusionStep() advances one
// frame and returns FALSE when the beat is over and the board should come back.
#define MSX2_FUSION_MATS  3
void Msx2_FusionBegin(const u8* materials, u8 count, u8 result, u8 fused);
bool Msx2_FusionStep(void);
void Msx2_FusionBegin_In(const u8* materials, u8 count, u8 result, u8 fused);
bool Msx2_FusionStep_In(void);

// ── The effect cut-in ───────────────────────────────────────────────────────
// A support card that resolves on its own: the card at cut-in size with the
// sentence that says what it does.  Begin composes it, Step counts the hold
// down and returns FALSE when the board should come back.  `by_com` only
// changes the line over the card: the opponent's THUNDER is the same card doing
// the same thing, and the player is owed the same look at it.
void Msx2_EffectBegin(u8 card, u8 by_com);
bool Msx2_EffectStep(void);
void Msx2_EffectBegin_In(u8 card, u8 by_com);
bool Msx2_EffectStep_In(void);
