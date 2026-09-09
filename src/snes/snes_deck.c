/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_deck.c — the SNES deck editor.
 *
 *  This is intentionally the same editor model as the PC-FX/FM TOWNS screen:
 *  DECK and STORAGE are two tabs over one six-column gallery.  A moves the
 *  highlighted card between them, B checks it, and START only leaves for a
 *  duel when the deck really contains forty cards.  The card art is the same
 *  OBJ sheet the duel uses; the panel behind it is a resident Mode 3 screen.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_deck.h"
#include "snes_save.h"
#include "snes_obj.h"
#include "snes_cards.h"
#include "snes_scene.h"
#include "snes_scene_data.h"
#include "msx2_cards.h"
#include "msx2_duel.h"
#include "snes_deck_data.h"

#define DECK_COUNT          SNES_SAVE_DECK_SIZE
#define STORAGE_COUNT       SNES_SAVE_STORAGE_SIZE
#define GRID_COLS            6
#define GRID_ROWS            3
#define VISIBLE_CARDS        (GRID_COLS * GRID_ROWS)
#define CARD_X0              14
#define CARD_Y0              48
#define CARD_PITCH_X         40
#define CARD_PITCH_Y         45

static u8 deck_cards[DECK_COUNT] = { 0 };
static u8 storage_cards[STORAGE_COUNT] = { 0 };
static u8 deck_count = 0;
static u8 storage_count = 0;
static u8 deck_tab = 0;             /* 0 = DECK, 1 = STORAGE */
static u8 deck_cursor = 0;
static u8 deck_scroll[2] = { 0, 0 };
static u8 active_slot = 0;
static u8 deck_ready = 0;
static u8 preview_active = 0;
static u8 preview_card = 0;
static const char *deck_status = 0;
static u8 deck_status_timer = 0;
static u16 deck_text_map[1024] = { 0 };
static u8 deck_text_dirty = 0;

static u8 deck_selected_card(void);

static void deck_say(const char *s)
{
    deck_status = s;
    deck_status_timer = 90;
}

static void deck_text_clear(void)
{
    u16 i;
    for (i = 0; i < 1024; ++i) deck_text_map[i] = 0x2000;
}

static void deck_text_line(u8 row, u8 first_col, const char *s)
{
    u8 col = 0;
    while (*s && (u8)(first_col + col) < 32) {
        const u8 c = (u8)*s++;
        if (c >= SNES_SPR_GLYPH_FIRST &&
            c < SNES_SPR_GLYPH_FIRST + SNES_SPR_GLYPH_COUNT)
            deck_text_map[(u16)row * 32 + first_col + col] =
                (u16)(0x2000 + c - SNES_SPR_GLYPH_FIRST);
        ++col;
    }
}

static void deck_text_num(u8 row, u8 first_col, u16 value, u8 digits)
{
    u8 i;
    u8 col = (u8)(first_col + digits);
    for (i = 0; i < digits; ++i) {
        const u8 digit = (u8)(value % 10);
        --col;
        deck_text_map[(u16)row * 32 + col] =
            (u16)(0x2000 + '0' - SNES_SPR_GLYPH_FIRST + digit);
        value /= 10;
    }
}

static void deck_text_build_editor(void)
{
    deck_text_clear();
    deck_text_line(1, 10, "DECK EDITOR");
    deck_text_line(4, 3, "DECK");
    deck_text_num(4, 8, deck_count, 2);
    deck_text_line(4, 10, "/40");
    deck_text_line(4, 19, "STORAGE");
    deck_text_num(4, 27, storage_count, 2);
    deck_text_line(23, 1, snesCardName(deck_selected_card()));
    if (Msx2_IsMonster(deck_selected_card())) {
        deck_text_line(24, 1, "ATK");
        deck_text_num(24, 5, Msx2_CardAtk(deck_selected_card()), 4);
        deck_text_line(24, 12, "DEF");
        deck_text_num(24, 16, Msx2_CardDef(deck_selected_card()), 4);
    } else {
        deck_text_line(24, 1, "SUPPORT CARD");
    }
    if (deck_status) deck_text_line(25, 1, deck_status);
    deck_text_line(26, 1, "A MOVE  B CHECK  X SWAP  Y SAVE");
    deck_text_dirty = 1;
}

static void deck_text_build_preview(void)
{
    deck_text_clear();
    deck_text_line(1, 11, "CARD CHECK");
    deck_text_line(11, 9, snesCardName(preview_card));
    if (Msx2_IsMonster(preview_card)) {
        deck_text_line(13, 9, "ATK");
        deck_text_num(13, 13, Msx2_CardAtk(preview_card), 4);
        deck_text_line(13, 20, "DEF");
        deck_text_num(13, 24, Msx2_CardDef(preview_card), 4);
    } else {
        deck_text_line(13, 9, "SUPPORT CARD");
    }
    deck_text_line(23, 10, "B BACK");
    deck_text_dirty = 1;
}

