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
 *  Both are the same textured slab in perspective through the same renderer
 *  (snes_board3d.c) into the same 256x144 frame, at the screen's own
 *  resolution in every pose: twenty slots with their cards lying on them, a
 *  marker under the slot in question, and the card being played held in the
 *  air over its target through the convex-quad path.  At REST the picture is
 *  the ROM floor for the seat with the cards drawn over it, baked once and
 *  then PATCHED cell by cell as slots change; while the CAMERA MOVES -- the
 *  turn pass, the lift into the overhead view -- every frame is rendered
 *  whole.  The overhead view is that camera at the top of the lift, looking
 *  straight down, with a sprite bracket for its cursor so walking over the
 *  table costs no render at all.  snes_video.h has the presentation model.
 *
 *  A battle or a direct attack leaves the board for the Mode 4 lanes of
 *  snes_battle.c and comes back to a board baked again from nothing.
 *
 *  NOTHING IN THE HUD IS DRAWN INTO THE FRAME.  Life points, the prompt, the
 *  hand and both cursors are sprites, which is what lets them stay on screen
 *  at the console's own resolution across a board change that rewrites no
 *  VRAM at all.  See snes_obj.h.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_cardart.h"
#include "snes_battle.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_planar_data.h"
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
#define CAM_HEIGHT   SNES_REST_CAM_HEIGHT    /* 2.75 */
#define CAM_Z        SNES_REST_CAM_Z         /* 3 in front of the near edge */
/* Half the unit viewport's width, in Q8.8: what makes du a shift. */
#define CAM_FOCAL    ((s16)(64 << 8))
/* The top of the lift: over the middle of the board, looking straight down
 * from EXACTLY 4.0 units.  There the focal length of 128 makes a unit 32
 * pixels, which is the world texture's 32 texels a unit: the overhead pose
 * is the texture itself, one texel a pixel, so its floor rows are block
 * moves (snesSpanFloorTex) and no texel is resampled.  The slab's five
 * columns are 160 of the 256 pixels and its four rows 128 of the 144 lines
 * -- the board fills the overhead view the way the MSX2 and PC-FX tactical
 * views do, instead of sitting in the middle of it as a minimap.  The
 * camera's foot is on line 72 so the slab's 128 lines are 8..136: sixteen
 * whole 8-line bands of twenty cells, 320 of the converter's 351. */
#define LIFT_HEIGHT  ((s16)(1024))           /* 4.0 units */
#define LIFT_HORIZON ((s16)72)
/* THE LIFT SWOOPS.  Partway up, the camera is closer to the near edge than
 * either endpoint and looking along the board, so the slab and its front
 * wall spread over more 8x8 cells than the converter's 351-cell budget --
 * a frame over budget is not shown at all.  Climbing an extra 0.8 units in
 * the middle of the move (a half sine over the lift) keeps every pose
 * inside the budget, and reads as the camera rising before it looks down. */
#define LIFT_BUMP    ((s16)205)             /* 0.8 units */
/* THREE POSES, NOT FOUR.  A perspective pose is fifty-odd fields of walk
 * and conversion whatever its angle, so the poses ARE the lift's duration:
 * two on the way (at the eased thirds of the move) and the 1:1 overhead
 * itself, about 150 fields, against 210 with a fourth.  The hand slides
 * continuously between them either way. */
#define LIFT_FRAMES  3

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

#define HAND_Y       150
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

/* How far the hand slides off the bottom of the screen over the lift. */
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
    UI_BATTLE_ART       /* the Mode 4 battle or direct attack (snes_battle.c) */
};

enum SnesViewMotion {
    VIEW_BOARD_REST = 0,
    VIEW_TO_TOP,
    VIEW_TOP_REST,
    VIEW_TO_HAND
};

static SnesCamera cam;
static SnesViewport vp_rest;
/* THE MOVING CAMERA RENDERS AT 128x72.  The motion frame is the top 9 KB of
 * the same buffer at half the stride, walked at a quarter of the texels and
 * converted at half the cost a cell (each texel a 2x2 block of pixels: the
 * converter looks a texel up as a pixel pair and stores each row twice, so
 * a cell is four texels by four lines).  It is the unit viewport itself,
 * sub 0; the camera is the same one with its horizon in the frame's own
 * pixels (cam_move).  (Doubling the lines by an HDMA on BG1VOFS instead,
 * for half the cells, was tried and withdrawn: BG1's scroll registers share
 * the PPU multiplier's write-twice latch, and an HDMA write landing between
 * the two halves of a product corrupted a row of the next render.) */
static SnesViewport vp_move;
static SnesCamera cam_move;
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
enum SnesComPresentation {
    COM_PRESENT_IDLE = 0, COM_PRESENT_SELECT, COM_PRESENT_FLY, COM_PRESENT_LAND
};
/* The PC's COM select (src/main.c IB_COM_SELECT / IB_COM_EQUIP_SELECT):
 * the red cursor visits a hand slot every four frames and settles on the
 * chosen one for the last quarter, then the card flies to its slot. */
#define COM_SELECT_FIELDS 24
#define COM_SELECT_SETTLE 18
#define COM_FLY_FIELDS    14
#define DRAW_FIELDS        6
static u8 com_present = COM_PRESENT_IDLE;
static u8 com_present_field = 0;
static u8 com_cursor = 0;       /* the hand slot the sweeping cursor is on */
static u8 com_hand[MSX2_HAND];
static u8 com_hand_slot = MSX2_SLOT_NONE;
static u8 com_field_slot = MSX2_SLOT_NONE;
static u8 com_card = MSX2_CARD_NONE;
static u8 com_defense = 0;
static u8 com_action = MSX2_ACTION_NONE;
static u8 hand_visible[2] = { 0, 0 };
static u8 draw_pending[2] = { 0, 0 };
static u8 draw_field = 0;
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
/* THE OVERHEAD VIEW HAS A CURSOR OF ITS OWN, and it is a board position rather
 * than whatever the hand happened to be pointing at.  The view shows twenty
 * slots and every other port lets the player walk over all of them and read
 * what is lying there; carrying the hand's cursor up there instead left the
 * marker stuck on a hand slot the view does not even draw.  It is an
 * INSPECTION cursor: it names the card under it in the HUD row and A opens
 * the card check, but it does not play anything. */
static u8 top_row = SNES_ROW_YOU_MONSTER;
static u8 top_col = 0;
/* THE OVERHEAD VIEW IS ALSO WHERE THE PLAYER ATTACKS FROM, the way the PC
 * and PC-FX builds do it (IB_PLAYER_TOP in src/main.c): A on one of the
 * player's own standing monsters picks it as the attacker, the cursor jumps
 * to the opponent's row, and A on a monster there declares the attack; B
 * puts the attacker down again.  With no monster to attack the pick IS the
 * direct attack.  The hand row's X:FIGHT path is the other way in. */
static u8 top_attacker = MSX2_SLOT_NONE;
static u8 battle_return_top = 0;
/* The opponent's card flies FACE UP, out of an OBJ slot of its own: the
 * hand's slot holds the back the row is drawn with, and swapping its tiles
 * for the face would leave the card invisible for the four vblanks of the
 * upload.  Slot 5 is the first the hand does not use, and it has a palette
 * of its own (SNES_SPR_CARD_PALS is seven), so the face is queued at the
 * start of the opponent's select sweep and is resident when the flight
 * begins. */
#define COM_FLY_SLOT 5
/* THE DUEL OPENS FROM BLACK.  The other builds swing the camera round the
 * empty board while the palette comes up; a moving pose is fifty fields
 * here, so the board is baked at rest and the master brightness is walked
 * up over half a second instead, and the hands are dealt (the draw
 * cadence) once it is fully lit.  The level is applied in vblank
 * (snesDuelVblank), never mid-frame. */
#define INTRO_FADE_FIELDS 32
static u8 intro_fade = 0;       /* fields of the fade left */
static u8 fade_level = 15;      /* what INIDISP should hold */
static u8 fade_dirty = 0;
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
/* A card being put down IS LOWERED ONTO ITS SLOT.  The card the player has
 * been holding over the slot (draw_held_card, the convex-quad path through
 * the rest picture's cells) comes down from its hover and levels out, one
 * patched frame a game frame, and the rules are told when it is flat; the
 * next patch then bakes it as a face on the board.  It is the same card
 * from the same texture the whole way, so there is no sprite to land and
 * nothing to hand over -- the flight this replaces put a 32x32 sprite over
 * the board and held it there until the bake was presented. */
#define LOWER_FRAMES  6
static u8  lower_frame = 0;
static u8  lower_def = 0;
/* The card check: one card, enlarged into the board bitmap, over black. */
static u8  check_face = SNES_CARD_NONE_FACE;
static u8 check_return_ui = UI_HAND;
static u8 check_return_top = 0;
static u8  check_has_stats = 0;
static u16 check_atk = 0, check_def = 0;
static u8 mode3_active = 0;
static u8 battle_frame = 0;             /* fields into the battle, for the stamp */
static u8 battle_return_ui = UI_COM;
/* How far into its entrance the result banner is. */
static u8  over_step = 0;
static u8 texture_w = 0, texture_h = 0;
#define TURN_FRAMES 4

static u8 next_pool = 0;
static u16 requested_generation = 0;
/* The debug pattern is a bounded 1:1 converter fixture. */
static u8  pattern_mode = 0;
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

/* Y continuously requests production full-resolution frames for profiling. */
static u8  force_moving = 0;
static u8 configured_story = MSX2_STORY_NONE;
static u8 configured_story_mode = 0;

static void render(void);
static void build_objects(void);
static void slide_hand(void);
static u8   face_of(u8 card, u8 faceup);
static u8   focus_card(u8 *face);
static u8   focus_stats(u8 card, u16 *atk, u16 *def);
static void say(const char *msg);
static void note_draws(u8 owner, const u8 *before);
static void place_chosen(u8 defense);

u8 snesDuelMode3Active(void) { return mode3_active; }

/* The master brightness, applied in vblank.  The scene machine turns the
 * screen on at this level (snes_main.c) so the fade starts from black rather
 * than after a field of full brightness. */
u8 snesDuelBrightness(void) { return fade_level; }
void snesDuelVblank(void)
{
    if (!fade_dirty) return;
    fade_dirty = 0;
    REG_INIDISP = fade_level;
}

static void set_viewport(void)
{
    vp_rest.w = SNES_FRAME_W;
    vp_rest.h = SNES_FRAME_H;
    vp_rest.origin = (u16)(u16)snes_frame_fb;
    vp_rest.stride = SNES_FRAME_STRIDE;
    vp_rest.bank = 0x7E;
    vp_rest.sub = 1;
    /* 32 texels a unit over a focal length of 64 is half a texel a unit
     * pixel; a screen pixel is half of that. */
    vp_rest.du_k = 64;

    vp_move.w = SNES_FRAME_W / 2;
    vp_move.h = SNES_FRAME_H / 2;
    vp_move.origin = (u16)(u16)snes_frame_fb;
    vp_move.stride = SNES_FRAME_STRIDE / 2;
    vp_move.bank = 0x7E;
    vp_move.sub = 0;
    vp_move.du_k = 128;

    /* The focal length is half the unit viewport's width, which is what
     * makes the floor's texture step a shift rather than a divide.  The
     * horizon sits a thirty-second of the board down, in the viewport's own
     * pixels: the resting camera's is the ROM floor's. */
    cam.focal = CAM_FOCAL;
    cam.horizon = SNES_REST_HORIZON_PX;
}

