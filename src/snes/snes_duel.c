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
 *  TWO VIEWS OF ONE BOARD: UP opens inspection and B returns to the hand.
 *
 *  The board view is the textured slab in perspective: twenty slots with their
 *  cards lying on them, a marker under the slot in question, and the card being
 *  played held in the air over its target through the convex-quad path.  Its
 *  128x80 board rectangle is used at rest and during motion.  Motion has
 *  a twelve-vblank minimum interval, which keeps the full-detail picture
 *  stable while the presenter uploads it in chunks.  Card-heavy frames can
 *  take longer than that renderer/upload floor without changing resolution.
 *
 *  The top view is the same board as a flat table: Mode 3, the full 256x224,
 *  no software rendering at all and therefore sixty fields a second.  It is
 *  the view the other ports call the tactical top view.  UP zooms the resident 128x80 board while the hand slides out
 *  of the way; B plays the same path in reverse.
 *
 *  NOTHING IN THE HUD IS DRAWN INTO THE BITMAP any more.  Life points, the
 *  prompt, the hand and both cursors are sprites, which is what lets them stay
 *  on screen at the console's own resolution across a mode change that rewrites
 *  no VRAM at all.  See snes_obj.h.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_cardart.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_cards.h"
#include "snes_obj.h"
#include "snes_stamp.h"
#include "snes_deck.h"
#include "snes_audio.h"
#include "snes_save.h"
#include "snes_scene.h"
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
 * is what stops the ascenders touching the gradient's top rule at line 197. */
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

/* THE CAMERA MOVE IS TWENTY FIELDS AND NOT EIGHT RENDERS.
 *
 * It used to render eight live poses of the board through the general quad
 * path, each one re-baking the twenty-card board texture, and each one waiting
 * out the twelve-field motion interval on top of the thirteen fields the
 * render itself cost: the lift took the better part of seven seconds and spent
 * most of it looking identical.  Nothing is rendered here now.  The board
 * bitmap already in VRAM is the picture; the move is the board band's Mode 7
 * scale walked from 4.0 to SNES_M7_SCALE_TOP, one value a FIELD, with the hand
 * sliding off the bottom over the same curve.  See snes_video.h.
 *
 * Twenty fields is also about what the top view's twenty card sprites need to
 * reach VRAM at four 128-byte rows a vblank, so the two finish together and
 * the mode change happens with the table already complete. */
#define VIEW_ANIM_FRAMES       20
#define VIEW_HAND_OFFSET       80

enum SnesDuelUi {
    UI_HAND = 0,        /* choosing a card in hand */
    UI_PLACE,           /* choosing where a monster goes */
    UI_EQUIP_TARGET,    /* choosing the monster an equip attaches to */
    UI_ATTACKER,        /* choosing which of your monsters attacks */
    UI_DEFENDER,        /* choosing what it attacks */
    UI_COM,             /* the opponent is acting */
    UI_RESULT,
    UI_FUSE_TARGET,     /* where the chosen fusion chain lands */
    UI_CHECK,           /* one card over a Mode 3 presentation */
    UI_BATTLE_ART       /* two independently moving Mode 3 cards */
};

