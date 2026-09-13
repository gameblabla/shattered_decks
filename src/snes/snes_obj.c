/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_obj.c — the sprite layer.  See snes_obj.h for the VRAM and CGRAM map.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_obj.h"
#include "snes_video.h"

/* ── The OAM shadow ──────────────────────────────────────────────────────── */

/* Its own buffer rather than pvsneslib's oamMemory, and that is deliberate:
 * oamSet ORs its attribute byte into whatever the entry already held, so a
 * sprite id reused with a different palette keeps bits of the old one.  A
 * shadow this file owns is written whole and DMA'd whole. */
u8 snes_oam_shadow[128 * 4 + 32];
u8 snes_obj_n = 0;               /* sprites emitted this frame */
static u8 obj_prev = 0;          /* ...and last frame, so only those are hidden */
u8 snes_oam_dirty = 1;
#define oam_shadow snes_oam_shadow
#define obj_n      snes_obj_n
#define oam_dirty  snes_oam_dirty

/* ── Card tiles ──────────────────────────────────────────────────────────── */

/* What each OBJ card slot HOLDS and what it has been ASKED for.  A slot whose
 * two disagree is queued; snesObjVblank drains one 128-byte card row at a
 * time, because a whole 512-byte face beside OAM can run into active display
 * and leave the final tile word stale. */
static u8 card_have[SNES_OBJ_CARDS];
static u8 card_want[SNES_OBJ_CARDS];
/* ...and out of WHICH SHEET, which is part of what a slot holds and not a
 * property of the frame: a slot switching between the clustered sheet and the
 * per-face one has to re-upload its tiles and its palette even though the face
 * did not change.  That is what makes walking into the top view and back put
 * the right art up rather than the right art in the wrong colours. */
static u8 card_have_hi[SNES_OBJ_CARDS];
static u8 card_want_hi[SNES_OBJ_CARDS];
/* Grey is a palette selection, not a tile sheet.  Keep it separate from the
 * residency key so moving the hand cursor does not make an unchanged card
 * disappear while its four tile rows are uploaded again. */
static u8 card_have_grey[SNES_OBJ_CARDS];
static u8 card_want_grey[SNES_OBJ_CARDS];
static u8 card_hi_mode = 0;

/* WHICH SLOT GOES UP NEXT IS DECIDED OUTSIDE VBLANK, and that is not tidiness.
 *
 * Vblank is thirty-eight lines and every master cycle of it is shared between
 * the bitmap's rows, OAM and this; a twenty-iteration 816-tcc loop comparing
 * two arrays costs more of that window than the 512-byte transfer it is
 * looking for, and the rows it steals from the board simply never reach VRAM
 * -- a black board under a correct framebuffer.  So the scan happens in
 * snesObjEnd, with the rest of the frame's work, and the vblank routine does
 * one comparison and one row DMA. */
static u8 next_card = SNES_OBJ_CARDS;
static u8 next_card_row = 0;
static u8 next_card_face = SNES_OBJ_NO_FACE;
static u8 next_card_hi = 0;
/* Selected by snesObjEnd, so VBlank does no card-array scan. */
static u8 next_palette = SNES_OBJ_CARDS;

/* Where slot `k`'s tiles start in the OBJ name table.  Four 32x32 sprites fit
 * across the sixteen-wide table, so a slot is a column of one 64-name group. */
#define CARD_TILE(k)   (u16)(((u16)((k) >> 2) << 6) + (((k) & 3) << 2))
/* ...and where row `r` of those tiles starts in VRAM, as a word address. */
#define CARD_WORD(k, r)  (u16)(SNES_VRAM_OBJ + ((u16)((k) >> 2) << 10) \
                               + ((u16)(r) << 8) + (((k) & 3) << 6))

#define FONT_TILE      320u
#define CORNER_TILE    (FONT_TILE + SNES_SPR_GLYPH_COUNT)
#define BAR_TILE       (CORNER_TILE + SNES_SPR_CORNER_COUNT)
#define PLATE_TILE     (BAR_TILE + SNES_SPR_BAR_COUNT)
#define ICON_TILE      (PLATE_TILE + SNES_SPR_PLATE_COUNT)
/* The banner's big letters are LAST in the sheet, so their base is every
 * count before them added up -- which is also why the generator asserts the
 * whole sheet against what the twenty card sprites leave of the OBJ region. */