static void deck_clear_arrays(void)
{
    u8 i;
    for (i = 0; i < DECK_COUNT; ++i) deck_cards[i] = 0;
    for (i = 0; i < STORAGE_COUNT; ++i) storage_cards[i] = 0;
}

static void deck_default(void)
{
    u8 i;

    deck_clear_arrays();
    /* First boot mirrors the reference editor's useful starting state: a
     * complete forty-card deck plus a small STORAGE reward pool.  The support
     * mix deliberately matches the reference counters (18 support cards,
     * including four equips), while card 0 is first so the regression can
     * prove a move changed the head to card 7. */
    deck_count = 40;
    storage_count = 4;
    deck_cards[0] = 0;
    deck_cards[1] = 7;
    for (i = 2; i < 22; ++i)
        deck_cards[i] = (u8)(((u16)i * 7u) % MSX2_CARD_COUNT);
    for (i = 22; i < 26; ++i) deck_cards[i] = MSX2_CARD_COUNT;
    for (i = 26; i < 30; ++i) deck_cards[i] = MSX2_CARD_COUNT + 1;
    for (i = 30; i < 34; ++i) deck_cards[i] = MSX2_CARD_COUNT + 2;
    for (i = 34; i < 37; ++i) deck_cards[i] = MSX2_CARD_COUNT + 3;
    for (i = 37; i < 39; ++i) deck_cards[i] = MSX2_CARD_COUNT + 4;
    deck_cards[39] = MSX2_CARD_COUNT + 5;
    storage_cards[0] = MSX2_CARD_COUNT + 5;
    storage_cards[1] = MSX2_CARD_COUNT + 4;
    storage_cards[2] = MSX2_CARD_COUNT + 3;
    storage_cards[3] = MSX2_CARD_COUNT + 2;
    deck_tab = 0;
    deck_cursor = 0;
    deck_scroll[0] = 0;
    deck_scroll[1] = 0;
}

static u8 *deck_editor_active_array(void)
{
    return deck_tab ? storage_cards : deck_cards;
}

static u8 deck_editor_active_count(void)
{
    return deck_tab ? storage_count : deck_count;
}

static void deck_editor_clamp_cursor(void)
{
    const u8 count = deck_editor_active_count();
    u8 cur_row, scroll_row, max_scroll_row;

    if (!count) {
        deck_cursor = 0;
        deck_scroll[deck_tab] = 0;
        return;
    }
    if (deck_cursor >= count) deck_cursor = (u8)(count - 1);

    cur_row = (u8)(deck_cursor / GRID_COLS);
    scroll_row = (u8)(deck_scroll[deck_tab] / GRID_COLS);
    if (cur_row < scroll_row) scroll_row = cur_row;
    if (cur_row >= (u8)(scroll_row + GRID_ROWS))
        scroll_row = (u8)(cur_row - GRID_ROWS + 1);
    max_scroll_row = (u8)((count - 1) / GRID_COLS);
    if (max_scroll_row >= GRID_ROWS) max_scroll_row = (u8)(max_scroll_row - GRID_ROWS + 1);
    else max_scroll_row = 0;
    if (scroll_row > max_scroll_row) scroll_row = max_scroll_row;
    deck_scroll[deck_tab] = (u8)(scroll_row * GRID_COLS);
}

static void deck_editor_switch_tab(void)
{
    deck_tab ^= 1;
    deck_editor_clamp_cursor();
}

static void deck_editor_move_cursor(s8 dx, s8 dy)
{
    const u8 count = deck_editor_active_count();
    u16 target;

    if (!count) {
        deck_editor_switch_tab();
        return;
    }
    if (dy < 0) {
        if (deck_cursor < GRID_COLS) {
            deck_editor_switch_tab();
            return;
        }
        deck_cursor = (u8)(deck_cursor - GRID_COLS);
    } else if (dy > 0) {
        target = (u16)deck_cursor + GRID_COLS;
        if (target < count) deck_cursor = (u8)target;
        else if (deck_cursor < (u8)(count - 1)) deck_cursor = (u8)(count - 1);
    }
    if (dx < 0) {
        if (deck_cursor) --deck_cursor;
        else deck_cursor = (u8)(count - 1);
    } else if (dx > 0) {
        if (++deck_cursor >= count) deck_cursor = 0;
    }
    deck_editor_clamp_cursor();
}

