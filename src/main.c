#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#include "defines.h"
#include "common.h"
#include "renderer3d.h"
#include "bmp_writer.h"
#include "font_menudata.h"
#include "waifu_assets.h"
#include "title_asset.h"
#include "zmbv_mkv.h"
#include "game_api.h"

#define W 256
#define H 240
#define BOARD_COLS 5
#define BOARD_ROWS 4
#define Q8_SHIFT 8
#define Q8_ONE   (1 << Q8_SHIFT)
#define Q8_HALF  (1 << (Q8_SHIFT - 1))
#define Q8_FROM_INT(v) ((int32_t)(v) * Q8_ONE)
#define Q8_FRAC(n, d) ((int32_t)(((n) * Q8_ONE + ((d) / 2)) / (d)))
#define FIELD_X0 (-781)   /* -3.05 in Q8.8 */
#define FIELD_X1 ( 781)   /*  3.05 in Q8.8 */
#define FIELD_Z0 (-653)   /* -2.55 in Q8.8 */
#define FIELD_Z1 ( 653)   /*  2.55 in Q8.8 */
#define FIELD_Y  (0)
#define FIELD_THICK (-108) /* -0.42 in Q8.8 */
#define CFX_PI_Q8 804
#define TITLE_SEQUENCE_FRAMES 310
#define DUEL_TOTAL_FRAMES 2696
#define DUEL_OPENING_END 150
#define DUEL_PREVIEW_END 270
#define DUEL_SCRIPT_OFFSET (DUEL_PREVIEW_END - 84)

static uint8_t framebuffer[W * H];
static CfxRenderer3D renderer;
static int g_you_lp = 8000;
static int g_com_lp = 8000;
static int g_player_hand_offset_y = 0;
static int g_enemy_hand_offset_y = 0;
static int g_suppress_hand_cursor = 0;
static int g_player_hide_index = -1;
static int g_enemy_hide_index = -1;
static int g_battle_late_frame = -1;
static int g_force_deckout_demo = 0;
static int g_force_lp_loss_demo = 0;
static int g_story_name_to_intro = 0;

/* Demo card choices. The IDs are generated from the uploaded waifu-card set. */
static const int hand_ids[5] = {40, 12, 9, 41, 37}; /* White dragon, Golem, Voltara, Witch, Gold Dragon */
static const int com_hand_ids[5] = {41, 15, 22, 29, 36}; /* Witch, Jester, Priestess, Slime, Tyranno */
static const int player_summon_id = 40;
static const int enemy_summon_id = 15;
/* Field coordinates: rows 0/1 are opponent side, rows 2/3 are player side. */
static const int PLAYER_CARD_COL = 0;
static const int PLAYER_CARD_ROW = 2;
static const int ENEMY_CARD_COL = 0;
static const int ENEMY_CARD_ROW = 1;
static const int PLAYER_CARD2_COL = 1;
static const int ENEMY_CARD2_COL = 1;
static const int PLAYER_CARD3_COL = 2;

static int g_b_top_col = 0;
static int g_b_top_row = PLAYER_CARD_ROW;
static int g_b_top_prev_col = 0;
static int g_b_top_prev_row = PLAYER_CARD_ROW;
static int g_b_top_cursor_anim = 8;
static int g_b_attack_attacker_slot = -1;

#define BATTLE_BURN_DUR 52
#define BATTLE_BURN_VANISH_FRAMES 44
#define SUPPORT_EQUIP_CARD_ID   (WAIFU_CARD_COUNT + 0)
#define SUPPORT_GUARD_CARD_ID   (WAIFU_CARD_COUNT + 1)
#define SUPPORT_DRAW_CARD_ID    (WAIFU_CARD_COUNT + 2)
#define SUPPORT_HEAL_CARD_ID    (WAIFU_CARD_COUNT + 3)
#define SUPPORT_CARD_VARIANTS   4

static int support_card_kind(int card_id)
{
    if (card_id < WAIFU_CARD_COUNT) return 0;
    return (card_id - WAIFU_CARD_COUNT) % SUPPORT_CARD_VARIANTS;
}

static const char *support_card_name(int card_id)
{
    switch (support_card_kind(card_id)) {
    case 0: return "BRONZE EQUIP";
    case 1: return "DESERT GUARD";
    case 2: return "ANCIENT DRAW";
    default: return "OASIS LIGHT";
    }
}

static const char *support_card_type(int card_id)
{
    switch (support_card_kind(card_id)) {
    case 0: return "Equip / Support";
    case 1: return "Guard / Support";
    case 2: return "Draw / Support";
    default: return "Heal / Support";
    }
}

static const char *support_card_effect(int card_id)
{
    switch (support_card_kind(card_id)) {
    case 0: return "Equip card. Use from hand; does not count as your one monster placement.";
    case 1: return "Support card. A defensive charm for the starter deck.";
    case 2: return "Support card. A small draw charm for Serena's first dream duel.";
    default: return "Support card. A small life charm for the starter deck.";
    }
}

static int is_support_card(int card_id)
{
    /* Support and equip cards live above the generated monster id range. */
    return card_id >= WAIFU_CARD_COUNT;
}

static int is_monster_card(int card_id)
{
    return card_id >= 0 && card_id < WAIFU_CARD_COUNT;
}

static int field_card_atk(int owner, int slot);
static int field_card_def(int owner, int slot);

/* ------------------------------------------------------------------------- */
/* Small vector/camera utilities. The actual textured quad rasterization is   */
/* Cascade FX's renderer3d; these functions only provide a PC/headless camera. */

typedef struct { int32_t x, y, z; } Vec3;
typedef struct { Vec3 eye, target, up; int32_t focal; } Camera;
typedef struct { int x, y; int32_t depth; int ok; } ScreenPt;
typedef struct { int x, y, u, v; } TexV;

static void draw_late_field_cards(Camera cam, int f);
static void draw_textured_tri(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c);

static int32_t q8_mul(int32_t a, int32_t b) { return (int32_t)((a * b) >> Q8_SHIFT); }
static int32_t q8_div(int32_t a, int32_t b) { return b ? (int32_t)((a * Q8_ONE) / b) : 0; }
static int32_t q8_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int q16_floor_to_int(int32_t v) { return v >= 0 ? (int)(v / 65536) : -(int)(((-v) + 65535) / 65536); }
static int32_t q16_div_q8(int32_t n, int32_t d)
{
    int sign = 1;
    int32_t whole, rem;
    if (!d) return 0;
    if (n < 0) { n = -n; sign = -sign; }
    if (d < 0) { d = -d; sign = -sign; }
    whole = n / d;
    rem = n - whole * d;
    return sign * (whole * 65536 + (rem * 65536) / d);
}
static int32_t q8_smoothstep(int32_t t)
{
    t = q8_clamp(t, 0, Q8_ONE);
    return q8_mul(q8_mul(t, t), Q8_FROM_INT(3) - q8_mul(Q8_FROM_INT(2), t));
}
static int32_t q8_ratio(int n, int d)
{
    if (d <= 0) return 0;
    return q8_clamp((int32_t)((n * Q8_ONE + d / 2) / d), 0, Q8_ONE);
}
static int32_t q8_smooth_ratio(int n, int d) { return q8_smoothstep(q8_ratio(n, d)); }
static int32_t q8_ps1step(int32_t t)
{
    t = q8_smoothstep(t);
    return (int32_t)(((t * 60 + Q8_HALF) / Q8_ONE) * Q8_ONE / 60);
}
static int q8_to_int(int32_t v) { return v >= 0 ? (int)(v >> Q8_SHIFT) : -(int)((-v) >> Q8_SHIFT); }
static int lerp_i(int a, int b, int32_t t) { return q8_to_int(Q8_FROM_INT(a) + q8_mul(Q8_FROM_INT(b - a), t)); }
static int32_t q8_lerp(int32_t a, int32_t b, int32_t t) { return a + q8_mul(b - a, t); }

static const int16_t q8_sin_quarter[65] = {
    0,6,13,19,25,31,38,44,50,56,62,68,74,80,86,92,98,
    104,109,115,121,126,132,137,142,147,152,157,162,167,
    172,177,181,185,190,194,198,202,206,209,213,216,220,
    223,226,229,231,234,237,239,241,243,245,247,248,250,
    251,252,253,254,255,255,256,256,256
};

static int32_t q8_sin_turn_sample(int idx)
{
    idx &= 255;
    int quad = idx >> 6;
    int off = idx & 63;
    int32_t v = (quad & 1) ? q8_sin_quarter[64 - off] : q8_sin_quarter[off];
    return (quad >= 2) ? -v : v;
}

static int32_t q8_sin_turn(int32_t a)
{
    int idx = (int)((a & 0xffff) >> 8);
    int frac = (int)(a & 255);
    int32_t v0 = q8_sin_turn_sample(idx);
    int32_t v1 = q8_sin_turn_sample(idx + 1);
    return v0 + (((v1 - v0) * frac + 128) / 256);
}
static int32_t q8_cos_turn(int32_t a) { return q8_sin_turn(a + 0x4000); }
static int32_t q8_sin_pi(int32_t t) { return q8_sin_turn(t << 7); }
static int32_t q8_sin_rad(int32_t radians) { return q8_sin_turn((int32_t)((radians * 32768) / CFX_PI_Q8)); }
static int32_t q8_cos_rad(int32_t radians) { return q8_cos_turn((int32_t)((radians * 32768) / CFX_PI_Q8)); }

static uint32_t isqrt_u32(uint32_t x)
{
    uint32_t op = x, res = 0, one = 1u << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) { op -= res + one; res = (res >> 1) + one; }
        else res >>= 1;
        one >>= 2;
    }
    return res;
}

static Vec3 v3(int32_t x, int32_t y, int32_t z) { Vec3 r = {x,y,z}; return r; }
static Vec3 vsub(Vec3 a, Vec3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static Vec3 vscale(Vec3 a, int32_t s) { return v3(q8_mul(a.x,s), q8_mul(a.y,s), q8_mul(a.z,s)); }
static int32_t vdot(Vec3 a, Vec3 b) { return q8_mul(a.x,b.x) + q8_mul(a.y,b.y) + q8_mul(a.z,b.z); }
static Vec3 vcross(Vec3 a, Vec3 b) { return v3(q8_mul(a.y,b.z)-q8_mul(a.z,b.y), q8_mul(a.z,b.x)-q8_mul(a.x,b.z), q8_mul(a.x,b.y)-q8_mul(a.y,b.x)); }
static Vec3 vnorm(Vec3 a)
{
    int32_t d = vdot(a, a);
    uint32_t len = isqrt_u32((uint32_t)(d > 0 ? d : 0) << Q8_SHIFT);
    return len > 1 ? vscale(a, q8_div(Q8_ONE, (int32_t)len)) : v3(0,0,Q8_ONE);
}

static Camera make_camera(Vec3 eye, Vec3 target, Vec3 up, int32_t focal)
{
    Camera c; c.eye = eye; c.target = target; c.up = up; c.focal = focal; return c;
}

static Camera lerp_camera(Camera a, Camera b, int32_t t)
{
    t = q8_smoothstep(t);
    Camera c;
    c.eye = v3(q8_lerp(a.eye.x, b.eye.x, t),
               q8_lerp(a.eye.y, b.eye.y, t),
               q8_lerp(a.eye.z, b.eye.z, t));
    c.target = v3(q8_lerp(a.target.x, b.target.x, t),
                  q8_lerp(a.target.y, b.target.y, t),
                  q8_lerp(a.target.z, b.target.z, t));
    c.up = vnorm(v3(q8_lerp(a.up.x, b.up.x, t),
                   q8_lerp(a.up.y, b.up.y, t),
                   q8_lerp(a.up.z, b.up.z, t)));
    c.focal = q8_lerp(a.focal, b.focal, t);
    return c;
}

static Camera player_camera(void)
{
    return make_camera(v3(0, Q8_FRAC(245,100), Q8_FRAC(505,100)), v3(0, 0, -Q8_FRAC(25,100)), v3(0,Q8_ONE,0), Q8_FROM_INT(148));
}

static Camera enemy_camera(void)
{
    return make_camera(v3(0, Q8_FRAC(245,100), -Q8_FRAC(505,100)), v3(0, 0, Q8_FRAC(25,100)), v3(0,Q8_ONE,0), Q8_FROM_INT(148));
}

static Camera top_camera(void)
{
    /* FM-style tactical top view: closer than the first headless pass,
       so the board fills the 256x240 screen instead of looking like a minimap. */
    return make_camera(v3(0, Q8_FRAC(502,100), Q8_FRAC(5,100)), v3(0, 0, 0), v3(0,0,-Q8_ONE), Q8_FROM_INT(202));
}

static Camera placement_camera(void)
{
    /* Slight perspective during the card fly-in: still readable as a top placement
       view, but with the front edge visible like the PS1 reference. */
    return make_camera(v3(0, Q8_FRAC(455,100), Q8_FRAC(215,100)), v3(0, 0, Q8_FRAC(18,100)), v3(0,Q8_ONE,0), Q8_FROM_INT(164));
}

static Camera enemy_placement_camera(void)
{
    /* Opponent placement must stay from COM's side. Earlier builds used the
       player placement camera here, which made the card appear to jump/swap
       to the wrong side of the field during the 00:06-00:07 beat. */
    return make_camera(v3(0, Q8_FRAC(455,100), -Q8_FRAC(215,100)), v3(0, 0, -Q8_FRAC(18,100)), v3(0,Q8_ONE,0), Q8_FROM_INT(164));
}

static Camera battle_top_camera(void)
{
    /* Player-oriented tactical/battle view. */
    return make_camera(v3(0, Q8_FRAC(505,100), Q8_FRAC(3,100)), v3(0, 0, 0), v3(0,0,-Q8_ONE), Q8_FROM_INT(196));
}

static Camera enemy_battle_top_camera(void)
{
    /* COM-oriented tactical/battle view. This preserves opponent POV instead of
       flipping to the player side during the 00:06-00:07 COM placement/attack. */
    return make_camera(v3(0, Q8_FRAC(505,100), -Q8_FRAC(3,100)), v3(0, 0, 0), v3(0,0,Q8_ONE), Q8_FROM_INT(196));
}

static Camera side_battle_camera(int attacker_row)
{
    return (attacker_row <= 1) ? enemy_battle_top_camera() : battle_top_camera();
}

static Camera opening_camera(int f)
{
    int32_t t = q8_ps1step(q8_ratio(f - 18, 66));
    int32_t angle = -Q8_FRAC(92,100) + q8_mul(Q8_FRAC(92,100), t);
    int32_t radius = Q8_FRAC(715,100) - q8_mul(Q8_FRAC(205,100), t);
    int32_t y = Q8_FRAC(345,100) - t;
    return make_camera(v3(q8_mul(q8_sin_rad(angle), radius), y, q8_mul(q8_cos_rad(angle), radius)), v3(0,0,-Q8_FRAC(15,100)), v3(0,Q8_ONE,0), Q8_FROM_INT(142) + q8_mul(Q8_FROM_INT(6), t));
}

static Camera turn_camera(int f, int start, int end, int to_enemy)
{
    int32_t t = q8_ps1step(q8_ratio(f - start, end - start));
    int32_t a0 = to_enemy ? 0 : CFX_PI_Q8;
    int32_t a1 = to_enemy ? CFX_PI_Q8 : 0;
    int32_t a = q8_lerp(a0, a1, t);
    int32_t st = q8_sin_pi(t);
    int32_t radius = Q8_FRAC(535,100) + q8_mul(Q8_FRAC(90,100), st);
    int32_t y = Q8_FRAC(245,100) + q8_mul(Q8_FRAC(85,100), st);
    return make_camera(v3(q8_mul(q8_sin_rad(a), radius), y, q8_mul(q8_cos_rad(a), radius)), v3(0,0,0), v3(0,Q8_ONE,0), Q8_FROM_INT(148));
}

static int32_t col_x0(int c);
static int32_t row_z0(int r);

static ScreenPt project_point(Camera cam, Vec3 p)
{
    Vec3 fwd = vnorm(vsub(cam.target, cam.eye));
    Vec3 right = vnorm(vcross(fwd, cam.up));
    Vec3 up = vcross(right, fwd);
    Vec3 rel = vsub(p, cam.eye);
    int32_t cx = vdot(rel, right);
    int32_t cy = vdot(rel, up);
    int32_t cz = vdot(rel, fwd);
    ScreenPt s;
    s.depth = cz;
    if (cz <= Q8_FRAC(5,100)) { s.x = s.y = 0; s.ok = 0; return s; }
    s.x = W / 2 + q8_to_int(q8_mul(q8_div(cx, cz), cam.focal));
    s.y = H / 2 - q8_to_int(q8_mul(q8_div(cy, cz), cam.focal));
    s.ok = 1;
    return s;
}

static int project_quad3d(Camera cam, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                          ScreenPt *pa, ScreenPt *pb, ScreenPt *pc, ScreenPt *pd)
{
    *pa = project_point(cam, a);
    *pb = project_point(cam, b);
    *pc = project_point(cam, c);
    *pd = project_point(cam, d);
    return pa->ok && pb->ok && pc->ok && pd->ok;
}

static void draw_quad3d_safe(Camera cam, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int tile)
{
    ScreenPt pa, pb, pc, pd;
    if (!project_quad3d(cam, a, b, c, d, &pa, &pb, &pc, &pd)) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const uint8_t *src = waifu_texture_atlas + ((size_t)tile * WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    TexV t0 = {pa.x, pa.y, 0, 0};
    TexV t1 = {pb.x, pb.y, Q8_ONE, 0};
    TexV t2 = {pc.x, pc.y, Q8_ONE, Q8_ONE};
    TexV t3 = {pd.x, pd.y, 0, Q8_ONE};
    draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t1, t2);
    draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t2, t3);
}

