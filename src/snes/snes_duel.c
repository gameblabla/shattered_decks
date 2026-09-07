/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_duel.c — duel presentation.
 *
 *  M3 stage: the board is a textured floor with REAL CARDS ON IT.  The ground
 *  and the backdrop come from tools/snes/gen_snes_textures.py and the twenty
 *  card faces from tools/snes/gen_snes_cards.py -- the same paintings in the
 *  same frames every other port shows.  There is no placeholder art anywhere
 *  in it.
 *
 *  A RESTING CARD IS THE FLOOR WITH A DIFFERENT TEXTURE and a card in the air
 *  is a convex quad; both live in snes_board3d.c.  The board state comes from
 *  the real rules model (src/msx2/msx2_duel.c) rather than from a table of
 *  card ids -- what M3 does not have yet is the rules DRIVING it, which is M4;
 *  the hand and the HUD text arrive with it.
 *
 *  The two resolutions are what the whole video model exists for: the camera
 *  moves at 64x40 texels (4x4 screen pixels) and rests at 128x80 (2x2), and
 *  the PPU does the doubling, so a moving board costs a quarter of the pixels
 *  and nothing extra to display.  Y switches between them, and the resting
 *  camera is what makes the still frame's three-vblank upload invisible.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_duel.h"
#include "snes_video.h"
#include "snes_board3d.h"
#include "snes_textures.h"
#include "snes_cards.h"
#include "snes_stamp.h"
#include "msx2_duel.h"

/* The arena camera.  One unit is one slot pitch; the board runs z = -2..2, so
 * a camera one unit up and three back looks along the slab with its far edge
 * near the horizon. */
#define CAM_HEIGHT   ((s16)256)         /* 1.0 */
#define CAM_Z        ((s16)(-3 * 256))
#define CAM_SWAY     ((s16)192)         /* 0.75 units either side */

/* The surround: the texel the board floats on where the slab does not reach.
 * Direct colour is BBGGGRRR, so this is a dark warm brown -- the arena's
 * shadowed ground, not a debug colour. */
#define BACKDROP     SNES_DC(1, 1, 0)

static SnesCamera   cam;
static SnesViewport vp;
static u8  phase;               /* the sway's angle, 0..255 */
static u8  lift_phase;          /* the held card's bob, 0..255 */
/* SELECT hides the cards, and it is not a debug convenience: the board is the
 * only thing the port is allowed to make a performance claim about, and every
 * claim has to come from a measurement or an ABLATION.  This is the ablation --
 * the same frame with the twenty cards and the held one compiled in but not
 * drawn -- and tools/snes/verify.py drives it to attribute the cost and to
 * measure the slab's own shape without cards lying on it. */
static u8  show_cards = 1;

/* The slot the held card hovers over.  In M4 this is the cursor; here it is
 * what exercises the quad path every frame, which is the only way the edge
 * chains get walked at all before the rules can lift a card. */
#define HELD_ROW   SNES_ROW_YOU_MONSTER
#define HELD_COL   2
#define HELD_LIFT  ((s16)96)            /* 0.375 world units above the board */
#define HELD_TILT  ((s16)160)           /* the far edge, 0.625 units higher */

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

/* The HUD band under the board: framebuffer rows 80..111 in both resolutions.
 *
 * It is the same arena sandstone, sampled 1:1, because the alternative at this
 * milestone is a flat rectangle and a flat rectangle is a placeholder.  The
 * life points, phase text and hand sprites land on top of it in M4; nothing
 * about its geometry changes when they do. */
static void draw_hud_panel(void)
{
    u16 y;

    for (y = 0; y < SNES_HUD_H; ++y) {
        /* A different part of the texture from the board's, so the panel does
         * not read as more floor: the groove rows, one tile down. */
        const u16 v = (y + SNES_FLOOR_TEXELS_PER_UNIT) & (SNES_FLOOR_PATTERN_H - 1);
        snesSpanFloor((u16)((SNES_HUD_ROW + y) * SNES_FB_STRIDE), SNES_HUD_W,
                      (u16)((v << 8) | 0), 0, 256);
    }
}

/* The board state M3 shows.
 *
 * It is built by the REAL rules model out of REAL decks -- five monsters a
 * side dealt off the shuffled deck the duel starts with, and the support rows
 * holding the actual support cards -- so every id the renderer sees is one the
 * duel could produce.  What it is not is a duel being played: the rules do not
 * drive the presentation until M4, and this is the fixture that lets the
 * renderer be verified before they do.
 */
static void fixture_board(void)
{
    Msx2Side *you = &g_duel.side[MSX2_OWNER_PLAYER];
    Msx2Side *com = &g_duel.side[MSX2_OWNER_COM];
    u8 col;

    Msx2_DuelInit(0x51E5u, MSX2_STORY_NONE);
    for (col = 0; col < MSX2_FIELD; ++col) {
        int a = waifu_deck_draw(&you->deck);
        int b = waifu_deck_draw(&com->deck);
        you->field[col] = (a >= 0) ? (u8)a : MSX2_CARD_NONE;
        com->field[col] = (b >= 0) ? (u8)b : MSX2_CARD_NONE;
        /* One monster is SET, and it proves the back face is a face like any
         * other: same page, same walker, no special case anywhere.  It is in
         * the player's own NEAR row because that is where a card is large
         * enough on screen for the harness to identify which face it is. */
        you->faceup[col] = (col == 3) ? 0 : 1;
        com->faceup[col] = 1;
        you->equip_field[col] = (u8)(WAIFU_SUPPORT_EQUIP_CARD_ID + col + 1);
        com->equip_field[col] = (u8)(WAIFU_SUPPORT_EQUIP_CARD_ID + col);
    }
}

