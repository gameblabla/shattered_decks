/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_cardart.c — the Mode 3 card presentation.  See snes_cardart.h.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>

#include "snes_cardart.h"
#include "snes_bigcard_data.h"
#include "snes_scene_data.h"
#include "snes_cards.h"
#include "msx2_duel.h"
#include "msx2_cards.h"

#define BG1_MAP_WORD    0x7400u
#define BG2_MAP_WORD    0x7800u
#define FONT_WORD       0x7C00u
#define FONT_TILE       192u
#define FONT_BYTES      2048u          /* the 64 glyphs, not the frame tiles */
#define FONT_FIRST      32u
#define FONT_COUNT      64u
#define FEET_TILE       (SNES_BIGCARD_TILES * 2u)
#define FEET_WORD       (FEET_TILE * 32u)
/* THE BACK'S OWN BOTTOM ROWS.  The cover painting carries its frame, so it
 * is baked edge to edge and its feet are seventy-five tiles in ITS colours,
 * not the shared frame's -- and those do not fit under BLANK_TILE.  They
 * go to the words above the card sheet: $5800 is past the burst atlas the
 * Thunder beat loads at $4000 (snes_battle.c FX_OBJ_WORD, 12 KB) and below
 * the duel's OBJ sheet at $6000, which no Mode 3 screen samples.  Only the
 * victim beat can show a back (the check refuses one, a defender is
 * flipped before its battle), so the battle's layout never meets them. */
#define BACK_FEET_WORD  0x5800u
#define BACK_FEET_TILE  (BACK_FEET_WORD / 32u)
#define BACK_FEET_TILES (SNES_BIGCARD_TILES_X * SNES_BIGCARD_FOOT_ROWS)
#define TEXT_PRIORITY   0x2000u
/* An empty BG2 cell is the space glyph: BG2's character base is $7000, and
 * tile 0 there is the duel's top-view map, not a blank. */
#define BLANK_CELL      ((u16)FONT_TILE)
/* ...and an empty BG1 cell is the last tile of the sheet, kept clear: tile 0
 * is a card's gold corner. */
#define BLANK_TILE      511u
#define SLOT_PAL(slot)  ((u16)((slot) ? 160u : 32u))

/* The 10-bit scroll values that put map cell (0, 0) at (4, 22).  The PPU
 * shows map line VOFS + 1 on the first scanline, hence the extra one. */
#define HOFS            ((u16)(0x400u - SNES_CARDART_X0))
#define VOFS            ((u16)(0x3FFu - SNES_CARDART_Y0))

/* Every static initialised: see snes_duel.c on this compiler's .bss. */
static u16 bg1_map[1024] = { 0 };
static u16 bg2_map[1024] = { 0 };
static u8  bg1_dirty = 0;
static u8  bg2_dirty = 0;
static u8  reveal_pending = 0;
static u8  reveal_value = 255;
static u8  flash_pending = 0;
static u8  flash_value = 0;
static u8  wipe_pending = 0;
static u8  wipe_value = 0;
/* Window 1's edges per line for the wipe: at most two 127-line runs of a
 * full-width window, two of an empty one, and the end mark. */
static u8  hdma_wipe[16];
static u8  plane7_byte = 0xFF;
static const u8 blank_tile[64] = { 0 };

static void vram_fill_high(u16 word, u16 count)
{
    /* One byte, `count` times, into the HIGH byte of every 32nd VRAM word:
     * $2115 = $81 steps the address by one 8bpp tile after each $2119 write,
     * and DMA mode 0 with a fixed A-bus source is the PPU's memset.  The
     * source's bank is the third byte of the pointer: this file's statics
     * land in bank $7F, not the $7E the duel's HDMA tables assume. */
    const u8 *src = &plane7_byte;
    REG_VMAIN = 0x81;
    REG_VMADDLH = word;
    *(vuint8 *)0x4300 = 0x08;
    *(vuint8 *)0x4301 = 0x19;
    *(vuint16 *)0x4302 = (u16)src;
    *(vuint8 *)0x4304 = ((const u8 *)&src)[2];
    *(vuint16 *)0x4305 = count;
    REG_MDMAEN = 1;
    REG_VMAIN = 0x80;
}