static void draw_quad3d(Camera cam, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int tile)
{
    ScreenPt pa, pb, pc, pd;
    if (!project_quad3d(cam, a, b, c, d, &pa, &pb, &pc, &pd)) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
    int ax = pa.x < 0 ? 0 : (pa.x >= W ? W - 1 : pa.x);
    int ay = pa.y < 0 ? 0 : (pa.y >= H ? H - 1 : pa.y);
    int bx = pb.x < 0 ? 0 : (pb.x >= W ? W - 1 : pb.x);
    int by = pb.y < 0 ? 0 : (pb.y >= H ? H - 1 : pb.y);
    int cx = pc.x < 0 ? 0 : (pc.x >= W ? W - 1 : pc.x);
    int cy = pc.y < 0 ? 0 : (pc.y >= H ? H - 1 : pc.y);
    int dx = pd.x < 0 ? 0 : (pd.x >= W ? W - 1 : pd.x);
    int dy = pd.y < 0 ? 0 : (pd.y >= H ? H - 1 : pd.y);
    Point2D p0 = {(DEFAULT_INT)ax, (DEFAULT_INT)ay, 0, 0};
    Point2D p1 = {(DEFAULT_INT)bx, (DEFAULT_INT)by, uvmax, 0};
    Point2D p2 = {(DEFAULT_INT)cx, (DEFAULT_INT)cy, uvmax, uvmax};
    Point2D p3 = {(DEFAULT_INT)dx, (DEFAULT_INT)dy, 0, uvmax};
    cfx_renderer3d_draw_quad(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
}

static int field_side_tile_for_cell(int c, int r)
{
    (void)c; (void)r;
    /* Dedicated side-wall material.  The wall is segmented per board cell, so
       this texture repeats locally instead of stretching across the full edge. */
    return 4;
}

static void draw_field_wall_z(Camera cam, int32_t z, int r_for_tile)
{
    int c;
    for (c = 0; c < BOARD_COLS; ++c) {
        int32_t x0 = col_x0(c), x1 = col_x0(c + 1);
        int tile = field_side_tile_for_cell(c, r_for_tile);
        draw_quad3d_safe(cam, v3(x0, FIELD_Y, z), v3(x1, FIELD_Y, z),
                              v3(x1, FIELD_THICK, z), v3(x0, FIELD_THICK, z), tile);
    }
}

static void draw_field_wall_x(Camera cam, int32_t x, int c_for_tile)
{
    int r;
    for (r = 0; r < BOARD_ROWS; ++r) {
        int32_t z0 = row_z0(r), z1 = row_z0(r + 1);
        int tile = field_side_tile_for_cell(c_for_tile, r);
        draw_quad3d_safe(cam, v3(x, FIELD_Y, z0), v3(x, FIELD_Y, z1),
                              v3(x, FIELD_THICK, z1), v3(x, FIELD_THICK, z0), tile);
    }
}

static void draw_field_slab_sides(Camera cam)
{
    /* The old renderer drew each slab side as one long textured quad.  Because
       draw_quad3d maps one 32x32 tile over the entire quad, the side texture was
       stretched across the full board edge and looked badly skewed.  Draw the
       walls in cell-sized segments so every side face gets a local UV mapping.
       With no z-buffer, draw the far edge first and the camera-facing edge last
       so side-wall corners do not overwrite the visible front wall. */
    if (cam.eye.z >= 0) {
        draw_field_wall_z(cam, FIELD_Z0, 0);
        if (cam.eye.x >= 0) {
            draw_field_wall_x(cam, FIELD_X0, 0);
            draw_field_wall_x(cam, FIELD_X1, BOARD_COLS - 1);
        } else {
            draw_field_wall_x(cam, FIELD_X1, BOARD_COLS - 1);
            draw_field_wall_x(cam, FIELD_X0, 0);
        }
        draw_field_wall_z(cam, FIELD_Z1, BOARD_ROWS - 1);
    } else {
        draw_field_wall_z(cam, FIELD_Z1, BOARD_ROWS - 1);
        if (cam.eye.x >= 0) {
            draw_field_wall_x(cam, FIELD_X0, 0);
            draw_field_wall_x(cam, FIELD_X1, BOARD_COLS - 1);
        } else {
            draw_field_wall_x(cam, FIELD_X1, BOARD_COLS - 1);
            draw_field_wall_x(cam, FIELD_X0, 0);
        }
        draw_field_wall_z(cam, FIELD_Z0, 0);
    }
}

/* ------------------------------------------------------------------------- */
/* 2D drawing */

static void clear_screen(uint8_t c) { memset(framebuffer, c, sizeof(framebuffer)); }

static void put_px(int x, int y, uint8_t c)
{
    if ((unsigned)x < W && (unsigned)y < H) framebuffer[y * W + x] = c;
}

static void hline(int x0, int x1, int y, uint8_t c)
{
    if ((unsigned)y >= H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= W) x1 = W - 1;
    memset(framebuffer + y * W + x0, c, (size_t)(x1 - x0 + 1));
}

static void rect_fill(int x, int y, int w, int h, uint8_t c)
{
    for (int yy = y; yy < y+h; ++yy) hline(x, x+w-1, yy, c);
}

static void rect_outline(int x, int y, int w, int h, uint8_t c)
{
    hline(x, x+w-1, y, c); hline(x, x+w-1, y+h-1, c);
    for (int yy = y; yy < y+h; ++yy) { put_px(x, yy, c); put_px(x+w-1, yy, c); }
}

static void line_i(int x0, int y0, int x1, int y1, uint8_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(x0,y0,c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_text(int x, int y, const char *s, uint8_t fg, uint8_t shadow)
{
    int ox = x;
    for (; *s; ++s) {
        if (*s == '\n') { y += 8; x = ox; continue; }
        unsigned char ch = (unsigned char)*s;
        const uint8_t *charfont = n2DLib_font + ((uint32_t)ch * 8u);
        for (int yy = 0; yy < 8; ++yy) {
            uint8_t row = charfont[yy];
            for (int xx = 0; xx < 8; ++xx) {
                if (row & (uint8_t)(1u << (7 - xx))) {
                    put_px(x + xx + 1, y + yy + 1, shadow);
                    put_px(x + xx, y + yy, fg);
                }
            }
        }
        x += 8;
    }
}

static void draw_text_small(int x, int y, const char *s, uint8_t fg, uint8_t shadow)
{
    /* Compact HUD text, but draw all 8 glyph columns. Earlier builds rendered
       only 6 columns, which clipped wide glyphs such as M and W whenever they
       appeared at the end of a word. Use a 7-pixel advance for PS1-style tight
       spacing while preserving the complete glyph bitmap. */
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        const uint8_t *charfont = n2DLib_font + ((uint32_t)ch * 8u);
        for (int yy = 0; yy < 8; ++yy) {
            uint8_t row = charfont[yy];
            for (int xx = 0; xx < 8; ++xx) {
                if (row & (uint8_t)(1u << (7 - xx))) {
                    put_px(x + xx + 1, y + yy + 1, shadow);
                    put_px(x + xx, y + yy, fg);
                }
            }
        }
        x += 7;
    }
}


static void draw_text_small_n(int x, int y, const char *s, int n, uint8_t fg, uint8_t shadow)
{
    char buf[64];
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    draw_text_small(x, y, buf, fg, shadow);
}

static void draw_text_small_ellipsis(int x, int y, const char *s, int max_chars, uint8_t fg, uint8_t shadow)
{
    char buf[64];
    int len = s ? (int)strlen(s) : 0;
    if (max_chars < 1) return;
    if (max_chars > 63) max_chars = 63;
    if (len <= max_chars) { draw_text_small(x, y, s ? s : "", fg, shadow); return; }
    if (max_chars <= 3) {
        int i;
        for (i = 0; i < max_chars; ++i) buf[i] = '.';
        buf[max_chars] = '\0';
    } else {
        memcpy(buf, s, (size_t)(max_chars - 3));
        buf[max_chars - 3] = '.';
        buf[max_chars - 2] = '.';
        buf[max_chars - 1] = '.';
        buf[max_chars] = '\0';
    }
    draw_text_small(x, y, buf, fg, shadow);
}

static int draw_wrapped_text_small_box(int x, int y, int max_w, int max_lines, int line_gap, const char *s, uint8_t fg, uint8_t shadow)
{
    const char *p = s ? s : "";
    int max_chars;
    int lines = 0;
    if (max_w < 7 || max_lines <= 0) return 0;
    max_chars = max_w / 7;
    if (max_chars < 1) max_chars = 1;
    if (max_chars > 60) max_chars = 60;

    while (*p && lines < max_lines) {
        int len, n, split = -1;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        len = (int)strlen(p);
        if (len <= max_chars) {
            draw_text_small(x, y + lines * line_gap, p, fg, shadow);
            ++lines;
            break;
        }

        for (int i = max_chars - 1; i > 0; --i) {
            char c = p[i];
            if (c == ' ' || c == '/' || c == ',' || c == '-') {
                split = i + ((c == '/' || c == ',' || c == '-') ? 1 : 0);
                break;
            }
        }
        n = (split > 0) ? split : max_chars;
        if (lines == max_lines - 1) {
            draw_text_small_ellipsis(x, y + lines * line_gap, p, max_chars, fg, shadow);
            ++lines;
            break;
        }
        draw_text_small_n(x, y + lines * line_gap, p, n, fg, shadow);
        p += n;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        ++lines;
    }
    return lines;
}

static void draw_wrapped_text_small(int x, int y, const char *s, int max_chars, uint8_t fg, uint8_t shadow)
{
    int len = (int)strlen(s);
    if (len <= max_chars) { draw_text_small(x, y, s, fg, shadow); return; }
    int split = max_chars;
    for (int i = max_chars; i > 6; --i) {
        if (s[i] == ' ' || s[i] == ',' || s[i] == '/') { split = i + (s[i] == ',' ? 1 : 0); break; }
    }
    draw_text_small_n(x, y, s, split, fg, shadow);
    while (s[split] == ' ' || s[split] == ',') split++;
    draw_text_small(x, y + 9, s + split, fg, shadow);
}

static void draw_panel_rect(int x, int y, int w, int h, uint8_t fill)
{
    rect_fill(x, y, w, h, fill);
    rect_outline(x, y, w, h, IDX_WHITE);
    rect_outline(x+1, y+1, w-2, h-2, IDX_UI_LIGHT);
    rect_outline(x+2, y+2, w-4, h-4, IDX_DIM);
}

static void draw_hud_offset(int field_ox, int field_oy, int lp_ox, int lp_oy)
{
    char lpbuf[16];
    draw_panel_rect(6 + field_ox, 7 + field_oy, 49, 29, IDX_UI_DARK);
    draw_text_small(11 + field_ox, 11 + field_oy, "FIELD", IDX_WHITE, IDX_BLACK);
    draw_text(21 + field_ox, 23 + field_oy, "MARE", IDX_WHITE, IDX_BLACK);

    draw_panel_rect(177 + lp_ox, 7 + lp_oy, 71, 12, IDX_UI_DARK);
    rect_fill(179 + lp_ox, 9 + lp_oy, 23, 8, IDX_UI_BLUE);
    draw_text_small(181 + lp_ox, 9 + lp_oy, "COM", IDX_WHITE, IDX_BLACK);
    snprintf(lpbuf, sizeof(lpbuf), "%4d", g_com_lp);
    draw_text_small(209 + lp_ox, 9 + lp_oy, lpbuf, IDX_GOLD_HI, IDX_BLACK);

    draw_panel_rect(177 + lp_ox, 23 + lp_oy, 71, 12, IDX_UI_DARK);
    rect_fill(179 + lp_ox, 25 + lp_oy, 23, 8, IDX_UI_RED);
    draw_text_small(181 + lp_ox, 25 + lp_oy, "YOU", IDX_WHITE, IDX_BLACK);
    snprintf(lpbuf, sizeof(lpbuf), "%4d", g_you_lp);
    draw_text_small(209 + lp_ox, 25 + lp_oy, lpbuf, IDX_GOLD_HI, IDX_BLACK);
}

static void draw_hud(void)
{
    draw_hud_offset(0, 0, 0, 0);
}

static uint8_t stat_delta_color(int delta)
{
    if (delta > 0) return IDX_GREEN;
    if (delta < 0) return IDX_RED;
    return IDX_WHITE;
}

static void draw_bottom_info_offset_ex(int card_id, const char *mode, int yoff, int atk, int defv)
{
    (void)mode;
    int base = 205 + yoff;
    rect_fill(0, base, 256, 35, IDX_UI_TEAL);
    hline(0,255,base,IDX_WHITE); hline(0,255,base+1,IDX_UI_LIGHT); hline(0,255,base+2,IDX_DIM);
    for (int y = base+4; y < base+35; y += 3) hline(0,255,y,IDX_UI_TEAL2);
    char line[64];
    if (is_support_card(card_id)) {
        char support_line[64];
        snprintf(support_line, sizeof(support_line), "%.24s", support_card_name(card_id));
        draw_text(6, base+6, support_line, IDX_WHITE, IDX_BLACK);
        snprintf(support_line, sizeof(support_line), "%.31s", support_card_type(card_id));
        draw_text_small(6, base+21, support_line, IDX_WHITE, IDX_BLACK);
        draw_text_small(188, base+21, "USE", IDX_GOLD_HI, IDX_BLACK);
        return;
    }
    if (!is_monster_card(card_id)) return;
    snprintf(line, sizeof(line), "%.24s", waifu_card_names[card_id]);
    draw_text(6, base+6, line, IDX_WHITE, IDX_BLACK);
    snprintf(line, sizeof(line), "%s / %s", waifu_card_attr[card_id], waifu_card_tribe[card_id]);
    draw_text_small(6, base+21, line, IDX_WHITE, IDX_BLACK);
    if (atk < 0) atk = (int)waifu_card_atk[card_id];
    if (defv < 0) defv = (int)waifu_card_def[card_id];
    snprintf(line, sizeof(line), "x%d", atk);
    draw_text_small(215, base+15, line, stat_delta_color(atk - (int)waifu_card_atk[card_id]), IDX_BLACK);
    snprintf(line, sizeof(line), "%d", defv);
    draw_text_small(221, base+26, line, stat_delta_color(defv - (int)waifu_card_def[card_id]), IDX_BLACK);
    rect_outline(215,base+25,6,6,IDX_WHITE);
}

static void draw_bottom_info_offset(int card_id, const char *mode, int yoff)
{
    draw_bottom_info_offset_ex(card_id, mode, yoff, -1, -1);
}

static void draw_bottom_info(int card_id, const char *mode)
{
    draw_bottom_info_offset(card_id, mode, 0);
}

static void draw_bottom_info_field(int owner, int slot, const char *mode);

static uint8_t gray_card_px(uint8_t src);

static void draw_card_raw(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    if (dw <= 0 || dh <= 0) return;
    for (int yy = 0; yy < dh; ++yy) {
        int sy = (yy * sh) / dh;
        int dy = y + yy;
        if ((unsigned)dy >= H) continue;
        for (int xx = 0; xx < dw; ++xx) {
            int sx = (xx * sw) / dw;
            int dx = x + xx;
            if ((unsigned)dx >= W) continue;
            framebuffer[dy * W + dx] = src[sy * sw + sx];
        }
    }
}

static void draw_card_raw_gray(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    if (dw <= 0 || dh <= 0) return;
    for (int yy = 0; yy < dh; ++yy) {
        int sy = (yy * sh) / dh;
        int dy = y + yy;
        if ((unsigned)dy >= H) continue;
        for (int xx = 0; xx < dw; ++xx) {
            int sx = (xx * sw) / dw;
            int dx = x + xx;
            if ((unsigned)dx >= W) continue;
            framebuffer[dy * W + dx] = gray_card_px(src[sy * sw + sx]);
        }
    }
}

static const uint8_t *card_face_ptr(int id)
{
    if (id < 0) id = 0;
    if (id >= WAIFU_CARD_COUNT) id = WAIFU_CARD_COUNT - 1;
    return waifu_card_faces + ((size_t)id * WAIFU_CARD_W * WAIFU_CARD_H);
}

static const uint8_t *big_art_ptr(int id)
{
    if (id < 0) id = 0;
    if (id >= WAIFU_CARD_COUNT) id = WAIFU_CARD_COUNT - 1;
    return waifu_big_art + ((size_t)id * WAIFU_BIG_W * WAIFU_BIG_H);
}

static void draw_card_sprite(int id, int x, int y, int w, int h, int back)
{
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
    draw_card_raw(back ? waifu_card_back : card_face_ptr(id), WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
}

static void draw_card_sprite_ex(int id, int x, int y, int w, int h, int back, int gray)
{
    const uint8_t *src = back ? waifu_card_back : card_face_ptr(id);
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
    if (gray) draw_card_raw_gray(src, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
    else draw_card_raw(src, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
}

static void draw_support_sprite(int x, int y, int w, int h)
{
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
    draw_card_raw(waifu_support_face, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
}

static void draw_hand_card_sprite(int id, int x, int y, int w, int h, int back)
{
    if (back) { draw_card_sprite(id, x, y, w, h, 1); return; }
    if (is_support_card(id)) draw_support_sprite(x, y, w, h);
    else draw_card_sprite(id, x, y, w, h, back);
}

static void draw_hand_card_sprite_ex(int id, int x, int y, int w, int h, int back, int gray)
{
    if (back) { draw_card_sprite_ex(id, x, y, w, h, 1, gray); return; }
    if (is_support_card(id)) draw_support_sprite(x, y, w, h);
    else draw_card_sprite_ex(id, x, y, w, h, back, gray);
}

static void draw_big_battle_card_stats(int id, int x, int y, int back, int atk, int defv)
{
    /* 120x160-ish duel cut-in card with a 112x112 art window. This matches the
       Forbidden Memories black battle card display much more closely than using
       the tiny 38x54 field/hand card. */
    const int w = 120, h = 160;
    rect_fill(x+3, y+4, w, h, IDX_BLACK);
    rect_fill(x, y, w, h, IDX_CARD_RIM);
    rect_outline(x, y, w, h, IDX_GOLD_HI);
    rect_outline(x+1, y+1, w-2, h-2, IDX_GOLD_DARK);
    rect_fill(x+4, y+4, w-8, h-8, IDX_CARD_GOLD);
    if (back) {
        draw_card_raw(waifu_card_back, WAIFU_CARD_W, WAIFU_CARD_H, x+4, y+4, w-8, h-8);
        return;
    } else {
        draw_card_raw(big_art_ptr(id), WAIFU_BIG_W, WAIFU_BIG_H, x+4, y+6, 112, 112);
    }
    rect_outline(x+3, y+5, 114, 114, IDX_CARD_RIM);
    rect_fill(x+5, y+123, 110, 28, IDX_DARK_BROWN);
    rect_outline(x+5, y+123, 110, 28, IDX_GOLD_DARK);
    if (!back) {
        char stats[32];
        if (atk < 0) atk = (int)waifu_card_atk[id];
        if (defv < 0) defv = (int)waifu_card_def[id];
        snprintf(stats, sizeof(stats), "A%d", atk);
        draw_text_small(x+9, y+128, stats, stat_delta_color(atk - (int)waifu_card_atk[id]), IDX_BLACK);
        snprintf(stats, sizeof(stats), "D%d", defv);
        draw_text_small(x+55, y+128, stats, stat_delta_color(defv - (int)waifu_card_def[id]), IDX_BLACK);
        draw_text_small(x+9, y+140, waifu_card_tribe[id], IDX_WHITE, IDX_BLACK);
    }
}

static void draw_big_battle_card(int id, int x, int y, int back)
{
    draw_big_battle_card_stats(id, x, y, back, -1, -1);
}


static void draw_big_battle_card_rect(int id, int x, int y, int w, int h, int back)
{
    /* Horizontally-scalable version used only for the face-down battle reveal.
       It deliberately squashes the whole framed card, producing the PS1-like
       card-turn silhouette before the real battle effect starts. */
    if (w < 2 || h < 8) return;
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
    rect_fill(x, y, w, h, IDX_CARD_RIM);
    rect_outline(x, y, w, h, IDX_GOLD_HI);
    if (w > 8 && h > 14) {
        if (back) {
            draw_card_raw(waifu_card_back, WAIFU_CARD_W, WAIFU_CARD_H, x+3, y+3, w-6, h-6);
        } else {
            rect_fill(x+3, y+3, w-6, h-6, IDX_CARD_GOLD);
            int art_w = w - 8;
            int art_h = h > 134 ? 112 : h - 20;
            if (art_w < 1) art_w = 1;
            if (art_h < 1) art_h = 1;
            draw_card_raw(big_art_ptr(id), WAIFU_BIG_W, WAIFU_BIG_H, x+4, y+6, art_w, art_h);
        }
    } else {
        rect_fill(x, y, w, h, IDX_WHITE);
    }
}

static void draw_big_battle_card_flip(int id, int x, int y, int frame, int duration)
{
    if (frame < 0) frame = 0;
    if (frame >= duration) frame = duration - 1;
    int32_t t = q8_ratio(frame, duration - 1);
    int32_t half = t < Q8_HALF ? t * 2 : (t - Q8_HALF) * 2;
    int showing_back = t < Q8_HALF;
    int w;
    half = q8_smoothstep(half);
    if (showing_back) w = lerp_i(120, 5, half);
    else              w = lerp_i(5, 120, half);
    if (w < 4) w = 4;
    int x2 = x + (120 - w) / 2;
    draw_big_battle_card_rect(id, x2, y, w, 160, showing_back);
    if (w <= 8) rect_fill(x + 58, y + 3, 4, 154, IDX_WHITE);
}

static void draw_red_cursor(int x, int y, int w, int h)
{
    /* FM-readable red selector: full-card outline plus small red chevrons.
       This remains visible even on the first/leftmost card where a left-only
       pointer would clip offscreen. */
    rect_outline(x-2, y-2, w+4, h+4, IDX_RED);
    rect_outline(x-3, y-3, w+6, h+6, IDX_UI_RED);
    hline(x-1, x+w, y-5, IDX_RED);
    hline(x-1, x+w, y+h+4, IDX_RED);

    int cx = x + w/2;
    for (int r = 0; r < 7; ++r) {
        hline(cx - r, cx + r, y - 12 + r, IDX_RED);       /* downward arrow above */
        hline(cx - r, cx + r, y + h + 11 - r, IDX_RED);   /* upward arrow below */
    }
}

static int hand_final_x(int i) { return 12 + i * 47; }
static int hand_y(void) { return 154 + g_player_hand_offset_y; }

static void draw_player_hand(int f, int selected)
{
    for (int i = 0; i < 5; ++i) {
        int x0 = hand_final_x(i);
        int y = hand_y();
        int x = x0;
        if (f < 116) {
            int32_t t = q8_smooth_ratio(f - (84 + i * 4), 12);
            x = lerp_i(272, x0, t);
        }
        if (i == g_player_hide_index) continue;
        if (((f >= 150 && f < 176) || (f >= 475 && f < 505) || (f >= 910 && f < 930)) && i == selected) continue;
        if (i == 2) draw_support_sprite(x, y+1, 38, 50);
        else draw_card_sprite(hand_ids[i], x, y, 38, 50, 0);
        if (!g_suppress_hand_cursor && i == selected && f >= 102) draw_red_cursor(x, y, 38, 50);
    }
}

static void draw_player_hand_draw_sequence(int f, int start, int selected)
{
    /* FM-like turn-start restoration: the row scrolls up, then replacement
       cards visibly draw from the right edge.  This is deliberately slower and
       clearer than v11 so the draw is legible in the full-match video. */
    int draw_slots[2] = {0, 4};
    int reveal_cursor = (f >= start + 76);
    int yoff = lerp_i(92, 0, q8_smooth_ratio(f - start, 30));

    if (f >= start + 20 && f < start + 78) draw_text_small(211, 142 + yoff / 3, "DRAW", IDX_GOLD_HI, IDX_BLACK);

    for (int i = 0; i < 5; ++i) {
        int x0 = hand_final_x(i);
        int y = 154 + yoff;
        int x = x0;
        int visible = 1;
        for (int d = 0; d < 2; ++d) {
            if (i == draw_slots[d]) {
                int32_t t = q8_smooth_ratio(f - (start + 24 + d * 20), 24);
                if (t <= 0) visible = 0;
                x = lerp_i(282, x0, t);
            }
        }
        if (!visible) continue;
        if (i == 2) draw_support_sprite(x, y+1, 38, 50);
        else draw_card_sprite(hand_ids[i], x, y, 38, 50, 0);
        if (reveal_cursor && i == selected) draw_red_cursor(x, y, 38, 50);
    }
}

static void draw_enemy_hand(int f, int selected, int reveal_one)
{
    (void)reveal_one;
    for (int i = 0; i < 5; ++i) {
        int x = 12 + i * 48;
        int y = 154 + g_enemy_hand_offset_y;
        if (i == g_enemy_hide_index) continue;
        if (((f >= 256 && f < 278) || (f >= 675 && f < 705)) && i == selected) continue;
        draw_card_sprite(0, x, y, 36, 49, 1);
        if (!g_suppress_hand_cursor && i == selected) draw_red_cursor(x, y, 36, 49);
    }
}


static void draw_enemy_hand_draw_sequence(int f, int start, int selected)
{
    int yoff = lerp_i(92, 0, q8_smooth_ratio(f - start, 30)) + g_enemy_hand_offset_y;
    if (f >= start + 14 && f < start + 70) draw_text_small(211, 142 + yoff / 3, "DRAW", IDX_GOLD_HI, IDX_BLACK);
    for (int i = 0; i < 5; ++i) {
        int x0 = 12 + i * 48;
        int32_t t = q8_smooth_ratio(f - (start + i * 6), 20);
        if (t <= 0) continue;
        int x = lerp_i(282, x0, t);
        int y = 154 + yoff;
        draw_card_sprite(0, x, y, 36, 49, 1);
        if (f >= start + 42 && i == selected) draw_red_cursor(x, y, 36, 49);
    }
}

/* ------------------------------------------------------------------------- */
/* Board rendering and board-card placement. */

static int32_t col_xq(int32_t c) { return FIELD_X0 + (int32_t)(((FIELD_X1 - FIELD_X0) * c) / (BOARD_COLS * Q8_ONE)); }
static int32_t row_zq(int32_t r) { return FIELD_Z0 + (int32_t)(((FIELD_Z1 - FIELD_Z0) * r) / (BOARD_ROWS * Q8_ONE)); }
static int32_t col_x0(int c) { return col_xq(Q8_FROM_INT(c)); }
static int32_t row_z0(int r) { return row_zq(Q8_FROM_INT(r)); }
static int32_t zone_cx(int c) { return (col_x0(c) + col_x0(c+1)) / 2; }
static int32_t zone_cz(int r) { return (row_z0(r) + row_z0(r+1)) / 2; }

static void draw_grid_line(Camera cam, Vec3 a, Vec3 b, uint8_t c)
{
    ScreenPt pa = project_point(cam, a), pb = project_point(cam, b);
    if (pa.ok && pb.ok) {
        line_i(pa.x, pa.y, pb.x, pb.y, c);
        line_i(pa.x, pa.y+1, pb.x, pb.y+1, IDX_DARK_BROWN);
    }
}

static void render_board(Camera cam)
{
    /* Every 3D field frame must start from a clean black framebuffer.
       The SDL 1.2 frontend exposed stale title/menu/hand pixels because the
       interactive field renderer only drew board geometry and UI, leaving
       untouched black-space pixels from the previous frame. Headless PNG
       tests were less likely to show it because most scripted paths cleared
       before calling render_board(). Clearing here makes the core renderer
       platform-agnostic and guarantees both SDL and headless produce the same
       full framebuffer. */
    clear_screen(IDX_BLACK);

    /* slab sides first */
    draw_field_slab_sides(cam);

    for (int r = 0; r < BOARD_ROWS; ++r) {
        for (int c = 0; c < BOARD_COLS; ++c) {
            int32_t x0 = col_x0(c), x1 = col_x0(c+1);
            int32_t z0 = row_z0(r), z1 = row_z0(r+1);
            /* Deliberately obvious PS1-era checker pattern: one bright gold tile,
               then one darker brown/gold tile. The prior two gold variants were
               too close and read as a flat texture instead of a board. */
            int tile = ((r + c) & 1) ? 5 : 1;
            draw_quad3d(cam, v3(x0, FIELD_Y, z0), v3(x1, FIELD_Y, z0), v3(x1, FIELD_Y, z1), v3(x0, FIELD_Y, z1), tile);
        }
    }

    for (int c = 0; c <= BOARD_COLS; ++c) draw_grid_line(cam, v3(col_x0(c),Q8_FRAC(3,100),FIELD_Z0), v3(col_x0(c),Q8_FRAC(3,100),FIELD_Z1), IDX_DARK_BROWN);
    for (int r = 0; r <= BOARD_ROWS; ++r) draw_grid_line(cam, v3(FIELD_X0,Q8_FRAC(3,100),row_z0(r)), v3(FIELD_X1,Q8_FRAC(3,100),row_z0(r)), IDX_DARK_BROWN);

    /* faint central Egyptian mark */
    ScreenPt a = project_point(cam, v3(-Q8_FRAC(55,100),Q8_FRAC(5,100),-Q8_FRAC(50,100)));
    ScreenPt b = project_point(cam, v3(0,Q8_FRAC(5,100),Q8_FRAC(25,100)));
    ScreenPt c = project_point(cam, v3(Q8_FRAC(55,100),Q8_FRAC(5,100),-Q8_FRAC(50,100)));
    if (a.ok && b.ok && c.ok) { line_i(a.x,a.y,b.x,b.y,IDX_GOLD_DARK); line_i(b.x,b.y,c.x,c.y,IDX_GOLD_DARK); }
}

static uint8_t g_gray_lut[256];
static int g_gray_lut_ready = 0;

static void init_gray_lut(void)
{
    int i, j;
    if (g_gray_lut_ready) return;
    for (i = 0; i < 256; ++i) {
        int r = waifu_palette_rgb[i * 3 + 0];
        int g = waifu_palette_rgb[i * 3 + 1];
        int b = waifu_palette_rgb[i * 3 + 2];
        int lum = (r * 30 + g * 59 + b * 11) / 100;
        int target = (lum * 58) / 100;
        int best = IDX_DIM;
        int best_score = 0x7fffffff;
        for (j = 0; j < 256; ++j) {
            int rr = waifu_palette_rgb[j * 3 + 0];
            int gg = waifu_palette_rgb[j * 3 + 1];
            int bb = waifu_palette_rgb[j * 3 + 2];
            int jr = rr - target;
            int jg = gg - target;
            int jb = bb - target;
            int chroma = abs(rr - gg) + abs(gg - bb) + abs(bb - rr);
            int score = jr * jr + jg * jg + jb * jb + chroma * 3;
            if (score < best_score) { best_score = score; best = j; }
        }
        g_gray_lut[i] = (uint8_t)best;
    }
    g_gray_lut[IDX_BLACK] = IDX_BLACK;
    g_gray_lut_ready = 1;
}

static uint8_t gray_card_px(uint8_t src)
{
    if (!g_gray_lut_ready) init_gray_lut();
    return g_gray_lut[src];
}

static void draw_textured_tri_ex(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c, int gray)
{
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    int den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (den == 0) return;
    if (minx < 0) minx = 0;
    if (maxx >= W) maxx = W - 1;
    if (miny < 0) miny = 0;
    if (maxy >= H) maxy = H - 1;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int px2 = x * 2 + 1, py2 = y * 2 + 1;
            int wa2 = (b.y - c.y) * (px2 - c.x * 2) + (c.x - b.x) * (py2 - c.y * 2);
            int wb2 = (c.y - a.y) * (px2 - c.x * 2) + (a.x - c.x) * (py2 - c.y * 2);
            int wc2 = den * 2 - wa2 - wb2;
            if ((den > 0 && wa2 >= 0 && wb2 >= 0 && wc2 >= 0) ||
                (den < 0 && wa2 <= 0 && wb2 <= 0 && wc2 <= 0)) {
                int u = (wa2 * a.u + wb2 * b.u + wc2 * c.u) / (den * 2);
                int v = (wa2 * a.v + wb2 * b.v + wc2 * c.v) / (den * 2);
                int sx = (u * (sw - 1) + Q8_HALF) >> Q8_SHIFT;
                int sy = (v * (sh - 1) + Q8_HALF) >> Q8_SHIFT;
                if (sx < 0) sx = 0;
                if (sx >= sw) sx = sw - 1;
                if (sy < 0) sy = 0;
                if (sy >= sh) sy = sh - 1;
                uint8_t pix = src[sy * sw + sx];
                put_px(x, y, gray ? (((x + y) & 1) ? gray_card_px(pix) : pix) : pix);
            }
        }
    }
}

static void draw_textured_tri(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c)
{
    draw_textured_tri_ex(src, sw, sh, a, b, c, 0);
}

static void draw_tri3d_tile(Camera cam, Vec3 a, Vec3 b, Vec3 c, int tile, int flip_u)
{
    ScreenPt pa = project_point(cam, a), pb = project_point(cam, b), pc = project_point(cam, c);
    if (!pa.ok || !pb.ok || !pc.ok) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const uint8_t *src = waifu_texture_atlas + ((size_t)tile * WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    TexV ta = {pa.x, pa.y, flip_u ? Q8_ONE : 0, Q8_ONE};
    TexV tb = {pb.x, pb.y, flip_u ? 0 : Q8_ONE, Q8_ONE};
    TexV tc = {pc.x, pc.y, Q8_HALF, 0};
    draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, ta, tb, tc);
    line_i(pa.x, pa.y, pb.x, pb.y, IDX_GOLD_DARK);
    line_i(pb.x, pb.y, pc.x, pc.y, IDX_GOLD_DARK);
    line_i(pc.x, pc.y, pa.x, pa.y, IDX_GOLD_DARK);
}

/* Pyramid face renderer: tiles the texture in multiple rows and columns
   across the triangular face instead of stretching one tile into the
   whole triangle.  base0/base1 are the bottom corners, apex is the top.
   rows = number of texture repeats base-to-apex, cols = repeats across
   the base width.  This produces stacked brick courses like real masonry
   instead of a single distorted square. */
static void draw_tri3d_pyramid_face(Camera cam, Vec3 base0, Vec3 base1, Vec3 apex_v, int tile, int flip_u, int rows, int cols)
{
    ScreenPt pa = project_point(cam, base0), pb = project_point(cam, base1), pc = project_point(cam, apex_v);
    if (!pa.ok || !pb.ok || !pc.ok) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const uint8_t *src = waifu_texture_atlas + ((size_t)tile * WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    int sw = WAIFU_TEX_TILE_SIZE, sh = WAIFU_TEX_TILE_SIZE;
    int minx, maxx, miny, maxy;
    int den;
    minx = pa.x < pb.x ? (pa.x < pc.x ? pa.x : pc.x) : (pb.x < pc.x ? pb.x : pc.x);
    maxx = pa.x > pb.x ? (pa.x > pc.x ? pa.x : pc.x) : (pb.x > pc.x ? pb.x : pc.x);
    miny = pa.y < pb.y ? (pa.y < pc.y ? pa.y : pc.y) : (pb.y < pc.y ? pb.y : pc.y);
    maxy = pa.y > pb.y ? (pa.y > pc.y ? pa.y : pc.y) : (pb.y > pc.y ? pb.y : pc.y);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;
    den = (pb.y - pc.y) * (pa.x - pc.x) + (pc.x - pb.x) * (pa.y - pc.y);
    if (den == 0) return;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int px2 = x * 2 + 1, py2 = y * 2 + 1;
            int wa2 = (pb.y - pc.y) * (px2 - pc.x * 2) + (pc.x - pb.x) * (py2 - pc.y * 2);
            int wb2 = (pc.y - pa.y) * (px2 - pc.x * 2) + (pa.x - pc.x) * (py2 - pc.y * 2);
            int wc2 = den * 2 - wa2 - wb2;
            if ((den > 0 && wa2 >= 0 && wb2 >= 0 && wc2 >= 0) ||
                (den < 0 && wa2 <= 0 && wb2 <= 0 && wc2 <= 0)) {
                int width = wa2 + wb2;
                int abs_width = width < 0 ? -width : width;
                int vq = (width * Q8_ONE) / (den * 2);
                int vt = (vq * rows) & (Q8_ONE - 1);
                int uq = abs_width > 0 ? (wa2 * Q8_ONE) / width : Q8_HALF;
                if (flip_u) uq = Q8_ONE - uq;
                int ut = (uq * cols) & (Q8_ONE - 1);
                int sx = (ut * (sw - 1) + Q8_HALF) >> Q8_SHIFT;
                int sy = ((Q8_ONE - vt) * (sh - 1) + Q8_HALF) >> Q8_SHIFT;
                if (sx < 0) sx = 0;
                if (sx >= sw) sx = sw - 1;
                if (sy < 0) sy = 0;
                if (sy >= sh) sy = sh - 1;
                put_px(x, y, src[sy * sw + sx]);
            }
        }
    }
    line_i(pa.x, pa.y, pb.x, pb.y, IDX_GOLD_DARK);
    line_i(pb.x, pb.y, pc.x, pc.y, IDX_GOLD_DARK);
    line_i(pc.x, pc.y, pa.x, pa.y, IDX_GOLD_DARK);
}

static void draw_projected_card_quad_ex(const uint8_t *src, int sw, int sh,
                                        ScreenPt p0, ScreenPt p1, ScreenPt p2, ScreenPt p3,
                                        int gray)
{
    if (!p0.ok || !p1.ok || !p2.ok || !p3.ok) return;
    TexV a = {p0.x, p0.y, 0, 0};
    TexV b = {p1.x, p1.y, Q8_ONE, 0};
    TexV c = {p2.x, p2.y, Q8_ONE, Q8_ONE};
    TexV d = {p3.x, p3.y, 0, Q8_ONE};
    draw_textured_tri_ex(src, sw, sh, a, b, c, gray);
    draw_textured_tri_ex(src, sw, sh, a, c, d, gray);
    line_i(p0.x,p0.y,p1.x,p1.y, gray ? IDX_DIM : IDX_CARD_RIM);
    line_i(p1.x,p1.y,p2.x,p2.y, gray ? IDX_DIM : IDX_CARD_RIM);
    line_i(p2.x,p2.y,p3.x,p3.y, gray ? IDX_DIM : IDX_CARD_RIM);
    line_i(p3.x,p3.y,p0.x,p0.y, gray ? IDX_DIM : IDX_CARD_RIM);
    if (gray) {
        line_i(p0.x,p0.y,p2.x,p2.y,IDX_DIM);
        line_i(p1.x,p1.y,p3.x,p3.y,IDX_DIM);
    }
}

static void draw_projected_card_quad(const uint8_t *src, int sw, int sh,
                                     ScreenPt p0, ScreenPt p1, ScreenPt p2, ScreenPt p3)
{
    draw_projected_card_quad_ex(src, sw, sh, p0, p1, p2, p3, 0);
}

static void draw_board_card_state(Camera cam, int col, int row, int card_id, int back, int gray, int defense)
{
    /* Real flat textured field card: project the four card corners on the 3D
       board plane and affine-map the 38x54 indexed card texture into the quad.
       This replaces the old billboard sprite so cards now lie on the field.
       In defense position the card is rotated 90 degrees; swap the quad's
       half-extents so the 38x54 texture keeps its aspect ratio instead of
       stretching when the UVs are rotated. */
    int32_t cx = zone_cx(col), cz = zone_cz(row);
    int32_t hw = defense ? Q8_FRAC(50,100) : Q8_FRAC(36,100);
    int32_t hz = defense ? Q8_FRAC(36,100) : Q8_FRAC(50,100);
    int32_t y = Q8_FRAC(115,1000);
    const uint8_t *tex = back ? waifu_card_back : (is_support_card(card_id) ? waifu_support_face : card_face_ptr(card_id));
    ScreenPt p0 = project_point(cam, v3(cx - hw, y, cz - hz));
    ScreenPt p1 = project_point(cam, v3(cx + hw, y, cz - hz));
    ScreenPt p2 = project_point(cam, v3(cx + hw, y, cz + hz));
    ScreenPt p3 = project_point(cam, v3(cx - hw, y, cz + hz));
    /* Player-side cards face YOU. COM-side cards are rotated 180 degrees on
       the board plane so they face the opponent instead of always facing YOU. */
    if (defense) {
        if (row <= 1) draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p3, p0, p1, p2, gray);
        else          draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p1, p2, p3, p0, gray);
    } else {
        if (row <= 1) draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p2, p3, p0, p1, gray);
        else          draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p0, p1, p2, p3, gray);
    }
}

static void draw_board_card_ex(Camera cam, int col, int row, int card_id, int back, int gray)
{
    draw_board_card_state(cam, col, row, card_id, back, gray, 0);
}

static void draw_board_card(Camera cam, int col, int row, int card_id, int back)
{
    draw_board_card_ex(cam, col, row, card_id, back, 0);
}

static void draw_zone_cursor_q(Camera cam, int32_t col, int32_t row)
{
    int32_t x0 = col_xq(col), x1 = col_xq(col + Q8_ONE);
    int32_t z0 = row_zq(row), z1 = row_zq(row + Q8_ONE);
    ScreenPt p0 = project_point(cam, v3(x0,Q8_FRAC(10,100),z0));
    ScreenPt p1 = project_point(cam, v3(x1,Q8_FRAC(10,100),z0));
    ScreenPt p2 = project_point(cam, v3(x1,Q8_FRAC(10,100),z1));
    ScreenPt p3 = project_point(cam, v3(x0,Q8_FRAC(10,100),z1));
    if (!p0.ok || !p1.ok || !p2.ok || !p3.ok) return;
    line_i(p0.x,p0.y,p1.x,p1.y,IDX_RED); line_i(p1.x,p1.y,p2.x,p2.y,IDX_RED);
    line_i(p2.x,p2.y,p3.x,p3.y,IDX_RED); line_i(p3.x,p3.y,p0.x,p0.y,IDX_RED);
    line_i(p0.x+1,p0.y,p1.x+1,p1.y,IDX_RED); line_i(p3.x+1,p3.y,p2.x+1,p2.y,IDX_RED);
}

static void draw_zone_cursor(Camera cam, int col, int row)
{
    draw_zone_cursor_q(cam, Q8_FROM_INT(col), Q8_FROM_INT(row));
}

static void draw_top_selector_cursor(Camera cam)
{
    int32_t t = q8_smooth_ratio(g_b_top_cursor_anim, 8);
    int32_t col = Q8_FROM_INT(g_b_top_prev_col) + q8_mul(Q8_FROM_INT(g_b_top_col - g_b_top_prev_col), t);
    int32_t row = Q8_FROM_INT(g_b_top_prev_row) + q8_mul(Q8_FROM_INT(g_b_top_row - g_b_top_prev_row), t);
    draw_zone_cursor_q(cam, col, row);
    if (g_b_top_cursor_anim < 8) ++g_b_top_cursor_anim;
}

static void draw_flying_card(Camera cam, int card_id, int hand_index, int target_col, int target_row, int frame, int start, int end, int back)
{
    int32_t t = q8_ratio(frame - start, end - start);
    int flip_to_back = (back == 2);
    int render_back = flip_to_back ? 0 : back;
    /* PS1-style placement beat: card jumps out of the hand, hangs large at
       center, glides over the selected slot, then snaps down with a landing
       flash. The actual field state is committed only after this completes. */
    int sx = hand_final_x(hand_index);
    int sy = 154 + ((target_row <= 1) ? g_enemy_hand_offset_y : g_player_hand_offset_y) - 2;
    int midx = 104, midy = 82;
    int midw = 48, midh = 66;

    ScreenPt dst = project_point(cam, v3(zone_cx(target_col), Q8_FRAC(10,100), zone_cz(target_row)));
    if (!dst.ok) return;
    int dx = dst.x - 14, dy = dst.y - 20;

    int x, y, w, h;
    if (t < Q8_FRAC(28,100)) {
        int32_t a = q8_smoothstep(q8_div(t, Q8_FRAC(28,100)));
        x = lerp_i(sx, midx, a);
        y = lerp_i(sy, midy, a);
        w = lerp_i(38, midw, a);
        h = lerp_i(50, midh, a);
    } else if (t < Q8_HALF) {
        int32_t a = q8_div(t - Q8_FRAC(28,100), Q8_FRAC(22,100));
        x = midx;
        y = midy - q8_to_int(q8_mul(Q8_FROM_INT(5), q8_sin_pi(a)));
        w = midw;
        h = midh;
    } else if (t < Q8_FRAC(86,100)) {
        int32_t a = q8_smoothstep(q8_div(t - Q8_HALF, Q8_FRAC(36,100)));
        int bob = -q8_to_int(q8_mul(Q8_FROM_INT(14), q8_sin_pi(a)));
        x = lerp_i(midx, dx, a);
        y = lerp_i(midy, dy, a) + bob;
        w = lerp_i(midw, 28, a);
        h = lerp_i(midh, 39, a);
    } else {
        int32_t a = q8_smoothstep(q8_div(t - Q8_FRAC(86,100), Q8_FRAC(14,100)));
        int snap = q8_to_int(q8_mul(Q8_FROM_INT(4), q8_sin_pi(a)));
        x = dx;
        y = dy - snap;
        w = 28;
        h = 39;
    }

    if (flip_to_back) {
        if (t < Q8_FRAC(28,100)) {
            render_back = 0;
        } else if (t < Q8_HALF) {
            int32_t ft = q8_div(t - Q8_FRAC(28,100), Q8_FRAC(22,100));
            int32_t half = ft < Q8_HALF ? (ft * 2) : ((ft - Q8_HALF) * 2);
            int old_w = w;
            int flip_w;
            half = q8_smoothstep(half);
            render_back = ft >= Q8_HALF;
            if (ft < Q8_HALF) flip_w = lerp_i(old_w, 4, half);
            else              flip_w = lerp_i(4, old_w, half);
            if (flip_w < 4) flip_w = 4;
            x += (old_w - flip_w) / 2;
            w = flip_w;
        } else {
            render_back = 1;
        }
    }

    /* soft black shadow under the flying card */
    rect_fill(x+3, y+h-2, w, 4, IDX_BLACK);
    draw_card_sprite(card_id, x, y, w, h, render_back);
    if (flip_to_back && t >= Q8_FRAC(38,100) && t < Q8_FRAC(42,100)) rect_fill(x + w / 2 - 1, y + 2, 2, h - 4, IDX_WHITE);
    if (t > Q8_FRAC(78,100)) {
        rect_outline(x-2,y-2,w+4,h+4,IDX_GOLD_HI);
        if (((frame - start) & 3) < 2) rect_outline(x-4,y-4,w+8,h+8,IDX_WHITE);
    }
}

static int placement_scan_col(int frame, int start, int final_col)
{
    /* Scripted stand-in for left/right target movement. It sweeps then settles
       on the first available slot, matching the current auto-place rule. */
    int span = frame - start;
    if (span < 5) return 2;
    if (span < 10) return 1;
    return final_col;
}

