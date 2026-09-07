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
 *  What is on screen: the textured slab with the twenty slots' cards on it, a
 *  cursor marker under the slot in question, the card the player is holding in
 *  the air over its target through the convex-quad path, and the HUD band --
 *  life points, the phase prompt, and the five cards in hand, all of them
 *  drawn into the chunky framebuffer rather than onto a layer, because Mode 7
 *  has exactly one layer and the board is using it.
 *
 *  The two resolutions are what the whole video model exists for: the board
 *  drops to 64x40 texels while something is moving and returns to 128x80 when
 *  it settles, and the PPU does the doubling, so a moving board costs a quarter
 *  of the pixels and nothing extra to display.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_textures.h"
#include "snes_cards.h"
#include "snes_text.h"
#include "snes_stamp.h"
#include "msx2_duel.h"

/* The arena camera.  One unit is one slot pitch; the board runs z = -2..2, so
 * a camera one unit up and three back looks along the slab with its far edge
 * near the horizon. */
#define CAM_HEIGHT   ((s16)256)         /* 1.0 */
#define CAM_Z        ((s16)(-3 * 256))

/* The surround: the texel the board floats on where the slab does not reach.
 * Direct colour is BBGGGRRR, so this is a dark warm brown -- the arena's
 * shadowed ground, not a debug colour. */
#define BACKDROP     SNES_DC(1, 1, 0)
#define INK          SNES_DC(7, 7, 3)
#define SHADOW       SNES_DC(0, 0, 0)
#define MARK_YOU     SNES_DC(7, 6, 1)   /* the cursor: hot gold */
#define MARK_COM     SNES_DC(7, 2, 1)   /* what the opponent is acting on */
#define HAND_SEL     SNES_DC(7, 7, 2)

/* The card the player is holding hovers over the slot it is going into. */
/* The lift and the lean, and their sum is the constraint: the camera is ONE
 * world unit up, so a far edge raised anywhere near that projects at the
 * horizon and the quad stretches off the top of the screen.  Two thirds of the
 * camera's height is as far as the card leans back. */
#define HELD_LIFT    ((s16)32)          /* 0.125 world units above the board */
#define HELD_TILT    ((s16)64)          /* the far edge, 0.25 units higher */

/* The HUD band's three rows: life points, the prompt, and the hand. */
#define HUD_LP_Y     (SNES_HUD_ROW + 0)
#define HUD_MSG_Y    (SNES_HUD_ROW + 9)
#define HUD_HAND_Y   (SNES_HUD_ROW + 18)
#define HAND_PITCH   24                 /* five 16-texel cards across 128 */
#define HAND_X0      4

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
static u8  board_dirty = 0, hud_dirty = 0;
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
    /* An eighth of the band is sky.  Lower would bury the board's far edge in
     * the backdrop; higher wastes rows on ground beyond the slab. */
    cam.horizon = (s16)(vp.h >> 3);
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
    hud_dirty = 1;
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

/* The five cards in hand, drawn into the band as 16-texel faces.
 *
 * They are framebuffer texels and not sprites, and the trade is deliberate: a
 * sprite hand needs a second conversion of every card into 4bpp tiles, ten
 * kilobytes of OBJ VRAM and an upload whenever the hand changes, to show the
 * same picture the card sheet already holds.  Here a hand card is sixteen
 * calls to the span walker the board already uses, the band is uploaded only
 * when it is dirty, and the sprite budget stays free for what actually needs
 * to move independently of the bitmap. */
static void draw_hand(void)
{
    const Msx2Side *s = &g_duel.side[MSX2_OWNER_PLAYER];
    u8 i, y;

    for (i = 0; i < MSX2_HAND; ++i) {
        const u8 x = (u8)(HAND_X0 + i * HAND_PITCH);
        const u8 card = s->hand[i];
        const u8 selected = (ui == UI_HAND) ? (cursor == i)
                                            : (chosen == i && ui != UI_ATTACKER
                                               && ui != UI_DEFENDER
                                               && ui != UI_COM);
        if (card == MSX2_CARD_NONE) continue;
        for (y = 0; y < SNES_CARD_TEXELS; ++y)
            snesSpanCard((u16)((HUD_HAND_Y + y) * SNES_FB_STRIDE + x),
                         SNES_CARD_TEXELS,
                         (u16)(snesCardPage(face_of(card, 1)) | (y << 4)),
                         0, 256);
        if (selected || s->used[i]) {
            /* A frame around the card: bright for the one under the cursor,
             * dark for one already spent this turn. */
            const u8 c = selected ? HAND_SEL : SHADOW;
            const u16 top = (u16)((HUD_HAND_Y - 1) * SNES_FB_STRIDE + x - 1);
            const u16 bot = (u16)((HUD_HAND_Y + SNES_CARD_TEXELS)
                                  * SNES_FB_STRIDE + x - 1);
            snesSpanFill(top, SNES_CARD_TEXELS + 2, c);
            snesSpanFill(bot, SNES_CARD_TEXELS + 2, c);
            for (y = 0; y < SNES_CARD_TEXELS; ++y) {
                snesSpanFill((u16)((HUD_HAND_Y + y) * SNES_FB_STRIDE + x - 1),
                             1, c);
                snesSpanFill((u16)((HUD_HAND_Y + y) * SNES_FB_STRIDE + x
                                   + SNES_CARD_TEXELS), 1, c);
            }
        }
    }
}

