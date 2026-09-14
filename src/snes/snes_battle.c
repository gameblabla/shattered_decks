/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_battle.c — the Mode 4 battle and direct-attack presentation.
 *  See snes_battle.h for the layout and the reasons.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_battle.h"
#include "snes_battle_data.h"
#include "snes_cardart.h"
#include "snes_bigcard_data.h"
#include "snes_cards.h"
#include "snes_board3d.h"
#include "snes_obj.h"
#include "snes_obj_data.h"
#include "snes_audio.h"
#include "snes_math.h"
#include "msx2_duel.h"
#include "msx2_cards.h"

/* ── VRAM ────────────────────────────────────────────────────────────────── */
#define FX_OBJ_WORD     0x4000u
#define BG1_MAP_WORD    0x6000u         /* 32x64 */
#define BG2_MAP_WORD    0x6800u
#define BG3_MAP_WORD    0x6C00u
#define FONT_WORD       0x7000u
#define FONT_FIRST      32u
#define FONT_COUNT      64u

/* The lanes: the player's card in columns 1..15 (x 8..127), the opponent's
 * in 17..31 (x 136..255).  Column 0 is the gutter offset-per-tile cannot
 * reach; column 16 is the gap between them. */
#define LANE_COL(l)     ((l) ? 17 : 1)
#define LANE_X(l)       ((l) ? 136 : 8)
#define LANE_W          120
#define CARD_H          160
#define CARD_ROWS       20
#define REST_Y          22                  /* the card check's card line */
#define ABOVE_Y         (-CARD_H)
#define BELOW_Y         224
/* BG2's rows under the cards: the figures, and the life points. */
#define TEXT_ROW        23
#define LP_ROW          25

/* Mode 4 offset entry: vertical, applied to BG1 only.  Screen line L shows
 * map line L + off + 1 (the PPU's scroll latch), so a card whose top is at
 * screen y sits at map line 0 when off = -(y + 1). */
#define OFFSET_ENTRY(y) ((u16)(0xA000u | ((u16)(0x3FF - (s16)(y)) & 0x3FFu)))

/* ── Timelines, in displayed fields ──────────────────────────────────────── */
/* The two-card battle. */
#define B_ENTER_END     20
#define B_HOLD_END      28
#define B_STRIKE_BACK   34                  /* the attacker pulls back... */
#define B_STRIKE_HIT    37                  /* ...and lands */
#define B_STRIKE_END    44
#define B_RESULT_END    68
#define B_EXIT_END      84
#define B_SKIP_FROM     44

/* The direct attack: the PC's 20-field entry, 32-field lunge with contact
 * at 62% of it, and a 72-field FX beat opening 115/1000 of itself before
 * contact -- src/main.c's WAIFU_DIRECT_* constants, spelled out. */
#define D_ENTER_END     20
#define D_LUNGE_FRAMES  32
#define D_CONTACT       (D_ENTER_END + (D_LUNGE_FRAMES * 62) / 100)     /* 39 */
#define D_FX_FLASH      ((SNES_FX_FIELDS * 115) / 1000)                 /* 8 */
#define D_FX_START      (D_CONTACT - D_FX_FLASH)                        /* 31 */
#define D_FX_TEXT       28                  /* the readout, inside the beat */
#define D_LUNGE_END     (D_ENTER_END + D_LUNGE_FRAMES)                  /* 52 */
#define D_FX_END        (D_FX_START + SNES_FX_FIELDS)                   /* 103 */
#define D_EXIT_END      (D_FX_END + 16)                                 /* 119 */
#define D_SKIP_FROM     (D_FX_START + D_FX_TEXT + 12)

/* THE VERDICT IS GIVEN HERE, NOT ON THE BOARD.  A blow that ends the duel
 * keeps its cards on the screen (the loser's dark, the winner's whole) and
 * the banner the board would have shown slides in over them instead; the
 * duel never goes back to the table.  The letters are the HUD sheet's big
 * glyphs, copied in after the effects atlas (which the digits and the burst
 * already fill to tile BIG_OBJ_TILE), drawn in OBJ palette 7 -- the
 * digits' palette during the beat, so the HUD's own is put back for them. */
#define BIG_OBJ_TILE    (SNES_FX_TILE_BYTES / 32)
#define BIG_SHEET_OFF   ((SNES_SPR_GLYPH_COUNT + SNES_SPR_CORNER_COUNT + \
                          SNES_SPR_BAR_COUNT + SNES_SPR_PLATE_COUNT + \
                          SNES_SPR_ICON_COUNT) * 32u)
#define BIG_OBJ_WORD    (FX_OBJ_WORD + BIG_OBJ_TILE * 16u)
#define VERDICT_Y       94                  /* across the middle of the cards */
#define VERDICT_SLIDE   30
#define VERDICT_HOLD    70

enum BattlePhase {
    PHASE_ENTER = 0, PHASE_HOLD, PHASE_STRIKE, PHASE_RESULT, PHASE_EXIT,
    PHASE_DONE, PHASE_LUNGE, PHASE_FX, PHASE_VERDICT
};

/* ── The snapshot ────────────────────────────────────────────────────────── */
/* Copied out of the rules the moment the battle begins; the visual sequence
 * neither reads the rules again nor applies any damage itself. */
static u8  direct = 0;
static u8  blocked = 0;                 /* a trap cancelled the attack */
static u8  attacker_owner = 0;          /* 0 the player, 1 the COM */
static u8  attacker_lane = 0;
static u8  lane_face[2];                /* per lane; NONE for an empty lane */
static u16 lane_atk[2], lane_def[2];
static u8  outcome = 0;
static u16 damage = 0;
static s8  damage_owner = -1;
static u16 lp_before = 0, lp_after = 0;
static u8  destroy_lane[2];