/* ------------------------------------------------------------------------- */
/* Battle cut-in */

static int pseudo_rand(int x)
{
    x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x & 0x7fffffff;
}

static void draw_flash_rect(int x, int y, int w, int h, uint8_t c)
{
    rect_fill(x, y, w, h, c);
    rect_outline(x, y, w, h, IDX_WHITE);
}

static void draw_flames(int x, int y, int w, int h, int frame)
{
    for (int i = 0; i < 90; ++i) {
        int r = pseudo_rand(i*97 + frame*131);
        int px = x + (r % w);
        int py = y + h - ((r / 13) % h) - (frame % 9);
        int rad = 1 + ((r >> 6) % 5);
        uint8_t col = (i % 3 == 0) ? IDX_FLAME1 : ((i % 3 == 1) ? IDX_FLAME2 : IDX_FLAME3);
        for (int yy = -rad; yy <= rad; ++yy)
            for (int xx = -rad; xx <= rad; ++xx)
                if (xx*xx + yy*yy <= rad*rad) put_px(px+xx, py+yy, col);
    }
}


static void draw_disc(int cx, int cy, int r, uint8_t c);
static void draw_big_battle_card_burning(int id, int x, int y, int back, int burn_frame)
{
    const int w = 120, h = 160;
    if (burn_frame < 0) burn_frame = 0;

    /* The card must visibly disappear into flames, then stop. Earlier builds
       kept emitting random bursts after the card had already vanished, which
       read as an off-center explosion. This version only draws fire while the
       remaining visible card area exists. */
    const int vanish_frames = BATTLE_BURN_VANISH_FRAMES;
    if (burn_frame >= vanish_frames) return;

    draw_big_battle_card(id, x, y, back);
    int erase_h = (burn_frame * h) / vanish_frames;
    if (erase_h > h) erase_h = h;
    int edge = y + h - erase_h;
    if (erase_h > 0) rect_fill(x-2, edge, w+4, erase_h+4, IDX_BLACK);

    /* Flame edge: bottom-to-top, with multiple lobes centered over the card.
       This is not a generic radial explosion; it is a burn wipe. */
    int flame_band = 13 + (burn_frame % 6);
    for (int i = 0; i < 84; ++i) {
        int rnd = pseudo_rand(i * 173 + burn_frame * 271);
        int px = x + 5 + (rnd % (w - 10));
        int rise = (rnd >> 7) % flame_band;
        int py = edge - 2 - rise;
        int rr = 1 + ((rnd >> 12) & 3);
        uint8_t col = (i % 3 == 0) ? IDX_FLAME3 : ((i % 3 == 1) ? IDX_FLAME2 : IDX_FLAME1);
        draw_disc(px, py, rr, col);
    }

    /* White-hot fringe only while the card still has a visible edge. */
    if (burn_frame < vanish_frames - 12) {
        for (int x0 = x + 8; x0 < x + w - 8; x0 += 7) {
            int yy = edge + ((pseudo_rand(x0 * 31 + burn_frame * 19) & 3) - 1);
            put_px(x0, yy, IDX_WHITE);
            put_px(x0+1, yy, IDX_FLAME1);
        }
    }
}

static void draw_disc(int cx, int cy, int r, uint8_t c)
{
    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            if (xx*xx + yy*yy <= r*r) put_px(cx+xx, cy+yy, c);
        }
    }
}



/* The earlier reference-cropped slash overlay was removed completely.
   Combat now uses direct card-ram motion, white flash, centered damage,
   and the procedural burn wipe; no external slash asset is required. */

static int text_pixel_width(const char *s)
{
    int n = 0;
    for (; s && *s; ++s) if (*s != '\n') n += 8;
    return n;
}

static void draw_centered_damage_text_in_card(int card_x, int card_y, const char *damage_text)
{
    int tw = text_pixel_width(damage_text);
    int tx = card_x + (120 - tw) / 2;
    int ty = card_y + 63;
    if (tx < card_x + 4) tx = card_x + 4;
    draw_text(tx, ty, damage_text, IDX_GOLD_HI, IDX_BLACK);
}

static void draw_direct_attack_slash(int target_x, int target_y, int frame, int attacker_owner)
{
    int dir = (attacker_owner == 0) ? 1 : -1;
    int cx = target_x + 36;
    int cy = target_y + 58;
    int len = 28 + frame * 5;
    int x0, x1, y0, y1;
    if (len > 92) len = 92;
    x0 = cx - dir * (len / 2);
    y0 = cy - 38;
    x1 = cx + dir * (len / 2);
    y1 = cy + 34;
    uint8_t core = (frame & 2) ? IDX_WHITE : IDX_RED;

    for (int off = -3; off <= 3; ++off) {
        uint8_t c = (off == 0) ? core : ((abs(off) <= 2) ? IDX_RED : IDX_FLAME3);
        line_i(x0, y0 + off, x1, y1 + off, c);
        line_i(x0 - dir * 2, y0 + off, x1 - dir * 2, y1 + off, c);
    }
    if (frame >= 3) {
        line_i(cx - dir * 34, cy + 26, cx + dir * 28, cy - 18, IDX_RED);
        line_i(cx - dir * 33, cy + 27, cx + dir * 29, cy - 17, IDX_FLAME2);
        line_i(cx - dir * 30, cy + 28, cx + dir * 31, cy - 15, IDX_FLAME3);
    }
    if (frame >= 5) {
        int burst = frame - 5;
        draw_disc(cx + dir * 26, cy, 5 + (burst % 7), IDX_RED);
        draw_disc(cx + dir * 26, cy, 2 + (burst % 4), IDX_FLAME1);
        line_i(cx - dir * 18, cy, cx + dir * 46, cy - 18, IDX_FLAME2);
        line_i(cx - dir * 10, cy + 16, cx + dir * 50, cy + 2, IDX_FLAME3);
    }
}

static void draw_big_battle_card_hit_flash(int id, int x, int y, int back, int phase)
{
    /* Victim impact flash: draw the card first, then a checker/stripe white
       overlay for a few frames. */
    draw_big_battle_card(id, x, y, back);
    if (((phase / 2) & 1) == 0) {
        for (int yy = y + 5; yy < y + 119; ++yy) {
            for (int xx = x + 4; xx < x + 116; ++xx) {
                if (((xx + yy + phase * 3) & 3) != 0) put_px(xx, yy, IDX_WHITE);
            }
        }
    }
}

static void draw_cutin_battle_card(int id, int x, int y, int back, int attacker_card);

static int calc_battle_delta(int atk_id, int def_id, int defender_in_defense)
{
    int atk = (int)waifu_card_atk[atk_id];
    int defv = defender_in_defense ? (int)waifu_card_def[def_id] : (int)waifu_card_atk[def_id];
    return atk - defv;
}

static void apply_black_dither_fade(int32_t visible)
{
    visible = q8_clamp(visible, 0, Q8_ONE);
    static const uint8_t bayer[8][8] = {
        { 0,48,12,60, 3,51,15,63}, {32,16,44,28,35,19,47,31},
        { 8,56, 4,52,11,59, 7,55}, {40,24,36,20,43,27,39,23},
        { 2,50,14,62, 1,49,13,61}, {34,18,46,30,33,17,45,29},
        {10,58, 6,54, 9,57, 5,53}, {42,26,38,22,41,25,37,21}
    };
    int threshold = (int)(((Q8_ONE - visible) * 64 + Q8_HALF) >> Q8_SHIFT);
    if (threshold <= 0) return;
    if (threshold >= 64) { clear_screen(IDX_BLACK); return; }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (bayer[y & 7][x & 7] < threshold) framebuffer[y * W + x] = IDX_BLACK;
        }
    }
}

/* Generic cross-fade transition macro.  Draws `from_call` fading out for the
   first half, then `to_call` fading in.  Usage:
   SCREEN_TRANSITION(f, 24, draw_a(f), draw_b()) */
#define SCREEN_TRANSITION(f, half, from_call, to_call) \
    do { \
        if ((f) < (half)) { from_call; apply_black_dither_fade(Q8_ONE - q8_ratio((f), (half))); } \
        else { int _st_local = (f) - (half); (void)_st_local; to_call; apply_black_dither_fade(q8_ratio(_st_local, (half))); } \
    } while (0)

static void draw_field_pair_for_battle(Camera cam, int atk_col, int atk_row, int atk_id, int atk_back,
                                       int def_col, int def_row, int def_id, int def_back)
{
    render_board(cam);
    if (g_battle_late_frame >= 0) {
        /* Late battles must keep every already-summoned field card visible in
           the tactical prelude. v13 only drew the attacker/target pair, causing
           unrelated field cards to disappear briefly before the black cut-in. */
        draw_late_field_cards(cam, g_battle_late_frame);
    } else {
        draw_board_card(cam, atk_col, atk_row, atk_id, atk_back);
        draw_board_card(cam, def_col, def_row, def_id, def_back);
    }
    /* Only the active player/AI card gets the red tactical outline. The target
       does not keep a second rectangle in this PS1-like top-view staging. */
    draw_zone_cursor(cam, atk_col, atk_row);
    draw_hud();
}


typedef enum {
    BATTLE_DESTROY_DEFENDER = 0,
    BATTLE_DESTROY_ATTACKER = 1,
    BATTLE_DESTROY_BOTH = 2,
    BATTLE_DIRECT_ATTACK = 3,
    BATTLE_NO_DESTROY = 4
} BattleOutcome;

static void draw_battle_cutin_event_ex(int f, int start,
                                       int atk_id, int def_id,
                                       int atk_col, int atk_row, int atk_back,
                                       int def_col, int def_row, int def_back,
                                       const char *damage_text,
                                       BattleOutcome outcome)
{
    clear_screen(IDX_BLACK);
    int local0 = f - start;

    /* Battle begins in tactical top view. Only the active card is outlined; no
       hand-selection arrow is shown during placement/top mode. Face-down cards
       are not revealed on the 3D field here. */
    if (local0 < 16) {
        Camera cam = side_battle_camera(atk_row);
        draw_field_pair_for_battle(cam, atk_col, atk_row, atk_id, atk_back,
                                        def_col, def_row, def_id, def_back);
        return;
    }

    int local = local0 - 16;
    const int ax = 4, ay = 33, dx = 132, dy = 33;
    const int slide_dur = 18;
    const int atk_flip_start = slide_dur;
    const int flip_dur = 48;
    const int def_flip_start = atk_flip_start + (atk_back ? flip_dur : 0);
    int reveal_end = def_flip_start + (def_back ? flip_dur : 0);
    int pause_after_reveal = 10;
    int ram_start = reveal_end + pause_after_reveal;
    int ram_dur = 34;
    int ram_contact = ram_start + 22;
    int counter_start = ram_start + ram_dur + 16;
    int counter_dur = 34;
    int hit_hold_start = (outcome == BATTLE_DESTROY_ATTACKER) ? (counter_start + 20) : ram_contact;
    int burn_start = (outcome == BATTLE_DESTROY_ATTACKER) ? (counter_start + counter_dur + 18)
                                                          : (ram_start + ram_dur + 30);
    if (outcome == BATTLE_DESTROY_BOTH) burn_start = ram_start + ram_dur + 22;
    int burn_dur = BATTLE_BURN_DUR;

    if (local < slide_dur) {
        int32_t e = q8_smooth_ratio(local, slide_dur);
        int ax0 = lerp_i(-128, ax, e);
        int dx0 = lerp_i(264, dx, e);
        draw_cutin_battle_card(atk_id, ax0, ay, atk_back, 1);
        draw_cutin_battle_card(def_id, dx0, dy, def_back, 0);
    } else if (atk_back && local < def_flip_start) {
        draw_big_battle_card_flip(atk_id, ax, ay, local - atk_flip_start, flip_dur);
        draw_cutin_battle_card(def_id, dx, dy, def_back, 0);
    } else if (def_back && local < reveal_end) {
        draw_cutin_battle_card(atk_id, ax, ay, 0, 1);
        draw_big_battle_card_flip(def_id, dx, dy, local - def_flip_start, flip_dur);
    } else if (local < ram_start) {
        draw_cutin_battle_card(atk_id, ax, ay, 0, 1);
        draw_cutin_battle_card(def_id, dx, dy, 0, 0);
    } else if (local < burn_start) {
        int atk_x = ax;
        int def_x = dx;
        int flash_attacker = 0;
        int flash_defender = 0;
        int shake_attacker = 0;
        int shake_defender = 0;

        if (local < ram_start + ram_dur) {
            /* Attacker rams the defender. The card moves right, contacts, then
               eases back. No slash overlay is used. */
            int32_t t = q8_ratio(local - ram_start, ram_dur);
            int32_t lunge;
            if (t < Q8_FRAC(62,100)) lunge = q8_smoothstep(q8_div(t, Q8_FRAC(62,100)));
            else lunge = Q8_ONE - q8_mul(q8_smoothstep(q8_div(t - Q8_FRAC(62,100), Q8_FRAC(38,100))), Q8_FRAC(72,100));
            atk_x = ax + q8_to_int(q8_mul(Q8_FROM_INT(46), lunge));
            if (local >= ram_contact && local < ram_contact + 24) {
                flash_defender = 1;
                /* v15: if the attacking card is the stronger card, the opposing
                   card must not appear to lunge/counterattack. It stays locked
                   at its battle-card position and only flashes before burning.
                   Counter motion is reserved for BATTLE_DESTROY_ATTACKER below. */
                shake_defender = 0;
            }
        } else if (outcome == BATTLE_DESTROY_ATTACKER && local < counter_start) {
            /* Stronger defender absorbs it: return to stillness before counter. */
        } else if (outcome == BATTLE_DESTROY_ATTACKER && local >= counter_start && local < counter_start + counter_dur) {
            /* Defender counter-rams only when the defender is actually stronger.
               v15 missed the outcome guard here, so in the attacker-wins case
               the target briefly lunged back before burning. */
            int32_t t = q8_ratio(local - counter_start, counter_dur);
            int32_t lunge;
            if (t < Q8_FRAC(62,100)) lunge = q8_smoothstep(q8_div(t, Q8_FRAC(62,100)));
            else lunge = Q8_ONE - q8_mul(q8_smoothstep(q8_div(t - Q8_FRAC(62,100), Q8_FRAC(38,100))), Q8_FRAC(68,100));
            def_x = dx - q8_to_int(q8_mul(Q8_FROM_INT(46), lunge));
            if (local >= counter_start + 20) {
                flash_attacker = 1;
                int q = local - (counter_start + 20);
                shake_attacker = ((q & 1) ? -2 : 2) + ((q & 4) ? 1 : 0);
            }
        } else if (outcome == BATTLE_DESTROY_ATTACKER && local >= counter_start + counter_dur) {
            flash_attacker = 1;
            int q = local - (counter_start + counter_dur);
            shake_attacker = ((q & 1) ? -2 : 2);
        }

        if (outcome == BATTLE_DESTROY_BOTH && local >= ram_contact && local < burn_start) {
            flash_attacker = 1;
            flash_defender = 1;
            shake_attacker = ((local & 1) ? -2 : 2);
            shake_defender = ((local & 1) ? 2 : -2);
        }

        if (flash_attacker) draw_big_battle_card_hit_flash(atk_id, atk_x + shake_attacker, ay, 0, local);
        else draw_cutin_battle_card(atk_id, atk_x, ay, 0, 1);

        if (flash_defender) draw_big_battle_card_hit_flash(def_id, def_x + shake_defender, dy, 0, local);
        else draw_cutin_battle_card(def_id, def_x, dy, 0, 0);

        if (outcome == BATTLE_DESTROY_DEFENDER && local >= hit_hold_start && local < burn_start) {
            draw_centered_damage_text_in_card(def_x + shake_defender, dy, damage_text);
        } else if (outcome == BATTLE_DESTROY_ATTACKER && local >= hit_hold_start && local < burn_start) {
            draw_centered_damage_text_in_card(atk_x + shake_attacker, ay, damage_text);
        } else if (outcome == BATTLE_DESTROY_BOTH && local >= hit_hold_start && local < burn_start) {
            draw_centered_damage_text_in_card((ax + dx) / 2, dy, "0");
        }
    } else if (local < burn_start + burn_dur) {
        int burn = local - burn_start;
        if (outcome == BATTLE_DESTROY_DEFENDER) {
            draw_cutin_battle_card(atk_id, ax, ay, 0, 1);
            draw_big_battle_card_burning(def_id, dx, dy, 0, burn);
        } else if (outcome == BATTLE_DESTROY_ATTACKER) {
            draw_big_battle_card_burning(atk_id, ax, ay, 0, burn);
            draw_cutin_battle_card(def_id, dx, dy, 0, 0);
        } else if (outcome == BATTLE_DESTROY_BOTH) {
            draw_big_battle_card_burning(atk_id, ax, ay, 0, burn);
            draw_big_battle_card_burning(def_id, dx, dy, 0, burn);
        } else {
            draw_cutin_battle_card(atk_id, ax, ay, 0, 1);
            draw_cutin_battle_card(def_id, dx, dy, 0, 0);
        }
    } else {
        if (outcome != BATTLE_DESTROY_ATTACKER && outcome != BATTLE_DESTROY_BOTH) draw_cutin_battle_card(atk_id, ax, ay, 0, 1);
        if (outcome != BATTLE_DESTROY_DEFENDER && outcome != BATTLE_DESTROY_BOTH) draw_cutin_battle_card(def_id, dx, dy, 0, 0);
    }

    rect_fill(0, 2, 128, 22, IDX_BLACK);
    {
        const char *name = is_monster_card(atk_id) ? waifu_card_names[atk_id] : (is_support_card(atk_id) ? support_card_name(atk_id) : "???");
        draw_wrapped_text_small(4, 4, name, 19, IDX_WHITE, IDX_BLACK);
    }
    rect_fill(118, 198, 138, 42, IDX_BLACK);
    {
        const char *name = is_monster_card(def_id) ? waifu_card_names[def_id] : (is_support_card(def_id) ? support_card_name(def_id) : "???");
        draw_wrapped_text_small(122, 199, name, 20, IDX_WHITE, IDX_BLACK);
    }
}

static void draw_battle_cutin_event(int f, int start,
                                    int atk_id, int def_id,
                                    int atk_col, int atk_row, int atk_back,
                                    int def_col, int def_row, int def_back,
                                    const char *damage_text)
{
    draw_battle_cutin_event_ex(f, start, atk_id, def_id, atk_col, atk_row, atk_back,
                               def_col, def_row, def_back, damage_text,
                               BATTLE_DESTROY_DEFENDER);
}

static void draw_battle_cutin(int f)
{
    /* COM attacks a stronger face-down defender. The defender only reveals
       inside the battle cut-in, survives, then counter-rams and destroys the
       attacker. Damage uses ATK-vs-DEF: 1500 - 2100 = -600. */
    int delta = calc_battle_delta(enemy_summon_id, player_summon_id, 1);
    char dmg[16];
    snprintf(dmg, sizeof(dmg), "%d", delta);
    draw_battle_cutin_event_ex(f, 554,
        enemy_summon_id, player_summon_id,
        ENEMY_CARD_COL, ENEMY_CARD_ROW, 0,
        PLAYER_CARD_COL, PLAYER_CARD_ROW, 1,
        dmg, BATTLE_DESTROY_ATTACKER);
}

/* ------------------------------------------------------------------------- */

static void set_lps_for_frame(int f)
{
    g_you_lp = 8000;
    g_com_lp = 8000;

    /* First battle is now a proper damage calculation case:
       COM ATK 1500 attacks Albathia DEF 2100, fails, and takes 600. */
    if (f >= 900) g_com_lp = 7400;

    if (f >= 940) {
        int vf = f - 550;
        if (vf >= 800)  g_com_lp = 5900;  /* 3000 - 1500 */
        if (vf >= 1215) g_you_lp = 5600;  /* later COM attack connects */
        if (vf >= 1705) g_com_lp = 0;
    }
    if (g_force_lp_loss_demo && f >= 2645) {
        g_you_lp = 0;
        g_com_lp = 5600;
    }
}

static void draw_late_field_cards(Camera cam, int f)
{
    /* The face-down card survived the opening battle, so it is now face-up on
       the 3D field. This matches FM: reveal during battle, then keep revealed
       only if it survives. */
    if (f < 1705) draw_board_card(cam, PLAYER_CARD_COL, PLAYER_CARD_ROW, player_summon_id, 0);
    if (f < 800) draw_board_card(cam, ENEMY_CARD_COL, ENEMY_CARD_ROW, enemy_summon_id, 0);
    if (f >= 505 && f < 1215) draw_board_card(cam, PLAYER_CARD2_COL, PLAYER_CARD_ROW, 37, 0);
    if (f >= 920 && f < 1705) draw_board_card(cam, ENEMY_CARD2_COL, ENEMY_CARD_ROW, 28, 0);
    if (f >= 1385) draw_board_card(cam, PLAYER_CARD3_COL, PLAYER_CARD_ROW, player_summon_id, 0);
}

static void draw_text_scaled(int x, int y, const char *s, int scale, uint8_t fg, uint8_t shadow)
{
    int cx = x;
    for (; s && *s; ++s, cx += 8 * scale) {
        unsigned char ch = (unsigned char)*s;
        const uint8_t *charfont = n2DLib_font + ((uint32_t)ch * 8u);
        for (int yy = 0; yy < 8; ++yy) {
            uint8_t row = charfont[yy];
            for (int xx = 0; xx < 8; ++xx) {
                if (row & (uint8_t)(1u << (7 - xx))) {
                    rect_fill(cx + xx * scale + 2, y + yy * scale + 2, scale, scale, shadow);
                    rect_fill(cx + xx * scale, y + yy * scale, scale, scale, fg);
                    if (scale >= 3 && ((xx + yy) & 3) == 0) put_px(cx + xx * scale, y + yy * scale, IDX_WHITE);
                }
            }
        }
    }
}

static void draw_centered_text_scaled(int y, const char *s, int scale, uint8_t fg, uint8_t shadow);
static void draw_title_background(void);
static void draw_title_logo(void);
static void draw_title_prompt(int f);
static int story_save_exists(void);

static void draw_result_screen(int f, const char *msg)
{
    int local = f - 1750;
    if (local < 0) local = 0;
    int32_t t = q8_smooth_ratio(local, 90);
    Camera cam = lerp_camera(battle_top_camera(), player_camera(), t);
    render_board(cam);
    draw_late_field_cards(cam, f);

    /* Reference-style finish: the UI leaves the screen first, then only the
       3D field and surviving cards remain while the large result text appears. */
    if (local < 70) {
        int32_t e = q8_smooth_ratio(local, 70);
        draw_hud_offset(-q8_to_int(q8_mul(Q8_FROM_INT(76), e)), 0, q8_to_int(q8_mul(Q8_FROM_INT(92), e)), 0);
        draw_bottom_info_offset(player_summon_id, "WIN", q8_to_int(q8_mul(Q8_FROM_INT(44), e)));
    }

    if (local >= 50) {
        int32_t e = q8_smooth_ratio(local - 50, 42);
        int scale = (local < 92) ? 2 + (e > Q8_FRAC(55,100) ? 1 : 0) : 3;
        int tw = (int)strlen(msg) * 8 * scale;
        int x = (W - tw) / 2;
        int y = 100 - q8_to_int(q8_mul(Q8_FROM_INT(10), Q8_ONE - e));
        if (((local / 6) & 1) == 0) draw_text_scaled(x + 1, y + 1, msg, scale, IDX_WHITE, IDX_BLACK);
        draw_text_scaled(x, y, msg, scale, IDX_GOLD_HI, IDX_BLACK);
    }
}

static int battle_result_from_state(int you_lp, int com_lp, int you_deck_left, int com_deck_left)
{
    /* Real battle-mode loss/win priority for the eventual interactive game.
       Deck-out is included here now: drawing with zero cards loses. */
    if (you_deck_left <= 0) return -1; /* YOU LOSE: deck-out */
    if (com_deck_left <= 0) return 1;
    if (you_lp <= 0) return -1;
    if (com_lp <= 0) return 1;
    return 0;
}

static char battle_rank_from_stats(int won, int lp_left, int cards_used, int turns, int *score_out)
{
    int score = 0;
    if (won) score += 500;
    score += lp_left / 20;
    score += (40 - cards_used) * 7;
    score -= turns * 18;
    if (score < 0) score = 0;
    if (score_out) *score_out = score;
    if (!won) return 'D';
    if (score >= 950) return 'S';
    if (score >= 780) return 'A';
    if (score >= 620) return 'B';
    if (score >= 470) return 'C';
    return 'D';
}

