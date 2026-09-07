/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_duel.c — duel presentation.
 *
 *  M4 stage: a duel that is PLAYED.  The rules model (src/msx2/msx2_duel.c) is
 *  the same render-free fork the MSX2 and Atari ST ports use and it is not
 *  touched here; this file asks it questions, shows the answers, and hands it
 *  the player's actions.  The state machine below is the ST's
 *  (src/atarist/atarist_duel.c), because a second, differently-shaped UI over
 *  the same rules is two things to keep correct instead of one.
 *
 *  TWO VIEWS OF ONE BOARD, and the player moves between them with UP and DOWN.
 *
 *  The board view is the textured slab in perspective: twenty slots with their
 *  cards lying on them, a marker under the slot in question, and the card being
 *  played held in the air over its target through the convex-quad path.  Its
 *  two resolutions are what the whole video model exists for -- 64x40 texels
 *  while something is moving, 128x80 once it settles, with the PPU doing the
 *  doubling -- so a moving board costs a quarter of the pixels and nothing
 *  extra to display.
 *
 *  The top view is the same board as a flat table: Mode 3, the full 256x224,
 *  no software rendering at all and therefore sixty fields a second.  It is
 *  the view the other ports call the tactical top view, and the player walks
 *  up into it the same way -- UP -- and back down with DOWN.
 *
 *  NOTHING IN THE HUD IS DRAWN INTO THE BITMAP any more.  Life points, the
 *  prompt, the hand and both cursors are sprites, which is what lets them stay
 *  on screen at the console's own resolution across a mode change that rewrites
 *  no VRAM at all.  See snes_obj.h.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_cards.h"
#include "snes_obj.h"
#include "snes_stamp.h"
#include "msx2_duel.h"
#include "msx2_cards.h"

/* The arena camera.  One unit is one slot pitch; the board runs z = -2..2, so
 * a camera one unit up and three back looks along the slab with its far edge
 * near the horizon. */
/* WHERE THE CAMERA STANDS IS FIXED BY THE FOCAL LENGTH, not by taste.
 *
 * The focal length has to be half the viewport width -- that is what makes the
 * floor's texture step a shift instead of a divide (see set_viewport) -- so the
 * slab's half width of 2.5 units fills the half screen exactly when the near
 * edge is 2.5 units away, and overflows it at anything closer.  At three back
 * the near row ran off both sides of the screen and the far row was a fifth of
 * its depth, which is one huge row of cards with three squashed ones behind
 * it.  Standing 2.75 units in front of the near edge fits the whole slab
 * across with a few pixels to spare and brings the near/far depth ratio to
 * 2.4, which is the trapezoid the FM TOWNS, PC-FX and Atari ST boards show:
 * four rows the player can count.  The height then follows from wanting the
 * near edge just above the bottom of the board band. */
#define CAM_HEIGHT   ((s16)704)         /* 2.75 */
#define CAM_Z        ((s16)(-1216))     /* 2.75 in front of the near edge */

/* The surround: BLACK, everywhere the slab does not reach.  The board is the
 * only textured object on the screen -- no painted ground, no sky band -- and
 * a black surround is also what makes the sprite layer over it read as a HUD
 * rather than as clutter on a picture. */
#define BACKDROP     SNES_DC(0, 0, 0)
#define MARK_YOU     SNES_DC(7, 6, 1)   /* the cursor: hot gold */
#define MARK_COM     SNES_DC(7, 2, 1)   /* what the opponent is acting on */

/* The card the player is holding hovers over the slot it is going into. */
/* The lift and the lean, and their sum is the constraint: the camera is ONE
 * world unit up, so a far edge raised anywhere near that projects at the
 * horizon and the quad stretches off the top of the screen.  Two thirds of the
 * camera's height is as far as the card leans back. */
#define HELD_LIFT    ((s16)32)          /* 0.125 world units above the board */
#define HELD_TILT    ((s16)64)          /* the far edge, 0.25 units higher */

/* The sprite HUD, in SCREEN pixels.  The bitmap's HUD band under the board is
 * flat black and is uploaded exactly once; everything below is drawn over it
 * by the PPU.
 *
 * THE LIFE POINTS ARE AT THE TOP OF THE SCREEN AND THE HAND IS ABOVE THE TEXT
 * THAT DESCRIBES IT, which is the arrangement every other port uses and the
 * one this port had backwards.  Two things follow from it:
 *
 *   * The LP panels are the same red/blue pair the PC and PC-FX builds draw
 *     (src/main.c's draw_lp_label), eight pixels in from the top corners, and
 *     they are drawn in EITHER view -- they are the one part of the HUD that
 *     does not move when the player walks up into the top view.
 *   * The band under the board reads downwards the way a card does: the hand,
 *     then the name of the card the cursor is on, then its ATK and DEF.  The
 *     button legend that used to occupy the name row now falls back into the
 *     stat row whenever the focused card has no stats to show, so nothing was
 *     lost by giving the name a line of its own. */
#define LP_Y         8
#define LP_YOU_X     8
#define LP_COM_X     (256 - 8 - SNES_OBJ_LIFE_W)

