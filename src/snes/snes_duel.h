/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_duel.h — duel presentation.
 *
 *  It asks the rules questions and shows the answers; it never drives them.
 *  The rules are src/msx2/msx2_duel.c, compiled into this build unchanged.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_DUEL_H
#define WAIFU_SNES_DUEL_H

#include "snes_types.h"

void snesDuelEnter(void);
u8   snesDuelFrame(void);
void snesDuelConfigure(u8 story_duel, u8 story_mode);
u8   snesDuelMode3Active(void);

#endif /* WAIFU_SNES_DUEL_H */
