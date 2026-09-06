/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_duel.c — the duel screen.
 *
 *  The board is rasterised into the chunky buffer at one of two internal
 *  resolutions and converted by the C2P; the hand and the HUD are drawn planar
 *  at the full 320x200.  That division is the requirement, not an optimisation
 *  detail: the 3D view is allowed to lose resolution while it moves, the cards
 *  never are.
 *
 *  WHAT MAKES THE CAMERA "MOVING".  It is not whether the camera is animating
 *  right now -- it is whether anything on screen will change next frame.  A
 *  cursor step, a placement, an attack and the settle after one all raise
 *  g_motion; when it reaches zero the same view is re-rendered once at full
 *  320x112 and then left alone.  A still board therefore costs nothing at all
 *  per frame, which is what buys the moving frames their budget.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stddef.h>
#include <stdint.h>

#include "atarist_duel.h"
#include "atarist_video.h"
#include "atarist_draw.h"
#include "atarist_board3d.h"
#include "atarist_assets.h"
#include "atarist_input.h"
#include "atarist_probe.h"
#include "atarist_os.h"
#include "atarist_audio.h"
#include "atarist_disk.h"
#include "atarist_blitter.h"
#include "atarist_battle.h"

#include "msxgl.h"
#include "msx2_duel.h"
#include "msx2_cards.h"

/* ── Camera ──────────────────────────────────────────────────────────────────
 *  Framed against the slab, not guessed.  With the camera six units back and
 *  2.28 up at a focal length of 220, the board's near edge (z = -2, depth 4)
 *  lands on viewport row 95 and its far edge (z = 2, depth 8) on row 33, and
 *  the near edge is 275 of the 320 pixels wide.  Those are the proportions the
 *  MSX2 board has -- a wide near edge, a far edge half its width, the whole
 *  slab in the lower two thirds of its band and black above it.
 *
 *  THE HORIZON IS ABOVE THE SCREEN (row -30), which is what "looking down at a
 *  table" means and is why nothing here draws a sky: the vanishing point is
 *  off the top of the viewport and the plane never reaches it.
 *
 *  All of it is derived from the viewport, so the halved moving view and the
 *  full still view frame the identical picture. */
#define CAM_FOCAL_FULL   220     /* pixels at 320 wide */
#define CAM_HEIGHT_Q16   (149504)           /* 2.28125 world units */
#define CAM_Z_Q16        (-(6 << 16))       /* behind the slab's near edge */
#define CAM_HORIZON_FULL (-30)              /* viewport row, at 112 tall */
/* A card LIES FLAT on its tile, as it does on the MSX2 board.  Standing them
 * up was what made the old board unreadable: four rows one unit apart, seen
 * from a low camera, means the near card covers all but a few rows of the one
 * behind it, and the COM's board was invisible.  Flat, nothing occludes
 * anything, and the tile a card is on stays obvious.  0.70 x 0.90 is a card's
 * own aspect, laid out along x and z. */
#define CARD_W_Q16       (45875)            /* 0.70 world units across */
#define CARD_D_Q16       (58982)            /* 0.90 world units deep */
/* Lifted a hair off the plane so it reads as sitting ON the tile rather than
 * painted into it. */
#define CARD_LIFT_Q16    (3277)             /* 0.05 */
#define WORLD_TO_TEXEL_LOG2  4      /* ATARIST_TEXELS_PER_UNIT == 16 */

/* Whether the board is re-rendered at the full 320x112 once it comes to rest.
 * It costs a visible settle on a plain 8 MHz ST; the STE build and anything
 * with cycles to spare wants it on. */
#ifndef ATARIST_BOARD_STILL_FULLRES
#define ATARIST_BOARD_STILL_FULLRES 1
#endif

/* ── Layout of the card half ─────────────────────────────────────────────── */
/* EVERY X HERE IS A MULTIPLE OF 16 (rectangles) OR 8 (text), and that is a
 * performance decision, not a tidiness one: an aligned rectangle is written
 * with whole 32-bit stores, while a misaligned one pays a read-modify-write
 * per edge group per scanline.  The first version of this layout used a
 * 63-pixel pitch starting at x=5 and the hand alone cost eight vblanks. */
#define HAND_Y        118
#define HAND_CARD_W    64
#define HAND_CARD_H    44
#define HAND_PITCH     64
#define HAND_X0         0
#define HUD_Y         166
#define PROMPT_Y      178
#define STATUS_Y      190