/* The camera for a resting board: the ROM floor's pose. */
/* Whether the camera is exactly at a rest pose: set by set_rest_camera,
 * cleared by anything that moves it. */
static u8 camera_rest = 0;
/* The last render's timings, in scanlines, copied into the stamp at the end
 * of the game frame. */
static u16 t_map = 0, t_conv = 0, t_render = 0;
static u16 t_turn_max = 0, t_rest_max = 0, t_held_max = 0;
static void set_rest_camera(u8 mirror)
{
    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, CAM_FOCAL, SNES_REST_HORIZON_PX);
    cam.yaw = mirror ? 128 : 0;
    camera_rest = 1;
}

/* The camera as the motion frame sees it: the same pose, the horizon in
 * that frame's pixels (the camera's is in the 256x144 picture's). */
static void sync_cam_move(void)
{
    cam_move = cam;
    cam_move.horizon = (s16)(cam.horizon >> 1);
}

/* Q8.8 smoothstep, using the SNES multiplier instead of a 32-bit product.
 * The eased value is shared by the camera and hand so the two settle together
 * at the exact frame the PPU switches to the resident top table. */
static void job_begin(u8 half);
static u8   job_step(void);
static u8   job_slice(void);
static u8   job_active(void);
static u16  job_progress(void);

static u16 ease_frac(u8 frame, u8 total)
{
    u16 t;
    u16 t2;
    if (frame >= total) return SNES_ONE;
    t = snesUQDiv(frame, total);
    t2 = (u16)snesQMul((s16)t, (s16)t);
    return (u16)snesQMul((s16)t2, (s16)(3 * SNES_ONE - 2 * t));
}

/* The lift's progress for the sprite layer, Q8.8 eased: the poses done plus
 * the fraction of the one on its way, so the hand slides continuously while
 * the board arrives four times. */
static u16 view_anim_ease(u8 frame)
{
    u16 t;
    /* The descent renders one pose fewer (see step_view_transition), so
     * the hand's move is spread over the poses that ARE rendered. */
    const u8 poses = (view_motion == VIEW_TO_HAND) ? LIFT_FRAMES - 1 : LIFT_FRAMES;
    if (frame >= poses) return SNES_ONE;
    /* (frame - 1 + progress) / poses, then smoothstepped. */
    t = (u16)(((u16)(frame ? frame - 1 : 0) << 8) + job_progress());
    t = (u16)(snesMulLo(t, (u16)(SNES_ONE / poses)) >> 8);
    {
        const u16 t2 = (u16)snesQMul((s16)t, (s16)t);
        return (u16)snesQMul((s16)t2, (s16)(3 * SNES_ONE - 2 * t));
    }
}

static s16 view_lerp(s16 a, s16 b, u16 t)
{
    return (s16)(a + snesQMul((s16)(b - a), (s16)t));
}

/* THE LIFT IS A CAMERA MOVE AND NOTHING ELSE.  The camera climbs from its
 * seat to LIFT_HEIGHT over the middle of the board while pitching to straight
 * down, one full-resolution frame a game frame, and the hand slides off on
 * the same curve.  The overhead view is the same renderer at the top of that
 * path, so there is no second picture to load and nothing to switch to. */
static void lift_camera(u8 frame)
{
    const u16 t = ease_frac(frame, LIFT_FRAMES);
    /* sin(pi * t): t is Q8.8 over one lift, and 256 counts of snesSin's
     * angle are a whole turn, so half a turn is t / 2. */
    const s16 bump = snesQMul(LIFT_BUMP, snesSin((u8)(t >> 1)));
    snesCameraSet(&cam, 0,
                  view_lerp(CAM_Z, 0, t),
                  (s16)(view_lerp(CAM_HEIGHT, LIFT_HEIGHT, t) + bump),
                  CAM_FOCAL, LIFT_HORIZON);
    cam.pitch = (u8)view_lerp(0, 64, t);
    /* Straight down, the horizon has no meaning: the frame is centred on
     * the camera's foot.  Partway, the horizon rises with the pitch. */
    cam.horizon = (s16)view_lerp(SNES_REST_HORIZON_PX, LIFT_HORIZON, t);
}

static void motion_sequence_begin(u8 frames);
static void motion_frame(void);
static void rest_invalidate(void);

/* THE OVERHEAD VIEW ARRIVES TWICE.  The lift's last pose is the camera at
 * the top, rendered like the ones before it as a 128x72 motion frame, so
 * the board stops moving as soon as it gets there; the same picture is
 * then rendered again at 1:1 (the sharpening job, view_sharp) and switched
 * to when it is ready, forty-odd fields later.  The top view's cursor is
 * live from the first arrival: the d-pad moves it over the doubled picture
 * as well, only A and B wait for the sharp one. */
static u8 view_sharp = 0;       /* 0 not yet, 1 the 1:1 job is running/done */

static void begin_view_transition(u8 to_top)
{
    view_motion = to_top ? VIEW_TO_TOP : VIEW_TO_HAND;
    view_anim_frame = 0;
    view_sharp = 0;
    motion_sequence_begin(LIFT_FRAMES);
    if (!to_top) {
        top_view = 0;
    }
}

static void finish_view_transition(u8 to_top)
{
    if (to_top) {
        /* The last lift frame IS the overhead pose: nothing to redraw, and
         * the cursor up here is a sprite, so the view runs at sixty. */
        top_view = 1;
        view_motion = VIEW_TOP_REST;
    } else {
        top_view = 0;
        view_motion = VIEW_BOARD_REST;
        /* Back at the seat: the rest picture is baked again from the ROM
         * floor, which is also what puts the 32x32 card art back. */
        set_rest_camera((u8)(board_yaw == 128));
        rest_invalidate();
    }
    view_anim_frame = 0;
}

/* One game frame of the move: render the next lift frame, and move the
 * hand on the same curve.  Going up, the mode changes only once every card
 * the table needs is in VRAM and the last frame has been shown; coming
 * down, the board view is switched on as soon as the first frame is up. */
static void step_view_transition(void)
{
    const u8 to_top = (view_motion == VIEW_TO_TOP);

    /* The last pose's job is begun with view_anim_frame already at
     * LIFT_FRAMES, so it is the JOB that says whether there is still
     * rendering to do; a pose left in progress here is the overhead view
     * never arriving. */
    /* THE DESCENT STOPS ONE POSE SHORT.  Its last pose would be the rest
     * camera through the moving renderer, fifty fields for a picture that
     * finish_view_transition bakes again from the ROM floor the moment it
     * is done; the poses on the way down are the lift's, in reverse. */
    const u8 poses = to_top ? LIFT_FRAMES : (u8)(LIFT_FRAMES - 1);
    if (view_anim_frame < poses || job_active()) {
        if (!job_active()) {
            ++view_anim_frame;
            lift_camera(to_top ? view_anim_frame : (u8)(LIFT_FRAMES - view_anim_frame));
            /* Every pose on the way is a 128x72 motion frame, the top of
             * the lift included: the sharp overhead picture follows. */
            job_begin(1);
            build_objects();
        }
        /* One bounded renderer slice per game loop.  The completed map stays
         * visible while this generation is built, and the main loop's pad,
         * OAM and audio service runs between every slice; the hand's
         * sprites slide between the slice's steps (slide_hand), so they
         * move at the field rate without a sprite rebuild a step. */
        job_slice();
        slide_hand();
        return;
    }
    if (to_top && !view_sharp) {
        /* The camera is at the top and its doubled picture is on its way
         * up or shown: the cursor is live from here, and the 1:1 picture
         * is rendered behind it. */
        view_sharp = 1;
        top_view = 1;
        job_begin(0);
        build_objects();
        job_slice();
        return;
    }
    if (!snesVideoPresentDone()) {
        slide_hand();
        return;
    }
    finish_view_transition(to_top);
    build_objects();
}

static u8 texture_check = 1;    /* the world texture may be stale */
static void touch_board(u8 frames)
{
    board_dirty = 1;
    texture_check = 1;
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
        /* The cursor picks the MONSTER the equip goes on, as the PC and
         * PC-FX do it; the card itself hovers over the support zone the
         * rules will give it (held_slot). */
        *row = monster_row(MSX2_OWNER_PLAYER);
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
    if (ui == UI_EQUIP_TARGET) {
        /* The zone is the rules' choice (Msx2_PlaySupport takes the first
         * free one), so the card hovers over -- and comes down on -- that. */
        *row = SNES_ROW_YOU_SUPPORT;
        return Msx2_FirstFreeEquipSlot(MSX2_OWNER_PLAYER);
    }
    return cursor_board_slot(row);
}

/* ── The board picture ───────────────────────────────────────────────────── */

/* ONE FRAME, ONE PICTURE, TWO WAYS OF PAINTING IT.
 *
 * The 256x144 chunky frame in bank $7E is the board as the player sees it,
 * and it is converted to tiles by snes_conv_drivers.inc one 8x8 cell at a
 * time -- but only the cells that CHANGED.  The converter shares every other
 * cell's tile between the map on screen and the map being built, so what a
 * render costs is what it touches, and the two painting paths below exist to
 * touch as little as possible:
 *
 *   * A RESTING camera (yaw 0 or 128, level, at the ROM floor's pose) starts
 *     from the floor rendered at build time (tools/snes/gen_snes_planar.py),
 *     draws the cards over it from the 32x32 sheet at 1:1, and from then on
 *     PATCHES: a slot whose face changed, the marker's old and new slots and
 *     the held card's old and new cells get the ROM floor put back under
 *     them, the cards on those rows redrawn, and only those cells converted.
 *     A cursor move is a couple of dozen cells.
 *
 *   * A MOVING camera (the turn, the lift, the overhead pose) is the general
 *     path: the slab as one textured quad through the inverse-ray walker,
 *     with the cards stamped into the world texture, and every occupied cell
 *     converted.  It is the expensive frame and it is only paid while the
 *     camera is actually somewhere the ROM floor is not. */
static u8  rest_valid = 0;          /* the frame holds the rest picture */
static u8  rest_pose = 0;           /* ...for this seat (0 = player, 1 = COM) */
static u8  baked_faces[20];         /* ...with these faces on it */
static u8  baked_marker_row = 0xFF, baked_marker_col = 0xFF, baked_marker_colour = 0;
static u8  held_drawn = 0;          /* the held card's cells last frame... */
static u8  held_box[4];             /* ...as an inclusive cell box */
static u8  slot_box[2][20][4];
static u8  slot_box_valid[2] = { 0, 0 };

/* The cell box of a slot: its tile's four corners projected on the rest
 * viewport, in 8x8 cells, inclusive.  Generated with the floor, so the two
 * cannot disagree. */
static void slot_boxes(u8 mirror)
{
    u8 i;
    if (slot_box_valid[mirror]) return;
    for (i = 0; i < 80; ++i)
        ((u8 *)slot_box[mirror])[i] = snes_rest_slot_boxes[(u16)mirror * 80 + i];
    slot_box_valid[mirror] = 1;
}

/* The converter's dirty masks live in bank $7F beside its maps. */
static void cells_clear(void)
{
    u16 *d = snes_conv_dirty;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS * 2; ++r) d[r] = 0;
}

static void cells_all(void)
{
    u16 *d = snes_conv_dirty;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS * 2; ++r) d[r] = 0xFFFF;
}

static u8 cells_any(void)
{
    const u16 *d = snes_conv_dirty;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS * 2; ++r) if (d[r]) return 1;
    return 0;
}