/* What both card slots share: the frame feet, the blank tile, and the
 * frame and text colours -- the same frame colours again at 128 for the
 * slot whose tiles carry bitplane 7.  The battle loads these into its own
 * Mode 4 layout (snes_battle.c). */
void snesCardArtLoadCommon(void)
{
    dmaCopyVram((u8 *)snes_bigcard_feet, FEET_WORD,
                SNES_BIGCARD_FEET_TILES * 64u);
    dmaCopyVram((u8 *)blank_tile, BLANK_TILE * 32u, 64);
    dmaCopyCGram((u8 *)snes_bigcard_frame_pal, 0, 64);
    dmaCopyCGram((u8 *)snes_bigcard_frame_pal, 128, 32);
}

void snesCardArtEnter(u8 navy)
{
    u16 i;
    setScreenOff();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_TS = 0;
    setMode(BG_MODE3, 0);
    REG_BG1SC = (u8)(BG1_MAP_WORD >> 8);
    REG_BG2SC = (u8)(BG2_MAP_WORD >> 8);
    REG_BG12NBA = 0x70;
    REG_BG1HOFS = (u8)HOFS; REG_BG1HOFS = (u8)(HOFS >> 8);
    REG_BG1VOFS = (u8)VOFS; REG_BG1VOFS = (u8)(VOFS >> 8);
    REG_BG2HOFS = (u8)HOFS; REG_BG2HOFS = (u8)(HOFS >> 8);
    REG_BG2VOFS = (u8)VOFS; REG_BG2VOFS = (u8)(VOFS >> 8);

    dmaCopyVram((u8 *)snes_scene_font, FONT_WORD, FONT_BYTES);
    snesCardArtLoadCommon();
    if (navy) {
        /* CGRAM 0 is the backdrop: the card check's panel colour, the PC-FX
         * IDX_UI_DARK (7, 10, 43), as the BGR555 word $1420. */
        REG_CGADD = 0;
        *(vuint8 *)0x2122 = 0x20;
        *(vuint8 *)0x2122 = 0x14;
    }

    for (i = 0; i < 1024; ++i) { bg1_map[i] = BLANK_TILE; bg2_map[i] = BLANK_CELL; }
    dmaCopyVram((u8 *)bg1_map, BG1_MAP_WORD, 2048);
    dmaCopyVram((u8 *)bg2_map, BG2_MAP_WORD, 2048);
    bg1_dirty = 0;
    bg2_dirty = 0;
    reveal_value = 255;
    reveal_pending = 0;
    flash_value = 0;
    flash_pending = 0;
    REG_TM = BG1_ENABLE | BG2_ENABLE;
}

#define LOAD_CARD(sym, off) \
    do { \
        dmaCopyVram((u8 *)&sym[off], dest, SNES_BIGCARD_BYTES); \
        dmaCopyCGram((u8 *)&sym[(off) + SNES_BIGCARD_BYTES], SLOT_PAL(slot), \
                     SNES_BIGCARD_PAL_BYTES); \
        if (face == SNES_CARD_BACK) \
            dmaCopyVram((u8 *)&sym[(off) + SNES_BIGCARD_RECORD], \
                        BACK_FEET_WORD, SNES_BIGCARD_BACK_FEET_BYTES); \
    } while (0)

