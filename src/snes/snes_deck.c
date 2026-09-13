/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_deck.c — the SNES deck editor.
 *
 *  This is the PC-FX/FM TOWNS editor: DECK and STORAGE are two tabs over one
 *  six-by-three gallery, A moves the highlighted card between them, B checks
 *  it, and START only leaves for a duel when the deck really holds forty.
 *
 *  THE PICTURE IS MODE 3 WITH BG1 IN DIRECT COLOUR, the same 8bpp layer the
 *  duel board and the card check use, and nothing here is a sprite.  The
 *  gallery shows eighteen different cards at once, which no OBJ palette
 *  scheme could colour; direct colour needs no palette at all.  What is
 *  baked (tools/snes/gen_snes_deck.py): the panel, tabs and info box as a
 *  small tile pool with a map, and every card as a 40x40 ICON BLOCK -- the
 *  PC-FX 26x34 mini card with its drop shadow on the panel's navy, in a cell
 *  the size of the gallery pitch.  The words are BG2, the scene font at
 *  priority 1, with the white/gold/dim/red inks the PC-FX prints in.
 *
 *  THE GALLERY IS EIGHTEEN VRAM SLOTS of 25 tiles.  A slot's map cells never
 *  change; what changes is the block in its tiles.  A block is DMA'd from
 *  ROM into a WRAM stage, the cursor's red double outline is painted into
 *  the stage if the slot is the highlighted one, and the stage is queued for
 *  the NMI drain (snes_fb.asm) -- the duel board's uploader, which runs at
 *  the very top of vblank with the whole window's budget.  It has to be
 *  that path: by the time pvsneslib's NMI hands the main thread its vblank,
 *  about three kilobytes of DMA are all that fit, and two blocks (3200)
 *  issued from here lost their last tiles.  What the main thread still
 *  uploads is small: the seven text rows that ever change and the tab
 *  strip, reserved out of the drain's budget so the two never collide.
 *
 *  VRAM (words):
 *      $0000-$0FFF   BG1: blank tile 0, the static picture's pool
 *      $1000-$4C7F   BG1: icon slot s at tile 128 + 25 s
 *      $6000-$67FF   BG2 font (character base $6000)
 *      $7000-$73FF   BG1 map        $7400-$77FF   BG2 map
 *  The OBJ sheet lives at $6000-$7FFF in the duel; snesObjInit puts it back.
 *  CGRAM: 0 is the panel's navy backdrop (direct colour 0 is transparent),
 *  1..63 the four text inks.  OBJ palettes are untouched.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_deck.h"
#include "snes_deck_data.h"
#include "snes_save.h"
#include "snes_cards.h"
#include "snes_scene.h"
#include "snes_scene_data.h"
#include "snes_bigcard_data.h"
#include "msx2_cards.h"
#include "msx2_duel.h"
#include "snes_audio.h"
#include "snes_cardart.h"
#include "snes_video.h"

#define DECK_COUNT          SNES_SAVE_DECK_SIZE
#define STORAGE_COUNT       SNES_SAVE_STORAGE_SIZE
#define GRID_COLS            SNES_DECK_GRID_COLS
#define GRID_ROWS            SNES_DECK_GRID_ROWS
#define VISIBLE_CARDS        (GRID_COLS * GRID_ROWS)

#define BG1_MAP_WORD        0x7000u
#define BG2_MAP_WORD        0x7400u
#define FONT_WORD           0x6000u
#define FONT_FIRST          32u
#define FONT_COUNT          64u
#define TEXT_PRIORITY       0x2000u
#define BLANK_CELL          0u          /* the space glyph */
#define SLOT_TILE(s)        (SNES_DECK_ICON_BASE_TILE + (u16)(s) * SNES_DECK_ICON_TILES)
#define SLOT_WORD(s)        (SLOT_TILE(s) * 32u)
#define SLOT_NONE           0xFFu       /* an empty gallery cell */
#define SLOT_UNKNOWN        0xFEu       /* VRAM holds nobody knows what */
#define STAGES              2
/* The NMI drain's whole-vblank allowance (snes_fb.inc FB_BUDGET), out of
 * which the main thread's own row uploads are reserved. */