#define BIG_TILE       (ICON_TILE + SNES_SPR_ICON_COUNT)
#define FONT_SHEET     (SNES_SPR_GLYPH_COUNT + SNES_SPR_CORNER_COUNT \
                        + SNES_SPR_BAR_COUNT + SNES_SPR_PLATE_COUNT \
                        + SNES_SPR_ICON_COUNT + SNES_SPR_BIG_TILES)
#define FONT_WORD      (SNES_VRAM_OBJ + (FONT_TILE * 32u) / 2u)

/* The life panel, in pixels from its left edge. */
#define LIFE_PLATE_W   24               /* three cells: the label sits on it */
#define LIFE_BAR_X     28
#define LIFE_BAR_W     32               /* four cells of gauge */
#define LIFE_NUM_X     64

void snesObjInit(void)
{
    u16 i;

    /* CGRAM, once and for ever: the OBJ half for the sprites.  The board is
     * direct colour and reads none of it, so nothing in the duel rewrites it. */
    dmaCopyCGram((u8 *)snes_spr_pal, 128, 256);

    /* OBSEL: small 8x8 / large 32x32, no name gap, characters at word $6000.
     * The base field counts in 8192-word units, so $6000 is 3. */
    REG_OBSEL = 0x20 | (u8)(SNES_VRAM_OBJ >> 13);

    dmaCopyVram((u8 *)snes_spr_font, FONT_WORD, FONT_SHEET * 32);

    for (i = 0; i < SNES_OBJ_CARDS; ++i) {
        card_have[i] = SNES_OBJ_NO_FACE;
        card_want[i] = SNES_OBJ_NO_FACE;
        card_have_hi[i] = 0;
        card_want_hi[i] = 0;
        card_have_grey[i] = 0;
        card_want_grey[i] = 0;
    }
    for (i = 0; i < 128 * 4; i += 4) {
        oam_shadow[i + 0] = 0;
        oam_shadow[i + 1] = 240;        /* off the bottom of the screen */
        oam_shadow[i + 2] = 0;
        oam_shadow[i + 3] = 0;
    }
    for (i = 0; i < 32; ++i) oam_shadow[128 * 4 + i] = 0;
    obj_n = 0;
    obj_prev = 128;                     /* hide everything on the first frame */
    oam_dirty = 1;
    next_card = SNES_OBJ_CARDS;
    next_card_row = 0;
    next_card_face = SNES_OBJ_NO_FACE;
    next_card_hi = 0;
    next_palette = SNES_OBJ_CARDS;
}

/* ── Building a frame's list ─────────────────────────────────────────────── */

void snesObjBegin(void)
{
    obj_n = 0;
    card_hi_mode = 0;
}

/* The two card-sheet modes are BITS of one value, because both of them are
 * part of what a slot HOLDS: a slot that changes either has to re-upload, and
 * snesObjEnd compares the whole value. */
#define CARD_MODE_HI    1
#define CARD_MODE_GREY  2

void snesObjCardHiRes(u8 on)
{
    card_hi_mode = (u8)(on ? (card_hi_mode | CARD_MODE_HI)
                           : (card_hi_mode & ~CARD_MODE_HI));
}

void snesObjCardGrey(u8 on)
{
    card_hi_mode = (u8)(on ? (card_hi_mode | CARD_MODE_GREY)
                           : (card_hi_mode & ~CARD_MODE_GREY));
}

/* The store itself is snes_oam.asm's snesObjSpriteFlip: priority 3 always,
 * the ninth bit of x and the size into the high table. */
void snesObjSprite(s16 x, s16 y, u16 tile, u8 pal, u8 big)
{
    snesObjSpriteFlip(x, y, tile, pal, big, 0);
}

void snesObjText(s16 x, s16 y, const char *s)
{
    u8 c;
    while ((c = (u8)*s++) != 0) {
        if (c >= SNES_SPR_GLYPH_FIRST &&
            c < SNES_SPR_GLYPH_FIRST + SNES_SPR_GLYPH_COUNT && c != ' ')
            snesObjSprite(x, y, (u16)(FONT_TILE + c - SNES_SPR_GLYPH_FIRST),
                          SNES_SPR_HUD_PAL, 0);
        x += 8;
    }
}

void snesObjNum(s16 x, s16 y, u16 value, u8 digits)
{
    /* Right to left, so the field is fixed width and the two sides' numbers
     * line up under each other. */
    s16 px = x + ((s16)digits - 1) * 8;
    u8  i;
    for (i = 0; i < digits; ++i) {
        snesObjSprite(px, y, (u16)(FONT_TILE + '0' - SNES_SPR_GLYPH_FIRST
                                   + (value % 10)), SNES_SPR_HUD_PAL, 0);
        value /= 10;
        px -= 8;
    }
}