static void deck_remove_at(u8 *array, u8 *count, u8 index)
{
    u8 i;
    if (!array || !count || index >= *count) return;
    for (i = index; (u8)(i + 1) < *count; ++i) array[i] = array[i + 1];
    --(*count);
    array[*count] = 0;
}

static u8 deck_copies(u8 card)
{
    u8 i, copies = 0;
    for (i = 0; i < deck_count; ++i)
        if (deck_cards[i] == card) ++copies;
    return copies;
}

static void deck_editor_move_selected_card(void)
{
    u8 *active = deck_editor_active_array();
    const u8 count = deck_editor_active_count();
    u8 card;

    if (!count) {
        deck_say("EMPTY");
        return;
    }
    card = active[deck_cursor];
    if (deck_tab == 0) {
        if (storage_count >= STORAGE_COUNT) {
            deck_say("STORAGE FULL");
            return;
        }
        deck_remove_at(deck_cards, &deck_count, deck_cursor);
        storage_cards[storage_count++] = card;
    } else {
        if (deck_count >= DECK_COUNT) {
            deck_say("DECK IS FULL");
            return;
        }
        if (deck_copies(card) >= 4) {
            deck_say("MAX 4 COPIES");
            return;
        }
        deck_remove_at(storage_cards, &storage_count, deck_cursor);
        deck_cards[deck_count++] = card;
    }
    deck_editor_clamp_cursor();
    deck_say(deck_tab ? "ADDED TO DECK" : "ADDED TO STORAGE");
}

static void deck_load_active(void)
{
    u8 loaded_deck_count, loaded_storage_count;
    u8 loaded;

    active_slot = snesSaveActiveSlot();
    loaded_deck_count = 0;
    loaded_storage_count = 0;
    loaded = snesSaveLoadDeck(active_slot, deck_cards, &loaded_deck_count);
    if (loaded == 0) {
        deck_default();
    } else {
        loaded = snesSaveLoadStorage(active_slot, storage_cards,
                                     &loaded_storage_count);
        if (loaded == 0) {
            deck_default();
        } else {
            deck_count = loaded_deck_count;
            storage_count = loaded_storage_count;
            deck_tab = 0;
            deck_cursor = 0;
            deck_scroll[0] = 0;
            deck_scroll[1] = 0;
            deck_editor_clamp_cursor();
        }
    }
    deck_ready = 1;
}

static void deck_ensure(void)
{
    if (deck_ready) return;
    snesSaveInit();
    deck_load_active();
}

static u8 deck_selected_card(void)
{
    const u8 count = deck_editor_active_count();
    const u8 *array = deck_editor_active_array();
    return count ? array[deck_cursor] : 0;
}

static void deck_save_current(void)
{
    if (snesSaveStoreDeck(active_slot, deck_cards, deck_count,
                          storage_cards, storage_count)) deck_say("SAVED");
    else deck_say("SAVE FAILED");
}

static void deck_change_slot(void)
{
    u8 loaded;

    deck_save_current();
    active_slot = (u8)((active_slot + 1) % SNES_SAVE_SLOT_COUNT);
    if (snesSaveIsValid()) snesSaveSetActiveSlot(active_slot);
    loaded = snesSaveLoadDeck(active_slot, deck_cards, &deck_count);
    if (loaded == 0) {
        deck_default();
    } else {
        loaded = snesSaveLoadStorage(active_slot, storage_cards, &storage_count);
        if (loaded == 0) deck_default();
    }
    deck_tab = 0;
    deck_cursor = 0;
    deck_scroll[0] = 0;
    deck_scroll[1] = 0;
    deck_editor_clamp_cursor();
    deck_say("SLOT CHANGED");
}

static void deck_draw_editor(void)
{
    const u8 count = deck_editor_active_count();
    const u8 *array = deck_editor_active_array();
    u8 i;

    deck_text_build_editor();
    snesObjBegin();
    /* The tab geometry and six-column gallery are the PC-FX/FM TOWNS layout,
     * scaled to the SNES card sprite's 32x32 face. */
    snesObjBox(deck_tab ? 136 : 8, 24, 112, 16);

    snesObjCardHiRes(0);
    if (!count) {
        snesObjText(108, 105, "EMPTY");
    } else {
        for (i = 0; i < VISIBLE_CARDS; ++i) {
            const u8 index = (u8)(deck_scroll[deck_tab] + i);
            const u8 col = (u8)(i % GRID_COLS);
            const u8 row = (u8)(i / GRID_COLS);
            if (index >= count) break;
            snesObjCard((s16)(CARD_X0 + col * CARD_PITCH_X),
                        (s16)(CARD_Y0 + row * CARD_PITCH_Y), i, array[index]);
            if (index == deck_cursor)
                snesObjBox((s16)(CARD_X0 + col * CARD_PITCH_X - 3),
                           (s16)(CARD_Y0 + row * CARD_PITCH_Y - 3), 38, 38);
        }
    }

    snesObjEnd();
}

