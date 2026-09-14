/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_battle.h — the battle and direct-attack presentation (Mode 4).
 *
 *  A battle is a 2D scene: the big gold-rimmed cards of the card check
 *  (snes_cardart.h's sheets), one in each of two VERTICAL LANES, moving on
 *  Mode 4's offset-per-tile.  Mode 4 keeps an 8bpp BG1 for the card art and
 *  gives BG3's tilemap over to per-column scroll offsets; a vertical offset
 *  keeps pixel precision where a horizontal one is quantised to eight, so
 *  the player's card enters DOWN from above the screen and the opponent's
 *  UP from below, whoever is attacking, and both move a pixel a field.
 *
 *  A direct attack is the same lanes with only the attacker present and the
 *  PC/SDL3 build's impact beat over it (src/main.c draw_direct_attack_fx,
 *  src/platform/sdl3/shaders/impact.frag): the blade sweep, the whiteout, the
 *  hot core, the shock ring and its rays, sparks, and the damage readout that
 *  punches in while the life points count down -- as resident OBJ tiles
 *  (tools/snes/gen_snes_battle_fx.py) moving apart, since this machine does
 *  not scale a sprite.
 *
 *  VRAM while the battle is up (words):
 *      $0000-$3FFF  BG1 tiles: card slot 0, card slot 1, the frame feet,
 *                   the blank tile 511 (the card check's layout)
 *      $4000-$5FFF  the OBJ effects atlas (OBSEL name base 2)
 *      $6000-$67FF  BG1 map, 32x64: a 512-line period, so a 160-line card
 *                   can leave the 224-line screen without wrapping back in
 *      $6800-$6BFF  BG2 map, 32x32: the stationary figures
 *      $6C00-$6FFF  BG3 map: row 0 is the offset-per-tile row
 *      $7000-$71FF  the 2bpp battle font
 *  CGRAM: the card check's frame (0..31, 128..143) and art (32..111,
 *  160..239) entries; OBJ palettes 1 and 7 (144..159, 240..255) are the
 *  effects', which the card presentation leaves free.
 *
 *  Timing is in DISPLAYED FIELDS: the sequencer advances on every field and
 *  every cue is a crossing of a field count, so a late frame can neither
 *  lose a sound nor play it twice.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_BATTLE_H
#define WAIFU_SNES_BATTLE_H

#include "snes_types.h"

/* Enter the battle for the rules' last attack (g_duel.last_battle and its
 * neighbours), under force blank.  Copies everything it needs first: the
 * rules event is cleared by the caller afterwards and never consulted
 * again.  The caller turns the screen on. */
void snesBattleBegin(void);
/* One displayed field.  `down` is the pad's new presses; A, B or START
 * after the minimum readable interval skips to the end.  Returns 1 once
 * the sequence is over and the board may be restored; 2 when the blow
 * decided the duel and the player has dismissed the verdict shown over
 * the cards -- the board is not restored, the duel is over. */
u8   snesBattleStep(u16 down);
/* The vblank work: the offset row, the text map when it changed, the
 * cooling palette, the colour-math flash.  OAM goes up through
 * snesObjVblank as always. */
void snesBattleVblank(void);

/* The direct attack's burst on its own, for the Thunder scene (snes_duel.c):
 * Load puts the atlas, its palette and the OBJ base up under force blank,
 * Burst emits field t of SNES_FX_FIELDS (snes_battle_data.h) at (cx, cy)
 * into the open sprite list, and FxVblank carries the cooling palette. */
void snesBattleFxLoad(void);
void snesBattleFxBurst(s16 cx, s16 cy, u16 t);
void snesBattleFxVblank(void);

/* For the frame stamp: the current phase and the displayed field. */
u8   snesBattlePhase(void);
u16  snesBattleField(void);
u8   snesBattleIsDirect(void);
u16  snesBattleDamage(void);

#endif /* WAIFU_SNES_BATTLE_H */
