/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_save.c — battery SRAM persistence for the SNES fork.
 *
 *  SRAM is read once when the editor/game first needs it.  A write replaces a
 *  complete record, rather than updating one field in place: an interrupted
 *  save can then only invalidate the record, never leave a half-new deck with
 *  an old checksum that happens to pass.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_save.h"
#include "msx2_cards.h"
#include "msx2_duel.h"

#define SAVE_MAGIC_LO       0x44u       /* "WD" */
#define SAVE_MAGIC_HI       0x57u
#define SAVE_VERSION        2u
#define SAVE_HEADER_BYTES   8u
#define SAVE_CHECKSUM_OFF   (SAVE_HEADER_BYTES + \
                             SNES_SAVE_SLOT_COUNT * SNES_SAVE_SLOT_BYTES)

static u8 save_blob[SNES_SAVE_BYTES] = { 0 };
static u8 save_initialized = 0;
static u8 save_valid = 0;

static u8 save_card_valid(u8 card)
{
    return card < MSX2_TOTAL_CARDS;
}

static u16 save_slot_offset(u8 slot)
{
    return (u16)(SAVE_HEADER_BYTES + (u16)slot * SNES_SAVE_SLOT_BYTES);
}

static u16 save_checksum(void)
{
    u16 sum = 0;
    u16 i;
    for (i = 0; i < SAVE_CHECKSUM_OFF; ++i) sum = (u16)(sum + save_blob[i]);
    return sum;
}

static u8 save_blob_valid(void)
{
    u16 stored;
    u16 i;

    if (save_blob[0] != SAVE_MAGIC_LO || save_blob[1] != SAVE_MAGIC_HI ||
        save_blob[2] != SAVE_VERSION ||
        save_blob[3] >= SNES_SAVE_SLOT_COUNT)
        return 0;
    stored = (u16)save_blob[SAVE_CHECKSUM_OFF] |
             ((u16)save_blob[SAVE_CHECKSUM_OFF + 1] << 8);
    if (stored != save_checksum()) return 0;
    for (i = 0; i < SNES_SAVE_SLOT_COUNT; ++i) {
        const u16 base = save_slot_offset((u8)i);
        u8 j;
        if (save_blob[base] > SNES_SAVE_DECK_SIZE ||
            save_blob[base + 1] > SNES_SAVE_STORAGE_SIZE)
            return 0;
        for (j = 0; j < SNES_SAVE_DECK_SIZE; ++j)
            if (!save_card_valid(save_blob[base + 2 + j])) return 0;
        for (j = 0; j < SNES_SAVE_STORAGE_SIZE; ++j)
            if (!save_card_valid(save_blob[base + 2 + SNES_SAVE_DECK_SIZE + j]))
                return 0;
    }
    return 1;
}

static void save_write_sram(void)
{
    /* The SRAM helper does not share PPU state with the NMI routine, but the
     * record must not be copied while the vblank handler is observing pad/OAM
     * state.  Restore precisely the bits this port had enabled. */
    const u8 nmitimen = REG_NMITIMEN;
    REG_NMITIMEN = (u8)(nmitimen & (u8)~INT_VBLENABLE);
    /* Makefile.snes is HiROM.  pvsneslib's plain helper selects the matching
     * $30:$6000 SRAM window; the offset helper's zero offset is the LoROM
     * $70:0000 layout and silently targets the wrong bus window here. */
    consoleCopySram(save_blob, SNES_SAVE_BYTES);
    REG_NMITIMEN = nmitimen;
}

void snesSaveInit(void)
{
    if (save_initialized) return;
    save_initialized = 1;
    consoleLoadSram(save_blob, SNES_SAVE_BYTES);
    save_valid = save_blob_valid();
}

u8 snesSaveIsValid(void)
{
    snesSaveInit();
    return save_valid;
}

u8 snesSaveActiveSlot(void)
{
    snesSaveInit();
    return save_valid ? save_blob[3] : 0;
}

void snesSaveSetActiveSlot(u8 slot)
{
    u16 sum;
    snesSaveInit();
    if (slot >= SNES_SAVE_SLOT_COUNT) return;
    if (save_valid == 0) return;
    save_blob[3] = slot;
    sum = save_checksum();
    save_blob[SAVE_CHECKSUM_OFF] = (u8)sum;
    save_blob[SAVE_CHECKSUM_OFF + 1] = (u8)(sum >> 8);
    save_write_sram();
}