/* ── UI state machine ────────────────────────────────────────────────────── */
enum {
    UI_HAND = 0,        /* choosing a card in hand */
    UI_PLACE,           /* choosing where a monster goes */
    UI_EQUIP_TARGET,    /* choosing the monster an equip attaches to */
    UI_ATTACKER,        /* choosing which of your monsters attacks */
    UI_DEFENDER,        /* choosing what it attacks */
    UI_COM,             /* the opponent is thinking */
    UI_RESULT
};

static uint8_t  g_ui;
static uint8_t  g_cursor;        /* hand slot or field slot, per state */
static uint8_t  g_chosen_hand;
static uint8_t  g_motion;        /* frames of "something is changing" left */
static uint8_t  g_com_delay;
static uint8_t  g_finished;
static uint16_t g_msg_timer;
static const char *g_msg;
/* Set while the attack animation owns the screen, so the frame it hands the
 * duel back on repaints everything: the animation ran without the raster split
 * and with a battle card's palette, and both halves of the duel screen are
 * stale under it. */
static uint8_t g_battle_return;
static AtaristCamera g_cam;

/* THE GROUND IS RENDERED ONCE, NOT ONCE A FRAME.  The duel camera is fixed --
 * the board does not orbit, only the cards on it change -- so the textured
 * plane is the same picture every frame, and it is by a wide margin the most
 * expensive thing the board draws.  Rasterising it into a cache and copying
 * the cache in took a moving frame from ten vblanks to three.  The cache is
 * per resolution, because the halved moving view and the full still view are
 * different pictures; `g_ground_res` says which one is in there. */
#define GROUND_HALF_BYTES (ATARIST_SCREEN_W / 2 * (ATARIST_SPLIT_Y / 2))
#define GROUND_FULL_BYTES (ATARIST_SCREEN_W * ATARIST_SPLIT_Y)
/* BOTH RESOLUTIONS ARE CACHED, not one slot reused.  A single slot is rebuilt
 * every time the board settles and then again the moment it moves, which put
 * a full-resolution ground rasterisation -- twenty-eight vblanks -- on every
 * single action.  Two slots cost 44 KB and are each built once. */
static uint8_t *g_ground_cache[2];
static uint8_t  g_ground_valid[2];

/* HOW MANY BUFFERS STILL OWE A REDRAW.  The screen is double buffered, so
 * "nothing changed, skip the frame" has to skip TWICE before it is safe: the
 * buffer that is not being drawn into is one present behind, and going quiet
 * after a single repaint flips between the new picture and the old one.  Two
 * settling frames, then the duel screen costs nothing at all until the player
 * or the opponent does something -- which is what leaves the whole frame
 * budget to the frames that do move. */
#define ATARIST_BUFFERS 2
static uint8_t g_redraw;
/* The hand and the HUD are counted separately from the board, because they do
 * not animate: a twelve-frame board settle used to repaint the five hand cards
 * twelve times for an identical result, and the hand is the most expensive
 * thing on the screen after the ground. */
static uint8_t g_hud_redraw;

static void duel_touch(int frames)
{
    if (frames > 255) frames = 255;
    if (g_motion < frames) g_motion = (uint8_t)frames;
    g_redraw = ATARIST_BUFFERS;
    g_hud_redraw = ATARIST_BUFFERS;
}

static void duel_say(const char *msg)
{
    g_msg = msg;
    g_msg_timer = 90;
}

/* ── Board rendering ─────────────────────────────────────────────────────── */

/* The chunky surface a board render works on: compact, one byte per pixel,
 * `w` bytes per row -- not the 320-byte chunky stride.  The C2P is told the
 * stride, and a compact surface makes the ground cache one flat copy. */
static void viewport_for(AtaristViewport *vp, int moving)
{
    vp->pixels = Atarist_Chunky();
    vp->half   = moving;
    vp->w      = moving ? ATARIST_SCREEN_W / 2 : ATARIST_SCREEN_W;
    vp->h      = moving ? ATARIST_SPLIT_Y / 2 : ATARIST_SPLIT_Y;
    vp->stride = vp->w;
}

static void camera_for(const AtaristViewport *vp)
{
    int32_t focal = (int32_t)((CAM_FOCAL_FULL * vp->w / ATARIST_SCREEN_W) << 16);
    Atarist_CameraSet(&g_cam, 0, CAM_Z_Q16, CAM_HEIGHT_Q16, 0, focal);
    g_cam.horizon = (int32_t)(CAM_HORIZON_FULL * vp->h / ATARIST_SPLIT_Y);
}