static void deck_draw_preview(void)
{
    const u8 selected = preview_card;

    deck_text_build_preview();
    snesObjBegin();
    snesObjCardHiRes(1);
    snesObjCard(112, 40, 0, selected);
    snesObjBox(108, 36, 40, 40);
    snesObjEnd();
}

u8 snesDeckGetCurrent(u8 *dst)
{
    u8 i;
    deck_ensure();
    if (!dst || deck_count != DECK_COUNT) return 0;
    for (i = 0; i < DECK_COUNT; ++i) dst[i] = deck_cards[i];
    return 1;
}

u8 snesDeckSaveValid(void)
{
    deck_ensure();
    return snesSaveIsValid();
}

u8 snesDeckActiveSlot(void)
{
    deck_ensure();
    return active_slot;
}

u8 snesDeckCount(void)
{
    deck_ensure();
    return deck_count;
}

u8 snesDeckStorageCount(void)
{
    deck_ensure();
    return storage_count;
}

u8 snesDeckHead(void)
{
    deck_ensure();
    return deck_count ? deck_cards[0] : 0;
}

void snesDeckInit(void)
{
    deck_ensure();
    preview_active = 0;
    deck_status = 0;
    deck_status_timer = 0;

    setScreenOff();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;
    snesObjInit();
    dmaCopyCGram((u8 *)snes_deck_pal, 0, 256);
    dmaCopyVram((u8 *)snes_scene_font, 0x6000, SNES_SCENE_FONT_BYTES);
    dmaCopyVram((u8 *)snes_deck_tiles, 0, SNES_DECK_TILE_BYTES);
    dmaCopyVram((u8 *)snes_deck_map, 0x7000, SNES_DECK_MAP_BYTES);
    dmaCopyVram((u8 *)deck_text_map, 0x7400, SNES_DECK_MAP_BYTES);
    REG_BG1SC = 0x70;
    REG_BG2SC = 0x74;
    REG_BG12NBA = 0x60;
    REG_BG1HOFS = 0;
    REG_BG1HOFS = 0;
    REG_BG1VOFS = 0;
    REG_BG1VOFS = 0;
    setMode(BG_MODE3, 0);
    REG_TM = BG1_ENABLE | BG2_ENABLE | OBJ_ENABLE;
    REG_TS = 0;
    deck_text_dirty = 0;
    deck_draw_editor();
}

u8 snesDeckFrame(void)
{
    const u16 down = padsDown(0);

    deck_ensure();
    if (deck_status_timer && --deck_status_timer == 0) deck_status = 0;

    if (preview_active) {
        if (down & (KEY_A | KEY_B | KEY_START | KEY_L)) {
            preview_active = 0;
            deck_draw_editor();
        } else {
            deck_draw_preview();
        }
        return SNES_SCENE_COUNT;
    }

    if (down & KEY_L) return SNES_SCENE_TITLE;
    if (down & KEY_START) {
        if (deck_count == DECK_COUNT) return SNES_SCENE_DUEL;
        deck_say("DECK MUST BE 40");
    }
    if (down & KEY_SELECT) deck_change_slot();
    if (down & KEY_R) {
        deck_default();
        deck_say("DEFAULT DECK");
    }
    if (down & KEY_Y) deck_save_current();
    if (down & KEY_X) deck_editor_switch_tab();
    if (down & KEY_LEFT) deck_editor_move_cursor(-1, 0);
    if (down & KEY_RIGHT) deck_editor_move_cursor(1, 0);
    if (down & KEY_UP) deck_editor_move_cursor(0, -1);
    if (down & KEY_DOWN) deck_editor_move_cursor(0, 1);
    if (down & KEY_A) deck_editor_move_selected_card();
    if ((down & KEY_B) && deck_editor_active_count()) {
        preview_card = deck_selected_card();
        preview_active = 1;
    }

    deck_draw_editor();
    return SNES_SCENE_COUNT;
}

void snesDeckVblank(void)
{
    if (deck_text_dirty) {
        dmaCopyVram((u8 *)deck_text_map, 0x7400, SNES_DECK_MAP_BYTES);
        deck_text_dirty = 0;
    }
    snesObjVblank();
}