/* ── The sequence ────────────────────────────────────────────────────────── */
static u8  phase = PHASE_DONE;
static u16 field = 0;                   /* displayed fields since the sequence began */
static u16 field0 = 0;                  /* snes_vblank_count at the first step */
static u16 field_skip = 0;              /* fields the skip took off the counter */
static s16 lane_y[2];
static u8  cue_blade = 0, cue_hit = 0, cue_burn = 0;
static u16 offset_row[32];
static u8  offset_dirty = 0;
static u16 bg2_map[1024];
static u8  bg2_dirty = 0;               /* whole map */
static u8  bg2_row_dirty[32];           /* ...or just these rows */
static u8  cool = 0, cool_shown = 0xFF;
/* Colour math, decided by the sequencer and written in vblank. */
static u8  cm_mode = 0;                 /* 0 none, 1 whiteout, 2 wash, 3 lane fade */
static u8  cm_level = 0;                /* the fixed colour's intensity */
static u8  cm_lane = 0;
static u8  cm_dirty = 0;
static u16 lp_shown = 0;
static u8  lp_lane = 0;
static u8  lp_visible = 0;
/* The duel was decided by this blow (snapshot): the sequence ends on the
 * verdict instead of the exit, and verdict_step counts its fields. */
static u8  decided = 0;
static u16 verdict_step = 0;
static u8  pal_restore = 0;

u8  snesBattlePhase(void)   { return phase; }
u16 snesBattleField(void)   { return field; }
u8  snesBattleIsDirect(void) { return direct; }
u16 snesBattleDamage(void)   { return damage; }

/* ── Text on BG2 ─────────────────────────────────────────────────────────── */

#define BLANK_CELL      0u                  /* the space glyph is tile 0 */
#define TEXT_PRIORITY   0x2000u
#define PAL_WHITE       0                   /* CGRAM 1/2: white on black... */
#define PAL_GOLD        4                   /* ...CGRAM 17/18: gold on black */

static void put_char(u8 col, u8 row, u8 c, u8 pal)
{
    if (col >= 32 || row >= 32) return;
    if (c == ' ' || c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT) {
        bg2_map[(u16)row * 32 + col] = BLANK_CELL;
        bg2_row_dirty[row] = 1;
        return;
    }
    bg2_map[(u16)row * 32 + col] =
        (u16)(TEXT_PRIORITY | ((u16)(pal & 7) << 10) | (c - FONT_FIRST));
    bg2_row_dirty[row] = 1;
}

static void put_text(u8 col, u8 row, const char *s, u8 pal)
{
    while (*s && col < 32) put_char(col++, row, (u8)*s++, pal);
}

/* Decimal digits by subtraction: 816-tcc's division is a thousand cycles a
 * digit, and the readout and the life points print every field. */
static void decimal(u16 value, u8 *out)
{
    static const u16 pow[4] = { 1000, 100, 10, 1 };
    u8 i;
    for (i = 0; i < 4; ++i) {
        u8 d = 0;
        while (value >= pow[i]) { value = (u16)(value - pow[i]); ++d; }
        out[i] = d;
    }
}

static void put_num(u8 col, u8 row, u16 value, u8 digits, u8 pal)
{
    u8 d[4];
    u8 i;
    if (value > 9999) value = 9999;
    decimal(value, d);
    for (i = 0; i < digits && i < 4; ++i)
        put_char((u8)(col + i), row, (u8)('0' + d[4 - digits + i]), pal);
}

static void clear_text(void)
{
    u16 i;
    for (i = 0; i < 1024; ++i) bg2_map[i] = BLANK_CELL;
    bg2_dirty = 1;
}

static void clear_row(u8 row)
{
    u8 i;
    for (i = 0; i < 32; ++i) bg2_map[(u16)row * 32 + i] = BLANK_CELL;
    bg2_row_dirty[row] = 1;
}

/* The ATK/DEF figures under a lane's card, the PC-FX way: a modified figure
 * in gold, the printed one in white. */
static void lane_stats(u8 lane)
{
    const u8 col = LANE_COL(lane);
    const u8 face = lane_face[lane];
    u8 pal_a = PAL_WHITE, pal_d = PAL_WHITE;
    if (face >= MSX2_TOTAL_CARDS || !Msx2_IsMonster(face)) return;
    if (lane_atk[lane] != Msx2_CardAtk(face)) pal_a = PAL_GOLD;
    if (lane_def[lane] != Msx2_CardDef(face)) pal_d = PAL_GOLD;
    put_text((u8)(col + 1), TEXT_ROW, "A", pal_a);
    put_num((u8)(col + 2), TEXT_ROW, lane_atk[lane], 4, pal_a);
    put_text((u8)(col + 7), TEXT_ROW, "D", pal_d);
    put_num((u8)(col + 8), TEXT_ROW, lane_def[lane], 4, pal_d);
}

static void lane_lp(u8 lane, u16 lp)
{
    const u8 col = LANE_COL(lane);
    put_text((u8)(col + 3), LP_ROW, "LP", PAL_GOLD);
    put_num((u8)(col + 6), LP_ROW, lp, 4, PAL_WHITE);
}

/* ── Entry ───────────────────────────────────────────────────────────────── */

static u8 face_of(u8 card)
{
    if (card == MSX2_CARD_NONE) return SNES_CARD_NONE_FACE;
    if (card >= SNES_CARD_BACK) return SNES_CARD_BACK;
    return card;
}