/* OR a cell box into the dirty masks, or clear it from the ROM masks: the
 * ROM masks name the resting cells nothing is drawn over, which the
 * converter takes straight from the planar ROM floor. */
static void mask_box(u16 *d, u8 set, u8 cx0, u8 cy0, u8 cx1, u8 cy1)
{
    u16 mlo = 0, mhi = 0;
    u8 r;
    if (cx1 < cx0 || cy1 < cy0) return;
    if (cx1 > 31) cx1 = 31;
    if (cy1 >= SNES_CELL_ROWS) cy1 = SNES_CELL_ROWS - 1;
    for (r = cx0; r <= cx1; ++r) {
        if (r < 16) mlo |= (u16)(1u << r);
        else        mhi |= (u16)(1u << (r - 16));
    }
    for (r = cy0; r <= cy1; ++r) {
        if (set) {
            d[r * 2]     |= mlo;
            d[r * 2 + 1] |= mhi;
        } else {
            d[r * 2]     &= (u16)~mlo;
            d[r * 2 + 1] &= (u16)~mhi;
        }
    }
}

static void cells_box(u8 cx0, u8 cy0, u8 cx1, u8 cy1)
{
    mask_box(snes_conv_dirty, 1, cx0, cy0, cx1, cy1);
}

static void cells_slot(u8 mirror, u8 row, u8 col)
{
    const u8 *b = slot_box[mirror][row * SNES_COLS + col];
    cells_box(b[0], b[1], b[2], b[3]);
}

static void rom_cells_none(void)
{
    u16 *d = snes_conv_rom;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS * 2; ++r) d[r] = 0;
}

static void rom_cells_all(void)
{
    u16 *d = snes_conv_rom;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS * 2; ++r) d[r] = 0xFFFF;
}

static void rom_cells_clear_slot(u8 mirror, u8 row, u8 col)
{
    const u8 *b = slot_box[mirror][row * SNES_COLS + col];
    mask_box(snes_conv_rom, 0, b[0], b[1], b[2], b[3]);
}

/* The pixel rows the dirty cells cover, for a clipped card redraw. */
static void cells_rows(u16 *y0, u16 *y1)
{
    const u16 *d = snes_conv_dirty;
    u8 r, first = 0xFF, last = 0;
    for (r = 0; r < SNES_CELL_ROWS; ++r) {
        if (!(d[r * 2] | d[r * 2 + 1])) continue;
        if (first == 0xFF) first = r;
        last = r;
    }
    if (first == 0xFF) { *y0 = *y1 = 0; return; }
    *y0 = (u16)first << 3;
    *y1 = (u16)(last + 1) << 3;
}