void snesObjIcon(s16 x, s16 y, u8 kind)
{
    snesObjSprite(x, y, (u16)(ICON_TILE + kind), SNES_SPR_HUD_PAL, 0);
}

static void obj_box(s16 x, s16 y, u8 w, u8 h, u16 base)
{
    snesObjSprite(x, y, base + 0, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x + (s16)w - 8, y, base + 1, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x, y + (s16)h - 8, base + 2, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x + (s16)w - 8, y + (s16)h - 8, base + 3,
                  SNES_SPR_HUD_PAL, 0);
}

void snesObjBox(s16 x, s16 y, u8 w, u8 h)
{
    obj_box(x, y, w, h, CORNER_TILE + SNES_SPR_CORNER_GOLD);
}

void snesObjBoxRed(s16 x, s16 y, u8 w, u8 h)
{
    obj_box(x, y, w, h, CORNER_TILE + SNES_SPR_CORNER_RED);
}

/* A BIG LETTER IS FOUR SMALL SPRITES, not one 16x16 one: OBSEL gives this
 * screen 8x8 and 32x32, and the 32x32 half is spent on the cards.  Four 8x8
 * sprites cost two slivers a scanline per letter, so a seven-letter banner is
 * fourteen of the thirty-four a line allows -- and it shares no line with the
 * hand or the top table. */
void snesObjBigText(s16 x, s16 y, const char *s, u8 set)
{
    u8 c, g;
    u16 base;
    while ((c = (u8)*s++) != 0) {
        if (c >= SNES_SPR_GLYPH_FIRST &&
            c < SNES_SPR_GLYPH_FIRST + SNES_SPR_GLYPH_COUNT) {
            g = snes_spr_big_index[c - SNES_SPR_GLYPH_FIRST];
            if (g != 0xFF) {
                base = (u16)(BIG_TILE +
                             (((u16)set * SNES_SPR_BIG_GLYPHS + g) << 2));
                snesObjSprite(x,     y,     base + 0, SNES_SPR_HUD_PAL, 0);
                snesObjSprite(x + 8, y,     base + 1, SNES_SPR_HUD_PAL, 0);
                snesObjSprite(x,     y + 8, base + 2, SNES_SPR_HUD_PAL, 0);
                snesObjSprite(x + 8, y + 8, base + 3, SNES_SPR_HUD_PAL, 0);
            }
        }
        x += SNES_OBJ_BIG_PITCH;
    }
}

/* THE LABEL IS EMITTED BEFORE THE PLATE IT SITS ON, and that ordering is the
 * whole of the drawing here: every sprite in this port is priority 3, so what
 * decides which of two overlapping ones is seen is the OAM index, and a lower
 * index wins.  Text first, then the plate under it, then the gauge -- which
 * overlaps nothing and could go anywhere. */
void snesObjLifePanel(s16 x, s16 y, u8 side, u16 lp, u16 lp_max)
{
    const u16 base = (u16)(BAR_TILE + (side ? SNES_SPR_BAR_STEPS : 0));
    u16 px;
    u8  i;

    snesObjText(x + 2, y, side ? "COM" : "YOU");
    snesObjNum(x + LIFE_NUM_X, y, lp, 4);

    for (i = 0; i < LIFE_PLATE_W / 8; ++i)
        snesObjSprite(x + (s16)i * 8, y,
                      (u16)(PLATE_TILE + (side ? 1 : 0)), SNES_SPR_HUD_PAL, 0);

    /* Both sides are scaled down by eight first: the bar is thirty-two pixels
     * and the starting life is eight thousand, so the honest product overflows
     * sixteen bits and the shift costs nothing a byte of accuracy could buy --
     * one pixel of this gauge is two hundred and fifty life points. */
    if (lp_max < 8) lp_max = 8;
    if (lp > lp_max) lp = lp_max;
    px = (u16)(((u16)(lp >> 3) * LIFE_BAR_W) / (u16)(lp_max >> 3));
    for (i = 0; i < LIFE_BAR_W / 8; ++i) {
        u16 w = (px > (u16)i * 8) ? (px - (u16)i * 8) : 0;
        if (w > 8) w = 8;
        snesObjSprite(x + LIFE_BAR_X + (s16)i * 8, y, (u16)(base + w),
                      SNES_SPR_HUD_PAL, 0);
    }
}

void snesObjCard(s16 x, s16 y, u8 slot, u8 face)
{
    snesObjCardFlip(x, y, slot, face, 0);
}