static void snapshot(void)
{
    const Msx2BattleCalc *bc = &g_duel.last_battle;
    u8 lane, other;
    attacker_owner = g_duel.last_attacker_owner ? 1 : 0;
    attacker_lane = attacker_owner;             /* the player's lane is the left */
    other = (u8)(attacker_lane ^ 1);
    /* A trap that fired cancelled the attack before any battle was
     * calculated: the attacker alone, repelled, no damage and no readout.
     * The rules' last_battle is stale in that case and is not read. */
    blocked = (u8)(g_duel.last_trap_fired != 0);
    decided = (u8)(g_duel.result != 0);
    direct = (u8)(blocked || bc->outcome == MSX2_BATTLE_DIRECT ||
                  g_duel.last_defender_card == MSX2_CARD_NONE);
    lane_face[attacker_lane] = face_of(g_duel.last_attacker_card);
    lane_face[other] = direct ? SNES_CARD_NONE_FACE
                              : face_of(g_duel.last_defender_card);
    if (blocked) {
        lane_atk[0] = lane_atk[1] = lane_def[0] = lane_def[1] = 0;
        outcome = MSX2_BATTLE_NONE;
        damage = 0;
        damage_owner = -1;
        destroy_lane[0] = destroy_lane[1] = 0;
        lp_before = lp_after = 0;
        lp_lane = other;
        return;
    }
    /* The figures the battle was fought with: the attacker's ATK as the
     * rules used it, the defender's ATK or DEF by its stance. */
    lane_atk[attacker_lane] = (u16)bc->attacker_atk;
    lane_def[attacker_lane] = (lane_face[attacker_lane] < SNES_CARD_BACK)
                            ? Msx2_CardDef(lane_face[attacker_lane]) : 0;
    if (bc->defender_passive) {
        lane_atk[other] = (lane_face[other] < SNES_CARD_BACK)
                        ? Msx2_CardAtk(lane_face[other]) : 0;
        lane_def[other] = (u16)bc->defender_value;
    } else {
        lane_atk[other] = (u16)bc->defender_value;
        lane_def[other] = (lane_face[other] < SNES_CARD_BACK)
                        ? Msx2_CardDef(lane_face[other]) : 0;
    }
    outcome = bc->outcome;
    damage = (bc->damage > 0) ? (u16)bc->damage : 0;
    damage_owner = bc->damage_owner;
    destroy_lane[0] = destroy_lane[1] = 0;
    if (outcome == MSX2_BATTLE_DESTROY_DEFENDER || outcome == MSX2_BATTLE_DESTROY_BOTH)
        destroy_lane[other] = 1;
    if (outcome == MSX2_BATTLE_DESTROY_ATTACKER || outcome == MSX2_BATTLE_DESTROY_BOTH)
        destroy_lane[attacker_lane] = 1;
    /* The rules have already taken the damage: count down to where they
     * left the life points, from where they were. */
    if (damage_owner >= 0 && damage) {
        lp_after = (u16)g_duel.side[damage_owner].lp;
        lp_before = (u16)(lp_after + damage);
        lp_lane = (u8)(damage_owner ? 1 : 0);
    } else {
        lp_before = lp_after = 0;
        lp_lane = other;
    }
    for (lane = 0; lane < 2; ++lane) {
        if (lane_face[lane] >= SNES_CARD_FACES) lane_face[lane] = SNES_CARD_NONE_FACE;
    }
}

static void write_offsets(void)
{
    u8 i;
    const u16 p = OFFSET_ENTRY(lane_y[0]);
    const u16 c = OFFSET_ENTRY(lane_y[1]);
    /* Entry e drives visible column e + 1: the player's columns 1..15 are
     * entries 0..14, the opponent's 17..31 are 16..30.  The rest -- the
     * gutter and the gap -- are blank columns and take no offset. */
    for (i = 0; i < 15; ++i) offset_row[i] = p;
    offset_row[15] = 0;
    for (i = 16; i < 31; ++i) offset_row[i] = c;
    offset_row[31] = 0;
    offset_dirty = 1;
}

static void set_colour_math(u8 mode, u8 level, u8 lane)
{
    if (mode == cm_mode && level == cm_level && lane == cm_lane) return;
    cm_mode = mode;
    cm_level = level;
    cm_lane = lane;
    cm_dirty = 1;
}

void snesBattleBegin(void)
{
    u16 row_cells[32];
    u8 ty, tx, lane;

    snapshot();
    setScreenOff();
    /* The sprite layer's reset FIRST: it reloads the duel's OBJ palettes
     * over CGRAM 128..255 and the HUD font, and everything below writes
     * over that. */
    snesObjInit();
    REG_HDMAEN = 0;
    REG_CGWSEL = 0;
    REG_CGADSUB = 0;
    REG_TMW = 0;
    REG_W12SEL = 0;
    REG_WOBJSEL = 0;
    REG_TS = 0;

    /* Mode 4: 8bpp BG1 in CGRAM colour (direct colour off), 2bpp BG2, BG3
     * as the offset store. */
    setMode(BG_MODE4, 0);
    REG_BG1SC = (u8)(((BG1_MAP_WORD >> 10) << 2) | 0x02);   /* $62: 32x64 */
    REG_BG2SC = (u8)((BG2_MAP_WORD >> 10) << 2);            /* $68 */
    REG_BG3SC = (u8)((BG3_MAP_WORD >> 10) << 2);            /* $6C */
    REG_BG12NBA = 0x70;                                     /* BG1 $0000, BG2 $7000 */
    REG_BG34NBA = 0x00;
    REG_BG1HOFS = 0; REG_BG1HOFS = 0;
    REG_BG1VOFS = 0; REG_BG1VOFS = 0;
    REG_BG2HOFS = 0; REG_BG2HOFS = 0;
    REG_BG2VOFS = 0xFF; REG_BG2VOFS = 0x03;                 /* map row r on line 8r */
    REG_BG3HOFS = 0; REG_BG3HOFS = 0;
    REG_BG3VOFS = 0; REG_BG3VOFS = 0;

    /* The effects atlas and its two palettes; OBJ names from $4000. */
    dmaCopyVram((u8 *)snes_fx_tiles, FX_OBJ_WORD, SNES_FX_TILE_BYTES);
    dmaCopyCGram((u8 *)snes_fx_pal, (u16)(128 + SNES_FX_BURST_PAL * 16), 32);
    dmaCopyCGram((u8 *)&snes_fx_pal[32], (u16)(128 + SNES_FX_DIGIT_PAL * 16), 32);
    REG_OBSEL = (u8)(0x20 | (FX_OBJ_WORD >> 13));
    dmaCopyVram((u8 *)snes_battle_font, FONT_WORD, SNES_FX_FONT_BYTES);
    if (decided)
        dmaCopyVram((u8 *)&snes_spr_font[BIG_SHEET_OFF], BIG_OBJ_WORD,
                    SNES_SPR_BIG_TILES * 32);

    /* The card sheets, feet, frame palettes and the blank tile, as the card
     * check loads them. */
    snesCardArtLoadCommon();
    for (lane = 0; lane < 2; ++lane)
        if (lane_face[lane] != SNES_CARD_NONE_FACE)
            snesCardArtLoad(lane, lane_face[lane]);

    /* BG1's 64-row map: each lane's card at map rows 0..19 in its own
     * columns, blank everywhere else. */
    for (ty = 0; ty < 64; ++ty) {
        for (tx = 0; tx < 32; ++tx) row_cells[tx] = SNES_CARDART_BLANK_TILE;
        if (ty < CARD_ROWS) {
            for (lane = 0; lane < 2; ++lane)
                if (lane_face[lane] != SNES_CARD_NONE_FACE)
                    snesCardArtRowCells(lane, lane_face[lane], ty,
                                        &row_cells[LANE_COL(lane)]);
        }
        dmaCopyVram((u8 *)row_cells, (u16)(BG1_MAP_WORD + ty * 32), 64);
    }

    clear_text();
    dmaCopyVram((u8 *)bg2_map, BG2_MAP_WORD, 2048);
    bg2_dirty = 0;
    for (ty = 0; ty < 32; ++ty) bg2_row_dirty[ty] = 0;

    lane_y[0] = ABOVE_Y;
    lane_y[1] = BELOW_Y;
    write_offsets();
    /* Both rows 0 and 1 of the BG3 map, whichever the PPU's latch reads. */
    dmaCopyVram((u8 *)offset_row, BG3_MAP_WORD, 64);
    dmaCopyVram((u8 *)offset_row, (u16)(BG3_MAP_WORD + 32), 64);
    offset_dirty = 0;

    phase = PHASE_ENTER;
    field = 0;
    field0 = (u16)(snes_vblank_count + 1);
    field_skip = 0;
    cue_blade = cue_hit = cue_burn = 0;
    cool = 0;
    cool_shown = 0xFF;
    cm_mode = cm_level = cm_lane = 0;
    cm_dirty = 1;
    lp_visible = 0;
    lp_shown = lp_before;
    verdict_step = 0;
    pal_restore = 0;

    REG_OBSEL = (u8)(0x20 | (FX_OBJ_WORD >> 13));
    REG_TM = BG1_ENABLE | BG2_ENABLE | OBJ_ENABLE;
}