static void draw_hud(void)
{
    u16 y;

    /* The panel under the board: the same arena sandstone as the ground,
     * sampled 1:1 from a different part of the texture so it does not read as
     * more floor. */
    for (y = 0; y < SNES_HUD_H; ++y) {
        const u16 v = (y + SNES_FLOOR_TEXELS_PER_UNIT)
                      & (SNES_FLOOR_PATTERN_H - 1);
        snesSpanFloor((u16)((SNES_HUD_ROW + y) * SNES_FB_STRIDE), SNES_HUD_W,
                      (u16)((v << 8) | 0), 0, 256);
    }

    /* Sixteen glyphs is the whole line, so the two sides are laid out to fit
     * it exactly: three letters and four digits each, with a glyph of air
     * between them and one at the end. */
    snesTextAt(0, HUD_LP_Y, "YOU", INK, SHADOW);
    snesTextNum(24, HUD_LP_Y, (u16)g_duel.side[MSX2_OWNER_PLAYER].lp, 4,
                INK, SHADOW);
    snesTextAt(64, HUD_LP_Y, "COM", INK, SHADOW);
    snesTextNum(88, HUD_LP_Y, (u16)g_duel.side[MSX2_OWNER_COM].lp, 4,
                INK, SHADOW);
    /* One ink for every line of the band.  A dimmer prompt looked better and
     * cost more than it was worth: the harness reads this text back off the
     * screen and matches it against the font, and a second ink means a second
     * threshold between "letter" and "sandstone" -- which the sandstone wins. */
    snesTextAt(0, HUD_MSG_Y, message ? message : prompt_text(), INK, SHADOW);
    draw_hand();

    snesVideoHudDirty();
    hud_dirty = 0;
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
    hud_dirty = 1;
}

/* ── The player's turn ───────────────────────────────────────────────────── */

static void move_cursor(u8 count, u8 board)
{
    const u16 down = padsDown(0);
    u8 moved = 0;

    if (down & KEY_LEFT)  { cursor = (u8)((cursor + count - 1) % count); moved = 1; }
    if (down & KEY_RIGHT) { cursor = (u8)((cursor + 1) % count); moved = 1; }
    if (!moved) return;
    hud_dirty = 1;
    if (board) touch_board(4);
}

static void begin_battle_phase(void)
{
    g_duel.phase = MSX2_PHASE_BATTLE;
    ui = UI_ATTACKER;
    cursor = 0;
    touch_board(8);
    hud_dirty = 1;
}

static void end_player_turn(void)
{
    Msx2_EndTurn();
    ui = UI_COM;
    com_delay = 12;
    touch_board(8);
    hud_dirty = 1;
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
    hud_dirty = 1;
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
            hud_dirty = 1;
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
            hud_dirty = 1;
        }
        break;

    case UI_EQUIP_TARGET:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) place_chosen(0);
        else if (down & KEY_B) {
            ui = UI_HAND;
            cursor = chosen;
            touch_board(8);
            hud_dirty = 1;
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
                hud_dirty = 1;
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
            hud_dirty = 1;
        }
        if (down & KEY_B) {
            ui = UI_ATTACKER;
            cursor = chosen;
            touch_board(8);
            hud_dirty = 1;
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

    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, 0, 0);
    snesVideoSetBoardRes(SNES_RES_STILL);
    set_viewport();
    snesVideoClear(BACKDROP);
    draw_hud();
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
        /* The panel does not move with the band, but clearing the buffer wipes
         * it, so it is redrawn and re-uploaded with the change. */
        snesVideoClear(BACKDROP);
        hud_dirty = 1;
        touch_board(2);
    }

    if (message_timer) {
        if (--message_timer == 0) {
            message = NULL;
            hud_dirty = 1;
        }
    }

    if (g_duel.result != 0 && ui != UI_RESULT) {
        ui = UI_RESULT;
        /* The outcome is the PROMPT, not a message: a message expires, and the
         * one line that must still be readable a minute after the duel ended is
         * which way it went. */
        message = 0;
        message_timer = 0;
        hud_dirty = 1;
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
            hud_dirty = 1;
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
            hud_dirty = 1;
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

    if (hud_dirty) draw_hud();
    if (board_dirty && (res == SNES_RES_MOVING || snesVideoPresentDone()))
        render();

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