#define DECK_VBLANK_BUDGET  3584u
#define TEXT_ROW_BYTES      64u
#define TEXT_ROWS_UPLOADED  7u
#define STRIP_BYTES         (SNES_DECK_TAB_STRIP_BYTES)

#define PAL_WHITE 0
#define PAL_GOLD  1
#define PAL_DIM   2
#define PAL_RED   3

/* The text rows sit at y = 8 row + 4: BG2 is scrolled by (-4, -4), which
 * also puts column c at x = 8 c + 4.  The PPU shows map line VOFS + 1 first. */
#define BG2_HOFS  ((u16)(0x400u - 4u))
#define BG2_VOFS  ((u16)(0x3FFu - 4u))
#define BG1_VOFS  0x3FFu
#define ROW_TITLE  1
#define ROW_TABS   3
#define ROW_COUNTS 5
#define ROW_EMPTY  12
#define ROW_NAME   22
#define ROW_STATS  23
#define ROW_HINT   26

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
static u16 deck_text_map[1024] = { 0 };
static u16 deck_text_vram[TEXT_ROWS_UPLOADED * 32];   /* what BG2 holds */
static u8 deck_text_dirty = 0;      /* bit r: text_rows[r] wants uploading */
static u8 deck_text_stale = 0;      /* the words want rebuilding */
static u16 deck_bg1_map[SNES_DECK_MAP_ROWS * 32] = { 0 };
static u8 deck_bg1_dirty = 0;       /* the tab strip wants uploading */
static u8 deck_bg1_tab = 0;         /* the tab the BG1 map shows */
static u8 deck_drain = 0;           /* stages are queued on the NMI drain */
static u8 deck_flash = 0;           /* frames left of the blinking notice */
static u8 deck_flash_pal = PAL_RED;
static const char *deck_flash_msg = 0;
/* What each gallery slot's VRAM holds and what it should hold. */
static u8 slot_vram_face[VISIBLE_CARDS];
static u8 slot_vram_cursor[VISIBLE_CARDS];
static u8 stage_slot[STAGES];       /* SLOT_NONE when the stage is free */
static u8 stage_face[STAGES];
static u8 stage_cursor[STAGES];

static u8 deck_selected_card(void);
static u8 deck_editor_active_count(void);
static u8 *deck_editor_active_array(void);

/* A notice over the hint line for a second and a half, blinking the way the
 * PC-FX's does: red for a refusal, white for something that happened. */
static void deck_notice(const char *s, u8 pal)
{
    deck_flash_msg = s;
    deck_flash_pal = pal;
    deck_flash = 90;
    deck_text_stale = 1;
}
static void deck_say(const char *s)  { deck_notice(s, PAL_RED); }
static void deck_tell(const char *s) { deck_notice(s, PAL_WHITE); }

/* The rows any word ever lands on; only these are cleared and uploaded. */
static const u8 text_rows[TEXT_ROWS_UPLOADED] = {
    ROW_TITLE, ROW_TABS, ROW_COUNTS, ROW_EMPTY, ROW_NAME, ROW_STATS, ROW_HINT
};

static void deck_text_clear(void)
{
    u8 r, c;
    for (r = 0; r < TEXT_ROWS_UPLOADED; ++r)
        for (c = 0; c < 32; ++c)
            deck_text_map[(u16)text_rows[r] * 32 + c] = BLANK_CELL;
}

/* Which of the text rows differ from what BG2 shows.  A cursor move
 * changes two rows, 128 bytes: small enough that both of its blocks still
 * fit the same vblank. */