static void draw_tally_screen(int f, int won)
{
    (void)f;
    clear_screen(IDX_BLACK);
    draw_panel_rect(31, 24, 194, 178, IDX_UI_DARK);
    draw_centered_text_scaled(37, won ? "DUEL VICTORY" : "DUEL DEFEAT", 1, won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    hline(45, 210, 56, IDX_UI_LIGHT);

    int cards_used = 4;
    int turns = 3;
    int lp_left = won ? 5600 : (g_force_lp_loss_demo ? 0 : 1200);
    int deck_left = won ? 36 : (g_force_deckout_demo ? 0 : 30);
    int score = 0;
    char rank = battle_rank_from_stats(won, lp_left, cards_used, turns, &score);
    char line[64];

    draw_text(55, 74, "RESULT", IDX_WHITE, IDX_BLACK);
    draw_text(150, 74, won ? "WIN" : "LOSE", won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", lp_left);
    draw_text(55, 94, "LP LEFT", IDX_WHITE, IDX_BLACK);
    draw_text(160, 94, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", cards_used);
    draw_text(55, 112, "CARDS USED", IDX_WHITE, IDX_BLACK);
    draw_text(176, 112, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", deck_left);
    draw_text(55, 130, "DECK LEFT", IDX_WHITE, IDX_BLACK);
    draw_text(176, 130, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", score);
    draw_text(55, 148, "SCORE", IDX_WHITE, IDX_BLACK);
    draw_text(152, 148, line, IDX_GOLD_HI, IDX_BLACK);

    snprintf(line, sizeof(line), "RANK %c", rank);
    draw_centered_text_scaled(170, line, 2, IDX_GOLD_HI, IDX_BLACK);
    draw_text_small(50, 210, "PRESS RUN: RETURN TO TITLE", IDX_WHITE, IDX_BLACK);
}

static void draw_victory_screen(int f)
{
    int local = f - 1750;
    if (local < 0) local = 0;
    /* The scripted Battle Mode branch wins, but the result selection goes
       through the same condition function that handles deck-out loss. */
    int result = g_force_lp_loss_demo ? battle_result_from_state(0, 5600, 30, 20) : (g_force_deckout_demo ? battle_result_from_state(g_you_lp, g_com_lp, 0, 20) : battle_result_from_state(g_you_lp, g_com_lp, 36, 20));
    const char *result_text = (result < 0) ? "YOU LOSE" : "YOU WIN";

    if (local < 150) {
        draw_result_screen(f, result_text);
    } else if (local < 205) {
        draw_result_screen(f, result_text);
        apply_black_dither_fade(Q8_ONE - q8_ratio(local - 150, 55));
    } else if (local < 475) {
        draw_tally_screen(f, result >= 0);
        if (local < 245) apply_black_dither_fade(q8_ratio(local - 205, 40));
    } else if (local < 535) {
        draw_tally_screen(f, result >= 0);
        apply_black_dither_fade(Q8_ONE - q8_ratio(local - 475, 60));
    } else {
        int rf = local - 535;
        draw_title_background();
        draw_title_logo();
        draw_title_prompt(rf);
        if (rf < 45) apply_black_dither_fade(q8_ratio(rf, 45));
    }
}


static void render_duel_frame(int f);
static void render_duel_script_frame(int f);

static int text_px_width(const char *s, int scale)
{
    return s ? (int)strlen(s) * 8 * scale : 0;
}

static void draw_centered_text(int y, const char *s, uint8_t fg, uint8_t shadow)
{
    int x = (W - ((int)strlen(s) * 8)) / 2;
    draw_text(x, y, s, fg, shadow);
}

static void draw_centered_text_scaled(int y, const char *s, int scale, uint8_t fg, uint8_t shadow)
{
    int x = (W - text_px_width(s, scale)) / 2;
    draw_text_scaled(x, y, s, scale, fg, shadow);
}

static void draw_title_background(void)
{
    /* v18: no title/prompt blue bars. The image is shown cleanly; only the
       menu itself uses a blue panel. */
    draw_card_raw(title_screen_img, TITLE_SCREEN_W, TITLE_SCREEN_H, 0, 0, W, H);
}

static void draw_title_logo(void)
{
    draw_centered_text_scaled(20, "SHATTERED", 2, IDX_GOLD_HI, IDX_BLACK);
    draw_centered_text_scaled(38, "DECKS", 2, IDX_WHITE, IDX_BLACK);
}

static void draw_title_prompt(int f)
{
    if (((f / 24) & 1) == 0) {
        draw_centered_text(190, story_save_exists() ? "RUN START   B LOAD" : "PUSH RUN TO START", IDX_WHITE, IDX_BLACK);
    }
}

static void draw_menu_screen(int selected)
{
    int has_save = story_save_exists();
    const char *help = "RANDOM DECK / FREE DUEL";
    draw_title_background();
    draw_title_logo();
    rect_fill(39, 124, 178, 75, IDX_BLACK);
    draw_panel_rect(41, 126, 174, 71, IDX_UI_DARK);
    draw_text(72, 139, "STORY MODE", selected == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(72, 159, "BATTLE MODE", selected == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(72, 179, "LOAD STORY", selected == 2 ? (has_save ? IDX_GOLD_HI : IDX_DIM) : (has_save ? IDX_WHITE : IDX_DIM), IDX_BLACK);
    int ay = selected == 0 ? 143 : (selected == 1 ? 163 : 183);
    for (int r = 0; r < 7; ++r) hline(54, 54+r, ay-3+r, IDX_RED);
    if (selected == 0) help = "ENTER NAME / FIRST DREAM";
    else if (selected == 2) help = has_save ? "RESUME SAVED STORY" : "NO SAVE FILE FOUND";
    draw_text_small(55, 207, help, has_save || selected != 2 ? IDX_WHITE : IDX_RED, IDX_BLACK);
}

static void render_title_sequence(int f)
{
    clear_screen(IDX_BLACK);
    if (f < 168) {
        draw_title_background();
        draw_title_logo();
        draw_title_prompt(f);
        if (f < 30) apply_black_dither_fade(q8_ratio(f, 30));
        else if (f >= 138) apply_black_dither_fade(Q8_ONE - q8_ratio(f - 138, 30));
    } else if (f < 245) {
        int sel = (f < 220) ? 0 : 1;
        draw_menu_screen(sel);
        if (f < 198) apply_black_dither_fade(q8_ratio(f - 168, 30));
    } else if (f < 285) {
        draw_menu_screen(1);
        apply_black_dither_fade(Q8_ONE - q8_ratio(f - 245, 40));
    } else {
        clear_screen(IDX_BLACK);
    }
}


static int card_star_count(int id)
{
    int power = ((int)waifu_card_atk[id] + (int)waifu_card_def[id]) / 2;
    int stars = 2 + power / 500;
    if (stars < 1) stars = 1;
    if (stars > 8) stars = 8;
    return stars;
}

static void draw_stars_line(int x, int y, int stars)
{
    draw_text_small(x, y, "STARS", IDX_WHITE, IDX_BLACK);
    int sx = x + 38;
    for (int i = 0; i < stars; ++i) {
        /* tiny PS1-readable star diamond */
        put_px(sx + i*7 + 3, y + 0, IDX_GOLD_HI);
        hline(sx + i*7 + 2, sx + i*7 + 4, y + 1, IDX_GOLD_HI);
        hline(sx + i*7 + 1, sx + i*7 + 5, y + 2, IDX_GOLD_HI);
        hline(sx + i*7 + 2, sx + i*7 + 4, y + 3, IDX_GOLD_HI);
        put_px(sx + i*7 + 3, y + 4, IDX_GOLD_HI);
    }
}

static void draw_card_preview_screen(int f)
{
    int local = f - DUEL_OPENING_END;
    clear_screen(IDX_BLACK);
    int id = hand_ids[0];
    int32_t in_t = q8_smooth_ratio(local, 24);
    int32_t out_t = local > 96 ? q8_smooth_ratio(local - 96, 24) : 0;
    int32_t vis = q8_mul(in_t, Q8_ONE - out_t);
    if (vis <= 0) return;

    int card_x = lerp_i(-126, 9, vis);
    int card_y = 36;
    draw_big_battle_card(id, card_x, card_y, 0);

    int tx = 136;
    int y = 24;
    draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
    draw_wrapped_text_small(tx, y, waifu_card_names[id], 20, IDX_WHITE, IDX_BLACK); y += 24;
    draw_stars_line(tx, y, card_star_count(id)); y += 13;

    char line[64];
    snprintf(line, sizeof(line), "%s / %s", waifu_card_attr[id], waifu_card_tribe[id]);
    draw_wrapped_text_small(tx, y, line, 20, IDX_WHITE, IDX_BLACK); y += 20;

    draw_text_small(tx, y, "LORE", IDX_GOLD_HI, IDX_BLACK); y += 11;
    draw_wrapped_text_small(tx, y, "A wandering duel maiden", 20, IDX_WHITE, IDX_BLACK); y += 18;
    snprintf(line, sizeof(line), "of %s power. Her", waifu_card_attr[id]);
    draw_wrapped_text_small(tx, y, line, 20, IDX_WHITE, IDX_BLACK); y += 18;
    snprintf(line, sizeof(line), "%s style breaks", waifu_card_tribe[id]);
    draw_wrapped_text_small(tx, y, line, 20, IDX_WHITE, IDX_BLACK); y += 18;
    draw_wrapped_text_small(tx, y, "weak field lines.", 20, IDX_WHITE, IDX_BLACK); y += 18;

    snprintf(line, sizeof(line), "ATK %u", (unsigned)waifu_card_atk[id]);
    draw_text_small(tx, 197, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "DEF %u", (unsigned)waifu_card_def[id]);
    draw_text_small(tx, 209, line, IDX_GOLD_HI, IDX_BLACK);
    if (((local / 16) & 1) == 0) draw_text_small(74, 223, "B: BACK", IDX_WHITE, IDX_BLACK);

    if (local < 24) apply_black_dither_fade(vis);
    else if (local > 96) apply_black_dither_fade(Q8_ONE - out_t);
}

static void render_duel_opening_frame(int f)
{
    clear_screen(IDX_BLACK);
    if (f < 16) return;
    int lf = f - 16;
    int32_t ft = q8_ratio(lf, DUEL_OPENING_END - 16);
    Camera cam = opening_camera(18 + q8_to_int(q8_mul(Q8_FROM_INT(66), q8_smoothstep(ft))));
    render_board(cam);
    /* Longer fade-in: the field slowly resolves out of black before the hand UI. */
    apply_black_dither_fade(q8_smoothstep(ft));
    if (f > 40) draw_hud();
}

static void render_duel_frame(int f)
{
    if (f < DUEL_OPENING_END) {
        render_duel_opening_frame(f);
        return;
    }
    if (f < DUEL_PREVIEW_END) {
        draw_card_preview_screen(f);
        return;
    }
    render_duel_script_frame(f - DUEL_SCRIPT_OFFSET);
}

static void render_frame(int f)
{
    if (f < TITLE_SEQUENCE_FRAMES) {
        render_title_sequence(f);
        return;
    }
    render_duel_frame(f - TITLE_SEQUENCE_FRAMES);
}

static void render_late_match_frame(int f)
{
    Camera cam = player_camera();
    int show_hand = 0, show_enemy_hand = 0, selected = -1;
    int player_fly = 0, enemy_fly = 0;
    int player_hand_offset = 0, enemy_hand_offset = 0;
    int draw_sequence = 0;
    int enemy_draw_sequence = 0;

    if (f < 430) {
        cam = turn_camera(f, 390, 430, 0);
    } else if (f < 475) {
        cam = player_camera(); show_hand = 1; selected = 4;
    } else if (f < 505) {
        cam = placement_camera(); show_hand = 1; selected = 4; player_fly = 1;
        player_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 475, 30)));
    } else if (f < 525) {
        cam = lerp_camera(placement_camera(), battle_top_camera(), q8_ratio(f - 505, 20));
    } else if (f < 545) {
        cam = battle_top_camera();
    } else if (f < 720) {
        g_battle_late_frame = f;
        draw_battle_cutin_event(f, 545,
            37, enemy_summon_id,
            PLAYER_CARD2_COL, PLAYER_CARD_ROW, 0,
            ENEMY_CARD_COL, ENEMY_CARD_ROW, 0,
            "-1500");
        return;
    } else if (f < 845) {
        /* After the cut-in, restore the tactical board view and keep the last
           attacking card selected before passing the turn. */
        cam = battle_top_camera();
    } else if (f < 890) {
        cam = turn_camera(f, 845, 890, 1);
    } else if (f < 920) {
        cam = enemy_placement_camera(); show_enemy_hand = 1; selected = 3; enemy_fly = 1; enemy_draw_sequence = 1;
        enemy_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 890, 30)));
    } else if (f < 940) {
        cam = lerp_camera(enemy_placement_camera(), enemy_battle_top_camera(), q8_ratio(f - 920, 20));
    } else if (f < 960) {
        cam = enemy_battle_top_camera();
    } else if (f < 1135) {
        g_battle_late_frame = f;
        draw_battle_cutin_event(f, 960,
            28, 37,
            ENEMY_CARD2_COL, ENEMY_CARD_ROW, 0,
            PLAYER_CARD2_COL, PLAYER_CARD_ROW, 0,
            "-2200");
        return;
    } else if (f < 1260) {
        cam = enemy_battle_top_camera();
    } else if (f < 1305) {
        cam = turn_camera(f, 1260, 1305, 0);
    } else if (f < 1385) {
        cam = player_camera(); selected = 0; draw_sequence = 1;
    } else if (f < 1415) {
        cam = placement_camera(); show_hand = 1; selected = 0; player_fly = 2;
        player_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 1385, 30)));
    } else if (f < 1450) {
        cam = lerp_camera(placement_camera(), battle_top_camera(), q8_ratio(f - 1415, 35));
    } else if (f < 1625) {
        g_battle_late_frame = f;
        draw_battle_cutin_event(f, 1450,
            player_summon_id, 28,
            PLAYER_CARD3_COL, PLAYER_CARD_ROW, 0,
            ENEMY_CARD2_COL, ENEMY_CARD_ROW, 0,
            "-6500");
        return;
    } else if (f < 1750) {
        cam = battle_top_camera();
    } else {
        draw_victory_screen(f);
        return;
    }

    render_board(cam);
    draw_late_field_cards(cam, f);

    if (f >= 505 && f < 545) {
        draw_zone_cursor(cam, PLAYER_CARD2_COL, PLAYER_CARD_ROW);
    }
    if (f >= 800 && f < 845) {
        draw_zone_cursor(cam, PLAYER_CARD2_COL, PLAYER_CARD_ROW);
    }
    if (f >= 920 && f < 960) {
        draw_zone_cursor(cam, ENEMY_CARD2_COL, ENEMY_CARD_ROW);
    }
    if (f >= 1215 && f < 1260) {
        draw_zone_cursor(cam, ENEMY_CARD2_COL, ENEMY_CARD_ROW);
    }
    if (f >= 1415 && f < 1450) {
        draw_zone_cursor(cam, PLAYER_CARD3_COL, PLAYER_CARD_ROW);
    }
    if (f >= 1705 && f < 1750) {
        draw_zone_cursor(cam, PLAYER_CARD3_COL, PLAYER_CARD_ROW);
    }

    if (player_fly == 1) draw_flying_card(cam, 37, 4, PLAYER_CARD2_COL, PLAYER_CARD_ROW, f, 475, 505, 0);
    if (enemy_fly) draw_flying_card(cam, 28, 3, ENEMY_CARD2_COL, ENEMY_CARD_ROW, f, 890, 920, 0);
    if (player_fly == 2) draw_flying_card(cam, player_summon_id, 0, PLAYER_CARD3_COL, PLAYER_CARD_ROW, f, 1385, 1415, 0);

    draw_hud();
    g_player_hand_offset_y = player_hand_offset;
    g_enemy_hand_offset_y = enemy_hand_offset;
    g_suppress_hand_cursor = (player_hand_offset > 0 || enemy_hand_offset > 0);
    g_player_hide_index = (player_fly == 1) ? 4 : ((player_fly == 2) ? 0 : -1);
    g_enemy_hide_index = enemy_fly ? 3 : -1;
    if (draw_sequence) draw_player_hand_draw_sequence(f, 1305, selected);
    else if (show_hand) draw_player_hand(f, selected);
    if (enemy_draw_sequence) draw_enemy_hand_draw_sequence(f, 865, selected);
    else if (show_enemy_hand) draw_enemy_hand(f, selected, 0);
    g_player_hand_offset_y = 0;
    g_enemy_hand_offset_y = 0;
    g_suppress_hand_cursor = 0;
    g_player_hide_index = -1;
    g_enemy_hide_index = -1;

    if (f >= 390 && f < 800) draw_bottom_info(37, "TURN");
    else if (f >= 845 && f < 1215) draw_bottom_info(28, "COM");
    else draw_bottom_info(player_summon_id, "FINAL");

    if (f >= 800 && f < 845) apply_black_dither_fade(q8_ratio(f - 800, 45));
    if (f >= 1215 && f < 1260) apply_black_dither_fade(q8_ratio(f - 1215, 45));
    if (f >= 1705 && f < 1750) apply_black_dither_fade(q8_ratio(f - 1705, 45));
}

static void render_duel_script_frame(int f)
{
    g_battle_late_frame = -1;
    set_lps_for_frame(f);
    clear_screen(IDX_BLACK);

    /* First duel beat is deliberately slower in v5. It now demonstrates:
       hand -> UP to top/spell-trap field -> DOWN back to hand -> UP/placement,
       then a slower opponent selection and placement before battle. */
    if (f >= 554 && f < 900) { draw_battle_cutin(f); return; }
    if (f >= 900 && f < 940) {
        Camera cam = enemy_battle_top_camera();
        render_board(cam);
        draw_board_card(cam, PLAYER_CARD_COL, PLAYER_CARD_ROW, player_summon_id, 0);
        draw_zone_cursor(cam, PLAYER_CARD_COL, PLAYER_CARD_ROW);
        draw_hud();
        draw_bottom_info(player_summon_id, "REVEALED");
        apply_black_dither_fade(q8_ratio(f - 900, 40));
        return;
    }
    if (f >= 940) { render_late_match_frame(f - 550); return; }

    Camera cam = player_camera();
    int show_hand = 0, show_enemy_hand = 0;
    int selected = 0;
    int show_player_card = 0, show_enemy_card = 0;
    int top_mode = 0;
    int player_hand_offset = 0, enemy_hand_offset = 0;
    int support_demo_cursor = 0;
    int enemy_draw_sequence = 0;

    if (f < 18) {
        if (f > 8) draw_hud();
        return;
    } else if (f < 84) {
        cam = opening_camera(f);
    } else if (f < 132) {
        cam = player_camera(); show_hand = 1; selected = 0;
    } else if (f < 154) {
        /* UP from hand: smooth camera lift into tactical top view. */
        cam = lerp_camera(player_camera(), top_camera(), q8_ratio(f - 132, 22));
        show_hand = 1; selected = -1; top_mode = 1; support_demo_cursor = 1; player_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 132, 22)));
    } else if (f < 176) {
        cam = top_camera(); show_hand = 1; selected = -1; top_mode = 1; support_demo_cursor = 1; player_hand_offset = 92;
    } else if (f < 198) {
        /* DOWN from spell/trap/top field: smooth return to hand. */
        cam = lerp_camera(top_camera(), player_camera(), q8_ratio(f - 176, 22));
        show_hand = 1; selected = -1; top_mode = 1; support_demo_cursor = 1; player_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), Q8_ONE - q8_smooth_ratio(f - 176, 22)));
    } else if (f < 220) {
        cam = player_camera(); show_hand = 1; selected = 0;
    } else if (f < 248) {
        /* Second UP: move to the monster placement target. */
        cam = lerp_camera(player_camera(), placement_camera(), q8_ratio(f - 220, 28));
        show_hand = 1; selected = -1; top_mode = 1; player_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 220, 28)));
    } else if (f < 286) {
        cam = placement_camera(); show_hand = 1; selected = -1; top_mode = 1; player_hand_offset = 92;
    } else if (f < 316) {
        /* After placement, switch into tactical top view instead of snapping
           straight back to the hand camera. */
        cam = lerp_camera(placement_camera(), battle_top_camera(), q8_ratio(f - 286, 30));
        show_hand = 1; selected = -1; show_player_card = 1; top_mode = 1; player_hand_offset = 92;
    } else if (f < 344) {
        cam = battle_top_camera(); show_player_card = 1; top_mode = 1;
    } else if (f < 392) {
        cam = turn_camera(f, 344, 392, 1); show_player_card = 1;
    } else if (f < 408) {
        cam = enemy_camera(); show_enemy_hand = 1; selected = -1; show_player_card = 1; enemy_draw_sequence = 1;
    } else if (f < 420) {
        cam = enemy_camera(); show_enemy_hand = 1; selected = 0; show_player_card = 1; enemy_draw_sequence = 1;
    } else if (f < 432) {
        cam = enemy_camera(); show_enemy_hand = 1; selected = 1; show_player_card = 1; enemy_draw_sequence = 1;
    } else if (f < 446) {
        cam = enemy_camera(); show_enemy_hand = 1; selected = 2; show_player_card = 1; enemy_draw_sequence = 1;
    } else if (f < 472) {
        cam = lerp_camera(enemy_camera(), enemy_placement_camera(), q8_ratio(f - 446, 26));
        show_enemy_hand = 1; selected = -1; show_player_card = 1; enemy_hand_offset = q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(f - 446, 26)));
    } else if (f < 506) {
        cam = enemy_placement_camera(); show_enemy_hand = 1; selected = -1; show_player_card = 1; enemy_hand_offset = 92;
    } else if (f < 536) {
        cam = lerp_camera(enemy_placement_camera(), enemy_battle_top_camera(), q8_ratio(f - 506, 30));
        show_player_card = 1; show_enemy_card = 1; top_mode = 1;
    } else if (f < 554) {
        cam = enemy_battle_top_camera(); show_player_card = 1; show_enemy_card = 1; top_mode = 1;
    }

    render_board(cam);

    if (show_player_card) draw_board_card(cam, PLAYER_CARD_COL, PLAYER_CARD_ROW, player_summon_id, 1);
    if (show_enemy_card) draw_board_card(cam, ENEMY_CARD_COL, ENEMY_CARD_ROW, enemy_summon_id, 0);

    if (support_demo_cursor) {
        draw_zone_cursor(cam, placement_scan_col(f, 132, 0), 3); /* spell/trap row demonstration */
    } else if (top_mode && f < 248) {
        draw_zone_cursor(cam, placement_scan_col(f, 220, PLAYER_CARD_COL), PLAYER_CARD_ROW);
    } else if (top_mode && f < 286) {
        draw_zone_cursor(cam, PLAYER_CARD_COL, PLAYER_CARD_ROW);
    }
    if (top_mode && f >= 506) {
        draw_zone_cursor(cam, ENEMY_CARD_COL, ENEMY_CARD_ROW);
    }

    if (f >= 248 && f < 286) {
        draw_flying_card(cam, player_summon_id, 0, PLAYER_CARD_COL, PLAYER_CARD_ROW, f, 248, 286, 1);
    }
    if (f >= 472 && f < 506) {
        draw_flying_card(cam, enemy_summon_id, 2, ENEMY_CARD_COL, ENEMY_CARD_ROW, f, 472, 506, 0);
    } else if (f >= 506 && f < 554) {
        draw_board_card(cam, ENEMY_CARD_COL, ENEMY_CARD_ROW, enemy_summon_id, 0);
    }

    draw_hud();

    g_player_hand_offset_y = player_hand_offset;
    g_enemy_hand_offset_y = enemy_hand_offset;
    g_suppress_hand_cursor = (top_mode || player_hand_offset > 0 || enemy_hand_offset > 0);
    g_player_hide_index = (f >= 248 && f < 286) ? 0 : -1;
    g_enemy_hide_index = (f >= 472 && f < 506) ? 2 : -1;
    if (show_hand) draw_player_hand(f, selected);
    if (enemy_draw_sequence) draw_enemy_hand_draw_sequence(f, 392, selected);
    else if (show_enemy_hand) draw_enemy_hand(f, selected, 0);
    g_player_hand_offset_y = 0;
    g_enemy_hand_offset_y = 0;
    g_suppress_hand_cursor = 0;
    g_player_hide_index = -1;
    g_enemy_hide_index = -1;

    if (f >= 84 && f < 302) draw_bottom_info(hand_ids[0], "HAND");
    else if (f >= 392 && f < 506) draw_bottom_info(enemy_summon_id, "COM");
    else if (f >= 506) draw_bottom_info(player_summon_id, "BATTLE");
}

static void ensure_dir(const char *out_dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", out_dir);
    system(cmd);
}

static void write_frame_png(const char *out_dir, int f)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05d.png", out_dir, f);
    cfx_write_png8(path, framebuffer, W, H, waifu_palette_rgb);
}

static void write_showcase(const char *out_dir)
{
    int frames[] = {0, 30, 72, 120, 150, 185, 210, 230, 260, 300, TITLE_SEQUENCE_FRAMES+0, TITLE_SEQUENCE_FRAMES+40, TITLE_SEQUENCE_FRAMES+130, TITLE_SEQUENCE_FRAMES+170, TITLE_SEQUENCE_FRAMES+245, TITLE_SEQUENCE_FRAMES+DUEL_PREVIEW_END, TITLE_SEQUENCE_FRAMES+DUEL_PREVIEW_END+32, TITLE_SEQUENCE_FRAMES+226, TITLE_SEQUENCE_FRAMES+286, TITLE_SEQUENCE_FRAMES+350, TITLE_SEQUENCE_FRAMES+430, TITLE_SEQUENCE_FRAMES+540, TITLE_SEQUENCE_FRAMES+650, TITLE_SEQUENCE_FRAMES+790, TITLE_SEQUENCE_FRAMES+965, TITLE_SEQUENCE_FRAMES+1305, TITLE_SEQUENCE_FRAMES+1450, TITLE_SEQUENCE_FRAMES+1750, TITLE_SEQUENCE_FRAMES+1840, TITLE_SEQUENCE_FRAMES+1970, TITLE_SEQUENCE_FRAMES+2225, TITLE_SEQUENCE_FRAMES+2305, TITLE_SEQUENCE_FRAMES+2470, TITLE_SEQUENCE_FRAMES+2820, TITLE_SEQUENCE_FRAMES+3020};
    char path[512];
    ensure_dir(out_dir);
    for (size_t i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
        render_frame(frames[i]);
        snprintf(path, sizeof(path), "%s/show_%02zu_f%03d.png", out_dir, i, frames[i]);
        cfx_write_png8(path, framebuffer, W, H, waifu_palette_rgb);
    }
}



/* ------------------------------------------------------------------------- */
/* Platform-agnostic game API used by the SDL 1.2 backend and by headless      */
/* command playback. SDL never drives the old scripted movie path: Battle Mode */
/* is now an input-driven state machine. Headless can feed the same API from   */
/* an external command file.                                                   */

typedef enum WaifuInteractiveState {
    WAIFU_I_TITLE = 0,
    WAIFU_I_MENU,
    WAIFU_I_STORY_NAME,
    WAIFU_I_STORY_NAME_TO_INTRO,
    WAIFU_I_STORY_INTRO,
    WAIFU_I_STORY_FIRE,
    WAIFU_I_STORY_FIRE_TO_DECK,
    WAIFU_I_STORY_MAP,
    WAIFU_I_STORY_PYRAMID,
    WAIFU_I_STORY_SAVE,
    WAIFU_I_STORY_TO_PLAZA,
    WAIFU_I_STORY_PLAZA,
    WAIFU_I_DECK_EDITOR,
    WAIFU_I_DECK_PREVIEW,
    WAIFU_I_BATTLE
} WaifuInteractiveState;

typedef enum WaifuBattlePhase {
    IB_OPENING = 0,
    IB_PLAYER_HAND,
    IB_PLAYER_TOP,
    IB_CARD_PREVIEW,
    IB_FIELD_CARD_PREVIEW,
    IB_PLAYER_PLACE,
    IB_PLAYER_EQUIP_TARGET,
    IB_PLAYER_EQUIP_ANIM,
    IB_PLAYER_BATTLE,
    IB_PLAYER_RETURN_TOP,
    IB_TURN_TO_COM,
    IB_COM_SELECT,
    IB_COM_PLACE,
    IB_COM_BATTLE,
    IB_COM_RETURN,
    IB_TURN_TO_PLAYER,
    IB_PLAYER_DRAW,
    IB_RESULT,
    IB_TALLY
} WaifuBattlePhase;

#define I_HAND 5
#define I_FIELD 5
#define CARD_NONE (-1)
#define BATTLE_ANIM_FRAMES 196
#define DIRECT_ATTACK_ANIM_FRAMES 108

static int g_api_initialized = 0;
static WaifuInteractiveState g_i_state = WAIFU_I_TITLE;
static int g_i_frame = 0;
static int g_i_menu_selected = 1;
static WaifuFmInput g_prev_input;

static WaifuBattlePhase g_b_phase = IB_OPENING;
static int g_b_frame = 0;
static int g_b_phase_frame = 0;
static int g_b_selected_hand = 0;
static int g_b_selected_player_slot = 0;
static int g_b_selected_com_slot = 0;
static int g_b_preview_card_id = CARD_NONE;
static int g_b_place_hand = -1;
static int g_b_place_slot = -1;
static int g_b_place_card = -1;
static int g_b_equip_hand = -1;
static int g_b_equip_slot = -1;
static int g_b_equip_zone_slot = -1;
static int g_b_equip_card = CARD_NONE;
static int g_b_equip_target_card = CARD_NONE;
static int g_b_equip_target_faceup = 1;
static int g_b_equip_base_atk = 0;
static int g_b_equip_base_def = 0;
static int g_b_equip_pending_atk = 0;
static int g_b_equip_pending_def = 0;
static int g_b_battle_atk_slot = -1;
static int g_b_battle_def_slot = -1;
static int g_b_battle_atk_owner = 0; /* 0 player, 1 COM */
static int g_b_battle_atk_card = CARD_NONE;
static int g_b_battle_def_card = CARD_NONE;
static int g_b_battle_atk_display_atk = 0;
static int g_b_battle_atk_display_def = 0;
static int g_b_battle_def_display_atk = 0;
static int g_b_battle_def_display_def = 0;
static int g_b_battle_atk_back = 0;
static int g_b_battle_def_back = 0;
static int g_b_battle_outcome = BATTLE_DESTROY_DEFENDER;
static char g_b_damage_text[16] = "0";
static int g_b_result = 0;
static int g_b_turns = 1;
static int g_b_cards_used = 0;
static int g_b_deck_seed = 17;

static int g_i_player_hand[I_HAND];
static int g_i_player_used[I_HAND];
static int g_i_com_hand[I_HAND];
static int g_i_com_used[I_HAND];
static int g_i_player_field[I_FIELD];
static int g_i_player_faceup[I_FIELD];
static int g_i_player_defense[I_FIELD];
static int g_i_player_atk_bonus[I_FIELD];
static int g_i_player_def_bonus[I_FIELD];
static int g_i_player_equip_field[I_FIELD];
static int g_i_player_equip_target[I_FIELD];
static int g_i_com_field[I_FIELD];
static int g_i_com_faceup[I_FIELD];
static int g_i_com_defense[I_FIELD];
static int g_i_com_atk_bonus[I_FIELD];
static int g_i_com_def_bonus[I_FIELD];
static int g_i_player_attacked[I_FIELD];
static int g_i_com_attacked[I_FIELD];
static int g_b_draw_slots[I_HAND];
static int g_b_draw_count = 0;
static int g_b_player_hand_intro_pending = 1;
static int g_b_direct_damage = 0;
static int g_i_player_deck_left = 35;
static int g_i_com_deck_left = 35;
static int g_b_player_monster_played_this_turn = 0;
static int g_b_com_monster_played_this_turn = 0;

#define STORY_NAME_LEN 6
#define STORY_DECK_SIZE 40
#define STORY_SAVE_PATH "waifu_story.sav"
static int g_story_battle_active = 0;
static char g_story_name[STORY_NAME_LEN + 1] = "SERENA";
static int g_story_name_pos = 0;
static int g_story_intro_line = 0;
static int g_story_player_deck[STORY_DECK_SIZE];
static int g_story_player_deck_pos = 0;
static int g_story_strong_card = 37;
static int g_story_weak_card = 29;
static int g_story_equip_count = 0;
static int g_story_support_count = 0;

#define STORY_STORAGE_SIZE 64
#define DECK_GRID_COLS 6
#define DECK_GRID_ROWS 3
#define DECK_VISIBLE_CARDS (DECK_GRID_COLS * DECK_GRID_ROWS)
static int g_story_storage[STORY_STORAGE_SIZE];
static int g_story_deck_count = STORY_DECK_SIZE;
static int g_story_storage_count = 0;
static int g_story_fire_line = 0;
static int g_deck_tab = 0; /* 0 deck, 1 storage */
static int g_deck_cursor = 0;
static int g_deck_scroll[2] = {0, 0};
static int g_deck_flash = 0;
static int g_deck_flash_reason = 0; /* 0 generic/count, 1 copy limit */
static int g_deck_preview_card = CARD_NONE;

#define STORY_MAX_DUELS 8
static int g_story_duel_index = 0;
static int g_story_map_cursor = 0;     /* 0 pyramid, 1 plaza */
static int g_story_pyramid_cursor = 0; /* 0 save, 1 editor, 2 back */
static int g_story_plaza_line = 0;
static int g_story_saved_flash = 0;
static int g_story_com_draw_pos = 0;
static int g_story_editor_from_pyramid = 0;
static int g_story_save_status = 0; /* 1 saved/loaded, -1 failed/no save */

static void story_return_to_map_after_duel(void);
static void init_battle_state(void);

static void draw_cutin_battle_card(int id, int x, int y, int back, int attacker_card)
{
    if (!back && id >= 0) {
        if (attacker_card && id == g_b_battle_atk_card) {
            draw_big_battle_card_stats(id, x, y, 0, g_b_battle_atk_display_atk, g_b_battle_atk_display_def);
            return;
        }
        if (!attacker_card && id == g_b_battle_def_card) {
            draw_big_battle_card_stats(id, x, y, 0, g_b_battle_def_display_atk, g_b_battle_def_display_def);
            return;
        }
    }
    draw_big_battle_card(id, x, y, back);
}

static int input_pressed(int now, int prev) { return now && !prev; }

static int player_can_place_monster(void)
{
    return !g_b_player_monster_played_this_turn;
}

static int com_can_place_monster(void)
{
    return !g_b_com_monster_played_this_turn;
}

static int next_live_hand_index(int cur, int dir)
{
    int i;
    for (i = 0; i < I_HAND; ++i) {
        cur = (cur + dir + I_HAND) % I_HAND;
        if (!g_i_player_used[cur]) return cur;
    }
    return 0;
}

static int first_live_player_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_player_field[i] >= 0) return i;
    return -1;
}

static int first_live_com_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_com_field[i] >= 0) return i;
    return -1;
}

static void set_top_selector(int col, int row)
{
    if (col < 0) col = 0;
    if (col >= BOARD_COLS) col = BOARD_COLS - 1;
    if (row < 0) row = 0;
    if (row >= BOARD_ROWS) row = BOARD_ROWS - 1;
    if (g_b_top_col != col || g_b_top_row != row) {
        g_b_top_prev_col = g_b_top_col;
        g_b_top_prev_row = g_b_top_row;
        g_b_top_cursor_anim = 0;
    } else {
        g_b_top_prev_col = col;
        g_b_top_prev_row = row;
        g_b_top_cursor_anim = 8;
    }
    g_b_top_col = col;
    g_b_top_row = row;
    if (row == PLAYER_CARD_ROW) g_b_selected_player_slot = col;
    if (row == ENEMY_CARD_ROW) g_b_selected_com_slot = col;
}

static void move_top_selector(int dx, int dy)
{
    set_top_selector(g_b_top_col + dx, g_b_top_row + dy);
}

static int top_selector_player_monster_slot(void)
{
    if (g_b_top_row != PLAYER_CARD_ROW || g_b_top_col < 0 || g_b_top_col >= I_FIELD) return -1;
    return g_i_player_field[g_b_top_col] >= 0 ? g_b_top_col : -1;
}

static int top_selector_com_monster_slot(void)
{
    if (g_b_top_row != ENEMY_CARD_ROW || g_b_top_col < 0 || g_b_top_col >= I_FIELD) return -1;
    return g_i_com_field[g_b_top_col] >= 0 ? g_b_top_col : -1;
}

/* Card id under the tactical top-view selector for any occupied zone (player
   monster, opponent monster, or player equip row), or -1 when the zone is
   empty. Used by the B-button card check from top view. */
static int top_selector_preview_card(void)
{
    if (g_b_top_col < 0 || g_b_top_col >= I_FIELD) return -1;
    if (g_b_top_row == PLAYER_CARD_ROW && g_i_player_field[g_b_top_col] >= 0)
        return g_i_player_field[g_b_top_col];
    if (g_b_top_row == ENEMY_CARD_ROW && g_i_com_field[g_b_top_col] >= 0)
        return g_i_com_field[g_b_top_col];
    if (g_b_top_row == PLAYER_CARD_ROW + 1 && g_i_player_equip_field[g_b_top_col] >= 0)
        return g_i_player_equip_field[g_b_top_col];
    return -1;
}

static int card_base_atk(int id)
{
    return is_monster_card(id) ? (int)waifu_card_atk[id] : 0;
}

static int card_base_def(int id)
{
    return is_monster_card(id) ? (int)waifu_card_def[id] : 0;
}

static int field_card_atk(int owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (!is_monster_card(id)) return 0;
    return card_base_atk(id) + (owner == 0 ? g_i_player_atk_bonus[slot] : g_i_com_atk_bonus[slot]);
}

static int field_card_def(int owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (!is_monster_card(id)) return 0;
    return card_base_def(id) + (owner == 0 ? g_i_player_def_bonus[slot] : g_i_com_def_bonus[slot]);
}

static void draw_bottom_info_field(int owner, int slot, const char *mode)
{
    int card_id;
    if (slot < 0 || slot >= I_FIELD) return;
    card_id = owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    draw_bottom_info_offset_ex(card_id, mode, 0, field_card_atk(owner, slot), field_card_def(owner, slot));
}

static void draw_bottom_empty_field(const char *mode)
{
    char line[48];
    int base = 205;
    rect_fill(0, base, 256, 35, IDX_UI_TEAL);
    hline(0,255,base,IDX_WHITE); hline(0,255,base+1,IDX_UI_LIGHT); hline(0,255,base+2,IDX_DIM);
    for (int y = base+4; y < base+35; y += 3) hline(0,255,y,IDX_UI_TEAL2);
    snprintf(line, sizeof(line), "%s C%d R%d", mode ? mode : "FIELD", g_b_top_col + 1, g_b_top_row + 1);
    draw_text(6, base+6, line, IDX_DIM, IDX_BLACK);
    draw_text_small(6, base+21, "EMPTY ZONE", IDX_WHITE, IDX_BLACK);
}

static void draw_bottom_info_top_selector(const char *mode)
{
    if (g_b_top_col < 0 || g_b_top_col >= I_FIELD) {
        draw_bottom_empty_field(mode);
        return;
    }
    if (g_b_top_row == PLAYER_CARD_ROW && g_i_player_field[g_b_top_col] >= 0) {
        draw_bottom_info_field(0, g_b_top_col, mode ? mode : "FIELD");
    } else if (g_b_top_row == ENEMY_CARD_ROW && g_i_com_field[g_b_top_col] >= 0) {
        draw_bottom_info_field(1, g_b_top_col, mode ? mode : "TARGET");
    } else if (g_b_top_row == PLAYER_CARD_ROW + 1 && g_i_player_equip_field[g_b_top_col] >= 0) {
        draw_bottom_info(g_i_player_equip_field[g_b_top_col], mode ? mode : "EQUIP");
    } else {
        draw_bottom_empty_field(mode);
    }
}

static int field_card_defense_position(int owner, int slot)
{
    if (slot < 0 || slot >= I_FIELD) return 0;
    return owner == 0 ? g_i_player_defense[slot] : g_i_com_defense[slot];
}

