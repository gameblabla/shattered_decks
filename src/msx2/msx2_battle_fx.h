#pragma once

#include "msxgl.h"

#define MSX2_BATTLE_BURN_STEPS 6

// How many poses the blade sweep grows over, and how many the damage readout
// counts up over (the sweep plus the sprite burst that follows it).
#define MSX2_BATTLE_SLASH_STEPS 8
#define MSX2_BATTLE_COUNT_STEPS 16

// ── The cut-in lives in the MODAL bank, behind one trampoline ────────────────
//
// It used to sit in segment 2 beside the duel screen that calls it, and that
// bank ran out: the shipping cartridge had ONE byte of it left, and the MSX2+
// build -- which adds two mode calls to the duel -- could not be linked at all.
// The effects touch nothing of the duel bank (sprites, the video layer and
// g_duel, all of them _CODE), so they are modal-bank code like the fusion
// cut-in beside them, reached through msx2_bank.c.
//
// ONE trampoline, not five.  Every trampoline is _CODE, and _CODE has a couple
// of hundred bytes left: five of them would have cost more up there than the
// five bodies cost down here.  So the entry points below are macros onto a
// single (op, x, y, value) call, and the bank-side dispatcher turns it back
// into the five routines -- which are written exactly as they were.
#define MSX2_FX_SLASH   0
#define MSX2_FX_BURST   1
#define MSX2_FX_DAMAGE  2
#define MSX2_FX_RESULT  3
#define MSX2_FX_BURN    4

void Msx2_BattleFx(u8 op, u8 x, u8 y, i16 value);
void Msx2_BattleFx_In(u8 op, u8 x, u8 y, i16 value);

// One pose of the blade sweep centred on (x, 83).  It is SPRITES: it touches no
// part of the picture under it, so there is nothing to erase and no page to
// level between poses, which is the whole reason an attack is quick.
#define Msx2_BattleFxSlash(x, step) \
	Msx2_BattleFx(MSX2_FX_SLASH, (x), 0, (i16)(step))

// The running damage readout under the struck lane: -0 climbing to -<damage>.
// Its own black plate, so the digits can shrink again without leaving a tail --
// and it is the one thing in the beat that IS bitmap, so the caller must give
// the same figure to two consecutive frames or the two pages disagree and it
// flickers between two numbers.
#define Msx2_BattleFxDamageCount(x, value) \
	Msx2_BattleFx(MSX2_FX_DAMAGE, (x), 0, (value))

// One frame of the sprite explosion at (x, y).  `step` past the last frame
// takes it off the screen.
#define Msx2_BattleFxBurst(x, y, step) \
	Msx2_BattleFx(MSX2_FX_BURST, (x), (y), (i16)(step))
#define Msx2_BattleFxResult(trap) \
	Msx2_BattleFx(MSX2_FX_RESULT, 0, 0, (i16)(trap))
#define Msx2_BattleFxBurnCard(x, h, step) \
	Msx2_BattleFx(MSX2_FX_BURN, (x), (h), (i16)(step))