/* ── Motion helpers ──────────────────────────────────────────────────────── */

/* Q8.8 smoothstep of f/n. */
static u16 ease(u16 f, u16 n)
{
    u16 t, t2;
    if (f >= n) return SNES_ONE;
    t = snesUQDiv(f, n);
    t2 = (u16)snesQMul((s16)t, (s16)t);
    return (u16)snesQMul((s16)t2, (s16)(3 * SNES_ONE - 2 * t));
}

static s16 lerp(s16 a, s16 b, u16 t)
{
    return (s16)(a + snesQMul((s16)(b - a), (s16)t));
}

/* A lane's "forward": the direction it entered in. */
#define FORWARD(lane)   ((lane) ? -1 : 1)
#define ENTRY_Y(lane)   ((lane) ? BELOW_Y : ABOVE_Y)

static void lane_enter(u8 lane, u16 f, u16 n)
{
    lane_y[lane] = lerp((s16)ENTRY_Y(lane), REST_Y, ease(f, n));
}

static void lane_leave(u8 lane, u16 f, u16 n)
{
    lane_y[lane] = lerp(REST_Y, (s16)ENTRY_Y(lane), ease(f, n));
}

/* ── Sprites ─────────────────────────────────────────────────────────────── */

/* No clipping here: OAM's ninth x bit and its 240-line y range hide a
 * sprite that has left the screen, and the asm store is cheaper than the
 * comparisons 816-tcc would spend on avoiding it. */
#define sprite32(x, y, tile, pal, flip) snesObjSpriteFlip((x), (y), (tile), (pal), 1, (flip))
#define sprite8(x, y, tile, pal)        snesObjSpriteFlip((x), (y), (tile), (pal), 0, 0)

/* A digit is a 2x2 block of 8x8 sprites out of the atlas. */
static void digit16(s16 x, s16 y, u8 d, u8 pal)
{
    u16 base = (d < 8) ? (u16)(SNES_FX_DIGIT0 + d * 2)
                       : (u16)(SNES_FX_DIGIT_ROW2 + (d - 8) * 2);
    sprite8(x, y, base, pal);
    sprite8(x + 8, y, base + 1, pal);
    sprite8(x, y + 8, base + 16, pal);
    sprite8(x + 8, y + 8, base + 17, pal);
}

static void damage_readout(s16 cx, s16 cy, u16 value, u8 pal, s16 dy)
{
    u8 d[4];
    u8 first = 3, i, n;
    s16 x;
    if (value > 9999) value = 9999;
    decimal(value, d);
    /* Leading zeros off: the first non-zero digit, or the last digit. */
    first = 0;
    while (first < 3 && d[first] == 0) ++first;
    n = (u8)(4 - first);
    x = (s16)(cx - (s16)(n * 8));
    for (i = 0; i < n; ++i)
        digit16((s16)(x + i * 16), (s16)(cy - 8 + dy), d[first + i], pal);
}

/* Eight points on a circle at the eighth-turns, from one product: the
 * diagonal ones are r * cos 45.  The order is the atlas's: east, north-east,
 * north, north-west, then their opposites. */
static void octagon(s16 cx, s16 cy, s16 r, s16 *xs, s16 *ys)
{
    const s16 d = snesQMul(r, 181);
    xs[0] = (s16)(cx + r);  ys[0] = cy;
    xs[1] = (s16)(cx + d);  ys[1] = (s16)(cy - d);
    xs[2] = cx;             ys[2] = (s16)(cy - r);
    xs[3] = (s16)(cx - d);  ys[3] = (s16)(cy - d);
    xs[4] = (s16)(cx - r);  ys[4] = cy;
    xs[5] = (s16)(cx - d);  ys[5] = (s16)(cy + d);
    xs[6] = cx;             ys[6] = (s16)(cy + r);
    xs[7] = (s16)(cx + d);  ys[7] = (s16)(cy + d);
}

