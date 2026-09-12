/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_save.h — the small, checksummed SRAM record used by the SNES fork.
 *
 *  The cartridge has 8 KB of battery SRAM.  The editor keeps four compact
 *  collections, each with a 40-card DECK list and a 64-card STORAGE list;
 *  keeping the record inside the first 512 bytes leaves the rest available
 *  for story progress later.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_SAVE_H
#define WAIFU_SNES_SAVE_H

#include "snes_types.h"

#define SNES_SAVE_SLOT_COUNT 4
#define SNES_SAVE_DECK_SIZE  40
#define SNES_SAVE_STORAGE_SIZE 64
#define SNES_SAVE_SLOT_BYTES (2 + SNES_SAVE_DECK_SIZE + SNES_SAVE_STORAGE_SIZE)
#define SNES_SAVE_BYTES      512

void snesSaveInit(void);
u8   snesSaveIsValid(void);
u8   snesSaveActiveSlot(void);
void snesSaveSetActiveSlot(u8 slot);
u8   snesSaveLoadDeck(u8 slot, u8 *dst, u8 *count);
u8   snesSaveLoadStorage(u8 slot, u8 *dst, u8 *count);
u8   snesSaveStoreDeck(u8 slot, const u8 *deck, u8 deck_count,
                       const u8 *storage, u8 storage_count);
u8   snesSaveStoryProgress(void);
void snesSaveStoryProgressStore(u8 progress);

#endif /* WAIFU_SNES_SAVE_H */