/* A card lying flat on its slot, as a screen-space quad.  All four corners are
 * on the board plane, so this is four projections and no billboard maths at
 * all; the quad is a trapezoid, which is exactly what the affine texture
 * mapper wants.  Vertex 0 is the far-left corner and the texture's top-left,
 * so a painting's top points away from the camera. */
static int card_quad(const AtaristViewport *vp, int row, int col,
                     AtaristVert *q, int tex_w, int tex_h)
{
    static const int8_t sx[4] = { -1, 1, 1, -1 };
    static const int8_t sz[4] = {  1, 1, -1, -1 };
    int32_t wx, wz;
    int i;

    Atarist_SlotCentre(row, col, &wx, &wz);
    for (i = 0; i < 4; ++i) {
        int32_t ox, oy;
        if (!Atarist_Project(&g_cam, vp,
                             wx + sx[i] * (CARD_W_Q16 / 2),
                             wz + sz[i] * (CARD_D_Q16 / 2),
                             CARD_LIFT_Q16, &ox, &oy))
            return 0;
        q[i].x = ox;
        q[i].y = oy;
        q[i].u = (i == 1 || i == 2) ? (int32_t)tex_w << 16 : 0;
        q[i].v = (i >= 2) ? (int32_t)tex_h << 16 : 0;
    }
    return 1;
}

/* The slab's front face.  It is a plain rectangle, not a trapezoid: both of
 * its corner pairs sit at the same depth, and screen x depends only on world x
 * and depth, so the face's sides are vertical.  A lit line along the top of it
 * and the darker face below is the whole of the board's thickness, and it is
 * what stops the board reading as a rug painted on the backdrop. */
static void draw_board_rim(const AtaristViewport *vp)
{
    AtaristVert q[4];
    int32_t lx, rx, ty, by, edge, junk;

    if (!Atarist_Project(&g_cam, vp, -ATARIST_BOARD_HALF_X,
                         ATARIST_BOARD_Z_NEAR, 0, &lx, &ty)) return;
    if (!Atarist_Project(&g_cam, vp, ATARIST_BOARD_HALF_X,
                         ATARIST_BOARD_Z_NEAR, 0, &rx, &junk)) return;
    if (!Atarist_Project(&g_cam, vp, -ATARIST_BOARD_HALF_X,
                         ATARIST_BOARD_Z_NEAR, -ATARIST_BOARD_THICK,
                         &junk, &by)) return;

    edge = ty + (2 << 16);
    if (edge > by) edge = by;

    q[0].x = lx; q[0].y = ty;   q[1].x = rx; q[1].y = ty;
    q[2].x = rx; q[2].y = edge; q[3].x = lx; q[3].y = edge;
    Atarist_FillQuad(vp, q, (uint8_t)(ARENA_RIM_TOP << 2));

    q[0].y = edge; q[1].y = edge;
    q[2].y = by;   q[3].y = by;
    Atarist_FillQuad(vp, q, (uint8_t)(ARENA_RIM_SIDE << 2));
}

/* A flat marker lying on the board, used for the cursor and for the slot the
 * COM is acting on.  Flat filled -- the requirement allows the non-texture
 * surfaces to be, and a marker that costs a texture fetch per pixel would be
 * paid for every frame the cursor moves. */
static void draw_slot_marker(const AtaristViewport *vp, int row, int col,
                             uint8_t colour)
{
    int32_t wx, wz;
    AtaristVert q[4];
    /* A whole tile, less a hair, so the groove around it still shows. */
    int32_t hx = (int32_t)30802;       /* 0.47 */
    int32_t hz = (int32_t)30802;
    int i;
    static const int8_t sx[4] = { -1, 1, 1, -1 };
    static const int8_t sz[4] = { 1, 1, -1, -1 };

    Atarist_SlotCentre(row, col, &wx, &wz);
    for (i = 0; i < 4; ++i) {
        int32_t ox, oy;
        if (!Atarist_Project(&g_cam, vp, wx + sx[i] * hx, wz + sz[i] * hz, 0,
                             &ox, &oy))
            return;
        q[i].x = ox; q[i].y = oy; q[i].u = 0; q[i].v = 0;
    }
    Atarist_FillQuad(vp, q, colour);
}