static int equip_atk_bonus(int card_id)
{
    (void)card_id;
    return 500;
}

static int equip_def_bonus(int card_id)
{
    return support_card_kind(card_id) == 0 ? 300 : 0;
}


static int count_live_player_monsters(void)
{
    int i, n = 0;
    for (i = 0; i < I_FIELD; ++i) if (g_i_player_field[i] >= 0) ++n;
    return n;
}

static int count_live_com_monsters(void)
{
    int i, n = 0;
    for (i = 0; i < I_FIELD; ++i) if (g_i_com_field[i] >= 0) ++n;
    return n;
}

static int selected_or_first_live_player_slot(void)
{
    if (g_b_selected_player_slot >= 0 && g_b_selected_player_slot < I_FIELD &&
        g_i_player_field[g_b_selected_player_slot] >= 0) {
        return g_b_selected_player_slot;
    }
    return first_live_player_slot();
}

static int first_attackable_com_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_com_field[i] >= 0 && !g_i_com_attacked[i]) return i;
    }
    return -1;
}

static int player_first_turn_attack_locked(void)
{
    /* Yu-Gi-Oh-style rule: the player who opens the duel cannot attack during
       their first turn. COM may still attack on its first turn after the opener
       passes, so this guard is player-side only. */
    return g_b_turns <= 1;
}

static void clear_player_attacks(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) g_i_player_attacked[i] = 0;
}

static void clear_com_attacks(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) g_i_com_attacked[i] = 0;
}

static int first_free_player_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_player_field[i] < 0) return i;
    return -1;
}

static int first_free_player_equip_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_player_equip_field[i] < 0) return i;
    return -1;
}

static void clear_player_equips_for_target(int target_slot)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_equip_target[i] == target_slot) {
            g_i_player_equip_field[i] = CARD_NONE;
            g_i_player_equip_target[i] = -1;
        }
    }
}

static int first_free_com_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_com_field[i] < 0) return i;
    return -1;
}

static void clear_battle_snapshot(void)
{
    g_b_battle_atk_slot = -1;
    g_b_battle_def_slot = -1;
    g_b_battle_atk_card = CARD_NONE;
    g_b_battle_def_card = CARD_NONE;
    g_b_battle_atk_display_atk = 0;
    g_b_battle_atk_display_def = 0;
    g_b_battle_def_display_atk = 0;
    g_b_battle_def_display_def = 0;
    g_b_battle_atk_back = 0;
    g_b_battle_def_back = 0;
    g_b_direct_damage = 0;
}

static void set_battle_phase(WaifuBattlePhase phase)
{
    g_b_phase = phase;
    g_b_phase_frame = 0;
}

static int story_hash_name(void)
{
    int h = 216;
    for (int i = 0; i < STORY_NAME_LEN; ++i) h = (h * 33 + (unsigned char)g_story_name[i]) & 0x7fffffff;
    return h ? h : 17;
}

static int story_prng_next(int *seed)
{
    uint32_t v = (uint32_t)(*seed);
    v = v * 1103515245u + 12345u;
    *seed = (int)(v & 0x7fffffffu);
    return *seed;
}

static void story_shuffle_tail(int start, int *seed)
{
    for (int i = STORY_DECK_SIZE - 1; i > start; --i) {
        int j = start + (story_prng_next(seed) % (i - start + 1));
        int tmp = g_story_player_deck[i];
        g_story_player_deck[i] = g_story_player_deck[j];
        g_story_player_deck[j] = tmp;
    }
}

static void generate_story_starter_deck(void)
{
    int seed = story_hash_name();
    int strong_pool[] = {37, 40, 33, 28, 8, 14};
    int weak_pool[] = {29, 20, 23, 34, 19, 43, 30, 6};
    int strong = strong_pool[story_prng_next(&seed) % (int)(sizeof(strong_pool) / sizeof(strong_pool[0]))];
    int weak = weak_pool[story_prng_next(&seed) % (int)(sizeof(weak_pool) / sizeof(weak_pool[0]))];
    int idx = 0;

    g_story_strong_card = strong;
    g_story_weak_card = weak;
    g_story_equip_count = 0;
    g_story_support_count = 0;
    g_story_deck_count = STORY_DECK_SIZE;

    for (int i = 0; i < STORY_DECK_SIZE; ++i) g_story_player_deck[i] = CARD_NONE;

    /* Guaranteed opening profile, but every card still obeys the four-copy
       deck construction rule. */
    g_story_player_deck[idx++] = strong;
    g_story_player_deck[idx++] = weak;
    g_story_player_deck[idx++] = weak;
    g_story_player_deck[idx++] = SUPPORT_EQUIP_CARD_ID;
    g_story_player_deck[idx++] = SUPPORT_GUARD_CARD_ID + (story_prng_next(&seed) % 3);

    while (idx < STORY_DECK_SIZE) {
        int roll = story_prng_next(&seed) % 100;
        int card;
        if (roll < 34) {
            card = weak;
        } else if (roll < 48) {
            card = weak_pool[story_prng_next(&seed) % (int)(sizeof(weak_pool) / sizeof(weak_pool[0]))];
        } else if (roll < 62) {
            card = SUPPORT_EQUIP_CARD_ID;
        } else if (roll < 82) {
            card = WAIFU_CARD_COUNT + (story_prng_next(&seed) % SUPPORT_CARD_VARIANTS);
        } else {
            int mid = story_prng_next(&seed) % WAIFU_CARD_COUNT;
            if (mid == strong) mid = (mid + 5) % WAIFU_CARD_COUNT;
            card = mid;
        }
        int copies = 0;
        for (int j = 0; j < idx; ++j) if (g_story_player_deck[j] == card) ++copies;
        if (copies >= 4) continue;
        g_story_player_deck[idx++] = card;
    }
    story_shuffle_tail(5, &seed);
    g_story_player_deck_pos = 0;

    for (idx = 0; idx < STORY_DECK_SIZE; ++idx) {
        if (g_story_player_deck[idx] == SUPPORT_EQUIP_CARD_ID) ++g_story_equip_count;
        else if (is_support_card(g_story_player_deck[idx])) ++g_story_support_count;
    }
}


static void generate_story_storage_pool(void)
{
    for (int i = 0; i < STORY_STORAGE_SIZE; ++i) g_story_storage[i] = CARD_NONE;
    g_story_storage_count = 0;
}

static void reset_story_deck_editor(void)
{
    g_deck_tab = 0;
    g_deck_cursor = 0;
    g_deck_scroll[0] = 0;
    g_deck_scroll[1] = 0;
    g_deck_flash = 0;
    g_deck_flash_reason = 0;
    g_deck_preview_card = CARD_NONE;
}

static int story_deck_card_count(int card)
{
    int n = 0;
    for (int i = 0; i < g_story_deck_count; ++i) if (g_story_player_deck[i] == card) ++n;
    return n;
}

static int story_deck_can_add_card(int card)
{
    return story_deck_card_count(card) < 4;
}

static int story_deck_max_card_copies(void)
{
    int max = 0;
    for (int i = 0; i < g_story_deck_count; ++i) {
        int n = 0;
        for (int j = 0; j < g_story_deck_count; ++j) if (g_story_player_deck[j] == g_story_player_deck[i]) ++n;
        if (n > max) max = n;
    }
    return max;
}

static int *deck_editor_active_array(void)
{
    return g_deck_tab ? g_story_storage : g_story_player_deck;
}

static int deck_editor_active_count(void)
{
    return g_deck_tab ? g_story_storage_count : g_story_deck_count;
}

static void deck_editor_clamp_cursor(void)
{
    int count = deck_editor_active_count();
    int *scroll = &g_deck_scroll[g_deck_tab];
    if (count <= 0) {
        g_deck_cursor = 0;
        *scroll = 0;
        return;
    }
    if (g_deck_cursor < 0) g_deck_cursor = 0;
    if (g_deck_cursor >= count) g_deck_cursor = count - 1;
    if (*scroll < 0) *scroll = 0;
    if (g_deck_cursor < *scroll) *scroll = g_deck_cursor;
    if (g_deck_cursor >= *scroll + DECK_VISIBLE_CARDS) *scroll = g_deck_cursor - DECK_VISIBLE_CARDS + 1;
    if (*scroll > count - 1) *scroll = count - 1;
}

static void deck_editor_switch_tab(void)
{
    g_deck_tab ^= 1;
    deck_editor_clamp_cursor();
}

static void deck_editor_move_cursor(int dx, int dy)
{
    int count = deck_editor_active_count();
    if (count <= 0) { deck_editor_switch_tab(); return; }
    if (dy < 0 && g_deck_cursor < DECK_GRID_COLS) {
        deck_editor_switch_tab();
        return;
    }
    if (dy > 0) g_deck_cursor += DECK_GRID_COLS;
    if (dy < 0) g_deck_cursor -= DECK_GRID_COLS;
    if (dx != 0) g_deck_cursor += dx;
    if (g_deck_cursor < 0) g_deck_cursor = count - 1;
    if (g_deck_cursor >= count) g_deck_cursor = 0;
    deck_editor_clamp_cursor();
}

static void remove_card_at(int *arr, int *count, int idx)
{
    if (!arr || !count || idx < 0 || idx >= *count) return;
    for (int i = idx; i < *count - 1; ++i) arr[i] = arr[i + 1];
    --(*count);
}

static void append_card_to(int *arr, int *count, int max_count, int card)
{
    if (!arr || !count || *count >= max_count) return;
    arr[*count] = card;
    ++(*count);
}

static void recalc_story_deck_counts(void)
{
    g_story_equip_count = 0;
    g_story_support_count = 0;
    for (int i = 0; i < g_story_deck_count; ++i) {
        if (g_story_player_deck[i] == SUPPORT_EQUIP_CARD_ID) ++g_story_equip_count;
        else if (is_support_card(g_story_player_deck[i])) ++g_story_support_count;
    }
}

static void sanitize_story_deck_copy_limit(void)
{
    int i = 0;
    while (i < g_story_deck_count) {
        int copies = 0;
        for (int j = 0; j <= i; ++j) if (g_story_player_deck[j] == g_story_player_deck[i]) ++copies;
        if (copies > 4) {
            int card = g_story_player_deck[i];
            remove_card_at(g_story_player_deck, &g_story_deck_count, i);
            append_card_to(g_story_storage, &g_story_storage_count, STORY_STORAGE_SIZE, card);
        } else {
            ++i;
        }
    }
}

static int story_reward_drop_card(void)
{
    int seed = story_hash_name() ^ (g_story_duel_index * 131 + g_b_turns * 17 + g_b_cards_used * 29 + 0x2468);
    int strong_pool[] = {37, 40, 33, 28, 8, 14};
    int roll = story_prng_next(&seed) % 100;
    if (roll == 0) {
        return strong_pool[story_prng_next(&seed) % (int)(sizeof(strong_pool) / sizeof(strong_pool[0]))];
    }
    if (roll < 11) {
        return SUPPORT_EQUIP_CARD_ID;
    }
    return story_prng_next(&seed) % WAIFU_CARD_COUNT;
}

static void award_story_win_drop(void)
{
    if (g_story_storage_count >= STORY_STORAGE_SIZE) return;
    append_card_to(g_story_storage, &g_story_storage_count, STORY_STORAGE_SIZE, story_reward_drop_card());
}

static int story_save_exists(void)
{
    FILE *fp = fopen(STORY_SAVE_PATH, "r");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static int write_story_save(void)
{
    FILE *fp = fopen(STORY_SAVE_PATH, "w");
    if (!fp) return 0;

    fprintf(fp, "WAIFU_STORY_SAVE_V1\n");
    fprintf(fp, "name %s\n", g_story_name);
    fprintf(fp, "duel %d\n", g_story_duel_index);
    fprintf(fp, "map %d pyramid %d plaza %d\n", g_story_map_cursor, g_story_pyramid_cursor, g_story_plaza_line);
    fprintf(fp, "deck_count %d storage_count %d\n", g_story_deck_count, g_story_storage_count);
    fprintf(fp, "deck");
    for (int i = 0; i < g_story_deck_count; ++i) fprintf(fp, " %d", g_story_player_deck[i]);
    fprintf(fp, "\n");
    fprintf(fp, "storage");
    for (int i = 0; i < g_story_storage_count; ++i) fprintf(fp, " %d", g_story_storage[i]);
    fprintf(fp, "\n");

    if (fclose(fp) != 0) return 0;
    return 1;
}

static int read_story_save(void)
{
    FILE *fp = fopen(STORY_SAVE_PATH, "r");
    char magic[64];
    char key[64];
    char saved_name[64];
    char tok1[64];
    char tok2[64];
    int duel = 0, map_cursor = 0, pyramid_cursor = 0, plaza_line = 0;
    int deck_count = 0, storage_count = 0;

    if (!fp) return 0;
    if (fscanf(fp, "%63s", magic) != 1 || strcmp(magic, "WAIFU_STORY_SAVE_V1") != 0) { fclose(fp); return 0; }
    if (fscanf(fp, "%63s %63s", key, saved_name) != 2 || strcmp(key, "name") != 0) { fclose(fp); return 0; }
    if (fscanf(fp, "%63s %d", key, &duel) != 2 || strcmp(key, "duel") != 0) { fclose(fp); return 0; }
    if (fscanf(fp, "%63s %d %63s %d %63s %d", key, &map_cursor, tok1, &pyramid_cursor, tok2, &plaza_line) != 6 ||
        strcmp(key, "map") != 0 || strcmp(tok1, "pyramid") != 0 || strcmp(tok2, "plaza") != 0) { fclose(fp); return 0; }
    if (fscanf(fp, "%63s %d %63s %d", key, &deck_count, tok1, &storage_count) != 4 ||
        strcmp(key, "deck_count") != 0 || strcmp(tok1, "storage_count") != 0) { fclose(fp); return 0; }
    if (deck_count < 0 || deck_count > STORY_DECK_SIZE || storage_count < 0 || storage_count > STORY_STORAGE_SIZE) { fclose(fp); return 0; }

    if (fscanf(fp, "%63s", key) != 1 || strcmp(key, "deck") != 0) { fclose(fp); return 0; }
    for (int i = 0; i < deck_count; ++i) { if (fscanf(fp, "%d", &g_story_player_deck[i]) != 1) { fclose(fp); return 0; } }
    if (fscanf(fp, "%63s", key) != 1 || strcmp(key, "storage") != 0) { fclose(fp); return 0; }
    for (int i = 0; i < storage_count; ++i) { if (fscanf(fp, "%d", &g_story_storage[i]) != 1) { fclose(fp); return 0; } }
    fclose(fp);

    memset(g_story_name, 0, sizeof(g_story_name));
    strncpy(g_story_name, saved_name, STORY_NAME_LEN);
    for (int i = 0; i < STORY_NAME_LEN; ++i) {
        if (g_story_name[i] < 'A' || g_story_name[i] > 'Z') g_story_name[i] = 'A';
    }
    g_story_name[STORY_NAME_LEN] = '\0';
    g_story_duel_index = duel;
    if (g_story_duel_index < 0) g_story_duel_index = 0;
    if (g_story_duel_index >= STORY_MAX_DUELS) g_story_duel_index = STORY_MAX_DUELS - 1;
    g_story_map_cursor = map_cursor ? 1 : 0;
    g_story_pyramid_cursor = pyramid_cursor;
    if (g_story_pyramid_cursor < 0 || g_story_pyramid_cursor > 2) g_story_pyramid_cursor = 0;
    g_story_plaza_line = plaza_line;
    if (g_story_plaza_line < 0) g_story_plaza_line = 0;
    g_story_deck_count = deck_count;
    g_story_storage_count = storage_count;
    g_story_player_deck_pos = 0;
    if (g_story_deck_count > 0) g_story_strong_card = g_story_player_deck[0];
    if (g_story_deck_count > 1) g_story_weak_card = g_story_player_deck[1];
    g_story_battle_active = 0;
    g_story_intro_line = 0;
    g_story_fire_line = 0;
    g_story_saved_flash = 0;
    g_story_editor_from_pyramid = 0;
    reset_story_deck_editor();
    sanitize_story_deck_copy_limit();
    recalc_story_deck_counts();
    init_battle_state();
    return 1;
}

static int load_story_to_map(void)
{
    if (!read_story_save()) {
        g_story_save_status = -1;
        return 0;
    }
    g_story_save_status = 1;
    g_i_state = WAIFU_I_STORY_MAP;
    g_i_frame = -1;
    return 1;
}

static void deck_editor_move_selected_card(void)
{
    int count = deck_editor_active_count();
    int card;
    if (count <= 0) { g_deck_flash = 50; return; }
    if (g_deck_tab == 0) {
        if (g_story_storage_count >= STORY_STORAGE_SIZE) { g_deck_flash = 50; return; }
        card = g_story_player_deck[g_deck_cursor];
        remove_card_at(g_story_player_deck, &g_story_deck_count, g_deck_cursor);
        append_card_to(g_story_storage, &g_story_storage_count, STORY_STORAGE_SIZE, card);
    } else {
        if (g_story_deck_count >= STORY_DECK_SIZE) { g_deck_flash = 50; g_deck_flash_reason = 0; return; }
        card = g_story_storage[g_deck_cursor];
        if (!story_deck_can_add_card(card)) { g_deck_flash = 50; g_deck_flash_reason = 1; return; }
        remove_card_at(g_story_storage, &g_story_storage_count, g_deck_cursor);
        append_card_to(g_story_player_deck, &g_story_deck_count, STORY_DECK_SIZE, card);
    }
    g_deck_flash_reason = 0;
    recalc_story_deck_counts();
    deck_editor_clamp_cursor();
}

static int next_random_draw_id(void)
{
    int id = g_b_deck_seed % WAIFU_CARD_COUNT;
    g_b_deck_seed = (g_b_deck_seed * 13 + 7) % 997;
    return id;
}

static const char *story_opponent_name(void)
{
    switch (g_story_duel_index) {
    case 0: return "DREAM SHADE";
    case 1: return "PLAZA NOVICE";
    case 2: return "TEMPLE ADEPT";
    case 3: return "SAND REAVER";
    case 4: return "BURNING SOUL";
    case 5: return "VOID WALKER";
    case 6: return "SPHINX GUARDIAN";
    default: return "THE DEMON";
    }
}

static int story_opponent_is_boss(void)
{
    return g_story_duel_index == 4 || g_story_duel_index == 6 || g_story_duel_index == 7;
}

static int story_opponent_card_at(int duel, int pos)
{
    static const int dream[]    = {20, 23, 29, 34, 19, 30, 6, 43, 29, 20};
    static const int plaza[]    = {12, 15, 22, 29, 36, 6, 19, 23, 30, 34};
    static const int adept[]    = {28, 33, 37, 15, 22, 40, 12, 36, 8, 14};
    static const int reaver[]   = {37, 40, 8, 14, 28, 33, 12, 36, 22, 15};
    static const int burning[]  = {8, 37, 40, 14, 28, 33, 36, 22, 12, 15};
    static const int void_w[]   = {40, 37, 8, 28, 14, 33, 36, 22, 12, 15};
    static const int sphinx[]   = {33, 37, 40, 28, 14, 8, 36, 22, 12, 15};
    static const int demon[]    = {8, 37, 40, 28, 14, 33, 36, 22, 12, 15};
    const int *pool = dream;
    int count = (int)(sizeof(dream) / sizeof(dream[0]));
    if (duel == 1) { pool = plaza; count = (int)(sizeof(plaza) / sizeof(plaza[0])); }
    else if (duel == 2) { pool = adept; count = (int)(sizeof(adept) / sizeof(adept[0])); }
    else if (duel == 3) { pool = reaver; count = (int)(sizeof(reaver) / sizeof(reaver[0])); }
    else if (duel == 4) { pool = burning; count = (int)(sizeof(burning) / sizeof(burning[0])); }
    else if (duel == 5) { pool = void_w; count = (int)(sizeof(void_w) / sizeof(void_w[0])); }
    else if (duel == 6) { pool = sphinx; count = (int)(sizeof(sphinx) / sizeof(sphinx[0])); }
    else if (duel >= 7) { pool = demon; count = (int)(sizeof(demon) / sizeof(demon[0])); }
    return pool[pos % count];
}

static int next_story_com_draw_id(void)
{
    int card = story_opponent_card_at(g_story_duel_index, g_story_com_draw_pos++);
    if (g_story_duel_index >= 2 && (g_story_com_draw_pos % 7) == 0) card = SUPPORT_EQUIP_CARD_ID;
    return card;
}

static int next_draw_id(void)
{
    if (g_story_battle_active && g_story_player_deck_pos < g_story_deck_count) {
        return g_story_player_deck[g_story_player_deck_pos++];
    }
    return next_random_draw_id();
}

static int next_com_draw_id(void)
{
    /* COM draws must never consume Serena's story/player deck. */
    if (g_story_battle_active) return next_story_com_draw_id();
    return next_random_draw_id();
}

static void init_battle_state(void)
{
    int i;
    g_story_battle_active = 0;
    for (i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = hand_ids[i];
        g_i_player_used[i] = 0;
        g_i_com_hand[i] = com_hand_ids[i];
        g_i_com_used[i] = 0;
    }
    for (i = 0; i < I_FIELD; ++i) {
        g_i_player_field[i] = CARD_NONE;
        g_i_player_faceup[i] = 1;
        g_i_player_defense[i] = 0;
        g_i_player_atk_bonus[i] = 0;
        g_i_player_def_bonus[i] = 0;
        g_i_player_equip_field[i] = CARD_NONE;
        g_i_player_equip_target[i] = -1;
        g_i_com_field[i] = CARD_NONE;
        g_i_com_faceup[i] = 1;
        g_i_com_defense[i] = 0;
        g_i_com_atk_bonus[i] = 0;
        g_i_com_def_bonus[i] = 0;
        g_i_player_attacked[i] = 0;
        g_i_com_attacked[i] = 0;
    }
    g_you_lp = 8000;
    g_com_lp = 8000;
    g_i_player_deck_left = 35;
    g_i_com_deck_left = 35;
    g_b_phase = IB_OPENING;
    g_b_frame = 0;
    g_b_phase_frame = 0;
    g_b_selected_hand = 0;
    g_b_selected_player_slot = 0;
    g_b_selected_com_slot = 0;
    g_b_top_col = 0;
    g_b_top_row = PLAYER_CARD_ROW;
    g_b_top_prev_col = 0;
    g_b_top_prev_row = PLAYER_CARD_ROW;
    g_b_top_cursor_anim = 8;
    g_b_attack_attacker_slot = -1;
    g_b_place_hand = -1;
    g_b_place_slot = -1;
    g_b_place_card = -1;
    g_b_equip_hand = -1;
    g_b_equip_slot = -1;
    g_b_equip_zone_slot = -1;
    g_b_equip_card = CARD_NONE;
    g_b_equip_target_card = CARD_NONE;
    g_b_equip_target_faceup = 1;
    g_b_equip_base_atk = 0;
    g_b_equip_base_def = 0;
    g_b_equip_pending_atk = 0;
    g_b_equip_pending_def = 0;
    clear_battle_snapshot();
    g_b_battle_atk_owner = 0;
    g_b_battle_outcome = BATTLE_DESTROY_DEFENDER;
    g_b_draw_count = 0;
    g_b_player_hand_intro_pending = 1;
    for (i = 0; i < I_HAND; ++i) g_b_draw_slots[i] = -1;
    strcpy(g_b_damage_text, "0");
    g_b_result = 0;
    g_b_turns = 1;
    g_b_cards_used = 0;
    g_b_deck_seed = 17;
    g_b_player_monster_played_this_turn = 0;
    g_b_com_monster_played_this_turn = 0;
}

static void init_story_battle_state(void)
{
    if (g_story_deck_count != STORY_DECK_SIZE) {
        generate_story_starter_deck();
        generate_story_storage_pool();
    }
    init_battle_state();
    g_story_battle_active = 1;
    g_story_player_deck_pos = 0;
    for (int i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = next_draw_id();
        g_i_player_used[i] = 0;
    }
    g_i_player_deck_left = g_story_deck_count - I_HAND;
    g_story_com_draw_pos = 0;
    for (int i = 0; i < I_HAND; ++i) {
        g_i_com_hand[i] = next_story_com_draw_id();
        g_i_com_used[i] = 0;
    }
    g_i_com_deck_left = STORY_DECK_SIZE - I_HAND;
    g_b_deck_seed = (story_hash_name() + g_story_duel_index * 97) % 997;
    if (g_b_deck_seed <= 0) g_b_deck_seed = 17;
    /* Bosses have higher LP. */
    if (story_opponent_is_boss()) g_com_lp = 10000;
}

static void draw_interactive_field_cards(Camera cam)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_com_field[i] >= 0) draw_board_card_state(cam, i, ENEMY_CARD_ROW, g_i_com_field[i], !g_i_com_faceup[i], g_i_com_attacked[i], g_i_com_defense[i]);
    }
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_field[i] >= 0) draw_board_card_state(cam, i, PLAYER_CARD_ROW, g_i_player_field[i], !g_i_player_faceup[i], g_i_player_attacked[i], g_i_player_defense[i]);
    }
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_equip_field[i] >= 0) draw_board_card_ex(cam, i, PLAYER_CARD_ROW + 1, g_i_player_equip_field[i], 0, 0);
    }
}

static void draw_interactive_player_hand(int f, int selected, int yoff, int suppress_cursor)
{
    int i;
    int y = 154 + yoff;
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        if (f < 48) {
            int32_t t = q8_smooth_ratio(f - i * 5, 18);
            x = lerp_i(282, x0, t);
        }
        if (g_i_player_used[i]) continue;
        draw_hand_card_sprite_ex(g_i_player_hand[i], x, y, 38, 50, 0,
                                 is_monster_card(g_i_player_hand[i]) && !player_can_place_monster());
        if (!suppress_cursor && i == selected) draw_red_cursor(x, y, 38, 50);
    }
}

static void draw_interactive_com_hand(int f, int selected, int yoff)
{
    int i;
    /* SDL/live play uses the same convention as the player view: the active
       duelist's hand is presented along the lower edge. Earlier SDL builds put
       COM's hand at the top of the screen while also using a COM-facing camera,
       which made the opponent turn look upside-down and unlike the headless
       scripted presentation. */
    int y = 154 + yoff;
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        if (f < 60) {
            int32_t t = q8_smooth_ratio(f - i * 5, 18);
            x = lerp_i(282, x0, t);
        }
        if (g_i_com_used[i]) continue;
        draw_hand_card_sprite(g_i_com_hand[i], x, y, 38, 50, 1);
        if (i == selected) draw_red_cursor(x, y, 38, 50);
    }
}

static void draw_interactive_common(Camera cam, int bottom_card, const char *bottom_mode)
{
    render_board(cam);
    draw_interactive_field_cards(cam);
    draw_hud();
    if (bottom_card >= 0 && (!bottom_mode || strcmp(bottom_mode, "COM") != 0)) {
        draw_bottom_info(bottom_card, bottom_mode ? bottom_mode : "CARD");
    }
}

static void draw_interactive_base(Camera cam)
{
    render_board(cam);
    draw_interactive_field_cards(cam);
    draw_hud();
}

static void draw_preview_stat_line(int x, int y, const char *label, unsigned value)
{
    char line[32];
    snprintf(line, sizeof(line), "%s %u", label, value);
    draw_text_small(x, y, line, IDX_GOLD_HI, IDX_BLACK);
}