static void deck_text_diff(void)
{
    u8 r, c;
    for (r = 0; r < TEXT_ROWS_UPLOADED; ++r) {
        const u16 *now = &deck_text_map[(u16)text_rows[r] * 32];
        u16 *was = &deck_text_vram[(u16)r * 32];
        for (c = 0; c < 32; ++c) {
            if (now[c] != was[c]) {
                deck_text_dirty |= (u8)(1u << r);
                for (; c < 32; ++c) was[c] = now[c];
            }
        }
    }
}

static void deck_text_line(u8 row, u8 col, const char *s, u8 pal)
{
    while (*s && col < 32) {
        const u8 c = (u8)*s++;
        if (c != ' ' && c >= FONT_FIRST && c < FONT_FIRST + FONT_COUNT)
            deck_text_map[(u16)row * 32 + col] =
                (u16)(TEXT_PRIORITY | ((u16)pal << 10) | (c - FONT_FIRST));
        ++col;
    }
}

static void deck_text_num(u8 row, u8 col, u16 value, u8 digits, u8 pal)
{
    u8 i;
    col = (u8)(col + digits);
    for (i = 0; i < digits; ++i) {
        --col;
        deck_text_map[(u16)row * 32 + col] =
            (u16)(TEXT_PRIORITY | ((u16)pal << 10) | ('0' - FONT_FIRST + value % 10));
        value /= 10;
    }
}

static void deck_text_centred(u8 row, const char *s, u8 pal)
{
    u8 n = 0;
    while (s[n]) ++n;
    deck_text_line(row, (u8)((32 - n) / 2), s, pal);
}

/* The name column is 30 cells wide; the PC-FX cuts a long name with an
 * ellipsis and so does this. */
static void deck_text_name(u8 row, u8 col, const char *s, u8 width, u8 pal)
{
    char line[32];
    u8 n = 0;
    while (s[n] && n < 31) ++n;
    if (n > width) {
        u8 i;
        for (i = 0; i + 3 < width; ++i) line[i] = s[i];
        line[i++] = '.'; line[i++] = '.'; line[i++] = '.';
        line[i] = 0;
        s = line;
    }
    deck_text_line(row, col, s, pal);
}

static void deck_count_supports(u8 *support, u8 *equip)
{
    u8 i, s = 0, e = 0;
    for (i = 0; i < deck_count; ++i) {
        if (Msx2_IsMonster(deck_cards[i])) continue;
        ++s;
        if (deck_cards[i] == MSX2_CARD_COUNT + MSX2_SUP_EQUIP) ++e;
    }
    *support = s;
    *equip = e;
}

/* Every word on the screen, top to bottom: draw_deck_editor()'s text. */
static void deck_text_build_editor(void)
{
    const u8 selected = deck_selected_card();
    const u8 count = deck_editor_active_count();
    u8 support, equip;

    deck_text_clear();
    deck_text_centred(ROW_TITLE, "DECK EDITOR", PAL_GOLD);

    deck_text_line(ROW_TABS, 2, "DECK", deck_tab == 0 ? PAL_WHITE : PAL_DIM);
    deck_text_num(ROW_TABS, 7, deck_count, 2, deck_tab == 0 ? PAL_WHITE : PAL_DIM);
    deck_text_line(ROW_TABS, 9, "/40", deck_tab == 0 ? PAL_WHITE : PAL_DIM);
    deck_text_line(ROW_TABS, 18, "STORAGE", deck_tab == 1 ? PAL_WHITE : PAL_DIM);
    deck_text_num(ROW_TABS, 26, storage_count, 2, deck_tab == 1 ? PAL_WHITE : PAL_DIM);

    deck_count_supports(&support, &equip);
    deck_text_line(ROW_COUNTS, 7, "SUPPORT", PAL_GOLD);
    deck_text_num(ROW_COUNTS, 15, support, 2, PAL_GOLD);
    deck_text_line(ROW_COUNTS, 19, "EQ", PAL_GOLD);
    deck_text_num(ROW_COUNTS, 22, equip, 2, PAL_GOLD);

    if (!count) deck_text_centred(ROW_EMPTY, "EMPTY", PAL_DIM);

    if (count) {
        deck_text_name(ROW_NAME, 1, snesCardInfoName(selected), 29, PAL_WHITE);
        if (Msx2_IsMonster(selected)) {
            deck_text_line(ROW_STATS, 1, "ATK", PAL_GOLD);
            deck_text_num(ROW_STATS, 5, Msx2_CardAtk(selected), 4, PAL_GOLD);
            deck_text_line(ROW_STATS, 10, "DEF", PAL_GOLD);
            deck_text_num(ROW_STATS, 14, Msx2_CardDef(selected), 4, PAL_GOLD);
        } else {
            deck_text_name(ROW_STATS, 1, snesCardInfoKind(selected), 29, PAL_GOLD);
        }
    }
    /* The red warning blinks where the hint line is; the PC-FX puts it just
     * above its info box, which 224 lines have no room for. */
    if (deck_flash && ((deck_flash / 8) & 1) == 0)
        deck_text_centred(ROW_HINT, deck_flash_msg, deck_flash_pal);
    else
        deck_text_line(ROW_HINT, 1, "A MOVE B CHECK X TAB Y SAVE", PAL_WHITE);
    deck_text_diff();
}

