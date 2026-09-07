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
static u8 oam_shadow[128 * 4 + 32];
static u8 obj_n = 0;             /* sprites emitted this frame */
static u8 obj_prev = 0;          /* ...and last frame, so only those are hidden */
static u8 oam_dirty = 1;

/* ── Card tiles ──────────────────────────────────────────────────────────── */

/* What each OBJ card slot HOLDS and what it has been ASKED for.  A slot whose
 * two disagree is queued; snesObjVblank drains the queue a couple of faces at
 * a time, because 512 bytes a card against a vblank that also carries up to
 * 4 KB of board is the whole DMA budget twice over. */
static u8 card_have[SNES_OBJ_CARDS];
static u8 card_want[SNES_OBJ_CARDS];
/* ...and out of WHICH SHEET, which is part of what a slot holds and not a
 * property of the frame: a slot switching between the clustered sheet and the
 * per-face one has to re-upload its tiles and its palette even though the face
 * did not change.  That is what makes walking into the top view and back put
 * the right art up rather than the right art in the wrong colours. */
static u8 card_have_hi[SNES_OBJ_CARDS];
static u8 card_want_hi[SNES_OBJ_CARDS];
static u8 card_hi_mode = 0;

/* WHICH SLOT GOES UP NEXT IS DECIDED OUTSIDE VBLANK, and that is not tidiness.
 *
 * Vblank is thirty-eight lines and every master cycle of it is shared between
 * the bitmap's rows, OAM and this; a twenty-iteration 816-tcc loop comparing
 * two arrays costs more of that window than the 512-byte transfer it is
 * looking for, and the rows it steals from the board simply never reach VRAM
 * -- a black board under a correct framebuffer.  So the scan happens in
 * snesObjEnd, with the rest of the frame's work, and the vblank routine does
 * one comparison and four DMAs. */
static u8 next_card = SNES_OBJ_CARDS;

/* Where slot `k`'s tiles start in the OBJ name table.  Four 32x32 sprites fit
 * across the sixteen-wide table, so a slot is a column of one 64-name group. */
#define CARD_TILE(k)   (u16)(((u16)((k) >> 2) << 6) + (((k) & 3) << 2))
/* ...and where row `r` of those tiles starts in VRAM, as a word address. */
#define CARD_WORD(k, r)  (u16)(0x4000u + ((u16)((k) >> 2) << 10) \
                               + ((u16)(r) << 8) + (((k) & 3) << 6))

#define FONT_TILE      320u
#define CORNER_TILE    (FONT_TILE + SNES_SPR_GLYPH_COUNT)
#define BAR_TILE       (CORNER_TILE + SNES_SPR_CORNER_COUNT)
#define PLATE_TILE     (BAR_TILE + SNES_SPR_BAR_COUNT)
#define ICON_TILE      (PLATE_TILE + SNES_SPR_PLATE_COUNT)
#define FONT_SHEET     (SNES_SPR_GLYPH_COUNT + SNES_SPR_CORNER_COUNT \
                        + SNES_SPR_BAR_COUNT + SNES_SPR_PLATE_COUNT \
                        + SNES_SPR_ICON_COUNT)
#define FONT_WORD      (0x4000u + (FONT_TILE * 32u) / 2u)

/* The life panel, in pixels from its left edge. */
#define LIFE_PLATE_W   24               /* three cells: the label sits on it */
#define LIFE_BAR_X     28
#define LIFE_BAR_W     32               /* four cells of gauge */
#define LIFE_NUM_X     64

#define TOP_TILES_WORD 0x6000u
#define TOP_MAP_WORD   0x7000u