/* The shock ring: eight arcs, the medium set while it is small enough to
 * curve like them, the thin set past that.  Orientation k's piece is the
 * atlas's k for 0..3 and the flipped opposite beyond. */
static const u16 arc_tile[8] = { SNES_FX_ARC0, SNES_FX_ARC1, SNES_FX_ARC2, SNES_FX_ARC3,
                                 SNES_FX_ARC0, SNES_FX_ARC1, SNES_FX_ARC2, SNES_FX_ARC3 };
static const u8  arc_flip[8] = { 0, 0, 0, 0, 0xC0, 0xC0, 0xC0, 0xC0 };
static const u16 thin_tile[8] = { SNES_FX_THIN0, SNES_FX_THIN1, SNES_FX_THIN2, SNES_FX_THIN1,
                                  SNES_FX_THIN0, SNES_FX_THIN1, SNES_FX_THIN2, SNES_FX_THIN1 };
static const u8  thin_flip[8] = { 0, 0, 0, 0x40, 0x40, 0xC0, 0x80, 0x80 };
static const u16 ray_tile[8] = { SNES_FX_RAY0, SNES_FX_RAY1, SNES_FX_RAY2, SNES_FX_RAY3,
                                 SNES_FX_RAY0, SNES_FX_RAY1, SNES_FX_RAY2, SNES_FX_RAY3 };

static void ring_sprites(s16 cx, s16 cy, s16 r)
{
    u8 k;
    const u16 *tiles = (r >= 64) ? thin_tile : arc_tile;
    const u8 *flips = (r >= 64) ? thin_flip : arc_flip;
    s16 xs[8], ys[8];
    octagon((s16)(cx - 16), (s16)(cy - 16), r, xs, ys);
    for (k = 0; k < 8; ++k)
        sprite32(xs[k], ys[k], tiles[k], SNES_FX_BURST_PAL, flips[k]);
}

static void ray_sprites(s16 cx, s16 cy, s16 d)
{
    u8 k;
    s16 xs[8], ys[8];
    octagon((s16)(cx - 16), (s16)(cy - 16), d, xs, ys);
    for (k = 0; k < 8; ++k)
        sprite32(xs[k], ys[k], ray_tile[k], SNES_FX_BURST_PAL, arc_flip[k]);
}

static void core_sprite(s16 cx, s16 cy, u8 r)
{
    u16 tile;
    if (!r) return;
    tile = (r < 9) ? SNES_FX_CORE_S : (r < 14) ? SNES_FX_CORE_M : SNES_FX_CORE_L;
    sprite32((s16)(cx - 16), (s16)(cy - 16), tile, SNES_FX_BURST_PAL, 0);
}

static void spark_sprites(s16 cx, s16 cy, u16 t)
{
    u8 i;
    const u16 tt = snesMulLo(t, t);
    for (i = 0; i < SNES_FX_SPARKS; ++i) {
        /* Q12.4: position = v * t, plus gravity t*t/16.  The products are
         * small and the PPU multiplier's low word is exact for them, sign
         * and all; 816-tcc's own multiply is a 300-cycle loop. */
        s16 x = (s16)(cx + ((s16)snesMulLo(snes_fx_spark_vx[i], t) >> 4));
        s16 y = (s16)(cy + ((s16)snesMulLo(snes_fx_spark_vy[i], t) >> 4) + (s16)(tt >> 4));
        u16 tile;
        switch (snes_fx_spark_kind[i]) {
        case 0: tile = SNES_FX_SPARK0; break;
        case 1: tile = SNES_FX_SPARK1; break;
        case 2: tile = SNES_FX_SPARK2; break;
        default: tile = SNES_FX_SPARK3; break;
        }
        sprite8((s16)(x - 4), (s16)(y - 4), tile, SNES_FX_BURST_PAL);
    }
}

/* ── The direct attack's beat ────────────────────────────────────────────── */

/* Where the blow lands: the empty lane's middle, at the card's height. */
static void impact_point(s16 *cx, s16 *cy)
{
    const u8 target = (u8)(attacker_lane ^ 1);
    *cx = (s16)(LANE_X(target) + LANE_W / 2);
    *cy = (s16)(REST_Y + 78);
}