#define HAND_Y       162
#define HAND_PITCH   48                 /* five 32-pixel cards across 256 */
#define HAND_X0      8
/* The name and the stats sit ONE PIXEL LOWER than the plate they are on, which
 * is what stops the ascenders touching the gradient's top highlight. */
#define NAME_Y       199
#define STAT_Y       209
#define STAT_ATK_X   8
#define STAT_DEF_X   72
/* A sword and a shield, not the words ATK and DEF: the label is one cell wide
 * instead of three, and the gap after it is the second. */
#define STAT_NUM_DX  16

/* The top view's table, from tools/snes/gen_snes_obj.py: a 5x4 grid of
 * SNES_TOP_CELL cells with a 32x32 card centred in each. */
#define TOP_CARD_X(col)  (SNES_TOP_X0 + (col) * SNES_TOP_CELL + (SNES_TOP_CELL - 32) / 2)
#define TOP_CARD_Y(row)  (SNES_TOP_Y0 + (row) * SNES_TOP_CELL + (SNES_TOP_CELL - 32) / 2)
/* The top view has one free row, under the table's near edge, so the name and
 * the stats share it. */
#define TOP_MSG_Y    213
/* Both stats have to fit beside the name on that one row: an icon and its gap
 * are two cells and a number four, so a stat is six and the pair is the
 * right-hand half of the screen with a cell to spare.  Everything here stays on
 * the eight-pixel grid, which is also what lets the harness read the row back
 * as columns rather than as pixels. */
#define TOP_STAT_X   120
#define TOP_STAT_GAP 72

enum SnesDuelUi {
    UI_HAND = 0,        /* choosing a card in hand */
    UI_PLACE,           /* choosing where a monster goes */
    UI_EQUIP_TARGET,    /* choosing the monster an equip attaches to */
    UI_ATTACKER,        /* choosing which of your monsters attacks */
    UI_DEFENDER,        /* choosing what it attacks */
    UI_COM,             /* the opponent is acting */
    UI_RESULT
};

static SnesCamera   cam;
static SnesViewport vp;
/* EVERY STATIC IS INITIALISED EXPLICITLY, and that is not style.
 *
 * An uninitialised static on this build is NOT zero at boot: a probe placed in
 * the frame stamp read 255 out of a fresh one, because 816-tcc's .bss goes into
 * a RAM section that pvsneslib's crt0 clear does not actually cover, while
 * anything with an initialiser is copied from the ROM image and is therefore
 * exact.  The autoplay flag is what found it -- a duel that played itself with
 * no input, which reads as a rules bug and is not one. */
static u8  ui = UI_COM;
static u8  cursor = 0;          /* hand slot or field slot, per state */
static u8  chosen = 0;          /* the hand slot being played, or the attacker */
static u8  com_delay = 0;
static u8  motion = 0;          /* frames of "something is changing" left */
static u8  lift_phase = 0;
static u8  board_dirty = 0;
/* UP walks the camera up into the tactical top view and DOWN walks it back
 * down, in every state the player owns -- the same gesture the PC, FM TOWNS
 * and PC-FX builds use. */
static u8  top_view = 0;
static const char *message = 0;
static u8  message_timer = 0;

/* SELECT hides the cards, and it is not a debug convenience: the board is the
 * only thing the port is allowed to make a performance claim about, and every
 * claim has to come from a measurement or an ABLATION.  This is the ablation --
 * the same frame with the cards compiled in but not drawn -- and
 * tools/snes/verify.py drives it to attribute the cost and to measure the
 * slab's own shape without cards lying on it. */
static u8  show_cards = 1;

/* L hands the player's side to the rules as well, which is both a demo mode
 * and the port's soak harness: a duel that plays itself to a result with no
 * input at all.  It is a RUNTIME toggle and not a build flag on purpose -- the
 * MSX2 port kept photographing its soak ROM by mistake, and a second ROM that
 * looks like the real one is exactly how that happens. */
static u8  autoplay = 0;

/* Y pins the board to the moving resolution.  The duel picks the resolution
 * itself -- moving while something is changing, still once it settles -- so a
 * plain toggle would be undone by the next frame; what this is for is holding
 * the cheap resolution still enough to MEASURE it. */
static u8  force_moving = 0;

static void set_viewport(void)
{
    if (snesVideoBoardRes() == SNES_RES_STILL) {
        vp.w = SNES_STILL_W;
        vp.h = SNES_STILL_H;
    } else {
        vp.w = SNES_MOVING_W;
        vp.h = SNES_MOVING_H;
    }
    vp.origin = 0;
    vp.stride = SNES_FB_STRIDE;

    /* The focal length is half the viewport width, which is what makes the
     * floor's texture step a shift rather than a divide: 32 texels a world
     * unit over a focal length of w/2 is exactly 2 texels a pixel at w = 128
     * and 1 at w = 64.  See snesDrawFloor. */
    cam.focal  = (s16)((u16)vp.w << 7);
    /* The horizon sits a thirty-second of the band down, which puts the
     * SLAB'S FRONT WALL clear of the bottom of the board band with black
     * under it.  That black is what makes the wall read as the near face of a
     * slab standing on nothing rather than as more floor: at an eighth the
     * board ran to the last row of the band and the wall touched the HUD.  It
     * is the same proportion in both resolutions, so the two frame the board
     * identically and switching between them does not shift it. */
    cam.horizon = (s16)(vp.h >> 5);
}