void snesObjQueueCard(u8 slot, u8 face, u8 hi)
{
    if (slot >= SNES_OBJ_CARDS) return;
    card_want[slot] = face;
    card_want_hi[slot] = hi;
}

void snesObjCardFlip(s16 x, s16 y, u8 slot, u8 face, u8 flip)
{
    /* A slot outside the card palettes cannot own one, so it falls back to the
     * clustered sheet however the caller asked -- there is no arrangement that
     * gives the top view's slot 12 a palette of its own. */
    const u8 hi = (u8)((card_hi_mode & CARD_MODE_HI) &&
                       slot < SNES_SPR_CARD_PALS ? CARD_MODE_HI : 0);
    const u8 grey = (u8)((hi && (card_hi_mode & CARD_MODE_GREY)) ? 1 : 0);

    if (slot >= SNES_OBJ_CARDS || face == SNES_OBJ_NO_FACE) return;
    card_want[slot] = face;
    card_want_hi[slot] = hi;
    card_want_grey[slot] = grey;
    /* Still on its way up -- and a slot whose SHEET changed is as much on its
     * way up as one whose face did, because its palette is going with it. */
    if (card_have[slot] != face || card_have_hi[slot] != hi) return;
    if (obj_n >= 128) return;
    snesObjSprite(x, y, CARD_TILE(slot),
                  (hi & CARD_MODE_HI) ? slot : snes_spr_group[face], 1);
    oam_shadow[((u16)(obj_n - 1) << 2) + 3] |= flip & 0xC0;
}

void snesObjEnd(void)
{
    u8 i;
    for (i = obj_n; i < obj_prev; ++i) {
        oam_shadow[((u16)i << 2) + 1] = 240;
        oam_shadow[128 * 4 + (i >> 2)] &= (u8)~(3 << ((i & 3) << 1));
    }
    obj_prev = obj_n;
    oam_dirty = 1;

    /* Keep a partially uploaded face ahead of later slots.  If the editor
     * changes the requested face while its rows are in flight, restart that
     * slot from row zero so no rows from two faces can be mixed. */
    if (next_card < SNES_OBJ_CARDS &&
        (card_have[next_card] != card_want[next_card] ||
         card_have_hi[next_card] != card_want_hi[next_card])) {
        if (next_card_face != card_want[next_card] ||
            next_card_hi != card_want_hi[next_card]) {
            next_card_face = card_want[next_card];
            next_card_hi = card_want_hi[next_card];
            next_card_row = 0;
        }
        return;
    }

    next_card = SNES_OBJ_CARDS;
    for (i = 0; i < SNES_OBJ_CARDS; ++i) {
        if (card_have[i] == card_want[i] &&
            card_have_hi[i] == card_want_hi[i]) continue;
        next_card = i;
        next_card_row = 0;
        next_card_face = card_want[i];
        next_card_hi = card_want_hi[i];
        break;
    }

    /* Grey is a palette-only transition.  Pick one resident hand slot here,
     * outside VBlank, and let the VBlank pump apply only that queued 32-byte
     * CGRAM update.  Scanning all slots during VBlank steals time from the
     * framebuffer DMA and can leave its final rows stale. */
    next_palette = SNES_OBJ_CARDS;
    for (i = 0; i < SNES_SPR_CARD_PALS && i < SNES_OBJ_CARDS; ++i) {
        if (card_have[i] == SNES_OBJ_NO_FACE || !card_have_hi[i] ||
            card_have[i] != card_want[i] ||
            card_have_grey[i] == card_want_grey[i]) continue;
        next_palette = i;
        break;
    }
}

u8 snesObjCardsReady(void)
{
    u8 i;
    for (i = 0; i < SNES_OBJ_CARDS; ++i)
        if (card_have[i] != card_want[i] ||
            card_have_hi[i] != card_want_hi[i]) return 0;
    return 1;
}

/* ── The vblank pump ─────────────────────────────────────────────────────── */

/* A slot's tiles, and -- when they came out of the per-face sheet -- the
 * fifteen colours they were cut against.
 *
 * THE PALETTE GOES UP WITH THE TILES AND IN THE SAME VBLANK, because the two
 * are one picture: land the tiles a field before their palette and the card is
 * on screen for that field drawn through the previous face's colours, which is
 * a flash of confetti exactly where the player is looking.  Thirty-two bytes
 * of CGRAM next to five hundred and twelve of VRAM is not a budget question.
 *
 * Going the other way -- a slot leaving the per-face sheet, which is what
 * walking up into the top view does to all five hand slots -- the clustered
 * palette has to be put BACK, or the top view draws its twenty cards through
 * five hand cards' palettes. */