/* ── The gallery slots ─────────────────────────────────────────────────── */

static u8 wram_bank_of(const u8 *p)
{
    /* The bank is the pointer's third byte (the stages are the assembly
     * file's $7E section, but nothing here needs to know that). */
    return ((const u8 *)&p)[2];
}

/* The stages and the cursor ring painter (snes_deck_ring.asm). */
extern u8 snes_deck_stage[];
void snesDeckPaintRing(u16 stage);
#define STAGE(k) (&snes_deck_stage[(u16)(k) * SNES_DECK_ICON_BYTES])

#define STAGE_ICON(sym, off) \
    snesFbWramDma((u16)(u16)&sym[off], (u16)SNES_DECK_ICON_BANK0 + bank, \
                  (u16)(u16)STAGE(k), wram_bank_of(STAGE(k)), \
                  SNES_DECK_ICON_BYTES)

static void stage_fill(u8 k, u8 slot, u8 face, u8 cursor)
{
    if (face == SLOT_NONE) {
        /* All zero: every texel transparent, which is the navy. */
        snesFbWramFill((u16)(u16)STAGE(k), wram_bank_of(STAGE(k)),
                       SNES_DECK_ICON_BYTES, 0);
    } else {
        const u8 bank = (u8)(face / SNES_DECK_ICONS_PER_BANK);
        const u16 off = (u16)(face % SNES_DECK_ICONS_PER_BANK) * SNES_DECK_ICON_BYTES;
        SNES_DECK_ICON_BANK_SWITCH(bank, off, STAGE_ICON);
    }
    if (cursor) snesDeckPaintRing(k);
    stage_slot[k] = slot;
    stage_face[k] = face;
    stage_cursor[k] = cursor;
}

/* What the main thread will DMA itself in the coming vblank. */
u16 snesDeckVblankBytes(void)
{
    u16 bytes = 0;
    u8 r;
    for (r = 0; r < TEXT_ROWS_UPLOADED; ++r)
        if (deck_text_dirty & (1u << r)) bytes += TEXT_ROW_BYTES;
    if (deck_bg1_dirty) bytes += STRIP_BYTES;
    return bytes;
}

/* A stage's block is in VRAM: the slot now holds what the stage held. */
static void stage_done(u8 k)
{
    slot_vram_face[stage_slot[k]] = stage_face[k];
    slot_vram_cursor[stage_slot[k]] = stage_cursor[k];
    stage_slot[k] = SLOT_NONE;
}

/* One stage straight to VRAM: force blank only. */
static void stage_flush_now(u8 k)
{
    if (stage_slot[k] == SLOT_NONE) return;
    dmaCopyVram((u8 *)STAGE(k), SLOT_WORD(stage_slot[k]),
                SNES_DECK_ICON_BYTES);
    stage_done(k);
}