void snesCardArtLoad(u8 slot, u8 face)
{
    u8 bank, r;
    u16 off, dest;
    if (slot > 1 || face >= SNES_CARD_FACES) return;
    bank = (u8)(face / SNES_BIGCARD_PER_BANK);
    off = (u16)(face % SNES_BIGCARD_PER_BANK) * SNES_BIGCARD_RECORD;
    dest = (u16)slot * (SNES_BIGCARD_TILES * 32u);
    SNES_BIGCARD_BANK_SWITCH(bank, off, LOAD_CARD);
    if (slot) {
        /* Slot 1 is the same tiles with bitplane 7 set, so its colours read
         * from 160..239: eight fills, one per tile row, into the odd bytes of
         * each tile's last plane pair (bytes 49, 51, .. 63). */
        for (r = 0; r < 8; ++r)
            vram_fill_high((u16)(dest + 24u + r), SNES_BIGCARD_TILES);
        /* ...and the back's feet with them: they are read through the
         * slot that loaded the back last. */
        if (face == SNES_CARD_BACK)
            for (r = 0; r < 8; ++r)
                vram_fill_high((u16)(BACK_FEET_WORD + 24u + r), BACK_FEET_TILES);
    }
}

/* The map cell of foot tile (tx, ty) of a card in slot `slot`: the shared
 * frame foot of its kind, or the back's own. */
static u16 foot_cell(u8 slot, u8 face, u8 ty, u8 tx)
{
    u8 kind;
    const u8 *foot;
    (void)slot;
    if (face == SNES_CARD_BACK)
        return (u16)(BACK_FEET_TILE + (u16)ty * SNES_BIGCARD_TILES_X + tx);
    kind = (face < SNES_CARD_FACES) ? snesCardInfoFrame(face)
                                    : SNES_BIGCARD_KIND_MONSTER;
    foot = &snes_bigcard_feet_map[(u16)kind * SNES_BIGCARD_TILES_X *
                                  SNES_BIGCARD_FOOT_ROWS * 2u];
    return (u16)(FEET_TILE + foot[((u16)ty * SNES_BIGCARD_TILES_X + tx) * 2]);
}

void snesCardArtPlace(u8 slot, u8 col, u8 face)
{
    u8 tx, ty;
    u16 base = (u16)slot * SNES_BIGCARD_TILES;
    for (ty = 0; ty < SNES_BIGCARD_TOP_ROWS; ++ty)
        for (tx = 0; tx < SNES_BIGCARD_TILES_X; ++tx)
            bg1_map[(u16)ty * 32 + col + tx] =
                (u16)(base + ty * SNES_BIGCARD_TILES_X + tx);
    for (ty = 0; ty < SNES_BIGCARD_FOOT_ROWS; ++ty)
        for (tx = 0; tx < SNES_BIGCARD_TILES_X; ++tx)
            bg1_map[(u16)(SNES_BIGCARD_TOP_ROWS + ty) * 32 + col + tx] =
                foot_cell(slot, face, ty, tx);
    bg1_dirty = 1;
}

/* The fifteen map cells of tile row `ty` (0..19) of a card in slot `slot`,
 * for a map that is not this file's: the battle's 32x64 one. */
void snesCardArtRowCells(u8 slot, u8 face, u8 ty, u16 *cells)
{
    u8 tx;
    const u16 base = (u16)slot * SNES_BIGCARD_TILES;
    for (tx = 0; tx < SNES_BIGCARD_TILES_X; ++tx) {
        if (ty < SNES_BIGCARD_TOP_ROWS)
            cells[tx] = (u16)(base + ty * SNES_BIGCARD_TILES_X + tx);
        else
            cells[tx] = foot_cell(slot, face,
                                  (u8)(ty - SNES_BIGCARD_TOP_ROWS), tx);
    }
}

void snesCardArtClear(void)
{
    u16 i;
    for (i = 0; i < 1024; ++i) bg1_map[i] = BLANK_TILE;
    bg1_dirty = 1;
}

void snesCardArtTextClear(void)
{
    u16 i;
    for (i = 0; i < 1024; ++i) bg2_map[i] = BLANK_CELL;
    bg2_dirty = 1;
}

static void put_char(u8 col, u8 row, u8 c, u8 pal)
{
    if (col >= 32 || row >= 32) return;
    if (c == ' ' || c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT) {
        bg2_map[(u16)row * 32 + col] = BLANK_CELL;
        return;
    }
    bg2_map[(u16)row * 32 + col] =
        (u16)(TEXT_PRIORITY | ((u16)(pal & 7) << 10) | (FONT_TILE + c - FONT_FIRST));
}

