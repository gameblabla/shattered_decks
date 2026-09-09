/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_deck.h — the SNES deck editor scene.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_DECK_H
#define WAIFU_SNES_DECK_H

#include "snes_types.h"

void snesDeckInit(void);
u8   snesDeckFrame(void);
void snesDeckVblank(void);

/* Copy the active editor deck for the rules model.  This also lazily loads a
 * valid SRAM deck, or creates the deterministic first-run deck. */
u8   snesDeckGetCurrent(u8 *dst);
u8   snesDeckSaveValid(void);
u8   snesDeckActiveSlot(void);
u8   snesDeckCount(void);
u8   snesDeckStorageCount(void);
u8   snesDeckHead(void);

#endif /* WAIFU_SNES_DECK_H */