static void direct_fx(u16 t)
{
    s16 cx, cy;
    impact_point(&cx, &cy);

    /* 1. The blade (0..13): a bright vertical sweep from the attacker's
     * edge to the impact point, the head first and the tail behind it. */
    if (t < 14) {
        const s16 from = (s16)(attacker_lane ? LANE_X(1) - 16 : LANE_X(0) + LANE_W - 16);
        const s16 to = (s16)(cx - 16);
        const u16 head = ease((u16)(t < 7 ? t + 1 : 7), 7);
        const u16 tail = (t < 3) ? 0 : ease((u16)(t - 3 < 7 ? t - 3 : 7), 7);
        const s16 hx = lerp(from, to, head);
        const s16 tx = lerp(from, to, tail);
        s16 k;
        for (k = -1; k <= 1; ++k) {
            if (t < 7 || (t & 1))
                sprite32(hx, (s16)(cy - 16 + k * 32), SNES_FX_BLADE_HEAD, SNES_FX_BURST_PAL, 0);
            if (t >= 3 && t < 12)
                sprite32(tx, (s16)(cy - 16 + k * 32), SNES_FX_BLADE_TAIL, SNES_FX_BURST_PAL, 0);
        }
    }

    /* 2. The whiteout (8..10) and the wash after it: the arena goes white
     * for three fields, then the attacker's card is held dark under the
     * burst until the beat's last quarter lets it back. */
    if (t >= 8 && t < 11) {
        set_colour_math(1, 31, 0);
    } else if (t >= 11) {
        u8 level = 20;
        if (t >= 56) level = (u8)((SNES_FX_FIELDS - t) * 20 / 16);
        set_colour_math(2, level, 0);
    }

    /* 3. The readout (28..end), emitted BEFORE the ring.  Lower OAM indices
     * win both priority and the SNES's per-line sprite budget, so the number
     * remains readable even when a late, wide ring crosses the same lines. */
    if (t >= D_FX_TEXT) {
        const u16 e = (u16)(t - D_FX_TEXT);
        const u16 count = (e < 32) ? e : 32;
        const s16 dy = (e < 1) ? -10 : (e < 2) ? -6 : (e < 3) ? -3 : 0;
        const u8 pal = (e < 3) ? SNES_FX_BURST_PAL : SNES_FX_DIGIT_PAL;
        damage_readout(cx, cy, damage, pal, dy);
        if (!lp_visible) {
            lp_visible = 1;
            lane_lp(lp_lane, lp_before);
        }
        if (lp_visible == 1 && lp_shown != lp_after) {
            /* The sequence is timed in displayed fields, while this function
             * can run less often when a sprite-heavy field overruns.  Derive
             * the number from the field itself so skipped updates do not make
             * the visible LP trail the rules.  Split damage / 32 into whole
             * and remainder parts so every product still fits sixteen bits. */
            const u16 dropped = (u16)(snesMulLo((u16)(damage >> 5), count) +
                                      (snesMulLo((u16)(damage & 31), count) >> 5));
            lp_shown = (dropped < damage) ? (u16)(lp_before - dropped) : lp_after;
            lane_lp(lp_lane, lp_shown);
        }
    }

    /* 4. The core, the ring, the rays and the sparks (8..end). */
    if (t >= 8) {
        const s16 ring = (s16)snes_fx_ring_r[t];
        const u8 core = snes_fx_core_r[t];
        if (t < 60) ray_sprites(cx, cy, (s16)((ring + (ring << 2)) >> 3));
        if (ring > 8) ring_sprites(cx, cy, ring);
        core_sprite(cx, cy, core);
        if (t < 44) spark_sprites(cx, cy, (u16)(t - 8));
        /* Cooling: the palette's six bands slide down the ramp past three
         * quarters of the beat. */
        cool = (t >= 62) ? 2 : (t >= 52) ? 1 : 0;
    }

}

/* ── The step ────────────────────────────────────────────────────────────── */

/* The big letters, as snesObjBigText draws them but from the atlas's tail. */
static void big_text(s16 x, s16 y, const char *s, u8 set)
{
    u8 c, g;
    u16 base;
    while ((c = (u8)*s++) != 0) {
        if (c >= SNES_SPR_GLYPH_FIRST &&
            c < SNES_SPR_GLYPH_FIRST + SNES_SPR_GLYPH_COUNT) {
            g = snes_spr_big_index[c - SNES_SPR_GLYPH_FIRST];
            if (g != 0xFF) {
                base = (u16)(BIG_OBJ_TILE +
                             (((u16)set * SNES_SPR_BIG_GLYPHS + g) << 2));
                sprite8(x,     y,     base + 0, SNES_SPR_HUD_PAL);
                sprite8(x + 8, y,     base + 1, SNES_SPR_HUD_PAL);
                sprite8(x,     y + 8, base + 2, SNES_SPR_HUD_PAL);
                sprite8(x + 8, y + 8, base + 3, SNES_SPR_HUD_PAL);
            }
        }
        x += SNES_OBJ_BIG_PITCH;
    }
}

/* The verdict: the cards stay where the beat left them and the banner
 * slides in from the left, the board's own entrance, then holds until a
 * press (snesBattleStep). */
static void step_verdict(void)
{
    const u8 won = (g_duel.result > 0);
    const char *word = won ? "YOU WIN" : "YOU LOSE";
    const s16 span = (s16)((won ? 7 : 8) * SNES_OBJ_BIG_PITCH);
    const s16 home = (s16)((256 - span) >> 1);
    s16 x = home;
    if (phase != PHASE_VERDICT) {
        phase = PHASE_VERDICT;
        verdict_step = 0;
        pal_restore = 1;
        snesAudioPlay(won ? SNES_AUDIO_VICTORY : SNES_AUDIO_FAIL);
    } else if (verdict_step <= VERDICT_SLIDE + VERDICT_HOLD) {
        ++verdict_step;
    }
    if (verdict_step < VERDICT_SLIDE) {
        const s16 rem = (s16)(VERDICT_SLIDE - verdict_step);
        x = (s16)(home - ((((home + span) * rem) / VERDICT_SLIDE) * rem)
                          / VERDICT_SLIDE);
    }
    big_text(x, VERDICT_Y, word, won ? SNES_SPR_BIG_SET_GOLD : SNES_SPR_BIG_SET_RED);
}