void snesCardArtText(u8 col, u8 row, const char *s, u8 pal)
{
    while (*s && col < 32) put_char(col++, row, (u8)*s++, pal);
    bg2_dirty = 1;
}

void snesCardArtNum(u8 col, u8 row, u16 value, u8 digits, u8 pal)
{
    u8 i;
    u8 c = (u8)(col + digits);
    for (i = 0; i < digits; ++i) {
        --c;
        put_char(c, row, (u8)('0' + value % 10), pal);
        value /= 10;
    }
    bg2_dirty = 1;
}

u8 snesCardArtWrap(u8 col, u8 row, u8 width, u8 max_rows, const char *s,
                   u8 pal)
{
    u8 rows = 0;
    while (*s && rows < max_rows) {
        u8 n = 0, cut, i;
        while (n < width && s[n]) ++n;
        cut = n;
        if (n == width && s[n] && s[n] != ' ') {
            while (cut && s[cut] != ' ') --cut;
            if (!cut) cut = n;
        }
        for (i = 0; i < cut; ++i) put_char((u8)(col + i), (u8)(row + rows), (u8)s[i], pal);
        s += cut;
        while (*s == ' ') ++s;
        ++rows;
    }
    bg2_dirty = 1;
    return rows;
}

void snesCardArtStats(u8 col, u8 face, u16 atk, u16 def)
{
    const char *kind;
    u8 pal_a, pal_d;
    if (face >= MSX2_TOTAL_CARDS || !Msx2_IsMonster(face)) return;
    /* The PC-FX prints a modified figure in another colour; here gold. */
    pal_a = (atk != Msx2_CardAtk(face)) ? SNES_CARDART_PAL_GOLD
                                        : SNES_CARDART_PAL_WHITE;
    pal_d = (def != Msx2_CardDef(face)) ? SNES_CARDART_PAL_GOLD
                                        : SNES_CARDART_PAL_WHITE;
    snesCardArtText((u8)(col + 1), 16, "A", pal_a);
    snesCardArtNum((u8)(col + 2), 16, atk, 4, pal_a);
    snesCardArtText((u8)(col + 7), 16, "D", pal_d);
    snesCardArtNum((u8)(col + 8), 16, def, 4, pal_d);
    /* The tribe: what follows " / " in the attribute line. */
    kind = snesCardInfoKind(face);
    while (*kind && *kind != '/') ++kind;
    if (*kind == '/') ++kind;
    while (*kind == ' ') ++kind;
    snesCardArtText((u8)(col + 1), 17, kind, SNES_CARDART_PAL_WHITE);
}