/* Which board row a field slot of `owner` occupies. */
static int monster_row(int owner)
{
    return owner ? ATARIST_ROW_COM_MONSTER : ATARIST_ROW_YOU_MONSTER;
}
/* The cursor's board position, or -1 when the cursor is not on the board. */
static int cursor_board_slot(int *row)
{
    switch (g_ui) {
    case UI_PLACE:
    case UI_EQUIP_TARGET:
    case UI_ATTACKER:
        *row = monster_row(MSX2_OWNER_PLAYER);
        break;
    case UI_DEFENDER:
        *row = monster_row(MSX2_OWNER_COM);
        break;
    default:
        return -1;
    }
    /* A support play that needs no target parks the cursor on MSX2_SLOT_NONE
     * for one frame; that is not a column. */
    return (g_cursor < ATARIST_COLS) ? g_cursor : -1;
}

static void render_board(int moving)
{
    AtaristViewport vp;
    int row, col, cur_row, cur_slot;
    int slot = moving ? 0 : 1;
    uint8_t *cached = g_ground_cache[slot];

    viewport_for(&vp, moving);
    camera_for(&vp);

    if (cached && !g_ground_valid[slot]) {
        AtaristViewport cache = vp;
        cache.pixels = cached;
        Atarist_DrawBoardPlane(&cache, &g_cam, Atarist_ArenaTexture(),
                               WORLD_TO_TEXEL_LOG2,
                               ATARIST_BOARD_HALF_X, ATARIST_BOARD_Z_NEAR,
                               ATARIST_BOARD_Z_FAR,
                               (uint8_t)(ARENA_BLACK << 2));
        {
            AtaristViewport rim = cache;
            draw_board_rim(&rim);
        }
        g_ground_valid[slot] = 1;
    }

    if (cached) {
        int32_t bytes = (int32_t)vp.w * vp.h;
        if (Atarist_BlitterUsable(vp.pixels, cached, bytes))
            Atarist_BlitterCopy(vp.pixels, cached, bytes);
        else
            Atarist_ChunkyCopy(vp.pixels, cached, bytes);
    }
    else {
        Atarist_DrawBoardPlane(&vp, &g_cam, Atarist_ArenaTexture(),
                               WORLD_TO_TEXEL_LOG2,
                               ATARIST_BOARD_HALF_X, ATARIST_BOARD_Z_NEAR,
                               ATARIST_BOARD_Z_FAR,
                               (uint8_t)(ARENA_BLACK << 2));
        draw_board_rim(&vp);
    }

    cur_slot = cursor_board_slot(&cur_row);
    if (cur_slot >= 0)
        draw_slot_marker(&vp, cur_row, cur_slot, (uint8_t)(ARENA_HILIGHT << 2));

    /* Far to near, so a nearer card overdraws a farther one with no z buffer.
     * The board rows are already stored in that order. */
    for (row = 0; row < ATARIST_ROWS; ++row) {
        int owner = (row <= ATARIST_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                     : MSX2_OWNER_PLAYER;
        int is_support = (row == ATARIST_ROW_COM_SUPPORT ||
                          row == ATARIST_ROW_YOU_SUPPORT);
        const Msx2Side *s = &g_duel.side[owner];
        for (col = 0; col < ATARIST_COLS; ++col) {
            u8 card = is_support ? s->equip_field[col] : s->field[col];
            AtaristVert q[4];
            const AtaristTexture *tex;
            int face;

            if (card == MSX2_CARD_NONE) continue;
            /* A support in play is face up and has a painting of its own;
             * only a set monster shows the back. */
            face = Atarist_CardFaceForCard(card,
                        (is_support || s->faceup[col]) ? 1 : 0);
            tex = Atarist_CardFace(face);
            if (!card_quad(&vp, row, col, q, 1 << tex->w_log2, 1 << tex->h_log2))
                continue;
            Atarist_TexQuad(&vp, q, tex);
        }
    }

    if (moving)
        Atarist_C2P_Double(vp.pixels, Atarist_BackBuffer(),
                           ATARIST_SPLIT_Y / 2, vp.stride);
    else
        Atarist_C2P_Direct(vp.pixels, Atarist_BackBuffer(),
                           ATARIST_SPLIT_Y, vp.stride);
}

/* ── The card half ───────────────────────────────────────────────────────── */

/* The art window: 16-pixel aligned inside the card, which is what keeps the
 * blit whole-word (see STATUS.md -- a misaligned blit is a read-modify-write
 * per group per row).  ART_H + one stat row is exactly the card's height. */
#define HAND_ART_X      16
#define HAND_ART_Y       2
#define HAND_ATK_Y      27
#define HAND_DEF_Y      35

static void draw_hand_card(int i, u8 card, int selected, int used)
{
    int x = HAND_X0 + i * HAND_PITCH;
    uint8_t frame = selected ? CARD_YELLOW : CARD_SILVER;
    uint8_t ink = used ? CARD_PANEL_LIGHT : CARD_WHITE;
    const AtaristImage *art;

    Atarist_FillRect(x, HAND_Y, HAND_CARD_W, HAND_CARD_H,
                     used ? CARD_PANEL_DARK : CARD_PANEL_MID);
    Atarist_FrameRect(x, HAND_Y, HAND_CARD_W, HAND_CARD_H, frame);
    if (selected)
        Atarist_FrameRect(x + 1, HAND_Y + 1, HAND_CARD_W - 2, HAND_CARD_H - 2,
                          CARD_YELLOW);
    if (card == MSX2_CARD_NONE) return;

    /* The card's own painting, converted for the CARD palette.  The board
     * shows the same picture converted for the ARENA one, so a card reads the
     * same in hand as on the field. */
    art = Atarist_CardHandImage(Atarist_CardFaceForCard(card, 1));
    if (art)
        Atarist_BlitImage(art, x + HAND_ART_X, HAND_Y + HAND_ART_Y);
    else
        Atarist_FillRect(x + HAND_ART_X, HAND_Y + HAND_ART_Y,
                         ATARIST_ART_HAND_W, ATARIST_ART_HAND_H,
                         Msx2_IsSupport(card) ? CARD_GOLD : CARD_PANEL_LIGHT);

    /* Two stat rows under the art, right aligned.  Side by side they fit --
     * four digits at eight pixels is exactly half the card each -- but a
     * 2300 and a 2100 printed adjacent read as one eight-digit number, which
     * is what the first build of this layout actually looked like. */
    if (Msx2_IsSupport(card)) {
        Atarist_DrawText(x + 8, HAND_Y + HAND_ATK_Y, "SUP", CARD_GOLD,
                         CARD_BLACK);
        Atarist_DrawNumber(x + 56, HAND_Y + HAND_DEF_Y, Msx2_SupportKind(card),
                           CARD_YELLOW, CARD_BLACK);
    } else {
        Atarist_DrawNumber(x + 56, HAND_Y + HAND_ATK_Y, Msx2_CardAtk(card),
                           ink, CARD_BLACK);
        Atarist_DrawNumber(x + 56, HAND_Y + HAND_DEF_Y, Msx2_CardDef(card),
                           used ? CARD_PANEL_LIGHT : CARD_GREEN, CARD_BLACK);
    }
}

static const char *prompt_text(void)
{
    switch (g_ui) {
    case UI_HAND:         return "A:PLAY  SPACE:BATTLE  TAB:END";
    case UI_PLACE:        return "A:ATTACK POS   B:DEFENCE POS";
    case UI_EQUIP_TARGET: return "PICK A MONSTER TO EQUIP";
    case UI_ATTACKER:     return "PICK AN ATTACKER  TAB:END TURN";
    case UI_DEFENDER:     return "PICK A TARGET  B:DIRECT";
    case UI_COM:          return "OPPONENT THINKING";
    default:              return "";
    }
}

static void draw_hud(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    const Msx2Side *com = &g_duel.side[MSX2_OWNER_COM];
    int i;

    Atarist_ClearPlanarBand(ATARIST_SPLIT_Y, ATARIST_SCREEN_H, CARD_PANEL_DARK);
    Atarist_HLine(0, ATARIST_SPLIT_Y, ATARIST_SCREEN_W, CARD_GOLD);

    for (i = 0; i < MSX2_HAND; ++i)
        draw_hand_card(i, you->hand[i],
                       (g_ui == UI_HAND && g_cursor == i) ||
                       (g_ui != UI_HAND && g_chosen_hand == i &&
                        g_ui != UI_ATTACKER && g_ui != UI_DEFENDER &&
                        g_ui != UI_COM),
                       you->used[i]);

    /* Shadowless: the HUD sits on a flat panel, and a shadow would double the
     * glyph cost for nothing.  Text over card art keeps its shadow. */
    Atarist_DrawText(0, HUD_Y, "YOU", CARD_WHITE, CARD_WHITE);
    Atarist_DrawNumber(80, HUD_Y, you->lp, CARD_GREEN, CARD_GREEN);
    Atarist_DrawText(160, HUD_Y, "COM", CARD_WHITE, CARD_WHITE);
    Atarist_DrawNumber(240, HUD_Y, com->lp, CARD_RED, CARD_RED);
    Atarist_DrawText(256, HUD_Y, "T", CARD_SILVER, CARD_SILVER);
    Atarist_DrawNumber(320, HUD_Y, g_duel.turns, CARD_SILVER, CARD_SILVER);

    if (g_ui == UI_RESULT) {
        Atarist_DrawTextCentred(160, PROMPT_Y,
                                g_duel.result > 0 ? "YOU WIN" : "YOU LOSE",
                                CARD_YELLOW, CARD_YELLOW);
        Atarist_DrawTextCentred(160, STATUS_Y, "PRESS SPACE",
                                CARD_WHITE, CARD_WHITE);
        return;
    }

    Atarist_DrawTextCentred(160, PROMPT_Y, prompt_text(), CARD_WHITE, CARD_WHITE);
    if (g_msg_timer && g_msg)
        Atarist_DrawTextCentred(160, STATUS_Y, g_msg, CARD_YELLOW, CARD_YELLOW);
    else
        Atarist_DrawTextCentred(160, STATUS_Y,
                                g_duel.turn_owner == MSX2_OWNER_PLAYER
                                    ? "YOUR TURN" : "OPPONENT TURN",
                                CARD_SILVER, CARD_SILVER);
}

/* ── Input ───────────────────────────────────────────────────────────────── */

static void move_cursor(int count)
{
    if (Atarist_InputRepeat(ATARIST_BTN_LEFT, 12)) {
        g_cursor = (uint8_t)((g_cursor + count - 1) % count);
        duel_touch(4);
    }
    if (Atarist_InputRepeat(ATARIST_BTN_RIGHT, 12)) {
        g_cursor = (uint8_t)((g_cursor + 1) % count);
        duel_touch(4);
    }
}

/* Hand an attack that has already been resolved to the animation.  Both call
 * sites -- the player declaring one and the COM's turn producing one -- have to
 * do this BEFORE Msx2_ClearActionEvent, which is what says an attack happened
 * at all. */
static void show_attack(void)
{
    if (Atarist_BattleBegin()) g_battle_return = 1;
}

static void begin_battle_phase(void)
{
    g_duel.phase = MSX2_PHASE_BATTLE;
    g_ui = UI_ATTACKER;
    g_cursor = 0;
    duel_touch(8);
}

static void end_player_turn(void)
{
    Msx2_EndTurn();
    g_ui = UI_COM;
    g_com_delay = 12;
    duel_touch(8);
}

static void place_chosen(int defense)
{
    u8 card = g_duel.side[MSX2_OWNER_PLAYER].hand[g_chosen_hand];
    if (Msx2_IsSupport(card)) {
        if (!Msx2_PlaySupport(MSX2_OWNER_PLAYER, g_chosen_hand, g_cursor))
            duel_say("CANNOT PLAY THAT");
    } else if (!Msx2_PlaceMonster(MSX2_OWNER_PLAYER, g_chosen_hand, g_cursor,
                                  defense ? TRUE : FALSE)) {
        duel_say("CANNOT PLACE THERE");
    }
    Msx2_ClearActionEvent();
    g_ui = UI_HAND;
    g_cursor = g_chosen_hand;
    duel_touch(20);
}

static void step_player(void)
{
    const Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];

    switch (g_ui) {
    case UI_HAND:
        move_cursor(MSX2_HAND);
        if (g_atarist_input.pressed & ATARIST_BTN_A) {
            u8 card = you->hand[g_cursor];
            if (card == MSX2_CARD_NONE || you->used[g_cursor]) {
                duel_say("NOTHING THERE");
            } else if (Msx2_IsSupport(card)) {
                u8 kind = Msx2_SupportKind(card);
                g_chosen_hand = g_cursor;
                if (kind == MSX2_SUP_EQUIP || kind == MSX2_SUP_GUARD) {
                    g_ui = UI_EQUIP_TARGET;
                    g_cursor = 0;
                } else {
                    g_cursor = MSX2_SLOT_NONE;
                    place_chosen(0);
                }
                duel_touch(8);
            } else {
                g_chosen_hand = g_cursor;
                g_ui = UI_PLACE;
                g_cursor = Msx2_FirstFreeSlot(MSX2_OWNER_PLAYER);
                if (g_cursor == MSX2_SLOT_NONE) g_cursor = 0;
                duel_touch(8);
            }
        }
        if (g_atarist_input.pressed & ATARIST_BTN_START) begin_battle_phase();
        if (g_atarist_input.pressed & ATARIST_BTN_TAB) end_player_turn();
        break;

    case UI_PLACE:
        move_cursor(MSX2_FIELD);
        if (g_atarist_input.pressed & ATARIST_BTN_A) place_chosen(0);
        else if (g_atarist_input.pressed & ATARIST_BTN_B) place_chosen(1);
        else if (g_atarist_input.pressed & ATARIST_BTN_START) {
            g_ui = UI_HAND;
            g_cursor = g_chosen_hand;
            duel_touch(8);
        }
        break;

    case UI_EQUIP_TARGET:
        move_cursor(MSX2_FIELD);
        if (g_atarist_input.pressed & ATARIST_BTN_A) place_chosen(0);
        else if (g_atarist_input.pressed & ATARIST_BTN_START) {
            g_ui = UI_HAND;
            g_cursor = g_chosen_hand;
            duel_touch(8);
        }
        break;

    case UI_ATTACKER:
        move_cursor(MSX2_FIELD);
        if (g_atarist_input.pressed & ATARIST_BTN_A) {
            if (!Msx2_IsMonster(you->field[g_cursor])) {
                duel_say("NO MONSTER THERE");
            } else if (you->attacked[g_cursor]) {
                duel_say("ALREADY ATTACKED");
            } else if (Msx2_FirstTurnAttackLocked()) {
                duel_say("NO ATTACK ON TURN ONE");
            } else {
                g_chosen_hand = g_cursor;       /* reused: the attacker slot */
                g_ui = UI_DEFENDER;
                g_cursor = Msx2_FirstLiveSlot(MSX2_OWNER_COM);
                if (g_cursor == MSX2_SLOT_NONE) g_cursor = 0;
                duel_touch(8);
            }
        }
        if (g_atarist_input.pressed & ATARIST_BTN_B) {
            /* Position switch: the one main-phase action still legal in
             * battle, and the rules model exposes it as its own call. */
            if (Msx2_ChangePosition(MSX2_OWNER_PLAYER, g_cursor)) duel_touch(12);
            else duel_say("CANNOT TURN THAT");
        }
        if (g_atarist_input.pressed & ATARIST_BTN_TAB) end_player_turn();
        break;

    case UI_DEFENDER: {
        int direct = (Msx2_LiveMonsterCount(MSX2_OWNER_COM) == 0) ||
                     (g_atarist_input.pressed & ATARIST_BTN_B) != 0;
        move_cursor(MSX2_FIELD);
        if ((g_atarist_input.pressed & ATARIST_BTN_A) || direct) {
            u8 target = direct ? MSX2_SLOT_NONE : g_cursor;
            if (!Msx2_Attack(MSX2_OWNER_PLAYER, g_chosen_hand, target))
                duel_say("THAT ATTACK IS ILLEGAL");
            else
                show_attack();
            Msx2_ClearActionEvent();
            g_ui = UI_ATTACKER;
            g_cursor = g_chosen_hand;
            duel_touch(24);
        }
        if (g_atarist_input.pressed & ATARIST_BTN_START) {
            g_ui = UI_ATTACKER;
            g_cursor = g_chosen_hand;
            duel_touch(8);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Entry points ────────────────────────────────────────────────────────── */

void Atarist_DuelEnter(uint32_t seed, uint8_t story_index)
{
    if (!g_ground_cache[0]) {
        /* Losing the allocation is survivable -- the board falls back to
         * rasterising the ground every frame -- so it is not a boot failure. */
        uint8_t *block = (uint8_t *)st_malloc(GROUND_HALF_BYTES +
                                              GROUND_FULL_BYTES);
        if (block) {
            g_ground_cache[0] = block;
            g_ground_cache[1] = block + GROUND_HALF_BYTES;
        }
    }
    Msx2_DuelInit(seed, story_index);
    /* Loading the track reads the floppy, which is why it happens here and not
     * per frame: GEMDOS is still resident and a Fread costs whole frames. */
    Atarist_MusicLoadTrack(story_index == MSX2_STORY_FINAL_DUEL
                               ? ATARIST_MUSIC_FINAL_BOSS
                               : ATARIST_MUSIC_BATTLE);
    Atarist_ApplyArenaPalette();
    Atarist_ApplyCardPalette();
    Atarist_SetSplitEnabled(1);
    Atarist_InputFlush();

    g_ui = UI_COM;              /* TURN_START runs first, for either side */
    g_cursor = 0;
    g_chosen_hand = 0;
    g_com_delay = 4;
    g_finished = 0;
    g_battle_return = 0;
    g_msg = NULL;
    g_msg_timer = 0;
    g_redraw = ATARIST_BUFFERS;
    g_hud_redraw = ATARIST_BUFFERS;
    g_motion = 30;
    Atarist_ClearPlanar(0);
}

void Atarist_DuelStep(int vblanks)
{
    /* THE ANIMATION OWNS THE WHOLE FRAME WHILE IT RUNS.  It has taken the
     * split down and installed a battle card's own sixteen colours, so nothing
     * else may draw, and the rules must not advance underneath a picture of an
     * attack that has already been applied. */
    if (Atarist_BattleActive()) {
        Atarist_BattleStep(vblanks);
        return;
    }
    if (g_battle_return) {
        g_battle_return = 0;
        Atarist_ApplyArenaPalette();
        Atarist_ApplyCardPalette();
        Atarist_SetSplitEnabled(1);
        Atarist_InputFlush();
        /* Both buffers still hold the animation's last frame. */
        Atarist_ClearPlanar(0);
        g_redraw = ATARIST_BUFFERS;
        g_hud_redraw = ATARIST_BUFFERS;
        g_ground_valid[0] = g_ground_valid[1] = 0;
        duel_touch(2);
    }

    if (g_msg_timer) {
        g_msg_timer = (uint16_t)(g_msg_timer > vblanks ? g_msg_timer - vblanks : 0);
        /* The status line changes when the message expires, so the frame it
         * expires on is a frame that has to be repainted. */
        if (!g_msg_timer) duel_touch(1);
    }

    if (g_duel.result != 0 && g_ui != UI_RESULT) {
        g_ui = UI_RESULT;
        duel_touch(30);
    }

    if (g_ui == UI_RESULT) {
        if (g_atarist_input.pressed & (ATARIST_BTN_START | ATARIST_BTN_A))
            g_finished = 1;
    } else if (g_duel.turn_owner == MSX2_OWNER_PLAYER &&
               (g_duel.phase == MSX2_PHASE_MAIN ||
                g_duel.phase == MSX2_PHASE_BATTLE)) {
        /* The player's own phases are the only ones the rules model does not
         * drive itself; everything else -- including the player's draw at
         * TURN_START -- goes through Msx2_DuelStep. */
        if (g_ui == UI_COM) {
            g_ui = (g_duel.phase == MSX2_PHASE_MAIN) ? UI_HAND : UI_ATTACKER;
            g_cursor = 0;
            duel_touch(12);
        }
        step_player();
    } else {
        g_ui = UI_COM;
        if (g_com_delay > vblanks) {
            g_com_delay = (uint8_t)(g_com_delay - vblanks);
        } else {
            g_com_delay = 10;
            Msx2_DuelStep();
            show_attack();
            Msx2_ClearActionEvent();
            duel_touch(12);
        }
    }

    if (g_motion) {
        g_motion = (uint8_t)(g_motion > vblanks ? g_motion - vblanks : 0);
        g_redraw = ATARIST_BUFFERS;
    }

    /* A moving frame renders the board at half resolution; the settling frames
     * after it render it once per buffer at the full 320x112 and then the
     * screen goes quiet. */
    if (g_redraw) {
        render_board(ATARIST_BOARD_STILL_FULLRES ? (g_motion != 0) : 1);
        if (!g_motion) --g_redraw;
    }
    if (g_hud_redraw) {
        draw_hud();
        --g_hud_redraw;
    }

    g_atarist_probe.phase = g_duel.phase;
    g_atarist_probe.turns = g_duel.turns;
    g_atarist_probe.lp_player = g_duel.side[MSX2_OWNER_PLAYER].lp;
    g_atarist_probe.lp_com = g_duel.side[MSX2_OWNER_COM].lp;
    g_atarist_probe.menu_cursor = g_cursor;
}

int Atarist_DuelFinished(void) { return g_finished; }
int Atarist_DuelResult(void)   { return g_duel.result; }