static void touch_board(u8 frames)
{
    board_dirty = 1;
    if (frames > motion) motion = frames;
}

static void say(const char *msg)
{
    message = msg;
    message_timer = 60;
}

/* ── The board ───────────────────────────────────────────────────────────── */

/* Which face a slot shows: its own picture face up, the back face down.  The
 * back is the id past the last card, so a set monster costs no special case in
 * the renderer at all. */
static u8 face_of(u8 card, u8 faceup)
{
    if (card == MSX2_CARD_NONE) return SNES_CARD_NONE_FACE;
    if (!faceup || card >= SNES_CARD_BACK) return SNES_CARD_BACK;
    return card;
}

/* Which board row a field slot of `owner` occupies. */
static u8 monster_row(u8 owner)
{
    return owner ? SNES_ROW_COM_MONSTER : SNES_ROW_YOU_MONSTER;
}

/* The board slot the cursor is on, or 0xFF when the cursor is in the hand. */
static u8 cursor_board_slot(u8 *row)
{
    switch (ui) {
    case UI_PLACE:
    case UI_ATTACKER:
        *row = monster_row(MSX2_OWNER_PLAYER);
        return cursor;
    case UI_EQUIP_TARGET:
        *row = SNES_ROW_YOU_SUPPORT;
        return cursor;
    case UI_DEFENDER:
        *row = monster_row(MSX2_OWNER_COM);
        return cursor;
    default:
        return MSX2_SLOT_NONE;
    }
}

/* The slot the held card is going into, if the player is holding one. */
static u8 held_slot(u8 *row)
{
    if (ui != UI_PLACE && ui != UI_EQUIP_TARGET) return MSX2_SLOT_NONE;
    return cursor_board_slot(row);
}