void snesCardArtCheck(u8 face, u8 has_stats, u16 atk, u16 def)
{
    const u8 tx = SNES_CARDART_TEXT_COL;
    const u8 tw = SNES_CARDART_TEXT_W;
    u8 row = 0, stars, i;
    char line[12];

    snesCardArtClear();
    snesCardArtTextClear();
    if (face >= SNES_CARD_FACES) return;
    snesCardArtLoad(0, face);
    snesCardArtPlace(0, SNES_CARDART_COL_L, face);
    if (!has_stats && face < MSX2_TOTAL_CARDS && Msx2_IsMonster(face)) {
        atk = Msx2_CardAtk(face);
        def = Msx2_CardDef(face);
    }
    snesCardArtStats(SNES_CARDART_COL_L, face, atk, def);

    /* The PC-FX column, top to bottom: heading, name, stars, attribute and
     * tribe, the lore, and the figures pinned to the foot of the screen. */
    snesCardArtText(tx, row, "CARD CHECK", SNES_CARDART_PAL_GOLD);
    row = (u8)(row + 2);
    row = (u8)(row + snesCardArtWrap(tx, row, tw, 3, snesCardInfoName(face),
                                     SNES_CARDART_PAL_WHITE));
    if (face < MSX2_TOTAL_CARDS && Msx2_IsMonster(face)) {
        stars = snesCardInfoStars(face);
        for (i = 0; i < 8; ++i) line[i] = (char)(i < stars ? '*' : 0);
        line[8] = 0;
        snesCardArtText(tx, row, "STARS", SNES_CARDART_PAL_WHITE);
        snesCardArtText((u8)(tx + 6), row, line, SNES_CARDART_PAL_GOLD);
        ++row;
        row = (u8)(row + snesCardArtWrap(tx, row, tw, 2, snesCardInfoKind(face),
                                         SNES_CARDART_PAL_WHITE));
        ++row;
        snesCardArtText(tx, row, "LORE", SNES_CARDART_PAL_GOLD);
    } else {
        snesCardArtText(tx, row, "TYPE", SNES_CARDART_PAL_GOLD);
        ++row;
        row = (u8)(row + snesCardArtWrap(tx, row, tw, 2, snesCardInfoKind(face),
                                         SNES_CARDART_PAL_WHITE));
        ++row;
        snesCardArtText(tx, row, "EFFECT", SNES_CARDART_PAL_GOLD);
    }
    ++row;
    row = (u8)(row + snesCardArtWrap(tx, row, tw, 7, snesCardInfoDesc(face),
                                     SNES_CARDART_PAL_WHITE));
    if (face < MSX2_TOTAL_CARDS && Msx2_IsMonster(face)) {
        if (row < 20) row = 20;
        snesCardArtText(tx, row, "ATK", SNES_CARDART_PAL_GOLD);
        snesCardArtNum((u8)(tx + 4), row, atk, 4, SNES_CARDART_PAL_GOLD);
        snesCardArtText(tx, (u8)(row + 1), "DEF", SNES_CARDART_PAL_GOLD);
        snesCardArtNum((u8)(tx + 4), (u8)(row + 1), def, 4, SNES_CARDART_PAL_GOLD);
    }
    snesCardArtText(tx, 24, "B: BACK", SNES_CARDART_PAL_WHITE);
}

/* A SUPPORT CUT-IN USES THE CARD-CHECK COMPOSITION, WITHOUT THE INSPECTION
 * LEGEND.  That makes the cause precede the already-committed rules effect,
 * and gives the opponent's supports the same readable explanation as the
 * player's instead of sending an unplaceable flight toward slot $FF. */
void snesCardArtEffect(u8 face, u8 by_com)
{
    const u8 tx = SNES_CARDART_TEXT_COL;
    u8 row = 0;
    snesCardArtClear();
    snesCardArtTextClear();
    if (face >= SNES_CARD_FACES) return;
    snesCardArtLoad(0, face);
    snesCardArtPlace(0, SNES_CARDART_COL_L, face);
    snesCardArtText(tx, row, by_com ? "OPPONENT USED" : "SUPPORT USED",
                    SNES_CARDART_PAL_GOLD);
    row += 2;
    row = (u8)(row + snesCardArtWrap(tx, row, SNES_CARDART_TEXT_W, 3,
                                     snesCardInfoName(face),
                                     SNES_CARDART_PAL_WHITE));
    ++row;
    snesCardArtText(tx, row++, "EFFECT", SNES_CARDART_PAL_GOLD);
    snesCardArtWrap(tx, row, SNES_CARDART_TEXT_W, 12,
                    snesCardInfoDesc(face), SNES_CARDART_PAL_WHITE);
}

/* THUNDER'S VICTIM IS THE CARD ALONE, IN THE MIDDLE OF THE SCREEN -- the
 * 112x112 painting in its frame, as the PC-FX shows each card the bolt
 * takes -- with the bolt's name over it.  The burst that follows is
 * sprites (snes_battle.c's) and the card then wipes off the screen from
 * the top down through snesCardArtWipe; the words are BG2, which the wipe
 * leaves alone.  Row 30 is eight lines above the card's top (the map is
 * scrolled by 22, so its last rows are the first lines of the screen). */