static void draw_interactive_card_preview(int card_id, int f)
{
    int tx = 136;
    int maxw = 112;
    int y = 17;
    int lines;
    char line[128];
    clear_screen(IDX_BLACK);

    /* Card-check/detail view: keep the full-size card display on the left,
       while wrapping the right column by pixel width so names, type lines and
       lore stay inside the 256x240 screen. */
    draw_panel_rect(2, 10, 252, 218, IDX_UI_DARK);

    if (is_support_card(card_id)) {
        rect_fill(11, 40, 120, 160, IDX_BLACK);
        draw_card_raw(waifu_support_face, WAIFU_CARD_W, WAIFU_CARD_H, 8, 36, 120, 160);
        rect_outline(7, 35, 122, 162, IDX_GOLD_HI);
        draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
        lines = draw_wrapped_text_small_box(tx, y, maxw, 3, 10, support_card_name(card_id), IDX_WHITE, IDX_BLACK);
        y += lines * 10 + 7;
        draw_text_small(tx, y, "TYPE", IDX_GOLD_HI, IDX_BLACK); y += 11;
        lines = draw_wrapped_text_small_box(tx, y, maxw, 2, 10, support_card_type(card_id), IDX_WHITE, IDX_BLACK);
        y += lines * 10 + 7;
        draw_text_small(tx, y, "EFFECT", IDX_GOLD_HI, IDX_BLACK); y += 11;
        draw_wrapped_text_small_box(tx, y, maxw, 6, 10, support_card_effect(card_id), IDX_WHITE, IDX_BLACK);
        if (((f / 16) & 1) == 0) draw_text_small(74, 218, "B: BACK", IDX_WHITE, IDX_BLACK);
        return;
    }

    if (!is_monster_card(card_id)) return;
    draw_big_battle_card(card_id, 8, 36, 0);

    draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
    lines = draw_wrapped_text_small_box(tx, y, maxw, 4, 10, waifu_card_names[card_id], IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 6;
    draw_stars_line(tx, y, card_star_count(card_id)); y += 13;

    snprintf(line, sizeof(line), "%s / %s", waifu_card_attr[card_id], waifu_card_tribe[card_id]);
    lines = draw_wrapped_text_small_box(tx, y, maxw, 3, 10, line, IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 7;

    draw_text_small(tx, y, "LORE", IDX_GOLD_HI, IDX_BLACK); y += 11;
    snprintf(line, sizeof(line), "A duel maiden whose %s force shapes the shattered field.", waifu_card_attr[card_id]);
    lines = draw_wrapped_text_small_box(tx, y, maxw, 5, 10, line, IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 7;

    if (y < 194) y = 194;
    draw_preview_stat_line(tx, y, "ATK", (unsigned)waifu_card_atk[card_id]);
    draw_preview_stat_line(tx, y + 12, "DEF", (unsigned)waifu_card_def[card_id]);
    if (((f / 16) & 1) == 0) draw_text_small(74, 218, "B: BACK", IDX_WHITE, IDX_BLACK);
}


static int defender_battle_value(int defender_owner, int slot)
{
    int id;
    int faceup;
    if (slot < 0 || slot >= I_FIELD) return 0;
    if (defender_owner == 0) {
        id = g_i_player_field[slot];
        faceup = g_i_player_faceup[slot];
    } else {
        id = g_i_com_field[slot];
        faceup = g_i_com_faceup[slot];
    }
    if (id < 0) return 0;
    /* Face-down and sideways defense-position monsters use DEF as their battle value. */
    if (!faceup || field_card_defense_position(defender_owner, slot)) return field_card_def(defender_owner, slot);
    return field_card_atk(defender_owner, slot);
}

static void prepare_battle(int attacker_owner, int attacker_slot, int defender_slot)
{
    int atk_id, def_id, delta;
    if (attacker_owner == 0 && player_first_turn_attack_locked()) return;
    g_b_battle_atk_owner = attacker_owner;
    g_b_battle_atk_slot = attacker_slot;
    g_b_battle_def_slot = defender_slot;
    if (attacker_owner == 0) {
        atk_id = g_i_player_field[attacker_slot];
        def_id = g_i_com_field[defender_slot];
        g_b_battle_atk_back = !g_i_player_faceup[attacker_slot];
        g_b_battle_def_back = !g_i_com_faceup[defender_slot];
        g_b_battle_atk_display_atk = field_card_atk(0, attacker_slot);
        g_b_battle_atk_display_def = field_card_def(0, attacker_slot);
        g_b_battle_def_display_atk = field_card_atk(1, defender_slot);
        g_b_battle_def_display_def = field_card_def(1, defender_slot);
        delta = field_card_atk(0, attacker_slot) - defender_battle_value(1, defender_slot);
    } else {
        atk_id = g_i_com_field[attacker_slot];
        def_id = g_i_player_field[defender_slot];
        g_b_battle_atk_back = !g_i_com_faceup[attacker_slot];
        g_b_battle_def_back = !g_i_player_faceup[defender_slot];
        g_b_battle_atk_display_atk = field_card_atk(1, attacker_slot);
        g_b_battle_atk_display_def = field_card_def(1, attacker_slot);
        g_b_battle_def_display_atk = field_card_atk(0, defender_slot);
        g_b_battle_def_display_def = field_card_def(0, defender_slot);
        delta = field_card_atk(1, attacker_slot) - defender_battle_value(0, defender_slot);
    }
    g_b_battle_atk_card = atk_id;
    g_b_battle_def_card = def_id;
    snprintf(g_b_damage_text, sizeof(g_b_damage_text), "%d", delta);
    if (delta > 0) g_b_battle_outcome = BATTLE_DESTROY_DEFENDER;
    else if (delta < 0) g_b_battle_outcome = BATTLE_DESTROY_ATTACKER;
    else g_b_battle_outcome = BATTLE_DESTROY_BOTH;
    set_battle_phase(attacker_owner == 0 ? IB_PLAYER_BATTLE : IB_COM_BATTLE);
}


static void prepare_direct_attack(int attacker_owner, int attacker_slot)
{
    if (attacker_owner == 0 && player_first_turn_attack_locked()) return;

    /* Hard rule: direct attacks are illegal while the opponent controls any
       live monster. Guard this here as well as in the phase logic so neither
       SDL/live input nor command playback can enter an invalid direct attack
       because of stale selection state. */
    if (attacker_owner == 0 && count_live_com_monsters() > 0) {
        int def = first_live_com_slot();
        if (def >= 0) { prepare_battle(0, attacker_slot, def); return; }
    }
    if (attacker_owner == 1 && count_live_player_monsters() > 0) {
        int def = first_live_player_slot();
        if (def >= 0) { prepare_battle(1, attacker_slot, def); return; }
    }
    int atk_id = (attacker_owner == 0) ? g_i_player_field[attacker_slot] : g_i_com_field[attacker_slot];
    int dmg = field_card_atk(attacker_owner, attacker_slot);
    g_b_battle_atk_owner = attacker_owner;
    g_b_battle_atk_slot = attacker_slot;
    g_b_battle_def_slot = -1;
    g_b_battle_atk_card = atk_id;
    g_b_battle_def_card = CARD_NONE;
    g_b_battle_atk_display_atk = field_card_atk(attacker_owner, attacker_slot);
    g_b_battle_atk_display_def = field_card_def(attacker_owner, attacker_slot);
    g_b_battle_def_display_atk = 0;
    g_b_battle_def_display_def = 0;
    g_b_battle_atk_back = attacker_owner == 0 ? !g_i_player_faceup[attacker_slot] : !g_i_com_faceup[attacker_slot];
    g_b_battle_def_back = 0;
    g_b_direct_damage = dmg;
    snprintf(g_b_damage_text, sizeof(g_b_damage_text), "-%d", dmg);
    g_b_battle_outcome = BATTLE_DIRECT_ATTACK;
    set_battle_phase(attacker_owner == 0 ? IB_PLAYER_BATTLE : IB_COM_BATTLE);
}

static void resolve_battle(void)
{
    if (g_b_battle_atk_owner == 0 && g_b_battle_atk_slot >= 0) {
        g_i_player_attacked[g_b_battle_atk_slot] = 1;
        g_i_player_faceup[g_b_battle_atk_slot] = 1;
    }
    if (g_b_battle_atk_owner == 1 && g_b_battle_atk_slot >= 0) {
        g_i_com_attacked[g_b_battle_atk_slot] = 1;
        g_i_com_faceup[g_b_battle_atk_slot] = 1;
    }

    if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) {
        if (g_b_battle_atk_owner == 0) g_com_lp -= g_b_direct_damage;
        else g_you_lp -= g_b_direct_damage;
    } else if (g_b_battle_atk_owner == 0) {
        if (g_b_battle_outcome == BATTLE_DESTROY_DEFENDER) {
            int delta = field_card_atk(0, g_b_battle_atk_slot) - defender_battle_value(1, g_b_battle_def_slot);
            if (delta < 0) delta = 0;
            g_com_lp -= delta;
            g_i_com_field[g_b_battle_def_slot] = CARD_NONE;
            g_i_com_faceup[g_b_battle_def_slot] = 1;
            g_i_com_defense[g_b_battle_def_slot] = 0;
            g_i_com_attacked[g_b_battle_def_slot] = 0;
            g_i_com_atk_bonus[g_b_battle_def_slot] = 0;
            g_i_com_def_bonus[g_b_battle_def_slot] = 0;
        } else if (g_b_battle_outcome == BATTLE_DESTROY_ATTACKER) {
            int delta = defender_battle_value(1, g_b_battle_def_slot) - field_card_atk(0, g_b_battle_atk_slot);
            if (delta < 0) delta = 0;
            g_you_lp -= delta;
            g_i_player_field[g_b_battle_atk_slot] = CARD_NONE;
            g_i_player_faceup[g_b_battle_atk_slot] = 1;
            g_i_player_defense[g_b_battle_atk_slot] = 0;
            g_i_player_attacked[g_b_battle_atk_slot] = 0;
            g_i_player_atk_bonus[g_b_battle_atk_slot] = 0;
            g_i_player_def_bonus[g_b_battle_atk_slot] = 0;
            clear_player_equips_for_target(g_b_battle_atk_slot);
            g_i_com_faceup[g_b_battle_def_slot] = 1;
        } else if (g_b_battle_outcome == BATTLE_DESTROY_BOTH) {
            g_i_player_field[g_b_battle_atk_slot] = CARD_NONE;
            g_i_player_faceup[g_b_battle_atk_slot] = 1;
            g_i_player_defense[g_b_battle_atk_slot] = 0;
            g_i_player_attacked[g_b_battle_atk_slot] = 0;
            g_i_player_atk_bonus[g_b_battle_atk_slot] = 0;
            g_i_player_def_bonus[g_b_battle_atk_slot] = 0;
            clear_player_equips_for_target(g_b_battle_atk_slot);
            g_i_com_field[g_b_battle_def_slot] = CARD_NONE;
            g_i_com_faceup[g_b_battle_def_slot] = 1;
            g_i_com_defense[g_b_battle_def_slot] = 0;
            g_i_com_attacked[g_b_battle_def_slot] = 0;
            g_i_com_atk_bonus[g_b_battle_def_slot] = 0;
            g_i_com_def_bonus[g_b_battle_def_slot] = 0;
        }
    } else {
        if (g_b_battle_outcome == BATTLE_DESTROY_DEFENDER) {
            int delta = field_card_atk(1, g_b_battle_atk_slot) - defender_battle_value(0, g_b_battle_def_slot);
            if (delta < 0) delta = 0;
            g_you_lp -= delta;
            g_i_player_field[g_b_battle_def_slot] = CARD_NONE;
            g_i_player_faceup[g_b_battle_def_slot] = 1;
            g_i_player_defense[g_b_battle_def_slot] = 0;
            g_i_player_attacked[g_b_battle_def_slot] = 0;
            g_i_player_atk_bonus[g_b_battle_def_slot] = 0;
            g_i_player_def_bonus[g_b_battle_def_slot] = 0;
            clear_player_equips_for_target(g_b_battle_def_slot);
        } else if (g_b_battle_outcome == BATTLE_DESTROY_ATTACKER) {
            int delta = defender_battle_value(0, g_b_battle_def_slot) - field_card_atk(1, g_b_battle_atk_slot);
            if (delta < 0) delta = 0;
            g_com_lp -= delta;
            g_i_com_field[g_b_battle_atk_slot] = CARD_NONE;
            g_i_com_faceup[g_b_battle_atk_slot] = 1;
            g_i_com_defense[g_b_battle_atk_slot] = 0;
            g_i_com_attacked[g_b_battle_atk_slot] = 0;
            g_i_com_atk_bonus[g_b_battle_atk_slot] = 0;
            g_i_com_def_bonus[g_b_battle_atk_slot] = 0;
            g_i_player_faceup[g_b_battle_def_slot] = 1;
        } else if (g_b_battle_outcome == BATTLE_DESTROY_BOTH) {
            g_i_com_field[g_b_battle_atk_slot] = CARD_NONE;
            g_i_com_faceup[g_b_battle_atk_slot] = 1;
            g_i_com_defense[g_b_battle_atk_slot] = 0;
            g_i_com_attacked[g_b_battle_atk_slot] = 0;
            g_i_com_atk_bonus[g_b_battle_atk_slot] = 0;
            g_i_com_def_bonus[g_b_battle_atk_slot] = 0;
            g_i_player_field[g_b_battle_def_slot] = CARD_NONE;
            g_i_player_faceup[g_b_battle_def_slot] = 1;
            g_i_player_defense[g_b_battle_def_slot] = 0;
            g_i_player_attacked[g_b_battle_def_slot] = 0;
            g_i_player_atk_bonus[g_b_battle_def_slot] = 0;
            g_i_player_def_bonus[g_b_battle_def_slot] = 0;
            clear_player_equips_for_target(g_b_battle_def_slot);
        }
    }
    if (g_you_lp <= 0) { g_you_lp = 0; g_b_result = -1; set_battle_phase(IB_RESULT); }
    else if (g_com_lp <= 0) { g_com_lp = 0; g_b_result = 1; set_battle_phase(IB_RESULT); }
}

static void draw_direct_attack_event(int f, int atk_id, int atk_col, int atk_row, int atk_back)
{
    int local = f;
    const int flip_dur = 48;
    /* Match the normal monster-battle card lanes. The previous direct-attack
       lanes were shifted inward, so the attacker appeared too far right/left
       before and after the lunge. */
    int ax = (g_b_battle_atk_owner == 0) ? 4 : 132;
    int ay = 36;
    int target_x = (g_b_battle_atk_owner == 0) ? 154 : 28;
    int target_y = 36;
    int card_x = ax;
    clear_screen(IDX_BLACK);
    if (local < 16) {
        Camera cam = side_battle_camera(atk_row);
        render_board(cam);
        draw_interactive_field_cards(cam);
        draw_zone_cursor(cam, atk_col, atk_row);
        draw_hud();
        return;
    }
    local -= 16;
    if (atk_back && local < flip_dur) {
        draw_big_battle_card_flip(atk_id, ax, ay, local, flip_dur);
        return;
    }
    if (atk_back) local -= flip_dur;
    if (local < 24) {
        int32_t e = q8_smooth_ratio(local, 24);
        card_x = lerp_i((g_b_battle_atk_owner == 0) ? -128 : 264, ax, e);
    } else if (local < 64) {
        int32_t t = q8_ratio(local - 24, 40);
        int32_t lunge = (t < Q8_FRAC(62,100)) ? q8_smoothstep(q8_div(t, Q8_FRAC(62,100))) : Q8_ONE - q8_smoothstep(q8_div(t - Q8_FRAC(62,100), Q8_FRAC(38,100)));
        int dir = (g_b_battle_atk_owner == 0) ? 1 : -1;
        card_x = ax + q8_to_int(q8_mul(Q8_FROM_INT(64), lunge)) * dir;
    }
    draw_cutin_battle_card(atk_id, card_x, ay, 0, 1);
    if (local >= 45 && local < 78) draw_direct_attack_slash(target_x, target_y, local - 45, g_b_battle_atk_owner);
    if (local >= 62 && local < 100) draw_centered_damage_text_in_card(target_x - 20, 36, g_b_damage_text);
}

static void draw_interactive_battle(void)
{
    int atk_id, def_id, atk_col, atk_row, def_col, def_row, atk_back, def_back;
    Camera prelude_cam;
    if (g_b_battle_atk_owner == 0) {
        atk_id = g_b_battle_atk_card;
        atk_col = g_b_battle_atk_slot; atk_row = PLAYER_CARD_ROW; atk_back = 0;
        if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) {
            draw_direct_attack_event(g_b_phase_frame, atk_id, atk_col, atk_row, atk_back);
            return;
        }
        def_id = g_b_battle_def_card;
        def_col = g_b_battle_def_slot; def_row = ENEMY_CARD_ROW; def_back = g_b_battle_def_back;
    } else {
        atk_id = g_b_battle_atk_card;
        atk_col = g_b_battle_atk_slot; atk_row = ENEMY_CARD_ROW; atk_back = 0;
        if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) {
            draw_direct_attack_event(g_b_phase_frame, atk_id, atk_col, atk_row, atk_back);
            return;
        }
        def_id = g_b_battle_def_card;
        def_col = g_b_battle_def_slot; def_row = PLAYER_CARD_ROW; def_back = g_b_battle_def_back;
    }

    if (g_b_phase_frame < 16) {
        prelude_cam = side_battle_camera(atk_row);
        render_board(prelude_cam);
        draw_interactive_field_cards(prelude_cam);
        draw_zone_cursor(prelude_cam, atk_col, atk_row);
        draw_hud();
        return;
    }

    /* Start the cut-in proper now. The separate tactical prelude above already
       drew all field cards, so we skip the older pair-only prelude inside the
       scripted cut-in helper. */
    draw_battle_cutin_event_ex(g_b_phase_frame, 0, atk_id, def_id,
                               atk_col, atk_row, atk_back,
                               def_col, def_row, def_back,
                               g_b_damage_text, g_b_battle_outcome);
}

static void draw_post_battle_return(int attacker_owner, int f, int focus_slot, int bottom_card, const char *mode)
{
    const int settle_frames = 20;
    const int fade_frames = 8;
    Camera from = side_battle_camera(attacker_owner == 0 ? PLAYER_CARD_ROW : ENEMY_CARD_ROW);
    Camera to = (attacker_owner == 0) ? battle_top_camera() : enemy_battle_top_camera();
    Camera cam = lerp_camera(from, to, q8_ratio(f, settle_frames));
    render_board(cam);
    draw_interactive_field_cards(cam);
    draw_hud();
    if (attacker_owner == 0 && focus_slot >= 0) draw_bottom_info_field(0, focus_slot, mode ? mode : "FIELD");
    if (focus_slot >= 0) draw_zone_cursor(cam, focus_slot, attacker_owner == 0 ? PLAYER_CARD_ROW : ENEMY_CARD_ROW);
    if (f < fade_frames) apply_black_dither_fade(q8_ratio(f, fade_frames));
}

static void draw_interactive_result(void)
{
    int local = g_b_phase_frame;
    const char *msg = g_b_result < 0 ? "YOU LOSE" : "YOU WIN";
    Camera cam = lerp_camera(battle_top_camera(), player_camera(), q8_smooth_ratio(local, 90));
    render_board(cam);
    draw_interactive_field_cards(cam);
    if (local < 70) {
        int32_t e = q8_smooth_ratio(local, 70);
        draw_hud_offset(-q8_to_int(q8_mul(Q8_FROM_INT(76), e)), 0, q8_to_int(q8_mul(Q8_FROM_INT(92), e)), 0);
        draw_bottom_info_offset(first_live_player_slot() >= 0 ? g_i_player_field[first_live_player_slot()] : hand_ids[0], "RESULT", q8_to_int(q8_mul(Q8_FROM_INT(44), e)));
    }
    if (local >= 50) {
        int32_t e = q8_smooth_ratio(local - 50, 42);
        int scale = (local < 92) ? 2 + (e > Q8_FRAC(55,100) ? 1 : 0) : 3;
        int tw = (int)strlen(msg) * 8 * scale;
        int x = (W - tw) / 2;
        int y = 100 - q8_to_int(q8_mul(Q8_FROM_INT(10), Q8_ONE - e));
        draw_text_scaled(x, y, msg, scale, g_b_result < 0 ? IDX_RED : IDX_GOLD_HI, IDX_BLACK);
    }
}

static void draw_interactive_tally(void)
{
    int won = g_b_result >= 0;
    clear_screen(IDX_BLACK);
    draw_panel_rect(31, 24, 194, 178, IDX_UI_DARK);
    draw_centered_text_scaled(37, won ? "DUEL VICTORY" : "DUEL DEFEAT", 1, won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    hline(45, 210, 56, IDX_UI_LIGHT);
    int score = 0;
    char rank = battle_rank_from_stats(won, won ? g_you_lp : 0, g_b_cards_used, g_b_turns, &score);
    char line[64];
    draw_text(55, 74, "RESULT", IDX_WHITE, IDX_BLACK);
    draw_text(150, 74, won ? "WIN" : "LOSE", won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", won ? g_you_lp : 0); draw_text(55, 94, "LP LEFT", IDX_WHITE, IDX_BLACK); draw_text(160, 94, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", g_b_cards_used); draw_text(55, 112, "CARDS USED", IDX_WHITE, IDX_BLACK); draw_text(176, 112, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", g_i_player_deck_left); draw_text(55, 130, "DECK LEFT", IDX_WHITE, IDX_BLACK); draw_text(176, 130, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "%d", score); draw_text(55, 148, "SCORE", IDX_WHITE, IDX_BLACK); draw_text(152, 148, line, IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "RANK %c", rank); draw_centered_text_scaled(170, line, 2, IDX_GOLD_HI, IDX_BLACK);
    draw_text_small(50, 210, g_story_battle_active ? "RUN: RETURN TO MAP" : "RUN: RETURN TO TITLE", IDX_WHITE, IDX_BLACK);
}

static int draw_replacement_cards_to_hand(void)
{
    int i;
    g_b_draw_count = 0;
    for (i = 0; i < I_HAND; ++i) g_b_draw_slots[i] = -1;
    if (g_i_player_deck_left <= 0) return 0;

    /* Draw into every spent/empty hand slot first.  If the player ended the
       turn with a completely full live hand, the turn draw must still happen:
       slot 0 is discarded/replaced by the drawn card.  This prevents full-hand
       stalling where the deck never advances and deck-out can be avoided. */
    for (i = 0; i < I_HAND; ++i) {
        if (g_i_player_used[i]) {
            if (g_i_player_deck_left <= 0) return g_b_draw_count > 0;
            g_i_player_hand[i] = next_draw_id();
            g_i_player_used[i] = 0;
            --g_i_player_deck_left;
            g_b_draw_slots[g_b_draw_count++] = i;
        }
    }
    if (g_b_draw_count == 0 && g_i_player_deck_left > 0) {
        g_i_player_hand[0] = next_draw_id();
        g_i_player_used[0] = 0;
        --g_i_player_deck_left;
        g_b_draw_slots[g_b_draw_count++] = 0;
    }
    return g_b_draw_count > 0;
}

static void draw_replacement_cards_to_com_hand(void)
{
    int i;
    int drew = 0;
    if (g_i_com_deck_left <= 0) return;
    for (i = 0; i < I_HAND; ++i) {
        if (g_i_com_used[i]) {
            if (g_i_com_deck_left <= 0) return;
            g_i_com_hand[i] = next_com_draw_id();
            g_i_com_used[i] = 0;
            --g_i_com_deck_left;
            drew = 1;
        }
    }
    if (!drew && g_i_com_deck_left > 0) {
        g_i_com_hand[0] = next_com_draw_id();
        g_i_com_used[0] = 0;
        --g_i_com_deck_left;
    }
}

static int is_recent_draw_slot(int slot)
{
    int i;
    for (i = 0; i < g_b_draw_count; ++i) if (g_b_draw_slots[i] == slot) return i;
    return -1;
}

static void draw_player_hand_turn_draw(int f, int selected)
{
    int i;
    int yoff = q8_to_int(q8_mul(Q8_FROM_INT(92), Q8_ONE - q8_smooth_ratio(f, 36)));
    int y = 154 + yoff;
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        int d = is_recent_draw_slot(i);
        if (d >= 0) {
            int32_t t = q8_smooth_ratio(f - 20 - d * 12, 24);
            x = lerp_i(282, x0, t);
        }
        if (g_i_player_used[i]) continue;
        draw_hand_card_sprite_ex(g_i_player_hand[i], x, y, 38, 50, 0,
                                 is_monster_card(g_i_player_hand[i]) && !player_can_place_monster());
        if (f >= 64 && i == selected) draw_red_cursor(x, y, 38, 50);
    }
}

static int current_battle_anim_frames(void)
{
    if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) return DIRECT_ATTACK_ANIM_FRAMES + (g_b_battle_atk_back ? 48 : 0);

    const int slide_dur = 18;
    const int flip_dur = 48;
    const int pause_after_reveal = 10;
    const int ram_dur = 34;
    const int counter_dur = 34;
    int reveal_end = slide_dur + (g_b_battle_atk_back ? flip_dur : 0) + (g_b_battle_def_back ? flip_dur : 0);
    int ram_start = reveal_end + pause_after_reveal;
    int burn_start;

    if (g_b_battle_outcome == BATTLE_DESTROY_ATTACKER) {
        int counter_start = ram_start + ram_dur + 16;
        burn_start = counter_start + counter_dur + 18;
    } else {
        burn_start = ram_start + ram_dur + 30;
        if (g_b_battle_outcome == BATTLE_DESTROY_BOTH) burn_start = ram_start + ram_dur + 22;
    }

    /* +16 accounts for the tactical prelude before draw_battle_cutin_event_ex()
       starts its local cut-in clock. +8 leaves a brief final settle without
       terminating the upward burn wipe mid-effect. */
    return 16 + burn_start + BATTLE_BURN_DUR + 8;
}

static void start_player_equip(int hand_slot, int target_slot)
{
    int equip_slot = first_free_player_equip_slot();
    if (hand_slot < 0 || hand_slot >= I_HAND || target_slot < 0 || target_slot >= I_FIELD) return;
    if (!is_support_card(g_i_player_hand[hand_slot]) || !is_monster_card(g_i_player_field[target_slot])) return;
    if (equip_slot < 0) return;

    g_b_equip_hand = hand_slot;
    g_b_equip_slot = target_slot;
    g_b_equip_zone_slot = equip_slot;
    g_b_equip_card = g_i_player_hand[hand_slot];
    g_b_equip_target_card = g_i_player_field[target_slot];
    g_b_equip_target_faceup = g_i_player_faceup[target_slot];
    g_b_equip_base_atk = field_card_atk(0, target_slot);
    g_b_equip_base_def = field_card_def(0, target_slot);
    g_b_equip_pending_atk = equip_atk_bonus(g_b_equip_card);
    g_b_equip_pending_def = equip_def_bonus(g_b_equip_card);
    g_i_player_field[target_slot] = CARD_NONE;
    g_i_player_faceup[target_slot] = 1;
    g_i_player_used[hand_slot] = 1;
    set_battle_phase(IB_PLAYER_EQUIP_ANIM);
}

static void finish_player_equip(void)
{
    if (g_b_equip_slot < 0 || g_b_equip_slot >= I_FIELD) return;
    g_i_player_field[g_b_equip_slot] = g_b_equip_target_card;
    g_i_player_faceup[g_b_equip_slot] = 1;
    g_i_player_atk_bonus[g_b_equip_slot] += g_b_equip_pending_atk;
    g_i_player_def_bonus[g_b_equip_slot] += g_b_equip_pending_def;
    if (g_b_equip_zone_slot >= 0 && g_b_equip_zone_slot < I_FIELD) {
        g_i_player_equip_field[g_b_equip_zone_slot] = g_b_equip_card;
        g_i_player_equip_target[g_b_equip_zone_slot] = g_b_equip_slot;
    }
    g_b_cards_used++;
    g_b_selected_player_slot = g_b_equip_slot;
    set_top_selector(g_b_equip_slot, PLAYER_CARD_ROW);
    g_b_selected_hand = next_live_hand_index(g_b_equip_hand, 1);
    g_b_equip_hand = -1;
    g_b_equip_slot = -1;
    g_b_equip_zone_slot = -1;
    g_b_equip_card = CARD_NONE;
    g_b_equip_target_card = CARD_NONE;
    set_battle_phase(IB_PLAYER_TOP);
}

static void draw_equip_stat_line_centered(int y, const char *label, int from, int to, int32_t t)
{
    char line[32];
    int value = from + (int)(((to - from) * q8_smoothstep(t) + Q8_HALF) >> Q8_SHIFT);
    snprintf(line, sizeof(line), "%s %04d", label, value);
    draw_text((W - text_px_width(line, 1)) / 2, y, line, stat_delta_color(to - from), IDX_BLACK);
}

static void draw_player_equip_target(void)
{
    Camera cam = battle_top_camera();
    draw_interactive_base(cam);
    draw_top_selector_cursor(cam);
    draw_text_small(70, 191, "SELECT EQUIP TARGET", IDX_GOLD_HI, IDX_BLACK);
    if (top_selector_player_monster_slot() >= 0) draw_bottom_info_top_selector("EQUIP");
    else draw_bottom_info(g_i_player_hand[g_b_equip_hand], "EQUIP");
}

static void draw_player_equip_anim(void)
{
    int f = g_b_phase_frame;
    int32_t t = q8_smooth_ratio(f, 120);
    int reveal = (!g_b_equip_target_faceup && f < 48);
    int32_t merge_t = q8_smooth_ratio(f - 42, 54);
    int32_t vanish_t = q8_smooth_ratio(f - 76, 34);
    int card_w = lerp_i(92, 38, vanish_t);
    int card_h = lerp_i(126, 52, vanish_t);
    int target_x = 68;
    int target_y = 22;
    int target_cx = target_x + 60;
    int target_cy = target_y + 80;
    int card_x = lerp_i(26, target_cx - card_w / 2, merge_t);
    int card_y = lerp_i(36, target_cy - card_h / 2, merge_t);
    int atk_to = g_b_equip_base_atk + g_b_equip_pending_atk;
    int def_to = g_b_equip_base_def + g_b_equip_pending_def;

    clear_screen(IDX_BLACK);
    for (int y = 0; y < H; ++y) {
        uint8_t c = (y & 8) ? IDX_UI_DARK : IDX_BLACK;
        hline(0, 255, y, c);
    }
    if (reveal) {
        draw_big_battle_card_flip(g_b_equip_target_card, target_x, target_y, f, 48);
    } else {
        draw_big_battle_card(g_b_equip_target_card, target_x, target_y, 0);
        if (f < 110) {
            rect_fill(card_x + 5, card_y + 6, card_w, card_h, IDX_BLACK);
            draw_support_sprite(card_x, card_y, card_w, card_h);
        }
    }

    for (int i = 0; i < 18; ++i) {
        int32_t ax = (int32_t)((f + i * 13) * Q8_FRAC(11,100));
        int32_t ay = (int32_t)((f + i * 17) * Q8_FRAC(9,100));
        int cx = target_cx + q8_to_int(q8_mul(q8_sin_rad(ax), Q8_FROM_INT(18 + (i % 5) * 5)));
        int cy = target_cy + q8_to_int(q8_mul(q8_cos_rad(ay), Q8_FROM_INT(18 + (i % 4) * 3)));
        draw_disc(cx, cy, 1 + (i % 3), (i & 1) ? IDX_GREEN : IDX_WHITE);
    }
    if (f >= 92 && f < 108) rect_fill(0, 0, W, H, (f & 2) ? IDX_WHITE : IDX_GOLD_HI);
    if ((f & 4) == 0) rect_outline(target_x - 4, target_y - 4, 128, 168, IDX_WHITE);
    draw_centered_text(9, "EQUIP POWER", IDX_GOLD_HI, IDX_BLACK);
    draw_equip_stat_line_centered(190, "ATK", g_b_equip_base_atk, atk_to, t);
    draw_equip_stat_line_centered(208, "DEF", g_b_equip_base_def, def_to, t);
}