u8 snesSaveLoadDeck(u8 slot, u8 *dst, u8 *count)
{
    u16 base;
    u8 i;

    snesSaveInit();
    if (save_valid == 0) return 0;
    if (dst == 0) return 0;
    if (slot >= SNES_SAVE_SLOT_COUNT) return 0;
    base = save_slot_offset(slot);
    if (count) *count = save_blob[base];
    for (i = 0; i < SNES_SAVE_DECK_SIZE; ++i)
        dst[i] = save_blob[base + 2 + i];
    return 1;
}

u8 snesSaveLoadStorage(u8 slot, u8 *dst, u8 *count)
{
    u16 base;
    u8 i;

    snesSaveInit();
    if (save_valid == 0) return 0;
    if (dst == 0) return 0;
    if (slot >= SNES_SAVE_SLOT_COUNT) return 0;
    base = save_slot_offset(slot);
    if (count) *count = save_blob[base + 1];
    for (i = 0; i < SNES_SAVE_STORAGE_SIZE; ++i)
        dst[i] = save_blob[base + 2 + SNES_SAVE_DECK_SIZE + i];
    return 1;
}

u8 snesSaveStoreDeck(u8 slot, const u8 *deck, u8 deck_count,
                     const u8 *storage, u8 storage_count)
{
    u16 dst;
    u16 sum;
    u16 i;

    snesSaveInit();
    if (deck == 0) return 0;
    if (storage == 0) return 0;
    if (slot >= SNES_SAVE_SLOT_COUNT) return 0;
    if (deck_count > SNES_SAVE_DECK_SIZE) return 0;
    if (storage_count > SNES_SAVE_STORAGE_SIZE) return 0;
    for (i = 0; i < deck_count; ++i)
        if (save_card_valid(deck[i]) == 0) return 0;
    for (i = 0; i < storage_count; ++i)
        if (save_card_valid(storage[i]) == 0) return 0;

    if (save_valid == 0) {
        /* A new cartridge gets all four slots initialised to the first
         * collection the player saves.  This makes every slot a valid,
         * useful editor state and avoids exposing 0xFF SRAM as card ids after
         * changing slots. */
        for (i = 0; i < SNES_SAVE_BYTES; ++i) save_blob[i] = 0;
        save_blob[0] = SAVE_MAGIC_LO;
        save_blob[1] = SAVE_MAGIC_HI;
        save_blob[2] = SAVE_VERSION;
        save_blob[3] = slot;
        for (i = 0; i < SNES_SAVE_SLOT_COUNT; ++i) {
            u8 j;
            dst = save_slot_offset(i);
            save_blob[dst] = deck_count;
            save_blob[dst + 1] = storage_count;
            for (j = 0; j < SNES_SAVE_DECK_SIZE; ++j)
                save_blob[dst + 2 + j] = (j < deck_count) ? deck[j] : 0;
            for (j = 0; j < SNES_SAVE_STORAGE_SIZE; ++j)
                save_blob[dst + 2 + SNES_SAVE_DECK_SIZE + j] =
                    (j < storage_count) ? storage[j] : 0;
        }
        save_valid = 1;
    } else {
        u8 j;
        dst = save_slot_offset(slot);
        save_blob[dst] = deck_count;
        save_blob[dst + 1] = storage_count;
        for (j = 0; j < SNES_SAVE_DECK_SIZE; ++j)
            save_blob[dst + 2 + j] = (j < deck_count) ? deck[j] : 0;
        for (j = 0; j < SNES_SAVE_STORAGE_SIZE; ++j)
            save_blob[dst + 2 + SNES_SAVE_DECK_SIZE + j] =
                (j < storage_count) ? storage[j] : 0;
        save_blob[3] = slot;
    }

    sum = save_checksum();
    save_blob[SAVE_CHECKSUM_OFF] = (u8)sum;
    save_blob[SAVE_CHECKSUM_OFF + 1] = (u8)(sum >> 8);
    save_write_sram();
    return 1;
}

u8 snesSaveStoryProgress(void)
{
    snesSaveInit();
    return save_valid ? save_blob[4] : 0;
}

void snesSaveStoryProgressStore(u8 progress)
{
    u16 sum;
    snesSaveInit();
    if (!save_valid || progress > MSX2_STORY_MAX_DUELS) return;
    save_blob[4] = progress;
    sum = save_checksum();
    save_blob[SAVE_CHECKSUM_OFF] = (u8)sum;
    save_blob[SAVE_CHECKSUM_OFF + 1] = (u8)(sum >> 8);
    save_write_sram();
}