static void current_faces(u8 *faces)
{
    u8 row, col;
    for (row = 0; row < SNES_ROWS; ++row) {
        const u8 owner = (row <= SNES_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                       : MSX2_OWNER_PLAYER;
        const u8 support = (row == SNES_ROW_COM_SUPPORT ||
                            row == SNES_ROW_YOU_SUPPORT);
        const Msx2Side *s = &g_duel.side[owner];
        for (col = 0; col < SNES_COLS; ++col) {
            const u8 card = support ? s->equip_field[col] : s->field[col];
            faces[row * SNES_COLS + col] = show_cards
                ? face_of(card, support ? 1 : s->faceup[col])
                : SNES_CARD_NONE_FACE;
        }
    }
}

/* ── The moving camera's world texture ───────────────────────────────────── */

/* Flat cards have one world-space footprint in every pose.  Camera pitch and
 * yaw alter their projection, never the source texture's dimensions. */
/* Whether the world texture matches the field.  When it does not, the floor
 * is put back under every slot and the caller stamps the cards -- all at
 * once (update_board_texture) or a few a field (the camera job). */
static u8 texture_stale(void)
{
    u8 faces[20];
    u8 changed = (texture_w != 24 || texture_h != 32);
    u8 r;
    current_faces(faces);
    for (r = 0; r < 20; ++r) {
        if (texture_faces[r] != faces[r]) changed = 1;
        texture_faces[r] = faces[r];
    }
    if (!changed) return 0;
    texture_w = 24;
    texture_h = 32;
    snesBoardTextureClear();
    return 1;
}

static void texture_stamp(u8 slot)
{
    const u8 r = (u8)(slot / SNES_COLS), c = (u8)(slot % SNES_COLS);
    const u8 face = texture_faces[slot];
    if (face != SNES_CARD_NONE_FACE)
        snesBoardTextureCard(
            (u16)(((48 - (s16)r * 32) & 127) * 256 |
                  ((16 + ((s16)c - 2) * 32) & 255)),
            face, r < 2, 24, 32);
}

static void update_board_texture(void)
{
    u8 i;
    if (!texture_stale()) return;
    for (i = 0; i < 20; ++i) texture_stamp(i);
}

/* ── The resting picture ─────────────────────────────────────────────────── */

/* Put the ROM floor back under the dirty cells. */
static void restore_floor_cells(void)
{
    const u16 src = (u16)(u16)(rest_pose ? snes_floor_chunky_1 : snes_floor_chunky_0);
    const u8 bank = rest_pose ? SNES_FLOOR_CHUNKY_BANK_1 : SNES_FLOOR_CHUNKY_BANK_0;
    const u16 *d = snes_conv_dirty;
    u8 r;
    for (r = 0; r < SNES_CELL_ROWS; ++r) {
        const u16 m_lo = d[r * 2], m_hi = d[r * 2 + 1];
        u8 c = 0;
        if (!(m_lo | m_hi)) continue;
        while (c < 32) {
            u8 c0, n;
            const u8 bit = (c < 16) ? (u8)((m_lo >> c) & 1) : (u8)((m_hi >> (c - 16)) & 1);
            if (!bit) { ++c; continue; }
            c0 = c;
            while (c < 32) {
                const u8 b2 = (c < 16) ? (u8)((m_lo >> c) & 1) : (u8)((m_hi >> (c - 16)) & 1);
                if (!b2) break;
                ++c;
            }
            n = (u8)(c - c0);
            /* Eight DMAs of a run's rows: the ROM floor is on the A bus and
             * the frame behind WMDATA, so this is a DMA and not the byte
             * loop, which cost more than the cards it was making room for. */
            {
                u16 off = (u16)((u16)r * 2048 + (u16)c0 * 8);
                u8 k;
                for (k = 0; k < 8; ++k, off += SNES_FRAME_STRIDE)
                    snesFbWramDma((u16)(src + off), bank,
                                  (u16)(vp_rest.origin + off), 0x7E, (u16)n * 8);
            }
        }
    }
}

/* The cards whose rows touch pixel rows [y0, y1). */
static void draw_rest_cards(const u8 *faces, u16 y0, u16 y1)
{
    u8 row;
    for (row = 0; row < SNES_ROWS; ++row)
        snesDrawCardRow(&vp_rest, &cam, row, &faces[row * SNES_COLS],
                        rest_pose, y0, y1);
}

/* The ROM floor's occupied columns per cell row: the rest picture's span. */
static void rest_rowspan(void)
{
    const u8 *src = rest_pose ? snes_floor_rowspan_1 : snes_floor_rowspan_0;
    u8 *dst = snes_conv_rowspan;
    u8 i;
    for (i = 0; i < SNES_CELL_ROWS * 2; ++i) dst[i] = src[i];
}

/* Raised cards are drawn over the frame after everything else.  Their cells
 * are remembered so the next patch can put the board back under them. */
static void draw_held_card(void)
{
    const Msx2Side *s = &g_duel.side[MSX2_OWNER_PLAYER];
    u8 row = 0;
    const u8 slot = held_slot(&row);
    u8 card;
    SnesVert q[4];
    s16 lift, tilt;
    u16 x0, y0, x1, y1;
    if (slot == MSX2_SLOT_NONE || slot >= SNES_COLS || ui == UI_FUSE_TARGET)
        return;
    card = s->hand[chosen];
    if (card == MSX2_CARD_NONE) return;
    if (lower_frame) {
        /* On its way down: the hover and the lean both ease out to nothing
         * over the same curve the camera moves on. */
        const u16 t = ease_frac(lower_frame, LOWER_FRAMES);
        lift = view_lerp(HELD_LIFT, 0, t);
        tilt = view_lerp(HELD_TILT, 0, t);
    } else {
        lift = (s16)(HELD_LIFT + (snesSin(lift_phase) >> 4));
        tilt = HELD_TILT;
    }
    if (!snesCardQuad(&cam, &vp_rest, row, slot, rest_pose, lift, tilt, q))
        return;
    if (!snesQuadBounds(&vp_rest, q, &x0, &y0, &x1, &y1)) return;
    snesTexQuad(&vp_rest, q, face_of(card, 1));
    held_box[0] = (u8)(x0 >> 3);
    held_box[1] = (u8)(y0 >> 3);
    held_box[2] = (u8)((x1 - 1) >> 3);
    held_box[3] = (u8)((y1 - 1) >> 3);
    held_drawn = 1;
    cells_box(held_box[0], held_box[1], held_box[2], held_box[3]);
    mask_box(snes_conv_rom, 0, held_box[0], held_box[1], held_box[2], held_box[3]);
}

static void rest_invalidate(void)
{
    rest_valid = 0;
    held_drawn = 0;
    board_dirty = 1;
}

/* The resting picture, baked or patched, with the marker and the held card
 * on it.  Leaves the dirty masks naming every cell the converter must take. */
static void render_rest(u8 marker_row, u8 marker_col, u8 marker_colour)
{
    u8 faces[20];
    u8 i;
    const u8 mirror = (u8)(board_yaw == 128);

    slot_boxes(mirror);
    current_faces(faces);

    if (!rest_valid || rest_pose != mirror) {
        /* The whole picture: the ROM floor, then every card. */
        rest_pose = mirror;
        snesFbWramDma((u16)(u16)(mirror ? snes_floor_chunky_1 : snes_floor_chunky_0),
                      mirror ? SNES_FLOOR_CHUNKY_BANK_1 : SNES_FLOOR_CHUNKY_BANK_0,
                      vp_rest.origin, 0x7E, SNES_FRAME_W * SNES_FRAME_H);
        cells_all();
        if (marker_col != MSX2_SLOT_NONE && marker_col < SNES_COLS)
            snesDrawSlotMarker(&vp_rest, &cam, marker_row, marker_col, mirror,
                               marker_colour);
        draw_rest_cards(faces, 0, SNES_FRAME_H);
        rest_valid = 1;
        held_drawn = 0;
    } else {
        /* A patch: the slots that changed, the marker's two slots, the held
         * card's old cells.  The floor goes back under all of them first,
         * then the marker, then the cards of the rows involved. */
        u16 y0, y1;
        for (i = 0; i < 20; ++i)
            if (faces[i] != baked_faces[i])
                cells_slot(mirror, i / SNES_COLS, i % SNES_COLS);
        if (baked_marker_row != 0xFF)
            cells_slot(mirror, baked_marker_row, baked_marker_col);
        if (marker_col != MSX2_SLOT_NONE && marker_col < SNES_COLS)
            cells_slot(mirror, marker_row, marker_col);
        if (held_drawn) {
            cells_box(held_box[0], held_box[1], held_box[2], held_box[3]);
            held_drawn = 0;
        }
        if (cells_any()) {
            restore_floor_cells();
            if (marker_col != MSX2_SLOT_NONE && marker_col < SNES_COLS)
                snesDrawSlotMarker(&vp_rest, &cam, marker_row, marker_col, mirror,
                                   marker_colour);
            cells_rows(&y0, &y1);
            draw_rest_cards(faces, y0, y1);
        }
    }
    for (i = 0; i < 20; ++i) baked_faces[i] = faces[i];
    if (marker_col != MSX2_SLOT_NONE && marker_col < SNES_COLS) {
        baked_marker_row = marker_row;
        baked_marker_col = marker_col;
        baked_marker_colour = marker_colour;
    } else {
        baked_marker_row = baked_marker_col = 0xFF;
    }
    /* What the ROM floor can supply unconverted: every resting cell no
     * card, marker or held card is drawn over. */
    rom_cells_all();
    for (i = 0; i < 20; ++i)
        if (faces[i] != SNES_CARD_NONE_FACE)
            rom_cells_clear_slot(mirror, i / SNES_COLS, i % SNES_COLS);
    if (baked_marker_row != 0xFF)
        rom_cells_clear_slot(mirror, baked_marker_row, baked_marker_col);
    snesConvSetFloor((u16)(u16)(mirror ? snes_floor_planar_1 : snes_floor_planar_0),
                     mirror ? SNES_FLOOR_PLANAR_BANK_1 : SNES_FLOOR_PLANAR_BANK_0);
    rest_rowspan();
    if (show_cards) draw_held_card();
    /* The held card stands above its slot, so its cells can reach outside
     * the flat floor's spans; a cell outside the row's span is one the
     * converter leaves blank. */
    if (held_drawn) {
        u8 r;
        for (r = held_box[1]; r <= held_box[3]; ++r) {
            u8 *e = &snes_conv_rowspan[(u16)r << 1];
            const u8 x0 = (u8)(held_box[0] << 3), x1 = (u8)((held_box[2] << 3) | 7);
            if (x0 < e[0]) e[0] = x0;
            if (x1 > e[1]) e[1] = x1;
        }
    }
}

/* ── The transaction ─────────────────────────────────────────────────────── */

static void motion_sequence_begin(u8 frames)
{
    (void)frames;
    camera_rest = 0;
}

/* Render, convert and queue one complete frame.  Only one transaction may
 * be pending: the inactive map and the tiles it frees are reused only after
 * the previous one has been switched to. */
static void render_camera_frame(u8 marker_row, u8 marker_col, u8 marker_colour)
{
    u16 clock0, map_done, occupied;
    while (snesFbFramesPending()) { }

    clock0 = snesClock();
    snesRasterTarget(0x7E);

    if (pattern_mode) {
        u16 y;
        snesSpanFill(vp_rest.origin, SNES_FRAME_W * SNES_FRAME_H, BACKDROP);
        for (y = 16; y < 128; ++y) {
            u16 x;
            u8 *row = &snes_frame_fb[y * SNES_FRAME_STRIDE];
            for (x = 32; x < 224; ++x) row[x] = (u8)(x + y);
        }
        for (y = 0; y < SNES_CELL_ROWS; ++y) {
            snes_conv_rowspan[y * 2] = (y >= 2 && y < 16) ? 32 : 255;
            snes_conv_rowspan[y * 2 + 1] = (y >= 2 && y < 16) ? 223 : 0;
        }
        cells_all();
        rom_cells_none();
        rest_valid = 0;
    } else if (camera_rest) {
        render_rest(marker_row, marker_col, marker_colour);
    } else {
        update_board_texture();
        sync_cam_move();
        snesDrawCameraFloor(&vp_move, &cam_move, BACKDROP);
        cells_all();
        rom_cells_none();
        rest_valid = 0;
        held_drawn = 0;
    }

    map_done = snesClock();
    requested_generation = snesVideoRequestGeneration();
    snesConvSetHalf((u16)(!pattern_mode && !camera_rest));
    occupied = snesConvFrame(next_pool, requested_generation);
    if (occupied) next_pool ^= 1;
    cells_clear();
    /* Into locals, not the stamp: the stamp is only written whole, with its
     * checksum, at the end of a game frame, or a WRAM dump taken during a
     * ten-field render reads as torn. */
    t_map = (u16)(map_done - clock0);
    t_render = (u16)(snesClock() - clock0);
    t_conv = (u16)(t_render - t_map);
    board_dirty = 0;
}

static void motion_frame(void)
{
    camera_rest = 0;
    render_camera_frame(0, MSX2_SLOT_NONE, MARK_YOU);
    if (t_render > t_turn_max) t_turn_max = t_render;
}

/* ── The resumable camera frame ──────────────────────────────────────────── */

/* A LIFT FRAME IS PAINTED A FEW ROWS A DISPLAY FIELD.  A moving frame is
 * forty-odd fields of mapping and conversion, and painted in one call it
 * froze the sprite layer for all of them: the hand jumped four times on its
 * way off the screen and nothing answered the pad.  Here the same work is a
 * small state machine stepped once a game frame -- the texture, the floor's
 * rows, the converter's cell rows, the map -- with each step sized to fit
 * inside a field, so the main loop's vblank service (OAM, the drain) runs
 * between every two.  The picture the PPU shows is the previous complete
 * generation until the new map is switched to, as before. */
enum SnesCamJob {
    JOB_IDLE = 0,
    JOB_WAIT,           /* the previous transaction has not been shown */
    JOB_TEXTURE,        /* stamping cards into the world texture */
    JOB_CLEAR,          /* the backdrop fill */
    JOB_MAP_BEGIN,      /* walls, edges */
    JOB_MAP_ROWS,       /* the floor, JOB_ROWS_PER_STEP rows a step */
    JOB_CONV_BEGIN,     /* count the span, copy the map */
    JOB_CONV_ROWS,      /* one cell row a step */
    JOB_CONV_END        /* queue the map */
};
/* THE STEPS ARE SMALL AND THE HAND MOVES BETWEEN THEM.  Two floor rows are
 * a quarter of a field of the walker and eight cells a third of one of the
 * converter; job_slice takes steps until a couple of fields have gone by,
 * sliding the hand's sprites in place after each and having the NMI upload
 * them, so the hand glides at the field rate while the board arrives four
 * times, and the pad is read between slices.  (One step a game frame was
 * tried: the field-boundary wait and the sprite rebuild a step cost more
 * than the rendering, and the lift took a thousand fields.) */
#define JOB_ROWS_PER_STEP   2
#define JOB_HALF_ROWS_PER_STEP 4    /* a motion-frame row is a quarter the texels */
#define JOB_CELLS_PER_STEP  8
#define JOB_CARDS_PER_STEP  3

static u8  job_phase = JOB_IDLE;
static u8  job_half = 0;        /* the pose is a 128x72 motion frame */
static u16 job_y = 0, job_y1 = 0;
static u8  job_row = 0, job_col = 0;
static u8  job_card = 0;
static u16 job_clock0 = 0, job_map0 = 0;
static u16 job_steps = 0, job_steps_total = 1;

static u8 job_active(void) { return job_phase != JOB_IDLE; }

/* How far through the current frame the job is, Q8.8: what the sprite
 * layer animates by while the board is on its way. */
static u16 job_progress(void)
{
    if (!job_active()) return SNES_ONE;
    if (job_steps >= job_steps_total) return SNES_ONE;
    return snesUQDiv(job_steps, job_steps_total);
}

/* `half`: render the pose as a 128x72 motion frame (a camera on its way);
 * otherwise at 1:1 (the overhead view itself, where the camera comes to
 * rest -- "128x72 only while the camera moves"). */
static void job_begin(u8 half)
{
    camera_rest = 0;
    job_half = half;
    job_phase = JOB_WAIT;
    job_steps = 0;
    /* Floor rows, converter cells, and the two ends. */
    job_steps_total = half
        ? (u16)(SNES_FRAME_H / 2 / JOB_HALF_ROWS_PER_STEP +
                SNES_CELL_ROWS * (SNES_CELL_COLS / (2 * JOB_CELLS_PER_STEP)) + 3)
        : (u16)(SNES_FRAME_H / JOB_ROWS_PER_STEP +
                SNES_CELL_ROWS * (SNES_CELL_COLS / JOB_CELLS_PER_STEP) + 3);
}

/* One step: returns 1 while the frame is still on its way. */
static u8 job_step(void)
{
    u16 n;
    switch (job_phase) {
    case JOB_WAIT:
        if (snesFbFramesPending()) return 1;
        job_clock0 = snesClock();
        snesRasterTarget(0x7E);
        job_card = 0;
        job_phase = texture_stale() ? JOB_TEXTURE : JOB_CLEAR;
        return 1;
    case JOB_TEXTURE:
        for (n = 0; n < JOB_CARDS_PER_STEP && job_card < 20; ++n, ++job_card)
            texture_stamp(job_card);
        if (job_card >= 20) job_phase = JOB_CLEAR;
        return 1;
    case JOB_CLEAR:
        ++job_steps;
        if (job_half)
            snesFbWramFill(vp_move.origin, vp_move.bank,
                           (u16)(vp_move.stride * vp_move.h), BACKDROP);
        else
            snesFbWramFill(vp_rest.origin, vp_rest.bank,
                           (u16)(SNES_FRAME_W * SNES_FRAME_H), BACKDROP);
        job_phase = JOB_MAP_BEGIN;
        return 1;
    case JOB_MAP_BEGIN:
        ++job_steps;
        sync_cam_move();
        if (job_half ? snesDrawCameraFloorBegin(&vp_move, &cam_move, BACKDROP, 0, &job_y, &job_y1)
                     : snesDrawCameraFloorBegin(&vp_rest, &cam, BACKDROP, 0, &job_y, &job_y1))
            job_phase = JOB_MAP_ROWS;
        else
            job_phase = JOB_CONV_BEGIN;
        return 1;
    case JOB_MAP_ROWS:
        ++job_steps;
        n = (u16)(job_y1 - job_y);
        if (job_half) {
            if (n > JOB_HALF_ROWS_PER_STEP) n = JOB_HALF_ROWS_PER_STEP;
        } else if (n > JOB_ROWS_PER_STEP) {
            n = JOB_ROWS_PER_STEP;
        }
        snesFloorRowsPitch(job_y, (u16)(job_y + n));
        job_y = (u16)(job_y + n);
        if (job_y >= job_y1) job_phase = JOB_CONV_BEGIN;
        return 1;
    case JOB_CONV_BEGIN:
        ++job_steps;
        cells_all();
        rom_cells_none();
        rest_valid = 0;
        held_drawn = 0;
        job_map0 = snesClock();
        requested_generation = snesVideoRequestGeneration();
        snesConvSetHalf(job_half);
        if (!snesConvBegin(next_pool, requested_generation)) {
            /* Over the cell budget: the frame is refused (the stamp counts
             * it) and the previous picture stays up. */
            cells_clear();
            job_phase = JOB_IDLE;
            board_dirty = 0;
            return 0;
        }
        job_row = 0;
        job_col = 0;
        job_phase = JOB_CONV_ROWS;
        return 1;
    case JOB_CONV_ROWS:
        ++job_steps;
        if (job_half) {
            /* A motion frame's cells are half the work: twice as many a step. */
            snesConvCells(job_row, job_col, (u16)(job_col + 2 * JOB_CELLS_PER_STEP));
            job_col = (u8)(job_col + 2 * JOB_CELLS_PER_STEP);
        } else {
            snesConvCells(job_row, job_col, (u16)(job_col + JOB_CELLS_PER_STEP));
            job_col = (u8)(job_col + JOB_CELLS_PER_STEP);
        }
        if (job_col >= SNES_CELL_COLS) {
            job_col = 0;
            if (++job_row >= SNES_CELL_ROWS) job_phase = JOB_CONV_END;
        }
        return 1;
    case JOB_CONV_END:
        snesConvEnd();
        next_pool ^= 1;
        cells_clear();
        t_map = (u16)(job_map0 - job_clock0);
        t_render = (u16)(snesClock() - job_clock0);
        t_conv = (u16)(t_render - t_map);
        if (t_render > t_turn_max) t_turn_max = t_render;
        board_dirty = 0;
        job_phase = JOB_IDLE;
        return 0;
    default:
        return 0;
    }
}

/* A SLICE IS A MEASURED ALLOWANCE OF STEPS, NOT ONE STEP.  A step is a
 * quarter to half a field, and one step a game loop -- each loop being a
 * vblank wait plus the sprite layer -- made a pose two hundred fields.  The
 * slice takes steps until JOB_SLICE_LINES (three and a half fields) of the
 * stopwatch have gone by, sliding the hand between them, then hands the
 * loop back for the pad, the OAM and the audio queue.  What is lost is the
 * tail of the last step past the vblank and the wait for the next one,
 * about a fifth of a field a slice; measured, a pose that is 50 fields of
 * work synchronously is 60 this way, against 85 with a two-field slice. */
#define JOB_SLICE_LINES 900
static u8 job_slice(void)
{
    const u16 t0 = snesClock();
    u16 slid = snes_vblank_count;
    for (;;) {
        u16 now;
        /* The previous pose is still on its way up: that is the main
         * loop's vblank service (snesVideoPresent), not something to spin
         * on here. */
        if (job_phase == JOB_WAIT && snesFbFramesPending()) return 1;
        if (!job_step()) break;
        now = snesClock();
        /* The hand's row moves under half a pixel a field, and the slide
         * is a tenth of a step (the eased progress divides twice): sliding
         * after every step was a sixth of the whole lift, once a field a
         * tenth.  Every other field is not visible. */
        if ((u16)(snes_vblank_count - slid) >= 2) {
            slid = snes_vblank_count;
            slide_hand();
        }
        if ((u16)(now - t0) >= JOB_SLICE_LINES) return 1;
    }
    return 0;
}

/* THE MAIN LOOP DOES NOT WAIT FOR VBLANK WHILE A POSE IS BEING RENDERED.
 * The wait is there to pace a scene, and a slice that ends just after a
 * vblank would idle most of a field for nothing; the NMI uploads the
 * hand's slid sprites on its own (snes_fb_oam_pending).  The loop waits
 * again when the pose is waiting for the previous one to be shown, since
 * that is vblank work. */
u8 snesDuelBusy(void)
{
    return (u8)((view_motion == VIEW_TO_TOP || view_motion == VIEW_TO_HAND) &&
                job_active() && job_phase != JOB_WAIT);
}

static void render(void)
{
    u8 row = 0;
    u8 slot = cursor_board_slot(&row);
    const u8 colour = (ui == UI_DEFENDER) ? MARK_COM : MARK_YOU;
    /* Overhead, the cursor is a sprite (build_objects): nothing to paint. */
    if (top_view) slot = MSX2_SLOT_NONE;
    render_camera_frame(row, slot, colour);
    if (ui == UI_PLACE || ui == UI_EQUIP_TARGET) {
        if (t_render > t_held_max) t_held_max = t_render;
    } else if (t_render > t_rest_max) {
        t_rest_max = t_render;
    }
}
/* ── The card check ──────────────────────────────────────────────────────── */

/* B checks the hovered hand card; A checks the top-view card. The enlarged
 * face has its own Mode 3 OBJ sheet. Closing restores the previous cursor
 * and UI, repainting the board when its bitmap becomes visible. */

/* THE BOARD'S UPLOADS STOP BEFORE VRAM CHANGES HANDS.  Whatever the sparse
 * presenter had queued -- tiles, a map, a completion -- is dropped
 * (snesFbCancel), so no board job can land in the card art's tiles; on the
 * way back snesVideoInitDuel resets the tile store and the board is baked
 * again from nothing. */
static void enter_mode3_art(void)
{
    snesFbCancel();
    if (ui == UI_CHECK) {
        snesVideoSetOwner(SNES_OWNER_CARD_CHECK);
        snesCardArtEnter(1);
        snesCardArtCheck(check_face, check_has_stats, check_atk, check_def);
        snesCardArtVblank();
        mode3_active = 1;
    } else {
        snesVideoSetOwner(SNES_OWNER_BATTLE);
        snesBattleBegin();
        snesBattleVblank();
        mode3_active = 2;
    }
    setScreenOn();
}

static void leave_mode3_art(void)
{
    setScreenOff();
    mode3_active = 0;
    snesVideoInitDuel();
    snesObjInit();
    snesVideoSetOwner(SNES_OWNER_BOARD);
    rest_invalidate();
    snesFbDrain(1);
    if (top_view) {
        /* The overhead pose at 1:1, whole, under force blank: the view a
         * check or an attack was opened from is the one it returns to,
         * not a doubled motion frame of it. */
        job_begin(0);
        while (job_step()) { }
    } else {
        render();
    }
    build_objects();
    /* The hand's tiles go up here, under force blank, and not a card a
     * vblank over the bake that follows (snesObjFlushBlank). */
    snesObjFlushBlank();
    while (!snesVideoPresentDone()) WaitForVBlank();
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
    top_view = 0;
    view_motion = VIEW_BOARD_REST;
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
    check_face = SNES_CARD_NONE_FACE;
    check_has_stats = 0;
    mode3_active = 0;
    battle_frame = 0;
    touch_board(4);
    leave_mode3_art();
}

/* The battle takes its snapshot of the rules' event as it begins
 * (snes_battle.c); the event is cleared only once the sequence is over. */
static void begin_battle_art(u8 return_ui)
{
    battle_return_ui = return_ui;
    battle_return_top = top_view;
    battle_frame = 0;
    ui = UI_BATTLE_ART;
    top_attacker = MSX2_SLOT_NONE;
    enter_mode3_art();
}

static void end_battle_art(void)
{
    ui = battle_return_ui;
    top_view = battle_return_top;
    view_motion = top_view ? VIEW_TOP_REST : VIEW_BOARD_REST;
    Msx2_ClearActionEvent();
    touch_board(24);
    leave_mode3_art();
}

/* ── The HUD band ────────────────────────────────────────────────────────── */

/* THE BAND IS NOT PART OF THE BOARD AT ALL.
 *
 * BG1 stops at line 144 (an HDMA write to TM), and the hand and the two rows
 * of words under it are sprites over the backdrop, which an HDMA channel
 * tints per scanline into the blue plate -- see snes_video.c.  That is what
 * lets the plate be a five-bit-a-channel gradient instead of the four blues
 * direct colour has. */

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
    /* THE OPPONENT'S CARD IS NAMED AS IT FLIES, as the PC's is (its
     * draw_flying_card shows the face): from the moment it leaves the hand
     * until it has landed, the name row and the stat row are its. */
    if (ui == UI_COM && com_present >= COM_PRESENT_FLY &&
        com_card != MSX2_CARD_NONE) {
        *face = face_of(com_card, 1);
        return com_card;
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
 * A list that is never patched cannot keep a sprite belonging to a screen
 * the player has left -- which is the failure the top view would otherwise
 * produce on every entry, since the two views share the same twenty card
 * sprites.  It is NOT free, though: measured with the cycle profiler
 * (tools/snes/prof.py), a hundred sprites through 816-tcc were 1.3 fields
 * a frame -- a resting board ran at thirty frames a second for its HUD
 * alone -- so everything per sprite (the store, text, digits, the life
 * panel, the card, snesObjEnd's scans) is snes_oam.asm's, on the fast
 * direct page, and this function is the part that is still C: a rest
 * frame is now three quarters of a field.
 *
 * IT IS BUILT FROM THE FRONT BACKWARDS.  Every sprite in this port is priority
 * 3, so between two that overlap the one with the LOWER OAM index is the one
 * seen: the banner and the text go in first, then the plates they sit on, then
 * the cursor, and the cards last of all. */
/* The hand's sprites, by OAM index, so the lift can move them without a
 * rebuild: the card, its selection box (four corners, or 0xFF), and whether
 * it bobs. */
static u8 hand_oam[MSX2_HAND] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static u8 hand_oam_box[MSX2_HAND] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static u8 hand_oam_bob[MSX2_HAND] = { 0, 0, 0, 0, 0 };

/* Where the hand sits this frame: at rest, or on its way off the bottom of
 * the screen during the lift and back during the descent. */
static s16 hand_row_y(void)
{
    if (view_motion == VIEW_TO_TOP)
        return (s16)(HAND_Y + (s16)(((u16)VIEW_HAND_OFFSET *
                                     view_anim_ease(view_anim_frame)) >> 8));
    if (view_motion == VIEW_TO_HAND)
        return (s16)(HAND_Y + (s16)(((u16)VIEW_HAND_OFFSET *
                                     (SNES_ONE - view_anim_ease(view_anim_frame))) >> 8));
    return HAND_Y;
}

/* Slide the hand's sprites to this frame's row, in place. */
static void slide_hand(void)
{
    const s16 y = hand_row_y();
    u8 i;
    snesObjTouch();
    /* The NMI's own copy of the shadow is for the slices, when the main
     * loop is not waiting for vblank and snesObjVblank does not run.  When
     * it IS waiting (a pose waiting on the previous one's upload), the
     * library's copy, this one and snesObjVblank's would be three OAM DMAs
     * in one vblank: the NMI drain then found the V counter past its
     * deadline every field, nothing went up, and the wait never ended. */
    if (snesDuelBusy()) snes_fb_oam_pending = 1;
    for (i = 0; i < MSX2_HAND; ++i) {
        s16 cy = y;
        if (hand_oam[i] == 0xFF) continue;
        if (hand_oam_bob[i]) cy = (s16)(cy - (snesSin(bob_phase) >> 6));
        snesObjPatchY(hand_oam[i], cy, 1);
        if (hand_oam_box[i] != 0xFF) {
            const u8 b = hand_oam_box[i];
            snesObjPatchY(b, (s16)(cy - 4), 0);
            snesObjPatchY((u8)(b + 1), (s16)(cy - 4), 0);
            snesObjPatchY((u8)(b + 2), (s16)(cy + 28), 0);
            snesObjPatchY((u8)(b + 3), (s16)(cy + 28), 0);
        }
    }
}

static void build_objects(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    const u8 hand_owner = (ui == UI_COM) ? g_duel.turn_owner : MSX2_OWNER_PLAYER;
    /* Constant indices only: 816-tcc's `&g_duel.side[hand_owner]` came out
     * as the player's side whatever hand_owner held, and the opponent's row
     * showed five backs after it had played one. */
    const Msx2Side *hand_side = (hand_owner == MSX2_OWNER_COM)
                              ? &g_duel.side[MSX2_OWNER_COM]
                              : &g_duel.side[MSX2_OWNER_PLAYER];
    /* The opponent's hand as it was BEFORE its action while that action is
     * being presented, the live hand otherwise. */
    u8 shown_hand[MSX2_HAND];
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
    /* A sword and a shield both at nought describe nothing: the row shows
     * the prompt instead. */
    if (has_stats && atk == 0 && def == 0) has_stats = 0;
    hand_y = hand_row_y();
    for (i = 0; i < MSX2_HAND; ++i) {
        if (hand_owner == MSX2_OWNER_COM && com_present != COM_PRESENT_IDLE)
            shown_hand[i] = com_hand[i];
        else
            shown_hand[i] = hand_side->hand[i];
    }
    /* A message pre-empts the name, because it is the thing that just
     * happened; the name is back the moment it expires. */
    const char *name = message ? message
                     : (top_view && top_attacker != MSX2_SLOT_NONE) ? "A:HIT  B:CANCEL"
                     : (face != SNES_CARD_NONE_FACE) ? snesCardName(face)
                     : prompt_text();

    snesObjBegin();

    /* Card inspection and attacks are 2D Mode 3 scenes.  They deliberately
     * use the normal OBJ card cache, so no Mode 7 framebuffer is sampled or
     * rewritten while the presentation is visible. */
    if (mode3_active) {
        /* The card check is BG1/BG2 pictures (snes_cardart.c); the sprite
         * list is simply empty while it is up. */
        snesObjEnd();
        return;
    }

    if (ui == UI_RESULT) build_result_banner();

    if (top_view) {
        /* Overhead inspection is the same board through the same renderer,
         * seen from straight above; the HUD rows are where they always are.
         * THE CURSOR IS A SPRITE: the slot's four corners projected through
         * the overhead camera and bracketed in the red the PC-FX and FM
         * TOWNS builds use, which costs no render at all -- the overhead
         * frame is the expensive kind and is painted exactly once. */
        s16 x0, y0, x1, y1;
        s16 cx, cz;
        snesSlotCentre(top_row, top_col, 0, &cx, &cz);
        /* The slot's far-left and near-right corners through THE SAME CAMERA
         * THE BOARD WAS RENDERED WITH -- the lift's top pose, which is where
         * `cam` rests for as long as the overhead view is up -- so the
         * bracket lands on the slot wherever the overhead pose is put. */
        if (snesProject(&cam, &vp_rest, (s16)(cx - 128), (s16)(cz + 128), 0, &x0, &y0) &&
            snesProject(&cam, &vp_rest, (s16)(cx + 128), (s16)(cz - 128), 0, &x1, &y1) &&
            x1 > x0 && y1 > y0 && x1 - x0 < 128 && y1 - y0 < 128)
            snesObjBoxRed(x0, y0, (u8)(x1 - x0), (u8)(y1 - y0));
        /* The attacker already picked keeps the hand's gold bracket while
         * the red one walks the opponent's row for its target. */
        if (top_attacker != MSX2_SLOT_NONE) {
            snesSlotCentre(SNES_ROW_YOU_MONSTER, top_attacker, 0, &cx, &cz);
            if (snesProject(&cam, &vp_rest, (s16)(cx - 128), (s16)(cz + 128), 0, &x0, &y0) &&
                snesProject(&cam, &vp_rest, (s16)(cx + 128), (s16)(cz - 128), 0, &x1, &y1) &&
                x1 > x0 && y1 > y0 && x1 - x0 < 128 && y1 - y0 < 128)
                snesObjBox(x0, y0, (u8)(x1 - x0), (u8)(y1 - y0));
        }
        snesObjText(8, NAME_Y, name);
        if (has_stats) {
            snesObjIcon(STAT_ATK_X, STAT_Y, SNES_SPR_ICON_ATK);
            snesObjNum(STAT_ATK_X + STAT_NUM_DX, STAT_Y, atk, 4);
            snesObjIcon(STAT_DEF_X, STAT_Y, SNES_SPR_ICON_DEF);
            snesObjNum(STAT_DEF_X + STAT_NUM_DX, STAT_Y, def, 4);
        } else {
            snesObjText(STAT_ATK_X, STAT_Y, top_attacker != MSX2_SLOT_NONE
                                            ? "PICK A TARGET" : "A:CHECK B:BACK");
        }
        snesObjLifePanel(LP_YOU_X, LP_Y, 0, (u16)you->lp, MSX2_START_LP);
        snesObjLifePanel(LP_COM_X, LP_Y, 1,
                         (u16)g_duel.side[MSX2_OWNER_COM].lp, MSX2_START_LP);

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
        } else if (name != prompt_text()) {
            /* (With no card under the cursor the name row IS the legend,
             * and "OPPONENT'S TURN" printed twice, one under the other,
             * was what that looked like.) */
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
        for (i = 0; i < MSX2_HAND; ++i) hand_oam[i] = hand_oam_box[i] = 0xFF;
        for (i = 0; ui != UI_CHECK && i < MSX2_HAND; ++i) {
            s16 x = (s16)(HAND_X0 + i * HAND_PITCH);
            s16 y = hand_y;
            const u8 hcard = shown_hand[i];
            const u8 queued = (u8)(hand_owner == MSX2_OWNER_PLAYER &&
                                   queue_order(i) != 0);
            const u8 selected = (hand_owner == MSX2_OWNER_COM)
                              ? (com_present != COM_PRESENT_IDLE &&
                                 com_cursor == i)
                              : (ui == UI_HAND)
                              ? (cursor == i)
                              : (chosen == i && ui != UI_ATTACKER &&
                                 ui != UI_DEFENDER && ui != UI_COM);
            /* TWO TESTS, NOT ONE `||`: 816-tcc compiled the pair with the
             * first operand's true branch jumping INTO the body (its .ps
             * says "ERROR no jump found to patch"), so an emptied slot was
             * drawn as whatever the sheet still held. */
            if (hcard == MSX2_CARD_NONE) continue;
            if (!(hand_visible[hand_owner] & (1u << i))) continue;
            /* A CARD BEING PLAYED IS NOT IN THE HAND ANY MORE.  While the
             * player is choosing its slot it hovers over the board as the
             * held card (draw_held_card), and a fusion chain's cards are
             * spoken for the moment their target is being chosen; the hand
             * sprite would be a second copy.  B puts the card back, and the
             * same rule -- decided from the UI state alone -- shows it
             * again.  The flight sprite is the exception: it IS the card,
             * on its way down. */
            if (hand_owner == MSX2_OWNER_COM && com_present == COM_PRESENT_FLY &&
                i == com_hand_slot) {
                const u16 t = ease_frac(com_present_field, COM_FLY_FIELDS);
                s16 wx, wz, sx, sy;
                const u8 com_row = (com_action == MSX2_ACTION_PLACE ||
                                    com_action == MSX2_ACTION_FUSION)
                                 ? SNES_ROW_COM_MONSTER : SNES_ROW_COM_SUPPORT;
                /* WORLD coordinates: snesProject applies the camera's yaw
                 * itself, so a mirrored slot centre would be turned twice
                 * and the card would fly to the opposite column. */
                snesSlotCentre(com_row, com_field_slot, 0, &wx, &wz);
                if (snesProject(&cam, &vp_rest, wx, wz, 0, &sx, &sy)) {
                    x = view_lerp(x, (s16)(sx - 16), t);
                    y = view_lerp(y, (s16)(sy - 16), t);
                }
            } else if (hand_owner == MSX2_OWNER_PLAYER &&
                ((i == chosen && (ui == UI_PLACE || ui == UI_EQUIP_TARGET)) ||
                 (queued && ui == UI_FUSE_TARGET)))
                continue;
            if (hand_y >= 224) {
                continue;
            } else if (selected) {
                /* A SMALL UP AND DOWN, once a field.  It is the same beat the
                 * PC build gives the selected card and it is what says which
                 * of the five the buttons are about. */
                y = (s16)(y - (snesSin(bob_phase) >> 6));
            }
            /* The opponent's backs are all alike; only the cursor says
             * which one it is thinking about, as on the PC. */
            snesObjCardGrey((u8)(hand_owner == MSX2_OWNER_PLAYER &&
                                 !selected && !queued));
            hand_oam_box[i] = 0xFF;
            hand_oam_bob[i] = selected;
            if (queued || selected) {
                hand_oam_box[i] = snesObjCount();
                /* The opponent's cursor is the PC's red one
                 * (draw_red_cursor); the player's stays gold. */
                if (hand_owner == MSX2_OWNER_COM)
                    snesObjBoxRed(x - 4, y - 4, 40, 40);
                else
                    snesObjBox(x - 4, y - 4, 40, 40);
            }
            hand_oam[i] = snesObjCount();
            if (hand_owner == MSX2_OWNER_COM && com_present == COM_PRESENT_FLY &&
                i == com_hand_slot)
                snesObjCard(x, y, COM_FLY_SLOT, face_of(com_card, 1));
            else
                snesObjCard(x, y, i, hand_owner == MSX2_OWNER_COM
                                      ? SNES_CARD_BACK : face_of(hcard, 1));
            if (snesObjCount() == hand_oam[i]) hand_oam[i] = 0xFF;
            snesObjCardGrey(0);
        }
    }
    snesObjEnd();
}

/* ── The measurement fixture ─────────────────────────────────────────────── */

#if defined(SNES_DEBUG)
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
#endif

/* ── The duel's end ──────────────────────────────────────────────────────── */

/* Where the duel goes once its verdict is dismissed: a won story duel
 * records its progress and opens the next talk (or the ending); anything
 * else is the title. */
static u8 result_scene(void)
{
    if (configured_story_mode && g_duel.result > 0) {
        const u8 next = (u8)(configured_story + 1);
        /* Story can begin directly from a fresh cartridge.  The default
         * deck is resident in the editor model, but SRAM is still
         * uninitialised until the player visits the editor; materialise it
         * before recording story progress. */
        if (!snesSaveIsValid()) snesDeckSaveCurrent();
        snesSaveStoryProgressStore(next);
        if (next >= MSX2_STORY_MAX_DUELS)
            return SNES_SCENE_ENDING;
        snesStoryBegin(next);
        return SNES_SCENE_STORY_TALK;
    }
    return SNES_SCENE_TITLE;
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

/* THE CARD IS LOWERED BEFORE THE RULES HEAR ABOUT IT.  A hovering card
 * (PLACE, EQUIP_TARGET) starts its descent here and place_chosen is called
 * when it is flat on the slot; a fusion chain's cards do not hover, so the
 * fusion is placed at once and the bake shows its result. */
static void begin_place_lower(u8 defense)
{
    if (ui == UI_FUSE_TARGET) {
        place_chosen(defense);
        return;
    }
    /* The first lowered pose is this frame's render, at the end of
     * snesDuelFrame; the frame loop steps the rest. */
    lower_def = defense;
    lower_frame = 1;
    touch_board(2);
}

static void place_chosen(u8 defense)
{
    u8 hand_before[MSX2_HAND];
    u8 hi;
    const u8 card = g_duel.side[MSX2_OWNER_PLAYER].hand[chosen];
    u8 placed = 0;
    u8 redrawn = 0;
    for (hi = 0; hi < MSX2_HAND; ++hi)
        hand_before[hi] = g_duel.side[MSX2_OWNER_PLAYER].hand[hi];
    if (ui == UI_FUSE_TARGET) {
        placed = Msx2_PlaceFusion(MSX2_OWNER_PLAYER, queue, queue_n, cursor,
                                  defense ? TRUE : FALSE);
        if (placed) queue_n = 0;
        else say("THEY DO NOT FUSE");
    } else if (Msx2_IsSupport(card)) {
        if (Msx2_PlaySupport(MSX2_OWNER_PLAYER, chosen, cursor)) {
            placed = 1;
            /* A DRAW SUPPORT LEAVES A NEW CARD IN ITS OWN SLOT (Msx2_PlaySupport
             * puts the drawn card where the support was).  note_draws only
             * sees a slot go from empty to full, so the slot is put through
             * the draw cadence here: hidden with the support, then dealt
             * with the draw sound like any other new card.  Without this
             * the slot was hidden and never shown again. */
            if (g_duel.side[MSX2_OWNER_PLAYER].hand[chosen] != MSX2_CARD_NONE)
                redrawn = 1;
        } else {
            say("CANNOT PLAY IT");
        }
    } else if (Msx2_PlaceMonster(MSX2_OWNER_PLAYER, chosen, cursor,
                                 defense ? TRUE : FALSE)) {
        placed = 1;
    } else {
        say("CANNOT PLACE IT");
    }
    if (placed) snesAudioSfx(SNES_SFX_CARD_PLACED);
    note_draws(MSX2_OWNER_PLAYER, hand_before);
    if (placed) hand_visible[MSX2_OWNER_PLAYER] &= (u8)~(1u << chosen);
    if (redrawn) draw_pending[MSX2_OWNER_PLAYER] |= (u8)(1u << chosen);
    Msx2_ClearActionEvent();
    ui = UI_HAND;
    cursor = chosen;
    touch_board(20);
}

static void note_draws(u8 owner, const u8 *before)
{
    u8 i;
    for (i = 0; i < MSX2_HAND; ++i) {
        if (g_duel.side[owner].hand[i] == MSX2_CARD_NONE)
            hand_visible[owner] &= (u8)~(1u << i);
        if (before[i] == MSX2_CARD_NONE &&
            g_duel.side[owner].hand[i] != MSX2_CARD_NONE) {
            hand_visible[owner] &= (u8)~(1u << i);
            draw_pending[owner] |= (u8)(1u << i);
        }
    }
}

static u8 step_draw_presentation(void)
{
    const u8 owner = (ui == UI_COM) ? g_duel.turn_owner : MSX2_OWNER_PLAYER;
    u8 i;
    if (!draw_pending[owner]) return 0;
    if (++draw_field < DRAW_FIELDS) return 1;
    draw_field = 0;
    for (i = 0; i < MSX2_HAND; ++i) {
        if (draw_pending[owner] & (1u << i)) {
            draw_pending[owner] &= (u8)~(1u << i);
            hand_visible[owner] |= (u8)(1u << i);
            snesAudioSfx(SNES_SFX_CARD_DRAWN);
            break;
        }
    }
    return 1;
}

static void begin_com_presentation(const u8 *before)
{
    u8 i;
    for (i = 0; i < MSX2_HAND; ++i) com_hand[i] = before[i];
    com_hand_slot = g_duel.last_action_hand_slot;
    com_field_slot = g_duel.last_action_field_slot;
    com_card = g_duel.last_action_card;
    com_defense = g_duel.last_action_defense;
    com_action = g_duel.last_action;
    com_present_field = 0;
    com_cursor = 0;
    com_present = COM_PRESENT_SELECT;
    /* The rules have already taken the card out of the hand and note_draws
     * hid the empty slot; the presentation shows the BEFORE hand, so the
     * chosen back stays until its flight has landed. */
    if (com_hand_slot < MSX2_HAND)
        hand_visible[MSX2_OWNER_COM] |= (u8)(1u << com_hand_slot);
    if (com_card != MSX2_CARD_NONE)
        snesObjQueueCard(COM_FLY_SLOT, face_of(com_card, 1), 1);
}

static u8 step_com_presentation(void)
{
    if (com_present == COM_PRESENT_IDLE) return 0;
    if (com_present == COM_PRESENT_SELECT) {
        if (++com_present_field >= COM_SELECT_FIELDS) {
            com_present_field = 1;
            com_present = COM_PRESENT_FLY;
        } else if (com_present_field < COM_SELECT_SETTLE) {
            com_cursor = (u8)((com_present_field >> 2) % MSX2_HAND);
        } else {
            com_cursor = com_hand_slot;
        }
    } else if (com_present == COM_PRESENT_FLY) {
        if (++com_present_field > COM_FLY_FIELDS) {
            hand_visible[MSX2_OWNER_COM] &= (u8)~(1u << com_hand_slot);
            snesAudioSfx(SNES_SFX_CARD_PLACED);
            Msx2_ClearActionEvent();
            touch_board(12);
            render();
            com_present = COM_PRESENT_LAND;
        }
    } else if (snesVideoPresentDone()) {
        com_present = COM_PRESENT_IDLE;
        com_hand_slot = com_field_slot = MSX2_SLOT_NONE;
    }
    return 1;
}

/* The PC's per-frame UI pass (src/main.c): A confirms, B and START are the
 * alternate; the cursor sound is move_cursor's.  Every player-driven state
 * goes through here, so the individual actions never sound their own press. */
static void press_sfx(u16 down)
{
    if (down & KEY_A) snesAudioSfx(SNES_SFX_CONFIRM);
    if (down & (KEY_B | KEY_START)) snesAudioSfx(SNES_SFX_CONFIRM_ALT);
}

static void step_player(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    const u16 down = padsDown(0);

    press_sfx(down);
    switch (ui) {
    case UI_HAND:
        move_cursor(MSX2_HAND, 0);
        /* DOWN IS THE FUSION CHAIN.  The hand row is the bottom of the board
         * view, so down means nothing else there, and it is the gesture the
         * MSX2 build uses for the same thing over the same rules model. */
        if (down & KEY_DOWN) {
            if (you->hand[cursor] == MSX2_CARD_NONE) {
                say("NO CARD THERE");
            } else if (you->monster_played) {
                /* A fusion is the turn's one summon too (Msx2_PlaceFusion
                 * refuses it, MSX2_FUSE_SPENT); the chain is not opened. */
                say("ONE MONSTER A TURN");
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
                    /* Refused in the hand when it cannot land anywhere,
                     * rather than after the pick and the descent. */
                    if (Msx2_LiveMonsterCount(MSX2_OWNER_PLAYER) == 0) {
                        say("NO MONSTER TO EQUIP");
                    } else if (Msx2_FirstFreeEquipSlot(MSX2_OWNER_PLAYER) == MSX2_SLOT_NONE) {
                        say("NO ROOM FOR IT");
                    } else {
                        ui = UI_EQUIP_TARGET;
                        cursor = Msx2_FirstLiveSlot(MSX2_OWNER_PLAYER);
                        if (cursor == MSX2_SLOT_NONE) cursor = 0;
                        touch_board(8);
                    }
                } else {
                    cursor = MSX2_SLOT_NONE;
                    place_chosen(0);
                }
            } else if (you->monster_played) {
                /* THE SECOND MONSTER IS REFUSED IN THE HAND, not on the
                 * board: the PC greys the hand out (player_can_place_monster)
                 * and the rules refuse the placement either way, so picking
                 * the card up, carrying it to a slot and being told there
                 * was the same answer with three more presses in it. */
                say("ONE MONSTER A TURN");
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
                begin_place_lower(0);
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
        if (down & KEY_A) begin_place_lower(0);
        else if (down & KEY_X) begin_place_lower(1);
        else if (down & KEY_B) {
            ui = UI_HAND;
            cursor = chosen;
            touch_board(8);
        }
        break;

    case UI_EQUIP_TARGET:
        move_cursor(MSX2_FIELD, 1);
        if (down & KEY_A) {
            if (Msx2_IsMonster(you->field[cursor])) begin_place_lower(0);
            else say("NO MONSTER THERE");
        } else if (down & KEY_B) {
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
    com_present = COM_PRESENT_IDLE;
    com_present_field = 0;
    com_hand_slot = com_field_slot = MSX2_SLOT_NONE;
    com_action = MSX2_ACTION_NONE;
    hand_visible[MSX2_OWNER_PLAYER] = hand_visible[MSX2_OWNER_COM] = 0;
    draw_pending[MSX2_OWNER_PLAYER] = draw_pending[MSX2_OWNER_COM] = 0;
    draw_field = 0;
    {
        u8 owner, i;
        for (owner = 0; owner < 2; ++owner)
            for (i = 0; i < MSX2_HAND; ++i)
                if (g_duel.side[owner].hand[i] != MSX2_CARD_NONE)
                    draw_pending[owner] |= (u8)(1u << i);
    }
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
    top_attacker = MSX2_SLOT_NONE;
    battle_return_top = 0;
    intro_fade = INTRO_FADE_FIELDS;
    fade_level = 0;
    fade_dirty = 1;
    bob_phase = 0;
    queue_n = 0;
    lower_frame = 0;
    over_step = 0;
    check_face = SNES_CARD_NONE_FACE;
    check_has_stats = 0;
    texture_w = texture_h = 0;
    pattern_mode = 0;
    next_pool = 0;
    requested_generation = 0;
    rest_valid = 0;
    held_drawn = 0;
    slot_box_valid[0] = slot_box_valid[1] = 0;
    baked_marker_row = baked_marker_col = 0xFF;
    t_map = t_conv = t_render = 0;
    t_turn_max = t_rest_max = t_held_max = 0;
    set_viewport();
    set_rest_camera(0);
    /* The upload queue starts under force blank; the drain runs from the
     * first vblank on. */
    snesFbDrain(1);
    build_objects();
    render();
}

u8 snesDuelFrame(void)
{
    const u16 down = padsDown(0);

#if defined(SNES_DEBUG)
    /* THE HARNESS SWITCHES ARE A DEBUG BUILD'S (make DEBUG=1): the fixture
     * board, the card ablation, the self-playing demo and the pinned moving
     * camera.  A retail cartridge answers none of them. */
    if ((down & (KEY_R | KEY_SELECT)) == (KEY_R | KEY_SELECT)) {
        pattern_mode ^= 1;
        touch_board(2);
    } else {
        if (down & KEY_R) fixture_board();
        if (down & KEY_SELECT) {
            show_cards ^= 1;
            texture_w = 0;
            touch_board(2);
        }
    }
    if (down & KEY_L) {
        autoplay ^= 1;
        say(autoplay ? "DEMO ON" : "DEMO OFF");
    }
    if (down & KEY_Y) {
        force_moving ^= 1;
        if (!force_moving) set_rest_camera((u8)(board_yaw == 128));
        touch_board(2);
    }
#endif

    /* The opening fade: the board is already baked underneath it, and
     * nothing else -- no deal, no input -- happens until it is lit. */
    if (intro_fade) {
        u8 level;
        --intro_fade;
        level = (u8)(15 - (intro_fade * 15) / INTRO_FADE_FIELDS);
        if (level != fade_level) {
            fade_level = level;
            fade_dirty = 1;
        }
        if (intro_fade) {
            if (board_dirty) render();
            build_objects();
            goto stamp;
        }
    }

    ++bob_phase;
    if (ui == UI_BATTLE_ART) {
        /* One displayed field a game frame: nothing renders, so the loop
         * runs at sixty and the sequencer counts fields.  The step's cost is
         * stamped so the harness can hold it to a field. */
        const u16 before = snesClock();
        const u8 over = snesBattleStep(down);
        t_render = (u16)(snesClock() - before);
        /* A blow that decided the duel shows the verdict over its cards
         * (snes_battle.c) and the duel leaves from there. */
        if (over == 2) return result_scene();
        if (over) end_battle_art();
        else ++battle_frame;
        goto stamp;
    }
    if (view_motion == VIEW_BOARD_REST && ui == UI_HAND && (down & KEY_B)) {
        press_sfx(down);
        begin_check();
        if (ui == UI_CHECK) {
            build_objects();
            goto stamp;
        }
    }

    /* Inspection pauses the duel and restores the previous UI on close.
     * Consume the closing press here so it cannot also play a card. */
    if (ui == UI_CHECK) {
        if (down & (KEY_B | KEY_A | KEY_START)) { press_sfx(down); end_check(); }
        build_objects();
        goto stamp;
    }

    /* UP walks up into the tactical top view.  In the top view the d-pad is
     * the TABLE'S OWN CURSOR -- twenty slots the player can walk over and read
     * -- so B is what walks back down; DOWN there would otherwise cost the
     * cursor a whole axis of the board. */
    if (view_motion == VIEW_BOARD_REST && (down & KEY_UP) && !lower_frame &&
        !turn_frame && ui != UI_COM && ui != UI_RESULT) {
        snesAudioSfx(SNES_SFX_SELECT);
        begin_view_transition(1);
    } else if (g_duel.result == 0 &&
               (view_motion == VIEW_TOP_REST ||
                (view_motion == VIEW_TO_TOP && top_view))) {
        /* (A duel decided by an attack declared up here falls through to
         * the result banner below, over the overhead picture.) */
        u8 moved = 0;
        if (down & KEY_LEFT)  { top_col = (u8)((top_col + SNES_COLS - 1) % SNES_COLS); moved = 1; }
        if (down & KEY_RIGHT) { top_col = (u8)((top_col + 1) % SNES_COLS); moved = 1; }
        if (down & KEY_UP)    { top_row = (u8)((top_row + SNES_ROWS - 1) % SNES_ROWS); moved = 1; }
        if (down & KEY_DOWN)  { top_row = (u8)((top_row + 1) % SNES_ROWS); moved = 1; }
        if (moved) snesAudioSfx(SNES_SFX_SELECT);
        if (view_motion == VIEW_TO_TOP) {
            /* The sharp picture is still on its way: the cursor moves over
             * the doubled one, A and B wait. */
            step_view_transition();
            build_objects();
            goto stamp;
        }
        if (down & KEY_A) {
            const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
            const Msx2Side *com = &g_duel.side[MSX2_OWNER_COM];
            press_sfx(down);
            if (top_attacker != MSX2_SLOT_NONE) {
                /* The target.  The rules say whether the attack is legal
                 * (defence position, already attacked, the opening turn). */
                if (top_row != SNES_ROW_COM_MONSTER ||
                    !Msx2_IsMonster(com->field[top_col])) {
                    say("PICK A TARGET");
                } else if (!Msx2_Attack(MSX2_OWNER_PLAYER, top_attacker, top_col)) {
                    say("ILLEGAL ATTACK");
                } else {
                    chosen = top_attacker;
                    cursor = top_attacker;
                    begin_battle_art(ui == UI_ATTACKER ? UI_ATTACKER : UI_HAND);
                    goto stamp;
                }
            } else if (top_row == SNES_ROW_YOU_MONSTER &&
                       Msx2_IsMonster(you->field[top_col]) &&
                       (ui == UI_HAND || ui == UI_ATTACKER)) {
                /* One of the player's own monsters: pick it as the attacker,
                 * the PC's gesture, and A on anything else still checks it. */
                if (you->attacked[top_col]) {
                    say("ALREADY ATTACKED");
                } else if (you->defense[top_col]) {
                    say("IN DEFENCE");
                } else if (Msx2_FirstTurnAttackLocked()) {
                    say("NOT ON TURN ONE");
                } else if (Msx2_LiveMonsterCount(MSX2_OWNER_COM) == 0) {
                    /* Nothing to attack: the pick is the direct attack. */
                    if (Msx2_Attack(MSX2_OWNER_PLAYER, top_col, MSX2_SLOT_NONE)) {
                        chosen = top_col;
                        cursor = top_col;
                        begin_battle_art(ui == UI_ATTACKER ? UI_ATTACKER : UI_HAND);
                        goto stamp;
                    }
                    say("ILLEGAL ATTACK");
                } else {
                    top_attacker = top_col;
                    top_row = SNES_ROW_COM_MONSTER;
                    top_col = Msx2_FirstLiveSlot(MSX2_OWNER_COM);
                    if (top_col == MSX2_SLOT_NONE) top_col = 0;
                }
            } else {
                begin_check();
                if (ui == UI_CHECK) {
                    build_objects();
                    goto stamp;
                }
            }
        }
        if ((down & KEY_START) && (ui == UI_HAND || ui == UI_ATTACKER)) {
            /* The turn passes from up here too: the attacker is put down,
             * the camera walks back to the seat, and the swing to the
             * opponent's side follows from there. */
            press_sfx(down);
            top_attacker = MSX2_SLOT_NONE;
            end_player_turn();
            if (board_dirty) render();
            begin_view_transition(0);
        } else if (down & KEY_B) {
            press_sfx(down);
            if (top_attacker != MSX2_SLOT_NONE) {
                /* B puts the attacker down: the cursor goes back to it. */
                top_row = SNES_ROW_YOU_MONSTER;
                top_col = top_attacker;
                top_attacker = MSX2_SLOT_NONE;
            } else {
                /* Card check and rule changes may have replaced the saved bitmap. */
                if (board_dirty) render();
                begin_view_transition(0);
            }
        }
        if (board_dirty) render();
        build_objects();
        goto stamp;
    }

    /* A card on its way down to the board: one lowered pose a game frame
     * through the held-card patch, no input and no rules step until it is
     * flat.  The flat pose itself is not drawn as a held card -- that frame
     * is the placement, and the bake that follows shows the card lying on
     * the slot as a face of the board. */
    if (lower_frame) {
        if (++lower_frame >= LOWER_FRAMES) {
            lower_frame = 0;
            place_chosen(lower_def);
            render();
        } else {
            touch_board(2);
            render();
        }
        build_objects();
        goto stamp;
    }

    if (message_timer) {
        if (--message_timer == 0) {
            message = NULL;
        }
    }

    /* THE BOARD IS BAKED BEFORE A PRESENTATION PLAYS OVER IT.  Both the
     * draw cadence and the opponent's turn are sprites over the board, and
     * each skips the render below; after a turn swing the board is the
     * other seat's still to be baked, and left dirty it stayed the last
     * swing pose -- the wrong seat -- for the whole of the draws. */
    if (!turn_frame && board_yaw == (g_duel.turn_owner ? 128 : 0) &&
        step_draw_presentation()) {
        if (board_dirty) render();
        build_objects();
        goto stamp;
    }

    if (!turn_frame && board_yaw == (g_duel.turn_owner ? 128 : 0) &&
        step_com_presentation()) {
        if (board_dirty) render();
        build_objects();
        goto stamp;
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

    /* Finish the camera move before allowing the next side to act.  The
     * turn cannot pass while the overhead view is up (the duel is paused
     * there), so the yaw is simply noted for the descent. */
    if (ui != UI_RESULT && !turn_frame && board_yaw != (g_duel.turn_owner ? 128 : 0)) {
        if (top_view || view_motion != VIEW_BOARD_REST) {
            board_yaw = g_duel.turn_owner ? 128 : 0;
            touch_board(2);
        } else {
            turn_from = board_yaw;
            turn_target = g_duel.turn_owner ? 128 : 0;
            motion_sequence_begin(TURN_FRAMES);
            turn_frame = 1;
        }
        snesAudioSfx(SNES_SFX_TURN_PASSED);
    }
    if (turn_frame) {
        /* One rendered frame a game frame.  The camera swings out as it
         * turns so the slab's corners stay inside the frame at the diagonal.
         * THE LAST POSE OF THE SWING IS THE OTHER SEAT'S REST PICTURE, baked
         * from the ROM floor by the touch below; rendering it through the
         * yawed mapper first was fifty fields for a picture replaced at
         * once, so the swing stops one pose short. */
        if (turn_frame < TURN_FRAMES) {
            const u16 t = ease_frac(turn_frame, TURN_FRAMES);
            board_yaw = (u8)view_lerp(turn_from, turn_target, t);
            snesCameraSet(&cam, 0,
                          (s16)(CAM_Z - snesQMul(384, snesSin(board_yaw))),
                          CAM_HEIGHT, CAM_FOCAL, SNES_REST_HORIZON_PX);
            cam.yaw = board_yaw;
            motion_frame();
            build_objects();
        }
        if (++turn_frame >= TURN_FRAMES) {
            turn_frame = 0;
            board_yaw = turn_target;
            set_rest_camera((u8)(board_yaw == 128));
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
            press_sfx(down);
            return result_scene();
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
            u8 hand_before[MSX2_HAND];
            u8 hi;
            const u8 step_owner = g_duel.turn_owner;
            com_delay = 6;
            for (hi = 0; hi < MSX2_HAND; ++hi)
                hand_before[hi] = g_duel.side[g_duel.turn_owner].hand[hi];
            Msx2_DuelStep();
            note_draws(step_owner, hand_before);
            if (g_duel.last_action == MSX2_ACTION_ATTACK)
                begin_battle_art(UI_COM);
            else if (g_duel.last_action_owner == MSX2_OWNER_COM &&
                     (g_duel.last_action == MSX2_ACTION_PLACE ||
                      g_duel.last_action == MSX2_ACTION_FUSION ||
                      g_duel.last_action == MSX2_ACTION_EQUIP ||
                      g_duel.last_action == MSX2_ACTION_SUPPORT)) {
                begin_com_presentation(hand_before);
            }
            else {
                Msx2_ClearActionEvent();
                touch_board(12);
            }
        }
    }

    if (force_moving || pattern_mode) {
        /* Y pins the general path at the resting camera, a frame a game
         * frame: what the harness measures as the moving cost.  The rest
         * picture is invalid afterwards (motion_frame says so). */
        set_rest_camera(0);
        motion_frame();
        build_objects();
        goto stamp;
    }

    /* A CARD IN THE AIR IS SOMETHING CHANGING: while the player is choosing
     * where to put a card, the card hovers over the slot and bobs, one quad
     * a game frame through the rest picture's cells. */
    if (ui == UI_PLACE || ui == UI_EQUIP_TARGET) {
        lift_phase += 8;
        touch_board(2);
    }
    if (motion) --motion;

    /* A resting board changes by diff: render() finds the slots that differ
     * from the baked picture and patches those cells.  After a camera move
     * (or anything that took the tiles) it is a full bake. */
    if (board_dirty) render();
    /* THE MOVING CAMERA'S WORLD TEXTURE IS KEPT WARM.  Stamping twenty cards
     * into it is a dozen fields, and paying that on the first frame of a
     * lift was most of the wait before anything moved; done here, in the
     * first idle frame after the board settled, the lift's first pose costs
     * no more than its others. */
    else if (texture_check) {
        texture_check = 0;
        update_board_texture();
    }
    build_objects();

stamp:
    g_stamp.map_lines = t_map;
    g_stamp.conv_lines = t_conv;
    g_stamp.render_lines = t_render;
    g_stamp.turn_max_lines = t_turn_max;
    g_stamp.rest_max_lines = t_rest_max;
    g_stamp.held_max_lines = t_held_max;
    g_stamp.nmi_skips = snesFbNmiSkips();
    g_stamp.occupied = snesFbOccupied();
    g_stamp.dropped = snesFbOverflows();
    g_stamp.view = (view_motion == VIEW_TO_TOP) ? 2 : (view_motion == VIEW_TO_HAND) ? 3
                 : top_view ? 1 : 0;
    g_stamp.battle_phase = (ui == UI_BATTLE_ART) ? snesBattlePhase() : 0xFFFF;
    g_stamp.battle_field = (ui == UI_BATTLE_ART) ? snesBattleField() : 0;
    g_stamp.battle_damage = (ui == UI_BATTLE_ART) ? snesBattleDamage() : 0;
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