static void draw_board_cards(void)
{
    u8 row, col, hrow = 0;
    const u8 hslot = held_slot(&hrow);

    for (row = 0; row < SNES_ROWS; ++row) {
        const u8 owner = (row <= SNES_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                       : MSX2_OWNER_PLAYER;
        const u8 support = (row == SNES_ROW_COM_SUPPORT ||
                            row == SNES_ROW_YOU_SUPPORT);
        const Msx2Side *s = &g_duel.side[owner];
        u8 faces[SNES_COLS];

        for (col = 0; col < SNES_COLS; ++col) {
            const u8 card = support ? s->equip_field[col] : s->field[col];
            faces[col] = face_of(card, support ? 1 : s->faceup[col]);
        }
        snesDrawCardRow(&vp, &cam, row, faces);
        (void)hslot;
        (void)hrow;
    }
}

/* The card the player is holding, through the general convex quad path: four
 * projected corners, two edge chains, an affine walk.  It leans back so the
 * picture turns towards the player, and it bobs, which means the quad is a
 * different quad every frame -- the case the two chains exist for. */
static void draw_held_card(void)
{
    const Msx2Side *s = &g_duel.side[MSX2_OWNER_PLAYER];
    u8 row = 0;
    const u8 slot = held_slot(&row);
    u8 card;
    SnesVert q[4];
    s16 lift;

    if (slot == MSX2_SLOT_NONE || slot >= SNES_COLS) return;
    card = s->hand[chosen];
    if (card == MSX2_CARD_NONE) return;

    /* A SMALL bob.  The sine is Q8.8, so a shift of two would swing the card a
     * quarter of a world unit -- from under the board to level with the
     * camera, which is where the quad stretches off the top of the screen.
     * Four is about a sixteenth of a unit, which reads as a card being held. */
    lift = (s16)(HELD_LIFT + (snesSin(lift_phase) >> 4));
    if (!snesCardQuad(&cam, &vp, row, slot, lift, HELD_TILT, q)) return;
    snesTexQuad(&vp, q, face_of(card, 1));
}

/* A board, timed.
 *
 * NTSC is 262 scanlines a field, and a still frame is more than one field of
 * work, so the V counter alone would wrap and underreport; the vblank count
 * carries the whole fields and the V counter the remainder.  This number is
 * the only thing in the port allowed to support a performance claim -- that,
 * or an ablation.  tools/snes/verify.py reads it out of the WRAM dump. */
static void render(void)
{
    const u16 vbl0  = snes_vblank_count;
    const u16 line0 = snesVCounter();
    u8 row = 0;
    const u8 slot = cursor_board_slot(&row);

    snesDrawFloor(&vp, &cam, BACKDROP);
    /* The marker goes down BEFORE the cards: it covers a whole tile and a card
     * four fifths of one, so what is left of it is a rim around the card,
     * which is what makes "this slot" readable when the slot is occupied. */
    if (slot != MSX2_SLOT_NONE && slot < SNES_COLS)
        snesDrawSlotMarker(&vp, &cam, row, slot,
                           (ui == UI_DEFENDER) ? MARK_COM : MARK_YOU);
    if (show_cards) {
        draw_board_cards();
        draw_held_card();
    }

    g_stamp.render_lines = (u16)((snes_vblank_count - vbl0) * 262
                                 + snesVCounter() - line0);
    snesVideoPresentRestart();
    board_dirty = 0;
}

/* ── The HUD band ────────────────────────────────────────────────────────── */

/* THE BAND'S TEXT ROWS SIT ON A BLUE PLATE AND THE REST OF IT STAYS BLACK.
 *
 * The hand is card art and wants black around it; the two rows of words under
 * it are white letters on black, which over a black surround is a caption
 * floating in nothing.  A gradient behind them is the cheapest thing that makes
 * them read as a panel, and it costs one span a row, once, because the bitmap's
 * HUD band is uploaded only when it is dirtied.
 *
 * The arithmetic is the band's, from snes_video.h: the band always samples at
 * 2x2 out of framebuffer rows 80..111, so a row is two screen lines and rows
 * 99..111 are lines 197..222.  Line 223 CANNOT be reached and painting row 112
 * does not reach it either -- the band is thirty-two rows and the presenter
 * uploads exactly those, so a row past 111 sits in WRAM and never goes to
 * VRAM.  The last line of the screen stays black; it is inside every set's
 * overscan and is not worth a taller band to own.
 *
 * Direct colour has THREE bits of red and green and only TWO of blue, so a blue
 * ramp cannot be made out of blue: the darkening happens in the other two
 * channels while blue holds near its top, which is also why the first row is
 * lighter than the second -- a highlight rule along the top edge is a step the
 * cube can afford where a smooth fade at the top is not. */
#define BAND_Y0      99
#define BAND_ROWS    13

static const u8 band_ramp[BAND_ROWS] = {
    SNES_DC(2, 4, 3),   /* the top rule, a shade brighter than the plate */
    SNES_DC(1, 3, 3),
    SNES_DC(1, 3, 3),
    SNES_DC(1, 2, 3),
    SNES_DC(1, 2, 3),
    SNES_DC(0, 2, 3),
    SNES_DC(0, 2, 2),
    SNES_DC(0, 1, 2),
    SNES_DC(0, 1, 2),
    SNES_DC(0, 1, 2),
    SNES_DC(0, 0, 2),
    SNES_DC(0, 0, 1),
    SNES_DC(0, 0, 1),
};

static void paint_hud_band(void)
{
    u8 r;
    for (r = 0; r < BAND_ROWS; ++r)
        snesSpanFill((u16)((u16)(BAND_Y0 + r) * SNES_FB_STRIDE),
                     SNES_HUD_W, band_ramp[r]);
}

static const char *prompt_text(void)
{
    switch (ui) {
    case UI_RESULT:       return (g_duel.result > 0) ? "YOU WIN  A:AGAIN"
                                                     : "YOU LOSE A:AGAIN";
    case UI_HAND:         return "A:PLAY  X:FIGHT";
    case UI_PLACE:        return "A:ATK  X:DEF  B:X";
    case UI_EQUIP_TARGET: return "A:EQUIP  B:BACK";
    case UI_ATTACKER:     return "A:PICK  ST:END";
    case UI_DEFENDER:     return "A:HIT  X:DIRECT";
    case UI_COM:          return "OPPONENT'S TURN";
    default:              return "A:CONTINUE";      /* not reached */
    }
}

/* ── What the cursor is on ───────────────────────────────────────────────── */

/* THE NAME ROW DESCRIBES ONE CARD AND THE STATE DECIDES WHICH.
 *
 * In hand it is the card under the cursor; once one is picked up it stays the
 * card being played, because that is the thing the player is still deciding
 * about; in the battle phase it is whichever monster the cursor is over, on
 * either side of the board.
 *
 * The answer is a FACE id and not a card id, so a set monster names itself
 * "FACE DOWN" for free -- the face sheet's last page is the card back and the
 * name table is indexed by the same id, so the two cannot disagree.  The real
 * card id comes back separately, and only when the player is entitled to see
 * it, which is what gates the stat row. */
static u8 focus_card(u8 *face)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    const Msx2Side *com = &g_duel.side[MSX2_OWNER_COM];
    u8 card;

    *face = SNES_CARD_NONE_FACE;
    switch (ui) {
    case UI_HAND:
        card = you->hand[cursor];
        break;
    case UI_PLACE:
    case UI_EQUIP_TARGET:
        card = you->hand[chosen];
        break;
    case UI_ATTACKER:
        card = you->field[cursor];
        if (card != MSX2_CARD_NONE && !you->faceup[cursor]) {
            *face = SNES_CARD_BACK;
            return MSX2_CARD_NONE;
        }
        break;
    case UI_DEFENDER:
        card = com->field[cursor];
        if (card != MSX2_CARD_NONE && !com->faceup[cursor]) {
            *face = SNES_CARD_BACK;
            return MSX2_CARD_NONE;
        }
        break;
    default:
        return MSX2_CARD_NONE;
    }
    if (card == MSX2_CARD_NONE) return MSX2_CARD_NONE;
    *face = card;
    return card;
}