static void step_battle_interactive(const WaifuFmInput *input, int press_up, int press_down, int press_left, int press_right, int press_a, int press_b, int press_start, int press_tab)
{
    int slot, atk_slot, def_slot, view_slot;
    (void)input;

    switch (g_b_phase) {
    case IB_OPENING:
        render_duel_opening_frame(g_b_phase_frame < DUEL_OPENING_END ? g_b_phase_frame : DUEL_OPENING_END - 1);
        if (g_b_phase_frame >= DUEL_OPENING_END) { g_b_player_hand_intro_pending = 1; set_battle_phase(IB_PLAYER_HAND); }
        break;

    case IB_PLAYER_HAND:
        if (press_left) g_b_selected_hand = next_live_hand_index(g_b_selected_hand, -1);
        if (press_right) g_b_selected_hand = next_live_hand_index(g_b_selected_hand, 1);
        if (press_b) { set_battle_phase(IB_CARD_PREVIEW); break; }
        if (press_up) { set_top_selector(g_b_selected_player_slot, PLAYER_CARD_ROW); g_b_attack_attacker_slot = -1; set_battle_phase(IB_PLAYER_TOP); break; }
        if (press_a && !g_i_player_used[g_b_selected_hand]) {
            int selected_card = g_i_player_hand[g_b_selected_hand];
            if (is_support_card(selected_card)) {
                int target = selected_or_first_live_player_slot();
                if (target >= 0 && first_free_player_equip_slot() >= 0) {
                    g_b_equip_hand = g_b_selected_hand;
                    g_b_selected_player_slot = target;
                    set_top_selector(target, PLAYER_CARD_ROW);
                    set_battle_phase(IB_PLAYER_EQUIP_TARGET);
                }
                break;
            }
            slot = first_free_player_slot();
            if (slot >= 0 && player_can_place_monster()) {
                g_b_place_hand = g_b_selected_hand;
                g_b_place_slot = slot;
                g_b_place_card = selected_card;
                set_battle_phase(IB_PLAYER_PLACE);
                break;
            }
        }
        if (press_start) { g_b_attack_attacker_slot = -1; clear_com_attacks(); g_b_com_monster_played_this_turn = 0; set_battle_phase(IB_TURN_TO_COM); break; }
        draw_interactive_base(player_camera());
        draw_interactive_player_hand(g_b_player_hand_intro_pending ? g_b_phase_frame : 999, g_b_selected_hand, 0, 0);
        draw_bottom_info(g_i_player_hand[g_b_selected_hand], "HAND");
        if (g_b_phase_frame >= 48) g_b_player_hand_intro_pending = 0;
        break;

    case IB_CARD_PREVIEW:
        draw_interactive_card_preview(g_i_player_hand[g_b_selected_hand], g_b_phase_frame);
        if (press_b || press_a || press_start) { g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND); }
        break;

    case IB_FIELD_CARD_PREVIEW:
        draw_interactive_card_preview(g_b_preview_card_id, g_b_phase_frame);
        if (press_b || press_a || press_start) { set_battle_phase(IB_PLAYER_TOP); }
        break;

    case IB_PLAYER_PLACE:
        draw_interactive_base(placement_camera());
        draw_zone_cursor(placement_camera(), g_b_place_slot, PLAYER_CARD_ROW);
        draw_flying_card(placement_camera(), g_b_place_card, g_b_place_hand, g_b_place_slot, PLAYER_CARD_ROW, g_b_phase_frame, 0, 48, 2);
        draw_interactive_player_hand(999, g_b_place_hand, q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(g_b_phase_frame, 38))), 1);
        draw_bottom_info(g_b_place_card, "PLACE");
        if (g_b_phase_frame >= 48) {
            g_i_player_field[g_b_place_slot] = g_b_place_card;
            g_i_player_faceup[g_b_place_slot] = 0;
            g_i_player_defense[g_b_place_slot] = 0;
            g_i_player_atk_bonus[g_b_place_slot] = 0;
            g_i_player_def_bonus[g_b_place_slot] = 0;
            g_i_player_used[g_b_place_hand] = 1;
            g_b_player_monster_played_this_turn = 1;
            g_b_cards_used++;
            g_b_selected_player_slot = g_b_place_slot;
            set_top_selector(g_b_place_slot, PLAYER_CARD_ROW);
            set_battle_phase(IB_PLAYER_TOP);
        }
        break;

    case IB_PLAYER_EQUIP_TARGET:
        if (press_b) { g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND); break; }
        if (press_left) move_top_selector(-1, 0);
        if (press_right) move_top_selector(1, 0);
        if (press_up) move_top_selector(0, -1);
        if (press_down) move_top_selector(0, 1);
        if (press_a) {
            int target = top_selector_player_monster_slot();
            if (target >= 0) {
                start_player_equip(g_b_equip_hand, target);
                break;
            }
        }
        draw_player_equip_target();
        break;

    case IB_PLAYER_EQUIP_ANIM:
        draw_player_equip_anim();
        if (g_b_phase_frame >= 120) finish_player_equip();
        break;

    case IB_PLAYER_TOP:
        if (press_left) move_top_selector(-1, 0);
        if (press_right) move_top_selector(1, 0);
        if (press_up) move_top_selector(0, -1);
        if (press_down) {
            if (g_b_top_row < BOARD_ROWS - 1) move_top_selector(0, 1);
            else { g_b_player_hand_intro_pending = 0; g_b_attack_attacker_slot = -1; set_battle_phase(IB_PLAYER_HAND); break; }
        }
        if (press_b) {
            if (g_b_attack_attacker_slot >= 0) {
                /* B cancels attack-target selection and returns to free movement. */
                g_b_attack_attacker_slot = -1;
            } else {
                /* B checks the card under the top-view cursor, mirroring the
                   hand card check. Empty zones are ignored. */
                int preview_id = top_selector_preview_card();
                if (preview_id >= 0) {
                    g_b_preview_card_id = preview_id;
                    set_battle_phase(IB_FIELD_CARD_PREVIEW);
                    break;
                }
            }
        }
        atk_slot = top_selector_player_monster_slot();
        def_slot = top_selector_com_monster_slot();
        if (g_b_attack_attacker_slot >= 0) {
            if (g_i_player_field[g_b_attack_attacker_slot] < 0 || g_i_player_attacked[g_b_attack_attacker_slot]) {
                g_b_attack_attacker_slot = -1;
            } else if (press_a && !player_first_turn_attack_locked()) {
                if (count_live_com_monsters() > 0) {
                    if (def_slot >= 0) {
                        prepare_battle(0, g_b_attack_attacker_slot, def_slot);
                        g_b_attack_attacker_slot = -1;
                        break;
                    }
                } else {
                    prepare_direct_attack(0, g_b_attack_attacker_slot);
                    g_b_attack_attacker_slot = -1;
                    break;
                }
            }
        } else if (press_tab && atk_slot >= 0 && !g_i_player_attacked[atk_slot]) {
            g_i_player_defense[atk_slot] = !g_i_player_defense[atk_slot];
        } else if (press_a && atk_slot >= 0 && !g_i_player_attacked[atk_slot] && !player_first_turn_attack_locked()) {
            g_b_selected_player_slot = atk_slot;
            if (count_live_com_monsters() > 0) {
                g_b_attack_attacker_slot = atk_slot;
                def_slot = first_live_com_slot();
                set_top_selector(def_slot >= 0 ? def_slot : 0, ENEMY_CARD_ROW);
            } else {
                prepare_direct_attack(0, atk_slot);
                break;
            }
        }
        if (press_start) { g_b_attack_attacker_slot = -1; clear_com_attacks(); g_b_com_monster_played_this_turn = 0; set_battle_phase(IB_TURN_TO_COM); break; }
        view_slot = top_selector_player_monster_slot();
        draw_interactive_base(battle_top_camera());
        if (g_b_attack_attacker_slot >= 0) draw_zone_cursor(battle_top_camera(), g_b_attack_attacker_slot, PLAYER_CARD_ROW);
        draw_top_selector_cursor(battle_top_camera());
        if (g_b_attack_attacker_slot >= 0) {
            draw_bottom_info_top_selector("TARGET");
        } else if (view_slot >= 0) {
            draw_bottom_info_field(0, view_slot, player_first_turn_attack_locked() ? "NO ATK" : (g_i_player_attacked[view_slot] ? "USED" : (g_i_player_defense[view_slot] ? "DEF" : "FIELD")));
        } else {
            draw_bottom_info_top_selector(player_first_turn_attack_locked() ? "NO ATK" : "FIELD");
        }
        break;

    case IB_PLAYER_BATTLE:
        draw_interactive_battle();
        if (g_b_phase_frame >= current_battle_anim_frames()) {
            resolve_battle();
            if (g_b_phase == IB_PLAYER_BATTLE) set_battle_phase(IB_PLAYER_RETURN_TOP);
        }
        break;

    case IB_PLAYER_RETURN_TOP:
        atk_slot = selected_or_first_live_player_slot();
        draw_post_battle_return(0, g_b_phase_frame, atk_slot,
                                atk_slot >= 0 ? g_i_player_field[atk_slot] : hand_ids[0],
                                atk_slot >= 0 && g_i_player_attacked[atk_slot] ? "USED" : "FIELD");
        if (g_b_phase_frame >= 24) {
            if (atk_slot >= 0) set_top_selector(atk_slot, PLAYER_CARD_ROW);
            g_b_attack_attacker_slot = -1;
            set_battle_phase(IB_PLAYER_TOP);
        }
        break;

    case IB_TURN_TO_COM:
        draw_interactive_base(turn_camera(g_b_phase_frame, 0, 58, 1));
        if (g_b_phase_frame >= 58) {
            draw_replacement_cards_to_com_hand();
            g_b_selected_com_slot = 0;
            set_battle_phase(IB_COM_SELECT);
        }
        break;

    case IB_COM_SELECT:
        draw_interactive_common(enemy_camera(), first_live_com_slot() >= 0 ? g_i_com_field[first_live_com_slot()] : g_i_com_hand[0], "COM");
        g_b_selected_com_slot = (g_b_phase_frame / 14) % I_HAND;
        draw_interactive_com_hand(g_b_phase_frame, g_b_selected_com_slot, 0);
        if (g_b_phase_frame >= 70) {
            int h = 0;
            while (h < I_HAND && g_i_com_used[h]) h++;
            slot = first_free_com_slot();
            if (h < I_HAND && slot >= 0 && com_can_place_monster()) {
                g_b_place_hand = h;
                g_b_place_slot = slot;
                g_b_place_card = g_i_com_hand[h];
                set_battle_phase(IB_COM_PLACE);
            } else {
                clear_battle_snapshot();
                set_battle_phase(IB_COM_BATTLE);
            }
        }
        break;

    case IB_COM_PLACE:
        draw_interactive_base(enemy_placement_camera());
        draw_zone_cursor(enemy_placement_camera(), g_b_place_slot, ENEMY_CARD_ROW);
        draw_flying_card(enemy_placement_camera(), g_b_place_card, g_b_place_hand, g_b_place_slot, ENEMY_CARD_ROW, g_b_phase_frame, 0, 48, 1);
        draw_interactive_com_hand(999, g_b_place_hand, q8_to_int(q8_mul(Q8_FROM_INT(82), q8_smooth_ratio(g_b_phase_frame, 38))));
        if (g_b_phase_frame >= 48) {
            g_i_com_field[g_b_place_slot] = g_b_place_card;
            g_i_com_faceup[g_b_place_slot] = 0;
            g_i_com_defense[g_b_place_slot] = 0;
            g_i_com_atk_bonus[g_b_place_slot] = 0;
            g_i_com_def_bonus[g_b_place_slot] = 0;
            g_i_com_used[g_b_place_hand] = 1;
            g_b_com_monster_played_this_turn = 1;
            clear_battle_snapshot();
            set_battle_phase(IB_COM_BATTLE);
        }
        break;

    case IB_COM_BATTLE:
        if (g_b_battle_atk_card < 0) {
            atk_slot = first_attackable_com_slot();
            def_slot = first_live_player_slot();
            if (atk_slot >= 0) {
                if (count_live_player_monsters() > 0 && def_slot >= 0) prepare_battle(1, atk_slot, def_slot);
                else if (count_live_player_monsters() == 0) prepare_direct_attack(1, atk_slot);
            } else {
                set_battle_phase(IB_COM_RETURN);
                break;
            }
        }
        draw_interactive_battle();
        if (g_b_phase_frame >= current_battle_anim_frames()) {
            resolve_battle();
            if (g_b_phase == IB_COM_BATTLE) set_battle_phase(IB_COM_RETURN);
        }
        break;

    case IB_COM_RETURN:
        atk_slot = first_live_com_slot();
        draw_post_battle_return(1, g_b_phase_frame, atk_slot,
                                atk_slot >= 0 ? g_i_com_field[atk_slot] : hand_ids[0],
                                "COM");
        if (g_b_phase_frame >= 30) {
            clear_player_attacks();
            g_b_player_monster_played_this_turn = 0;
            g_b_turns++;
            set_battle_phase(IB_TURN_TO_PLAYER);
        }
        break;

    case IB_TURN_TO_PLAYER:
        draw_interactive_base(turn_camera(g_b_phase_frame, 0, 58, 0));
        {
            int yoff = 0;
            if (g_b_phase_frame < 40) {
                yoff = 44;
            } else if (g_b_phase_frame < 58) {
                int t = g_b_phase_frame - 40;
                yoff = q8_to_int(q8_mul(Q8_FROM_INT(44), Q8_ONE - q8_smooth_ratio(t, 18)));
            }
            if (g_b_phase_frame >= 40) {
                int card = first_live_com_slot() >= 0 ? g_i_com_field[first_live_com_slot()] : hand_ids[0];
                draw_bottom_info_offset(card, "TURN", yoff);
            }
        }
        if (g_b_phase_frame >= 58) {
            if (g_i_player_deck_left <= 0) {
                g_b_result = -1;
                set_battle_phase(IB_RESULT);
            } else {
                draw_replacement_cards_to_hand();
                set_battle_phase(IB_PLAYER_DRAW);
            }
        }
        break;

    case IB_PLAYER_DRAW:
        draw_interactive_base(player_camera());
        draw_player_hand_turn_draw(g_b_phase_frame, g_b_selected_hand);
        if (g_b_phase_frame < 64 && g_b_draw_count > 0) draw_text_small(211, 142, "DRAW", IDX_GOLD_HI, IDX_BLACK);
        draw_bottom_info(g_i_player_hand[g_b_selected_hand], "DRAW");
        if (g_b_phase_frame >= 84) { g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND); }
        break;

    case IB_RESULT:
        draw_interactive_result();
        if (g_b_phase_frame >= 170) set_battle_phase(IB_TALLY);
        break;

    case IB_TALLY:
        draw_interactive_tally();
        if (press_start || press_a) {
            if (g_story_battle_active) {
                story_return_to_map_after_duel();
            } else {
                g_i_state = WAIFU_I_TITLE;
                g_i_frame = 0;
                init_battle_state();
            }
        }
        break;
    }

    g_b_frame++;
    g_b_phase_frame++;
}

void waifu_fm_init(void)
{
    if (g_api_initialized) return;
    initDivs();
    CfxRenderer3DConfig cfg = { framebuffer, (DEFAULT_INT)W, (DEFAULT_INT)H };
    cfx_renderer3d_init(&renderer, &cfg);
    cfx_renderer3d_set_texture_atlas(&renderer, waifu_texture_atlas,
                                      WAIFU_TEX_TILE_SIZE,
                                      WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    g_api_initialized = 1;
}

void waifu_fm_reset_interactive(void)
{
    g_i_state = WAIFU_I_TITLE;
    g_i_frame = 0;
    g_i_menu_selected = 1;
    memset(&g_prev_input, 0, sizeof(g_prev_input));
    init_battle_state();
}

uint8_t *waifu_fm_framebuffer(void)
{
    return framebuffer;
}

const uint8_t *waifu_fm_palette_rgb(void)
{
    return waifu_palette_rgb;
}

void waifu_fm_render_scripted_frame(int frame)
{
    /* Legacy visual-reference renderer kept for comparison tooling only.     */
    /* SDL and normal headless playback do not call this.                     */
    waifu_fm_init();
    render_frame(frame);
}

static const char story_name_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static int story_name_char_index(char c)
{
    for (int i = 0; story_name_chars[i]; ++i) if (story_name_chars[i] == c) return i;
    return 0;
}

static void reset_story_entry(void)
{
    memcpy(g_story_name, "SERENA", STORY_NAME_LEN + 1);
    g_story_name_pos = 0;
    g_story_intro_line = 0;
    g_story_fire_line = 0;
    g_story_duel_index = 0;
    g_story_map_cursor = 0;
    g_story_pyramid_cursor = 0;
    g_story_plaza_line = 0;
    g_story_name_to_intro = 0;
    g_story_saved_flash = 0;
    g_story_editor_from_pyramid = 0;
    g_story_save_status = 0;
    generate_story_starter_deck();
    generate_story_storage_pool();
    reset_story_deck_editor();
}

static void draw_egyptian_corner(int x, int y, int flip)
{
    for (int i = 0; i < 18; ++i) {
        int xx = flip ? x - i : x + i;
        hline(xx, xx, y + i / 3, IDX_GOLD_DARK);
        if ((i & 3) == 0) put_px(xx, y + 8 + (i / 2), IDX_GOLD_HI);
    }
}

static void draw_story_name_entry(void)
{
    char buf[32];
    clear_screen(IDX_BLACK);
    for (int y = 0; y < H; ++y) {
        uint8_t c = (y & 8) ? IDX_DARK_BROWN : IDX_BLACK;
        if (y < 36 || y > 204) hline(0, 255, y, c);
    }
    draw_panel_rect(20, 29, 216, 180, IDX_UI_DARK);
    draw_egyptian_corner(31, 42, 0);
    draw_egyptian_corner(224, 42, 1);
    draw_centered_text_scaled(47, "NAME ENTRY", 1, IDX_GOLD_HI, IDX_BLACK);
    draw_centered_text(66, "SCRIBE OF THE NILE", IDX_WHITE, IDX_BLACK);

    rect_fill(46, 92, 164, 38, IDX_BLACK);
    rect_outline(46, 92, 164, 38, IDX_GOLD_HI);
    for (int i = 0; i < STORY_NAME_LEN; ++i) {
        int x = 61 + i * 23;
        char ch[2] = { g_story_name[i], 0 };
        if (i == g_story_name_pos) {
            rect_fill(x - 4, 99, 19, 21, IDX_DARK_BROWN);
            rect_outline(x - 5, 98, 21, 23, IDX_GOLD_HI);
        }
        draw_text(x, 104, ch, i == g_story_name_pos ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
        hline(x - 2, x + 10, 121, IDX_UI_LIGHT);
    }

    snprintf(buf, sizeof(buf), "LETTER %c", g_story_name[g_story_name_pos]);
    draw_centered_text(143, buf, IDX_WHITE, IDX_BLACK);
    draw_text_small(34, 166, "LEFT/RIGHT SLOT", IDX_WHITE, IDX_BLACK);
    draw_text_small(34, 180, "UP/DOWN GLYPH", IDX_WHITE, IDX_BLACK);
    draw_text_small(34, 194, "A NEXT   RUN DREAM", IDX_GOLD_HI, IDX_BLACK);
    if (!g_story_name_to_intro && g_i_frame >= 0 && g_i_frame < 24) apply_black_dither_fade(q8_ratio(g_i_frame, 24));
    if (g_story_name_to_intro) apply_black_dither_fade(Q8_ONE - q8_ratio(g_i_frame, 20));
}

static void draw_blue_gradient_box(int x, int y, int w, int h)
{
    for (int yy = 0; yy < h; ++yy) {
        uint8_t c;
        if (yy < h / 3) c = IDX_UI_BLUE;
        else if (yy < (h * 2) / 3) c = IDX_UI_TEAL;
        else c = IDX_UI_DARK;
        hline(x, x + w - 1, y + yy, c);
    }
    rect_outline(x, y, w, h, IDX_WHITE);
    rect_outline(x + 1, y + 1, w - 2, h - 2, IDX_UI_LIGHT);
}

static const char *story_intro_lines[] = {
    "I remember the gold dust on the market stones.",
    "I remember the cards calling my name from a sealed box.",
    "Every night, the same black river opens under my feet.",
    "Tonight, I dream my first duel again."
};

static void draw_story_intro_screen(int f)
{
    int line_count = (int)(sizeof(story_intro_lines) / sizeof(story_intro_lines[0]));
    int line = g_story_intro_line;
    if (line < 0) line = 0;
    if (line >= line_count) line = line_count - 1;

    clear_screen(IDX_BLACK);
    if ((f & 32) == 0) {
        for (int i = 0; i < 24; ++i) put_px(116 + ((i * 17 + f) & 23), 38 + ((i * 11) & 31), IDX_DIM);
    }
    draw_blue_gradient_box(8, 171, 240, 58);
    draw_text_small(18, 181, "SERENA", IDX_GOLD_HI, IDX_BLACK);
    draw_wrapped_text_small_box(18, 196, 218, 3, 10, story_intro_lines[line], IDX_WHITE, IDX_BLACK);
    if (((f / 18) & 1) == 0) draw_text_small(198, 216, "A/RUN", IDX_WHITE, IDX_BLACK);
    if (f >= 0 && f < 24) apply_black_dither_fade(q8_ratio(f, 24));
}


static const char *story_fire_lines[] = {
    "SERENA... THE DREAM HAS TEETH.",
    "EIGHT GUARDIANS STAND BETWEEN YOU AND THE TRUTH.",
    "BEAT THEM ALL, AND I WILL WAIT FOR YOU IN THE VOID.",
    "YOU ARE GOING TO BURN IN HELL."
};

static uint8_t fire_color_from_intensity(int v)
{
    if (v > 45) return IDX_GOLD_HI;
    if (v > 34) return IDX_FLAME1;
    if (v > 23) return IDX_FLAME2;
    if (v > 13) return IDX_FLAME3;
    if (v > 5) return IDX_RED;
    return IDX_BLACK;
}

static void draw_oldschool_fire(int f)
{
    for (int y = 72; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int wave = q8_to_int(q8_mul(Q8_FROM_INT(10), q8_sin_rad((x + f * 3) * Q8_FRAC(55,1000)))) +
                       q8_to_int(q8_mul(Q8_FROM_INT(7), q8_sin_rad((x * 3 - f * 2) * Q8_FRAC(39,1000))));
            int noise = ((x * 23 + y * 17 + f * 11) ^ ((x + f) * 7) ^ ((y - f) * 13)) & 31;
            int rise = (H - y) / 2;
            int base = (y - 88 + wave) + noise - rise;
            if (base < 0) base = 0;
            put_px(x, y, fire_color_from_intensity(base));
        }
    }
}

static void draw_demon_face(int f)
{
    int cx = 128;
    int cy = 72;
    /* Horns */
    for (int i = 0; i < 28; ++i) {
        line_i(cx - 25, cy - 3, cx - 62 + i / 2, cy - 31 + i, IDX_RED);
        line_i(cx + 25, cy - 3, cx + 62 - i / 2, cy - 31 + i, IDX_RED);
    }
    rect_fill(cx - 36, cy - 10, 72, 47, IDX_BLACK);
    rect_outline(cx - 36, cy - 10, 72, 47, IDX_RED);
    rect_fill(cx - 23, cy + 5, 15, 7, ((f / 8) & 1) ? IDX_FLAME1 : IDX_GOLD_HI);
    rect_fill(cx + 8, cy + 5, 15, 7, ((f / 8) & 1) ? IDX_FLAME1 : IDX_GOLD_HI);
    line_i(cx - 20, cy + 26, cx - 4, cy + 20, IDX_FLAME2);
    line_i(cx - 4, cy + 20, cx + 4, cy + 20, IDX_FLAME2);
    line_i(cx + 4, cy + 20, cx + 20, cy + 26, IDX_FLAME2);
    draw_centered_text(26, "THE DEMON", IDX_RED, IDX_BLACK);
}

static void draw_story_fire_screen(int f)
{
    int line_count = (int)(sizeof(story_fire_lines) / sizeof(story_fire_lines[0]));
    int line = g_story_fire_line;
    if (line < 0) line = 0;
    if (line >= line_count) line = line_count - 1;
    clear_screen(IDX_BLACK);
    draw_oldschool_fire(f);
    draw_demon_face(f);
    draw_panel_rect(8, 172, 240, 57, IDX_UI_DARK);
    draw_text_small(18, 183, "DEMON", IDX_RED, IDX_BLACK);
    draw_wrapped_text_small_box(18, 198, 218, 3, 10, story_fire_lines[line], IDX_WHITE, IDX_BLACK);
    if (((f / 16) & 1) == 0) draw_text_small(197, 216, "A/RUN", IDX_WHITE, IDX_BLACK);
}

static const char *deck_editor_card_name(int id)
{
    return is_support_card(id) ? support_card_name(id) : waifu_card_names[id];
}

static void draw_deck_editor_icon(int card, int x, int y, int selected)
{
    draw_hand_card_sprite(card, x, y, 26, 34, 0);
    if (selected) {
        rect_outline(x - 2, y - 2, 30, 38, IDX_RED);
        rect_outline(x - 3, y - 3, 32, 40, IDX_UI_RED);
    }
}

static void draw_deck_editor(void)
{
    char line[96];
    int count = deck_editor_active_count();
    int *arr = deck_editor_active_array();
    int scroll = g_deck_scroll[g_deck_tab];
    int selected_card = (count > 0 && g_deck_cursor < count) ? arr[g_deck_cursor] : CARD_NONE;

    clear_screen(IDX_BLACK);
    for (int y = 0; y < H; ++y) {
        uint8_t c = (y < 28) ? IDX_DARK_BROWN : ((y & 8) ? IDX_UI_DARK : IDX_BLACK);
        hline(0, 255, y, c);
    }
    draw_panel_rect(4, 4, 248, 232, IDX_UI_DARK);
    draw_centered_text(12, "DECK EDITOR", IDX_GOLD_HI, IDX_BLACK);

    rect_fill(14, 27, 102, 14, g_deck_tab == 0 ? IDX_GOLD_DARK : IDX_BLACK);
    rect_outline(14, 27, 102, 14, g_deck_tab == 0 ? IDX_GOLD_HI : IDX_DIM);
    snprintf(line, sizeof(line), "DECK %02d/40", g_story_deck_count);
    draw_text_small(22, 31, line, g_deck_tab == 0 ? IDX_WHITE : IDX_DIM, IDX_BLACK);

    rect_fill(140, 27, 102, 14, g_deck_tab == 1 ? IDX_GOLD_DARK : IDX_BLACK);
    rect_outline(140, 27, 102, 14, g_deck_tab == 1 ? IDX_GOLD_HI : IDX_DIM);
    snprintf(line, sizeof(line), "STORAGE %02d", g_story_storage_count);
    draw_text_small(148, 31, line, g_deck_tab == 1 ? IDX_WHITE : IDX_DIM, IDX_BLACK);

    if (count <= 0) {
        draw_centered_text(105, "EMPTY", IDX_DIM, IDX_BLACK);
    } else {
        for (int i = 0; i < DECK_VISIBLE_CARDS; ++i) {
            int idx = scroll + i;
            if (idx >= count) break;
            int col = i % DECK_GRID_COLS;
            int row = i / DECK_GRID_COLS;
            int x = 14 + col * 40;
            int y = 50 + row * 45;
            draw_deck_editor_icon(arr[idx], x, y, idx == g_deck_cursor);
        }
    }

    rect_fill(9, 190, 238, 36, IDX_BLACK);
    rect_outline(9, 190, 238, 36, IDX_UI_LIGHT);
    if (selected_card >= 0) {
        draw_text_small_ellipsis(15, 196, deck_editor_card_name(selected_card), 25, IDX_WHITE, IDX_BLACK);
        if (is_support_card(selected_card)) {
            draw_text_small_ellipsis(15, 208, support_card_type(selected_card), 23, IDX_GOLD_HI, IDX_BLACK);
        } else {
            snprintf(line, sizeof(line), "ATK %u DEF %u", (unsigned)waifu_card_atk[selected_card], (unsigned)waifu_card_def[selected_card]);
            draw_text_small(15, 208, line, IDX_GOLD_HI, IDX_BLACK);
        }
    }
    if (g_deck_flash > 0 && ((g_deck_flash / 8) & 1) == 0) {
        const char *msg = g_deck_flash_reason == 1 ? "MAX 4 COPIES" :
            (g_story_deck_count == STORY_DECK_SIZE ? "DECK IS FULL" : "DECK MUST BE 40");
        draw_centered_text(181, msg, IDX_RED, IDX_BLACK);
    }
    draw_text_small(15, 226, "A MOVE  B CHECK  BTN4 TAB", IDX_WHITE, IDX_BLACK);
}


static Camera story_map_camera(int f)
{
    int32_t a = -Q8_FRAC(38,100) + q8_mul(Q8_FRAC(10,100), q8_sin_rad(f * Q8_FRAC(25,1000)));
    return make_camera(v3(q8_mul(q8_sin_rad(a), Q8_FRAC(56,10)), Q8_FRAC(275,100), q8_mul(q8_cos_rad(a), Q8_FRAC(56,10))),
                       v3(0, Q8_FRAC(55,100), 0), v3(0,Q8_ONE,0), Q8_FROM_INT(132));
}

/* PS1-style scanline floor renderer: inverse-projects each scanline to the
   floor plane, derives UVs from world XZ, and tiles the texture with modulo
   wrapping.  All math is Q8.8 fixed point and 32-bit integer only. */
static void draw_floor_tiled(Camera cam, int32_t floor_y, int tile_a, int tile_b, int32_t tile_size)
{
    Vec3 ffwd = vnorm(vsub(cam.target, cam.eye));
    Vec3 fright = vnorm(vcross(ffwd, cam.up));
    Vec3 fup = vcross(fright, ffwd);

    int tw = WAIFU_TEX_TILE_SIZE;
    const uint8_t *atlas = waifu_texture_atlas;

    for (int y = 0; y < H; ++y) {
        int32_t dy = q8_div(Q8_FROM_INT(H / 2 - y) - Q8_HALF, cam.focal);
        int32_t ray_y = q8_mul(fup.y, dy) + ffwd.y;
        if (ray_y >= 0) continue;
        int32_t t = q8_div(floor_y - cam.eye.y, ray_y);
        if (t <= Q8_FRAC(1,100)) continue;

        int32_t dx_l = q8_div(-Q8_FROM_INT(W / 2), cam.focal);
        int32_t dx_r = q8_div(Q8_FROM_INT(W - 1 - W / 2), cam.focal);

        int32_t wl_x = cam.eye.x + q8_mul(t, q8_mul(fright.x, dx_l) + ffwd.x);
        int32_t wl_z = cam.eye.z + q8_mul(t, q8_mul(fright.z, dx_l) + ffwd.z);
        int32_t wr_x = cam.eye.x + q8_mul(t, q8_mul(fright.x, dx_r) + ffwd.x);
        int32_t wr_z = cam.eye.z + q8_mul(t, q8_mul(fright.z, dx_r) + ffwd.z);

        int32_t u = q16_div_q8(wl_x, tile_size);
        int32_t v = q16_div_q8(wl_z, tile_size);
        int32_t du = q16_div_q8(wr_x - wl_x, tile_size) / (W - 1);
        int32_t dv = q16_div_q8(wr_z - wl_z, tile_size) / (W - 1);

        for (int x = 0; x < W; ++x) {
            int iu = q16_floor_to_int(u);
            int iv = q16_floor_to_int(v);
            int fu = (int)(u - (int32_t)iu * 65536);
            int fv = (int)(v - (int32_t)iv * 65536);
            int tile = ((iu + iv) & 1) ? tile_b : tile_a;
            const uint8_t *src = atlas + (size_t)tile * tw * tw;
            int sx = (fu * (tw - 1) + 0x8000) >> 16;
            int sy = (fv * (tw - 1) + 0x8000) >> 16;
            put_px(x, y, src[sy * tw + sx]);
            u += du;
            v += dv;
        }
    }
}

static void draw_map_pyramid_3d(int f)
{
    Camera cam = story_map_camera(f);
    Vec3 apex = v3(0, Q8_FRAC(215,100), 0);
    Vec3 a = v3(-Q8_FRAC(175,100), 0, -Q8_FRAC(175,100));
    Vec3 b = v3( Q8_FRAC(175,100), 0, -Q8_FRAC(175,100));
    Vec3 c = v3( Q8_FRAC(175,100), 0,  Q8_FRAC(175,100));
    Vec3 d = v3(-Q8_FRAC(175,100), 0,  Q8_FRAC(175,100));
    typedef struct PyramidFace {
        Vec3 p0, p1;
        int tile;
        int flip;
        int32_t depth;
    } PyramidFace;
    PyramidFace faces[4] = {
        {a, b, 1, 0, 0},
        {b, c, 1, 1, 0},
        {c, d, 1, 0, 0},
        {d, a, 1, 1, 0}
    };
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 1, 5, Q8_FRAC(176,100));

    for (int i = 0; i < 4; ++i) {
        ScreenPt p0 = project_point(cam, faces[i].p0);
        ScreenPt p1 = project_point(cam, faces[i].p1);
        ScreenPt p2 = project_point(cam, apex);
        faces[i].depth = (p0.depth + p1.depth + p2.depth) / 3;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            if (faces[i].depth < faces[j].depth) {
                PyramidFace tmp = faces[i];
                faces[i] = faces[j];
                faces[j] = tmp;
            }
        }
    }
    for (int i = 0; i < 4; ++i) draw_tri3d_pyramid_face(cam, faces[i].p0, faces[i].p1, apex, faces[i].tile, faces[i].flip, 5, 3);

    ScreenPt peak = project_point(cam, apex);
    if (peak.ok) put_px(peak.x, peak.y, IDX_GOLD_HI);
}

/* Story scene kinds: which 3D environment to show on the map screen. */
typedef enum {
    STORY_SCENE_DESERT = 0,
    STORY_SCENE_TEMPLE,
    STORY_SCENE_VOLCANO,
    STORY_SCENE_VOID
} StorySceneKind;

static StorySceneKind story_scene_kind(void)
{
    if (g_story_duel_index >= 7) return STORY_SCENE_VOID;
    if (g_story_duel_index >= 5) return STORY_SCENE_VOLCANO;
    if (g_story_duel_index >= 2) return STORY_SCENE_TEMPLE;
    return STORY_SCENE_DESERT;
}

static Camera story_temple_camera(int f)
{
    int32_t a = -Q8_FRAC(30,100) + q8_mul(Q8_FRAC(8,100), q8_sin_rad(f * Q8_FRAC(22,1000)));
    return make_camera(v3(q8_mul(q8_sin_rad(a), Q8_FRAC(52,10)), Q8_FRAC(24,10), q8_mul(q8_cos_rad(a), Q8_FRAC(52,10))),
                       v3(0, Q8_FRAC(7,10), 0), v3(0,Q8_ONE,0), Q8_FROM_INT(132));
}

static void draw_map_temple_3d(int f)
{
    Camera cam = story_temple_camera(f);
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 3, 3, Q8_FRAC(16,10));
    /* Temple pillars: four rows of columns forming a corridor. */
    int32_t pillar_y0 = 0, pillar_y1 = Q8_FRAC(26,10);
    for (int row = 0; row < 3; ++row) {
        int32_t pz = -Q8_FRAC(25,10) + Q8_FROM_INT(row * 2);
        for (int side = 0; side < 2; ++side) {
            int32_t px = side ? Q8_FRAC(22,10) : -Q8_FRAC(22,10);
            Vec3 b0 = v3(px - Q8_FRAC(35,100), pillar_y0, pz - Q8_FRAC(35,100));
            Vec3 b1 = v3(px + Q8_FRAC(35,100), pillar_y0, pz - Q8_FRAC(35,100));
            Vec3 b2 = v3(px + Q8_FRAC(35,100), pillar_y0, pz + Q8_FRAC(35,100));
            Vec3 b3 = v3(px - Q8_FRAC(35,100), pillar_y0, pz + Q8_FRAC(35,100));
            Vec3 t0 = v3(px - Q8_FRAC(35,100), pillar_y1, pz - Q8_FRAC(35,100));
            Vec3 t1 = v3(px + Q8_FRAC(35,100), pillar_y1, pz - Q8_FRAC(35,100));
            Vec3 t2 = v3(px + Q8_FRAC(35,100), pillar_y1, pz + Q8_FRAC(35,100));
            Vec3 t3 = v3(px - Q8_FRAC(35,100), pillar_y1, pz + Q8_FRAC(35,100));
            /* Front and back faces */
            draw_quad3d_safe(cam, b0, b1, t1, t0, 3);
            draw_quad3d_safe(cam, b3, b2, t2, t3, 3);
            draw_quad3d_safe(cam, b1, b2, t2, t1, 3);
            draw_quad3d_safe(cam, b0, b3, t3, t0, 3);
            /* Capital top */
            draw_quad3d_safe(cam, t0, t1, t2, t3, 3);
        }
    }
}

static Camera story_volcano_camera(int f)
{
    int32_t a = -Q8_FRAC(35,100) + q8_mul(Q8_FRAC(9,100), q8_sin_rad(f * Q8_FRAC(24,1000)));
    return make_camera(v3(q8_mul(q8_sin_rad(a), Q8_FRAC(58,10)), Q8_FRAC(29,10), q8_mul(q8_cos_rad(a), Q8_FRAC(58,10))),
                       v3(0, Q8_FRAC(5,10), 0), v3(0,Q8_ONE,0), Q8_FROM_INT(132));
}