void snesObjInit(void)
{
    u16 i;

    /* CGRAM, once and for ever: the background half for the top view, the OBJ
     * half for the sprites.  Neither view rewrites it. */
    dmaCopyCGram((u8 *)snes_bg_pal, 0, 256);
    dmaCopyCGram((u8 *)snes_spr_pal, 128, 256);

    /* OBSEL: small 8x8 / large 32x32, no name gap, characters at word $4000.
     * The base field counts in 8192-word units, so $4000 is 2. */
    REG_OBSEL = 0x20 | 0x02;

    dmaCopyVram((u8 *)snes_spr_font, FONT_WORD, FONT_SHEET * 32);

    /* The top view, preloaded into the VRAM neither the bitmap nor the sprites
     * use.  This is the whole trick: the Mode 3 picture is already in VRAM
     * before the duel starts, so entering the top view writes three registers
     * and never blanks the screen. */
    dmaCopyVram((u8 *)snes_top_tiles, TOP_TILES_WORD, SNES_TOP_TILE_BYTES);
    dmaCopyVram((u8 *)snes_top_map, TOP_MAP_WORD, 2048);

    for (i = 0; i < SNES_OBJ_CARDS; ++i) {
        card_have[i] = SNES_OBJ_NO_FACE;
        card_want[i] = SNES_OBJ_NO_FACE;
        card_have_hi[i] = 0;
        card_want_hi[i] = 0;
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
}

/* ── Building a frame's list ─────────────────────────────────────────────── */

void snesObjBegin(void)
{
    obj_n = 0;
    card_hi_mode = 0;
}

void snesObjCardHiRes(u8 on)
{
    card_hi_mode = on;
}

void snesObjSprite(s16 x, s16 y, u16 tile, u8 pal, u8 big)
{
    u16 i;
    u8  hi, sh;

    if (obj_n >= 128) return;
    i = (u16)obj_n << 2;
    oam_shadow[i + 0] = (u8)x;
    oam_shadow[i + 1] = (u8)y;
    oam_shadow[i + 2] = (u8)tile;
    /* Priority 3: the HUD is over the board in every mode, and in the top view
     * the cards are over the table. */
    oam_shadow[i + 3] = (u8)(((tile >> 8) & 1) | ((pal & 7) << 1) | 0x30);

    /* The high table: two bits a sprite, the ninth bit of x and the size. */
    sh = (u8)((obj_n & 3) << 1);
    hi = (u8)(((x >> 8) & 1) | (big ? 2 : 0));
    i = (u16)(128 * 4 + (obj_n >> 2));
    oam_shadow[i] = (u8)((oam_shadow[i] & ~(3 << sh)) | (hi << sh));

    ++obj_n;
    oam_dirty = 1;
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

void snesObjBox(s16 x, s16 y, u8 w, u8 h)
{
    snesObjSprite(x, y, CORNER_TILE + 0, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x + (s16)w - 8, y, CORNER_TILE + 1, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x, y + (s16)h - 8, CORNER_TILE + 2, SNES_SPR_HUD_PAL, 0);
    snesObjSprite(x + (s16)w - 8, y + (s16)h - 8, CORNER_TILE + 3,
                  SNES_SPR_HUD_PAL, 0);
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
    /* A slot outside the card palettes cannot own one, so it falls back to the
     * clustered sheet however the caller asked -- there is no arrangement that
     * gives the top view's slot 12 a palette of its own. */
    const u8 hi = (u8)(card_hi_mode && slot < SNES_SPR_CARD_PALS);

    if (slot >= SNES_OBJ_CARDS || face == SNES_OBJ_NO_FACE) return;
    card_want[slot] = face;
    card_want_hi[slot] = hi;
    /* Still on its way up -- and a slot whose SHEET changed is as much on its
     * way up as one whose face did, because its palette is going with it. */
    if (card_have[slot] != face || card_have_hi[slot] != hi) return;
    snesObjSprite(x, y, CARD_TILE(slot),
                  hi ? slot : snes_spr_group[face], 1);
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

    next_card = SNES_OBJ_CARDS;
    for (i = 0; i < SNES_OBJ_CARDS; ++i) {
        if (card_have[i] == card_want[i] &&
            card_have_hi[i] == card_want_hi[i]) continue;
        next_card = i;
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
static void upload_card(u8 slot)
{
    const u8 face = card_want[slot];
    const u8 hi = card_want_hi[slot];
    u16 src;
    u8  r;

    if (face == SNES_OBJ_NO_FACE) {
        card_have[slot] = face;
        card_have_hi[slot] = hi;
        return;
    }
    src = (u16)((u16)face * SNES_SPR_CARD_BYTES);
    if (hi) {
        for (r = 0; r < 4; ++r)
            dmaCopyVram((u8 *)&snes_spr_cards_hi[src + (u16)r * SNES_SPR_CARD_ROW],
                        CARD_WORD(slot, r), SNES_SPR_CARD_ROW);
        dmaCopyCGram((u8 *)&snes_spr_face_pal[(u16)face * 32],
                     (u16)(128 + (u16)slot * 16), 32);
    } else {
        for (r = 0; r < 4; ++r)
            dmaCopyVram((u8 *)&snes_spr_cards[src + (u16)r * SNES_SPR_CARD_ROW],
                        CARD_WORD(slot, r), SNES_SPR_CARD_ROW);
        if (card_have_hi[slot] && slot < SNES_SPR_CARD_PALS)
            dmaCopyCGram((u8 *)&snes_spr_pal[(u16)slot * 32],
                         (u16)(128 + (u16)slot * 16), 32);
    }
    card_have[slot] = face;
    card_have_hi[slot] = hi;
}

void snesObjVblank(void)
{
    if (oam_dirty) {
        dmaCopyOAram(oam_shadow, 0, sizeof(oam_shadow));
        oam_dirty = 0;
    }
    if (next_card < SNES_OBJ_CARDS) {
        upload_card(next_card);
        next_card = SNES_OBJ_CARDS;
    }
}
