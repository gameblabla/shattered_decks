/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_battle.c — the attack animation.
 *
 *  See atarist_battle.h for why only one card is ever on screen.  The three
 *  things this file has to get right on an 8 MHz 68000:
 *
 *  EVERY HORIZONTAL POSITION IS A MULTIPLE OF SIXTEEN.  The card slides in
 *  whole 16-pixel groups, so the blit is four `move.w` per group with no
 *  shifting and no read-modify-write, and the panel behind it is an aligned
 *  rectangle with no masked edge groups (atarist_draw.c explains what those
 *  cost).  A card sliding at 16 pixels a frame crosses the screen in twenty
 *  frames, which at 50 Hz is a fast, deliberate move rather than a drift.
 *
 *  THE SHAKE IS VERTICAL.  A horizontal one would take the panel off the group
 *  grid and turn every blit into a shifted, masked one for the sake of four
 *  pixels; moving a row pointer costs nothing at all.
 *
 *  NOTHING IS CACHED BETWEEN FRAMES.  The band the card lives in is cleared
 *  and repainted every frame, because the screen is double buffered and the
 *  card moves on almost every one of them.  It is 140 rows of aligned fill
 *  plus a 96x96 blit -- about 27 KB of stores, well inside a frame.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stddef.h>
#include <stdint.h>

#include "atarist_battle.h"
#include "atarist_video.h"
#include "atarist_draw.h"
#include "atarist_assets.h"
#include "atarist_disk.h"
#include "atarist_probe.h"

#include "msxgl.h"
#include "msx2_duel.h"
#include "msx2_cards.h"

#define BIG_W      ATARIST_ART_BIG_W        /* 96 */
#define BIG_H      ATARIST_ART_BIG_H        /* 96 */

/* The panel: 128 wide, so it is 16-aligned and centres exactly at x = 96.
 * The art sits 16 in from its left edge, which keeps that aligned too. */
#define PANEL_W    128
#define PANEL_H    140
#define PANEL_X    96                       /* (320 - 128) / 2 */
#define PANEL_Y    22
#define ART_DX     16
#define ART_DY     14
#define STAT_DY    (ART_DY + BIG_H + 6)     /* the ATK/DEF row inside the panel */
#define TITLE_Y    6                        /* the banner above the panel */
#define MSG_Y      176                      /* the verdict under it */

/* Only two entries of a battle card's palette are known before it is loaded,
 * and they are the two the converter pins: black at 0 and white at 15.  All
 * text and all frame rules are drawn in those, which is what lets the other
 * fourteen go entirely to the painting. */
#define INK        15
#define PAPER      0

/* HOW LONG EVERYTHING TAKES, scaled by one number.
 *
 * At 1 this is the shipping pace.  Built with a larger value the whole
 * animation slows down proportionally -- phases hold longer AND the card
 * crawls -- which is the only way to photograph it: the headless Hatari runs
 * about fifteen times real time, so a four-second animation is a quarter of a
 * second of wall clock and a screenshot sweep steps straight over it.  A
 * verification build uses -DATARIST_BATTLE_TIME_SCALE=16 and every phase is
 * then several seconds wide.  It changes nothing but durations. */
#ifndef ATARIST_BATTLE_TIME_SCALE
#define ATARIST_BATTLE_TIME_SCALE 1
#endif
#define BEATS(n)  ((n) * ATARIST_BATTLE_TIME_SCALE)

/* Phases.  Each has a duration in vblanks; the slides run until the card is
 * where it is going, so their timers are only a safety net. */
enum {
    BP_IDLE = 0,
    BP_ATK_IN,        /* the attacker enters from the right */
    BP_ATK_HOLD,      /* held, with its ATK printed */
    BP_ATK_OUT,       /* off to the left -- the screen empties for the swap */
    BP_SWAP,          /* one black beat: the palette becomes the defender's */
    BP_DEF_IN,        /* the defender enters from the right */
    BP_STRIKE,        /* the attempted attack: flash and shake */
    BP_VERDICT,       /* the outcome held on the defender's card */
    BP_BURN,          /* a destroyed card dissolves */
    BP_BACK_SWAP,     /* black beat back to the attacker's palette */
    BP_ATK_BACK,      /* the attacker returns to be destroyed in its turn */
    BP_ATK_BURN,
    BP_DONE
};