static void upload_card_row(u8 slot, u8 row)
{
    const u8 face = card_want[slot];
    const u8 hi = card_want_hi[slot];
    const u16 src = (u16)((u16)face * SNES_SPR_CARD_BYTES);

    if (face == SNES_OBJ_NO_FACE) {
        /* An empty top slot still releases its hand palette: another field
         * card can use that clustered palette even though this slot is empty. */
        if (card_have_hi[slot] && !hi && slot < SNES_SPR_CARD_PALS)
            dmaCopyCGram((u8 *)&snes_spr_pal[(u16)slot * 32],
                         (u16)(128 + (u16)slot * 16), 32);
        card_have[slot] = face;
        card_have_hi[slot] = hi;
        card_have_grey[slot] = 0;
        return;
    }
    if (hi & CARD_MODE_HI) {
        dmaCopyVram((u8 *)&snes_spr_cards_hi[src + (u16)row * SNES_SPR_CARD_ROW],
                    CARD_WORD(slot, row), SNES_SPR_CARD_ROW);
    } else {
        dmaCopyVram((u8 *)&snes_spr_cards[src + (u16)row * SNES_SPR_CARD_ROW],
                    CARD_WORD(slot, row), SNES_SPR_CARD_ROW);
    }
    if (row == 3) {
        if (hi & CARD_MODE_HI) {
            /* The grey sheet is the SAME tiles through a greyed, darkened copy
             * of this face's own fifteen colours, so a card dimming or lighting
             * up is thirty-two bytes of CGRAM. */
            const u8 *pal = card_want_grey[slot] ? snes_spr_face_pal_grey
                                                  : snes_spr_face_pal;
            dmaCopyCGram((u8 *)&pal[(u16)face * 32],
                         (u16)(128 + (u16)slot * 16), 32);
        } else if (card_have_hi[slot] && slot < SNES_SPR_CARD_PALS) {
            dmaCopyCGram((u8 *)&snes_spr_pal[(u16)slot * 32],
                         (u16)(128 + (u16)slot * 16), 32);
        }
        card_have[slot] = face;
        card_have_hi[slot] = hi;
        card_have_grey[slot] = (u8)(hi ? card_want_grey[slot] : 0);
    }
}

/* What the next snesObjVblank will put on the bus, in bytes, so the board's
 * NMI drain can leave that much of the vblank alone (snesFbReserve).  The
 * two used to share the window by guesswork, and the guess lost: a card
 * row landing after the window closed is the selected hand card with its
 * last tile row stale. */
u16 snesObjVblankBytes(void)
{
    u16 bytes = oam_dirty ? 544 : 0;
    if (next_card < SNES_OBJ_CARDS)
        bytes += (u16)(4 - next_card_row) * SNES_SPR_CARD_ROW + 32;
    if (next_palette < SNES_OBJ_CARDS) bytes += 32;
    return bytes;
}

void snesObjVblank(void)
{
    u8 budget = 4;
    u8 palette_done = 0;
    if (oam_dirty) {
        dmaCopyOAram(oam_shadow, 0, sizeof(oam_shadow));
        oam_dirty = 0;
    }
    while (budget-- && next_card < SNES_OBJ_CARDS) {
        if (next_card_row == 3 &&
            (card_want_hi[next_card] & CARD_MODE_HI))
            palette_done = 1;
        upload_card_row(next_card, next_card_row);
        if (next_card_row == 3) {
            next_card = SNES_OBJ_CARDS;
            next_card_row = 0;
        } else {
            ++next_card_row;
        }
    }

    /* Apply at most one queued grey palette per VBlank.  The validity checks
     * cover a slot that was reused between snesObjEnd and this VBlank. */
    if (!palette_done && next_palette < SNES_OBJ_CARDS &&
        card_have[next_palette] != SNES_OBJ_NO_FACE &&
        card_have_hi[next_palette] &&
        card_have[next_palette] == card_want[next_palette] &&
        card_have_grey[next_palette] != card_want_grey[next_palette]) {
        const u8 *pal = card_want_grey[next_palette]
                      ? snes_spr_face_pal_grey : snes_spr_face_pal;
        dmaCopyCGram((u8 *)&pal[(u16)card_have[next_palette] * 32],
                     (u16)(128 + (u16)next_palette * 16), 32);
        card_have_grey[next_palette] = card_want_grey[next_palette];
    }
    next_palette = SNES_OBJ_CARDS;
}