void snesCardArtVictim(u8 face, const char *title)
{
    u8 col = 0;
    const char *t = title;
    snesCardArtClear();
    snesCardArtTextClear();
    snesCardArtWipe(0);
    if (face >= SNES_CARD_FACES) return;
    snesCardArtLoad(0, face);
    snesCardArtPlace(0, SNES_CARDART_VICTIM_COL, face);
    while (*t++) ++col;
    snesCardArtText((u8)((32 - col) >> 1), 30, title, SNES_CARDART_PAL_GOLD);
}

void snesCardArtReveal(u8 reveal)
{
    reveal_value = reveal;
    reveal_pending = 1;
}

void snesCardArtFlash(u8 level)
{
    flash_value = level > 31 ? 31 : level;
    flash_pending = 1;
}

void snesCardArtWipe(u8 lines)
{
    wipe_value = lines;
    wipe_pending = 1;
}

/* Window 1 covers the whole width of every line above the edge and nothing
 * below it, by an HDMA on WH0/WH1 (mode 1: two registers, one byte each).
 * BG1 is masked inside the window, so the card is gone above the edge and
 * whole below it; a line count is at most 127, so a run may take two. */
static void arm_wipe(void)
{
    u8 *t = hdma_wipe;
    const u8 *src = hdma_wipe;
    u8 left = wipe_value;
    u8 rest = (u8)(224 - wipe_value);
    while (left) {
        const u8 n = left > 127 ? 127 : left;
        *t++ = n; *t++ = 0; *t++ = 255;
        left = (u8)(left - n);
    }
    while (rest) {
        const u8 n = rest > 127 ? 127 : rest;
        *t++ = n; *t++ = 255; *t++ = 0;
        rest = (u8)(rest - n);
    }
    *t = 0;
    REG_HDMAEN = 0;
    *(vuint8 *)0x4340 = 0x01;
    *(vuint8 *)0x4341 = 0x26;
    *(vuint16 *)0x4342 = (u16)src;
    *(vuint8 *)0x4344 = ((const u8 *)&src)[2];
    REG_W12SEL = 0x02;
    REG_TMW = 0x01;
    REG_HDMAEN = 0x10;
}

void snesCardArtVblank(void)
{
    if (bg1_dirty) {
        dmaCopyVram((u8 *)bg1_map, BG1_MAP_WORD, 2048);
        bg1_dirty = 0;
    }
    if (bg2_dirty) {
        dmaCopyVram((u8 *)bg2_map, BG2_MAP_WORD, 2048);
        bg2_dirty = 0;
    }
    if (reveal_pending) {
        reveal_pending = 0;
        if (reveal_value == 255) {
            REG_TMW = 0;
            REG_W12SEL = 0;
        } else {
            /* Both windows inverted and AND-combined: BG1 is masked where it
             * is outside window 1 AND outside window 2, i.e. it shows inside
             * either -- the left card's opening from x = 4 rightwards and the
             * right card's from x = 251 leftwards. */
            u8 r = reveal_value > 120 ? 120 : reveal_value;
            REG_W12SEL = 0x0F;
            *(vuint8 *)0x212A = 0x01;
            REG_TMW = 0x01;
            REG_WH0 = SNES_CARDART_X0;
            REG_WH1 = (u8)(SNES_CARDART_X0 + r);
            *(vuint8 *)0x2128 = (u8)(251 - r);
            *(vuint8 *)0x2129 = 251;
        }
    }
    if (wipe_pending) {
        wipe_pending = 0;
        if (!wipe_value) {
            REG_HDMAEN = 0;
            REG_TMW = 0;
            REG_W12SEL = 0;
        } else {
            arm_wipe();
        }
    }
    if (flash_pending) {
        flash_pending = 0;
        if (!flash_value) {
            REG_CGADSUB = 0;
        } else {
            REG_COLDATA = (u8)(0x20 | flash_value);
            REG_COLDATA = (u8)(0x40 | flash_value);
            REG_COLDATA = (u8)(0x80 | flash_value);
            REG_CGADSUB = 0x63; /* BG1, BG2 and backdrop + fixed colour. */
        }
    }
}