/* Put the gallery's next out-of-date slots on the free stages, and with
 * the drain running queue them for the NMI -- as many whole blocks as fit
 * beside the main thread's own uploads, so no block is ever split across
 * two vblanks and shown half old. */
static void deck_refresh_slots(void)
{
    const u8 count = deck_editor_active_count();
    const u8 *array = deck_editor_active_array();
    const u16 reserved = snesDeckVblankBytes();
    u8 k = 0, i, pushed = 0;

    if (deck_drain) {
        if (snesFbJobsPending()) return;    /* last frame's blocks not up yet */
        for (i = 0; i < STAGES; ++i)
            if (stage_slot[i] != SLOT_NONE) stage_done(i);
    }
    for (i = 0; i < VISIBLE_CARDS && k < STAGES; ++i) {
        const u8 index = (u8)(deck_scroll[deck_tab] + i);
        const u8 face = index < count ? array[index] : SLOT_NONE;
        const u8 cursor = (index < count && index == deck_cursor) ? 1 : 0;
        if (slot_vram_face[i] == face && slot_vram_cursor[i] == cursor) continue;
        if (deck_drain &&
            reserved + (u16)(pushed + 1) * SNES_DECK_ICON_BYTES > DECK_VBLANK_BUDGET)
            break;
        while (k < STAGES && stage_slot[k] != SLOT_NONE) ++k;
        if (k >= STAGES) break;
        stage_fill(k, i, face, cursor);
        if (deck_drain) {
            snesFbJobPush((u16)(u16)STAGE(k), wram_bank_of(STAGE(k)),
                          SLOT_WORD(i), SNES_DECK_ICON_BYTES, SNES_JOB_DMA);
            ++pushed;
        }
        ++k;
    }
}

static u8 deck_slots_outstanding(void)
{
    const u8 count = deck_editor_active_count();
    const u8 *array = deck_editor_active_array();
    u8 i;
    for (i = 0; i < STAGES; ++i)
        if (stage_slot[i] != SLOT_NONE) return 1;
    for (i = 0; i < VISIBLE_CARDS; ++i) {
        const u8 index = (u8)(deck_scroll[deck_tab] + i);
        const u8 face = index < count ? array[index] : SLOT_NONE;
        const u8 cursor = (index < count && index == deck_cursor) ? 1 : 0;
        if (slot_vram_face[i] != face || slot_vram_cursor[i] != cursor) return 1;
    }
    return 0;
}

/* The BG1 map: the baked picture with this tab's strip over the tab rows and
 * every gallery slot's 25 cells naming its own tiles. */
static void deck_bg1_build(void)
{
    const u16 *src = (const u16 *)snes_deck_bg1_map;
    const u16 *strip = (const u16 *)snes_deck_bg1_tabs;
    u16 i;
    u8 slot, r, c;
    for (i = 0; i < SNES_DECK_MAP_ROWS * 32; ++i) deck_bg1_map[i] = src[i];
    strip += (u16)deck_tab * (SNES_DECK_TAB_ROWS * 32);
    for (i = 0; i < SNES_DECK_TAB_ROWS * 32; ++i)
        deck_bg1_map[SNES_DECK_TAB_ROW0 * 32 + i] = strip[i];
    for (slot = 0; slot < VISIBLE_CARDS; ++slot) {
        const u8 col0 = (u8)(SNES_DECK_GRID_COL0 + (slot % GRID_COLS) * 5);
        const u8 row0 = (u8)(SNES_DECK_GRID_ROW0 + (slot / GRID_COLS) * 5);
        for (r = 0; r < 5; ++r)
            for (c = 0; c < 5; ++c)
                deck_bg1_map[(u16)(row0 + r) * 32 + col0 + c] =
                    (u16)(SNES_DECK_BG1_PAL_BITS | (SLOT_TILE(slot) + r * 5 + c));
    }
    deck_bg1_tab = deck_tab;
    deck_bg1_dirty = 1;
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
    deck_tell(deck_tab ? "ADDED TO DECK" : "ADDED TO STORAGE");
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
                          storage_cards, storage_count)) deck_tell("SAVED");
    else deck_say("SAVE FAILED");
}