/* The ATK and DEF the row prints: the card's own while it is in hand, the
 * SLOT's once it is on the board, so an equipped monster reads the number the
 * battle will actually use rather than the one printed on the card. */
static u8 focus_stats(u8 card, u16 *atk, u16 *def)
{
    if (card == MSX2_CARD_NONE || !Msx2_IsMonster(card)) return 0;
    if (ui == UI_ATTACKER) {
        *atk = (u16)Msx2_FieldAtk(MSX2_OWNER_PLAYER, cursor);
        *def = (u16)Msx2_FieldDef(MSX2_OWNER_PLAYER, cursor);
    } else if (ui == UI_DEFENDER) {
        *atk = (u16)Msx2_FieldAtk(MSX2_OWNER_COM, cursor);
        *def = (u16)Msx2_FieldDef(MSX2_OWNER_COM, cursor);
    } else {
        *atk = Msx2_CardAtk(card);
        *def = Msx2_CardDef(card);
    }
    return 1;
}

/* ── The sprite layer ────────────────────────────────────────────────────── */

/* THE WHOLE HUD IS REBUILT EVERY FRAME, in either view.
 *
 * A sprite list is a hundred and thirty bytes of OAM shadow and a few dozen
 * stores; a board render is four thousand scanlines.  Rebuilding costs nothing
 * measurable next to that, and a list that is never patched cannot keep a
 * sprite belonging to a screen the player has left -- which is the failure the
 * top view would otherwise produce on every entry, since the two views share
 * the same twenty card sprites.
 *
 * IT IS BUILT FROM THE FRONT BACKWARDS.  Every sprite in this port is priority
 * 3, so between two that overlap the one with the LOWER OAM index is the one
 * seen: the text goes in first, then the plates it sits on, then the cursor,
 * and the cards last of all. */