/* Which card the screen is showing, and therefore whose palette is loaded. */
enum { SIDE_ATK = 0, SIDE_DEF = 1 };

static uint8_t  g_phase;
static uint8_t  g_side;          /* which of the two records is displayed */
static uint8_t  g_have[2];       /* the record loaded off the floppy */
static int16_t  g_timer;
static int16_t  g_x;             /* panel x, in 16-pixel steps */
static uint8_t  g_shake;
static uint8_t  g_burn;          /* dissolve rows completed */
static uint8_t  g_flash;

/* What is being shown.  Copied out of g_duel at Begin: the rules keep moving
 * while the animation plays out only in the sense that nothing else runs, but
 * copying makes the animation independent of the record's lifetime. */
static uint8_t  g_atk_card, g_def_card;
static int16_t  g_atk_value, g_def_value, g_damage;
static uint8_t  g_outcome, g_trap, g_direct;
static uint8_t  g_atk_dies, g_def_dies;

/* The two records, READ WHOLE.  A record is its sixteen RGB triples followed
 * by the planar image, and both are pulled in by a single Fread: the palette is
 * 48 bytes and a second seek-and-read for it cost as much as the 4,608-byte one
 * did.  The array is uint16_t so the plane data is word aligned without relying
 * on the linker's choice for a byte array, and the 48-byte palette is a whole
 * number of words, so the image starts aligned too. */
#define BIG_PAL_WORDS  (ATARIST_ART_BIG_PAL / 2)
/* Plus the depacker's slack: a record arrives PACKED and is depacked in place
 * inside this very buffer, whose tail the packed bytes are read into. */
#define BIG_REC_WORDS  ((ATARIST_ART_BIG_RECORD + ATARIST_ZX0_SLACK + 1) / 2)
static uint16_t g_rec[2][BIG_REC_WORDS];
static uint16_t g_pal[2][16];
static AtaristImage g_img[2];
static int16_t g_file = -1;         /* DAT/BIG.CRD, held open */

/* DAT/BIG.CRD's index, read once when the file is opened.  The records are
 * packed one by one, so they are no longer a fixed stride and a face is an
 * offset lookup instead of a multiply; the extra entry is the end of the last
 * record, which is what gives every record a length. */
static uint32_t g_big_off[ATARIST_ART_BIG_FACES + 1];
static uint8_t  g_big_index;        /* the index is resident and valid */

/* ── Loading ─────────────────────────────────────────────────────────────── */

/* One face out of DAT/BIG.CRD, through an already-open handle.  Fixed-size
 * records, so a face is a seek and a read; an index table would be one more
 * thing to keep in step with the converter for no gain.
 *
 * THE HANDLE IS THE CALLER'S because a battle wants two records and opening a
 * file on a floppy is the expensive half: GEMDOS walks the directory and then
 * the FAT chain, and this file is read from 300 KB in.  Four opens a battle --
 * a palette and an image for each card -- stalled the game for five seconds;
 * one open and two reads is a quarter of that. */
/* The index off the front of DAT/BIG.CRD.  Read ONCE, with the open: it is
 * under 300 bytes and every battle after the first would otherwise pay a seek
 * to the start of a file it is about to seek 200 KB into. */