static void step_direct(void)
{
    const u8 a = attacker_lane;
    if (field < D_ENTER_END) {
        phase = PHASE_ENTER;
        lane_enter(a, field, D_ENTER_END);
    } else if (field < D_LUNGE_END) {
        /* Anticipation back for twelve fields, the lunge forward to contact
         * at field 39, the recoil home. */
        const u16 f = (u16)(field - D_ENTER_END);
        phase = PHASE_LUNGE;
        /* Sixteen back, twenty-four forward: the card's top stays on the
         * screen at the deepest point, so the blow reads as a blow and
         * not as the card leaving. */
        if (f < 12) {
            lane_y[a] = (s16)(REST_Y - FORWARD(a) * lerp(0, 16, ease(f, 12)));
        } else if (f < 19) {
            lane_y[a] = (s16)(REST_Y + FORWARD(a) * lerp(-16, 22, ease((u16)(f - 12), 7)));
        } else {
            lane_y[a] = (s16)(REST_Y + FORWARD(a) * lerp(22, 0, ease((u16)(f - 19), 13)));
        }
    } else if (field < D_FX_END) {
        phase = PHASE_FX;
        lane_y[a] = REST_Y;
    } else if (decided) {
        lane_y[a] = REST_Y;
        set_colour_math(0, 0, 0);
        step_verdict();
        return;
    } else if (field < D_EXIT_END) {
        phase = PHASE_EXIT;
        set_colour_math(0, 0, 0);
        lane_leave(a, (u16)(field - D_FX_END), (u16)(D_EXIT_END - D_FX_END));
    } else {
        phase = PHASE_DONE;
    }
    if (blocked) {
        /* Repelled: a short flash at the deepest point, the word, and
         * straight out -- the beat's length without its burst. */
        if (field >= D_CONTACT && field < D_CONTACT + 3)
            set_colour_math(1, (u8)(field == D_CONTACT ? 24 : 12), 0);
        else if (field >= D_CONTACT + 3 && field < D_FX_END)
            set_colour_math(0, 0, 0);
        if (field >= D_CONTACT && !cue_hit) {
            const u8 col = (u8)(LANE_COL(attacker_lane ^ 1) + 2);
            put_text(col, TEXT_ROW, "TRAP!", PAL_GOLD);
            put_text((u8)(col - 1), (u8)(TEXT_ROW + 1), "ATTACK LOST", PAL_WHITE);
            cue_hit = 1;
            snesAudioSfx(SNES_SFX_CARD_DESTROYED);
        }
        if (field >= D_CONTACT && field < D_CONTACT + 6)
            core_sprite(128, (s16)(REST_Y + 78), (u8)(field < D_CONTACT + 2 ? 15 : 9));
        if (field >= D_LUNGE_END + 24 && field < D_FX_END) {
            /* No beat to sit through: straight to the exit. */
            field_skip = (u16)(field_skip + (D_FX_END - field));
            field = D_FX_END;
            clear_row(TEXT_ROW);
            clear_row((u8)(TEXT_ROW + 1));
        }
        return;
    }
    if (field >= D_FX_START && field < D_FX_END) direct_fx((u16)(field - D_FX_START));
    /* Cues fire on CROSSING their field, so a late step neither loses nor
     * repeats one. */
    if (field >= D_FX_START && !cue_blade) { cue_blade = 1; snesAudioSfx(SNES_SFX_LASER); }
    if (field >= D_CONTACT && !cue_hit) { cue_hit = 1; snesAudioSfx(SNES_SFX_DIRECT_HIT); }
}

static void step_battle(void)
{
    const u8 a = attacker_lane, d = (u8)(a ^ 1);
    /* These are timeline crossings, independent of which pose branch is
     * current.  A delayed foreground step can skip a whole branch. */
    if (field >= B_STRIKE_HIT && !cue_hit) {
        cue_hit = 1;
        snesAudioSfx(SNES_SFX_LASER);
    }
    if (field >= B_STRIKE_END && !cue_burn &&
        (destroy_lane[0] || destroy_lane[1])) {
        cue_burn = 1;
        snesAudioSfx(SNES_SFX_CARD_DESTROYED);
    }
    if (field < B_ENTER_END) {
        phase = PHASE_ENTER;
        lane_enter(0, field, B_ENTER_END);
        lane_enter(1, field, B_ENTER_END);
    } else if (field < B_HOLD_END) {
        phase = PHASE_HOLD;
        lane_y[0] = lane_y[1] = REST_Y;
        if (!cue_blade) { cue_blade = 1; lane_stats(0); lane_stats(1); }
    } else if (field < B_STRIKE_END) {
        const u16 f = (u16)(field - B_HOLD_END);
        phase = PHASE_STRIKE;
        if (field < B_STRIKE_BACK) {
            lane_y[a] = (s16)(REST_Y - FORWARD(a) * lerp(0, 12, ease(f, B_STRIKE_BACK - B_HOLD_END)));
            lane_y[d] = REST_Y;
        } else if (field < B_STRIKE_HIT) {
            lane_y[a] = (s16)(REST_Y + FORWARD(a) * lerp(-12, 28, ease((u16)(field - B_STRIKE_BACK), B_STRIKE_HIT - B_STRIKE_BACK)));
            lane_y[d] = REST_Y;
        } else {
            const u16 g = (u16)(field - B_STRIKE_HIT);
            const u16 n = B_STRIKE_END - B_STRIKE_HIT;
            lane_y[a] = (s16)(REST_Y + FORWARD(a) * lerp(28, 0, ease(g, n)));
            /* The defender recoils away from the blow and settles. */
            if (damage_owner >= 0 || destroy_lane[d])
                lane_y[d] = (s16)(REST_Y - FORWARD(d) * lerp(14, 0, ease(g, n)));
            else
                lane_y[d] = REST_Y;
            if (g < 3) set_colour_math(1, (u8)(g < 2 ? 31 : 16), 0);
            else set_colour_math(0, 0, 0);
            if (g < 5) core_sprite(128, (s16)(REST_Y + 78), (u8)(g < 2 ? 15 : 11));
            if (g < 8) spark_sprites(128, (s16)(REST_Y + 78), g);
        }
        /* The PC's cues: the blow itself, then the burn if a card dies. */
    } else if (field < B_RESULT_END) {
        const u16 f = (u16)(field - B_STRIKE_END);
        phase = PHASE_RESULT;
        lane_y[0] = lane_y[1] = REST_Y;
        /* The destroyed card fades to black inside its own window; the
         * damage punches in over the side that took it. */
        if (destroy_lane[0] || destroy_lane[1]) {
            const u8 lane = destroy_lane[a] ? a : d;
            set_colour_math(3, (u8)(f < 16 ? (f * 2) : 31), lane);
        }
        if (damage) {
            const s16 dy = (f < 1) ? -10 : (f < 2) ? -6 : (f < 3) ? -3 : 0;
            const u8 pal = (f < 3) ? SNES_FX_BURST_PAL : SNES_FX_DIGIT_PAL;
            damage_readout((s16)(LANE_X(lp_lane) + LANE_W / 2), (s16)(REST_Y + 78),
                           damage, pal, dy);
            if (!lp_visible) { lp_visible = 1; lane_lp(lp_lane, lp_before); }
            if (lp_visible == 1 && lp_shown != lp_after) {
                const u16 step = (u16)((damage >> 4) + 25);
                lp_shown = (lp_shown > lp_after + step) ? (u16)(lp_shown - step) : lp_after;
                lane_lp(lp_lane, lp_shown);
            }
        }
    } else if (decided) {
        /* The loser's card keeps its fade; the readout stays at nought. */
        lane_y[0] = lane_y[1] = REST_Y;
        step_verdict();
        return;
    } else if (field < B_EXIT_END) {
        const u16 f = (u16)(field - B_RESULT_END);
        phase = PHASE_EXIT;
        if (lp_visible != 2) { lp_visible = 2; clear_row(TEXT_ROW); clear_row(LP_ROW); }
        lane_leave(0, f, B_EXIT_END - B_RESULT_END);
        lane_leave(1, f, B_EXIT_END - B_RESULT_END);
    } else {
        phase = PHASE_DONE;
    }
    /* A destroyed card stays dark on its way out; a whole-lane wash with
     * no destruction ends with the strike. */
    if (field >= B_RESULT_END && !(destroy_lane[0] || destroy_lane[1]))
        set_colour_math(0, 0, 0);
}

