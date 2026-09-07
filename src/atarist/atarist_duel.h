/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_duel.h — the duel screen.
 *
 *  Presentation only.  Every rule lives in src/msx2/msx2_duel.c, which both
 *  forks compile unmodified; this file decides what the camera looks at, what
 *  the cursor is over, and which of the two halves of the raster split each
 *  thing is drawn into.
 *
 *      rows   0 .. 111   ARENA palette — the 3D board, via the chunky C2P
 *      rows 112 .. 199   CARD  palette — hand, HUD, prompt, drawn planar
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_DUEL_H
#define WAIFU_ATARIST_DUEL_H

#include <stdint.h>

/* seed drives the deck shuffle; story_index is 0xFF for a free battle. */
void Atarist_DuelEnter(uint32_t seed, uint8_t story_index);
/* One frame.  `vblanks` is how long the previous frame took, so the camera and
 * the message timer run on real time rather than on frame count. */
void Atarist_DuelStep(int vblanks);
/* Non-zero once the duel has finished and its result screen has been shown. */
int  Atarist_DuelFinished(void);
/* The player pressed Escape out of the duel's top-level state: give up the
 * duel and go back to the title, rather than quitting to the desktop. */
int  Atarist_DuelAbandoned(void);
/* 1 = the player won, -1 = lost, 0 = still running. */
int  Atarist_DuelResult(void);

#endif /* WAIFU_ATARIST_DUEL_H */