static int load_big_index(int16_t handle)
{
    uint8_t hdr[ATARIST_ART_BIG_INDEX];
    int i;

    if (Atarist_DiskReadAt(handle, 0, hdr, sizeof hdr) != (int32_t)sizeof hdr)
        return 0;
    if (hdr[0] != 'Z' || hdr[1] != 'X' || hdr[2] != 'R' || hdr[3] != '1')
        return 0;
    if (((int)hdr[4] << 8 | hdr[5]) != ATARIST_ART_BIG_FACES) return 0;
    for (i = 0; i <= ATARIST_ART_BIG_FACES; ++i) {
        const uint8_t *p = hdr + 8 + i * 4;
        g_big_off[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
    }
    return 1;
}

static int load_face(int16_t handle, int slot, int face)
{
    const uint8_t *rgb = (const uint8_t *)g_rec[slot];
    int32_t off, avail;
    int i;

    g_have[slot] = 0;
    /* Monsters only -- see ATARIST_ART_BIG_FACES.  A support or a card back
     * reaching this is a bug in the caller, not a missing record. */
    if (face < 0 || face >= ATARIST_ART_BIG_FACES) return 0;
    if (!g_big_index) return 0;
    off = (int32_t)g_big_off[face];
    avail = (int32_t)(g_big_off[face + 1] - g_big_off[face]);
    if (Atarist_DiskReadPackedAt(handle, off, avail, g_rec[slot],
                                 (int32_t)sizeof g_rec[slot]) !=
        ATARIST_ART_BIG_RECORD)
        return 0;

    for (i = 0; i < 16; ++i)
        g_pal[slot][i] = Atarist_PackRGB(rgb[i * 3], rgb[i * 3 + 1],
                                         rgb[i * 3 + 2]);
    g_img[slot].w = BIG_W;
    g_img[slot].h = BIG_H;
    g_img[slot].has_mask = 0;
    g_img[slot].data = g_rec[slot] + BIG_PAL_WORDS;
    g_have[slot] = 1;
    return 1;
}

static void show_palette(int slot)
{
    static const uint16_t black[16] = {0};
    g_side = (uint8_t)slot;
    Atarist_SetWholePalette(g_have[slot] ? g_pal[slot] : black);
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

/* Atarist_BlitImageRect clips its right edge and both vertical edges but
 * refuses a negative destination group, so a card leaving to the left is drawn
 * as the sub-rectangle that is still on screen.  Everything is 16-aligned, so
 * that costs a subtraction rather than a shifted blit. */
static void blit_card(int slot, int x, int y)
{
    const AtaristImage *img = &g_img[slot];
    int sx = 0, w = BIG_W;

    if (!g_have[slot]) {
        Atarist_FillRect(x, y, BIG_W, BIG_H, PAPER);
        Atarist_FrameRect(x, y, BIG_W, BIG_H, INK);
        return;
    }
    if (x < 0) { sx = -x; w -= sx; x = 0; }
    if (w <= 0 || x >= ATARIST_SCREEN_W) return;
    Atarist_BlitImageRect(img, sx, 0, w, BIG_H, x, y);
}

static void draw_number_pair(int x, int y, const char *label, int32_t value)
{
    Atarist_DrawText(x, y, label, INK, PAPER);
    Atarist_DrawNumber(x + 96, y, value, INK, PAPER);
}

/* The card, its frame and its figures, at panel x `px`.
 *
 * THE FRAME IS TWO RULES IN BLACK AND WHITE AND NOTHING ELSE.  There is no
 * gold in this palette and there must not be: reserving an entry for a frame
 * would take it off the painting, which is the one thing on screen.  A white
 * keyline on a black panel is what a card reads as at this size anyway. */
static void draw_card(int slot, int px, int py)
{
    int ay = py + ART_DY;

    Atarist_FillRect(px, py, PANEL_W, PANEL_H, PAPER);
    Atarist_FrameRect(px, py, PANEL_W, PANEL_H, INK);
    blit_card(slot, px + ART_DX, ay);
    Atarist_FrameRect(px + ART_DX - 1, ay - 1, BIG_W + 2, BIG_H + 2, INK);

    if (slot == SIDE_ATK)
        draw_number_pair(px + 8, py + STAT_DY, "ATK", g_atk_value);
    else if (g_direct)
        draw_number_pair(px + 8, py + STAT_DY, "DMG", g_damage);
    else
        draw_number_pair(px + 8, py + STAT_DY,
                         g_duel.last_battle.defender_passive ? "DEF" : "ATK",
                         g_def_value);
}

static const char *banner(void)
{
    switch (g_phase) {
    case BP_ATK_IN:
    case BP_ATK_HOLD:
    case BP_ATK_OUT:   return "ATTACK";
    case BP_DEF_IN:    return g_direct ? "DIRECT ATTACK" : "DEFENDER";
    case BP_STRIKE:    return g_trap ? "TRAP" : "CLASH";
    case BP_ATK_BACK:
    case BP_ATK_BURN:  return "COUNTER";
    default:           return g_direct ? "DIRECT ATTACK" : "CLASH";
    }
}

static const char *verdict(void)
{
    if (g_trap)   return "THE TRAP DESTROYS IT";
    if (g_direct) return "A DIRECT HIT";
    switch (g_outcome) {
    case MSX2_BATTLE_DESTROY_DEFENDER: return "DEFENDER DESTROYED";
    case MSX2_BATTLE_DESTROY_ATTACKER: return "THE ATTACKER FALLS";
    case MSX2_BATTLE_DESTROY_BOTH:     return "BOTH DESTROYED";
    case MSX2_BATTLE_NO_DESTROY:       return "NEITHER FALLS";
    default:                           return "";
    }
}

/* The dissolve.  Rows are taken in a stride of thirteen rather than in order:
 * thirteen and ninety-six are coprime, so the walk visits every row exactly
 * once and the card comes apart all over at once instead of being wiped from
 * the top.  It is one aligned single-row fill per row, and the whole effect is
 * ninety-six of them spread over its phase. */
static void draw_burn(int px, int py, int rows)
{
    int ay = py + ART_DY;
    int i;
    for (i = 0; i < rows && i < BIG_H; ++i)
        Atarist_FillRect(px + ART_DX, ay + (i * 13) % BIG_H, BIG_W, 1, PAPER);
}

static void draw_frame(void)
{
    int px = g_x;
    int py = PANEL_Y + (g_shake ? ((g_shake & 1) ? 2 : -2) : 0);
    int showing = (g_phase != BP_SWAP && g_phase != BP_BACK_SWAP &&
                   g_phase != BP_DONE);

    Atarist_ClearPlanar(PAPER);
    if (!showing) return;

    Atarist_DrawTextCentred(160, TITLE_Y, banner(), INK, PAPER);
    draw_card(g_side, px, py);
    if (g_burn) draw_burn(px, py, g_burn);
    if (g_phase == BP_VERDICT || g_phase == BP_BURN ||
        g_phase == BP_ATK_BURN)
        Atarist_DrawTextCentred(160, MSG_Y, verdict(), INK, PAPER);
}

/* ── The state machine ───────────────────────────────────────────────────── */

#define SLIDE_STEP  16          /* pixels a step; keeps every x 16-aligned */
#define OFF_RIGHT   320
#define OFF_LEFT    (-PANEL_W)

/* Frames a 16-pixel step takes.  One at the shipping pace, so the card crosses
 * the screen in twenty frames. */
#define SLIDE_HOLD  ATARIST_BATTLE_TIME_SCALE

static int16_t g_slide_acc;

static void enter(int phase, int timer)
{
    g_phase = (uint8_t)phase;
    g_timer = (int16_t)timer;
    g_shake = 0;
    g_flash = 0;
    g_slide_acc = 0;
}

/* Move the panel `dir` * 16 pixels per SLIDE_HOLD vblanks, stopping at
 * `limit`.  Returns non-zero once it is there.  The accumulator is what keeps
 * every x on the 16-pixel group grid at any speed -- scaling the step instead
 * would put the card between groups and turn the blit into a shifted one. */
static int slide_to(int limit, int dir, int vblanks)
{
    g_slide_acc = (int16_t)(g_slide_acc + vblanks);
    while (g_slide_acc >= SLIDE_HOLD) {
        g_slide_acc = (int16_t)(g_slide_acc - SLIDE_HOLD);
        g_x = (int16_t)(g_x + dir * SLIDE_STEP);
        if ((dir < 0 && g_x <= limit) || (dir > 0 && g_x >= limit)) {
            g_x = (int16_t)limit;
            return 1;
        }
    }
    return (dir < 0) ? (g_x <= limit) : (g_x >= limit);
}

int Atarist_BattleBegin(void)
{
    const Msx2BattleCalc *bc = &g_duel.last_battle;

    if (g_duel.last_action != MSX2_ACTION_ATTACK) return 0;

    g_atk_card = g_duel.last_attacker_card;
    g_def_card = g_duel.last_defender_card;
    g_trap = g_duel.last_trap_fired;
    g_outcome = bc->outcome;
    g_direct = (uint8_t)(!g_trap && (bc->outcome == MSX2_BATTLE_DIRECT ||
                                     g_def_card == MSX2_CARD_NONE));
    g_atk_value = bc->attacker_atk;
    g_def_value = bc->defender_value;
    g_damage = bc->damage;
    /* A trap fires before the battle step, so the rules never filled in an
     * outcome for it -- the attacker simply dies. */
    g_atk_dies = (uint8_t)(g_trap ||
                           bc->outcome == MSX2_BATTLE_DESTROY_ATTACKER ||
                           bc->outcome == MSX2_BATTLE_DESTROY_BOTH);
    g_def_dies = (uint8_t)(!g_trap && !g_direct &&
                           (bc->outcome == MSX2_BATTLE_DESTROY_DEFENDER ||
                            bc->outcome == MSX2_BATTLE_DESTROY_BOTH));

    if (!Atarist_AssetsHaveArt()) return 0;
    {
        /* THE HANDLE IS OPENED ONCE AND KEPT.  The open is most of the cost --
         * GEMDOS walks the directory and then the FAT chain to a file read
         * from 300 KB in -- so paying it per attack was three seconds a
         * battle.  TOS stays resident for the life of the program and closes
         * it at exit; nothing else touches this file. */
        int ok;
        if (g_file < 0) {
            g_file = Atarist_DiskOpen("DAT\\BIG.CRD");
            if (g_file >= 0) g_big_index = (uint8_t)load_big_index(g_file);
        }
        if (g_file < 0 || !g_big_index) return 0;
        ok = load_face(g_file, SIDE_ATK,
                       Atarist_CardFaceForCard(g_atk_card, 1));
        if (ok && !g_direct && !g_trap)
            load_face(g_file, SIDE_DEF,
                      Atarist_CardFaceForCard(g_def_card, 1));
        else
            g_have[SIDE_DEF] = 0;
        if (!ok) return 0;
    }

    Atarist_SetSplitEnabled(0);
    show_palette(SIDE_ATK);
    g_x = OFF_RIGHT;
    g_burn = 0;
    enter(BP_ATK_IN, BEATS(60));
    return 1;
}

int Atarist_BattleActive(void)
{
    return g_phase != BP_IDLE && g_phase != BP_DONE;
}

/* One frame of white, used at the moment of impact.  It is a palette write and
 * costs nothing; a white rectangle over the card would cost a fill and would
 * have to be undone. */
static void flash_palette(void)
{
    uint16_t white[16];
    int i;
    uint16_t w = Atarist_PackRGB(248, 248, 248);
    for (i = 0; i < 16; ++i) white[i] = w;
    Atarist_SetWholePalette(white);
}

/* The dissolve advances four rows a frame at the shipping pace, and the whole
 * 96 rows take the same share of its phase at any time scale. */
static void burn_step(int vblanks)
{
    int step = (4 * vblanks + ATARIST_BATTLE_TIME_SCALE - 1) /
               ATARIST_BATTLE_TIME_SCALE;
    if (step < 1) step = 1;
    if (g_burn + step >= BIG_H) g_burn = BIG_H;
    else g_burn = (uint8_t)(g_burn + step);
}

static void advance(int vblanks)
{
    if (g_timer > vblanks) g_timer = (int16_t)(g_timer - vblanks);
    else g_timer = 0;

    switch (g_phase) {
    case BP_ATK_IN:
        if (slide_to(PANEL_X, -1, vblanks) && !g_timer)
            enter(BP_ATK_HOLD, BEATS(40));
        break;

    case BP_ATK_HOLD:
        if (!g_timer) enter(BP_ATK_OUT, BEATS(40));
        break;

    case BP_ATK_OUT:
        if (slide_to(OFF_LEFT, -1, vblanks)) {
            /* THE PALETTE CHANGES HERE, WITH THE SCREEN EMPTY.  It is the one
             * moment in the animation when nothing is drawn, which is exactly
             * why the attacker has to leave before the defender arrives. */
            enter(BP_SWAP, BEATS(8));
        }
        break;

    case BP_SWAP:
        if (!g_timer) {
            if (g_direct || g_trap) {
                /* No defender to bring on: the strike lands on the player. */
                show_palette(SIDE_ATK);
                g_x = PANEL_X;
                enter(BP_STRIKE, BEATS(24));
            } else {
                show_palette(SIDE_DEF);
                g_x = OFF_RIGHT;
                enter(BP_DEF_IN, BEATS(60));
            }
        }
        break;

    case BP_DEF_IN:
        if (slide_to(PANEL_X, -1, vblanks) && !g_timer)
            enter(BP_STRIKE, BEATS(24));
        break;

    case BP_STRIKE:
        /* The attempted attack, shown whether or not it lands: the card is
         * struck and shakes, and only then does the verdict appear.  Showing
         * the outcome without the blow is what made the first cut of this read
         * as a slide show. */
        g_shake = (uint8_t)(g_shake + 1);
        g_flash = (uint8_t)(g_timer > BEATS(18));
        if (!g_timer) enter(BP_VERDICT, BEATS(45));
        break;

    case BP_VERDICT:
        if (!g_timer) {
            if (g_def_dies) enter(BP_BURN, BEATS(30));
            /* A trap kills the attacker with no defender ever shown, so the
             * attacker's card is still the one on screen: burn it where it
             * stands rather than swapping to a palette that is already up and
             * sliding it back in from a side it never left. */
            else if (g_atk_dies && g_side == SIDE_ATK)
                enter(BP_ATK_BURN, BEATS(30));
            else if (g_atk_dies) enter(BP_BACK_SWAP, BEATS(10));
            else enter(BP_DONE, 0);
        }
        break;

    case BP_BURN:
        burn_step(vblanks);
        if (g_burn >= BIG_H && !g_timer) {
            if (g_atk_dies && !g_trap) enter(BP_BACK_SWAP, BEATS(10));
            else enter(BP_DONE, 0);
        }
        break;

    case BP_BACK_SWAP:
        if (!g_timer) {
            /* The counter-kill.  The attacker's own palette has to come back
             * before its card can, which is the same empty-screen swap the
             * hand-off used, run in reverse. */
            show_palette(SIDE_ATK);
            g_burn = 0;
            g_x = OFF_LEFT;
            enter(BP_ATK_BACK, BEATS(60));
        }
        break;

    case BP_ATK_BACK:
        if (slide_to(PANEL_X, +1, vblanks) && !g_timer)
            enter(BP_ATK_BURN, BEATS(30));
        break;

    case BP_ATK_BURN:
        burn_step(vblanks);
        if (g_burn >= BIG_H && !g_timer) enter(BP_DONE, 0);
        break;

    default:
        break;
    }
}

void Atarist_BattleStep(int vblanks)
{
    if (!Atarist_BattleActive()) return;
    if (vblanks < 1) vblanks = 1;

    advance(vblanks);
    draw_frame();

    if (g_flash) flash_palette();
    else if (g_phase != BP_SWAP && g_phase != BP_BACK_SWAP)
        show_palette(g_side);

    g_atarist_probe.menu_cursor = g_phase;
}