static void draw_map_volcano_3d(int f)
{
    Camera cam = story_volcano_camera(f);
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 5, 5, Q8_FRAC(176,100));
    /* Volcano cone: steep triangular faces like the pyramid but wider and
       darker, with a glowing crater rim. */
    Vec3 apex_v = v3(0, Q8_FRAC(32,10), 0);
    Vec3 a = v3(-Q8_FRAC(26,10), 0, -Q8_FRAC(26,10));
    Vec3 b = v3( Q8_FRAC(26,10), 0, -Q8_FRAC(26,10));
    Vec3 c = v3( Q8_FRAC(26,10), 0,  Q8_FRAC(26,10));
    Vec3 d = v3(-Q8_FRAC(26,10), 0,  Q8_FRAC(26,10));
    typedef struct { Vec3 p0, p1; int flip; int32_t depth; } VFace;
    VFace vf[4] = {
        {a, b, 0, 0}, {b, c, 1, 0}, {c, d, 0, 0}, {d, a, 1, 0}
    };
    for (int i = 0; i < 4; ++i) {
        ScreenPt p0 = project_point(cam, vf[i].p0);
        ScreenPt p1 = project_point(cam, vf[i].p1);
        ScreenPt p2 = project_point(cam, apex_v);
        vf[i].depth = (p0.depth + p1.depth + p2.depth) / 3;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (vf[i].depth < vf[j].depth) { VFace t = vf[i]; vf[i] = vf[j]; vf[j] = t; }
    for (int i = 0; i < 4; ++i)
        draw_tri3d_pyramid_face(cam, vf[i].p0, vf[i].p1, apex_v, 5, vf[i].flip, 6, 4);
    /* Glowing crater: pulsing red/orange circle at the peak. */
    ScreenPt peak = project_point(cam, apex_v);
    if (peak.ok) {
        int pulse = 2 + ((f / 8) & 3);
        for (int dy = -pulse; dy <= pulse; ++dy)
            for (int dx = -pulse; dx <= pulse; ++dx)
                if (dx*dx + dy*dy <= pulse*pulse)
                    put_px(peak.x + dx, peak.y + dy, ((f/4)&1) ? IDX_FLAME1 : IDX_RED);
    }
}

static Camera story_void_camera(int f)
{
    int32_t a = -Q8_FRAC(25,100) + q8_mul(Q8_FRAC(6,100), q8_sin_rad(f * Q8_FRAC(20,1000)));
    return make_camera(v3(q8_mul(q8_sin_rad(a), Q8_FRAC(48,10)), Q8_FRAC(22,10), q8_mul(q8_cos_rad(a), Q8_FRAC(48,10))),
                       v3(0, Q8_FRAC(8,10), 0), v3(0,Q8_ONE,0), Q8_FROM_INT(128));
}

static void draw_map_void_3d(int f)
{
    Camera cam = story_void_camera(f);
    /* Void background: deep black with stars (drawn by sky function). */
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 0, 0, Q8_FRAC(16,10));
    /* Floating crystals: small rotating diamonds hovering above the platform. */
    for (int ci = 0; ci < 5; ++ci) {
        int32_t ang = f * Q8_FRAC(3,100) + ci * Q8_FRAC(126,100);
        int32_t cx = q8_mul(q8_sin_rad(ang), Q8_FRAC(25,10));
        int32_t cz = q8_mul(q8_cos_rad(ang), Q8_FRAC(25,10));
        int32_t cy = Q8_FRAC(16,10) + q8_mul(Q8_FRAC(4,10), q8_sin_rad(f * Q8_FRAC(5,100) + Q8_FROM_INT(ci)));
        int32_t s = Q8_FRAC(35,100);
        Vec3 top = v3(cx, cy + s, cz);
        Vec3 bot = v3(cx, cy - s, cz);
        Vec3 lft = v3(cx - s, cy, cz);
        Vec3 rgt = v3(cx + s, cy, cz);
        Vec3 fwd = v3(cx, cy, cz - s);
        Vec3 bck = v3(cx, cy, cz + s);
        uint8_t cc = (ci & 1) ? IDX_UI_BLUE : IDX_FLAME1;
        ScreenPt st = project_point(cam, top), sb = project_point(cam, bot);
        ScreenPt sl = project_point(cam, lft), sr = project_point(cam, rgt);
        ScreenPt sf = project_point(cam, fwd), sk = project_point(cam, bck);
        if (st.ok && sl.ok && sr.ok) {
            line_i(sl.x, sl.y, st.x, st.y, cc); line_i(sr.x, sr.y, st.x, st.y, cc);
            line_i(sl.x, sl.y, sb.x, sb.y, cc); line_i(sr.x, sr.y, sb.x, sb.y, cc);
        }
        if (st.ok && sf.ok && sk.ok) {
            line_i(sf.x, sf.y, st.x, st.y, cc); line_i(sk.x, sk.y, st.x, st.y, cc);
            line_i(sf.x, sf.y, sb.x, sb.y, cc); line_i(sk.x, sk.y, sb.x, sb.y, cc);
        }
    }
}

static void draw_desert_sky(void)
{
    for (int y = 0; y < H; ++y) {
        uint8_t c = y < 52 ? IDX_UI_BLUE : (y < 93 ? IDX_UI_TEAL : (y < 143 ? IDX_GOLD_DARK : IDX_DARK_BROWN));
        hline(0, 255, y, c);
    }
    for (int x = 0; x < W; x += 6) {
        int yy = 142 + ((x * 13) & 7);
        hline(x, x + 5 < 255 ? x + 5 : 255, yy, IDX_GOLD_HI);
    }
}

static void draw_temple_sky(void)
{
    for (int y = 0; y < H; ++y) {
        uint8_t c = y < 40 ? IDX_UI_BLUE : (y < 80 ? IDX_UI_TEAL : (y < 120 ? IDX_DIM : IDX_DARK_BROWN));
        hline(0, 255, y, c);
    }
}

static void draw_volcano_sky(void)
{
    for (int y = 0; y < H; ++y) {
        uint8_t c = y < 45 ? IDX_BLACK : (y < 85 ? IDX_RED : (y < 120 ? IDX_FLAME3 : IDX_DARK_BROWN));
        hline(0, 255, y, c);
    }
    /* Embers drifting upward. */
    for (int i = 0; i < 40; ++i) {
        int x = (i * 53 + 17) & 255;
        int y = 120 - ((i * 31 + 7) & 63);
        put_px(x, y, (i & 1) ? IDX_FLAME1 : IDX_GOLD_HI);
    }
}

static void draw_void_sky(void)
{
    for (int y = 0; y < H; ++y) hline(0, 255, y, IDX_BLACK);
    /* Stars. */
    for (int i = 0; i < 60; ++i) {
        int x = (i * 67 + 13) & 255;
        int y = (i * 41 + 5) & 127;
        put_px(x, y, (i & 3) ? IDX_DIM : IDX_WHITE);
    }
}

static void draw_story_sky(void)
{
    switch (story_scene_kind()) {
    case STORY_SCENE_TEMPLE:  draw_temple_sky();  break;
    case STORY_SCENE_VOLCANO: draw_volcano_sky(); break;
    case STORY_SCENE_VOID:    draw_void_sky();    break;
    default:                  draw_desert_sky();  break;
    }
}

static void draw_story_scene_3d(int f)
{
    switch (story_scene_kind()) {
    case STORY_SCENE_TEMPLE:  draw_map_temple_3d(f);  break;
    case STORY_SCENE_VOLCANO: draw_map_volcano_3d(f); break;
    case STORY_SCENE_VOID:    draw_map_void_3d(f);    break;
    default:                  draw_map_pyramid_3d(f); break;
    }
}

static const char *story_scene_name(void)
{
    switch (story_scene_kind()) {
    case STORY_SCENE_TEMPLE:  return "STONE TEMPLE";
    case STORY_SCENE_VOLCANO: return "EMBER CRATER";
    case STORY_SCENE_VOID:    return "THE VOID";
    default:                  return "DESERT ROAD";
    }
}

static void draw_story_map_screen_content(int f)
{
    char line[96];
    draw_story_sky();
    draw_story_scene_3d(f);
    draw_panel_rect(6, 6, 105, 35, IDX_UI_DARK);
    draw_text_small(12, 13, story_scene_name(), IDX_GOLD_HI, IDX_BLACK);
    snprintf(line, sizeof(line), "DUEL %d/%d", g_story_duel_index + 1, STORY_MAX_DUELS);
    draw_text_small(12, 27, line, IDX_WHITE, IDX_BLACK);

    draw_panel_rect(126, 146, 121, 76, IDX_UI_DARK);
    draw_text_small(135, 155, "DESTINATION", IDX_GOLD_HI, IDX_BLACK);
    draw_text(143, 174, "SANCTUM", g_story_map_cursor == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(143, 194, "BATTLE", g_story_map_cursor == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    if (g_story_map_cursor == 0) draw_text(132, 174, ">", IDX_RED, IDX_BLACK);
    else draw_text(132, 194, ">", IDX_RED, IDX_BLACK);

    draw_panel_rect(8, 181, 108, 41, IDX_UI_DARK);
    if (g_story_duel_index >= STORY_MAX_DUELS - 1) {
        draw_wrapped_text_small_box(16, 190, 91, 3, 9, "The demon waits in the void. This is the final duel.", IDX_WHITE, IDX_BLACK);
    } else if (story_opponent_is_boss()) {
        snprintf(line, sizeof(line), "A boss: %s. Prepare well.", story_opponent_name());
        draw_wrapped_text_small_box(16, 190, 91, 3, 9, line, IDX_RED, IDX_BLACK);
    } else {
        snprintf(line, sizeof(line), "Next: %s awaits.", story_opponent_name());
        draw_wrapped_text_small_box(16, 190, 91, 3, 9, line, IDX_WHITE, IDX_BLACK);
    }
    draw_text_small(11, 226, "A/RUN SELECT", IDX_WHITE, IDX_BLACK);
}

static void draw_story_map_screen(int f)
{
    draw_story_map_screen_content(f);
    if (f >= 0 && f < 24) apply_black_dither_fade(q8_ratio(f, 24));
}static void draw_story_plaza_scene_content(void);

static void draw_story_to_plaza_transition(int f)
{
    SCREEN_TRANSITION(f, 24, draw_story_map_screen_content(f), draw_story_plaza_scene_content());
}

static void draw_story_fire_to_deck_transition(int f)
{
    SCREEN_TRANSITION(f, 24, draw_story_fire_screen(g_story_fire_line), draw_deck_editor());
}

static void draw_story_pyramid_menu(void)
{
    clear_screen(IDX_BLACK);
    draw_story_sky();
    draw_story_scene_3d(g_i_frame);
    draw_panel_rect(34, 46, 188, 130, IDX_UI_DARK);
    draw_centered_text(57, "SANCTUM", IDX_GOLD_HI, IDX_BLACK);
    draw_wrapped_text_small_box(48, 75, 158, 3, 10, "A place of rest. Serena can prepare before the next duel.", IDX_WHITE, IDX_BLACK);
    draw_text(72, 114, "SAVE", g_story_pyramid_cursor == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(72, 134, "DECK EDITOR", g_story_pyramid_cursor == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(72, 154, "BACK", g_story_pyramid_cursor == 2 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(58, 114 + g_story_pyramid_cursor * 20, ">", IDX_RED, IDX_BLACK);
    draw_text_small(49, 202, "A/RUN SELECT   B BACK", IDX_WHITE, IDX_BLACK);
}

static void draw_story_save_screen(void)
{
    clear_screen(IDX_BLACK);
    draw_story_sky();
    draw_story_scene_3d(g_i_frame);
    draw_panel_rect(31, 78, 194, 82, IDX_UI_DARK);
    if (g_story_save_status < 0) {
        draw_centered_text(95, "SAVE FAILED", IDX_RED, IDX_BLACK);
        draw_centered_text(118, "The memory seal is broken.", IDX_WHITE, IDX_BLACK);
    } else if (g_story_save_status > 0) {
        draw_centered_text(95, "PROGRESS SAVED", IDX_GOLD_HI, IDX_BLACK);
        draw_wrapped_text_small_box(57, 115, 143, 2, 10, "The sanctum remembers Serena.", IDX_WHITE, IDX_BLACK);
    } else {
        draw_centered_text(95, "NO SAVE DATA", IDX_RED, IDX_BLACK);
        draw_centered_text(118, "Nothing is written yet.", IDX_WHITE, IDX_BLACK);
    }
    if (((g_i_frame / 16) & 1) == 0) draw_centered_text(143, "A/RUN/B BACK", IDX_WHITE, IDX_BLACK);
}

static const char *story_battle_intro_lines(void)
{
    switch (g_story_duel_index) {
    case 0: return "The dream shifts. A shade rises from the sand.";
    case 1: return "The plaza bustles, but one duelist blocks the path.";
    case 2: return "Stone columns tower above. An adept tests Serena.";
    case 3: return "The sandstorm parts. A reaver grins behind her veil.";
    case 4: return "The crater glows. A burning soul rises from the lava.";
    case 5: return "Reality fractures. Something walks the void between dreams.";
    case 6: return "The ancient guardian awakens. The sphinx does not blink.";
    default: return "THE DEMON. It remembers Serena. This ends now.";
    }
}

static void draw_story_plaza_scene_content(void)
{
    int line = g_story_plaza_line;
    if (line < 0) line = 0;
    if (line > 2) line = 2;
    clear_screen(IDX_BLACK);
    draw_story_sky();
    draw_story_scene_3d(g_i_frame);
    draw_centered_text(35, story_scene_name(), IDX_GOLD_HI, IDX_BLACK);
    draw_panel_rect(8, 171, 240, 58, IDX_UI_DARK);
    draw_text_small(18, 181, "SERENA", IDX_GOLD_HI, IDX_BLACK);
    if (line == 0) {
        draw_wrapped_text_small_box(18, 196, 218, 3, 10, story_battle_intro_lines(), IDX_WHITE, IDX_BLACK);
    } else if (line == 1) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s stands ready. %sLP: %d.", story_opponent_name(),
                 story_opponent_is_boss() ? "BOSS " : "", story_opponent_is_boss() ? 10000 : 8000);
        draw_wrapped_text_small_box(18, 196, 218, 3, 10, buf, story_opponent_is_boss() ? IDX_RED : IDX_WHITE, IDX_BLACK);
    } else {
        draw_wrapped_text_small_box(18, 196, 218, 3, 10, "Serena raises her deck. The duel begins.", IDX_WHITE, IDX_BLACK);
    }
    if (((g_i_frame / 18) & 1) == 0) draw_text_small(198, 216, "A/RUN", IDX_WHITE, IDX_BLACK);
}

static void draw_story_plaza_scene(void)
{
    draw_story_plaza_scene_content();
    if (g_i_frame >= 0 && g_i_frame < 24) apply_black_dither_fade(q8_ratio(g_i_frame, 24));
}

static void story_return_to_map_after_duel(void)
{
    if (g_b_result >= 0) {
        award_story_win_drop();
        if (g_story_duel_index < STORY_MAX_DUELS - 1) ++g_story_duel_index;
    }
    g_story_battle_active = 0;
    g_story_map_cursor = 1;
    g_i_state = WAIFU_I_STORY_MAP;
    g_i_frame = -1;
    init_battle_state();
}


void waifu_fm_step(const WaifuFmInput *input)
{
    WaifuFmInput zero;
    int press_up, press_down, press_left, press_right, press_a, press_b, press_start, press_tab;

    waifu_fm_init();
    memset(&zero, 0, sizeof(zero));
    if (!input) input = &zero;

    press_up = input_pressed(input->up, g_prev_input.up);
    press_down = input_pressed(input->down, g_prev_input.down);
    press_left = input_pressed(input->left, g_prev_input.left);
    press_right = input_pressed(input->right, g_prev_input.right);
    press_a = input_pressed(input->a, g_prev_input.a);
    press_b = input_pressed(input->b, g_prev_input.b);
    press_start = input_pressed(input->start, g_prev_input.start);
    press_tab = input_pressed(input->tab, g_prev_input.tab);

    switch (g_i_state) {
    case WAIFU_I_TITLE:
        clear_screen(IDX_BLACK);
        draw_title_background();
        draw_title_logo();
        draw_title_prompt(g_i_frame);
        if (g_i_frame < 30) apply_black_dither_fade(q8_ratio(g_i_frame, 30));
        if (press_b) {
            load_story_to_map();
        } else if (press_start || press_a) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_MENU:
        if (press_up) g_i_menu_selected = (g_i_menu_selected + 2) % 3;
        if (press_down) g_i_menu_selected = (g_i_menu_selected + 1) % 3;
        draw_menu_screen(g_i_menu_selected);
        if (press_start || press_a) {
            if (g_i_menu_selected == 0) {
                reset_story_entry();
                g_i_state = WAIFU_I_STORY_NAME;
                g_i_frame = -1;
            } else if (g_i_menu_selected == 1) {
                g_i_state = WAIFU_I_BATTLE;
                init_battle_state();
                g_i_frame = -1;
            } else {
                if (!load_story_to_map()) g_deck_flash = 60;
            }
        }
        break;

    case WAIFU_I_STORY_NAME:
        g_story_name_to_intro = 0;
        if (press_left) g_story_name_pos = (g_story_name_pos + STORY_NAME_LEN - 1) % STORY_NAME_LEN;
        if (press_right || press_a) g_story_name_pos = (g_story_name_pos + 1) % STORY_NAME_LEN;
        if (press_up || press_down) {
            int idx = story_name_char_index(g_story_name[g_story_name_pos]);
            int count = (int)strlen(story_name_chars);
            idx = (idx + (press_up ? 1 : count - 1)) % count;
            g_story_name[g_story_name_pos] = story_name_chars[idx];
        }
        draw_story_name_entry();
        if (press_b) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        } else if (press_start) {
            g_story_name_to_intro = 1;
            g_i_state = WAIFU_I_STORY_NAME_TO_INTRO;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_NAME_TO_INTRO:
        g_story_name_to_intro = 1;
        draw_story_name_entry();
        if (g_i_frame >= 20) {
            g_story_name_to_intro = 0;
            g_story_intro_line = 0;
            g_i_state = WAIFU_I_STORY_INTRO;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_INTRO:
        draw_story_intro_screen(g_i_frame);
        if (press_a || press_start) {
            int line_count = (int)(sizeof(story_intro_lines) / sizeof(story_intro_lines[0]));
            ++g_story_intro_line;
            if (g_story_intro_line >= line_count) {
                g_story_fire_line = 0;
                g_i_state = WAIFU_I_STORY_FIRE;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_FIRE:
        draw_story_fire_screen(g_i_frame);
        if (press_a || press_start) {
            int line_count = (int)(sizeof(story_fire_lines) / sizeof(story_fire_lines[0]));
            ++g_story_fire_line;
            if (g_story_fire_line >= line_count) {
                reset_story_deck_editor();
                g_story_editor_from_pyramid = 0;
                g_i_state = WAIFU_I_STORY_FIRE_TO_DECK;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_FIRE_TO_DECK:
        draw_story_fire_to_deck_transition(g_i_frame);
        if (g_i_frame >= 48) {
            g_i_state = WAIFU_I_DECK_EDITOR;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_MAP:
        if (press_left || press_right || press_up || press_down) g_story_map_cursor ^= 1;
        draw_story_map_screen(g_i_frame);
        if (press_a || press_start) {
            if (g_story_map_cursor == 0) {
                g_story_pyramid_cursor = 0;
                g_i_state = WAIFU_I_STORY_PYRAMID;
                g_i_frame = -1;
            } else {
                g_story_plaza_line = 0;
                g_i_state = WAIFU_I_STORY_TO_PLAZA;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_PYRAMID:
        if (press_up) g_story_pyramid_cursor = (g_story_pyramid_cursor + 2) % 3;
        if (press_down) g_story_pyramid_cursor = (g_story_pyramid_cursor + 1) % 3;
        draw_story_pyramid_menu();
        if (press_a || press_start) {
            if (g_story_pyramid_cursor == 0) {
                g_story_saved_flash = 60;
                g_story_save_status = write_story_save() ? 1 : -1;
                g_i_state = WAIFU_I_STORY_SAVE;
                g_i_frame = -1;
            } else if (g_story_pyramid_cursor == 1) {
                reset_story_deck_editor();
                g_story_editor_from_pyramid = 1;
                g_i_state = WAIFU_I_DECK_EDITOR;
                g_i_frame = -1;
            } else {
                g_i_state = WAIFU_I_STORY_MAP;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_STORY_MAP;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_SAVE:
        draw_story_save_screen();
        if (press_a || press_b || press_start || g_i_frame > 90) {
            g_i_state = WAIFU_I_STORY_PYRAMID;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_TO_PLAZA:
        draw_story_to_plaza_transition(g_i_frame);
        if (g_i_frame >= 48) {
            g_i_state = WAIFU_I_STORY_PLAZA;
            g_i_frame = 24;
        }
        break;

    case WAIFU_I_STORY_PLAZA:
        draw_story_plaza_scene();
        if (press_a || press_start) {
            ++g_story_plaza_line;
            if (g_story_plaza_line >= 3) {
                if (g_story_deck_count == STORY_DECK_SIZE) {
                    recalc_story_deck_counts();
                    init_story_battle_state();
                    g_i_state = WAIFU_I_BATTLE;
                    g_i_frame = -1;
                } else {
                    g_deck_flash = 60;
                    reset_story_deck_editor();
                    g_story_editor_from_pyramid = 0;
                    g_i_state = WAIFU_I_DECK_EDITOR;
                    g_i_frame = -1;
                }
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_STORY_MAP;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_DECK_EDITOR:
        if (press_tab) deck_editor_switch_tab();
        if (press_left) deck_editor_move_cursor(-1, 0);
        if (press_right) deck_editor_move_cursor(1, 0);
        if (press_up) deck_editor_move_cursor(0, -1);
        if (press_down) deck_editor_move_cursor(0, 1);
        if (press_a) deck_editor_move_selected_card();
        if (press_b && deck_editor_active_count() > 0) {
            int *arr = deck_editor_active_array();
            g_deck_preview_card = arr[g_deck_cursor];
            g_i_state = WAIFU_I_DECK_PREVIEW;
            g_i_frame = -1;
        }
        if (press_start) {
            if (g_story_deck_count == STORY_DECK_SIZE) {
                recalc_story_deck_counts();
                if (g_story_editor_from_pyramid && g_story_duel_index > 0) {
                    g_story_editor_from_pyramid = 0;
                    g_i_state = WAIFU_I_STORY_PYRAMID;
                    g_i_frame = -1;
                } else {
                    init_story_battle_state();
                    g_i_state = WAIFU_I_BATTLE;
                    g_i_frame = -1;
                }
            } else {
                g_deck_flash = 60;
            }
        }
        if (g_deck_flash > 0) --g_deck_flash;
        draw_deck_editor();
        break;

    case WAIFU_I_DECK_PREVIEW:
        draw_interactive_card_preview(g_deck_preview_card, g_i_frame);
        if (press_b || press_a || press_start) {
            g_i_state = WAIFU_I_DECK_EDITOR;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_BATTLE:
        step_battle_interactive(input, press_up, press_down, press_left, press_right, press_a, press_b, press_start, press_tab);
        if (press_b && g_b_phase != IB_CARD_PREVIEW && g_b_phase != IB_TALLY && g_b_phase_frame > 12) {
            /* Back is only a local action inside battle screens; no hidden auto-quit. */
        }
        break;
    }

    g_prev_input = *input;
    ++g_i_frame;
}


#ifndef WAIFU_FM_NO_HEADLESS_MAIN

typedef struct CommandEvent {
    int start;
    int duration;
    WaifuFmInput input;
} CommandEvent;

#define MAX_COMMAND_EVENTS 4096

static void input_or_button(WaifuFmInput *in, const char *tok)
{
    char buf[32];
    int i = 0;
    while (*tok && i < (int)sizeof(buf)-1) {
        if (*tok != ',' && *tok != '|' && *tok != '+') buf[i++] = (char)toupper((unsigned char)*tok);
        tok++;
    }
    buf[i] = '\0';
    if (!strcmp(buf, "UP")) in->up = 1;
    else if (!strcmp(buf, "DOWN")) in->down = 1;
    else if (!strcmp(buf, "LEFT")) in->left = 1;
    else if (!strcmp(buf, "RIGHT")) in->right = 1;
    else if (!strcmp(buf, "A") || !strcmp(buf, "LCTRL") || !strcmp(buf, "CTRL")) in->a = 1;
    else if (!strcmp(buf, "B") || !strcmp(buf, "LALT") || !strcmp(buf, "ALT")) in->b = 1;
    else if (!strcmp(buf, "START") || !strcmp(buf, "RUN") || !strcmp(buf, "SPACE")) in->start = 1;
    else if (!strcmp(buf, "TAB") || !strcmp(buf, "BUTTON4") || !strcmp(buf, "BTN4") || !strcmp(buf, "4")) in->tab = 1;
}


static void input_or_button_list(WaifuFmInput *in, const char *tok)
{
    char part[32];
    int n = 0;
    const char *p = tok;
    while (1) {
        int delim = (*p == '\0' || *p == ',' || *p == '|' || *p == '+');
        if (delim) {
            if (n > 0) {
                part[n] = '\0';
                input_or_button(in, part);
                n = 0;
            }
            if (*p == '\0') break;
        } else if (n < (int)sizeof(part) - 1) {
            part[n++] = *p;
        }
        ++p;
    }
}

static int load_command_file(const char *path, CommandEvent *events, int max_events)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int count = 0;
    if (!fp) {
        fprintf(stderr, "could not open command file: %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *tok;
        CommandEvent ev;
        memset(&ev, 0, sizeof(ev));
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        tok = strtok(p, " \t\r\n");
        if (!tok) continue;
        ev.start = atoi(tok);
        tok = strtok(NULL, " \t\r\n");
        if (!tok) continue;
        ev.duration = atoi(tok);
        if (ev.duration < 1) ev.duration = 1;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL) {
            input_or_button_list(&ev.input, tok);
        }
        if (count < max_events) events[count++] = ev;
    }
    fclose(fp);
    return count;
}

static WaifuFmInput input_for_frame_from_events(int frame, const CommandEvent *events, int count)
{
    WaifuFmInput in;
    int i;
    memset(&in, 0, sizeof(in));
    for (i = 0; i < count; ++i) {
        if (frame >= events[i].start && frame < events[i].start + events[i].duration) {
            in.up |= events[i].input.up;
            in.down |= events[i].input.down;
            in.left |= events[i].input.left;
            in.right |= events[i].input.right;
            in.a |= events[i].input.a;
            in.b |= events[i].input.b;
            in.start |= events[i].input.start;
            in.tab |= events[i].input.tab;
        }
    }
    return in;
}

int main(int argc, char **argv)
{
    int frames = 3600;
    int dump_every = 1;
    int showcase = 0;
    int scripted_render = 0;
    const char *out_dir = "headless_frames";
    const char *record_mkv = NULL;
    const char *commands_path = NULL;
    int no_png = 0;
    int dump_state = 0;
    CommandEvent events[MAX_COMMAND_EVENTS];
    int event_count = 0;
    int f;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--showcase")) showcase = 1;
        else if (!strcmp(argv[i], "--scripted-render")) scripted_render = 1;
        else if (!strcmp(argv[i], "--commands") && i + 1 < argc) commands_path = argv[++i];
        else if (!strcmp(argv[i], "--record-mkv") && i + 1 < argc) record_mkv = argv[++i];
        else if (!strcmp(argv[i], "--no-png")) no_png = 1;
        else if (!strcmp(argv[i], "--dump-state")) dump_state = 1;
        else if (!strcmp(argv[i], "--deckout-demo")) g_force_deckout_demo = 1;
        else if (!strcmp(argv[i], "--lp-loss-demo")) g_force_lp_loss_demo = 1;
    }
    if (frames < 1) frames = 1;
    if (dump_every < 1) dump_every = 1;

    waifu_fm_init();

    if (showcase) {
        write_showcase(out_dir);
        return 0;
    }

    if (commands_path) {
        event_count = load_command_file(commands_path, events, MAX_COMMAND_EVENTS);
        if (event_count < 0) return 1;
    }

    zmbv_mkv_recorder_t *rec = NULL;
    if (record_mkv) {
        char err[256] = {0};
        rec = zmbv_mkv_open(record_mkv, W, H, 60, 1, err, sizeof(err));
        if (!rec) {
            fprintf(stderr, "record-mkv failed: %s\n", err[0] ? err : "unknown error");
            return 1;
        }
    }

    if (!no_png) ensure_dir(out_dir);
    waifu_fm_reset_interactive();
    for (f = 0; f < frames; ++f) {
        if (scripted_render) {
            render_frame(f);
        } else {
            WaifuFmInput in = input_for_frame_from_events(f, events, event_count);
            waifu_fm_step(&in);
        }
        if (rec && !zmbv_mkv_add_indexed_frame(rec, framebuffer, waifu_palette_rgb, (uint64_t)f)) {
            fprintf(stderr, "record-mkv frame %d failed: %s\n", f, zmbv_mkv_error(rec));
            zmbv_mkv_abort(rec);
            return 1;
        }
        if (!no_png && (f % dump_every) == 0) write_frame_png(out_dir, f);
    }
    if (rec) {
        if (!zmbv_mkv_close(rec)) {
            fprintf(stderr, "record-mkv close failed\n");
            return 1;
        }
        printf("wrote %s (%d ZMBV MKV frames at 256x240)\n", record_mkv, frames);
    }
    if (dump_state) {
        printf("STATE frame=%d phase=%d phase_frame=%d turns=%d you_lp=%d com_lp=%d ",
               frames, (int)g_b_phase, g_b_phase_frame, g_b_turns, g_you_lp, g_com_lp);
        printf("player_field0=%d player_field1=%d player_field2=%d player_field3=%d player_field4=%d ",
               g_i_player_field[0], g_i_player_field[1], g_i_player_field[2], g_i_player_field[3], g_i_player_field[4]);
        printf("player_faceup0=%d player_defense0=%d com_field0=%d com_faceup0=%d player_attacked0=%d com_attacked0=%d ",
               g_i_player_faceup[0], g_i_player_defense[0], g_i_com_field[0], g_i_com_faceup[0],
               g_i_player_attacked[0], g_i_com_attacked[0]);
        printf("player_atk0=%d player_def0=%d player_atk_bonus0=%d player_def_bonus0=%d ",
               field_card_atk(0, 0), field_card_def(0, 0), g_i_player_atk_bonus[0], g_i_player_def_bonus[0]);
        printf("player_equip0=%d player_equip_target0=%d ",
               g_i_player_equip_field[0], g_i_player_equip_target[0]);
        printf("player_monster_played=%d com_monster_played=%d result=%d top_col=%d top_row=%d attack_target=%d preview_card=%d ",
                g_b_player_monster_played_this_turn, g_b_com_monster_played_this_turn, g_b_result,
                g_b_top_col, g_b_top_row, g_b_attack_attacker_slot, g_b_preview_card_id);
        printf("istate=%d story=%d story_line=%d story_fire_line=%d story_name=%s story_strong=%d story_weak=%d story_equips=%d story_supports=%d ",
               (int)g_i_state, g_story_battle_active, g_story_intro_line, g_story_fire_line, g_story_name,
               g_story_strong_card, g_story_weak_card, g_story_equip_count, g_story_support_count);
        printf("deck_count=%d storage_count=%d max_deck_copies=%d deck_tab=%d deck_cursor=%d story_duel=%d map_cursor=%d pyramid_cursor=%d plaza_line=%d editor_from_pyramid=%d save_status=%d save_exists=%d opponent=%s ",
               g_story_deck_count, g_story_storage_count, story_deck_max_card_copies(), g_deck_tab, g_deck_cursor,
               g_story_duel_index, g_story_map_cursor, g_story_pyramid_cursor, g_story_plaza_line,
               g_story_editor_from_pyramid, g_story_save_status, story_save_exists(), story_opponent_name());
        printf("hand0=%d hand1=%d hand2=%d hand3=%d hand4=%d deck_pos=%d deck_left=%d\n",
               g_i_player_hand[0], g_i_player_hand[1], g_i_player_hand[2], g_i_player_hand[3], g_i_player_hand[4],
               g_story_player_deck_pos, g_i_player_deck_left);
    }
    return 0;
}
#endif /* WAIFU_FM_NO_HEADLESS_MAIN */