u8 snesBattleStep(u16 down)
{
    /* THE FIELD IS THE VBLANK COUNTER, not the number of times this ran: a
     * step that overran a field (the burst is forty sprites of 816-tcc
     * arithmetic) skips ahead rather than stretching the beat. */
    field = (u16)(snes_vblank_count - field0 + field_skip);
    snesObjBegin();
    if (direct) step_direct(); else step_battle();
    snesObjEnd();
    write_offsets();
    if (phase == PHASE_DONE) return 1;
    if (phase == PHASE_VERDICT) {
        /* The banner has to arrive and be readable before a press counts,
         * as on the board (snes_duel.c's OVER_HOLD_FRAMES). */
        if (verdict_step > VERDICT_SLIDE + VERDICT_HOLD &&
            (down & (KEY_A | KEY_B | KEY_START)))
            return 2;
        return 0;
    }
    if (down & (KEY_A | KEY_B | KEY_START)) {
        const u16 skip_from = direct ? D_SKIP_FROM : B_SKIP_FROM;
        const u16 exit_at = direct ? D_FX_END : B_RESULT_END;
        if (field >= skip_from && field < exit_at) {
            /* Straight to the exit, with the final state on show: the
             * readout's life points settle, the text goes. */
            field_skip = (u16)(field_skip + (exit_at - field));
            lp_shown = lp_after;
            if (lp_visible == 1) lane_lp(lp_lane, lp_after);
            return 0;
        }
    }
    return 0;
}

/* ── Vblank ──────────────────────────────────────────────────────────────── */

void snesBattleVblank(void)
{
    if (pal_restore) {
        pal_restore = 0;
        dmaCopyCGram((u8 *)&snes_spr_pal[SNES_SPR_HUD_PAL * 32], 128 + SNES_SPR_HUD_PAL * 16, 32);
    }
    if (offset_dirty) {
        dmaCopyVram((u8 *)offset_row, BG3_MAP_WORD, 64);
        dmaCopyVram((u8 *)offset_row, (u16)(BG3_MAP_WORD + 32), 64);
        offset_dirty = 0;
    }
    if (bg2_dirty) {
        u8 row;
        dmaCopyVram((u8 *)bg2_map, BG2_MAP_WORD, 2048);
        bg2_dirty = 0;
        for (row = 0; row < 32; ++row) bg2_row_dirty[row] = 0;
    } else {
        /* The rows that changed -- the counting life points are one row a
         * field, not the whole map. */
        u8 row;
        for (row = 0; row < 32; ++row) {
            if (!bg2_row_dirty[row]) continue;
            bg2_row_dirty[row] = 0;
            dmaCopyVram((u8 *)&bg2_map[(u16)row * 32], (u16)(BG2_MAP_WORD + row * 32), 64);
        }
    }
    if (cool != cool_shown) {
        u16 bands[SNES_FX_HEAT_BANDS];
        u8 i;
        for (i = 0; i < SNES_FX_HEAT_BANDS; ++i) bands[i] = snes_fx_heat_ramp[cool + i];
        dmaCopyCGram((u8 *)bands, (u16)(128 + SNES_FX_BURST_PAL * 16 + SNES_FX_HEAT_FIRST),
                     SNES_FX_HEAT_BANDS * 2);
        cool_shown = cool;
    }
    if (cm_dirty) {
        cm_dirty = 0;
        switch (cm_mode) {
        case 1:
            /* The whiteout: the fixed colour added to the card and the
             * backdrop everywhere. */
            REG_WOBJSEL = 0;
            REG_CGWSEL = 0x00;
            REG_COLDATA = (u8)(0x20 | cm_level);
            REG_COLDATA = (u8)(0x40 | cm_level);
            REG_COLDATA = (u8)(0x80 | cm_level);
            REG_CGADSUB = 0x21;
            break;
        case 2:
            /* The wash: grey subtracted from the card only. */
            REG_WOBJSEL = 0;
            REG_CGWSEL = 0x00;
            REG_COLDATA = (u8)(0x20 | cm_level);
            REG_COLDATA = (u8)(0x40 | cm_level);
            REG_COLDATA = (u8)(0x80 | cm_level);
            REG_CGADSUB = 0x81;
            break;
        case 3:
            /* A destroyed card fades: subtraction inside colour window 1,
             * which covers its lane. */
            REG_WH0 = (u8)LANE_X(cm_lane);
            REG_WH1 = (u8)(LANE_X(cm_lane) + LANE_W - 1);
            REG_WOBJSEL = 0x20;
            REG_CGWSEL = 0x10;
            REG_COLDATA = (u8)(0x20 | cm_level);
            REG_COLDATA = (u8)(0x40 | cm_level);
            REG_COLDATA = (u8)(0x80 | cm_level);
            REG_CGADSUB = 0x81;
            break;
        default:
            REG_WOBJSEL = 0;
            REG_CGWSEL = 0;
            REG_CGADSUB = 0;
            REG_COLDATA = 0xE0;
            break;
        }
    }
}