static void build_objects(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    u8 row, col, i;
    u8 face = SNES_CARD_NONE_FACE;
    const u8 card = focus_card(&face);
    u16 atk = 0, def = 0;
    const u8 has_stats = focus_stats(card, &atk, &def);
    /* A message pre-empts the name, because it is the thing that just
     * happened; the name is back the moment it expires. */
    const char *name = message ? message
                     : (face != SNES_CARD_NONE_FACE) ? snesCardName(face)
                     : prompt_text();

    snesObjBegin();

    if (top_view) {
        u8 crow = 0;
        const u8 cslot = cursor_board_slot(&crow);

        snesObjText(8, TOP_MSG_Y, name);
        if (has_stats) {
            snesObjIcon(TOP_STAT_X, TOP_MSG_Y, SNES_SPR_ICON_ATK);
            snesObjNum(TOP_STAT_X + STAT_NUM_DX, TOP_MSG_Y, atk, 4);
            snesObjIcon(TOP_STAT_X + TOP_STAT_GAP, TOP_MSG_Y,
                        SNES_SPR_ICON_DEF);
            snesObjNum(TOP_STAT_X + TOP_STAT_GAP + STAT_NUM_DX, TOP_MSG_Y,
                       def, 4);
        }
        snesObjLifePanel(LP_YOU_X, LP_Y, 0, (u16)you->lp, MSX2_START_LP);
        snesObjLifePanel(LP_COM_X, LP_Y, 1,
                         (u16)g_duel.side[MSX2_OWNER_COM].lp, MSX2_START_LP);

        if (cslot != MSX2_SLOT_NONE && cslot < SNES_COLS)
            snesObjBox(SNES_TOP_X0 + cslot * SNES_TOP_CELL,
                       SNES_TOP_Y0 + crow * SNES_TOP_CELL,
                       SNES_TOP_CELL, SNES_TOP_CELL);

        /* The field, far row first, exactly the order the perspective board
         * draws it in -- so walking up and back down does not reorder a thing
         * the player was looking at. */
        for (row = 0; row < SNES_ROWS; ++row) {
            const u8 owner = (row <= SNES_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                           : MSX2_OWNER_PLAYER;
            const u8 support = (row == SNES_ROW_COM_SUPPORT ||
                                row == SNES_ROW_YOU_SUPPORT);
            const Msx2Side *sd = &g_duel.side[owner];
            for (col = 0; col < SNES_COLS; ++col) {
                const u8 c = support ? sd->equip_field[col] : sd->field[col];
                const u8 f = face_of(c, support ? 1 : sd->faceup[col]);
                if (f == SNES_CARD_NONE_FACE) continue;
                snesObjCard(TOP_CARD_X(col), TOP_CARD_Y(row),
                            (u8)(row * SNES_COLS + col), f);
            }
        }
    } else {
        /* The name of the card the cursor is on, and under it what that card
         * is worth in a fight.  When there are no stats to show -- a support
         * card, a set monster, the opponent's turn -- the stat row carries the
         * button legend instead, so the row is never empty and the legend is
         * never sitting in the name's place. */
        snesObjText(8, NAME_Y, name);
        if (has_stats) {
            snesObjIcon(STAT_ATK_X, STAT_Y, SNES_SPR_ICON_ATK);
            snesObjNum(STAT_ATK_X + STAT_NUM_DX, STAT_Y, atk, 4);
            snesObjIcon(STAT_DEF_X, STAT_Y, SNES_SPR_ICON_DEF);
            snesObjNum(STAT_DEF_X + STAT_NUM_DX, STAT_Y, def, 4);
        } else {
            snesObjText(STAT_ATK_X, STAT_Y, prompt_text());
        }
        snesObjLifePanel(LP_YOU_X, LP_Y, 0, (u16)you->lp, MSX2_START_LP);
        snesObjLifePanel(LP_COM_X, LP_Y, 1,
                         (u16)g_duel.side[MSX2_OWNER_COM].lp, MSX2_START_LP);

        /* THE HAND IS NOT DRAWN IN THE TOP VIEW, because the top view is the
         * board seen from above and the hand is not on the board.  Here it is
         * five 32x32 sprites -- the card art at the console's own resolution,
         * which is twice what the bitmap band could show it at -- sitting at
         * the TOP of the band, with the words that describe it underneath. */
        /* THE HAND IS THE ONE PLACE A CARD GETS A PALETTE TO ITSELF.  Five
         * cards against seven card palettes is a slot each, so a hand card is
         * quantised against nothing but its own painting instead of sharing
         * fifteen entries with the ten faces nearest it in colour -- which is
         * what made the cards read as posterised.  The top view cannot have it:
         * it shows twenty at once. */
        snesObjCardHiRes(1);
        for (i = 0; i < MSX2_HAND; ++i) {
            const s16 x = (s16)(HAND_X0 + i * HAND_PITCH);
            const u8 hcard = you->hand[i];
            const u8 selected = (ui == UI_HAND)
                              ? (cursor == i)
                              : (chosen == i && ui != UI_ATTACKER &&
                                 ui != UI_DEFENDER && ui != UI_COM);
            if (hcard == MSX2_CARD_NONE) continue;
            if (selected) snesObjBox(x - 4, HAND_Y - 4, 40, 40);
            snesObjCard(x, HAND_Y, i, face_of(hcard, 1));
        }
    }

    snesObjEnd();
}

/* ── The measurement fixture ─────────────────────────────────────────────── */

/* R fills the board from the two decks: five monsters a side, the support rows
 * holding real support cards, one monster set face down.
 *
 * It is not a cheat and it is not placeholder art -- every id it uses is one
 * the duel could produce, dealt off the shuffled deck the duel started with --
 * it is the port's PERFORMANCE FIXTURE and the harness's card-identification
 * fixture, and it exists because both need a full board that is the same board
 * every run.  A duel played to that state instead would put a different board
 * under every measurement, because how long a render takes changes how many
 * game frames fit in a scripted run, which changes the duel. */
static void fixture_board(void)
{
    Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    Msx2Side *com = &g_duel.side[MSX2_OWNER_COM];
    u8 col;

    for (col = 0; col < MSX2_FIELD; ++col) {
        const int a = waifu_deck_draw(&you->deck);
        const int b = waifu_deck_draw(&com->deck);
        you->field[col] = (a >= 0) ? (u8)a : MSX2_CARD_NONE;
        com->field[col] = (b >= 0) ? (u8)b : MSX2_CARD_NONE;
        /* One monster is SET, and it proves the back face is a face like any
         * other: same sheet, same page, same walker.  It is in the player's own
         * near row because that is where a card is large enough on screen for
         * the harness to identify which face it is. */
        you->faceup[col] = (col == 3) ? 0 : 1;
        com->faceup[col] = 1;
        you->equip_field[col] = (u8)(WAIFU_SUPPORT_EQUIP_CARD_ID + col + 1);
        com->equip_field[col] = (u8)(WAIFU_SUPPORT_EQUIP_CARD_ID + col);
    }
    touch_board(2);
}

/* ── The player's turn ───────────────────────────────────────────────────── */

static void move_cursor(u8 count, u8 board)
{
    const u16 down = padsDown(0);
    u8 moved = 0;

    if (down & KEY_LEFT)  { cursor = (u8)((cursor + count - 1) % count); moved = 1; }
    if (down & KEY_RIGHT) { cursor = (u8)((cursor + 1) % count); moved = 1; }
    if (!moved) return;
    if (board) touch_board(4);
}

static void begin_battle_phase(void)
{
    g_duel.phase = MSX2_PHASE_BATTLE;
    ui = UI_ATTACKER;
    cursor = 0;
    touch_board(8);
}

static void end_player_turn(void)
{
    Msx2_EndTurn();
    ui = UI_COM;
    com_delay = 12;
    touch_board(8);
}

static void place_chosen(u8 defense)
{
    const u8 card = g_duel.side[MSX2_OWNER_PLAYER].hand[chosen];
    if (Msx2_IsSupport(card)) {
        if (!Msx2_PlaySupport(MSX2_OWNER_PLAYER, chosen, cursor))
            say("CANNOT PLAY IT");
    } else if (!Msx2_PlaceMonster(MSX2_OWNER_PLAYER, chosen, cursor,
                                  defense ? TRUE : FALSE)) {
        say("CANNOT PLACE IT");
    }
    Msx2_ClearActionEvent();
    ui = UI_HAND;
    cursor = chosen;
    touch_board(20);
}

static void step_player(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    const u16 down = padsDown(0);

    switch (ui) {
    case UI_HAND:
        move_cursor(MSX2_HAND, 0);
        if (down & KEY_A) {
            const u8 card = you->hand[cursor];
            if (card == MSX2_CARD_NONE || you->used[cursor]) {
                say("NOTHING THERE");
            } else if (Msx2_IsSupport(card)) {
                const u8 kind = Msx2_SupportKind(card);
                chosen = cursor;
                if (kind == MSX2_SUP_EQUIP || kind == MSX2_SUP_GUARD) {
                    ui = UI_EQUIP_TARGET;
                    cursor = 0;
                    touch_board(8);
                } else {
                    cursor = MSX2_SLOT_NONE;
                    place_chosen(0);
                }
            } else {
                chosen = cursor;
                ui = UI_PLACE;
                cursor = Msx2_FirstFreeSlot(MSX2_OWNER_PLAYER);
                if (cursor == MSX2_SLOT_NONE) cursor = 0;
                touch_board(8);
            }
        }
        if (down & KEY_X) begin_battle_phase();
        if (down & KEY_START) end_player_turn();
        break;

    case UI_PLACE:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) place_chosen(0);
        else if (down & KEY_X) place_chosen(1);
        else if (down & KEY_B) {
            ui = UI_HAND;
            cursor = chosen;
            touch_board(8);
        }
        break;

    case UI_EQUIP_TARGET:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) place_chosen(0);
        else if (down & KEY_B) {
            ui = UI_HAND;
            cursor = chosen;
            touch_board(8);
        }
        break;

    case UI_ATTACKER:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) {
            if (!Msx2_IsMonster(you->field[cursor])) {
                say("NO MONSTER THERE");
            } else if (you->attacked[cursor]) {
                say("ALREADY ATTACKED");
            } else if (Msx2_FirstTurnAttackLocked()) {
                say("NOT ON TURN ONE");
            } else {
                chosen = cursor;                /* reused: the attacker slot */
                ui = UI_DEFENDER;
                cursor = Msx2_FirstLiveSlot(MSX2_OWNER_COM);
                if (cursor == MSX2_SLOT_NONE) cursor = 0;
                touch_board(8);
            }
        }
        if (down & KEY_X) {
            /* Position switch: the one main-phase action still legal in
             * battle, and the rules model exposes it as its own call. */
            if (Msx2_ChangePosition(MSX2_OWNER_PLAYER, cursor)) touch_board(12);
            else say("CANNOT TURN IT");
        }
        if (down & KEY_START) end_player_turn();
        break;

    case UI_DEFENDER: {
        const u8 direct = (Msx2_LiveMonsterCount(MSX2_OWNER_COM) == 0) ||
                          ((down & KEY_X) != 0);
        move_cursor(MSX2_FIELD, 1);
        if ((down & KEY_A) || direct) {
            const u8 target = direct ? MSX2_SLOT_NONE : cursor;
            if (!Msx2_Attack(MSX2_OWNER_PLAYER, chosen, target))
                say("ILLEGAL ATTACK");
            Msx2_ClearActionEvent();
            ui = UI_ATTACKER;
            cursor = chosen;
            touch_board(24);
        }
        if (down & KEY_B) {
            ui = UI_ATTACKER;
            cursor = chosen;
            touch_board(8);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Entry points ────────────────────────────────────────────────────────── */

void snesDuelEnter(void)
{
    Msx2_DuelInit(0x51E5u, MSX2_STORY_NONE);

    /* TURN_START runs first for either side, and the rules own it; the UI
     * joins in when the player's own main phase begins. */
    ui = UI_COM;
    cursor = 0;
    chosen = 0;
    com_delay = 4;
    motion = 30;
    lift_phase = 0;
    message = NULL;
    message_timer = 0;

    top_view = 0;
    snesVideoSetView(SNES_VIEW_BOARD);
    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, 0, 0);
    snesVideoSetBoardRes(SNES_RES_STILL);
    set_viewport();
    snesVideoClear(BACKDROP);
    /* Nothing draws into the bitmap's HUD band per frame any more -- the HUD is
     * sprites -- so the plate under the two text rows is painted and the band
     * uploaded exactly once, here. */
    paint_hud_band();
    snesVideoHudDirty();
    build_objects();
    render();
}

void snesDuelFrame(void)
{
    const u16 down = padsDown(0);
    u8 res = snesVideoBoardRes();

    if (down & KEY_R) fixture_board();
    if (down & KEY_L) {
        autoplay ^= 1;
        say(autoplay ? "DEMO ON" : "DEMO OFF");
    }
    if (down & KEY_SELECT) {
        show_cards ^= 1;
        touch_board(2);
    }
    if (down & KEY_Y) {
        force_moving ^= 1;
        snesVideoClear(BACKDROP);
        paint_hud_band();
    snesVideoHudDirty();
        touch_board(2);
    }

    /* UP walks up into the tactical top view, DOWN walks back down.  Neither
     * costs a redraw: the perspective board stays in the bitmap untouched
     * while the top view is up, so coming back down puts the frame the player
     * left straight back on screen. */
    if ((down & KEY_UP) && !top_view) {
        top_view = 1;
        snesVideoSetView(SNES_VIEW_TOP);
    } else if ((down & KEY_DOWN) && top_view) {
        top_view = 0;
        snesVideoSetView(SNES_VIEW_BOARD);
    }

    if (message_timer) {
        if (--message_timer == 0) {
            message = NULL;
        }
    }

    if (g_duel.result != 0 && ui != UI_RESULT) {
        ui = UI_RESULT;
        /* The outcome is the PROMPT, not a message: a message expires, and the
         * one line that must still be readable a minute after the duel ended is
         * which way it went. */
        message = 0;
        message_timer = 0;
        touch_board(8);
    }

    if (ui == UI_RESULT) {
        /* The duel is over and there is no title screen to go back to yet, so
         * A starts another one.  M5 gives this an exit. */
        if (down & (KEY_A | KEY_B)) snesDuelEnter();
    } else if (!autoplay && g_duel.turn_owner == MSX2_OWNER_PLAYER &&
               (g_duel.phase == MSX2_PHASE_MAIN ||
                g_duel.phase == MSX2_PHASE_BATTLE)) {
        /* The player's own phases are the only ones the rules model does not
         * drive itself; everything else -- including the player's draw at
         * TURN_START -- goes through Msx2_DuelStep. */
        if (ui == UI_COM) {
            ui = (g_duel.phase == MSX2_PHASE_MAIN) ? UI_HAND : UI_ATTACKER;
            cursor = 0;
            touch_board(12);
        }
        step_player();
    } else {
        ui = UI_COM;
        if (com_delay) {
            --com_delay;
        } else {
            com_delay = 6;
            Msx2_DuelStep();
            Msx2_ClearActionEvent();
            touch_board(12);
        }
    }

    /* A CARD IN THE AIR IS SOMETHING CHANGING.  While the player is choosing
     * where to put a card, the card hovers over the slot and bobs, so the board
     * stays in the moving resolution and keeps being redrawn -- which is what
     * the moving resolution is for, and what makes the difference between a
     * card that is being held and one that has been dropped visible at all. */
    if (ui == UI_PLACE || ui == UI_EQUIP_TARGET) touch_board(2);

    /* THE BOARD DROPS TO THE MOVING RESOLUTION WHILE SOMETHING IS CHANGING and
     * returns to the still one when it settles.  That is what the two
     * resolutions are for: a quarter of the pixels while the picture is in
     * flux, the full board once it is worth looking at.  A still frame also
     * takes three vblanks to upload and a redraw restarts that upload, so the
     * settled board is only redrawn once the last one is entirely on screen. */
    /* THE TOP VIEW RENDERS NOTHING.  Its table is a resident tilemap and its
     * cards are sprites, so while it is up there is no board to draw, no
     * resolution to pick and no bitmap to upload -- the duel runs at sixty
     * fields a second and the vblank goes to the card sprites instead. */
    if (top_view) {
        build_objects();
        motion = 0;
        goto stamp;
    }

    if (motion || force_moving) {
        if (motion) --motion;
        lift_phase += 8;
        if (res != SNES_RES_MOVING) {
            snesVideoSetBoardRes(SNES_RES_MOVING);
            set_viewport();
            res = SNES_RES_MOVING;
        }
        board_dirty = 1;
    } else if (res != SNES_RES_STILL && snesVideoPresentDone()) {
        snesVideoSetBoardRes(SNES_RES_STILL);
        set_viewport();
        res = SNES_RES_STILL;
        board_dirty = 1;
    }

    if (board_dirty && (res == SNES_RES_MOVING || snesVideoPresentDone()))
        render();
    build_objects();

stamp:
    g_stamp.duel_turn = g_duel.turns;
    g_stamp.lp_player = (u16)g_duel.side[MSX2_OWNER_PLAYER].lp;
    g_stamp.lp_com = (u16)g_duel.side[MSX2_OWNER_COM].lp;
    g_stamp.duel_result = (u16)(s16)g_duel.result;
    g_stamp.ui = ui;
    g_stamp.cursor = cursor;
    {
        /* How many monsters the player has standing.  It is in the stamp
         * because it is what says a placement actually reached the rules, and
         * the harness cannot read a struct whose layout is 816-tcc's business.
         */
        u8 i, n = 0;
        for (i = 0; i < MSX2_FIELD; ++i)
            if (g_duel.side[MSX2_OWNER_PLAYER].field[i] != MSX2_CARD_NONE) ++n;
        g_stamp.field_cards = n;
        g_stamp.phase = g_duel.phase;
        g_stamp.turn_owner = g_duel.turn_owner;
    }
}