u8 snesDeckSaveCurrent(void)
{
    deck_ensure();
    return snesSaveStoreDeck(active_slot, deck_cards, deck_count,
                             storage_cards, storage_count);
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
    deck_tell("SLOT CHANGED");
}

static void deck_draw_editor(void)
{
    if (deck_text_stale) {
        deck_text_build_editor();
        deck_text_stale = 0;
    }
    if (deck_bg1_tab != deck_tab) deck_bg1_build();
    deck_refresh_slots();
}

/* The card check is the same Mode 3 picture the duel shows: the PC-FX
 * battle card on the left and its text column on the right, on BG1 and BG2.
 * Entering it takes over the whole display; leaving it rebuilds the editor's
 * own screen, which is what deck_screen_enter is for. */
static void deck_draw_preview(void)
{
    snesFbCancel();
    deck_drain = 0;
    snesCardArtEnter(1);
    snesCardArtCheck(preview_card, 0, 0, 0);
    snesCardArtVblank();
    setScreenOn();
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

/* The editor's own display, from force blank: Mode 3, BG1 direct colour
 * with the baked picture, BG2 the font, the navy backdrop and the inks.
 * Every gallery slot is then filled before the caller turns the screen on,
 * so the first visible frame is the whole editor.  Entered from the title
 * and again on the way back from the card check, which replaces every one
 * of these registers and most of VRAM. */
static void deck_screen_enter(void)
{
    u8 i;
    setScreenOff();
    snesFbCancel();
    deck_drain = 0;
    REG_HDMAEN = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_TS = 0;
    setMode(BG_MODE3, 0);
    REG_BG1SC = (u8)(BG1_MAP_WORD >> 8);
    REG_BG2SC = (u8)(BG2_MAP_WORD >> 8);
    REG_BG12NBA = (u8)(FONT_WORD >> 8);
    REG_BG1HOFS = 0; REG_BG1HOFS = 0;
    REG_BG1VOFS = (u8)BG1_VOFS; REG_BG1VOFS = (u8)(BG1_VOFS >> 8);
    REG_BG2HOFS = (u8)BG2_HOFS; REG_BG2HOFS = (u8)(BG2_HOFS >> 8);
    REG_BG2VOFS = (u8)BG2_VOFS; REG_BG2VOFS = (u8)(BG2_VOFS >> 8);
    REG_CGWSEL = 0x01;                  /* BG1 is direct colour */

    dmaCopyVram((u8 *)snes_scene_font, FONT_WORD, SNES_SCENE_GLYPH_BYTES);
    dmaCopyVram((u8 *)snes_deck_bg1_tiles, 0, SNES_DECK_STATIC_BYTES);
    dmaCopyCGram((u8 *)snes_deck_text_pal, 0, SNES_DECK_TEXT_PAL_BYTES);
    /* CGRAM 0 is the backdrop, and the backdrop is the panel. */
    REG_CGADD = 0;
    *(vuint8 *)0x2122 = (u8)SNES_DECK_NAVY_BGR555;
    *(vuint8 *)0x2122 = (u8)(SNES_DECK_NAVY_BGR555 >> 8);

    for (i = 0; i < VISIBLE_CARDS; ++i) {
        slot_vram_face[i] = SLOT_UNKNOWN;
        slot_vram_cursor[i] = 0;
    }
    for (i = 0; i < STAGES; ++i) stage_slot[i] = SLOT_NONE;
    deck_bg1_tab = 0xFF;
    deck_text_stale = 1;
    deck_draw_editor();
    while (deck_slots_outstanding()) {
        deck_refresh_slots();
        for (i = 0; i < STAGES; ++i) stage_flush_now(i);
    }
    dmaCopyVram((u8 *)deck_bg1_map, BG1_MAP_WORD, SNES_DECK_MAP_BYTES);
    dmaCopyVram((u8 *)deck_text_map, BG2_MAP_WORD, SNES_SCENE_MAP_BYTES);
    deck_bg1_dirty = 0;
    deck_text_dirty = 0;                /* the diff already mirrored every row */
    REG_TM = BG1_ENABLE | BG2_ENABLE;
    snesFbDrain(1);
    deck_drain = 1;
}

void snesDeckInit(void)
{
    deck_ensure();
    preview_active = 0;
    deck_flash_msg = 0;
    deck_flash = 0;
    deck_screen_enter();
}

u8 snesDeckFrame(void)
{
    const u16 down = padsDown(0);

    deck_ensure();
    if (deck_flash) {
        --deck_flash;
        /* The warning blinks every eight frames and goes at zero. */
        if ((deck_flash & 7) == 0) deck_text_stale = 1;
    }
    if (down & (KEY_L | KEY_START | KEY_SELECT | KEY_R | KEY_Y | KEY_X |
                KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN | KEY_A | KEY_B))
        deck_text_stale = 1;

    if (preview_active) {
        if (down & (KEY_A | KEY_B | KEY_START | KEY_L)) {
            preview_active = 0;
            deck_screen_enter();
            setScreenOn();
        }
        return SNES_SCENE_COUNT;
    }

    if (down & KEY_L) {
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        snesFbCancel();
        deck_drain = 0;
        return SNES_SCENE_TITLE;
    }
    if (down & KEY_START) {
        if (deck_count == DECK_COUNT) {
            snesAudioSfx(SNES_SFX_CONFIRM_ALT);
            snesFbCancel();
            deck_drain = 0;
            return SNES_SCENE_DUEL;
        }
        deck_say("DECK MUST BE 40");
    }
    if (down & KEY_SELECT) {
        deck_change_slot();
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_R) {
        deck_default();
        deck_tell("DEFAULT DECK");
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
    }
    if (down & KEY_Y) {
        deck_save_current();
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
    }
    if (down & KEY_X) {
        deck_editor_switch_tab();
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_LEFT) {
        deck_editor_move_cursor(-1, 0);
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_RIGHT) {
        deck_editor_move_cursor(1, 0);
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_UP) {
        deck_editor_move_cursor(0, -1);
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_DOWN) {
        deck_editor_move_cursor(0, 1);
        snesAudioSfx(SNES_SFX_SELECT);
    }
    if (down & KEY_A) {
        deck_editor_move_selected_card();
        snesAudioSfx(SNES_SFX_CARD_PLACED);
    }
    if ((down & KEY_B) && deck_editor_active_count()) {
        preview_card = deck_selected_card();
        preview_active = 1;
        snesAudioSfx(SNES_SFX_CONFIRM_ALT);
        deck_draw_preview();
        return SNES_SCENE_COUNT;
    }

    deck_draw_editor();
    return SNES_SCENE_COUNT;
}

/* The main thread's share of the vblank, after the NMI drain has put the
 * blocks up: the tab strip and the text rows, a few hundred bytes. */
void snesDeckVblank(void)
{
    u8 r;
    if (preview_active) {
        snesCardArtVblank();
        return;
    }
    if (deck_bg1_dirty) {
        dmaCopyVram((u8 *)&deck_bg1_map[SNES_DECK_TAB_ROW0 * 32],
                    (u16)(BG1_MAP_WORD + SNES_DECK_TAB_ROW0 * 32), STRIP_BYTES);
        deck_bg1_dirty = 0;
    }
    for (r = 0; r < TEXT_ROWS_UPLOADED; ++r) {
        if (!(deck_text_dirty & (1u << r))) continue;
        dmaCopyVram((u8 *)&deck_text_map[(u16)text_rows[r] * 32],
                    (u16)(BG2_MAP_WORD + (u16)text_rows[r] * 32), TEXT_ROW_BYTES);
    }
    deck_text_dirty = 0;
}