enum SnesViewMotion {
    VIEW_BOARD_REST = 0,
    VIEW_TO_TOP,
    VIEW_TOP_REST,
    VIEW_TO_HAND
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
static u8  view_motion = VIEW_BOARD_REST;
static u8  view_anim_frame = 0;
static u8 board_yaw = 0;
static u8 turn_frame = 0;
static u8 turn_from = 0;
static u8 turn_target = 0;
static u8 texture_faces[20];
/* THE TOP VIEW HAS A CURSOR OF ITS OWN, and it is a board position rather than
 * whatever the hand happened to be pointing at.  The table shows twenty slots
 * and every other port lets the player walk over all of them and read what is
 * lying there; carrying the hand's cursor up there instead left the marker
 * stuck on a hand slot the view does not even draw.  It is an INSPECTION
 * cursor: it names the card under it in the row below the table and A opens
 * the card check, but it does not play anything. */
static u8 top_row = SNES_ROW_YOU_MONSTER;
static u8 top_col = 0;
/* The selected hand card's small up-and-down, which is a per-FIELD thing and
 * therefore not the board's lift_phase: the board only advances on the twelve
 * field motion cadence and a bob that moves five times a second is a twitch. */
static u8 bob_phase = 0;
/* THE FUSION CHAIN, in the order the player chose it.  DOWN on the hand row
 * puts the card under the cursor into it and takes it out again; A with a
 * chain waiting carries it to the field row, which picks the slot it lands in.
 * That is the MSX2 build's gesture, and the rules model underneath is the same
 * one (Msx2_PlaceFusion). */
static u8 queue[MSX2_HAND];
static u8 queue_n = 0;
/* A card being put down FLIES THERE.  The hand sprite walks from its slot to
 * the projected centre of the board slot it is going into and the rules are
 * told only when it arrives, so the card is never in two places and never in
 * neither.  Sprites only: no board frame is rendered during the flight, so it
 * runs at sixty fields a second. */
#define FLY_FRAMES  14
static u8  fly_frame = 0;
static u8  fly_def = 0;
static u8 fly_landing = 0;
static u8 fly_face = SNES_CARD_NONE_FACE;
static s16 fly_x0 = 0, fly_y0 = 0, fly_x1 = 0, fly_y1 = 0;
/* The card check: one card, enlarged into the board bitmap, over black. */
static u8  check_face = SNES_CARD_NONE_FACE;
static u8 check_return_ui = UI_HAND;
static u8 check_return_top = 0;
static u8  check_has_stats = 0;
static u16 check_atk = 0, check_def = 0;
static u8 mode3_active = 0;
static u8 battle_frame = 0;
static u8 battle_left_face = SNES_CARD_NONE_FACE;
static u8 battle_right_face = SNES_CARD_NONE_FACE;
static u16 battle_left_atk = 0, battle_left_def = 0;
static u16 battle_right_atk = 0, battle_right_def = 0;
static u8 battle_return_ui = UI_COM;
/* How far into its entrance the result banner is. */
static u8  over_step = 0;
static u8 texture_w = 0, texture_h = 0;
static u16 motion_next_vblank = 0;
#define TURN_FRAMES 16
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

/* Y pins the board to the moving cadence.  The source stays 128x80; this
 * switch keeps the five-updates-per-second path active long enough to inspect
 * it in the harness. */
static u8  force_moving = 0;
static u8 configured_story = MSX2_STORY_NONE;
static u8 configured_story_mode = 0;

static void render(void);
static void build_objects(void);
static u8   face_of(u8 card, u8 faceup);
static u8   focus_card(u8 *face);
static u8   focus_stats(u8 card, u16 *atk, u16 *def);
static void say(const char *msg);

u8 snesDuelMode3Active(void) { return mode3_active; }

static void set_viewport(void)
{
    vp.w = SNES_STILL_W;
    vp.h = SNES_STILL_H;
    vp.origin = 0;
    vp.stride = SNES_FB_STRIDE;

    /* The focal length is half the viewport width, which is what makes the
     * floor's texture step a shift rather than a divide: 32 texels a world
     * unit over a focal length of w/2 is exactly 2 texels per pixel here. */
    cam.focal  = (s16)((u16)vp.w << 7);
    /* `du_k` is 32 / focal in Q8.8.  The focal length is deliberately one of
     * the two power-of-two values above, so spell the result as constants and
     * keep the renderer free of a 32-bit divide.  Leaving this field unset
     * makes the board's texture walk depend on the uninitialised viewport
     * storage, which looks like a bad card mapper rather than a missing
     * viewport setup. */
    vp.du_k = 128;
    /* The horizon sits a thirty-second of the band down, which puts the
     * SLAB'S FRONT WALL clear of the bottom of the board band with black
     * under it.  That black is what makes the wall read as the near face of a
     * slab standing on nothing rather than as more floor: at an eighth the
     * board ran to the last row of the band and the wall touched the HUD.  It
     * is the same proportion in both resolutions, so the two frame the board
     * identically and switching between them does not shift it. */
    cam.horizon = (s16)(vp.h >> 5);
}

/* Q8.8 smoothstep, using the SNES multiplier instead of a 32-bit product.
 * The eased value is shared by the camera and hand so the two settle together
 * at the exact frame the PPU switches to the resident top table. */
static u16 ease_frac(u8 frame, u8 total)
{
    u16 t;
    u16 t2;
    if (frame >= total) return SNES_ONE;
    t = snesUQDiv(frame, total);
    t2 = (u16)snesQMul((s16)t, (s16)t);
    return (u16)snesQMul((s16)t2, (s16)(3 * SNES_ONE - 2 * t));
}

static u16 view_anim_ease(u8 frame)
{
    return ease_frac(frame, VIEW_ANIM_FRAMES);
}

static s16 view_lerp(s16 a, s16 b, u16 t)
{
    return (s16)(a + snesQMul((s16)(b - a), (s16)t));
}

static u8 motion_frame_due(void)
{
    return (s16)(snes_vblank_count - motion_next_vblank) >= 0;
}

static void schedule_motion_frame(void)
{
    /* Keep an absolute 12-field phase.  A full board render can take longer
     * than one 12-field slot; advancing from the previous deadline lets the
     * next complete upload start as soon as it is ready instead of adding an
     * avoidable second wait after every render. */
    motion_next_vblank = (u16)(motion_next_vblank + SNES_MOTION_FIELDS);
}

/* Queue the top view's twenty card sprites.  `hand_gone` says whether the five
 * hand slots are free yet: they are OBJ slots 0..4 and the top table's first
 * row as well, so asking for the table's cards there while the hand is still
 * sliding would take the hand off the screen a card at a time.  The other
 * fifteen have nothing to wait for and start immediately, which is what gets
 * the whole table into VRAM inside the twenty fields the move takes. */
static void queue_top_cards(u8 hand_gone)
{
    u8 row, col;
    for (row = 0; row < SNES_ROWS; ++row) {
        const Msx2Side *s = &g_duel.side[row < 2 ? MSX2_OWNER_COM
                                                 : MSX2_OWNER_PLAYER];
        const u8 support = (row == SNES_ROW_COM_SUPPORT ||
                            row == SNES_ROW_YOU_SUPPORT);
        for (col = 0; col < SNES_COLS; ++col) {
            const u8 slot = (u8)(row * SNES_COLS + col);
            if (slot < SNES_SPR_CARD_PALS && !hand_gone) continue;
            snesObjQueueCard(slot,
                face_of(support ? s->equip_field[col] : s->field[col],
                        support ? 1 : s->faceup[col]), 0);
        }
    }
}

static void begin_view_transition(u8 to_top)
{
    view_motion = to_top ? VIEW_TO_TOP : VIEW_TO_HAND;
    view_anim_frame = 0;
    if (to_top) {
        /* Start the fifteen slots the hand does not own now; the board is not
         * rendered again from here, so the whole vblank is theirs. */
        queue_top_cards(0);
    } else {
        /* Mode 7 comes back on the next vblank with the board bitmap exactly
         * as the player left it and the zoom still at the top of the lift, so
         * there is nothing to render and nothing to upload. */
        top_view = 0;
        snesVideoSetBoardZoom(SNES_M7_SCALE_TOP);
        snesVideoSetView(SNES_VIEW_BOARD);
    }
}

static void finish_view_transition(u8 to_top)
{
    if (to_top) {
        top_view = 1;
        view_motion = VIEW_TOP_REST;
        snesVideoSetView(SNES_VIEW_TOP);
    } else {
        top_view = 0;
        view_motion = VIEW_BOARD_REST;
        snesVideoSetView(SNES_VIEW_BOARD);
        snesVideoSetBoardZoom(SNES_M7_SCALE_STILL);
    }
    view_anim_frame = 0;
}

/* One FIELD of the move.  Nothing is rendered and nothing is uploaded: the
 * board band's scale and the hand's y are both read off the same eased curve,
 * so the picture pushes in while the hand leaves, and the mode changes only
 * once every card the table needs is actually in VRAM. */
static void step_view_transition(void)
{
    const u8 to_top = (view_motion == VIEW_TO_TOP);
    u16 t;

    if (view_anim_frame < VIEW_ANIM_FRAMES) ++view_anim_frame;
    t = view_anim_ease(view_anim_frame);
    if (!to_top) t = (u16)(SNES_ONE - t);
    snesVideoSetBoardZoom((u16)view_lerp((s16)SNES_M7_SCALE_STILL,
                                         (s16)SNES_M7_SCALE_TOP, t));
    build_objects();
    if (view_anim_frame < VIEW_ANIM_FRAMES) return;
    if (to_top) {
        /* The hand is off the screen by now, so its five slots are free for
         * the table's first row. */
        queue_top_cards(1);
        if (!snesObjCardsReady()) return;
    }
    finish_view_transition(to_top);
    build_objects();
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
    case UI_FUSE_TARGET:
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
    if (ui != UI_PLACE && ui != UI_EQUIP_TARGET && ui != UI_FUSE_TARGET)
        return MSX2_SLOT_NONE;
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

    if (slot == MSX2_SLOT_NONE || slot >= SNES_COLS || ui == UI_FUSE_TARGET) return;
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

    if (cam.pitch || cam.yaw) {
        u8 r, c;
        u8 width = 24 - (cam.pitch * 3 >> 6);
        u8 height = 32 - (cam.pitch * 11 >> 6);
        u8 changed = width != texture_w || height != texture_h;
        for (r = 0; r < 4; ++r) {
            const Msx2Side *s = &g_duel.side[r < 2 ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER];
            u8 support = (r == 0 || r == 3);
            for (c = 0; c < 5; ++c) {
                u8 face = show_cards ? face_of(support ? s->equip_field[c] : s->field[c],
                                  support ? 1 : s->faceup[c]) : SNES_CARD_NONE_FACE;
                if (texture_faces[r * 5 + c] != face) changed = 1;
                texture_faces[r * 5 + c] = face;
            }
        }
        if (changed) {
            texture_w = width; texture_h = height;
            snesBoardTextureClear();
            for (r = 0; r < 4; ++r) for (c = 0; c < 5; ++c) {
                u8 face = texture_faces[r * 5 + c];
                if (face != SNES_CARD_NONE_FACE)
                    snesBoardTextureCard((u16)(((48 - (s16)r * 32) & 127) * 256 |
                                              ((16 + ((s16)c - 2) * 32) & 255)),
                                          face, r < 2,
                                          width, height);
            }
        }
        snesDrawCameraFloor(&vp, &cam, BACKDROP);
    }
    else snesDrawFloor(&vp, &cam, BACKDROP);
    /* The marker goes down BEFORE the cards: it covers a whole tile and a card
     * four fifths of one, so what is left of it is a rim around the card,
     * which is what makes "this slot" readable when the slot is occupied. */
    if (!cam.pitch && !cam.yaw && slot != MSX2_SLOT_NONE && slot < SNES_COLS)
        snesDrawSlotMarker(&vp, &cam, row, slot,
                           (ui == UI_DEFENDER) ? MARK_COM : MARK_YOU);
    if (show_cards) {
        if (!cam.pitch && !cam.yaw) draw_board_cards();
        if (!fly_frame) draw_held_card();
    }

    g_stamp.render_lines = (u16)((snes_vblank_count - vbl0) * 262
                                 + snesVCounter() - line0);
    snesVideoPresentRestart();
    board_dirty = 0;
}

/* ── The card check ──────────────────────────────────────────────────────── */

/* B checks the hovered hand card; A checks the top-view card. The enlarged
 * face has its own Mode 3 OBJ sheet. Closing restores the previous cursor
 * and UI, repainting the board when its bitmap becomes visible. */

static void enter_mode3_art(void)
{
    /* The card presentation is BG1 8bpp art and BG2 text: see snes_cardart.h.
     * Nothing the sprite layer holds is touched, so the top view and the
     * hand's card cache are exactly where they were when this closes. */
    snesCardArtEnter(ui == UI_CHECK);
    if (ui == UI_CHECK) {
        snesCardArtCheck(check_face, check_has_stats, check_atk, check_def);
    } else {
        snesCardArtClear();
        snesCardArtTextClear();
        if (battle_left_face != SNES_CARD_NONE_FACE) {
            snesCardArtLoad(0, battle_left_face);
            snesCardArtPlace(0, SNES_CARDART_COL_L, battle_left_face);
            snesCardArtStats(SNES_CARDART_COL_L, battle_left_face,
                             battle_left_atk, battle_left_def);
        }
        if (battle_right_face != SNES_CARD_NONE_FACE) {
            snesCardArtLoad(1, battle_right_face);
            snesCardArtPlace(1, SNES_CARDART_COL_R, battle_right_face);
            snesCardArtStats(SNES_CARDART_COL_R, battle_right_face,
                             battle_right_atk, battle_right_def);
        }
        snesCardArtReveal(0);
    }
    snesCardArtVblank();
    mode3_active = 1;
    setScreenOn();
}

static void leave_mode3_art(void)
{
    setScreenOff();
    mode3_active = 0;
    snesVideoInitDuel();
    snesObjInit();
    snesVideoSetBoardZoom(SNES_M7_SCALE_STILL);
    snesVideoSetView(top_view ? SNES_VIEW_TOP : SNES_VIEW_BOARD);
    render();
    snesVideoRestartHdma();
    setScreenOn();
}

static void begin_check(void)
{
    u8 face = SNES_CARD_NONE_FACE;
    const u8 card = focus_card(&face);

    if (face == SNES_CARD_NONE_FACE || face == SNES_CARD_BACK) {
        say("NOTHING TO CHECK");
        return;
    }
    check_return_ui = ui;
    check_return_top = top_view;
    check_face = face;
    check_has_stats = focus_stats(card, &check_atk, &check_def);
    snesAudioSfx(SNES_SFX_CONFIRM_ALT);
    top_view = 0;
    view_motion = VIEW_BOARD_REST;
    snesVideoSetView(SNES_VIEW_BOARD);
    snesVideoSetBoardZoom(SNES_M7_SCALE_STILL);
    ui = UI_CHECK;
    message = NULL;
    message_timer = 0;
    enter_mode3_art();
}

static void end_check(void)
{
    ui = check_return_ui;
    top_view = check_return_top;
    view_motion = top_view ? VIEW_TOP_REST : VIEW_BOARD_REST;
    snesVideoSetView(top_view ? SNES_VIEW_TOP : SNES_VIEW_BOARD);
    check_face = SNES_CARD_NONE_FACE;
    check_has_stats = 0;
    mode3_active = 0;
    battle_frame = 0;
    touch_board(4);
    leave_mode3_art();
}

static void begin_battle_art(u8 return_ui)
{
    battle_left_face = face_of(g_duel.last_attacker_card, 1);
    battle_right_face = face_of(g_duel.last_defender_card, 1);
    /* The figures the battle was fought with: the attacker's ATK as the
     * rules used it, and the defender's ATK or DEF according to its stance,
     * so the plate shows what actually decided the clash. */
    battle_left_atk = (u16)g_duel.last_battle.attacker_atk;
    battle_left_def = (battle_left_face < SNES_CARD_BACK)
                    ? Msx2_CardDef(battle_left_face) : 0;
    if (g_duel.last_battle.defender_passive) {
        battle_right_atk = (battle_right_face < SNES_CARD_BACK)
                         ? Msx2_CardAtk(battle_right_face) : 0;
        battle_right_def = (u16)g_duel.last_battle.defender_value;
    } else {
        battle_right_atk = (u16)g_duel.last_battle.defender_value;
        battle_right_def = (battle_right_face < SNES_CARD_BACK)
                         ? Msx2_CardDef(battle_right_face) : 0;
    }
    battle_return_ui = return_ui;
    battle_frame = 0;
    ui = UI_BATTLE_ART;
    enter_mode3_art();
}

static void end_battle_art(void)
{
    ui = battle_return_ui;
    battle_left_face = battle_right_face = SNES_CARD_NONE_FACE;
    Msx2_ClearActionEvent();
    touch_board(24);
    leave_mode3_art();
}

/* ── The HUD band ────────────────────────────────────────────────────────── */

/* THE BAND IS BLACK BITMAP AND NOTHING ELSE.
 *
 * The hand and the two rows of words under it are sprites, and the blue plate
 * they sit on is the backdrop tinted per scanline by an HDMA channel -- see
 * snes_m7fb.c.  So nothing is painted into the bitmap's HUD rows at all: they
 * are left transparent, which is exactly what lets the plate under them be a
 * five-bit-a-channel gradient instead of the four blues direct colour has.
 *
 * The band is still UPLOADED, once, because transparent means the bitmap has
 * to actually hold zeroes in VRAM and the clear only puts them in WRAM. */

static const char *prompt_text(void)
{
    switch (ui) {
    /* THE RESULT SAYS NOTHING HERE.  It is a banner of sixteen-pixel letters
     * that flies in from off the left edge -- see build_objects -- and the row
     * under the board printing the same words in eight-pixel ones beside a
     * button legend was the banner competing with itself. */
    case UI_RESULT:       return "";
    case UI_CHECK:        return "B:BACK";
    case UI_HAND:         return "A:PLAY X:FIGHT DN:FUSE";
    case UI_FUSE_TARGET:  return "A:FUSE  B:BACK";
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
    /* THE TOP VIEW NAMES WHAT ITS OWN CURSOR IS ON, whatever the duel state
     * underneath happens to be: the table is an inspection screen and the row
     * under it is the label for the slot the player walked to. */
    if (top_view) {
        const u8 support = (top_row == SNES_ROW_COM_SUPPORT ||
                            top_row == SNES_ROW_YOU_SUPPORT);
        const Msx2Side *sd = &g_duel.side[(top_row <= SNES_ROW_COM_MONSTER)
                                          ? MSX2_OWNER_COM : MSX2_OWNER_PLAYER];
        card = support ? sd->equip_field[top_col] : sd->field[top_col];
        if (card == MSX2_CARD_NONE) return MSX2_CARD_NONE;
        if (!support && !sd->faceup[top_col]) {
            *face = SNES_CARD_BACK;
            return MSX2_CARD_NONE;
        }
        *face = card;
        return card;
    }
    if (ui == UI_CHECK) {
        *face = check_face;
        return MSX2_CARD_NONE;
    }
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
    if (top_view) {
        const u8 owner = (top_row <= SNES_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                           : MSX2_OWNER_PLAYER;
        *atk = (u16)Msx2_FieldAtk(owner, top_col);
        *def = (u16)Msx2_FieldDef(owner, top_col);
    } else if (ui == UI_ATTACKER) {
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
/* Where in the fusion chain a hand slot sits, 1-based, or 0 for not in it. */
static u8 queue_order(u8 hand_slot)
{
    u8 i;
    for (i = 0; i < queue_n; ++i)
        if (queue[i] == hand_slot) return (u8)(i + 1);
    return 0;
}

static void queue_toggle(u8 hand_slot)
{
    u8 order = queue_order(hand_slot);
    u8 i;

    if (order != 0) {
        /* Taking a card out closes the gap, so what is left is still the
         * order the player chose it in. */
        for (i = (u8)(order - 1); i + 1 < queue_n; ++i) queue[i] = queue[i + 1];
        --queue_n;
    } else if (queue_n < MSX2_HAND &&
               g_duel.side[MSX2_OWNER_PLAYER].hand[hand_slot] != MSX2_CARD_NONE) {
        queue[queue_n++] = hand_slot;
    }
}

/* THE RESULT BANNER, and it is the MSX2 build's: the word starts wholly off
 * the left edge and eases to the middle of the screen along the REMAINING
 * distance, so it arrives slowing down instead of stopping dead, and then it
 * stays.  Two divisions rather than one product: the distance is about two
 * hundred and the step count thirty, and 200 * 30 * 30 does not fit in the
 * sixteen bits everything in this port is done in. */
#define OVER_SLIDE_FRAMES  30
#define OVER_HOLD_FRAMES   70
/* Under the life panels and above the board's far edge: the banner may not
 * cover the board it is announcing. */
#define OVER_Y             28

static void build_result_banner(void)
{
    const u8 won = (g_duel.result > 0);
    const char *word = won ? "YOU WIN" : "YOU LOSE";
    const s16 span = (s16)((won ? 7 : 8) * SNES_OBJ_BIG_PITCH);
    const s16 home = (s16)((256 - span) >> 1);
    s16 x = home;

    if (over_step < OVER_SLIDE_FRAMES) {
        const s16 rem = (s16)(OVER_SLIDE_FRAMES - over_step);
        x = (s16)(home - ((((home + span) * rem) / OVER_SLIDE_FRAMES) * rem)
                          / OVER_SLIDE_FRAMES);
    }
    snesObjBigText(x, OVER_Y, word,
                   won ? SNES_SPR_BIG_SET_GOLD : SNES_SPR_BIG_SET_RED);
}

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
 * seen: the banner and the text go in first, then the plates they sit on, then
 * the cursor, and the cards last of all. */
static void build_objects(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    u8 row, col, i;
    u8 face = SNES_CARD_NONE_FACE;
    const u8 card = focus_card(&face);
    u16 atk = 0, def = 0;
    u8 has_stats;
    s16 hand_y = HAND_Y;

    if (ui == UI_CHECK) {
        atk = check_atk;
        def = check_def;
        has_stats = check_has_stats;
    } else {
        has_stats = focus_stats(card, &atk, &def);
    }
    if (view_motion == VIEW_TO_TOP) {
        hand_y = (s16)(HAND_Y +
                       (s16)(((u16)VIEW_HAND_OFFSET *
                              view_anim_ease(view_anim_frame)) >> 8));
    } else if (view_motion == VIEW_TO_HAND) {
        hand_y = (s16)(HAND_Y +
                       (s16)(((u16)VIEW_HAND_OFFSET *
                              (SNES_ONE - view_anim_ease(view_anim_frame))) >> 8));
    }
    /* A message pre-empts the name, because it is the thing that just
     * happened; the name is back the moment it expires. */
    const char *name = message ? message
                     : (face != SNES_CARD_NONE_FACE) ? snesCardName(face)
                     : prompt_text();

    snesObjBegin();

    /* Card inspection and attacks are 2D Mode 3 scenes.  They deliberately
     * use the normal OBJ card cache, so no Mode 7 framebuffer is sampled or
     * rewritten while the presentation is visible. */
    if (mode3_active) {
        /* Card inspection and attacks are BG1/BG2 pictures (snes_cardart.c);
         * the sprite list is simply empty while one is up. */
        if (ui == UI_BATTLE_ART) {
            const u8 f = battle_frame > 24 ? 24 : battle_frame;
            snesCardArtReveal(f >= 24 ? 255 : (u8)(f * 5));
        }
        snesObjEnd();
        return;
    }

    if (ui == UI_RESULT) build_result_banner();

    if (top_view) {
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

        /* THE TABLE'S OWN CURSOR, in the red the PC-FX and FM TOWNS builds
         * draw it in, and it stands on a SLOT rather than on a card: an empty
         * slot is a thing the player is entitled to point at. */
        snesObjBoxRed(SNES_TOP_X0 + (board_yaw ? 4 - top_col : top_col) * SNES_TOP_CELL,
                      SNES_TOP_Y0 + (board_yaw ? 3 - top_row : top_row) * SNES_TOP_CELL,
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
                snesObjCardFlip(TOP_CARD_X(board_yaw ? 4 - col : col),
                            TOP_CARD_Y(board_yaw ? 3 - row : row),
                            (u8)(row * SNES_COLS + col), f,
                            (u8)((row < 2 ? 0x80 : 0) ^ (board_yaw ? 0xC0 : 0)));
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
         * the TOP of the band, with the words that describe it underneath.
         *
         * THE HAND IS THE ONE PLACE A CARD GETS A PALETTE TO ITSELF.  Five
         * cards against seven card palettes is a slot each, so a hand card is
         * quantised against nothing but its own painting instead of sharing
         * fifteen entries with the ten faces nearest it in colour.  That is
         * also what makes the DIMMING below free: the card the player is not
         * on keeps its tiles and swaps thirty-two bytes of palette for the
         * greyed, darkened copy of its own colours, so the hand reads as one
         * lit card among four without a second sheet or a second upload. */
        snesObjCardHiRes(1);
        for (i = 0; ui != UI_CHECK && i < MSX2_HAND; ++i) {
            s16 x = (s16)(HAND_X0 + i * HAND_PITCH);
            s16 y = hand_y;
            const u8 hcard = you->hand[i];
            const u8 queued = (u8)(queue_order(i) != 0);
            const u8 selected = (ui == UI_HAND)
                              ? (cursor == i)
                              : (chosen == i && ui != UI_ATTACKER &&
                                 ui != UI_DEFENDER && ui != UI_COM);
            if (hcard == MSX2_CARD_NONE) continue;
            if (fly_frame && i == chosen) {
                /* Mid-flight: the same eased curve the camera move uses, so a
                 * card leaves the hand quickly and settles onto its slot. */
                const u16 t = ease_frac(fly_frame, FLY_FRAMES);
                x = view_lerp(fly_x0, fly_x1, t);
                y = view_lerp(fly_y0, fly_y1, t);
            } else if (hand_y >= 224) {
                continue;
            } else if (selected) {
                /* A SMALL UP AND DOWN, once a field.  It is the same beat the
                 * PC build gives the selected card and it is what says which
                 * of the five the buttons are about. */
                y = (s16)(y - (snesSin(bob_phase) >> 6));
            }
            snesObjCardGrey((u8)(!selected && !queued));
            if (queued) snesObjBox(x - 4, y - 4, 40, 40);
            else if (selected) snesObjBox(x - 4, y - 4, 40, 40);
            snesObjCard(x, y, i, face_of(hcard, 1));
            snesObjCardGrey(0);
        }
    }

    if (fly_landing) {
        snesObjCardHiRes(1);
        snesObjCardGrey(0);
        snesObjCard(fly_x1, fly_y1, chosen, fly_face);
    }
    snesObjEnd();
}

/* ── The measurement fixture ─────────────────────────────────────────────── */

/* The random deck contains support cards as well as monsters.  A fixture field
 * is still a real rules field, so consume cards until the next monster rather
 * than writing a support id into the monster row (which makes the renderer and
 * the screenshot harness disagree about what the row means). */
static u8 fixture_draw_monster(WaifuDeck *deck)
{
    u8 tries = 0;
    int card;

    while (tries++ < WAIFU_DECK_SIZE) {
        card = waifu_deck_draw(deck);
        if (card >= 0 && Msx2_IsMonster((u8)card)) return (u8)card;
    }
    return MSX2_CARD_NONE;
}

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
        you->field[col] = fixture_draw_monster(&you->deck);
        com->field[col] = fixture_draw_monster(&com->deck);
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
    snesAudioSfx(SNES_SFX_SELECT);
    if (board) touch_board(4);
}

static void begin_battle_phase(void)
{
    queue_n = 0;
    g_duel.phase = MSX2_PHASE_BATTLE;
    ui = UI_ATTACKER;
    cursor = 0;
    touch_board(8);
}

static void end_player_turn(void)
{
    queue_n = 0;
    Msx2_EndTurn();
    ui = UI_COM;
    com_delay = 12;
    touch_board(8);
}

/* THE CARD FLIES TO THE SLOT BEFORE THE RULES HEAR ABOUT IT.
 *
 * The hand sprite walks from its position in the hand to the projected centre
 * of the board slot it is going into, and place_chosen is called on arrival --
 * so the card is on screen the whole way and the board is redrawn once, with
 * the card already lying on it.  It is the MSX2 build's landing, minus its
 * bend: the flight is sprites and costs no render at all, which is what lets
 * it run at sixty fields a second on this machine.  */
static void begin_place_flight(u8 defense)
{
    u8 row = 0;
    const u8 slot = held_slot(&row);
    s16 wx, wz, sx, sy;

    fly_def = defense;
    fly_face = face_of(g_duel.side[MSX2_OWNER_PLAYER].hand[chosen], 1);
    fly_x0 = (s16)(HAND_X0 + chosen * HAND_PITCH);
    fly_y0 = HAND_Y;
    fly_x1 = fly_x0;
    fly_y1 = fly_y0;
    if (slot != MSX2_SLOT_NONE && slot < SNES_COLS) {
        snesSlotCentre(row, slot, &wx, &wz);
        /* The projection is in VIEWPORT pixels and the viewport is half the
         * screen's width, so the sprite's screen position is twice it, less
         * half a card to centre the 32x32 sprite on the slot. */
        if (snesProject(&cam, &vp, wx, wz, 0, &sx, &sy)) {
            fly_x1 = (s16)((sx << 1) - 16);
            fly_y1 = (s16)((sy << 1) - 16);
        }
    }
    fly_frame = 1;
    render();
}

static void place_chosen(u8 defense)
{
    const u8 card = g_duel.side[MSX2_OWNER_PLAYER].hand[chosen];
    u8 placed = 0;
    if (ui == UI_FUSE_TARGET) {
        placed = Msx2_PlaceFusion(MSX2_OWNER_PLAYER, queue, queue_n, cursor,
                                  defense ? TRUE : FALSE);
        if (placed) queue_n = 0;
        else say("THEY DO NOT FUSE");
    } else if (Msx2_IsSupport(card)) {
        if (Msx2_PlaySupport(MSX2_OWNER_PLAYER, chosen, cursor))
            placed = 1;
        else
            say("CANNOT PLAY IT");
    } else if (Msx2_PlaceMonster(MSX2_OWNER_PLAYER, chosen, cursor,
                                 defense ? TRUE : FALSE)) {
        placed = 1;
    } else {
        say("CANNOT PLACE IT");
    }
    if (placed) snesAudioSfx(SNES_SFX_CARD_PLACED);
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
        /* DOWN IS THE FUSION CHAIN.  The hand row is the bottom of the board
         * view, so down means nothing else there, and it is the gesture the
         * MSX2 build uses for the same thing over the same rules model. */
        if (down & KEY_DOWN) {
            if (you->hand[cursor] == MSX2_CARD_NONE) {
                say("NO CARD THERE");
            } else {
                queue_toggle(cursor);
                snesAudioSfx(SNES_SFX_SELECT);
            }
        }
        if (down & KEY_A) {
            const u8 card = you->hand[cursor];
            /* A WAITING CHAIN OWNS A ON THE HAND ROW.  Without this the
             * ordinary single-card path played the hovered card on its own and
             * left the rest of the chain sitting in the hand, which reads
             * exactly like the button ignoring the selection. */
            if (queue_n != 0) {
                ui = UI_FUSE_TARGET;
                cursor = Msx2_FirstFreeSlot(MSX2_OWNER_PLAYER);
                if (cursor == MSX2_SLOT_NONE) cursor = 0;
                touch_board(8);
                break;
            }
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

    case UI_FUSE_TARGET:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) {
            const u8 preview = Msx2_FusionPreview(MSX2_OWNER_PLAYER, queue,
                                                 queue_n, cursor);
            if (preview == MSX2_FUSE_OK) {
                chosen = queue[0];
                begin_place_flight(0);
            } else {
                say(preview == MSX2_FUSE_SPENT ? "ONE MONSTER A TURN"
                                              : "THEY DO NOT FUSE");
            }
        } else if (down & KEY_B) {
            queue_n = 0;
            ui = UI_HAND;
            cursor = 0;
            touch_board(8);
        }
        break;

    case UI_PLACE:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) begin_place_flight(0);
        else if (down & KEY_X) begin_place_flight(1);
        else if (down & KEY_B) {
            ui = UI_HAND;
            cursor = chosen;
            touch_board(8);
        }
        break;

    case UI_EQUIP_TARGET:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) begin_place_flight(0);
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
            if (!Msx2_Attack(MSX2_OWNER_PLAYER, chosen, target)) {
                say("ILLEGAL ATTACK");
            } else {
                snesAudioSfx(SNES_SFX_LASER);
                cursor = chosen;
                begin_battle_art(UI_ATTACKER);
                return;
            }
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

void snesDuelConfigure(u8 story_duel, u8 story_mode)
{
    configured_story = story_duel;
    configured_story_mode = story_mode;
}

void snesDuelEnter(void)
{
    /* The editor's active slot is the player's actual deck.  The rules model
     * accepts overrides on a story duel, so use the first story opponent for
     * this standalone SNES battle while the presentation remains a free duel. */
    u8 saved_deck[WAIFU_DECK_SIZE];
    snesAudioPlay(SNES_AUDIO_ALTBATTLE);
    autoplay = 0;
    show_cards = 1;
    force_moving = 0;
    if (configured_story_mode && snesDeckGetCurrent(saved_deck)) {
        Msx2_DuelSetPlayerDeck(saved_deck, WAIFU_DECK_SIZE);
        Msx2_DuelInit(0x51E5u, configured_story);
    } else {
        Msx2_DuelSetPlayerDeck(0, 0);
        Msx2_DuelInit(0x51E5u, configured_story_mode ? configured_story
                                                     : MSX2_STORY_NONE);
    }

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
    view_motion = VIEW_BOARD_REST;
    view_anim_frame = 0;
    board_yaw = turn_frame = turn_from = turn_target = 0;
    top_row = SNES_ROW_YOU_MONSTER;
    top_col = 0;
    bob_phase = 0;
    queue_n = 0;
    fly_frame = 0;
    fly_landing = 0;
    over_step = 0;
    check_face = SNES_CARD_NONE_FACE;
    check_has_stats = 0;
    texture_w = texture_h = 0;
    motion_next_vblank = snes_vblank_count;
    snesVideoSetBoardZoom(SNES_M7_SCALE_STILL);
    snesVideoSetView(SNES_VIEW_BOARD);
    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, 0, 0);
    snesVideoSetBoardRes(SNES_RES_STILL);
    set_viewport();
    snesVideoClear(BACKDROP);
    /* Nothing draws into the bitmap's HUD band at all -- the HUD is sprites
     * and the plate under them is the HDMA'd backdrop -- so the band goes up
     * as zeroes exactly once, here. */
    snesVideoHudDirty();
    build_objects();
    render();
}

u8 snesDuelFrame(void)
{
    const u16 down = padsDown(0);
    u8 res = snesVideoBoardRes();
    u8 resized = 0;

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
        snesVideoHudDirty();
        touch_board(2);
    }

    ++bob_phase;
    if (ui == UI_BATTLE_ART) {
        if (battle_frame < 90) ++battle_frame;
        if (battle_frame >= 90 || (battle_frame > 24 &&
            (down & (KEY_A | KEY_B | KEY_START))))
            end_battle_art();
        build_objects();
        goto stamp;
    }
    if (view_motion == VIEW_BOARD_REST && ui == UI_HAND && !fly_landing && (down & KEY_B)) {
        begin_check();
        if (ui == UI_CHECK) {
            build_objects();
            goto stamp;
        }
    }

    /* Inspection pauses the duel and restores the previous UI on close.
     * Consume the closing press here so it cannot also play a card. */
    if (ui == UI_CHECK) {
        if (down & (KEY_B | KEY_A | KEY_START)) end_check();
        build_objects();
        goto stamp;
    }

    /* UP walks up into the tactical top view.  In the top view the d-pad is
     * the TABLE'S OWN CURSOR -- twenty slots the player can walk over and read
     * -- so B is what walks back down; DOWN there would otherwise cost the
     * cursor a whole axis of the board. */
    if (view_motion == VIEW_BOARD_REST && (down & KEY_UP) && !fly_frame && !fly_landing &&
        !turn_frame && ui != UI_COM && ui != UI_RESULT)
        begin_view_transition(1);
    else if (view_motion == VIEW_TOP_REST) {
        u8 moved = 0;
        if (down & KEY_LEFT)  { top_col = (u8)((top_col + SNES_COLS - 1) % SNES_COLS); moved = 1; }
        if (down & KEY_RIGHT) { top_col = (u8)((top_col + 1) % SNES_COLS); moved = 1; }
        if (down & KEY_UP)    { top_row = (u8)((top_row + SNES_ROWS - 1) % SNES_ROWS); moved = 1; }
        if (down & KEY_DOWN)  { top_row = (u8)((top_row + 1) % SNES_ROWS); moved = 1; }
        if (moved) snesAudioSfx(SNES_SFX_SELECT);
        if (down & KEY_A) {
            begin_check();
            if (ui == UI_CHECK) {
                build_objects();
                goto stamp;
            }
        }
        if (down & KEY_B) {
            /* Card check and rule changes may have replaced the saved bitmap. */
            if (board_dirty) render();
            begin_view_transition(0);
        }
        build_objects();
        goto stamp;
    }

    if (fly_landing) {
        if (snesVideoPresentDone()) fly_landing = 0;
        build_objects();
        goto stamp;
    }

    /* A card on its way to the board.  Sprites only: no rules step, no render
     * and no rendering decision until it lands. */
    if (fly_frame) {
        if (++fly_frame > FLY_FRAMES) {
            fly_frame = 0;
            place_chosen(fly_def);
            render();
            fly_landing = 1;
        }
        build_objects();
        goto stamp;
    }

    if (message_timer) {
        if (--message_timer == 0) {
            message = NULL;
        }
    }

    if (g_duel.result != 0 && ui != UI_RESULT) {
        ui = UI_RESULT;
        over_step = 0;
        queue_n = 0;
        snesAudioPlay((g_duel.result > 0) ? SNES_AUDIO_VICTORY
                                          : SNES_AUDIO_FAIL);
        /* The outcome is the PROMPT, not a message: a message expires, and the
         * one line that must still be readable a minute after the duel ended is
         * which way it went. */
        message = 0;
        message_timer = 0;
        motion = 0;
        touch_board(0);
    }

    if (view_motion == VIEW_TO_TOP || view_motion == VIEW_TO_HAND) {
        step_view_transition();
        goto stamp;
    }

    /* Finish the camera move before allowing the next side to act.
     *
     * IN THE TOP VIEW THERE IS NOTHING TO TURN.  The table is a resident
     * tilemap with the field on it as sprites, so a hand-off up there is the
     * sprites changing sides -- which build_objects already does off board_yaw
     * -- and dropping the player out of the view to animate a bitmap they
     * cannot see was the whole of what made passing the turn look broken. */
    if (ui != UI_RESULT && !turn_frame && board_yaw != (g_duel.turn_owner ? 128 : 0)) {
        if (top_view || view_motion != VIEW_BOARD_REST) {
            board_yaw = g_duel.turn_owner ? 128 : 0;
            touch_board(2);
        } else {
            turn_from = board_yaw;
            turn_target = g_duel.turn_owner ? 128 : 0;
            snesVideoSetView(SNES_VIEW_BOARD);
            snesVideoSetBoardRes(SNES_RES_BEND);
            set_viewport();
            turn_frame = 1;
            motion_next_vblank = snes_vblank_count;
        }
    }
    if (turn_frame) {
        if (!motion_frame_due()) {
            build_objects();
            goto stamp;
        }
        u16 t = ease_frac((u8)((turn_frame + 1) >> 1), (TURN_FRAMES + 1) >> 1);
        board_yaw = (u8)view_lerp(turn_from, turn_target, t);
        snesCameraSet(&cam, 0,
                      CAM_Z - snesQMul(256, snesSin(board_yaw)),
                      CAM_HEIGHT, (s16)((u16)vp.w << 7), vp.h >> 5);
        cam.yaw = board_yaw;
        render();
        build_objects();
        schedule_motion_frame();
        if (++turn_frame > TURN_FRAMES) {
            turn_frame = 0;
            board_yaw = turn_target;
            touch_board(2);
        }
        goto stamp;
    }

    if (ui == UI_RESULT) {
        /* The banner has to ARRIVE and be readable before a button is worth
         * anything.  Without the hold, a held A -- or the soak's permanently
         * held one -- ends the duel on the frame the banner is created and the
         * screen this whole path exists for is never on screen at all. */
        if (over_step <= OVER_SLIDE_FRAMES + OVER_HOLD_FRAMES) ++over_step;
        else if (down & (KEY_A | KEY_B | KEY_START)) {
            if (configured_story_mode && g_duel.result > 0) {
                const u8 next = (u8)(configured_story + 1);
                /* Story can begin directly from a fresh cartridge.  The
                 * default deck is resident in the editor model, but SRAM is
                 * still uninitialised until the player visits the editor;
                 * materialise it before recording story progress. */
                if (!snesSaveIsValid()) snesDeckSaveCurrent();
                snesSaveStoryProgressStore(next);
                if (next >= MSX2_STORY_MAX_DUELS)
                    return SNES_SCENE_ENDING;
                snesStoryBegin(next);
                return SNES_SCENE_STORY_TALK;
            }
            return SNES_SCENE_TITLE;
        }
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
            if (g_duel.last_action == MSX2_ACTION_ATTACK)
                begin_battle_art(UI_COM);
            else {
                Msx2_ClearActionEvent();
                touch_board(12);
            }
        }
    }

    /* A CARD IN THE AIR IS SOMETHING CHANGING.  While the player is choosing
     * where to put a card, the card hovers over the slot and bobs, so the board
     * stays on the fixed five-update cadence and keeps being redrawn. */
    if (ui == UI_PLACE || ui == UI_EQUIP_TARGET) touch_board(2);

    /* The board keeps its 128x80 source while something is changing.  A
     * movement frame is admitted only every twelve vblanks; the upload and
     * renderer therefore have a constant visual cadence instead of changing
     * the board's texel size. */
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
        if (res != SNES_RES_MOVING) {
            snesVideoSetBoardRes(SNES_RES_MOVING);
            set_viewport();
            res = SNES_RES_MOVING;
            resized = 1;
            motion_next_vblank = snes_vblank_count;
        }
        if (motion_frame_due()) {
            lift_phase += 8;
            board_dirty = 1;
        }
    } else if (res != SNES_RES_STILL && snesVideoPresentDone()) {
        snesVideoSetBoardRes(SNES_RES_STILL);
        set_viewport();
        res = SNES_RES_STILL;
        resized = 1;
        board_dirty = 1;
    }

    if (board_dirty && (resized || snesVideoPresentDone())) {
        render();
        if (res == SNES_RES_MOVING) schedule_motion_frame();
    }
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
    return SNES_SCENE_COUNT;
}