/* Which face a slot shows: its own picture face up, the back face down.  The
 * back is the id past the last card, so a set monster costs no special case in
 * the renderer at all. */
static u8 face_of(u8 card, u8 faceup)
{
    if (card == MSX2_CARD_NONE) return SNES_CARD_BACK;
    if (!faceup || card >= SNES_CARD_BACK) return SNES_CARD_BACK;
    return card;
}

/* Every card resting on the board, FAR ROW FIRST.  There is no z buffer, so
 * painter's order is the whole of the hidden-surface algorithm, and the row
 * order the rules use is already far to near. */
static void draw_board_cards(void)
{
    u8 row, col;

    for (row = 0; row < SNES_ROWS; ++row) {
        const u8 owner = (row <= SNES_ROW_COM_MONSTER) ? MSX2_OWNER_COM
                                                       : MSX2_OWNER_PLAYER;
        const u8 support = (row == SNES_ROW_COM_SUPPORT ||
                            row == SNES_ROW_YOU_SUPPORT);
        const Msx2Side *s = &g_duel.side[owner];
        u8 faces[SNES_COLS];

        for (col = 0; col < SNES_COLS; ++col) {
            const u8 card = support ? s->equip_field[col] : s->field[col];
            /* The held card is in the air, not in its slot. */
            if (card == MSX2_CARD_NONE ||
                (!support && row == HELD_ROW && col == HELD_COL))
                faces[col] = SNES_CARD_NONE_FACE;
            else
                faces[col] = face_of(card, support ? 1 : s->faceup[col]);
        }
        snesDrawCardRow(&vp, &cam, row, faces);
    }
}

/* The card the player is holding over its slot, through the general convex
 * quad path: four projected corners, two edge chains, an affine walk. */
static void draw_held_card(void)
{
    const Msx2Side *s = &g_duel.side[MSX2_OWNER_PLAYER];
    const u8 card = s->field[HELD_COL];
    SnesVert q[4];
    s16 lift;

    if (card == MSX2_CARD_NONE) return;
    /* A bob rather than a fixed height, so the quad is a DIFFERENT quad every
     * frame and a chain that only works at one height cannot pass. */
    lift = (s16)(HELD_LIFT + (snesSin(lift_phase) >> 2));
    if (!snesCardQuad(&cam, &vp, HELD_ROW, HELD_COL, lift, HELD_TILT, q)) return;
    snesTexQuad(&vp, q, face_of(card, s->faceup[HELD_COL]));
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

    snesDrawFloor(&vp, &cam, BACKDROP);
    if (show_cards) {
        draw_board_cards();
        draw_held_card();
    }

    g_stamp.render_lines = (u16)((snes_vblank_count - vbl0) * 262
                                 + snesVCounter() - line0);
    snesVideoPresentRestart();
}

void snesDuelEnter(void)
{
    phase = 0;
    lift_phase = 0;
    fixture_board();
    snesCameraSet(&cam, 0, CAM_Z, CAM_HEIGHT, 0, 0);
    snesVideoSetBoardRes(SNES_RES_STILL);
    set_viewport();
    snesVideoClear(BACKDROP);
    draw_hud_panel();
    snesVideoHudDirty();
    render();
}

void snesDuelFrame(void)
{
    const u16 pad = padsCurrent(0);
    u8 res = snesVideoBoardRes();
    u8 redraw = 0;

    if (padsDown(0) & KEY_SELECT) {
        show_cards ^= 1;
        redraw = 1;
    }
    if (padsDown(0) & KEY_Y) {
        res = (res == SNES_RES_STILL) ? SNES_RES_MOVING : SNES_RES_STILL;
        snesVideoSetBoardRes(res);
        set_viewport();
        /* The panel does not move with the band, but clearing the buffer
         * wipes it, so it is redrawn and re-uploaded with the change. */
        snesVideoClear(BACKDROP);
        draw_hud_panel();
        snesVideoHudDirty();
        redraw = 1;
    }

    /* The camera sways only in the moving resolution -- that is what the two
     * resolutions MEAN.  A resting camera leaves the still frame on screen and
     * its three-vblank upload finishes undisturbed. */
    if (res == SNES_RES_MOVING) {
        phase += 2;
        cam.x = snesQMul(snesSin(phase), CAM_SWAY);
        redraw = 1;
    }
    /* The held card bobs in both resolutions, so the quad path is exercised by
     * the still frame too -- that is the one a screenshot catches.  In the
     * still resolution it may only bob once the previous frame has finished
     * its three-vblank upload: a redraw restarts that upload at the top, so
     * animating regardless would leave the bottom two thirds of the board
     * permanently unuploaded.  Which is also why the camera never sways here. */
    if (res == SNES_RES_MOVING || snesVideoPresentDone()) {
        lift_phase += 4;
        redraw = 1;
    }
    if (pad & KEY_LEFT)  { cam.x -= 8; redraw = 1; }
    if (pad & KEY_RIGHT) { cam.x += 8; redraw = 1; }
    if (pad & KEY_UP)    { cam.z += 8; redraw = 1; }
    if (pad & KEY_DOWN)  { cam.z -= 8; redraw = 1; }

    if (redraw) render();
}
