#include <stdint.h>
#include <string.h>
/* stdio/stdlib/ctype are used only by the headless runner + regression harness
   (the headless platform's tooling, below the WAIFU_FM_NO_HEADLESS_MAIN guard).
   The common game code has no stdio dependency, so a ROM/RAM target that defines
   WAIFU_FM_NO_HEADLESS_MAIN compiles main.c without pulling any file/stdio API. */
#ifndef WAIFU_FM_NO_HEADLESS_MAIN
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#endif
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
#include <sys/time.h>
#endif

#if defined(WAIFU_ASSET_USE_CDROM)
#define WAIFU_ASSET_EXTERNAL_TITLE_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_ENDING_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS 1
#define WAIFU_ASSET_EXTERNAL_CARD_IMAGES 1
#endif

#include "defines.h"
#include "platform.h"
#include "common.h"
#include "renderer3d.h"
#include "bmp_writer.h"
#include "font_menudata.h"
#include "waifu_assets.h"
#include "title_asset.h"
#ifndef WAIFU_FM_NO_HEADLESS_MAIN
#include "zmbv_mkv.h"
#endif
#include "game_api.h"
#include "ai.h"
#include "deck.h"
#include "deck_pools.h"
#include "palette.h"
#include "sounds.h"
#include "assets.h"
#ifdef WAIFU_FM_PCFX
#include "waifu_pcfx_video.h"
#include "pcfx_biosfs.h"
#endif
#ifdef WAIFU_FM_CD32X
#include "waifu_cd32x_video.h"
#include "waifu_cd32x_memory.h"
#endif

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
#define FLOOR_SAMPLE_CACHE_MAX_PERIOD_Q16 (Q8_FROM_INT(4) << Q8_SHIFT)
#define FLOOR_SAMPLE_CACHE_SLOTS 2
#if (defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)) && !defined(WAIFU_BOARD_FAST_AFFINE_ENABLE)
/* PC-FX/CD32X: render the battle board through the pre-projected quad path.  The
   generic path reprojects every cell corner and wall corner independently;
   the fast path projects the shared grid once and draws the same 32x32 tile
   quads with the compact affine renderer. */
#define WAIFU_BOARD_FAST_AFFINE_ENABLE 1
#endif
#if defined(WAIFU_FM_CD32X) && !defined(WAIFU_CD32X_FIELD_SIDE_WALLS)
/* CD32X re-renders the battle field every frame, so keep side walls on the
   compact affine path and split part of that work across the Slave SH-2. */
#define WAIFU_CD32X_FIELD_SIDE_WALLS 1
#endif
#if defined(WAIFU_FM_CD32X) && !defined(WAIFU_CD32X_BOARD_FLAT_TOP)
/* Keep the CD32X field fully textured by default.  WAIFU_CD32X_BOARD_FLAT_TOP
   remains available as a speed/debug LOD path, but the textured path is the
   release target and is covered by the BlastEm battle capture. */
#define WAIFU_CD32X_BOARD_FLAT_TOP 0
#endif
#define CFX_PI_Q8 804
#define TITLE_SEQUENCE_FRAMES 310
#define DUEL_TOTAL_FRAMES 2696
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
#define DUEL_OPENING_END 72
#define WAIFU_PCFX_PLACE_FRAMES 12
#define WAIFU_PCFX_PLACE_SETTLE_FRAMES 10
#define WAIFU_PCFX_TURN_FRAMES 12
#define WAIFU_PCFX_SELECT_FRAMES 16
#define WAIFU_PCFX_RETURN_FRAMES 12
/* Hand<->top camera lift.  PC-FX uses cached 3D camera keyframes so the
   board still moves through real perspective steps without re-rendering a
   unique camera every transition frame. */
#define WAIFU_PCFX_HANDTOP_FRAMES 8
#define WAIFU_PCFX_HANDTOP_ANCHORS 5
#define WAIFU_RESULT_UI_CLEAR_FRAMES 32
#define WAIFU_RESULT_MUSIC_LEAD_FRAMES 8
#define WAIFU_RESULT_ANIM_START_FRAMES (WAIFU_RESULT_UI_CLEAR_FRAMES + WAIFU_RESULT_MUSIC_LEAD_FRAMES)
#define WAIFU_RESULT_TOTAL_FRAMES (WAIFU_RESULT_ANIM_START_FRAMES + WAIFU_PCFX_HANDTOP_FRAMES + 96)
#define WAIFU_PCFX_DRAW_FRAMES 18
#define WAIFU_HAND_INTRO_FRAMES 18
#define WAIFU_EQUIP_ANIM_FRAMES 36
#define WAIFU_FUSION_ANIM_FRAMES 74
#define WAIFU_BATTLE_PRELUDE_FRAMES 8
#define WAIFU_BATTLE_SLIDE_FRAMES 8
#define WAIFU_BATTLE_FLIP_FRAMES 8
#define WAIFU_BATTLE_REVEAL_PAUSE_FRAMES 1
#define WAIFU_BATTLE_RAM_FRAMES 10
#define WAIFU_BATTLE_COUNTER_GAP_FRAMES 5
#define WAIFU_BATTLE_BURN_DELAY_FRAMES 8
#define WAIFU_BATTLE_FINAL_SETTLE_FRAMES 3
#define WAIFU_DIRECT_SLIDE_FRAMES 8
#define WAIFU_DIRECT_LUNGE_FRAMES 12
#define WAIFU_DIRECT_DAMAGE_HOLD_FRAMES 8
#else
#define DUEL_OPENING_END 150
#define WAIFU_PCFX_PLACE_FRAMES 48
#define WAIFU_PCFX_PLACE_SETTLE_FRAMES 38
#define WAIFU_PCFX_TURN_FRAMES 58
#define WAIFU_PCFX_SELECT_FRAMES 70
#define WAIFU_PCFX_RETURN_FRAMES 30
#define WAIFU_PCFX_HANDTOP_FRAMES 18
#define WAIFU_RESULT_UI_CLEAR_FRAMES 50
#define WAIFU_RESULT_MUSIC_LEAD_FRAMES 12
#define WAIFU_RESULT_ANIM_START_FRAMES (WAIFU_RESULT_UI_CLEAR_FRAMES + WAIFU_RESULT_MUSIC_LEAD_FRAMES)
#define WAIFU_RESULT_TOTAL_FRAMES (WAIFU_RESULT_ANIM_START_FRAMES + WAIFU_PCFX_HANDTOP_FRAMES + 130)
#define WAIFU_PCFX_DRAW_FRAMES 84
#define WAIFU_HAND_INTRO_FRAMES 60
#define WAIFU_EQUIP_ANIM_FRAMES 120
#define WAIFU_FUSION_ANIM_FRAMES 166
#define WAIFU_BATTLE_PRELUDE_FRAMES 16
#define WAIFU_BATTLE_SLIDE_FRAMES 18
#define WAIFU_BATTLE_FLIP_FRAMES 32
#define WAIFU_BATTLE_REVEAL_PAUSE_FRAMES 6
#define WAIFU_BATTLE_RAM_FRAMES 34
#define WAIFU_BATTLE_COUNTER_GAP_FRAMES 16
#define WAIFU_BATTLE_BURN_DELAY_FRAMES 30
#define WAIFU_BATTLE_FINAL_SETTLE_FRAMES 8
#define WAIFU_DIRECT_SLIDE_FRAMES 24
#define WAIFU_DIRECT_LUNGE_FRAMES 40
#define WAIFU_DIRECT_DAMAGE_HOLD_FRAMES 38
#endif
#define DUEL_PREVIEW_END 270
#define DUEL_SCRIPT_OFFSET (DUEL_PREVIEW_END - 84)

/* Responsive battle-UI offsets.  Both reduce to 0 at the 256-wide layout (PC-FX
   and the 256x240 builds), so those screens are byte-for-byte unchanged; on a
   wider framebuffer (e.g. CD32X 320) they push the right-edge LP panel to the
   edge and re-center the card hand in the extra horizontal space. */
#define WAIFU_UI_EXTRA_W   (WAIFU_FM_WIDTH - 256)
#define WAIFU_UI_CENTER_DX ((WAIFU_FM_WIDTH - 256) / 2)
#define WAIFU_UI_EXTRA_H   (WAIFU_FM_HEIGHT - 240)
#define WAIFU_UI_BOTTOM_Y(y) ((y) + WAIFU_UI_EXTRA_H)
#define WAIFU_BOTTOM_INFO_Y (WAIFU_FM_HEIGHT - 35)
#define WAIFU_HAND_Y_BASE (WAIFU_BOTTOM_INFO_Y - 51)
#define WAIFU_BATTLE_CARD_W 120
#define WAIFU_BATTLE_CARD_H 160
#define WAIFU_BATTLE_CARD_X0 (WAIFU_UI_CENTER_DX + 4)
#define WAIFU_BATTLE_CARD_X1 (WAIFU_UI_CENTER_DX + 132)
#define WAIFU_BATTLE_CARD_Y (((WAIFU_FM_HEIGHT - 202) < 33) ? (WAIFU_FM_HEIGHT - 202) : 33)
#define WAIFU_SINGLE_BATTLE_CARD_X ((WAIFU_FM_WIDTH - WAIFU_BATTLE_CARD_W) / 2)
#define WAIFU_BIG_ART_128_X ((WAIFU_FM_WIDTH - 128) / 2)

#if defined(WAIFU_FM_CD32X)
/* CD32X renders the common 8bpp surface directly into the inactive 32X VDP
   framebuffer page. This avoids spending 75 KiB of SH-2 SDRAM on a shadow
   framebuffer before the asset/card-art caches are considered. */
static uint8_t *const framebuffer = (uint8_t *)WAIFU_CD32X_FRAMEBUFFER_PIXELS;
#elif defined(WAIFU_FM_PCFX)
static uint8_t framebuffer[WAIFU_FM_WIDTH * WAIFU_FM_HEIGHT] __attribute__((aligned(16)));
#else
static uint8_t framebuffer[WAIFU_FM_WIDTH * WAIFU_FM_HEIGHT];
#endif
static uint32_t g_frame_dirty_serial = 0;
static int g_frame_dirty_full = 0;
static int g_frame_dirty_count = 0;
static WaifuFmDirtyRect g_frame_dirty_rects[WAIFU_FM_MAX_DIRTY_RECTS];
static int g_video_fade_visible_q8 = Q8_ONE;
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

/* Local integer abs so the common game code does not depend on <stdlib.h>. */
static int i_abs(int v)
{
    return v < 0 ? -v : v;
}

static void frame_dirty_reset(void)
{
    g_frame_dirty_full = 0;
    g_frame_dirty_count = 0;
}

static void frame_mark_full_dirty(void)
{
    g_frame_dirty_full = 1;
    g_frame_dirty_count = 0;
    ++g_frame_dirty_serial;
}

static void frame_mark_dirty_rect(int x, int y, int w, int h)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    if (w <= 0 || h <= 0 || g_frame_dirty_full) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH;
    if (y1 > WAIFU_FM_HEIGHT) y1 = WAIFU_FM_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    if (g_frame_dirty_count >= WAIFU_FM_MAX_DIRTY_RECTS) {
        frame_mark_full_dirty();
        return;
    }
    g_frame_dirty_rects[g_frame_dirty_count].x = (uint16_t)x0;
    g_frame_dirty_rects[g_frame_dirty_count].y = (uint16_t)y0;
    g_frame_dirty_rects[g_frame_dirty_count].w = (uint16_t)(x1 - x0);
    g_frame_dirty_rects[g_frame_dirty_count].h = (uint16_t)(y1 - y0);
    ++g_frame_dirty_count;
    ++g_frame_dirty_serial;
}


#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
static int g_profile_render_enabled = 0;
static unsigned long long g_profile_board_cache_hits = 0;
static unsigned long long g_profile_board_cache_misses = 0;
static unsigned long long g_profile_board_cache_copy_us = 0;
static unsigned long long g_profile_board_cache_render_us = 0;
static unsigned long long g_profile_hand_calls = 0;
static unsigned long long g_profile_hand_total_us = 0;
static unsigned long long g_profile_hand_card_draws = 0;
static unsigned long long g_profile_hand_card_us = 0;
static unsigned long long g_profile_card2d_fast_calls = 0;
static unsigned long long g_profile_card2d_generic_calls = 0;
static unsigned long long g_profile_card2d_generic_us = 0;
static unsigned long long g_profile_ui_fast_fill_calls = 0;

static unsigned long long profile_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ull + (unsigned long long)tv.tv_usec;
}

#define PROFILE_HAND_CARD_DRAW(call_expr) \
    do { \
        if (g_profile_render_enabled) { \
            unsigned long long _profile_t0 = profile_now_us(); \
            call_expr; \
            g_profile_hand_card_us += profile_now_us() - _profile_t0; \
            g_profile_hand_card_draws++; \
        } else { \
            call_expr; \
        } \
    } while (0)
#define PROFILE_HAND_BEGIN() unsigned long long _profile_hand_t0 = g_profile_render_enabled ? profile_now_us() : 0
#define PROFILE_HAND_END() \
    do { \
        if (g_profile_render_enabled) { \
            g_profile_hand_total_us += profile_now_us() - _profile_hand_t0; \
            g_profile_hand_calls++; \
        } \
    } while (0)
#define PROFILE_CARD2D_FAST() do { if (g_profile_render_enabled) g_profile_card2d_fast_calls++; } while (0)
#define PROFILE_CARD2D_GENERIC_BEGIN() unsigned long long _profile_card2d_t0 = g_profile_render_enabled ? profile_now_us() : 0
#define PROFILE_CARD2D_GENERIC_END() \
    do { \
        if (g_profile_render_enabled) { \
            g_profile_card2d_generic_calls++; \
            g_profile_card2d_generic_us += profile_now_us() - _profile_card2d_t0; \
        } \
    } while (0)
#define PROFILE_UI_FAST_FILL() do { if (g_profile_render_enabled) g_profile_ui_fast_fill_calls++; } while (0)
#else
#define PROFILE_HAND_CARD_DRAW(call_expr) do { call_expr; } while (0)
#define PROFILE_HAND_BEGIN() do { } while (0)
#define PROFILE_HAND_END() do { } while (0)
#define PROFILE_CARD2D_FAST() do { } while (0)
#define PROFILE_CARD2D_GENERIC_BEGIN() do { } while (0)
#define PROFILE_CARD2D_GENERIC_END() do { } while (0)
#define PROFILE_UI_FAST_FILL() do { } while (0)
#endif

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
static int g_b_top_row = 2;
static int g_b_top_prev_col = 0;
static int g_b_top_prev_row = 2;
static int g_b_top_cursor_anim = 8;
static int g_b_attack_attacker_slot = -1;

#ifdef WAIFU_FM_PCFX
#define BATTLE_BURN_DUR 12
#define BATTLE_BURN_VANISH_FRAMES 10
#else
#define BATTLE_BURN_DUR 52
#define BATTLE_BURN_VANISH_FRAMES 44
#endif
#define SUPPORT_EQUIP_CARD_ID   (WAIFU_CARD_COUNT + 0)
#define SUPPORT_GUARD_CARD_ID   (WAIFU_CARD_COUNT + 1)
#define SUPPORT_DRAW_CARD_ID    (WAIFU_CARD_COUNT + 2)
#define SUPPORT_HEAL_CARD_ID    (WAIFU_CARD_COUNT + 3)
#define SUPPORT_THUNDER_CARD_ID (WAIFU_CARD_COUNT + 4)
#define SUPPORT_TRAP_CARD_ID    (WAIFU_CARD_COUNT + 5)
#define SUPPORT_STANDARD_CARD_VARIANTS 4
#define SUPPORT_CARD_VARIANTS   6
/* Common-palette blue accents matching the standard support card frame. */
#define IDX_SUPPORT_FRAME    IDX_BLUE_WHITE
#define IDX_SUPPORT_FRAME_HI IDX_UI_BLUE
/* Trap cards share the support layout but in a violet frame so they read as a
   distinct card type while standard support cards stay blue. */
#define IDX_TRAP_FRAME       137
#define IDX_TRAP_FRAME_HI    47
#define IDX_TRAP_FRAME_DK    145
#define STORY_MIN_SUPPORT_CARDS 9
#define STORY_MIN_EQUIP_CARDS   3
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
    case 4: return "THUNDER";
    case 5: return "MIRROR VEIL";
    default: return "OASIS LIGHT";
    }
}

static const char *support_card_type(int card_id)
{
    switch (support_card_kind(card_id)) {
    case 0: return "Equip / Support";
    case 1: return "Guard / Support";
    case 2: return "Draw / Support";
    case 4: return "Storm / Support";
    case 5: return "Trap / Counter";
    default: return "Heal / Support";
    }
}

static const char *support_card_effect(int card_id)
{
    switch (support_card_kind(card_id)) {
    case 0: return "Equip card. Use from hand; does not count as your one monster placement.";
    case 1: return "Equip card. Raises ATK by 250 and DEF by 800.";
    case 2: return "Support card. Draw 1 card from your deck.";
    case 4: return "Support card. Destroys every monster on the opponent's field.";
    case 5: return "Trap card. Auto-activates when a monster attacks you: it destroys that attacker and cancels the attack before the battle step.";
    default: return "Support card. Restore 1000 LP.";
    }
}

static int is_support_card(int card_id)
{
    /* Support and equip cards live above the generated monster id range. */
    return card_id >= WAIFU_CARD_COUNT;
}

static int is_thunder_support_card(int card_id)
{
    return support_card_kind(card_id) == 4;
}

static int is_trap_support_card(int card_id)
{
    return is_support_card(card_id) && support_card_kind(card_id) == 5;
}

static int is_guard_support_card(int card_id)
{
    return is_support_card(card_id) && support_card_kind(card_id) == 1;
}

static int is_draw_support_card(int card_id)
{
    return is_support_card(card_id) && support_card_kind(card_id) == 2;
}

static int is_heal_support_card(int card_id)
{
    return is_support_card(card_id) && support_card_kind(card_id) == 3;
}

static int is_equip_support_card(int card_id)
{
    return is_support_card(card_id) &&
           (support_card_kind(card_id) == 0 || support_card_kind(card_id) == 1);
}

static int is_monster_card(int card_id)
{
    return card_id >= 0 && card_id < WAIFU_CARD_COUNT;
}

typedef struct FusionRule {
    int a;
    int b;
    int result;
} FusionRule;

static const FusionRule g_fusion_rules[] = {
    {WAIFU_CARD_ID_COCINELLE, WAIFU_CARD_ID_BEE_WOMAN, WAIFU_CARD_ID_INSECT_QUEEN},
    {WAIFU_CARD_ID_COCINELLE, WAIFU_CARD_ID_REPTILE, WAIFU_CARD_ID_INSECT_QUEEN},
    {WAIFU_CARD_ID_PARROT, WAIFU_CARD_ID_PENGUIN, WAIFU_CARD_ID_OWL_WOMAN},
    {WAIFU_CARD_ID_RAT, WAIFU_CARD_ID_SLIME, WAIFU_CARD_ID_SQUID_TENTACLES},
    {WAIFU_CARD_ID_SNAKE, WAIFU_CARD_ID_CROW, WAIFU_CARD_ID_COBRA},
    {WAIFU_CARD_ID_GOLEM_IDOL, WAIFU_CARD_ID_ELECTRIC, WAIFU_CARD_ID_STONE_DRAGON},
    {WAIFU_CARD_ID_JESTER, WAIFU_CARD_ID_PRIESTESS, WAIFU_CARD_ID_MAGE_SKELETON},
    {WAIFU_CARD_ID_TURTLE, WAIFU_CARD_ID_REPTILE, WAIFU_CARD_ID_SHARK},
    {WAIFU_CARD_ID_WITCH, WAIFU_CARD_ID_PENGUIN, WAIFU_CARD_ID_PUMPKIN},
    {WAIFU_CARD_ID_INSECT_BOMB, WAIFU_CARD_ID_INSECT_SOLDIER, WAIFU_CARD_ID_INSECT_QUEEN_WOMAN},
    {WAIFU_CARD_ID_ELEC_WOLF, WAIFU_CARD_ID_CROW, WAIFU_CARD_ID_YOKAI},
    {WAIFU_CARD_ID_STREET, WAIFU_CARD_ID_BEE_WOMAN, WAIFU_CARD_ID_WARRIOR},
};

static int fusion_result_for_cards(int a, int b)
{
    int i;
    for (i = 0; i < (int)(sizeof(g_fusion_rules) / sizeof(g_fusion_rules[0])); ++i) {
        if ((g_fusion_rules[i].a == a && g_fusion_rules[i].b == b) ||
            (g_fusion_rules[i].a == b && g_fusion_rules[i].b == a)) {
            return g_fusion_rules[i].result;
        }
    }
    if (is_monster_card(a) && is_monster_card(b) &&
        strcmp(waifu_card_attr[a], "Water") == 0 &&
        strcmp(waifu_card_attr[b], "Water") == 0) {
        int atk_a = (int)waifu_card_atk[a];
        int atk_b = (int)waifu_card_atk[b];
        if (atk_a >= 2000 && atk_b >= 2000) return WAIFU_CARD_ID_ANGEL_FISHWOMAN;
        if (atk_a < 2000 && atk_b < 2000) return WAIFU_CARD_ID_SEA_SERPENT;
    }
    return -1;
}

static int field_card_atk(int owner, int slot);
static int field_card_def(int owner, int slot);
static int story_water_field_active(void);
static int equip_atk_bonus(int card_id);
static int equip_def_bonus(int card_id);

/* ------------------------------------------------------------------------- */
/* Small vector/camera utilities. The actual textured quad rasterization is   */
/* Cascade FX's renderer3d; these functions only provide a PC/headless camera. */

typedef struct { int32_t x, y, z; } Vec3;
typedef struct { Vec3 eye, target, up; int32_t focal; } Camera;

#if !defined(WAIFU_BG_CACHE_DISABLE)
static uint8_t g_board_bg_cache[WAIFU_FM_WIDTH * WAIFU_FM_HEIGHT];
static Camera g_board_bg_cache_cam;
static int g_board_bg_cache_valid = 0;
#endif
static int camera_equal(Camera a, Camera b)
{
    return a.eye.x == b.eye.x && a.eye.y == b.eye.y && a.eye.z == b.eye.z &&
           a.target.x == b.target.x && a.target.y == b.target.y && a.target.z == b.target.z &&
           a.up.x == b.up.x && a.up.y == b.up.y && a.up.z == b.up.z &&
           a.focal == b.focal;
}

static void invalidate_board_bg_cache(void)
{
#if !defined(WAIFU_BG_CACHE_DISABLE)
    g_board_bg_cache_valid = 0;
#endif
}
typedef struct { int x, y; int32_t depth; int ok; } ScreenPt;
typedef struct { int x, y, u, v; } TexV;
#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
typedef struct {
    int32_t tile_size;
    int32_t period_q16;
    uint8_t sample[FLOOR_SAMPLE_CACHE_MAX_PERIOD_Q16];
} FloorSampleCache;
#endif

static void draw_late_field_cards(Camera cam, int f);
static void draw_textured_tri(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c);

static int32_t q8_mul(int32_t a, int32_t b) { return (int32_t)((a * b) >> Q8_SHIFT); }
static const uint16_t q8_recip_q8_u16[257] = {
    0, 256, 128, 85, 64, 51, 43, 37, 32, 28, 26, 23, 21, 20, 18, 17,
    16, 15, 14, 13, 13, 12, 12, 11, 11, 10, 10, 9, 9, 9, 9, 8,
    8, 8, 8, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1
};

static int32_t div_toward_zero_i32(int32_t n, int32_t b)
{
    if (!b) return 0;
    uint8_t neg = 0;
    if (n < 0) { n = -n; neg ^= 1; }
    if (b < 0) { b = -b; neg ^= 1; }
    uint32_t un = (uint32_t)n;
    uint32_t ud = (uint32_t)b;
    uint32_t q;
    if (ud == 1u) {
        q = un;
    } else {
        uint32_t scaled = ud;
        uint16_t shift = 0;
        while (scaled > 256u) { scaled = (scaled + 1u) >> 1; ++shift; }
        q = ((un * (uint32_t)q8_recip_q8_u16[scaled]) >> (8 + shift));
        uint32_t prod = q * ud;
        while (prod > un) { --q; prod -= ud; }
        while ((uint32_t)(un - prod) >= ud) { ++q; prod += ud; }
    }
    return neg ? -(int32_t)q : (int32_t)q;
}
static int32_t q8_div(int32_t a, int32_t b) { return b ? div_toward_zero_i32(a * Q8_ONE, b) : 0; }
static int32_t q8_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
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

#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
static FloorSampleCache g_floor_sample_cache[FLOOR_SAMPLE_CACHE_SLOTS];
#endif
static uint8_t g_repeat_texel_q8[Q8_ONE];
static int g_repeat_texel_q8_ready = 0;

static void init_repeat_texel_q8(void)
{
    int tex = 0;
    int acc = Q8_HALF;
    int step = WAIFU_TEX_TILE_SIZE - 1;
    int max_tex = WAIFU_TEX_TILE_SIZE - 2;
    if (g_repeat_texel_q8_ready) return;
    for (int i = 0; i < Q8_ONE; ++i) {
        while (acc >= Q8_ONE) {
            acc -= Q8_ONE;
            if (tex < max_tex) ++tex;
        }
        g_repeat_texel_q8[i] = (uint8_t)tex;
        acc += step;
    }
    g_repeat_texel_q8_ready = 1;
}

static int repeat_texel_from_q8(int phase)
{
    init_repeat_texel_q8();
    return g_repeat_texel_q8[phase & (Q8_ONE - 1)];
}

static int32_t wrap_floor_sample_phase(int32_t phase, int32_t period)
{
    /* O(1) wrap into [0, period).  The old version looped subtracting the
       period; near the horizon one screen pixel spans many tiles (huge dx),
       so that looped dozens of times PER PIXEL and dominated the story-map
       floor cost.  A single hardware modulo replaces the whole loop. */
    if (period <= 0) return 0;
    phase %= period;
    if (phase < 0) phase += period;
    return phase;
}

#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
static void build_floor_sample_cache(FloorSampleCache *cache, int32_t tile_size)
{
    int32_t tile_q16 = tile_size << Q8_SHIFT;
    int32_t period_q16 = tile_q16 + tile_q16;
    int step = WAIFU_TEX_TILE_SIZE - 1;
    int max_tex = WAIFU_TEX_TILE_SIZE - 1;
    if (period_q16 > FLOOR_SAMPLE_CACHE_MAX_PERIOD_Q16) period_q16 = FLOOR_SAMPLE_CACHE_MAX_PERIOD_Q16;
    cache->tile_size = tile_size;
    cache->period_q16 = period_q16;
    for (int tile = 0; tile < 2; ++tile) {
        int tex = 0;
        int32_t acc = tile_q16 / 2;
        int32_t base = tile ? tile_q16 : 0;
        int32_t limit = tile_q16;
        if (base + limit > period_q16) limit = period_q16 - base;
        for (int32_t rem = 0; rem < limit; ++rem) {
            int32_t idx = base + rem;
            while (acc >= tile_q16) {
                acc -= tile_q16;
                if (tex < max_tex) ++tex;
            }
            cache->sample[idx] = (uint8_t)((tile << 5) | tex);
            acc += step;
        }
    }
}

static FloorSampleCache *floor_sample_cache_for(int32_t tile_size)
{
    int free_slot = -1;
    if (tile_size <= 0 || (tile_size << (Q8_SHIFT + 1)) > FLOOR_SAMPLE_CACHE_MAX_PERIOD_Q16) return NULL;
    for (int i = 0; i < FLOOR_SAMPLE_CACHE_SLOTS; ++i) {
        if (g_floor_sample_cache[i].tile_size == tile_size) return &g_floor_sample_cache[i];
        if (free_slot < 0 && g_floor_sample_cache[i].tile_size == 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    build_floor_sample_cache(&g_floor_sample_cache[free_slot], tile_size);
    return &g_floor_sample_cache[free_slot];
}

#else
static uint8_t floor_sample_direct(int32_t phase_q16, int32_t tile_size)
{
    int32_t tile_q16 = tile_size << Q8_SHIFT;
    int32_t period_q16 = tile_q16 + tile_q16;
    int32_t p;
    int tile;
    int32_t local;
    int tex;

    if (tile_q16 <= 0) return 0;
    p = wrap_floor_sample_phase(phase_q16, period_q16);
    tile = (p >= tile_q16) ? 1 : 0;
    local = tile ? (p - tile_q16) : p;
    tex = (int)(((int64_t)local * (WAIFU_TEX_TILE_SIZE - 1)) / tile_q16);
    if (tex < 0) tex = 0;
    if (tex >= WAIFU_TEX_TILE_SIZE) tex = WAIFU_TEX_TILE_SIZE - 1;
    return (uint8_t)((tile << 5) | tex);
}
#endif

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
    s.x = WAIFU_FM_WIDTH / 2 + q8_to_int(q8_mul(q8_div(cx, cz), cam.focal));
    s.y = WAIFU_FM_HEIGHT / 2 - q8_to_int(q8_mul(q8_div(cy, cz), cam.focal));
    if (s.x < -8192) s.x = -8192; else if (s.x > 8192) s.x = 8192;
    if (s.y < -8192) s.y = -8192; else if (s.y > 8192) s.y = 8192;
    s.ok = 1;
    return s;
}

typedef struct CameraBasis {
    Vec3 eye;
    Vec3 fwd;
    Vec3 right;
    Vec3 up;
    int32_t focal;
} CameraBasis;

static CameraBasis make_camera_basis(Camera cam)
{
    CameraBasis b;
    b.eye = cam.eye;
    b.fwd = vnorm(vsub(cam.target, cam.eye));
    b.right = vnorm(vcross(b.fwd, cam.up));
    b.up = vcross(b.right, b.fwd);
    b.focal = cam.focal;
    return b;
}

static ScreenPt project_point_basis(const CameraBasis *b, Vec3 p)
{
    Vec3 rel = vsub(p, b->eye);
    int32_t cx = vdot(rel, b->right);
    int32_t cy = vdot(rel, b->up);
    int32_t cz = vdot(rel, b->fwd);
    ScreenPt s;
    s.depth = cz;
    if (cz <= Q8_FRAC(5,100)) { s.x = s.y = 0; s.ok = 0; return s; }
    s.x = WAIFU_FM_WIDTH / 2 + q8_to_int(q8_mul(q8_div(cx, cz), b->focal));
    s.y = WAIFU_FM_HEIGHT / 2 - q8_to_int(q8_mul(q8_div(cy, cz), b->focal));
    /* Near-singularity vertices (tiny cz) overflow q8_mul into garbage screen
       coords in the millions; bound them so no downstream consumer (board, cards,
       lines) is ever fed a wild value -- the same +-8192 bound cfx_board_tri uses,
       so real on/off-screen geometry is unchanged. */
    if (s.x < -8192) s.x = -8192; else if (s.x > 8192) s.x = 8192;
    if (s.y < -8192) s.y = -8192; else if (s.y > 8192) s.y = 8192;
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
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
    /* Stone-temple pillar quads used to go through draw_textured_tri(), which
       evaluates barycentric texture coordinates with per-pixel MUL/DIV.  The
       PC-FX/CD32X map objects use the same 32x32 atlas tiles as the in-game
       board, so route them through the compact affine board rasterizer instead:
       projection is unchanged, but the fill path is edge-stepped and
       division-free in the hot loops. */
    {
        const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
        Point2D p0 = {(DEFAULT_INT)pa.x, (DEFAULT_INT)pa.y, 0, 0};
        Point2D p1 = {(DEFAULT_INT)pb.x, (DEFAULT_INT)pb.y, uvmax, 0};
        Point2D p2 = {(DEFAULT_INT)pc.x, (DEFAULT_INT)pc.y, uvmax, uvmax};
        Point2D p3 = {(DEFAULT_INT)pd.x, (DEFAULT_INT)pd.y, 0, uvmax};
        cfx_renderer3d_draw_quad_board(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
    }
#else
    const uint8_t *src = waifu_texture_atlas + ((size_t)tile * WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    TexV t0 = {pa.x, pa.y, 0, 0};
    TexV t1 = {pb.x, pb.y, Q8_ONE, 0};
    TexV t2 = {pc.x, pc.y, Q8_ONE, Q8_ONE};
    TexV t3 = {pd.x, pd.y, 0, Q8_ONE};
    draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t1, t2);
    draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t2, t3);
#endif
}

static void draw_quad3d(Camera cam, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int tile)
{
    ScreenPt pa, pb, pc, pd;
    if (!project_quad3d(cam, a, b, c, d, &pa, &pb, &pc, &pd)) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
    int ax = pa.x < 0 ? 0 : (pa.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pa.x);
    int ay = pa.y < 0 ? 0 : (pa.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pa.y);
    int bx = pb.x < 0 ? 0 : (pb.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pb.x);
    int by = pb.y < 0 ? 0 : (pb.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pb.y);
    int cx = pc.x < 0 ? 0 : (pc.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pc.x);
    int cy = pc.y < 0 ? 0 : (pc.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pc.y);
    int dx = pd.x < 0 ? 0 : (pd.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pd.x);
    int dy = pd.y < 0 ? 0 : (pd.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pd.y);
    Point2D p0 = {(DEFAULT_INT)ax, (DEFAULT_INT)ay, 0, 0};
    Point2D p1 = {(DEFAULT_INT)bx, (DEFAULT_INT)by, uvmax, 0};
    Point2D p2 = {(DEFAULT_INT)cx, (DEFAULT_INT)cy, uvmax, uvmax};
    Point2D p3 = {(DEFAULT_INT)dx, (DEFAULT_INT)dy, 0, uvmax};
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
    cfx_renderer3d_draw_quad_board(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
#else
    cfx_renderer3d_draw_quad(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
#endif
}

static int field_side_tile_for_cell(int c, int r);

static void draw_quad3d_fast_projected(ScreenPt pa, ScreenPt pb, ScreenPt pc, ScreenPt pd, int tile)
{
    if (!pa.ok || !pb.ok || !pc.ok || !pd.ok) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
    int ax = pa.x < 0 ? 0 : (pa.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pa.x);
    int ay = pa.y < 0 ? 0 : (pa.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pa.y);
    int bx = pb.x < 0 ? 0 : (pb.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pb.x);
    int by = pb.y < 0 ? 0 : (pb.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pb.y);
    int cx = pc.x < 0 ? 0 : (pc.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pc.x);
    int cy = pc.y < 0 ? 0 : (pc.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pc.y);
    int dx = pd.x < 0 ? 0 : (pd.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : pd.x);
    int dy = pd.y < 0 ? 0 : (pd.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : pd.y);
    Point2D p0 = {(DEFAULT_INT)ax, (DEFAULT_INT)ay, 0, 0};
    Point2D p1 = {(DEFAULT_INT)bx, (DEFAULT_INT)by, uvmax, 0};
    Point2D p2 = {(DEFAULT_INT)cx, (DEFAULT_INT)cy, uvmax, uvmax};
    Point2D p3 = {(DEFAULT_INT)dx, (DEFAULT_INT)dy, 0, uvmax};
    cfx_renderer3d_draw_quad_fast_affine(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
}

#if defined(WAIFU_FM_CD32X) && WAIFU_CD32X_BOARD_FLAT_TOP
static void cd32x_hspan_fast(int y, int x0, int x1, uint8_t color)
{
    if ((unsigned)y >= WAIFU_FM_HEIGHT) return;
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (x1 < 0 || x0 >= WAIFU_FM_WIDTH) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH - 1;
    {
        uint8_t *dst = framebuffer + (int32_t)y * WAIFU_FM_WIDTH + x0;
        int count = x1 - x0 + 1;
        uint16_t pair = (uint16_t)(((uint16_t)color << 8) | color);
        if ((uintptr_t)dst & 1u) {
            *dst++ = color;
            --count;
        }
        {
            uint16_t *dst16 = (uint16_t *)(uintptr_t)dst;
            int pairs = count >> 1;
            while (pairs >= 8) {
                dst16[0] = pair;
                dst16[1] = pair;
                dst16[2] = pair;
                dst16[3] = pair;
                dst16[4] = pair;
                dst16[5] = pair;
                dst16[6] = pair;
                dst16[7] = pair;
                dst16 += 8;
                pairs -= 8;
            }
            while (pairs-- > 0) *dst16++ = pair;
            if (count & 1) *(uint8_t *)(uintptr_t)dst16 = color;
        }
    }
}

static void cd32x_tri_edge_intersect(ScreenPt a, ScreenPt b, int y, int *xs, int *count)
{
    int miny;
    int maxy;
    if (a.y == b.y) return;
    miny = a.y < b.y ? a.y : b.y;
    maxy = a.y > b.y ? a.y : b.y;
    if (y < miny || y >= maxy) return;
    if (*count >= 3) return;
    xs[(*count)++] = a.x + (int)(((int32_t)(y - a.y) * (int32_t)(b.x - a.x)) / (int32_t)(b.y - a.y));
}

static void cd32x_fill_solid_tri_fast(ScreenPt a, ScreenPt b, ScreenPt c, uint8_t color)
{
    int miny;
    int maxy;
    if (!a.ok || !b.ok || !c.ok) return;
    if (a.x < -1024 || a.x > WAIFU_FM_WIDTH + 1024 ||
        b.x < -1024 || b.x > WAIFU_FM_WIDTH + 1024 ||
        c.x < -1024 || c.x > WAIFU_FM_WIDTH + 1024) return;
    miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (miny < 0) miny = 0;
    if (maxy >= WAIFU_FM_HEIGHT) maxy = WAIFU_FM_HEIGHT - 1;
    for (int y = miny; y <= maxy; ++y) {
        int xs[3];
        int count = 0;
        cd32x_tri_edge_intersect(a, b, y, xs, &count);
        cd32x_tri_edge_intersect(b, c, y, xs, &count);
        cd32x_tri_edge_intersect(c, a, y, xs, &count);
        if (count >= 2) cd32x_hspan_fast(y, xs[0], xs[1], color);
    }
}

static uint8_t cd32x_board_top_color(int tile)
{
    return (tile == 5) ? IDX_GOLD_DARK : IDX_CARD_GOLD;
}

static void draw_board_top_quad_flat_projected(ScreenPt pa, ScreenPt pb, ScreenPt pc, ScreenPt pd, int tile)
{
    uint8_t color = cd32x_board_top_color(tile);
    cd32x_fill_solid_tri_fast(pa, pb, pc, color);
    cd32x_fill_solid_tri_fast(pa, pc, pd, color);
}
#endif

static void draw_wall_quad3d_fast_projected(ScreenPt pa, ScreenPt pb, ScreenPt pc, ScreenPt pd, int tile)
{
    if (!pa.ok || !pb.ok || !pc.ok || !pd.ok) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
    const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
    /* Slab-wall corners may project slightly outside the viewport.  Hard
       clamping to 0..WAIFU_FM_WIDTH-1 / 0..WAIFU_FM_HEIGHT-1 folds the lower-left table side into a visible
       notch; passing the raw +-8192 guard coords is too slow for the compact
       affine quad walker.  Keep a small off-screen apron instead: the span
       drawer clips the actual pixels, while the wall edges retain their slope. */
#define WALL_APRON_X 64
#define WALL_APRON_Y 16
#define WALL_CLAMP_X(v) ((v) < -WALL_APRON_X ? -WALL_APRON_X : ((v) >= WAIFU_FM_WIDTH + WALL_APRON_X ? WAIFU_FM_WIDTH + WALL_APRON_X - 1 : (v)))
#define WALL_CLAMP_Y(v) ((v) < -WALL_APRON_Y ? -WALL_APRON_Y : ((v) >= WAIFU_FM_HEIGHT + WALL_APRON_Y ? WAIFU_FM_HEIGHT + WALL_APRON_Y - 1 : (v)))
    Point2D p0 = {(DEFAULT_INT)WALL_CLAMP_X(pa.x), (DEFAULT_INT)WALL_CLAMP_Y(pa.y), 0, 0};
    Point2D p1 = {(DEFAULT_INT)WALL_CLAMP_X(pb.x), (DEFAULT_INT)WALL_CLAMP_Y(pb.y), uvmax, 0};
    Point2D p2 = {(DEFAULT_INT)WALL_CLAMP_X(pc.x), (DEFAULT_INT)WALL_CLAMP_Y(pc.y), uvmax, uvmax};
    Point2D p3 = {(DEFAULT_INT)WALL_CLAMP_X(pd.x), (DEFAULT_INT)WALL_CLAMP_Y(pd.y), 0, uvmax};
#undef WALL_CLAMP_Y
#undef WALL_CLAMP_X
#undef WALL_APRON_Y
#undef WALL_APRON_X
    cfx_renderer3d_draw_quad_fast_affine(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
}

typedef struct BoardProjected {
    ScreenPt top[BOARD_ROWS + 1][BOARD_COLS + 1];
    ScreenPt bottom_z0[BOARD_COLS + 1];
    ScreenPt bottom_z1[BOARD_COLS + 1];
    ScreenPt bottom_x0[BOARD_ROWS + 1];
    ScreenPt bottom_x1[BOARD_ROWS + 1];
} BoardProjected;

static void build_board_projected(Camera cam, BoardProjected *bp)
{
    CameraBasis basis = make_camera_basis(cam);
    for (int r = 0; r <= BOARD_ROWS; ++r) {
        for (int c = 0; c <= BOARD_COLS; ++c) {
            bp->top[r][c] = project_point_basis(&basis, v3(col_x0(c), FIELD_Y, row_z0(r)));
        }
    }
    for (int c = 0; c <= BOARD_COLS; ++c) {
        bp->bottom_z0[c] = project_point_basis(&basis, v3(col_x0(c), FIELD_THICK, FIELD_Z0));
        bp->bottom_z1[c] = project_point_basis(&basis, v3(col_x0(c), FIELD_THICK, FIELD_Z1));
    }
    for (int r = 0; r <= BOARD_ROWS; ++r) {
        bp->bottom_x0[r] = project_point_basis(&basis, v3(FIELD_X0, FIELD_THICK, row_z0(r)));
        bp->bottom_x1[r] = project_point_basis(&basis, v3(FIELD_X1, FIELD_THICK, row_z0(r)));
    }
}

static void draw_field_slab_x_wall_fast(const BoardProjected *bp, int side)
{
    if (side == 0) {
        for (int r = 0; r < BOARD_ROWS; ++r)
            draw_wall_quad3d_fast_projected(bp->top[r][0], bp->top[r+1][0], bp->bottom_x0[r+1], bp->bottom_x0[r], field_side_tile_for_cell(0, r));
    } else {
        for (int r = 0; r < BOARD_ROWS; ++r)
            draw_wall_quad3d_fast_projected(bp->top[r][BOARD_COLS], bp->top[r+1][BOARD_COLS], bp->bottom_x1[r+1], bp->bottom_x1[r], field_side_tile_for_cell(BOARD_COLS - 1, r));
    }
}

static void draw_field_slab_facing_z_wall_fast(Camera cam, const BoardProjected *bp)
{
    if (cam.eye.z >= 0) {
        for (int c = 0; c < BOARD_COLS; ++c)
            draw_wall_quad3d_fast_projected(bp->top[BOARD_ROWS][c], bp->top[BOARD_ROWS][c+1], bp->bottom_z1[c+1], bp->bottom_z1[c], field_side_tile_for_cell(c, BOARD_ROWS - 1));
    } else {
        for (int c = 0; c < BOARD_COLS; ++c)
            draw_wall_quad3d_fast_projected(bp->top[0][c], bp->top[0][c+1], bp->bottom_z0[c+1], bp->bottom_z0[c], field_side_tile_for_cell(c, 0));
    }
}

static void draw_field_slab_sides_fast(Camera cam, const BoardProjected *bp)
{
    /* Only the camera-facing Z wall is externally visible once the board top is
       drawn; the far Z wall is fully hidden by the top surface but still cost 5
       textured quads.  Keep both X walls because the centered battle cameras can
       see both side lips.  This keeps the table textured, trims dead work, and
       avoids the old lower-left corner fold caused by screen-clamped wall verts. */
    if (cam.eye.x >= 0) {
        draw_field_slab_x_wall_fast(bp, 0);
        draw_field_slab_x_wall_fast(bp, 1);
    } else {
        draw_field_slab_x_wall_fast(bp, 1);
        draw_field_slab_x_wall_fast(bp, 0);
    }
    draw_field_slab_facing_z_wall_fast(cam, bp);
}

#if defined(WAIFU_FM_CD32X) && defined(WAIFU_BOARD_FAST_AFFINE_ENABLE)
enum {
    CD32X_BOARD_JOB_IDLE = 0,
    CD32X_BOARD_JOB_RENDER_ROWS = 1,
    CD32X_BOARD_JOB_RENDER_X_WALL = 2,
    CD32X_BOARD_JOB_DONE = 3,
    CD32X_BOARD_SLAVE_READY = 0x57335832u
};

typedef struct Cd32xBoardJob {
    volatile uint32_t ready;
    volatile uint32_t command;
    volatile int32_t row_start;
    volatile int32_t row_end;
    volatile int32_t side;
    BoardProjected bp;
} Cd32xBoardJob;

static Cd32xBoardJob g_cd32x_board_job __attribute__((aligned(16)));

static Cd32xBoardJob *cd32x_board_job_uncached(void)
{
    uintptr_t addr = (uintptr_t)&g_cd32x_board_job;
    if (addr >= WAIFU_CD32X_SDRAM_CACHED_BASE && addr < WAIFU_CD32X_SDRAM_CACHED_LIMIT) {
        addr = (addr - WAIFU_CD32X_SDRAM_CACHED_BASE) + WAIFU_CD32X_SDRAM_UNCACHED_BASE;
    }
    return (Cd32xBoardJob *)addr;
}

static int cd32x_render_board_sides_parallel(Camera cam, const BoardProjected *bp)
{
    Cd32xBoardJob *job = cd32x_board_job_uncached();
    int slave_side = (cam.eye.x >= 0) ? 0 : 1;
    int master_side = slave_side ^ 1;
    if (job->ready != CD32X_BOARD_SLAVE_READY || job->command != CD32X_BOARD_JOB_IDLE) {
        return 0;
    }

    job->bp = *bp;
    job->side = slave_side;
    __asm__ volatile ("" ::: "memory");
    job->command = CD32X_BOARD_JOB_RENDER_X_WALL;

    draw_field_slab_x_wall_fast(bp, master_side);

    while (job->command != CD32X_BOARD_JOB_DONE) {
    }
    job->command = CD32X_BOARD_JOB_IDLE;
    draw_field_slab_facing_z_wall_fast(cam, bp);
    return 1;
}

static void render_board_top_rows_projected(const BoardProjected *bp, int row_start, int row_end)
{
    if (row_start < 0) row_start = 0;
    if (row_end > BOARD_ROWS) row_end = BOARD_ROWS;
    for (int r = row_start; r < row_end; ++r) {
        for (int c = 0; c < BOARD_COLS; ++c) {
            int tile = ((r + c) & 1) ? 5 : 1;
#if defined(WAIFU_FM_CD32X) && WAIFU_CD32X_BOARD_FLAT_TOP
            draw_board_top_quad_flat_projected(bp->top[r][c], bp->top[r][c+1],
                                               bp->top[r+1][c+1], bp->top[r+1][c], tile);
#else
            draw_quad3d_fast_projected(bp->top[r][c], bp->top[r][c+1],
                                       bp->top[r+1][c+1], bp->top[r+1][c], tile);
#endif
        }
    }
}

static int cd32x_render_board_top_parallel(const BoardProjected *bp)
{
    Cd32xBoardJob *job = cd32x_board_job_uncached();
    const int split = BOARD_ROWS / 2;
    if (job->ready != CD32X_BOARD_SLAVE_READY || job->command != CD32X_BOARD_JOB_IDLE) {
        return 0;
    }

    job->bp = *bp;
    job->row_start = split;
    job->row_end = BOARD_ROWS;
    __asm__ volatile ("" ::: "memory");
    job->command = CD32X_BOARD_JOB_RENDER_ROWS;

    render_board_top_rows_projected(bp, 0, split);

    while (job->command != CD32X_BOARD_JOB_DONE) {
    }
    job->command = CD32X_BOARD_JOB_IDLE;
    return 1;
}

void waifu_cd32x_slave_service(void)
{
    Cd32xBoardJob *job = cd32x_board_job_uncached();
    job->ready = CD32X_BOARD_SLAVE_READY;
    if (job->command == CD32X_BOARD_JOB_RENDER_ROWS) {
        int row_start = (int)job->row_start;
        int row_end = (int)job->row_end;
        render_board_top_rows_projected(&job->bp, row_start, row_end);
        __asm__ volatile ("" ::: "memory");
        job->command = CD32X_BOARD_JOB_DONE;
    } else if (job->command == CD32X_BOARD_JOB_RENDER_X_WALL) {
        draw_field_slab_x_wall_fast(&job->bp, (int)job->side);
        __asm__ volatile ("" ::: "memory");
        job->command = CD32X_BOARD_JOB_DONE;
    }
}
#endif


static int field_side_tile_for_cell(int c, int r)
{
    (void)c; (void)r;
    /* Dedicated side-wall material.  The wall is segmented per board cell, so
       this texture repeats locally instead of stretching across the full edge. */
    return 4;
}

/* Slab-wall quad.  Unlike draw_quad3d (floor) this does NOT clamp the projected
   corners to the screen: on PC-FX the compact cfx_board_tri clips per scanline,
   so the walls share the single hot board rasterizer with the floor (I-cache
   locality) and need no separate clipping path. */
static void draw_wall_quad3d(Camera cam, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int tile)
{
    ScreenPt pa, pb, pc, pd;
    if (!project_quad3d(cam, a, b, c, d, &pa, &pb, &pc, &pd)) return;
    if (tile < 0) tile = 0;
    if (tile >= WAIFU_TEX_TILE_COUNT) tile = WAIFU_TEX_TILE_COUNT - 1;
#if defined(WAIFU_FM_PCFX)
    {
        /* Pass the TRUE projected corners (do NOT clamp them to the screen rect):
           on a low/perspective camera (result screen, story duels) the wall's
           bottom corners project below the screen, and clamping them to y=WAIFU_FM_HEIGHT-1
           pulled the triangle into jagged spikes.  cfx_board_tri clips per scanline
           and bounds the edge math internally, so off-screen corners render the
           correct on-screen slab edge. */
        const DEFAULT_INT uvmax = (DEFAULT_INT)((WAIFU_TEX_TILE_SIZE - 1) << 8);
        Point2D p0 = {(DEFAULT_INT)pa.x, (DEFAULT_INT)pa.y, 0, 0};
        Point2D p1 = {(DEFAULT_INT)pb.x, (DEFAULT_INT)pb.y, uvmax, 0};
        Point2D p2 = {(DEFAULT_INT)pc.x, (DEFAULT_INT)pc.y, uvmax, uvmax};
        Point2D p3 = {(DEFAULT_INT)pd.x, (DEFAULT_INT)pd.y, 0, uvmax};
        cfx_renderer3d_draw_quad_board(&renderer, &p0, &p1, &p2, &p3, (DEFAULT_INT)tile);
    }
#else
    {
        const uint8_t *src = waifu_texture_atlas + ((size_t)tile * WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
        TexV t0 = {pa.x, pa.y, 0, 0};
        TexV t1 = {pb.x, pb.y, Q8_ONE, 0};
        TexV t2 = {pc.x, pc.y, Q8_ONE, Q8_ONE};
        TexV t3 = {pd.x, pd.y, 0, Q8_ONE};
        draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t1, t2);
        draw_textured_tri(src, WAIFU_TEX_TILE_SIZE, WAIFU_TEX_TILE_SIZE, t0, t2, t3);
    }
#endif
}

static void draw_field_wall_z(Camera cam, int32_t z, int r_for_tile)
{
    int c;
    for (c = 0; c < BOARD_COLS; ++c) {
        int32_t x0 = col_x0(c), x1 = col_x0(c + 1);
        int tile = field_side_tile_for_cell(c, r_for_tile);
        draw_wall_quad3d(cam, v3(x0, FIELD_Y, z), v3(x1, FIELD_Y, z),
                              v3(x1, FIELD_THICK, z), v3(x0, FIELD_THICK, z), tile);
    }
}

static void draw_field_wall_x(Camera cam, int32_t x, int c_for_tile)
{
    int r;
    for (r = 0; r < BOARD_ROWS; ++r) {
        int32_t z0 = row_z0(r), z1 = row_z0(r + 1);
        int tile = field_side_tile_for_cell(c_for_tile, r);
        draw_wall_quad3d(cam, v3(x, FIELD_Y, z0), v3(x, FIELD_Y, z1),
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

static inline void fill_u8_fast(uint8_t *dst, int count, uint8_t c)
{
    if (count <= 0) return;
#if defined(WAIFU_FM_CD32X)
    uint16_t pair = (uint16_t)(((uint16_t)c << 8) | c);
    if ((uintptr_t)dst & 1u) {
        *dst++ = c;
        --count;
    }
    {
        uint16_t *dst16 = (uint16_t *)(uintptr_t)dst;
        int pairs = count >> 1;
        while (pairs >= 8) {
            dst16[0] = pair;
            dst16[1] = pair;
            dst16[2] = pair;
            dst16[3] = pair;
            dst16[4] = pair;
            dst16[5] = pair;
            dst16[6] = pair;
            dst16[7] = pair;
            dst16 += 8;
            pairs -= 8;
        }
        while (pairs-- > 0) *dst16++ = pair;
        if (count & 1) *(uint8_t *)(uintptr_t)dst16 = c;
    }
#elif defined(WAIFU_FM_PCFX)
    uint32_t n = (uint32_t)count;
    uint32_t v = (uint32_t)c;
    uint32_t groups;
    __asm__ volatile (
        "mov %[n],%[groups]\n"
        "shr 3,%[groups]\n"
        "cmp 0,%[groups]\n"
        "be 2f\n"
        "1:\n"
        "st.b %[v],0[%[dst]]\n"
        "st.b %[v],1[%[dst]]\n"
        "st.b %[v],2[%[dst]]\n"
        "st.b %[v],3[%[dst]]\n"
        "st.b %[v],4[%[dst]]\n"
        "st.b %[v],5[%[dst]]\n"
        "st.b %[v],6[%[dst]]\n"
        "st.b %[v],7[%[dst]]\n"
        "add 8,%[dst]\n"
        "add -1,%[groups]\n"
        "bne 1b\n"
        "2:\n"
        "andi 7,%[n],%[n]\n"
        "cmp 0,%[n]\n"
        "be 4f\n"
        "3:\n"
        "st.b %[v],0[%[dst]]\n"
        "add 1,%[dst]\n"
        "add -1,%[n]\n"
        "bne 3b\n"
        "4:\n"
        : [dst] "+r" (dst), [n] "+r" (n), [groups] "=&r" (groups)
        : [v] "r" (v)
        : "memory");
    PROFILE_UI_FAST_FILL();
#else
    memset(dst, c, (size_t)count);
#endif
}

#if defined(WAIFU_FM_CD32X)
static void clear_screen(uint8_t c) { waifu_cd32x_video_clear_back_index(c); }
#else
static void clear_screen(uint8_t c) { fill_u8_fast(framebuffer, WAIFU_FM_WIDTH * WAIFU_FM_HEIGHT, c); }
#endif

static inline void copy_u8_fast(uint8_t *dst, const uint8_t *src, int count)
{
    if (count <= 0) return;
#if defined(WAIFU_FM_CD32X)
    if ((uintptr_t)dst & 1u) {
        *dst++ = *src++;
        --count;
    }
    {
        uint16_t *dst16 = (uint16_t *)(uintptr_t)dst;
        int pairs = count >> 1;
        while (pairs >= 8) {
            dst16[0] = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
            dst16[1] = (uint16_t)(((uint16_t)src[2] << 8) | src[3]);
            dst16[2] = (uint16_t)(((uint16_t)src[4] << 8) | src[5]);
            dst16[3] = (uint16_t)(((uint16_t)src[6] << 8) | src[7]);
            dst16[4] = (uint16_t)(((uint16_t)src[8] << 8) | src[9]);
            dst16[5] = (uint16_t)(((uint16_t)src[10] << 8) | src[11]);
            dst16[6] = (uint16_t)(((uint16_t)src[12] << 8) | src[13]);
            dst16[7] = (uint16_t)(((uint16_t)src[14] << 8) | src[15]);
            src += 16;
            dst16 += 8;
            pairs -= 8;
        }
        while (pairs-- > 0) {
            *dst16++ = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
            src += 2;
        }
        if (count & 1) *(uint8_t *)(uintptr_t)dst16 = *src;
    }
#elif defined(WAIFU_FM_PCFX)
    uint32_t n = (uint32_t)count;
    uint32_t groups;
    /* Framebuffer/cache copies are aligned and large.  Keep the body compact:
       32 bytes per loop, eight ld.w/st.w pairs, well under the 1 KiB V810
       I-cache while avoiding a slow byte copy or unknown libc memcpy. */
    __asm__ volatile (
        "mov %[n],%[groups]\n"
        "shr 5,%[groups]\n"
        "cmp 0,%[groups]\n"
        "be 2f\n"
        "1:\n"
        "ld.w 0[%[src]],r10\n"
        "st.w r10,0[%[dst]]\n"
        "ld.w 4[%[src]],r10\n"
        "st.w r10,4[%[dst]]\n"
        "ld.w 8[%[src]],r10\n"
        "st.w r10,8[%[dst]]\n"
        "ld.w 12[%[src]],r10\n"
        "st.w r10,12[%[dst]]\n"
        "ld.w 16[%[src]],r10\n"
        "st.w r10,16[%[dst]]\n"
        "ld.w 20[%[src]],r10\n"
        "st.w r10,20[%[dst]]\n"
        "ld.w 24[%[src]],r10\n"
        "st.w r10,24[%[dst]]\n"
        "ld.w 28[%[src]],r10\n"
        "st.w r10,28[%[dst]]\n"
        "addi 32,%[src],%[src]\n"
        "addi 32,%[dst],%[dst]\n"
        "add -1,%[groups]\n"
        "bne 1b\n"
        "2:\n"
        "andi 31,%[n],%[n]\n"
        "cmp 0,%[n]\n"
        "be 4f\n"
        "3:\n"
        "ld.b 0[%[src]],r10\n"
        "st.b r10,0[%[dst]]\n"
        "add 1,%[src]\n"
        "add 1,%[dst]\n"
        "add -1,%[n]\n"
        "bne 3b\n"
        "4:\n"
        : [dst] "+r" (dst), [src] "+r" (src), [n] "+r" (n), [groups] "=&r" (groups)
        :
        : "r10", "memory");
#else
    memcpy(dst, src, (size_t)count);
#endif
}


static void put_px(int x, int y, uint8_t c)
{
    if ((unsigned)x < WAIFU_FM_WIDTH && (unsigned)y < WAIFU_FM_HEIGHT) framebuffer[y * WAIFU_FM_WIDTH + x] = c;
}

static void hline(int x0, int x1, int y, uint8_t c)
{
    if ((unsigned)y >= WAIFU_FM_HEIGHT) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= WAIFU_FM_WIDTH) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH - 1;
    fill_u8_fast(framebuffer + y * WAIFU_FM_WIDTH + x0, x1 - x0 + 1, c);
}

static void rect_fill(int x, int y, int w, int h, uint8_t c)
{
    if (w <= 0 || h <= 0) return;
    int x0 = x;
    int y0 = y;
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    if (x1 < 0 || y1 < 0 || x0 >= WAIFU_FM_WIDTH || y0 >= WAIFU_FM_HEIGHT) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH - 1;
    if (y1 >= WAIFU_FM_HEIGHT) y1 = WAIFU_FM_HEIGHT - 1;
    int count = x1 - x0 + 1;
    uint8_t *dst = framebuffer + y0 * WAIFU_FM_WIDTH + x0;
    for (int yy = y0; yy <= y1; ++yy) {
        fill_u8_fast(dst, count, c);
        dst += WAIFU_FM_WIDTH;
    }
}

static void rect_outline(int x, int y, int w, int h, uint8_t c)
{
    hline(x, x+w-1, y, c); hline(x, x+w-1, y+h-1, c);
    for (int yy = y; yy < y+h; ++yy) { put_px(x, yy, c); put_px(x+w-1, yy, c); }
}

static void line_i(int x0, int y0, int x1, int y1, uint8_t c)
{
    /* Clip the segment to the screen rect FIRST (Liang-Barsky, 64-bit so it
       survives huge inputs).  A projected vertex just in front of the camera
       (small cz, large cx/cz) can land at a coordinate in the millions; the old
       unclipped Bresenham then looped ~millions of times -- put_px() silently
       drops out-of-bounds writes, so it just spun forever and hung the game
       (repro: COM equips a monster then attacks).  After clipping the loop only
       walks the on-screen span. */
    long long ax = x0, ay = y0, dx = (long long)x1 - x0, dy = (long long)y1 - y0;
    if (dx == 0 && dy == 0) { put_px(x0, y0, c); return; }
    {
        long long t0 = 0, t1 = 1LL << 16;
        long long p[4] = { -dx, dx, -dy, dy };
        long long q[4] = { ax, (long long)(WAIFU_FM_WIDTH - 1) - ax, ay, (long long)(WAIFU_FM_HEIGHT - 1) - ay };
        int i;
        for (i = 0; i < 4; ++i) {
            if (p[i] == 0) { if (q[i] < 0) return; continue; }
            {
                long long r = (q[i] * (1LL << 16)) / p[i];
                if (p[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
                else          { if (r < t0) return; if (r < t1) t1 = r; }
            }
        }
        if (t0 > t1) return;
        x0 = (int)(ax + (dx * t0) / (1LL << 16));
        y0 = (int)(ay + (dy * t0) / (1LL << 16));
        x1 = (int)(ax + (dx * t1) / (1LL << 16));
        y1 = (int)(ay + (dy * t1) / (1LL << 16));
    }
    {
        int adx = i_abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int ady = -i_abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = adx + ady;
        for (;;) {
            put_px(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= ady) { err += ady; x0 += sx; }
            if (e2 <= adx) { err += adx; y0 += sy; }
        }
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
       only 6 columns, which clipped wide glyphs such as M and WAIFU_FM_WIDTH whenever they
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

static void draw_masked_bitmap(const uint8_t *pix, const uint8_t *mask, int sw, int sh, int x, int y)
{
    for (int yy = 0; yy < sh; ++yy) {
        int dy = y + yy;
        if ((unsigned)dy >= WAIFU_FM_HEIGHT) continue;
        for (int xx = 0; xx < sw; ++xx) {
            int dx = x + xx;
            if ((unsigned)dx >= WAIFU_FM_WIDTH) continue;
            int idx = yy * sw + xx;
            if (mask[idx]) put_px(dx, dy, pix[idx]);
        }
    }
}

static void draw_story_portrait(int portrait_id, int x, int y)
{
    if (portrait_id < 0 || portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return;
    const uint8_t *pix = waifu_assets_story_portrait_pixels(portrait_id);
    const uint8_t *mask = waifu_assets_story_portrait_mask(portrait_id);
    if (!pix || !mask) return;
    draw_masked_bitmap(pix, mask, WAIFU_STORY_PORTRAIT_W, WAIFU_STORY_PORTRAIT_H, x, y);
}

static int story_slide_x(int from_x, int to_x, int frame)
{
    return lerp_i(from_x, to_x, q8_smooth_ratio(frame, 26));
}

static void draw_story_dialog_box(const char *speaker, const char *subhead, const char *text, uint8_t speaker_color, int f)
{
    int box_y = WAIFU_FM_HEIGHT - 66;
    (void)f;
    draw_panel_rect(0, box_y, WAIFU_FM_WIDTH, 66, IDX_UI_DARK);
    draw_text_small(10, box_y + 10, speaker, speaker_color, IDX_BLACK);
    if (subhead && *subhead) draw_text_small_ellipsis(108, box_y + 10, subhead, 20, IDX_UI_LIGHT, IDX_BLACK);
    draw_wrapped_text_small_box(10, box_y + 25, WAIFU_FM_WIDTH - 20, 4, 10, text, IDX_WHITE, IDX_BLACK);
}

static void fmt_i32_dec(char *dst, int dst_size, int value);
static void fmt_lp5(char out[6], int value);
static void fmt_prefixed_i32(char *dst, int dst_size, char prefix, int value);

static void draw_hud_offset(int field_ox, int field_oy, int lp_ox, int lp_oy)
{
    char lpbuf[16];
    draw_panel_rect(6 + field_ox, 7 + field_oy, 49, 29, IDX_UI_DARK);
    draw_text_small(11 + field_ox, 11 + field_oy, "FIELD", IDX_WHITE, IDX_BLACK);
    draw_text(11 + field_ox, 23 + field_oy, story_water_field_active() ? "WATER" : "MARE", IDX_WHITE, IDX_BLACK);

    /* Anchor the LP panel to the right edge: shifts right by the extra width on
       wider framebuffers, stays put on the 256-wide layout. */
    lp_ox += WAIFU_UI_EXTRA_W;
    draw_panel_rect(177 + lp_ox, 7 + lp_oy, 71, 12, IDX_UI_DARK);
    rect_fill(179 + lp_ox, 9 + lp_oy, 23, 8, IDX_UI_BLUE);
    draw_text_small(181 + lp_ox, 9 + lp_oy, "COM", IDX_WHITE, IDX_BLACK);
    fmt_lp5(lpbuf, g_com_lp);
    draw_text_small(209 + lp_ox, 9 + lp_oy, lpbuf, IDX_GOLD_HI, IDX_BLACK);

    draw_panel_rect(177 + lp_ox, 23 + lp_oy, 71, 12, IDX_UI_DARK);
    rect_fill(179 + lp_ox, 25 + lp_oy, 23, 8, IDX_UI_RED);
    draw_text_small(181 + lp_ox, 25 + lp_oy, "YOU", IDX_WHITE, IDX_BLACK);
    fmt_lp5(lpbuf, g_you_lp);
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

static void fmt_u32_dec(char *dst, int dst_size, unsigned value)
{
    char tmp[10];
    int n = 0;
    int i;
    if (dst_size <= 0) return;
    if (value == 0) {
        if (dst_size > 1) { dst[0] = '0'; dst[1] = '\0'; }
        else dst[0] = '\0';
        return;
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (i = 0; i < n && i + 1 < dst_size; ++i) dst[i] = tmp[n - 1 - i];
    dst[i] = '\0';
}

static void fmt_i32_dec(char *dst, int dst_size, int value)
{
    unsigned u;
    if (dst_size <= 0) return;
    if (value < 0) {
        dst[0] = '-';
        if (value == (int)0x80000000u) u = 2147483648u;
        else u = (unsigned)(-value);
        fmt_u32_dec(dst + 1, dst_size - 1, u);
    } else {
        fmt_u32_dec(dst, dst_size, (unsigned)value);
    }
}

static void fmt_lp5(char out[6], int value)
{
    char tmp[12];
    int len = 0;
    int pad;
    fmt_i32_dec(tmp, (int)sizeof(tmp), value);
    while (tmp[len]) ++len;
    if (len > 5) {
        out[0] = tmp[len - 5]; out[1] = tmp[len - 4]; out[2] = tmp[len - 3]; out[3] = tmp[len - 2]; out[4] = tmp[len - 1]; out[5] = '\0';
        return;
    }
    for (pad = 0; pad < 5 - len; ++pad) out[pad] = ' ';
    for (int i = 0; i < len; ++i) out[pad + i] = tmp[i];
    out[5] = '\0';
}

static void fmt_prefixed_i32(char *dst, int dst_size, char prefix, int value)
{
    if (dst_size <= 0) return;
    if (dst_size == 1) { dst[0] = '\0'; return; }
    dst[0] = prefix;
    fmt_i32_dec(dst + 1, dst_size - 1, value);
}

static int waifu_cstrlen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static void waifu_str_copy(char *dst, int dst_size, const char *src)
{
    int i = 0;
    if (dst_size <= 0) return;
    if (!src) src = "";
    while (src[i] && i + 1 < dst_size) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void waifu_str_copy_n(char *dst, int dst_size, const char *src, int max_chars)
{
    int i = 0;
    if (dst_size <= 0) return;
    if (!src || max_chars <= 0) { dst[0] = '\0'; return; }
    while (src[i] && i < max_chars && i + 1 < dst_size) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static void waifu_str_cat(char *dst, int dst_size, const char *src)
{
    int d = 0;
    if (dst_size <= 0) return;
    while (d < dst_size && dst[d]) ++d;
    if (d >= dst_size) { dst[dst_size - 1] = '\0'; return; }
    if (!src) src = "";
    while (*src && d + 1 < dst_size) dst[d++] = *src++;
    dst[d] = '\0';
}

static void waifu_str_cat_char(char *dst, int dst_size, char c)
{
    int d = 0;
    if (dst_size <= 0) return;
    while (d < dst_size && dst[d]) ++d;
    if (d + 1 >= dst_size) { dst[dst_size - 1] = '\0'; return; }
    dst[d++] = c;
    dst[d] = '\0';
}

static void waifu_str_cat_i32(char *dst, int dst_size, int value)
{
    char tmp[16];
    fmt_i32_dec(tmp, (int)sizeof(tmp), value);
    waifu_str_cat(dst, dst_size, tmp);
}

static void waifu_str_cat_u32(char *dst, int dst_size, unsigned value)
{
    char tmp[16];
    fmt_u32_dec(tmp, (int)sizeof(tmp), value);
    waifu_str_cat(dst, dst_size, tmp);
}

static void waifu_str_cat_u32_z2(char *dst, int dst_size, unsigned value)
{
    char tmp[16];
    if (value < 10u) waifu_str_cat_char(dst, dst_size, '0');
    fmt_u32_dec(tmp, (int)sizeof(tmp), value);
    waifu_str_cat(dst, dst_size, tmp);
}

static void waifu_str_cat_u32_z4(char *dst, int dst_size, unsigned value)
{
    char tmp[16];
    int len, pad;
    fmt_u32_dec(tmp, (int)sizeof(tmp), value);
    len = waifu_cstrlen(tmp);
    for (pad = len; pad < 4; ++pad) waifu_str_cat_char(dst, dst_size, '0');
    waifu_str_cat(dst, dst_size, tmp);
}

static void waifu_str_cat_u32_zw(char *dst, int dst_size, unsigned value, int width)
{
    char tmp[16];
    int len, pad;
    fmt_u32_dec(tmp, (int)sizeof(tmp), value);
    len = waifu_cstrlen(tmp);
    for (pad = len; pad < width; ++pad) waifu_str_cat_char(dst, dst_size, '0');
    waifu_str_cat(dst, dst_size, tmp);
}

static void fmt_join2(char *dst, int dst_size, const char *a, const char *sep, const char *b)
{
    if (dst_size <= 0) return;
    dst[0] = '\0';
    waifu_str_cat(dst, dst_size, a);
    waifu_str_cat(dst, dst_size, sep);
    waifu_str_cat(dst, dst_size, b);
}

static void fmt_label_u32(char *dst, int dst_size, const char *label, unsigned value)
{
    if (dst_size <= 0) return;
    dst[0] = '\0';
    waifu_str_cat(dst, dst_size, label);
    waifu_str_cat_char(dst, dst_size, ' ');
    waifu_str_cat_u32(dst, dst_size, value);
}

static void fmt_label_i32(char *dst, int dst_size, const char *label, int value)
{
    if (dst_size <= 0) return;
    dst[0] = '\0';
    waifu_str_cat(dst, dst_size, label);
    waifu_str_cat_char(dst, dst_size, ' ');
    waifu_str_cat_i32(dst, dst_size, value);
}

static void draw_bottom_info_offset_ex(int card_id, const char *mode, int yoff, int atk, int defv)
{
    (void)mode;
    int base = WAIFU_BOTTOM_INFO_Y + yoff;
    rect_fill(0, base, WAIFU_FM_WIDTH, 35, IDX_UI_TEAL);
    hline(0,WAIFU_FM_WIDTH-1,base,IDX_WHITE); hline(0,WAIFU_FM_WIDTH-1,base+1,IDX_UI_LIGHT); hline(0,WAIFU_FM_WIDTH-1,base+2,IDX_DIM);
    for (int y = base+4; y < base+35; y += 3) hline(0,WAIFU_FM_WIDTH-1,y,IDX_UI_TEAL2);
    char line[64];
    if (is_support_card(card_id)) {
        char support_line[64];
        waifu_str_copy_n(support_line, (int)sizeof(support_line), support_card_name(card_id), 24);
        draw_text(6, base+6, support_line, IDX_WHITE, IDX_BLACK);
        waifu_str_copy_n(support_line, (int)sizeof(support_line), support_card_type(card_id), 31);
        draw_text_small(6, base+21, support_line, IDX_WHITE, IDX_BLACK);
        draw_text_small(WAIFU_FM_WIDTH - 68, base+21, "USE", IDX_GOLD_HI, IDX_BLACK);
        return;
    }
    if (!is_monster_card(card_id)) return;
    waifu_str_copy_n(line, (int)sizeof(line), waifu_card_names[card_id], 24);
    draw_text(6, base+6, line, IDX_WHITE, IDX_BLACK);
    fmt_join2(line, (int)sizeof(line), waifu_card_attr[card_id], " / ", waifu_card_tribe[card_id]);
    draw_text_small(6, base+21, line, IDX_WHITE, IDX_BLACK);
    if (atk < 0) atk = (int)waifu_card_atk[card_id];
    if (defv < 0) defv = (int)waifu_card_def[card_id];
    fmt_prefixed_i32(line, (int)sizeof(line), 'x', atk);
    draw_text_small(WAIFU_FM_WIDTH - 41, base+15, line, stat_delta_color(atk - (int)waifu_card_atk[card_id]), IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), defv);
    draw_text_small(WAIFU_FM_WIDTH - 35, base+26, line, stat_delta_color(defv - (int)waifu_card_def[card_id]), IDX_BLACK);
    rect_outline(WAIFU_FM_WIDTH - 41,base+25,6,6,IDX_WHITE);
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
            int chroma = i_abs(rr - gg) + i_abs(gg - bb) + i_abs(bb - rr);
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

static inline uint8_t gray_card_dither_px(uint8_t src, int x, int y)
{
    /* Used-card rendering is intentionally a half-tone over the original card,
       not a full grayscale replacement.  Hand cards and projected field cards
       both use this same screen-space checker so the "one monster/action used"
       state remains readable while still looking spent. */
    return ((x ^ y) & 1) ? gray_card_px(src) : src;
}

static const uint8_t g_card_ymap_38x50[50] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52 };
static const uint8_t g_card_ymap_36x49[49] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46, 47, 48, 49, 50, 51, 52 };
static const uint8_t g_card_xmap_36[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36 };

static uint8_t g_card_xmap_100[100];
static uint8_t g_card_ymap_112[112];
static uint8_t g_card_xmap_112[112];
static uint8_t g_card_ymap_136[136];
static uint8_t g_card_xmap_120[120];
static uint8_t g_card_ymap_160[160];
static int g_card_scale_maps_ready = 0;

static void init_card_scale_map(uint8_t *map, int dst_n, int src_n)
{
    int i;
    for (i = 0; i < dst_n; ++i) map[i] = (uint8_t)((i * src_n) / dst_n);
}

static void init_card_scale_maps(void)
{
    if (g_card_scale_maps_ready) return;
    init_card_scale_map(g_card_xmap_100, 100, WAIFU_CARD_W);
    init_card_scale_map(g_card_xmap_112, 112, WAIFU_CARD_W);
    init_card_scale_map(g_card_ymap_112, 112, WAIFU_CARD_H);
    init_card_scale_map(g_card_ymap_136, 136, WAIFU_CARD_H);
    init_card_scale_map(g_card_xmap_120, 120, WAIFU_CARD_W);
    init_card_scale_map(g_card_ymap_160, 160, WAIFU_CARD_H);
    g_card_scale_maps_ready = 1;
}

static inline int rect_fully_visible(int x, int y, int w, int h)
{
    return x >= 0 && y >= 0 && x + w <= WAIFU_FM_WIDTH && y + h <= WAIFU_FM_HEIGHT;
}

#if defined(WAIFU_FM_PCFX)
static inline __attribute__((always_inline)) void pcfx_blit_row38_v810(const uint8_t *src, uint8_t *dst)
{
    uint32_t t;
    __asm__ volatile (
        "ld.b 0[%[src]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "ld.b 1[%[src]],%[t]\n"
        "st.b %[t],1[%[dst]]\n"
        "ld.b 2[%[src]],%[t]\n"
        "st.b %[t],2[%[dst]]\n"
        "ld.b 3[%[src]],%[t]\n"
        "st.b %[t],3[%[dst]]\n"
        "ld.b 4[%[src]],%[t]\n"
        "st.b %[t],4[%[dst]]\n"
        "ld.b 5[%[src]],%[t]\n"
        "st.b %[t],5[%[dst]]\n"
        "ld.b 6[%[src]],%[t]\n"
        "st.b %[t],6[%[dst]]\n"
        "ld.b 7[%[src]],%[t]\n"
        "st.b %[t],7[%[dst]]\n"
        "ld.b 8[%[src]],%[t]\n"
        "st.b %[t],8[%[dst]]\n"
        "ld.b 9[%[src]],%[t]\n"
        "st.b %[t],9[%[dst]]\n"
        "ld.b 10[%[src]],%[t]\n"
        "st.b %[t],10[%[dst]]\n"
        "ld.b 11[%[src]],%[t]\n"
        "st.b %[t],11[%[dst]]\n"
        "ld.b 12[%[src]],%[t]\n"
        "st.b %[t],12[%[dst]]\n"
        "ld.b 13[%[src]],%[t]\n"
        "st.b %[t],13[%[dst]]\n"
        "ld.b 14[%[src]],%[t]\n"
        "st.b %[t],14[%[dst]]\n"
        "ld.b 15[%[src]],%[t]\n"
        "st.b %[t],15[%[dst]]\n"
        "ld.b 16[%[src]],%[t]\n"
        "st.b %[t],16[%[dst]]\n"
        "ld.b 17[%[src]],%[t]\n"
        "st.b %[t],17[%[dst]]\n"
        "ld.b 18[%[src]],%[t]\n"
        "st.b %[t],18[%[dst]]\n"
        "ld.b 19[%[src]],%[t]\n"
        "st.b %[t],19[%[dst]]\n"
        "ld.b 20[%[src]],%[t]\n"
        "st.b %[t],20[%[dst]]\n"
        "ld.b 21[%[src]],%[t]\n"
        "st.b %[t],21[%[dst]]\n"
        "ld.b 22[%[src]],%[t]\n"
        "st.b %[t],22[%[dst]]\n"
        "ld.b 23[%[src]],%[t]\n"
        "st.b %[t],23[%[dst]]\n"
        "ld.b 24[%[src]],%[t]\n"
        "st.b %[t],24[%[dst]]\n"
        "ld.b 25[%[src]],%[t]\n"
        "st.b %[t],25[%[dst]]\n"
        "ld.b 26[%[src]],%[t]\n"
        "st.b %[t],26[%[dst]]\n"
        "ld.b 27[%[src]],%[t]\n"
        "st.b %[t],27[%[dst]]\n"
        "ld.b 28[%[src]],%[t]\n"
        "st.b %[t],28[%[dst]]\n"
        "ld.b 29[%[src]],%[t]\n"
        "st.b %[t],29[%[dst]]\n"
        "ld.b 30[%[src]],%[t]\n"
        "st.b %[t],30[%[dst]]\n"
        "ld.b 31[%[src]],%[t]\n"
        "st.b %[t],31[%[dst]]\n"
        "ld.b 32[%[src]],%[t]\n"
        "st.b %[t],32[%[dst]]\n"
        "ld.b 33[%[src]],%[t]\n"
        "st.b %[t],33[%[dst]]\n"
        "ld.b 34[%[src]],%[t]\n"
        "st.b %[t],34[%[dst]]\n"
        "ld.b 35[%[src]],%[t]\n"
        "st.b %[t],35[%[dst]]\n"
        "ld.b 36[%[src]],%[t]\n"
        "st.b %[t],36[%[dst]]\n"
        "ld.b 37[%[src]],%[t]\n"
        "st.b %[t],37[%[dst]]\n"
        : [t] "=&r" (t)
        : [src] "r" (src), [dst] "r" (dst)
        : "memory");
}

static inline __attribute__((always_inline)) void pcfx_blit_row38_to36_v810(const uint8_t *src, uint8_t *dst)
{
    uint32_t t;
    __asm__ volatile (
        "ld.b 0[%[src]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "ld.b 1[%[src]],%[t]\n"
        "st.b %[t],1[%[dst]]\n"
        "ld.b 2[%[src]],%[t]\n"
        "st.b %[t],2[%[dst]]\n"
        "ld.b 3[%[src]],%[t]\n"
        "st.b %[t],3[%[dst]]\n"
        "ld.b 4[%[src]],%[t]\n"
        "st.b %[t],4[%[dst]]\n"
        "ld.b 5[%[src]],%[t]\n"
        "st.b %[t],5[%[dst]]\n"
        "ld.b 6[%[src]],%[t]\n"
        "st.b %[t],6[%[dst]]\n"
        "ld.b 7[%[src]],%[t]\n"
        "st.b %[t],7[%[dst]]\n"
        "ld.b 8[%[src]],%[t]\n"
        "st.b %[t],8[%[dst]]\n"
        "ld.b 9[%[src]],%[t]\n"
        "st.b %[t],9[%[dst]]\n"
        "ld.b 10[%[src]],%[t]\n"
        "st.b %[t],10[%[dst]]\n"
        "ld.b 11[%[src]],%[t]\n"
        "st.b %[t],11[%[dst]]\n"
        "ld.b 12[%[src]],%[t]\n"
        "st.b %[t],12[%[dst]]\n"
        "ld.b 13[%[src]],%[t]\n"
        "st.b %[t],13[%[dst]]\n"
        "ld.b 14[%[src]],%[t]\n"
        "st.b %[t],14[%[dst]]\n"
        "ld.b 15[%[src]],%[t]\n"
        "st.b %[t],15[%[dst]]\n"
        "ld.b 16[%[src]],%[t]\n"
        "st.b %[t],16[%[dst]]\n"
        "ld.b 17[%[src]],%[t]\n"
        "st.b %[t],17[%[dst]]\n"
        "ld.b 19[%[src]],%[t]\n"
        "st.b %[t],18[%[dst]]\n"
        "ld.b 20[%[src]],%[t]\n"
        "st.b %[t],19[%[dst]]\n"
        "ld.b 21[%[src]],%[t]\n"
        "st.b %[t],20[%[dst]]\n"
        "ld.b 22[%[src]],%[t]\n"
        "st.b %[t],21[%[dst]]\n"
        "ld.b 23[%[src]],%[t]\n"
        "st.b %[t],22[%[dst]]\n"
        "ld.b 24[%[src]],%[t]\n"
        "st.b %[t],23[%[dst]]\n"
        "ld.b 25[%[src]],%[t]\n"
        "st.b %[t],24[%[dst]]\n"
        "ld.b 26[%[src]],%[t]\n"
        "st.b %[t],25[%[dst]]\n"
        "ld.b 27[%[src]],%[t]\n"
        "st.b %[t],26[%[dst]]\n"
        "ld.b 28[%[src]],%[t]\n"
        "st.b %[t],27[%[dst]]\n"
        "ld.b 29[%[src]],%[t]\n"
        "st.b %[t],28[%[dst]]\n"
        "ld.b 30[%[src]],%[t]\n"
        "st.b %[t],29[%[dst]]\n"
        "ld.b 31[%[src]],%[t]\n"
        "st.b %[t],30[%[dst]]\n"
        "ld.b 32[%[src]],%[t]\n"
        "st.b %[t],31[%[dst]]\n"
        "ld.b 33[%[src]],%[t]\n"
        "st.b %[t],32[%[dst]]\n"
        "ld.b 34[%[src]],%[t]\n"
        "st.b %[t],33[%[dst]]\n"
        "ld.b 35[%[src]],%[t]\n"
        "st.b %[t],34[%[dst]]\n"
        "ld.b 36[%[src]],%[t]\n"
        "st.b %[t],35[%[dst]]\n"
        : [t] "=&r" (t)
        : [src] "r" (src), [dst] "r" (dst)
        : "memory");
}
static inline __attribute__((always_inline)) void pcfx_blit_row_mapped_v810(const uint8_t *src, uint8_t *dst, const uint8_t *xmap, int count)
{
    uint32_t groups = (uint32_t)count >> 2;
    uint32_t tail = (uint32_t)count & 3u;
    uint32_t idx, t, addr;
    __asm__ volatile (
        "cmp 0,%[groups]\n"
        "be 2f\n"
        "1:\n"
        "ld.b 0[%[xmap]],%[idx]\n"
        "add %[src],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "ld.b 1[%[xmap]],%[idx]\n"
        "add %[src],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],1[%[dst]]\n"
        "ld.b 2[%[xmap]],%[idx]\n"
        "add %[src],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],2[%[dst]]\n"
        "ld.b 3[%[xmap]],%[idx]\n"
        "add %[src],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],3[%[dst]]\n"
        "add 4,%[xmap]\n"
        "add 4,%[dst]\n"
        "add -1,%[groups]\n"
        "bne 1b\n"
        "2:\n"
        "cmp 0,%[tail]\n"
        "be 4f\n"
        "3:\n"
        "ld.b 0[%[xmap]],%[idx]\n"
        "add %[src],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "add 1,%[xmap]\n"
        "add 1,%[dst]\n"
        "add -1,%[tail]\n"
        "bne 3b\n"
        "4:\n"
        : [dst] "+r" (dst), [xmap] "+r" (xmap), [groups] "+r" (groups), [tail] "+r" (tail),
          [idx] "=&r" (idx), [t] "=&r" (t), [addr] "=&r" (addr)
        : [src] "r" (src)
        : "memory");
    (void)addr;
}

static inline __attribute__((always_inline)) void pcfx_blit_row_gray_v810(const uint8_t *src, uint8_t *dst, const uint8_t *lut, int count)
{
    uint32_t groups = (uint32_t)count >> 2;
    uint32_t tail = (uint32_t)count & 3u;
    uint32_t sp, dp, idx, t;
    __asm__ volatile (
        "mov %[src_in],%[src]\n"
        "mov %[dst_in],%[dst]\n"
        "cmp 0,%[groups]\n"
        "be 2f\n"
        "1:\n"
        "ld.b 0[%[src]],%[idx]\n"
        "andi 255,%[idx],%[idx]\n"
        "add %[lut],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "ld.b 1[%[src]],%[idx]\n"
        "andi 255,%[idx],%[idx]\n"
        "add %[lut],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],1[%[dst]]\n"
        "ld.b 2[%[src]],%[idx]\n"
        "andi 255,%[idx],%[idx]\n"
        "add %[lut],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],2[%[dst]]\n"
        "ld.b 3[%[src]],%[idx]\n"
        "andi 255,%[idx],%[idx]\n"
        "add %[lut],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],3[%[dst]]\n"
        "add 4,%[src]\n"
        "add 4,%[dst]\n"
        "add -1,%[groups]\n"
        "bne 1b\n"
        "2:\n"
        "cmp 0,%[tail]\n"
        "be 4f\n"
        "3:\n"
        "ld.b 0[%[src]],%[idx]\n"
        "andi 255,%[idx],%[idx]\n"
        "add %[lut],%[idx]\n"
        "ld.b 0[%[idx]],%[t]\n"
        "st.b %[t],0[%[dst]]\n"
        "add 1,%[src]\n"
        "add 1,%[dst]\n"
        "add -1,%[tail]\n"
        "bne 3b\n"
        "4:\n"
        : [src] "=&r" (sp), [dst] "=&r" (dp), [groups] "+r" (groups), [tail] "+r" (tail),
          [idx] "=&r" (idx), [t] "=&r" (t)
        : [src_in] "r" (src), [dst_in] "r" (dst), [lut] "r" (lut)
        : "memory");
}


#endif

#if defined(WAIFU_FM_CD32X)
static inline void cd32x_blit_row_mapped_pairs(const uint8_t *src, uint8_t *dst, const uint8_t *xmap, int count)
{
    if (count <= 0) return;
    if ((uintptr_t)dst & 1u) {
        *dst++ = src[*xmap++];
        --count;
    }
    {
        uint16_t *dst16 = (uint16_t *)(uintptr_t)dst;
        while (count >= 2) {
            uint8_t a = src[xmap[0]];
            uint8_t b = src[xmap[1]];
            *dst16++ = (uint16_t)(((uint16_t)a << 8) | b);
            xmap += 2;
            count -= 2;
        }
        if (count) *(uint8_t *)(uintptr_t)dst16 = src[*xmap];
    }
}

static void cd32x_blit_scaled_fast(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + dw;
    int y1 = y + dh;
    int32_t step_x;
    int32_t step_y;

    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (x1 > WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH;
    if (y1 > WAIFU_FM_HEIGHT) y1 = WAIFU_FM_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    step_x = (int32_t)(((uint32_t)sw << 16) / (uint32_t)dw);
    step_y = (int32_t)(((uint32_t)sh << 16) / (uint32_t)dh);

    for (int dy = y0; dy < y1; ++dy) {
        int sy = (int)(((int32_t)(dy - y) * step_y) >> 16);
        int count = x1 - x0;
        int32_t sxq = (int32_t)(x0 - x) * step_x;
        uint8_t *dst = framebuffer + (int32_t)dy * WAIFU_FM_WIDTH + x0;
        const uint8_t *srow;

        if (sy < 0) sy = 0;
        if (sy >= sh) sy = sh - 1;
        srow = src + (int32_t)sy * sw;

        if ((uintptr_t)dst & 1u) {
            int sx = (int)(sxq >> 16);
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            *dst++ = srow[sx];
            sxq += step_x;
            --count;
        }
        {
            uint16_t *dst16 = (uint16_t *)(uintptr_t)dst;
            while (count >= 2) {
                int sx0 = (int)(sxq >> 16);
                int sx1;
                sxq += step_x;
                sx1 = (int)(sxq >> 16);
                sxq += step_x;
                if (sx0 < 0) sx0 = 0;
                if (sx0 >= sw) sx0 = sw - 1;
                if (sx1 < 0) sx1 = 0;
                if (sx1 >= sw) sx1 = sw - 1;
                *dst16++ = (uint16_t)(((uint16_t)srow[sx0] << 8) | srow[sx1]);
                count -= 2;
            }
            if (count) {
                int sx = (int)(sxq >> 16);
                if (sx < 0) sx = 0;
                if (sx >= sw) sx = sw - 1;
                *(uint8_t *)(uintptr_t)dst16 = srow[sx];
            }
        }
    }
}
#endif

static void blit_card_38x50_fast(const uint8_t *src, int x, int y)
{
    uint8_t *dst = framebuffer + y * WAIFU_FM_WIDTH + x;
    int yy;
    for (yy = 0; yy < 50; ++yy) {
        const uint8_t *srow = src + (int)g_card_ymap_38x50[yy] * WAIFU_CARD_W;
#if defined(WAIFU_FM_PCFX)
        pcfx_blit_row38_v810(srow, dst);
#elif defined(WAIFU_FM_CD32X)
        copy_u8_fast(dst, srow, 38);
#else
        memcpy(dst, srow, 38);
#endif
        dst += WAIFU_FM_WIDTH;
    }
}

static void blit_card_36x49_fast(const uint8_t *src, int x, int y)
{
    uint8_t *dst = framebuffer + y * WAIFU_FM_WIDTH + x;
    int yy;
    for (yy = 0; yy < 49; ++yy) {
        const uint8_t *srow = src + (int)g_card_ymap_36x49[yy] * WAIFU_CARD_W;
#if defined(WAIFU_FM_CD32X)
        cd32x_blit_row_mapped_pairs(srow, dst, g_card_xmap_36, 36);
#else
        int xx;
        for (xx = 0; xx < 36; ++xx) dst[xx] = srow[g_card_xmap_36[xx]];
#endif
        dst += WAIFU_FM_WIDTH;
    }
}

static void blit_card_38x50_gray_fast(const uint8_t *src, int x, int y)
{
    uint8_t *dst = framebuffer + y * WAIFU_FM_WIDTH + x;
    int yy;
    if (!g_gray_lut_ready) init_gray_lut();
    for (yy = 0; yy < 50; ++yy) {
        const uint8_t *srow = src + (int)g_card_ymap_38x50[yy] * WAIFU_CARD_W;
#if defined(WAIFU_FM_PCFX)
        pcfx_blit_row_gray_v810(srow, dst, g_gray_lut, 38);
#else
        int xx;
        for (xx = 0; xx < 38; ++xx) dst[xx] = g_gray_lut[srow[xx]];
#endif
        dst += WAIFU_FM_WIDTH;
    }
}

static void blit_card_mapped_fast(const uint8_t *src, int x, int y, int dw, int dh, const uint8_t *xmap, const uint8_t *ymap)
{
    uint8_t *dst = framebuffer + y * WAIFU_FM_WIDTH + x;
    int yy;
    for (yy = 0; yy < dh; ++yy) {
        const uint8_t *srow = src + (int)ymap[yy] * WAIFU_CARD_W;
#if defined(WAIFU_FM_CD32X)
        cd32x_blit_row_mapped_pairs(srow, dst, xmap, dw);
#else
        int xx;
        for (xx = 0; xx < dw; ++xx) dst[xx] = srow[xmap[xx]];
#endif
        dst += WAIFU_FM_WIDTH;
    }
}

static int try_draw_card_raw_fast(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh, int gray)
{
    if (!src || sw != WAIFU_CARD_W || sh != WAIFU_CARD_H) return 0;
    if (dw == 38 && dh == 50 && rect_fully_visible(x, y, 38, 50)) {
        if (gray) return 0; /* use generic dithered gray path */
        blit_card_38x50_fast(src, x, y);
        PROFILE_CARD2D_FAST();
        return 1;
    }
    if (!gray && dw == 36 && dh == 49 && rect_fully_visible(x, y, 36, 49)) {
        blit_card_36x49_fast(src, x, y);
        PROFILE_CARD2D_FAST();
        return 1;
    }
    if (!gray && rect_fully_visible(x, y, dw, dh)) {
        init_card_scale_maps();
        if (dw == 112 && dh == 112) {
            blit_card_mapped_fast(src, x, y, 112, 112, g_card_xmap_112, g_card_ymap_112);
            PROFILE_CARD2D_FAST();
            return 1;
        }
        if (dw == 100 && dh == 136) {
            blit_card_mapped_fast(src, x, y, 100, 136, g_card_xmap_100, g_card_ymap_136);
            PROFILE_CARD2D_FAST();
            return 1;
        }
        if (dw == 120 && dh == 160) {
            blit_card_mapped_fast(src, x, y, 120, 160, g_card_xmap_120, g_card_ymap_160);
            PROFILE_CARD2D_FAST();
            return 1;
        }
    }
    return 0;
}

static void draw_card_raw(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    if (!src || dw <= 0 || dh <= 0) return;
    if (dw == sw && dh == sh) {
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = x + dw;
        int y1 = y + dh;
        if (x1 > WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH;
        if (y1 > WAIFU_FM_HEIGHT) y1 = WAIFU_FM_HEIGHT;
        if (x0 >= x1 || y0 >= y1) return;
        for (int yy = y0; yy < y1; ++yy) {
            copy_u8_fast(framebuffer + (int32_t)yy * WAIFU_FM_WIDTH + x0,
                         src + (int32_t)(yy - y) * sw + (x0 - x),
                         x1 - x0);
        }
        return;
    }
    if (try_draw_card_raw_fast(src, sw, sh, x, y, dw, dh, 0)) return;
#if defined(WAIFU_FM_CD32X)
    cd32x_blit_scaled_fast(src, sw, sh, x, y, dw, dh);
    PROFILE_CARD2D_FAST();
    return;
#endif
    PROFILE_CARD2D_GENERIC_BEGIN();
    for (int yy = 0; yy < dh; ++yy) {
        int sy = (yy * sh) / dh;
        int dy = y + yy;
        if ((unsigned)dy >= WAIFU_FM_HEIGHT) continue;
        for (int xx = 0; xx < dw; ++xx) {
            int sx = (xx * sw) / dw;
            int dx = x + xx;
            if ((unsigned)dx >= WAIFU_FM_WIDTH) continue;
            framebuffer[dy * WAIFU_FM_WIDTH + dx] = src[sy * sw + sx];
        }
    }
    PROFILE_CARD2D_GENERIC_END();
}

static void draw_card_raw_gray(const uint8_t *src, int sw, int sh, int x, int y, int dw, int dh)
{
    if (!src || dw <= 0 || dh <= 0) return;
    if (try_draw_card_raw_fast(src, sw, sh, x, y, dw, dh, 1)) return;
    PROFILE_CARD2D_GENERIC_BEGIN();
    for (int yy = 0; yy < dh; ++yy) {
        int sy = (yy * sh) / dh;
        int dy = y + yy;
        if ((unsigned)dy >= WAIFU_FM_HEIGHT) continue;
        for (int xx = 0; xx < dw; ++xx) {
            int sx = (xx * sw) / dw;
            int dx = x + xx;
            if ((unsigned)dx >= WAIFU_FM_WIDTH) continue;
            framebuffer[dy * WAIFU_FM_WIDTH + dx] = gray_card_dither_px(src[sy * sw + sx], dx, dy);
        }
    }
    PROFILE_CARD2D_GENERIC_END();
}


static const uint8_t *card_big_art_ptr(int id);
static const uint8_t *support_big_art_ptr(void);
static int g_big_art_direct_note_suppressed = 0;

static void blit_art112_fast(const uint8_t *src, int x, int y)
{
    if (!src || !rect_fully_visible(x, y, WAIFU_BIG_W, WAIFU_BIG_H)) return;
    uint8_t *dst = framebuffer + y * WAIFU_FM_WIDTH + x;
#if defined(WAIFU_FM_PCFX)
    /* The V810 word-copy path is only safe when both the source and destination
       are 32-bit aligned.  Battle cut-ins can shake/lunge cards by two-pixel
       increments, so keep the fast path for aligned static positions and use a
       byte-exact row copy for moving or otherwise unaligned frames. */
    if ((((uintptr_t)src | (uintptr_t)dst) & 3u) == 0u) {
        for (int yy = 0; yy < WAIFU_BIG_H; ++yy) {
            const uint8_t *sp = src + yy * WAIFU_BIG_W;
            uint8_t *dp = dst;
            uint32_t loops = WAIFU_BIG_W >> 5;
            uint32_t tail_words = (WAIFU_BIG_W & 31u) >> 2;
            uint32_t a, b, c, d, e, f, g, h;
            __asm__ volatile (
                "cmp 0,%[loops]\n"
                "be 2f\n"
                "1:\n"
                "ld.w 0[%[sp]],%[a]\n"
                "ld.w 4[%[sp]],%[b]\n"
                "ld.w 8[%[sp]],%[c]\n"
                "ld.w 12[%[sp]],%[d]\n"
                "ld.w 16[%[sp]],%[e]\n"
                "ld.w 20[%[sp]],%[f]\n"
                "ld.w 24[%[sp]],%[g]\n"
                "ld.w 28[%[sp]],%[h]\n"
                "st.w %[a],0[%[dp]]\n"
                "st.w %[b],4[%[dp]]\n"
                "st.w %[c],8[%[dp]]\n"
                "st.w %[d],12[%[dp]]\n"
                "st.w %[e],16[%[dp]]\n"
                "st.w %[f],20[%[dp]]\n"
                "st.w %[g],24[%[dp]]\n"
                "st.w %[h],28[%[dp]]\n"
                "addi 32,%[sp],%[sp]\n"
                "addi 32,%[dp],%[dp]\n"
                "add -1,%[loops]\n"
                "bne 1b\n"
                "2:\n"
                "cmp 0,%[tail_words]\n"
                "be 4f\n"
                "3:\n"
                "ld.w 0[%[sp]],%[a]\n"
                "st.w %[a],0[%[dp]]\n"
                "add 4,%[sp]\n"
                "add 4,%[dp]\n"
                "add -1,%[tail_words]\n"
                "bne 3b\n"
                "4:\n"
                : [sp] "+r" (sp), [dp] "+r" (dp), [loops] "+r" (loops), [tail_words] "+r" (tail_words),
                  [a] "=&r" (a), [b] "=&r" (b), [c] "=&r" (c), [d] "=&r" (d),
                  [e] "=&r" (e), [f] "=&r" (f), [g] "=&r" (g), [h] "=&r" (h)
                :
                : "memory");
            dst += WAIFU_FM_WIDTH;
        }
        return;
    }
#endif
    for (int yy = 0; yy < WAIFU_BIG_H; ++yy) {
        const uint8_t *srow = src + yy * WAIFU_BIG_W;
        uint8_t *drow = dst;
        for (int xx = 0; xx < WAIFU_BIG_W; ++xx) drow[xx] = srow[xx];
        dst += WAIFU_FM_WIDTH;
    }
}

static void draw_big_art_112_note(const uint8_t *src, WaifuBigArtKind kind, int card_id, int x, int y)
{
    if (!src) return;
    if (rect_fully_visible(x, y, WAIFU_BIG_W, WAIFU_BIG_H)) {
        blit_art112_fast(src, x, y);
        if (!g_big_art_direct_note_suppressed) {
            waifu_assets_note_big_art_draw(kind, card_id, x, y);
        }
        PROFILE_CARD2D_FAST();
        return;
    }
    draw_card_raw(src, WAIFU_BIG_W, WAIFU_BIG_H, x, y, WAIFU_BIG_W, WAIFU_BIG_H);
}

static void draw_card_big_art_112(int card_id, int x, int y)
{
    draw_big_art_112_note(card_big_art_ptr(card_id), WAIFU_BIG_ART_CARD, card_id, x, y);
}

static void draw_support_big_art_112(int x, int y)
{
    draw_big_art_112_note(support_big_art_ptr(), WAIFU_BIG_ART_SUPPORT, -1, x, y);
}

static void draw_big_art_scaled_note(const uint8_t *src, WaifuBigArtKind kind, int card_id, int x, int y, int w, int h)
{
    if (!src || w <= 0 || h <= 0) return;
    if (w == WAIFU_BIG_W && h == WAIFU_BIG_H) { draw_big_art_112_note(src, kind, card_id, x, y); return; }
    draw_card_raw(src, WAIFU_BIG_W, WAIFU_BIG_H, x, y, w, h);
}

static void draw_card_big_art_scaled(int card_id, int x, int y, int w, int h)
{
    draw_big_art_scaled_note(card_big_art_ptr(card_id), WAIFU_BIG_ART_CARD, card_id, x, y, w, h);
}

static void draw_support_big_art_scaled(int x, int y, int w, int h)
{
    draw_big_art_scaled_note(support_big_art_ptr(), WAIFU_BIG_ART_SUPPORT, -1, x, y, w, h);
}

static const uint8_t *card_face_ptr(int id)
{
    if (id < 0) id = 0;
    if (id >= WAIFU_CARD_COUNT) id = WAIFU_CARD_COUNT - 1;
#if defined(WAIFU_FM_CD32X)
    return waifu_assets_card_face_cached(id);
#else
    return waifu_assets_card_face(id);
#endif
}

static const uint8_t *card_big_art_ptr(int id)
{
    if (id < 0) id = 0;
    if (id >= WAIFU_CARD_COUNT) id = WAIFU_CARD_COUNT - 1;
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
    /* Rendering must never issue a synchronous CD/SCSI read.  Full-size card
       art is prewarmed into the target's resident cache; on an unexpected miss,
       draw the frame/text only instead of stalling inside presentation. */
    return waifu_assets_card_big_art_cached(id);
#else
    return waifu_assets_card_big_art(id);
#endif
}

static const uint8_t *support_big_art_ptr(void)
{
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
    return waifu_assets_support_big_art_cached();
#else
    return waifu_assets_support_big_art();
#endif
}

static void draw_card_sprite(int id, int x, int y, int w, int h, int back)
{
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
#if defined(WAIFU_FM_CD32X)
    if (back) {
        rect_fill(x, y, w, h, IDX_CARD_RIM);
        rect_outline(x, y, w, h, IDX_GOLD_HI);
        rect_outline(x+1, y+1, w-2, h-2, IDX_GOLD_DARK);
        rect_fill(x+3, y+3, w-6, h-6, IDX_DARK_BROWN);
        rect_outline(x+4, y+4, w-8, h-8, IDX_GOLD_DARK);
        line_i(x + w / 2, y + 6, x + w - 7, y + h / 2, IDX_GOLD_HI);
        line_i(x + w - 7, y + h / 2, x + w / 2, y + h - 7, IDX_GOLD_DARK);
        line_i(x + w / 2, y + h - 7, x + 6, y + h / 2, IDX_GOLD_HI);
        line_i(x + 6, y + h / 2, x + w / 2, y + 6, IDX_GOLD_DARK);
        return;
    }
    {
        const uint8_t *src = card_face_ptr(id);
        if (!src) {
            rect_fill(x, y, w, h, IDX_CARD_RIM);
            rect_outline(x, y, w, h, IDX_GOLD_HI);
            rect_outline(x+1, y+1, w-2, h-2, IDX_GOLD_DARK);
            rect_fill(x+3, y+3, w-6, h-6, IDX_UI_DARK);
            rect_fill(x+5, y+5, w-10, (h > 18) ? 5 : 3, IDX_GOLD_DARK);
            rect_fill(x+5, y+h-13, w-10, 8, IDX_GOLD_DARK);
            rect_outline(x+5, y+11, w-10, h-27, IDX_GOLD_DARK);
            line_i(x + w / 2, y + 14, x + w - 9, y + h / 2, IDX_GOLD_HI);
            line_i(x + w - 9, y + h / 2, x + w / 2, y + h - 17, IDX_GOLD_DARK);
            line_i(x + w / 2, y + h - 17, x + 8, y + h / 2, IDX_GOLD_HI);
            line_i(x + 8, y + h / 2, x + w / 2, y + 14, IDX_GOLD_DARK);
            return;
        }
        draw_card_raw(src, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
        return;
    }
#endif
    draw_card_raw(back ? waifu_assets_card_back() : card_face_ptr(id), WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
}

static void draw_card_sprite_ex(int id, int x, int y, int w, int h, int back, int gray)
{
    const uint8_t *src = back ? waifu_assets_card_back() : card_face_ptr(id);
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
#if defined(WAIFU_FM_CD32X)
    if (back) {
        draw_card_sprite(id, x, y, w, h, 1);
        return;
    }
    if (!src) {
        draw_card_sprite(id, x, y, w, h, 0);
        return;
    }
#endif
    if (gray) draw_card_raw_gray(src, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
    else draw_card_raw(src, WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
}

/* Recolour the baked blue support frame to violet for Trap cards, leaving the
   inner art window (the sigil) untouched. Rectangles mirror the baked support
   face layout (WAIFU_CARD_W x WAIFU_CARD_H) scaled to the destination. */
static void draw_trap_frame_overlay(int x, int y, int w, int h)
{
#define TRAP_MX(v) (x + (v) * w / WAIFU_CARD_W)
#define TRAP_MY(v) (y + (v) * h / WAIFU_CARD_H)
    rect_outline(TRAP_MX(1), TRAP_MY(0), TRAP_MX(37) - TRAP_MX(1), TRAP_MY(54) - TRAP_MY(0), IDX_TRAP_FRAME_HI);
    rect_outline(TRAP_MX(2), TRAP_MY(2), TRAP_MX(36) - TRAP_MX(2), TRAP_MY(52) - TRAP_MY(2), IDX_TRAP_FRAME_DK);
    rect_outline(TRAP_MX(3), TRAP_MY(3), TRAP_MX(35) - TRAP_MX(3), TRAP_MY(51) - TRAP_MY(3), IDX_TRAP_FRAME);
    /* type strip */
    rect_fill(TRAP_MX(4), TRAP_MY(4), TRAP_MX(34) - TRAP_MX(4), TRAP_MY(8) - TRAP_MY(4), IDX_TRAP_FRAME);
    /* lower stat/text strip */
    rect_fill(TRAP_MX(4), TRAP_MY(40), TRAP_MX(34) - TRAP_MX(4), TRAP_MY(50) - TRAP_MY(40), IDX_TRAP_FRAME);
    rect_fill(TRAP_MX(5), TRAP_MY(41), TRAP_MX(33) - TRAP_MX(5), TRAP_MY(49) - TRAP_MY(41), IDX_TRAP_FRAME_DK);
#undef TRAP_MX
#undef TRAP_MY
}

static void draw_support_sprite(int id, int x, int y, int w, int h)
{
    rect_fill(x+2, y+3, w, h, IDX_BLACK);
#if defined(WAIFU_FM_CD32X)
    {
        int trap = is_trap_support_card(id);
        uint8_t frame_hi = trap ? IDX_TRAP_FRAME_HI : IDX_SUPPORT_FRAME;
        uint8_t frame = trap ? IDX_TRAP_FRAME : IDX_SUPPORT_FRAME_HI;
        uint8_t frame_dk = trap ? IDX_TRAP_FRAME_DK : IDX_UI_DARK;
        rect_fill(x, y, w, h, frame_hi);
        rect_outline(x, y, w, h, IDX_WHITE);
        rect_outline(x+1, y+1, w-2, h-2, frame);
        rect_fill(x+3, y+3, w-6, h-6, frame_dk);
        rect_fill(x+5, y+5, w-10, (h > 18) ? 5 : 3, frame);
        rect_fill(x+5, y+h-13, w-10, 8, frame);
        rect_outline(x+5, y+11, w-10, h-27, frame);
        line_i(x + w / 2, y + 14, x + w - 9, y + h / 2, frame_hi);
        line_i(x + w - 9, y + h / 2, x + w / 2, y + h - 17, frame);
        line_i(x + w / 2, y + h - 17, x + 8, y + h / 2, frame_hi);
        line_i(x + 8, y + h / 2, x + w / 2, y + 14, frame);
        return;
    }
#endif
    draw_card_raw(waifu_assets_support_face(), WAIFU_CARD_W, WAIFU_CARD_H, x, y, w, h);
    if (is_trap_support_card(id)) draw_trap_frame_overlay(x, y, w, h);
}

static void draw_hand_card_sprite(int id, int x, int y, int w, int h, int back)
{
    if (back) { draw_card_sprite(id, x, y, w, h, 1); return; }
    if (is_support_card(id)) draw_support_sprite(id, x, y, w, h);
    else draw_card_sprite(id, x, y, w, h, back);
}

static void draw_hand_card_sprite_ex(int id, int x, int y, int w, int h, int back, int gray)
{
    if (back) { draw_card_sprite_ex(id, x, y, w, h, 1, gray); return; }
    if (is_support_card(id)) draw_support_sprite(id, x, y, w, h);
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
        draw_card_raw(waifu_assets_card_back(), WAIFU_CARD_W, WAIFU_CARD_H, x+4, y+4, w-8, h-8);
        return;
    }
    if (is_support_card(id)) {
        /* Support/equip cards can appear in battle reveals only if bad/stale
           state puts them in a monster zone. Draw the support art with the
           support card's distinct blue frame and no ATK/DEF/stat strings;
           they are treated as 0/0 non-attackers. */
        {
            int trap = is_trap_support_card(id);
            rect_outline(x, y, w, h, trap ? IDX_TRAP_FRAME_HI : IDX_BLUE_WHITE);
            rect_outline(x+1, y+1, w-2, h-2, trap ? IDX_TRAP_FRAME : IDX_UI_BLUE);
            draw_support_big_art_scaled(x+4, y+6, 112, 112);
            rect_outline(x+3, y+5, 114, 114, trap ? IDX_TRAP_FRAME : IDX_UI_BLUE);
        }
        return;
    }
    if (!is_monster_card(id)) {
        draw_card_raw(waifu_assets_card_back(), WAIFU_CARD_W, WAIFU_CARD_H, x+4, y+4, w-8, h-8);
        return;
    }
    draw_card_big_art_112(id, x+4, y+6);
    rect_outline(x+3, y+5, 114, 114, IDX_CARD_RIM);
    rect_fill(x+5, y+123, 110, 28, IDX_DARK_BROWN);
    rect_outline(x+5, y+123, 110, 28, IDX_GOLD_DARK);
    {
        char stats[32];
        if (atk < 0) atk = (int)waifu_card_atk[id];
        if (defv < 0) defv = (int)waifu_card_def[id];
        fmt_prefixed_i32(stats, (int)sizeof(stats), 'A', atk);
        draw_text_small(x+9, y+128, stats, stat_delta_color(atk - (int)waifu_card_atk[id]), IDX_BLACK);
        fmt_prefixed_i32(stats, (int)sizeof(stats), 'D', defv);
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
            draw_card_raw(waifu_assets_card_back(), WAIFU_CARD_W, WAIFU_CARD_H, x+3, y+3, w-6, h-6);
        } else {
            rect_fill(x+3, y+3, w-6, h-6, IDX_CARD_GOLD);
            int art_w = w - 8;
            int art_h = h > 134 ? 112 : h - 20;
            if (art_w < 1) art_w = 1;
            if (art_h < 1) art_h = 1;
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
            /* The face-down reveal was spending most of the visible flip in
               repeated scaled big-art reads/rasterization.  Show a cheap gold
               card-face silhouette while the card is still edge-on, then snap
               to the normal 112x112 direct-art path near full width.  This
               matches the already-fast equip-card reveal path and keeps SCSI/CD
               misses and CD32X scaled-art work out of the moving part of the
               animation. */
            if (w < 120) {
                int cx = x + w / 2;
                rect_fill(x + 4, y + 6, w - 8, art_h, IDX_DARK_BROWN);
                rect_outline(x + 4, y + 6, w - 8, art_h, IDX_GOLD_DARK);
                if (w > 20) {
                    hline(x + 8, x + w - 9, y + 24, IDX_GOLD_HI);
                    hline(x + 8, x + w - 9, y + 88, IDX_GOLD_HI);
                } else {
                    for (int yy = y + 10; yy <= y + art_h + 3; ++yy) put_px(cx, yy, IDX_GOLD_HI);
                }
            } else {
                draw_card_big_art_scaled(id, x+4, y+6, art_w, art_h);
            }
#else
            if (w < 92) {
                rect_fill(x + 4, y + 6, w - 8, art_h, IDX_DARK_BROWN);
                rect_outline(x + 4, y + 6, w - 8, art_h, IDX_GOLD_DARK);
            } else {
                draw_card_big_art_scaled(id, x+4, y+6, art_w, art_h);
            }
#endif
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

static int hand_final_x(int i) { return WAIFU_UI_CENTER_DX + 12 + i * 47; }
static int hand_y(void) { return WAIFU_HAND_Y_BASE + g_player_hand_offset_y; }

static void draw_player_hand(int f, int selected)
{
    PROFILE_HAND_BEGIN();
    for (int i = 0; i < 5; ++i) {
        int x0 = hand_final_x(i);
        int y = hand_y();
        int x = x0;
        if (f < 116) {
            int32_t t = q8_smooth_ratio(f - (84 + i * 4), 12);
            x = lerp_i(272 + WAIFU_UI_EXTRA_W, x0, t);
        }
        if (i == g_player_hide_index) continue;
        if (((f >= 150 && f < 176) || (f >= 475 && f < 505) || (f >= 910 && f < 930)) && i == selected) continue;
        if (i == 2) PROFILE_HAND_CARD_DRAW(draw_support_sprite(SUPPORT_EQUIP_CARD_ID, x, y+1, 38, 50));
        else PROFILE_HAND_CARD_DRAW(draw_card_sprite(hand_ids[i], x, y, 38, 50, 0));
        if (!g_suppress_hand_cursor && i == selected && f >= 102) draw_red_cursor(x, y, 38, 50);
    }
    PROFILE_HAND_END();
}

static void draw_player_hand_draw_sequence(int f, int start, int selected)
{
    PROFILE_HAND_BEGIN();
    /* FM-like turn-start restoration: the row scrolls up, then replacement
       cards visibly draw from the right edge.  This is deliberately slower and
       clearer than v11 so the draw is legible in the full-match video. */
    int draw_slots[2] = {0, 4};
    int reveal_cursor = (f >= start + 76);
    int yoff = lerp_i(92, 0, q8_smooth_ratio(f - start, 30));

    if (f >= start + 20 && f < start + 78) draw_text_small(211 + WAIFU_UI_CENTER_DX, 142 + yoff / 3, "DRAW", IDX_GOLD_HI, IDX_BLACK);

    for (int i = 0; i < 5; ++i) {
        int x0 = hand_final_x(i);
        int y = WAIFU_HAND_Y_BASE + yoff;
        int x = x0;
        int visible = 1;
        for (int d = 0; d < 2; ++d) {
            if (i == draw_slots[d]) {
                int32_t t = q8_smooth_ratio(f - (start + 24 + d * 20), 24);
                if (t <= 0) visible = 0;
                x = lerp_i(282 + WAIFU_UI_EXTRA_W, x0, t);
            }
        }
        if (!visible) continue;
        if (i == 2) PROFILE_HAND_CARD_DRAW(draw_support_sprite(SUPPORT_EQUIP_CARD_ID, x, y+1, 38, 50));
        else PROFILE_HAND_CARD_DRAW(draw_card_sprite(hand_ids[i], x, y, 38, 50, 0));
        if (reveal_cursor && i == selected) draw_red_cursor(x, y, 38, 50);
    }
    PROFILE_HAND_END();
}

static void draw_enemy_hand(int f, int selected, int reveal_one)
{
    PROFILE_HAND_BEGIN();
    (void)reveal_one;
    for (int i = 0; i < 5; ++i) {
        int x = WAIFU_UI_CENTER_DX + 12 + i * 48;
        int y = WAIFU_HAND_Y_BASE + g_enemy_hand_offset_y;
        if (i == g_enemy_hide_index) continue;
        if (((f >= 256 && f < 278) || (f >= 675 && f < 705)) && i == selected) continue;
        PROFILE_HAND_CARD_DRAW(draw_card_sprite(0, x, y, 36, 49, 1));
        if (!g_suppress_hand_cursor && i == selected) draw_red_cursor(x, y, 36, 49);
    }
    PROFILE_HAND_END();
}


static void draw_enemy_hand_draw_sequence(int f, int start, int selected)
{
    PROFILE_HAND_BEGIN();
    int yoff = lerp_i(92, 0, q8_smooth_ratio(f - start, 30)) + g_enemy_hand_offset_y;
    if (f >= start + 14 && f < start + 70) draw_text_small(211 + WAIFU_UI_CENTER_DX, 142 + yoff / 3, "DRAW", IDX_GOLD_HI, IDX_BLACK);
    for (int i = 0; i < 5; ++i) {
        int x0 = WAIFU_UI_CENTER_DX + 12 + i * 48;
        int32_t t = q8_smooth_ratio(f - (start + i * 6), 20);
        if (t <= 0) continue;
        int x = lerp_i(282 + WAIFU_UI_EXTRA_W, x0, t);
        int y = WAIFU_HAND_Y_BASE + yoff;
        PROFILE_HAND_CARD_DRAW(draw_card_sprite(0, x, y, 36, 49, 1));
        if (f >= start + 42 && i == selected) draw_red_cursor(x, y, 36, 49);
    }
    PROFILE_HAND_END();
}

/* ------------------------------------------------------------------------- */
/* Board rendering and board-card placement. */

static int32_t col_xq(int32_t c) { return FIELD_X0 + div_toward_zero_i32((FIELD_X1 - FIELD_X0) * c, (BOARD_COLS * Q8_ONE)); }
static int32_t row_zq(int32_t r) { return FIELD_Z0 + div_toward_zero_i32((FIELD_Z1 - FIELD_Z0) * r, (BOARD_ROWS * Q8_ONE)); }
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

static void draw_grid_line_projected(ScreenPt pa, ScreenPt pb, uint8_t c)
{
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
       before calling render_board_cached(). Clearing here makes the core renderer
       platform-agnostic and guarantees both SDL and headless produce the same
       full framebuffer. */
    clear_screen(IDX_BLACK);

#if !defined(WAIFU_BOARD_FAST_AFFINE_ENABLE)
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
#else
    BoardProjected bp;
    build_board_projected(cam, &bp);
#if !defined(WAIFU_FM_CD32X) || WAIFU_CD32X_FIELD_SIDE_WALLS
#if defined(WAIFU_FM_CD32X)
    if (!cd32x_render_board_sides_parallel(cam, &bp))
#endif
    {
    draw_field_slab_sides_fast(cam, &bp);
    }
#endif

#if defined(WAIFU_FM_CD32X)
    if (!cd32x_render_board_top_parallel(&bp))
#endif
    {
    for (int r = 0; r < BOARD_ROWS; ++r) {
        for (int c = 0; c < BOARD_COLS; ++c) {
            int tile = ((r + c) & 1) ? 5 : 1;
#if defined(WAIFU_FM_CD32X) && WAIFU_CD32X_BOARD_FLAT_TOP
            draw_board_top_quad_flat_projected(bp.top[r][c], bp.top[r][c+1], bp.top[r+1][c+1], bp.top[r+1][c], tile);
#else
            draw_quad3d_fast_projected(bp.top[r][c], bp.top[r][c+1], bp.top[r+1][c+1], bp.top[r+1][c], tile);
#endif
        }
    }
    }
#endif

#if !defined(WAIFU_BOARD_FAST_AFFINE_ENABLE)
    for (int c = 0; c <= BOARD_COLS; ++c) draw_grid_line(cam, v3(col_x0(c),Q8_FRAC(3,100),FIELD_Z0), v3(col_x0(c),Q8_FRAC(3,100),FIELD_Z1), IDX_DARK_BROWN);
    for (int r = 0; r <= BOARD_ROWS; ++r) draw_grid_line(cam, v3(FIELD_X0,Q8_FRAC(3,100),row_z0(r)), v3(FIELD_X1,Q8_FRAC(3,100),row_z0(r)), IDX_DARK_BROWN);
#else
    for (int c = 0; c <= BOARD_COLS; ++c) draw_grid_line_projected(bp.top[0][c], bp.top[BOARD_ROWS][c], IDX_DARK_BROWN);
    for (int r = 0; r <= BOARD_ROWS; ++r) draw_grid_line_projected(bp.top[r][0], bp.top[r][BOARD_COLS], IDX_DARK_BROWN);
#endif
}

static void render_board_cached(Camera cam)
{
#if defined(WAIFU_BG_CACHE_DISABLE)
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    unsigned long long _profile_render_t0 = g_profile_render_enabled ? profile_now_us() : 0;
#endif
    render_board(cam);
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    if (g_profile_render_enabled) {
        g_profile_board_cache_render_us += profile_now_us() - _profile_render_t0;
        g_profile_board_cache_misses++;
    }
#endif
#else
    if (g_board_bg_cache_valid && camera_equal(g_board_bg_cache_cam, cam)) {
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
        unsigned long long _profile_t0 = g_profile_render_enabled ? profile_now_us() : 0;
#endif
        copy_u8_fast(framebuffer, g_board_bg_cache, (int)sizeof(g_board_bg_cache));
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
        if (g_profile_render_enabled) {
            g_profile_board_cache_copy_us += profile_now_us() - _profile_t0;
            g_profile_board_cache_hits++;
        }
#endif
        return;
    }
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    unsigned long long _profile_render_t0 = g_profile_render_enabled ? profile_now_us() : 0;
#endif
    render_board(cam);
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    if (g_profile_render_enabled) {
        g_profile_board_cache_render_us += profile_now_us() - _profile_render_t0;
        g_profile_board_cache_misses++;
        _profile_render_t0 = profile_now_us();
    }
#endif
    copy_u8_fast(g_board_bg_cache, framebuffer, (int)sizeof(g_board_bg_cache));
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    if (g_profile_render_enabled) g_profile_board_cache_copy_us += profile_now_us() - _profile_render_t0;
#endif
    g_board_bg_cache_cam = cam;
    g_board_bg_cache_valid = 1;
#endif
}

static void draw_textured_tri_ex(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c, int gray)
{
    if (!src || sw <= 0 || sh <= 0) return;
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    /* A vertex projected just in front of the camera (tiny cz) lands at a screen
       coord in the millions; the edge/barycentric products below then overflow
       int32 and `den` can wrap to a value whose `*2` is 0, making the per-pixel
       divide trap -> the V810 jumps to the BIOS exception handler and hangs (repro:
       COM equips a monster then the equip card / monster is drawn at a grazing
       camera).  A real card spans far less than the screen, so any corner this far
       out is a degenerate near-singularity projection: skip it. */
    if (minx < -8192 || maxx > 8192 || miny < -8192 || maxy > 8192) return;
    int den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (den == 0) return;
    if (minx < 0) minx = 0;
    if (maxx >= WAIFU_FM_WIDTH) maxx = WAIFU_FM_WIDTH - 1;
    if (miny < 0) miny = 0;
    if (maxy >= WAIFU_FM_HEIGHT) maxy = WAIFU_FM_HEIGHT - 1;
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
                put_px(x, y, gray ? gray_card_dither_px(pix, x, y) : pix);
            }
        }
    }
}

static void draw_textured_tri(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c)
{
    draw_textured_tri_ex(src, sw, sh, a, b, c, 0);
}

#if defined(WAIFU_FM_CD32X)
static void draw_textured_tri_affine_cd32x(const uint8_t *src, int sw, int sh, TexV a, TexV b, TexV c, int gray)
{
    int minx;
    int maxx;
    int miny;
    int maxy;
    int den;
    int Aa;
    int Ba;
    int Ab;
    int Bb;
    int Ac;
    int Bc;
    int32_t U0;
    int32_t U1;
    int32_t U2;
    int32_t V0;
    int32_t V1;
    int32_t V2;
    int32_t du_dx;
    int32_t dv_dx;
    int32_t du_dy;
    int32_t dv_dy;
    int wa_row;
    int wb_row;
    int wc_row;
    int32_t u_row;
    int32_t v_row;

    if (!src || sw <= 0 || sh <= 0) return;
    minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (minx < -8192 || maxx > 8192 || miny < -8192 || maxy > 8192) return;
    den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (den == 0) return;
    if (minx < 0) minx = 0;
    if (maxx >= WAIFU_FM_WIDTH) maxx = WAIFU_FM_WIDTH - 1;
    if (miny < 0) miny = 0;
    if (maxy >= WAIFU_FM_HEIGHT) maxy = WAIFU_FM_HEIGHT - 1;

    Aa = b.y - c.y;
    Ba = c.x - b.x;
    Ab = c.y - a.y;
    Bb = a.x - c.x;
    Ac = -(Aa + Ab);
    Bc = -(Ba + Bb);
    U0 = (int32_t)(((int64_t)a.u * (int64_t)(sw - 1) << 16) >> Q8_SHIFT);
    U1 = (int32_t)(((int64_t)b.u * (int64_t)(sw - 1) << 16) >> Q8_SHIFT);
    U2 = (int32_t)(((int64_t)c.u * (int64_t)(sw - 1) << 16) >> Q8_SHIFT);
    V0 = (int32_t)(((int64_t)a.v * (int64_t)(sh - 1) << 16) >> Q8_SHIFT);
    V1 = (int32_t)(((int64_t)b.v * (int64_t)(sh - 1) << 16) >> Q8_SHIFT);
    V2 = (int32_t)(((int64_t)c.v * (int64_t)(sh - 1) << 16) >> Q8_SHIFT);
    du_dx = (int32_t)(((int64_t)Aa * U0 + (int64_t)Ab * U1 + (int64_t)Ac * U2) / den);
    dv_dx = (int32_t)(((int64_t)Aa * V0 + (int64_t)Ab * V1 + (int64_t)Ac * V2) / den);
    du_dy = (int32_t)(((int64_t)Ba * U0 + (int64_t)Bb * U1 + (int64_t)Bc * U2) / den);
    dv_dy = (int32_t)(((int64_t)Ba * V0 + (int64_t)Bb * V1 + (int64_t)Bc * V2) / den);

    wa_row = Aa * (minx - c.x) + Ba * (miny - c.y);
    wb_row = Ab * (minx - c.x) + Bb * (miny - c.y);
    wc_row = den - wa_row - wb_row;
    u_row = (int32_t)(((int64_t)wa_row * U0 + (int64_t)wb_row * U1 + (int64_t)wc_row * U2) / den);
    v_row = (int32_t)(((int64_t)wa_row * V0 + (int64_t)wb_row * V1 + (int64_t)wc_row * V2) / den);

    for (int y = miny; y <= maxy; ++y) {
        int wa = wa_row;
        int wb = wb_row;
        int wc = wc_row;
        int32_t u = u_row;
        int32_t v = v_row;
        uint8_t *dst = framebuffer + (int32_t)y * WAIFU_FM_WIDTH;
        for (int x = minx; x <= maxx; ++x) {
            if ((den > 0 && wa >= 0 && wb >= 0 && wc >= 0) ||
                (den < 0 && wa <= 0 && wb <= 0 && wc <= 0)) {
                int sx = u >> 16;
                int sy = v >> 16;
                uint8_t pix;
                if (sx < 0) sx = 0;
                else if (sx >= sw) sx = sw - 1;
                if (sy < 0) sy = 0;
                else if (sy >= sh) sy = sh - 1;
                pix = src[sy * sw + sx];
                dst[x] = gray ? gray_card_dither_px(pix, x, y) : pix;
            }
            wa += Aa;
            wb += Ab;
            wc += Ac;
            u += du_dx;
            v += dv_dx;
        }
        wa_row += Ba;
        wb_row += Bb;
        wc_row += Bc;
        u_row += du_dy;
        v_row += dv_dy;
    }
}
#endif

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
    int den = (pb.y - pc.y) * (pa.x - pc.x) + (pc.x - pb.x) * (pa.y - pc.y);
    if (den < 0) {
        ScreenPt t = pa;
        pa = pb;
        pb = t;
        flip_u = !flip_u;
        den = (pb.y - pc.y) * (pa.x - pc.x) + (pc.x - pb.x) * (pa.y - pc.y);
    }
    if (den == 0) return;
    minx = pa.x < pb.x ? (pa.x < pc.x ? pa.x : pc.x) : (pb.x < pc.x ? pb.x : pc.x);
    maxx = pa.x > pb.x ? (pa.x > pc.x ? pa.x : pc.x) : (pb.x > pc.x ? pb.x : pc.x);
    miny = pa.y < pb.y ? (pa.y < pc.y ? pa.y : pc.y) : (pb.y < pc.y ? pb.y : pc.y);
    maxy = pa.y > pb.y ? (pa.y > pc.y ? pa.y : pc.y) : (pb.y > pc.y ? pb.y : pc.y);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= WAIFU_FM_WIDTH) maxx = WAIFU_FM_WIDTH - 1;
    if (maxy >= WAIFU_FM_HEIGHT) maxy = WAIFU_FM_HEIGHT - 1;
    /* Projected winding can flip by camera/base-edge order.  Normalize it here
       and leave occlusion to the caller's painter-sorted face order; otherwise
       the PC-FX map can cull every visible pyramid face. */
    init_repeat_texel_q8();
    {
    /* Fully incremental affine textured triangle (envmap drawTriangle method): the
       barycentric weights wa2/wb2 AND the texture coords u/v are linear in screen
       (x,y), so they are seeded once and advanced with pure ADDS -- there is no
       per-pixel divide and no per-pixel multiply.  DIV and MUL are both very slow
       on the V810, so the only divides left are the 6 per-face gradient/seed
       setups (4 faces/frame -> negligible).  u/v are kept in 16.16 so the per-step
       truncation cannot drift a visible amount across a ~90px face.  The texture
       is the repeating brick LUT, so affine mapping is visually equivalent to the
       old rational one. */
    const int den2 = den * 2;
    const int Aa = pb.y - pc.y, Ba = pc.x - pb.x;
    const int Ab = pc.y - pa.y, Bb = pa.x - pc.x;
    const int Ac = -(Aa + Ab), Bc = -(Ba + Bb);
    const int step_a = Aa * 2, step_b = Ab * 2;     /* wa2/wb2 per-x */
    const int rstep_a = Ba * 2, rstep_b = Bb * 2;   /* wa2/wb2 per-y */
    const int two_pcx = pc.x * 2, two_pcy = pc.y * 2;
    const uint8_t *rep = g_repeat_texel_q8;
    /* Texture coords at a(base0), b(base1), c(apex) in 16.16; one repeat = 1<<16. */
    const int U0 = flip_u ? (cols << 16) : 0;
    const int U1 = flip_u ? 0 : (cols << 16);
    const int U2 = (cols << 16) >> 1;
    const int V0 = 0, V1 = 0, V2 = rows << 16;
    /* Per-pixel/per-row gradients (16.16): the divides happen here, once per face. */
    const int du_dx = (int)((2LL*Aa*U0 + 2LL*Ab*U1 + 2LL*Ac*U2) / den2);
    const int du_dy = (int)((2LL*Ba*U0 + 2LL*Bb*U1 + 2LL*Bc*U2) / den2);
    const int dv_dx = (int)((2LL*Aa*V0 + 2LL*Ab*V1 + 2LL*Ac*V2) / den2);
    const int dv_dy = (int)((2LL*Ba*V0 + 2LL*Bb*V1 + 2LL*Bc*V2) / den2);
    const int step_c = -(step_a + step_b);          /* wc2 per-x */
    const int rstep_c = -(rstep_a + rstep_b);        /* wc2 per-y */
    /* Seed weights and tex coords at the left of the bounding box, row = miny. */
    int dxc0 = (minx * 2 + 1) - two_pcx;
    int dyc0 = (miny * 2 + 1) - two_pcy;
    int wa2_row = Aa * dxc0 + Ba * dyc0;
    int wb2_row = Ab * dxc0 + Bb * dyc0;
    int wc2_row = den2 - wa2_row - wb2_row;
    int u_row = (int)(((int64_t)wa2_row*U0 + (int64_t)wb2_row*U1 + (int64_t)wc2_row*U2) / den2);
    int v_row = (int)(((int64_t)wa2_row*V0 + (int64_t)wb2_row*V1 + (int64_t)wc2_row*V2) / den2);
    /* Per-scanline span: instead of testing every bounding-box pixel, compute the
       x where each edge crosses zero and intersect the half-planes to get [xL,xR],
       then fill only that run.  The crossings move by a constant per row, so they
       are tracked incrementally in 16.16 -- no per-scanline or per-pixel divide
       (only the per-edge setup divides below).  Edges with a zero x-step are
       horizontal: they either pass (row-value >= 0) or reject the whole scanline. */
    int64_t xa16 = 0, xb16 = 0, xc16 = 0, dxa = 0, dxb = 0, dxc = 0;
    if (step_a) { xa16 = ((int64_t)minx << 16) - (((int64_t)wa2_row) << 16) / step_a; dxa = -(((int64_t)rstep_a) << 16) / step_a; }
    if (step_b) { xb16 = ((int64_t)minx << 16) - (((int64_t)wb2_row) << 16) / step_b; dxb = -(((int64_t)rstep_b) << 16) / step_b; }
    if (step_c) { xc16 = ((int64_t)minx << 16) - (((int64_t)wc2_row) << 16) / step_c; dxc = -(((int64_t)rstep_c) << 16) / step_c; }
    (void)sh;
    for (int y = miny; y <= maxy; ++y) {
        int xL = minx, xR = maxx, empty = 0;
        if (step_a > 0) { int xe = (int)((xa16 + 0xFFFF) >> 16); if (xe > xL) xL = xe; }
        else if (step_a < 0) { int xe = (int)(xa16 >> 16); if (xe < xR) xR = xe; }
        else if (wa2_row < 0) empty = 1;
        if (step_b > 0) { int xe = (int)((xb16 + 0xFFFF) >> 16); if (xe > xL) xL = xe; }
        else if (step_b < 0) { int xe = (int)(xb16 >> 16); if (xe < xR) xR = xe; }
        else if (wb2_row < 0) empty = 1;
        if (step_c > 0) { int xe = (int)((xc16 + 0xFFFF) >> 16); if (xe > xL) xL = xe; }
        else if (step_c < 0) { int xe = (int)(xc16 >> 16); if (xe < xR) xR = xe; }
        else if (wc2_row < 0) empty = 1;

        if (!empty && xL <= xR) {
            int u = u_row + du_dx * (xL - minx);
            int v = v_row + dv_dx * (xL - minx);
            uint8_t *prow = framebuffer + y * WAIFU_FM_WIDTH;
            for (int x = xL; x <= xR; ++x) {
                prow[x] = src[rep[(v >> 8) & (Q8_ONE - 1)] * sw + rep[(u >> 8) & (Q8_ONE - 1)]];
                u += du_dx; v += dv_dx;
            }
        }
        wa2_row += rstep_a; wb2_row += rstep_b; wc2_row += rstep_c;
        u_row += du_dy; v_row += dv_dy;
        xa16 += dxa; xb16 += dxb; xc16 += dxc;
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
    if (!src || sw <= 0 || sh <= 0) return;
    if (!p0.ok || !p1.ok || !p2.ok || !p3.ok) return;
    TexV a = {p0.x, p0.y, 0, 0};
    TexV b = {p1.x, p1.y, Q8_ONE, 0};
    TexV c = {p2.x, p2.y, Q8_ONE, Q8_ONE};
    TexV d = {p3.x, p3.y, 0, Q8_ONE};
#if defined(WAIFU_FM_CD32X)
    draw_textured_tri_affine_cd32x(src, sw, sh, a, b, c, gray);
    draw_textured_tri_affine_cd32x(src, sw, sh, a, c, d, gray);
#else
    draw_textured_tri_ex(src, sw, sh, a, b, c, gray);
    draw_textured_tri_ex(src, sw, sh, a, c, d, gray);
#endif
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

#if defined(WAIFU_FM_CD32X)
static void draw_solid_tri(ScreenPt a, ScreenPt b, ScreenPt c, uint8_t color)
{
#if defined(WAIFU_CD32X_BOARD_FLAT_TOP) && WAIFU_CD32X_BOARD_FLAT_TOP
    cd32x_fill_solid_tri_fast(a, b, c, color);
#else
    if (!a.ok || !b.ok || !c.ok) return;
    int minx = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    int maxx = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    int miny = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    int maxy = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    if (minx < -8192 || maxx > 8192 || miny < -8192 || maxy > 8192) return;
    int den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (den == 0) return;
    if (minx < 0) minx = 0;
    if (maxx >= WAIFU_FM_WIDTH) maxx = WAIFU_FM_WIDTH - 1;
    if (miny < 0) miny = 0;
    if (maxy >= WAIFU_FM_HEIGHT) maxy = WAIFU_FM_HEIGHT - 1;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            int px2 = x * 2 + 1, py2 = y * 2 + 1;
            int wa2 = (b.y - c.y) * (px2 - c.x * 2) + (c.x - b.x) * (py2 - c.y * 2);
            int wb2 = (c.y - a.y) * (px2 - c.x * 2) + (a.x - c.x) * (py2 - c.y * 2);
            int wc2 = den * 2 - wa2 - wb2;
            if ((den > 0 && wa2 >= 0 && wb2 >= 0 && wc2 >= 0) ||
                (den < 0 && wa2 <= 0 && wb2 <= 0 && wc2 <= 0)) {
                put_px(x, y, color);
            }
        }
    }
#endif
}

static ScreenPt midpoint_pt(ScreenPt a, ScreenPt b)
{
    ScreenPt m;
    m.x = (a.x + b.x) / 2;
    m.y = (a.y + b.y) / 2;
    m.ok = a.ok && b.ok;
    return m;
}

static void draw_cd32x_projected_card_back(ScreenPt p0, ScreenPt p1, ScreenPt p2, ScreenPt p3)
{
    if (!p0.ok || !p1.ok || !p2.ok || !p3.ok) return;
    draw_solid_tri(p0, p1, p2, IDX_DARK_BROWN);
    draw_solid_tri(p0, p2, p3, IDX_DARK_BROWN);
    line_i(p0.x, p0.y, p1.x, p1.y, IDX_CARD_RIM);
    line_i(p1.x, p1.y, p2.x, p2.y, IDX_CARD_RIM);
    line_i(p2.x, p2.y, p3.x, p3.y, IDX_CARD_RIM);
    line_i(p3.x, p3.y, p0.x, p0.y, IDX_CARD_RIM);
    ScreenPt top = midpoint_pt(p0, p1);
    ScreenPt right = midpoint_pt(p1, p2);
    ScreenPt bottom = midpoint_pt(p2, p3);
    ScreenPt left = midpoint_pt(p3, p0);
    line_i(top.x, top.y, right.x, right.y, IDX_GOLD_HI);
    line_i(right.x, right.y, bottom.x, bottom.y, IDX_GOLD_DARK);
    line_i(bottom.x, bottom.y, left.x, left.y, IDX_GOLD_HI);
    line_i(left.x, left.y, top.x, top.y, IDX_GOLD_DARK);
}

static void draw_cd32x_projected_support_card(ScreenPt p0, ScreenPt p1, ScreenPt p2, ScreenPt p3)
{
    if (!p0.ok || !p1.ok || !p2.ok || !p3.ok) return;
    draw_solid_tri(p0, p1, p2, IDX_UI_DARK);
    draw_solid_tri(p0, p2, p3, IDX_UI_DARK);
    line_i(p0.x, p0.y, p1.x, p1.y, IDX_SUPPORT_FRAME);
    line_i(p1.x, p1.y, p2.x, p2.y, IDX_SUPPORT_FRAME);
    line_i(p2.x, p2.y, p3.x, p3.y, IDX_SUPPORT_FRAME);
    line_i(p3.x, p3.y, p0.x, p0.y, IDX_SUPPORT_FRAME);
    ScreenPt top = midpoint_pt(p0, p1);
    ScreenPt right = midpoint_pt(p1, p2);
    ScreenPt bottom = midpoint_pt(p2, p3);
    ScreenPt left = midpoint_pt(p3, p0);
    line_i(left.x, left.y, right.x, right.y, IDX_SUPPORT_FRAME_HI);
    line_i(top.x, top.y, bottom.x, bottom.y, IDX_SUPPORT_FRAME_HI);
    line_i((top.x + left.x) / 2, (top.y + left.y) / 2,
           (bottom.x + right.x) / 2, (bottom.y + right.y) / 2, IDX_SUPPORT_FRAME);
    line_i((top.x + right.x) / 2, (top.y + right.y) / 2,
           (bottom.x + left.x) / 2, (bottom.y + left.y) / 2, IDX_SUPPORT_FRAME);
}
#endif

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
    int support = is_support_card(card_id);
#if defined(WAIFU_FM_CD32X)
    const uint8_t *tex = (back || support) ? NULL : card_face_ptr(card_id);
#else
    const uint8_t *tex = back ? waifu_assets_card_back() : (support ? waifu_assets_support_face() : card_face_ptr(card_id));
#endif
    if (back) gray = 0;
    ScreenPt p0 = project_point(cam, v3(cx - hw, y, cz - hz));
    ScreenPt p1 = project_point(cam, v3(cx + hw, y, cz - hz));
    ScreenPt p2 = project_point(cam, v3(cx + hw, y, cz + hz));
    ScreenPt p3 = project_point(cam, v3(cx - hw, y, cz + hz));
    /* Player-side cards face YOU. COM-side cards are rotated 180 degrees on
       the board plane so they face the opponent instead of always facing YOU. */
    if (defense) {
        if (row <= 1) {
#if defined(WAIFU_FM_CD32X)
            if (back) draw_cd32x_projected_card_back(p3, p0, p1, p2);
            else if (support) draw_cd32x_projected_support_card(p3, p0, p1, p2);
            else
#endif
            draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p3, p0, p1, p2, gray);
        } else {
#if defined(WAIFU_FM_CD32X)
            if (back) draw_cd32x_projected_card_back(p1, p2, p3, p0);
            else if (support) draw_cd32x_projected_support_card(p1, p2, p3, p0);
            else
#endif
            draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p1, p2, p3, p0, gray);
        }
    } else {
        if (row <= 1) {
#if defined(WAIFU_FM_CD32X)
            if (back) draw_cd32x_projected_card_back(p2, p3, p0, p1);
            else if (support) draw_cd32x_projected_support_card(p2, p3, p0, p1);
            else
#endif
            draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p2, p3, p0, p1, gray);
        } else {
#if defined(WAIFU_FM_CD32X)
            if (back) draw_cd32x_projected_card_back(p0, p1, p2, p3);
            else if (support) draw_cd32x_projected_support_card(p0, p1, p2, p3);
            else
#endif
            draw_projected_card_quad_ex(tex, WAIFU_CARD_W, WAIFU_CARD_H, p0, p1, p2, p3, gray);
        }
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
    /* Keep the cursor visible: far/upper zones (especially enemy rows) can project
       off the top of the top-down view, leaving the player targeting "blind".
       Clamp the box corners to the screen so a cursor is always drawn (at the edge
       if the zone is off-screen).  On-screen zones are unchanged (already in range). */
    p0.x = p0.x < 0 ? 0 : (p0.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : p0.x); p0.y = p0.y < 0 ? 0 : (p0.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : p0.y);
    p1.x = p1.x < 0 ? 0 : (p1.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : p1.x); p1.y = p1.y < 0 ? 0 : (p1.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : p1.y);
    p2.x = p2.x < 0 ? 0 : (p2.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : p2.x); p2.y = p2.y < 0 ? 0 : (p2.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : p2.y);
    p3.x = p3.x < 0 ? 0 : (p3.x >= WAIFU_FM_WIDTH ? WAIFU_FM_WIDTH - 1 : p3.x); p3.y = p3.y < 0 ? 0 : (p3.y >= WAIFU_FM_HEIGHT ? WAIFU_FM_HEIGHT - 1 : p3.y);
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
    int sy = WAIFU_HAND_Y_BASE + ((target_row <= 1) ? g_enemy_hand_offset_y : g_player_hand_offset_y) - 2;
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
    const int w = WAIFU_BATTLE_CARD_W, h = WAIFU_BATTLE_CARD_H;
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
        uint8_t c = (off == 0) ? core : ((i_abs(off) <= 2) ? IDX_RED : IDX_FLAME3);
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
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
    /* PC-FX/CD32X: do not dither-walk the framebuffer for partial fades.
       These targets apply fade as palette intensity.  CD32X in particular can
       present the title as a direct 32X framebuffer surface, so the host-style
       black dither would touch the wrong path and leave the visible title
       unfaded. */
    if ((int)visible < g_video_fade_visible_q8) g_video_fade_visible_q8 = (int)visible;
#if defined(WAIFU_FM_PCFX)
    /* PC-FX still clears at full black to avoid a one-frame stale KING surface
       flash when the following state restores full palette intensity.  CD32X
       does not do this here: its title surface is already resident in the 32X
       framebuffer pages and must survive the fully-black first title frame. */
    if ((int)visible <= 0) clear_screen(IDX_BLACK);
#endif
    return;
#endif
    static const uint8_t bayer[8][8] = {
        { 0,48,12,60, 3,51,15,63}, {32,16,44,28,35,19,47,31},
        { 8,56, 4,52,11,59, 7,55}, {40,24,36,20,43,27,39,23},
        { 2,50,14,62, 1,49,13,61}, {34,18,46,30,33,17,45,29},
        {10,58, 6,54, 9,57, 5,53}, {42,26,38,22,41,25,37,21}
    };
    int threshold = (int)(((Q8_ONE - visible) * 64 + Q8_HALF) >> Q8_SHIFT);
    if (threshold <= 0) return;
    if (threshold >= 64) { clear_screen(IDX_BLACK); return; }
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        for (int x = 0; x < WAIFU_FM_WIDTH; ++x) {
            if (bayer[y & 7][x & 7] < threshold) framebuffer[y * WAIFU_FM_WIDTH + x] = IDX_BLACK;
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

#ifdef WAIFU_FM_PCFX
#define WAIFU_FAST_TRANSITION_HALF_FRAMES 18
#else
#define WAIFU_FAST_TRANSITION_HALF_FRAMES 24
#endif

#ifdef WAIFU_FM_PCFX
#define WAIFU_TITLE_FADE_FRAMES 36
#else
#define WAIFU_TITLE_FADE_FRAMES 4
#endif

static void draw_field_pair_for_battle(Camera cam, int atk_col, int atk_row, int atk_id, int atk_back,
                                       int def_col, int def_row, int def_id, int def_back)
{
    render_board_cached(cam);
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
    if (local0 < WAIFU_BATTLE_PRELUDE_FRAMES) {
        Camera cam = side_battle_camera(atk_row);
        draw_field_pair_for_battle(cam, atk_col, atk_row, atk_id, atk_back,
                                        def_col, def_row, def_id, def_back);
        return;
    }

    int local = local0 - WAIFU_BATTLE_PRELUDE_FRAMES;
    const int ax = WAIFU_BATTLE_CARD_X0, ay = WAIFU_BATTLE_CARD_Y;
    const int dx = WAIFU_BATTLE_CARD_X1, dy = WAIFU_BATTLE_CARD_Y;
    const int slide_dur = WAIFU_BATTLE_SLIDE_FRAMES;
    const int atk_flip_start = slide_dur;
    const int flip_dur = WAIFU_BATTLE_FLIP_FRAMES;
    const int def_flip_start = atk_flip_start + (atk_back ? flip_dur : 0);
    int reveal_end = def_flip_start + (def_back ? flip_dur : 0);
    int pause_after_reveal = WAIFU_BATTLE_REVEAL_PAUSE_FRAMES;
    int ram_start = reveal_end + pause_after_reveal;
    int ram_dur = WAIFU_BATTLE_RAM_FRAMES;
    int ram_contact = ram_start + ((ram_dur * 65 + 50) / 100);
    int counter_start = ram_start + ram_dur + WAIFU_BATTLE_COUNTER_GAP_FRAMES;
    int counter_dur = WAIFU_BATTLE_RAM_FRAMES;
    int hit_hold_start = (outcome == BATTLE_DESTROY_ATTACKER) ? (counter_start + ((counter_dur * 60 + 50) / 100)) : ram_contact;
    int burn_start = (outcome == BATTLE_DESTROY_ATTACKER) ? (counter_start + counter_dur + WAIFU_BATTLE_COUNTER_GAP_FRAMES)
                                                          : (ram_start + ram_dur + WAIFU_BATTLE_BURN_DELAY_FRAMES);
    if (outcome == BATTLE_DESTROY_BOTH) burn_start = ram_start + ram_dur + (WAIFU_BATTLE_BURN_DELAY_FRAMES / 2);
    int burn_dur = BATTLE_BURN_DUR;

    /* SFX are tied to the visible cut-in beats, not to battle resolution. */
    if (local == ram_start) waifu_sound_play(WAIFU_SOUND_LASER_SHOOT);
    if (outcome == BATTLE_DESTROY_ATTACKER && local == counter_start) waifu_sound_play(WAIFU_SOUND_LASER_SHOOT);
    if ((outcome == BATTLE_DESTROY_DEFENDER || outcome == BATTLE_DESTROY_ATTACKER || outcome == BATTLE_DESTROY_BOTH) &&
        local == burn_start) waifu_sound_play(WAIFU_SOUND_CARD_DESTROYED);

    if (local < slide_dur) {
        int32_t e = q8_smooth_ratio(local, slide_dur);
        int ax0 = lerp_i(-WAIFU_BATTLE_CARD_W, ax, e);
        int dx0 = lerp_i(WAIFU_FM_WIDTH + 8, dx, e);
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
        } else if (outcome == BATTLE_NO_DESTROY && local >= ram_contact && local < burn_start && damage_text && strcmp(damage_text, "0") != 0) {
            flash_attacker = 1;
            shake_attacker = ((local & 1) ? -2 : 2);
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
        } else if (outcome == BATTLE_NO_DESTROY && local >= hit_hold_start && local < burn_start && damage_text && strcmp(damage_text, "0") != 0) {
            draw_centered_damage_text_in_card(atk_x + shake_attacker, ay, damage_text);
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
    rect_fill(WAIFU_FM_WIDTH - 138, WAIFU_FM_HEIGHT - 42, 138, 42, IDX_BLACK);
    {
        const char *name = is_monster_card(def_id) ? waifu_card_names[def_id] : (is_support_card(def_id) ? support_card_name(def_id) : "???");
        draw_wrapped_text_small(WAIFU_FM_WIDTH - 134, WAIFU_FM_HEIGHT - 41, name, 20, IDX_WHITE, IDX_BLACK);
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
    fmt_i32_dec(dmg, (int)sizeof(dmg), delta);
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
#ifdef WAIFU_FM_PCFX
static int story_save_exists_device(int ext);
#endif

static void draw_result_screen(int f, const char *msg)
{
    int local = f - 1750;
    if (local < 0) local = 0;
    int32_t t = q8_smooth_ratio(local, 90);
    Camera cam = lerp_camera(battle_top_camera(), player_camera(), t);
    render_board_cached(cam);
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
        int x = (WAIFU_FM_WIDTH - tw) / 2;
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
    int ox = WAIFU_UI_CENTER_DX;
    (void)f;
    clear_screen(IDX_BLACK);
    draw_panel_rect(ox + 31, 24, 194, 178, IDX_UI_DARK);
    draw_centered_text_scaled(37, won ? "DUEL VICTORY" : "DUEL DEFEAT", 1, won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    hline(ox + 45, ox + 210, 56, IDX_UI_LIGHT);

    int cards_used = 4;
    int turns = 3;
    int lp_left = won ? 5600 : (g_force_lp_loss_demo ? 0 : 1200);
    int deck_left = won ? 36 : (g_force_deckout_demo ? 0 : 30);
    int score = 0;
    char rank = battle_rank_from_stats(won, lp_left, cards_used, turns, &score);
    char line[64];

    draw_text(ox + 55, 74, "RESULT", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 150, 74, won ? "WIN" : "LOSE", won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), lp_left);
    draw_text(ox + 55, 94, "LP LEFT", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 160, 94, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), cards_used);
    draw_text(ox + 55, 112, "CARDS USED", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 176, 112, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), deck_left);
    draw_text(ox + 55, 130, "DECK LEFT", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 176, 130, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), score);
    draw_text(ox + 55, 148, "SCORE", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 152, 148, line, IDX_GOLD_HI, IDX_BLACK);

    waifu_str_copy(line, (int)sizeof(line), "RANK "); waifu_str_cat_char(line, (int)sizeof(line), rank);
    draw_centered_text_scaled(170, line, 2, IDX_GOLD_HI, IDX_BLACK);
    draw_text_small(ox + 50, WAIFU_UI_BOTTOM_Y(210), "PRESS RUN: RETURN TO TITLE", IDX_WHITE, IDX_BLACK);
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
        if (rf < 6) apply_black_dither_fade(q8_ratio(rf, 6));
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
    int x = (WAIFU_FM_WIDTH - ((int)strlen(s) * 8)) / 2;
    draw_text(x, y, s, fg, shadow);
}

static void draw_centered_text_scaled(int y, const char *s, int scale, uint8_t fg, uint8_t shadow)
{
    int x = (WAIFU_FM_WIDTH - text_px_width(s, scale)) / 2;
    draw_text_scaled(x, y, s, scale, fg, shadow);
}

#ifdef WAIFU_FM_PCFX
#define TITLE_PROMPT_DIRTY_X 48
#define TITLE_PROMPT_DIRTY_Y 188
#define TITLE_PROMPT_DIRTY_W 160
#define TITLE_PROMPT_DIRTY_H 14
#define TITLE_MENU_DIRTY_X 32
#define TITLE_MENU_DIRTY_Y 120
#define TITLE_MENU_DIRTY_W 192
#define TITLE_MENU_DIRTY_H 100
#define TITLE_MENU_LIST_DIRTY_X 50
#define TITLE_MENU_LIST_DIRTY_Y 136
#define TITLE_MENU_LIST_DIRTY_W 156
#define TITLE_MENU_LIST_DIRTY_H 58
#define TITLE_MENU_HELP_DIRTY_X 48
#define TITLE_MENU_HELP_DIRTY_Y 204
#define TITLE_MENU_HELP_DIRTY_W 168
#define TITLE_MENU_HELP_DIRTY_H 16

static void restore_title_rect(int x, int y, int w, int h)
{
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    const uint8_t *title_img;

    if (w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WAIFU_FM_WIDTH) x1 = WAIFU_FM_WIDTH;
    if (y1 > WAIFU_FM_HEIGHT) y1 = WAIFU_FM_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;

    title_img = waifu_assets_title_screen_img();
    if (!title_img) {
        rect_fill(x0, y0, x1 - x0, y1 - y0, IDX_BLACK);
        return;
    }

    for (int yy = y0; yy < y1; ++yy) {
        copy_u8_fast(framebuffer + yy * WAIFU_FM_WIDTH + x0,
                     title_img + yy * TITLE_SCREEN_W + x0,
                     x1 - x0);
    }
}
#endif

static void draw_asset_loading_screen(void)
{
    char line[40];
    waifu_fm_use_common_palette();
    clear_screen(IDX_BLACK);
    draw_centered_text(102, "LOADING...", IDX_WHITE, IDX_BLACK);
    waifu_str_copy(line, (int)sizeof(line), waifu_assets_loading_label()); waifu_str_cat_char(line, (int)sizeof(line), ' '); waifu_str_cat_i32(line, (int)sizeof(line), waifu_assets_loading_percent()); waifu_str_cat_char(line, (int)sizeof(line), '%');
    draw_centered_text(119, line, IDX_UI_LIGHT, IDX_BLACK);
}

#ifdef WAIFU_FM_PCFX
static void draw_backup_loading_screen(void)
{
    waifu_fm_use_common_palette();
    clear_screen(IDX_BLACK);
    draw_centered_text(96, "BACKUP RAM", IDX_GOLD_HI, IDX_BLACK);
    draw_centered_text(116, "LOADING...", IDX_WHITE, IDX_BLACK);
    frame_mark_full_dirty();
}
#endif

static void draw_transition_black_hold_frame(void)
{
    /* Terminal title/menu fade frame.  Use the common 8bpp path and a full
       black framebuffer before entering loading or battle.  On PC-FX this
       prevents the old 16M title KING surface from being re-presented for one
       frame while the video backend changes modes. */
    waifu_fm_use_common_palette();
    clear_screen(IDX_BLACK);
    /* Hold the palette fully black for the whole black-hold, do NOT let it snap
       back to full brightness here.  The PC-FX palette write lands on the VCE
       immediately, but the cleared (black) framebuffer only reaches the screen
       on the next KRAM page flip -- so resetting the fade to full while the
       displayed page still holds the just-dimmed scene flashes it fully lit for
       one frame before black.  Keeping the fade at 0 means the palette stays
       black until the black framebuffer is actually shown.  This is the common
       fade-to-black exit used by deck-editor, story fire/plaza, and menu
       transitions, so the fix covers all of them. */
    apply_black_dither_fade(0);
    frame_mark_full_dirty();
}

typedef void (*WaifuFadeSourceFn)(int frame, void *ctx);

static int draw_fade_to_black_transition(int frame, int fade_frames, int hold_frames,
                                         WaifuFadeSourceFn source, void *ctx)
{
    if (frame < 0) frame = 0;
    if (fade_frames < 1) fade_frames = 1;
    if (hold_frames < 0) hold_frames = 0;
    if (frame < fade_frames) {
        if (source) source(frame, ctx);
        else clear_screen(IDX_BLACK);
        apply_black_dither_fade(Q8_ONE - q8_ratio(frame, fade_frames));
    } else {
        draw_transition_black_hold_frame();
    }
    return frame >= fade_frames + hold_frames;
}

static void draw_title_background(void)
{
    /* The title/menu screen uses its own 256-color palette.  Text and UI
       routines keep using the normal IDX_* slots; those slots are preserved
       in title_screen_palette_rgb so the text remains visually identical. */
    waifu_fm_use_title_palette();
    {
        const uint8_t *title_img = waifu_assets_title_screen_img();
        if (title_img) {
            int tw = TITLE_SCREEN_W, th = TITLE_SCREEN_H;
            waifu_assets_title_screen_dims(&tw, &th);
            draw_card_raw(title_img, tw, th, 0, 0, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT);
        } else clear_screen(IDX_BLACK);
    }
}

static void draw_title_logo(void)
{
    draw_centered_text_scaled(20, "SHATTERED", 2, IDX_GOLD_HI, IDX_BLACK);
    draw_centered_text_scaled(38, "DECKS", 2, IDX_WHITE, IDX_BLACK);
}

static void draw_title_prompt(int f)
{
    if (((f / 24) & 1) == 0) {
        draw_centered_text(190, "PRESS RUN TO START", IDX_WHITE, IDX_BLACK);
    }
    draw_centered_text(208, "(C) 2026 GAMEBLABLA", IDX_WHITE, IDX_BLACK);
}

static void draw_menu_overlay(int selected)
{
    int has_save = story_save_exists();
    const char *help = "RANDOM DECK / FREE DUEL";
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

static void draw_menu_screen(int selected)
{
    draw_title_background();
    draw_title_logo();
    draw_menu_overlay(selected);
}

/* Composes the static title/menu background (image + logo) into the framebuffer
   and marks the frame fully dirty. On a hardware-text platform this is done only
   on entry / selection change so the resident KING 16M surface and VDC overlay
   are not rewritten every frame; software platforms call it as part of their
   per-frame redraw. */
static void draw_title_full_event(int f)
{
    (void)f;
    clear_screen(IDX_BLACK);
    draw_title_background();
    draw_title_logo();
    frame_mark_full_dirty();
}

static void transition_draw_menu_source(int frame, void *ctx)
{
    int selected = ctx ? *(int *)ctx : 0;
    (void)frame;
    if (waifu_platform_text_overlay_is_hardware()) {
        /* The menu overlay is already resident on the hardware text layer.
           During the fade, only advance the fade mask; redrawing it here makes
           PC-FX fade timing hitch before asset loading starts. */
        (void)selected;
        waifu_fm_use_title_palette();
    } else {
        draw_menu_screen(selected);
    }
}

#ifdef WAIFU_FM_PCFX
static void transition_draw_load_device_source(int frame, void *ctx)
{
    int selected = ctx ? *(int *)ctx : 0;
    (void)frame;
    (void)selected;
    /* The load-device picker overlay is already on the VDC layer.  Leave it in
       place and fade over it instead of rewriting backup status text each
       transition frame. */
    waifu_fm_use_title_palette();
}
#endif

static void render_title_sequence(int f)
{
    clear_screen(IDX_BLACK);
    if (f < 168) {
        draw_title_background();
        draw_title_logo();
        draw_title_prompt(f);
        if (f < WAIFU_TITLE_FADE_FRAMES) apply_black_dither_fade(q8_ratio(f, WAIFU_TITLE_FADE_FRAMES));
        else if (f >= 168 - WAIFU_TITLE_FADE_FRAMES) apply_black_dither_fade(Q8_ONE - q8_ratio(f - (168 - WAIFU_TITLE_FADE_FRAMES), WAIFU_TITLE_FADE_FRAMES));
    } else if (f < 245) {
        int sel = (f < 220) ? 0 : 1;
        draw_menu_screen(sel);
        if (f < 168 + WAIFU_TITLE_FADE_FRAMES) apply_black_dither_fade(q8_ratio(f - 168, WAIFU_TITLE_FADE_FRAMES));
    } else if (f < 285) {
        draw_menu_screen(1);
        apply_black_dither_fade(Q8_ONE - q8_ratio(f - 275, 10));
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

    int card_x = lerp_i(-126, WAIFU_UI_CENTER_DX + 9, vis);
    int card_y = WAIFU_BATTLE_CARD_Y;
    draw_big_battle_card(id, card_x, card_y, 0);

    int tx = WAIFU_UI_CENTER_DX + 136;
    int y = 24;
    draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
    draw_wrapped_text_small(tx, y, waifu_card_names[id], 20, IDX_WHITE, IDX_BLACK); y += 24;
    draw_stars_line(tx, y, card_star_count(id)); y += 13;

    char line[64];
    fmt_join2(line, (int)sizeof(line), waifu_card_attr[id], " / ", waifu_card_tribe[id]);
    draw_wrapped_text_small(tx, y, line, 20, IDX_WHITE, IDX_BLACK); y += 20;

    draw_text_small(tx, y, "LORE", IDX_GOLD_HI, IDX_BLACK); y += 11;
    draw_wrapped_text_small(tx, y, waifu_card_desc[id], 20, IDX_WHITE, IDX_BLACK); y += 54;

    fmt_label_u32(line, (int)sizeof(line), "ATK", (unsigned)waifu_card_atk[id]);
    draw_text_small(tx, WAIFU_UI_BOTTOM_Y(197), line, IDX_GOLD_HI, IDX_BLACK);
    fmt_label_u32(line, (int)sizeof(line), "DEF", (unsigned)waifu_card_def[id]);
    draw_text_small(tx, WAIFU_UI_BOTTOM_Y(209), line, IDX_GOLD_HI, IDX_BLACK);
    if (((local / 16) & 1) == 0) draw_text_small((WAIFU_FM_WIDTH - 56) / 2, WAIFU_FM_HEIGHT - 17, "B: BACK", IDX_WHITE, IDX_BLACK);

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
    render_board_cached(cam);
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
    waifu_fm_use_common_palette();
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

    render_board_cached(cam);
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
        render_board_cached(cam);
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

    render_board_cached(cam);

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

#ifndef WAIFU_FM_NO_HEADLESS_MAIN
/* PNG/showcase dump helpers are part of the headless runner tooling and use the
   host filesystem; they are excluded from non-headless (game) builds. */
static void ensure_dir(const char *out_dir)
{
    char cmd[512];
    waifu_str_copy(cmd, (int)sizeof(cmd), "mkdir -p '"); waifu_str_cat(cmd, (int)sizeof(cmd), out_dir); waifu_str_cat_char(cmd, (int)sizeof(cmd), '\'');
    system(cmd);
}

static void write_frame_png(const char *out_dir, int f)
{
    char path[512];
    waifu_str_copy(path, (int)sizeof(path), out_dir); waifu_str_cat(path, (int)sizeof(path), "/frame_"); waifu_str_cat_u32_zw(path, (int)sizeof(path), (unsigned)f, 5); waifu_str_cat(path, (int)sizeof(path), ".png");
    cfx_write_png8(path, framebuffer, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, waifu_fm_palette_rgb());
}

static void write_showcase(const char *out_dir)
{
    int frames[] = {0, 30, 72, 120, 150, 185, 210, 230, 260, 300, TITLE_SEQUENCE_FRAMES+0, TITLE_SEQUENCE_FRAMES+40, TITLE_SEQUENCE_FRAMES+130, TITLE_SEQUENCE_FRAMES+170, TITLE_SEQUENCE_FRAMES+245, TITLE_SEQUENCE_FRAMES+DUEL_PREVIEW_END, TITLE_SEQUENCE_FRAMES+DUEL_PREVIEW_END+32, TITLE_SEQUENCE_FRAMES+226, TITLE_SEQUENCE_FRAMES+286, TITLE_SEQUENCE_FRAMES+350, TITLE_SEQUENCE_FRAMES+430, TITLE_SEQUENCE_FRAMES+540, TITLE_SEQUENCE_FRAMES+650, TITLE_SEQUENCE_FRAMES+790, TITLE_SEQUENCE_FRAMES+965, TITLE_SEQUENCE_FRAMES+1305, TITLE_SEQUENCE_FRAMES+1450, TITLE_SEQUENCE_FRAMES+1750, TITLE_SEQUENCE_FRAMES+1840, TITLE_SEQUENCE_FRAMES+1970, TITLE_SEQUENCE_FRAMES+2225, TITLE_SEQUENCE_FRAMES+2305, TITLE_SEQUENCE_FRAMES+2470, TITLE_SEQUENCE_FRAMES+2820, TITLE_SEQUENCE_FRAMES+3020};
    char path[512];
    ensure_dir(out_dir);
    for (size_t i = 0; i < sizeof(frames)/sizeof(frames[0]); ++i) {
        render_frame(frames[i]);
        waifu_str_copy(path, (int)sizeof(path), out_dir); waifu_str_cat(path, (int)sizeof(path), "/show_"); waifu_str_cat_u32_z2(path, (int)sizeof(path), (unsigned)i); waifu_str_cat(path, (int)sizeof(path), "_f"); waifu_str_cat_u32_zw(path, (int)sizeof(path), (unsigned)frames[i], 3); waifu_str_cat(path, (int)sizeof(path), ".png");
        cfx_write_png8(path, framebuffer, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, waifu_fm_palette_rgb());
    }
}
#endif /* !WAIFU_FM_NO_HEADLESS_MAIN */



/* ------------------------------------------------------------------------- */
/* Platform-agnostic game API used by the SDL 1.2 backend and by headless      */
/* command playback. SDL never drives the old scripted movie path: Battle Mode */
/* is now an input-driven state machine. Headless can feed the same API from   */
/* an external command file.                                                   */

typedef enum WaifuInteractiveState {
    WAIFU_I_TITLE = 0,
    WAIFU_I_TITLE_TO_MENU,
    WAIFU_I_MENU,
    WAIFU_I_MENU_TO_STORY,
    WAIFU_I_MENU_TO_BATTLE,
    WAIFU_I_MENU_TO_LOAD,
    WAIFU_I_STORY_NAME,
    WAIFU_I_STORY_NAME_TO_INTRO,
    WAIFU_I_STORY_INTRO,
    WAIFU_I_STORY_FIRE,
    WAIFU_I_STORY_FIRE_TO_DECK,
    WAIFU_I_STORY_MAP,
    WAIFU_I_STORY_PYRAMID,
    WAIFU_I_STORY_SAVE,
#ifdef WAIFU_FM_PCFX
    WAIFU_I_STORY_SAVE_DEVICE,
    WAIFU_I_STORY_LOAD_DEVICE_TO_MAP,
#endif
    WAIFU_I_STORY_TO_PLAZA,
    WAIFU_I_STORY_PLAZA,
    WAIFU_I_STORY_PLAZA_TO_DECK,
    WAIFU_I_STORY_ENDING,
    WAIFU_I_STORY_ENDING_CREDITS,
    WAIFU_I_DECK_EDITOR,
    WAIFU_I_DECK_PREVIEW,
    WAIFU_I_DECK_EDITOR_TO_PYRAMID,
    WAIFU_I_DECK_EDITOR_TO_BATTLE,
    WAIFU_I_BATTLE,
    WAIFU_I_LOADING_ASSETS,
#ifdef WAIFU_FM_PCFX
    /* PC-FX only: pick which backup device (internal / FX-BMP) to load from. */
    WAIFU_I_STORY_LOAD_DEVICE,
    /* PC-FX only: visible backup-RAM load handoff before drawing the 3D map. */
    WAIFU_I_STORY_LOAD_TO_MAP,
#endif
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
    IB_PLAYER_FUSION_TARGET,
    IB_PLAYER_FUSION_ANIM,
    IB_PLAYER_BATTLE,
    IB_PLAYER_RETURN_TOP,
    IB_TURN_TO_COM,
    IB_COM_SELECT,
    IB_COM_PLACE,
    IB_COM_EQUIP_ANIM,
    IB_COM_BATTLE,
    IB_COM_RETURN,
    IB_TURN_TO_PLAYER,
    IB_PLAYER_DRAW,
    IB_RESULT,
    IB_TALLY,
    /* Appended at the end so existing g_b_phase values (e.g. in saved states)
       keep their numbering.  Smooth camera lift from the hand view up to the
       tactical top view, and the reverse descent back down to the hand. */
    IB_PLAYER_HAND_TO_TOP,
    IB_PLAYER_TOP_TO_HAND,
    /* COM beat between landing a monster and equipping it: hold on the field so
       the just-placed monster is visible in the top view, then cycle the COM
       hand and settle the (face-down) cursor on the equip card before the equip
       animation runs. */
    IB_COM_EQUIP_SELECT,
    IB_COM_THUNDER_ANIM,
    IB_PLAYER_SUPPORT_ANIM,
    /* Story-win reward reveal shown after the victory tally: displays the card
       the player just earned before returning to the sanctum map. */
    IB_REWARD
} WaifuBattlePhase;

#define I_HAND 5
#define I_FIELD 5
#define FUSION_MAX_MATERIALS (I_HAND + 1)
#define FUSION_FIELD_SLOT (-2)
#define CARD_NONE (-1)
#ifdef WAIFU_FM_PCFX
#define BATTLE_ANIM_FRAMES 48
#define DIRECT_ATTACK_ANIM_FRAMES 36
#define WAIFU_THUNDER_CARD_FRAMES 34
#define WAIFU_THUNDER_FADE_FRAMES 12
#define WAIFU_THUNDER_TARGET_GAP_FRAMES 5
#else
#define BATTLE_ANIM_FRAMES 196
#define DIRECT_ATTACK_ANIM_FRAMES 108
#define WAIFU_THUNDER_CARD_FRAMES 62
#define WAIFU_THUNDER_FADE_FRAMES 20
#define WAIFU_THUNDER_TARGET_GAP_FRAMES 10
#endif
#define WAIFU_SUPPORT_REVEAL_FRAMES WAIFU_THUNDER_CARD_FRAMES
#define WAIFU_SUPPORT_TEXT_FRAMES 38
#define WAIFU_SUPPORT_HEAL_AMOUNT 1000
#define WAIFU_FUSION_LANDING_FRAMES 56
#define WAIFU_FUSION_LANDING_HOLD_FRAMES 8

static int g_api_initialized = 0;
static WaifuInteractiveState g_i_state = WAIFU_I_TITLE;
static WaifuInteractiveState g_i_loading_target = WAIFU_I_TITLE;
static int g_i_frame = 0;
static int g_i_menu_selected = 0;
#ifdef WAIFU_FM_PCFX
static int g_i_load_device_sel = 0; /* 0 internal, 1 FX-BMP, 2 back */
static int g_i_save_device_sel = 0; /* 0 internal, 1 FX-BMP, 2 back */
static int g_i_load_pending_device = 0;
#endif
static WaifuFmInput g_prev_input;

static WaifuBattlePhase g_b_phase = IB_OPENING;
static int g_b_frame = 0;
static int g_b_phase_frame = 0;
static int g_b_selected_hand = 0;
static int g_b_selected_player_slot = 0;
static int g_b_selected_com_slot = 0;
static int g_b_preview_card_id = CARD_NONE;
#if defined(WAIFU_FM_CD32X)
static int g_b_preview_art_pending = 0;
#endif
static int g_b_fusion_hand_slots[I_HAND] = {-1, -1, -1, -1, -1};
static int g_b_fusion_count = 0;
static int g_b_fusion_target_slot = -1;
static int g_b_fusion_anim_slots[FUSION_MAX_MATERIALS] = {-1, -1, -1, -1, -1, -1};
static int g_b_fusion_anim_cards[FUSION_MAX_MATERIALS] = {CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE};
static int g_b_fusion_anim_material_kept[FUSION_MAX_MATERIALS] = {0, 0, 0, 0, 0, 0};
static int g_b_fusion_anim_count = 0;
static int g_b_fusion_anim_result = CARD_NONE;
static int g_b_fusion_anim_success = 0;
static int g_b_fusion_anim_equip_only = 0;
static int g_b_fusion_anim_final_card = CARD_NONE;
static int g_b_fusion_anim_final_atk_bonus = 0;
static int g_b_fusion_anim_final_def_bonus = 0;
static int g_b_fusion_anim_final_equips[I_FIELD] = {CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE};
static int g_b_fusion_anim_final_equip_count = 0;
static int g_b_fusion_anim_final_source_index = -1;
static int g_b_fusion_anim_target_slot = -1;
static int g_b_fusion_anim_has_field_card = 0;
static int g_b_place_hand = -1;
static int g_b_place_slot = -1;
static int g_b_place_card = -1;
static int g_b_place_defense = 0;
/* When set, the IB_PLAYER_PLACE flying-card animation is setting a face-down
   Trap onto the support row instead of summoning a monster. */
static int g_b_place_trap = 0;
static int g_b_com_equip_pending_hand = -1;
static int g_b_equip_owner = 0; /* 0 player, 1 COM */
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
static int g_b_thunder_hand = -1;
static int g_b_thunder_card = CARD_NONE;
static int g_b_thunder_owner = 1; /* 0 player, 1 COM */
static int g_b_thunder_slots[I_FIELD] = {-1, -1, -1, -1, -1};
static int g_b_thunder_cards[I_FIELD] = {CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE, CARD_NONE};
static int g_b_thunder_backs[I_FIELD] = {0, 0, 0, 0, 0};
static int g_b_thunder_count = 0;
/* When set, the destruction burst currently playing is the player's auto Trap
   reacting to a COM attack (it reuses the thunder burn animation but is not a
   Thunder card): it targets only the attacking COM monster and returns to the
   COM battle phase (the attack is cancelled) instead of ending a turn. */
static int g_b_trap_counter_active = 0;
/* Field slot holding the player's face-down set Trap that the current auto-trap
   burst is firing from (-1 if the trap was triggered straight out of hand). The
   slot is cleared when the trap resolves. */
static int g_b_trap_field_slot = -1;
static int g_b_support_hand = -1;
static int g_b_support_card = CARD_NONE;
static int g_b_support_kind = -1;
static int g_b_support_lp_from = 0;
static int g_b_support_lp_to = 0;
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
static int g_b_battle_damage = 0;
static int g_b_battle_damage_owner = -1; /* 0 player, 1 COM, -1 none */
static char g_b_damage_text[16] = "0";
static int g_b_com_return_fade = 0;
static int g_b_result = 0;
/* Card earned for a story win, computed once at the victory tally so the reward
   reveal and the storage award show the same card.  CARD_NONE when unset. */
static int g_b_reward_card = CARD_NONE;
static int g_b_turns = 1;
static int g_b_cards_used = 0;
static int g_b_player_fused_this_turn = 0;

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
static int g_i_com_equip_field[I_FIELD];
static int g_i_com_equip_target[I_FIELD];
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
static WaifuDeck g_i_player_deck;
static WaifuDeck g_i_com_deck;
static WaifuDeckRng g_i_deck_rng;
static int g_i_deck_rng_seeded = 0;
#ifdef WAIFU_FM_HEADLESS_TESTS
static int g_i_headless_draw_seed = 17;
#endif
static int g_b_player_monster_played_this_turn = 0;
static int g_b_com_monster_played_this_turn = 0;

#define STORY_NAME_LEN 6
#define STORY_DECK_SIZE 40
#define STORY_SAVE_PATH "waifu_story.sav"
static int g_story_battle_active = 0;
static char g_story_name[STORY_NAME_LEN + 1] = "SERENA";
static int g_story_name_pos = 0;

/* Substitute the player's chosen name in place of the default "Serena"/"SERENA"
   in story prose, returning a pointer to a rotating static buffer so several
   substituted strings can be live in a single draw call. */
static const char *story_subst_name(const char *templ)
{
    static char bufs[2][256];
    static int which = 0;
    char *out;
    int o = 0;
    int i = 0;
    if (!templ) return "";
    out = bufs[which];
    which ^= 1;
    while (templ[i] && o + 1 < (int)sizeof(bufs[0])) {
        if ((templ[i] == 'S' && templ[i+1] == 'e' && templ[i+2] == 'r' &&
             templ[i+3] == 'e' && templ[i+4] == 'n' && templ[i+5] == 'a') ||
            (templ[i] == 'S' && templ[i+1] == 'E' && templ[i+2] == 'R' &&
             templ[i+3] == 'E' && templ[i+4] == 'N' && templ[i+5] == 'A')) {
            int n = 0;
            while (g_story_name[n] && o + 1 < (int)sizeof(bufs[0])) out[o++] = g_story_name[n++];
            i += 6;
        } else {
            out[o++] = templ[i++];
        }
    }
    out[o] = '\0';
    return out;
}
static int g_story_intro_line = 0;
static int g_story_player_deck[STORY_DECK_SIZE];
static int g_story_player_deck_pos = 0;
static int g_story_strong_card = 37;
static int g_story_weak_card = 29;
static int g_story_equip_count = 0;
static int g_story_support_count = 0;

/* Static PC-FX battle views are far more expensive than cursor/text overlays.
   Cache board+field-cards+HUD composites and keep card-check's large art panel
   stable; volatile cursor/help text is drawn after restoring the cached pixels. */
typedef struct WaifuBattleBaseCache {
    int valid;
    Camera cam;
    uint32_t key;
    uint8_t pixels[WAIFU_FM_WIDTH * WAIFU_FM_HEIGHT];
} WaifuBattleBaseCache;

#if !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
static WaifuBattleBaseCache g_b_base_cache;
#if defined(WAIFU_FM_PCFX)
static WaifuBattleBaseCache g_b_base_cache_top;
/* Cached camera keyframes for the hand<->top lift.  Endpoints reuse the normal
   hand and top base caches; only the true in-between camera bases live here. */
static WaifuBattleBaseCache g_b_handtop_mid_cache[WAIFU_PCFX_HANDTOP_ANCHORS - 2];
static int g_b_handtop_prewarm_index = 0;
#endif
#endif

static void invalidate_battle_composite_cache(void)
{
#if !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
    g_b_base_cache.valid = 0;
#if defined(WAIFU_FM_PCFX)
    g_b_base_cache_top.valid = 0;
    for (int i = 0; i < WAIFU_PCFX_HANDTOP_ANCHORS - 2; ++i) g_b_handtop_mid_cache[i].valid = 0;
    g_b_handtop_prewarm_index = 0;
#endif
#endif
}

static uint32_t waifu_hash_step_u32(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t battle_base_visual_key(void)
{
    uint32_t h = 2166136261u;
    int i;
    h = waifu_hash_step_u32(h, (uint32_t)g_you_lp);
    h = waifu_hash_step_u32(h, (uint32_t)g_com_lp);
    h = waifu_hash_step_u32(h, (uint32_t)g_i_player_deck_left);
    h = waifu_hash_step_u32(h, (uint32_t)g_i_com_deck_left);
    for (i = 0; i < I_FIELD; ++i) {
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_com_equip_field[i] + 2));
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_com_equip_target[i] + 2));
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_com_field[i] + 2));
        h = waifu_hash_step_u32(h, (uint32_t)g_i_com_faceup[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_com_defense[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_com_attacked[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_com_atk_bonus[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_com_def_bonus[i]);
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_player_field[i] + 2));
        h = waifu_hash_step_u32(h, (uint32_t)g_i_player_faceup[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_player_defense[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_player_attacked[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_player_atk_bonus[i]);
        h = waifu_hash_step_u32(h, (uint32_t)g_i_player_def_bonus[i]);
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_player_equip_field[i] + 2));
        h = waifu_hash_step_u32(h, (uint32_t)(g_i_player_equip_target[i] + 2));
    }
    return h;
}

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
static void draw_deck_editor(void);
/* On CD32X a deck card-check streams the 112x112 art from CD on demand.  The
   B-press never issues that seek from the editor input frame; it enters the
   preview state, keeps drawing the deck editor while fading to black, blocks on
   the art read at black, then fades the loaded preview back in. */
static int g_deck_preview_art_pending = 0;
#if defined(WAIFU_FM_CD32X)
#define CD32X_CARD_CHECK_LOAD_PENDING 1
#define CD32X_CARD_CHECK_REVEAL       2
/* Fade length (frames) for each leg of the card-check black transition. */
#define CD32X_DECK_CHECK_FADE_FRAMES   14
#endif

#define STORY_MAX_DUELS 5
/* g_story_duel_index is the *currently selected* duel -- the opponent that will
   be (or is being) fought.  g_story_progress is the persistent furthest duel the
   player has unlocked: the frontier.  Beating the frontier advances progress;
   the player may also re-select and replay any already-cleared opponent in
   [0, g_story_progress] from the sanctum map without changing progress. */
static int g_story_duel_index = 0;
static int g_story_progress = 0;
static int g_story_map_cursor = 0;     /* 0 pyramid, 1 plaza */
static int g_story_pyramid_cursor = 0; /* 0 save, 1 editor, 2 quit (to title), 3 back */
static int g_story_scene_anim_frame = 0; /* free-running story 3D sway frame */
static int g_story_plaza_line = 0;
static int g_story_ending_line = 0;
static int g_story_ending_erasing = 0;
static int g_story_saved_flash = 0;
static int g_story_editor_from_pyramid = 0;
static int g_story_save_status = 0; /* 1 saved/loaded, -1 failed/no save */

typedef enum {
    STORY_SPK_SERENA = 0,
    STORY_SPK_OPPONENT = 1,
    STORY_SPK_NARRATOR = 2
} StorySpeaker;

typedef struct {
    StorySpeaker speaker;
    const char *text;
} StoryDialogueLine;

typedef struct {
    const char *name;
    const char *title;
    int portrait_id;
    int boss;
} StoryOpponentInfo;

enum {
    STORY_PORTRAIT_SERENA = 0,
    STORY_PORTRAIT_OPP0,
    STORY_PORTRAIT_OPP1,
    STORY_PORTRAIT_OPP2,
    STORY_PORTRAIT_OPP3,
    STORY_PORTRAIT_OPP4
};

static const StoryOpponentInfo g_story_opponents[STORY_MAX_DUELS] = {
    {"KASEM",   "SUN EXILE",          STORY_PORTRAIT_OPP0, 0},
    {"ANPU",    "JACKAL WARDEN",      STORY_PORTRAIT_OPP1, 0},
    {"RAHOTEP", "WAR-SAINT",          STORY_PORTRAIT_OPP2, 0},
    {"NADIRA",  "VEIL DANCER",        STORY_PORTRAIT_OPP3, 0},
    {"ISYRA",   "ORACLE",             STORY_PORTRAIT_OPP4, 1},
};

static const StoryDialogueLine g_story_duel0_dialogue[] = {
    {STORY_SPK_SERENA,   "So the desert road really was waiting for me. I felt the heat before I even opened my eyes."},
    {STORY_SPK_OPPONENT, "You stand on the Shifting Expanse, girl. Every grain here remembers the fall of the Sun Court."},
    {STORY_SPK_SERENA,   "Then maybe it remembers why these cards keep calling my name. Ever since the market, the deck feels alive."},
    {STORY_SPK_OPPONENT, "Alive? No. Bound. The dynasties pressed vows into crystal, then shattered them into a thousand dueling shards."},
    {STORY_SPK_SERENA,   "And people just play with relics from a dead kingdom?"},
    {STORY_SPK_OPPONENT, "Most do. I don't. Your deck carries the Twilight Seal -- the chain that closed the Gate Beneath the Sands."},
    {STORY_SPK_SERENA,   "That sounds a little too important for a girl who bought her first cards out of a roadside crate."},
    {STORY_SPK_OPPONENT, "Fate likes crude disguises. Defeat me, Serena, and I will believe the desert truly chose you."},
    {STORY_SPK_SERENA,   "Then watch closely, Kasem. If the sands chose me, they'll have to answer for it in a duel."},
};

static const StoryDialogueLine g_story_duel1_dialogue[] = {
    {STORY_SPK_OPPONENT, "No farther. Beyond this ridge stands the Jackal Gate, where the caravan dead surrender their names."},
    {STORY_SPK_SERENA,   "You make that sound like a warning. Usually that means I should keep walking."},
    {STORY_SPK_OPPONENT, "I am Anpu, Ninth Procession warden. My order judges by memory, oath, courage. Your steps rang all three."},
    {STORY_SPK_SERENA,   "I've barely started and already priests in masks are judging me. Wonderful."},
    {STORY_SPK_OPPONENT, "This mask predates me. The jackal lords wore it escorting souls through the underways beneath the dunes."},
    {STORY_SPK_SERENA,   "Underways? More tunnels? More ruins? This whole land feels stacked on top of old secrets."},
    {STORY_SPK_OPPONENT, "The Expanse is only the skin. Below sleep the Hall of Embers, the Mirror Wells, and the Hollow Throne where the seal was forged."},
    {STORY_SPK_SERENA,   "And you think my deck is tied to all of that."},
    {STORY_SPK_OPPONENT, "I think your victory or failure will wake what the sealed kings feared. Show me the measure of your spirit."},
    {STORY_SPK_SERENA,   "All right, Anpu. We can skip the scales and let the cards do the judging."},
};

static const StoryDialogueLine g_story_duel2_dialogue[] = {
    {STORY_SPK_OPPONENT, "Steel your heart. I am Rahotep, last war-saint of the auric host. I kneel to no weak claimant."},
    {STORY_SPK_SERENA,   "Good. I'd be worried if you surrendered before I even drew a hand."},
    {STORY_SPK_OPPONENT, "Boldness is not strength. When the gold banners flew, I led the phalanxes that held the King's Engine at Dawn Bastion."},
    {STORY_SPK_SERENA,   "I've heard three different ruins called the king's last bastion already."},
    {STORY_SPK_OPPONENT, "Because there were many. The empire fell to betrayal, revolt, and void-lit fire from below."},
    {STORY_SPK_SERENA,   "That fire again... the same one from my dreams."},
    {STORY_SPK_OPPONENT, "Your dreams are brushing the old catastrophe. The seal cracked when the court tried to turn dueling rites into a weapon."},
    {STORY_SPK_SERENA,   "So every duel I'm fighting is another piece of that history turning back toward me."},
    {STORY_SPK_OPPONENT, "Exactly. If you cannot break my formation, you cannot survive what waits beyond the temple line."},
    {STORY_SPK_SERENA,   "Then let's test that armor, Rahotep. I didn't come this far to bow before a ghost in gold."},
};

static const StoryDialogueLine g_story_duel3_dialogue[] = {
    {STORY_SPK_OPPONENT, "Careful where you stare, little pilgrim. In the Mirage Courts, desire is another kind of trap."},
    {STORY_SPK_SERENA,   "If you're trying to distract me before the duel, I should tell you it's working a little."},
    {STORY_SPK_OPPONENT, "Ha. Honesty. I like that. I am Nadira, keeper of the Veiled Oasis, and trader in secrets too shameful to voice."},
    {STORY_SPK_SERENA,   "Then you've probably made a fortune off this desert."},
    {STORY_SPK_OPPONENT, "On the contrary, I collect debts. Travelers kneel at my pool to ask what they lost. The water always takes something."},
    {STORY_SPK_SERENA,   "Memories?"},
    {STORY_SPK_OPPONENT, "Names. Promises. Once in a while, a future. The queens used this oasis to glimpse who would carry the seal."},
    {STORY_SPK_SERENA,   "And what did the water show you about me?"},
    {STORY_SPK_OPPONENT, "That you arrive carrying two shadows: the girl you were, and the sovereign you may become if the Void Crown notices you first."},
    {STORY_SPK_SERENA,   "That is exactly the kind of prophecy I hate. Let's settle for cards, Nadira."},
};

static const StoryDialogueLine g_story_duel4_dialogue[] = {
    {STORY_SPK_OPPONENT, "Serena of the broken deck, I have watched your path through dust, fire, mirage. You reach the White Threshold."},
    {STORY_SPK_SERENA,   "You're Isyra... the oracle the others kept circling around without naming."},
    {STORY_SPK_OPPONENT, "Names hold power here. Mine was hidden -- I guarded the seal's last clear reading. Kings feared prophecy more than swords."},
    {STORY_SPK_SERENA,   "Then tell me plainly. Why me? Why the dreams, the demon voice, the pull in these cards?"},
    {STORY_SPK_OPPONENT, "When the empire broke, seven keepers divided the Twilight Seal. Your shard answered not to bloodline, but to recognition."},
    {STORY_SPK_SERENA,   "So I wasn't chosen by birth. I was chosen because I heard it answer."},
    {STORY_SPK_OPPONENT, "Yes. The abyss beneath the old courts has begun to answer back. If it claims the seal first, every ruin will open at once."},
    {STORY_SPK_SERENA,   "Then this duel isn't just a test. It's a key."},
    {STORY_SPK_OPPONENT, "Precisely. Beat me and I yield the route to the sanctum beyond waking. Lose, and return to the surface with fragments."},
    {STORY_SPK_SERENA,   "I didn't cross the Expanse for fragments. Show me the truth, Isyra. I'll win it myself."},
};

static const StoryDialogueLine *story_dialogue_for_duel(int duel, int *count)
{
    if (count) *count = 0;
    switch (duel) {
    case 0: if (count) *count = (int)(sizeof(g_story_duel0_dialogue) / sizeof(g_story_duel0_dialogue[0])); return g_story_duel0_dialogue;
    case 1: if (count) *count = (int)(sizeof(g_story_duel1_dialogue) / sizeof(g_story_duel1_dialogue[0])); return g_story_duel1_dialogue;
    case 2: if (count) *count = (int)(sizeof(g_story_duel2_dialogue) / sizeof(g_story_duel2_dialogue[0])); return g_story_duel2_dialogue;
    case 3: if (count) *count = (int)(sizeof(g_story_duel3_dialogue) / sizeof(g_story_duel3_dialogue[0])); return g_story_duel3_dialogue;
    default: if (count) *count = (int)(sizeof(g_story_duel4_dialogue) / sizeof(g_story_duel4_dialogue[0])); return g_story_duel4_dialogue;
    }
}

static const StoryOpponentInfo *story_opponent_info(void)
{
    int idx = g_story_duel_index;
    if (idx < 0) idx = 0;
    if (idx >= STORY_MAX_DUELS) idx = STORY_MAX_DUELS - 1;
    return &g_story_opponents[idx];
}

static void story_return_to_map_after_duel(void);
static void init_battle_state(void);
static int selected_or_first_live_player_slot(void);
static int first_free_com_slot(void);
static int first_free_com_equip_slot(void);

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

static int player_can_start_fusion(void)
{
    /* A fusion summon (including occupied-zone transforms) counts as the turn's
       single monster action: once a monster has been Set OR fused this turn, no
       further fusion may start. Placement and fusion are mutually exclusive so
       the player can only commit one monster to the field per turn. */
    return !g_b_player_fused_this_turn && !g_b_player_monster_played_this_turn;
}

static int player_can_fusion_to_slot(int slot)
{
    if (slot < 0 || slot >= I_FIELD) return 0;
    if (is_monster_card(g_i_player_field[slot])) return player_can_start_fusion();
    return player_can_start_fusion() && player_can_place_monster();
}

static int player_hand_monster_blocked(void)
{
    return !player_can_place_monster() &&
           (!player_can_start_fusion() || selected_or_first_live_player_slot() < 0);
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

static void clear_player_fusion_queue(void)
{
    int i;
    for (i = 0; i < I_HAND; ++i) g_b_fusion_hand_slots[i] = -1;
    g_b_fusion_count = 0;
    g_b_fusion_target_slot = -1;
}

static int player_fusion_order_for_slot(int slot)
{
    int i;
    for (i = 0; i < g_b_fusion_count; ++i) {
        if (g_b_fusion_hand_slots[i] == slot) return i + 1;
    }
    return 0;
}

static void remove_player_fusion_slot(int slot)
{
    int i, out = 0;
    int next[I_HAND] = {-1, -1, -1, -1, -1};
    for (i = 0; i < g_b_fusion_count; ++i) {
        if (g_b_fusion_hand_slots[i] != slot && out < I_HAND) next[out++] = g_b_fusion_hand_slots[i];
    }
    for (i = 0; i < I_HAND; ++i) g_b_fusion_hand_slots[i] = next[i];
    g_b_fusion_count = out;
}

static int try_queue_player_fusion_slot(int slot)
{
    if (slot < 0 || slot >= I_HAND || g_i_player_used[slot]) return 0;
    if (g_i_player_hand[slot] < 0) return 0;
    if (player_fusion_order_for_slot(slot)) {
        remove_player_fusion_slot(slot);
        return g_b_fusion_count;
    }
    if (g_b_fusion_count >= I_HAND) return g_b_fusion_count;
    g_b_fusion_hand_slots[g_b_fusion_count++] = slot;
    return g_b_fusion_count;
}

static int fusion_material_is_equip(int card_id)
{
    /* Only support cards that use the equip flow normally should become
       ordered equip material in fusion chains.  One-shot supports keep their
       normal hand-only behavior and are discarded if forced into a failed
       fusion chain. */
    return is_equip_support_card(card_id);
}

static void fusion_append_equip_card(int equip_card, int equip_src,
                                     int equips[I_FIELD], int equip_srcs[I_FIELD], int *equip_count,
                                     int *atk_bonus, int *def_bonus)
{
    if (!fusion_material_is_equip(equip_card)) return;
    if (*equip_count < I_FIELD) {
        equips[*equip_count] = equip_card;
        equip_srcs[*equip_count] = equip_src;
        ++(*equip_count);
    }
    *atk_bonus += equip_atk_bonus(equip_card);
    *def_bonus += equip_def_bonus(equip_card);
}

static void fusion_copy_player_field_equips(int target_slot,
                                            int equips[I_FIELD], int equip_srcs[I_FIELD], int *equip_count)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_equip_target[i] == target_slot && fusion_material_is_equip(g_i_player_equip_field[i]) && *equip_count < I_FIELD) {
            equips[*equip_count] = g_i_player_equip_field[i];
            equip_srcs[*equip_count] = -1;
            ++(*equip_count);
        }
    }
}

static void fusion_clear_local_equips(int equips[I_FIELD], int equip_srcs[I_FIELD], int *equip_count)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        equips[i] = CARD_NONE;
        equip_srcs[i] = -1;
    }
    *equip_count = 0;
}

/* When a fusion (or a failed pair) replaces the current monster, drop the
   consumed monster's field-copied equips (src == -1) but KEEP the equip cards
   the player explicitly DOWN-selected into the chain (src >= 0): those are
   meant to land on the final summoned monster regardless of where they sit in
   the chain.  Without this, an equip queued before a later fusion in the chain
   was silently discarded (equipped to neither the fused monster nor the field).
   Recomputes the running atk/def bonus from the kept equips. */
static void fusion_keep_hand_equips(int equips[I_FIELD], int equip_srcs[I_FIELD], int *equip_count,
                                    int *atk_bonus, int *def_bonus)
{
    int i, out = 0;
    *atk_bonus = 0;
    *def_bonus = 0;
    for (i = 0; i < *equip_count; ++i) {
        if (equip_srcs[i] >= 0 && fusion_material_is_equip(equips[i])) {
            equips[out] = equips[i];
            equip_srcs[out] = equip_srcs[i];
            *atk_bonus += equip_atk_bonus(equips[i]);
            *def_bonus += equip_def_bonus(equips[i]);
            ++out;
        }
    }
    for (i = out; i < I_FIELD; ++i) {
        equips[i] = CARD_NONE;
        equip_srcs[i] = -1;
    }
    *equip_count = out;
}

static int player_fusion_should_resolve_hand_before_field(int field_card)
{
    int i;
    int current = CARD_NONE;
    int performed_fusion = 0;
    int failed_pair = 0;
    if (!is_monster_card(field_card)) return 0;

    for (i = 0; i < g_b_fusion_count; ++i) {
        int slot = g_b_fusion_hand_slots[i];
        int card;
        if (slot < 0 || slot >= I_HAND || g_i_player_used[slot]) return 0;
        card = g_i_player_hand[slot];
        if (!is_monster_card(card)) continue;
        if (!is_monster_card(current)) {
            current = card;
        } else {
            int fused = fusion_result_for_cards(current, card);
            if (is_monster_card(fused)) {
                current = fused;
                performed_fusion = 1;
            } else {
                current = card;
                failed_pair = 1;
            }
        }
    }

    return performed_fusion && !failed_pair && is_monster_card(current) &&
           is_monster_card(fusion_result_for_cards(current, field_card));
}

static int prepare_player_fusion_anim(int target_slot)
{
    int i, j;
    int out = 0;
    int field_card = CARD_NONE;
    int hand_before_field = 0;
    int current = CARD_NONE;
    int current_source = -1;
    int current_from_fusion = 0;
    int current_atk_bonus = 0;
    int current_def_bonus = 0;
    int current_equips[I_FIELD];
    int current_equip_srcs[I_FIELD];
    int current_equip_count = 0;
    int pending_equips[I_FIELD];
    int pending_equip_srcs[I_FIELD];
    int pending_equip_count = 0;
    int performed_fusion = 0;
    int failed_pair = 0;
    if (g_b_fusion_count <= 0) return 0;
    if (target_slot < 0 || target_slot >= I_FIELD) return 0;

    fusion_clear_local_equips(current_equips, current_equip_srcs, &current_equip_count);
    fusion_clear_local_equips(pending_equips, pending_equip_srcs, &pending_equip_count);

    for (i = 0; i < FUSION_MAX_MATERIALS; ++i) {
        g_b_fusion_anim_slots[i] = -1;
        g_b_fusion_anim_cards[i] = CARD_NONE;
        g_b_fusion_anim_material_kept[i] = 0;
    }
    for (i = 0; i < I_FIELD; ++i) g_b_fusion_anim_final_equips[i] = CARD_NONE;
    g_b_fusion_anim_final_card = CARD_NONE;
    g_b_fusion_anim_final_atk_bonus = 0;
    g_b_fusion_anim_final_def_bonus = 0;
    g_b_fusion_anim_final_equip_count = 0;
    g_b_fusion_anim_final_source_index = -1;
    g_b_fusion_anim_equip_only = 0;

    field_card = g_i_player_field[target_slot];
    g_b_fusion_anim_target_slot = target_slot;
    g_b_fusion_anim_has_field_card = is_monster_card(field_card);
    if (g_b_fusion_anim_has_field_card) {
        hand_before_field = player_fusion_should_resolve_hand_before_field(field_card);
    }
    if (g_b_fusion_anim_has_field_card && !hand_before_field) {
        g_b_fusion_anim_slots[out] = FUSION_FIELD_SLOT;
        g_b_fusion_anim_cards[out] = field_card;
        ++out;
    }

    for (i = 0; i < g_b_fusion_count; ++i) {
        int slot = g_b_fusion_hand_slots[i];
        if (slot < 0 || slot >= I_HAND || g_i_player_used[slot]) return 0;
        if (out >= FUSION_MAX_MATERIALS) return 0;
        g_b_fusion_anim_slots[out] = slot;
        g_b_fusion_anim_cards[out] = g_i_player_hand[slot];
        ++out;
    }
    if (g_b_fusion_anim_has_field_card && hand_before_field) {
        if (out >= FUSION_MAX_MATERIALS) return 0;
        g_b_fusion_anim_slots[out] = FUSION_FIELD_SLOT;
        g_b_fusion_anim_cards[out] = field_card;
        ++out;
    }

    g_b_fusion_anim_count = out;

    for (i = 0; i < g_b_fusion_anim_count; ++i) {
        int card = g_b_fusion_anim_cards[i];
        if (fusion_material_is_equip(card)) {
            if (is_monster_card(current)) {
                fusion_append_equip_card(card, i, current_equips, current_equip_srcs, &current_equip_count,
                                         &current_atk_bonus, &current_def_bonus);
            } else if (pending_equip_count < I_FIELD) {
                pending_equips[pending_equip_count] = card;
                pending_equip_srcs[pending_equip_count] = i;
                ++pending_equip_count;
            }
            continue;
        }
        if (!is_monster_card(card)) continue;

        if (!is_monster_card(current)) {
            current = card;
            current_source = i;
            current_from_fusion = 0;
            current_atk_bonus = 0;
            current_def_bonus = 0;
            fusion_clear_local_equips(current_equips, current_equip_srcs, &current_equip_count);
            if (g_b_fusion_anim_slots[i] == FUSION_FIELD_SLOT) {
                current_atk_bonus = g_i_player_atk_bonus[target_slot];
                current_def_bonus = g_i_player_def_bonus[target_slot];
                fusion_copy_player_field_equips(target_slot, current_equips, current_equip_srcs, &current_equip_count);
            }
            for (j = 0; j < pending_equip_count; ++j) {
                fusion_append_equip_card(pending_equips[j], pending_equip_srcs[j],
                                         current_equips, current_equip_srcs, &current_equip_count,
                                         &current_atk_bonus, &current_def_bonus);
            }
            fusion_clear_local_equips(pending_equips, pending_equip_srcs, &pending_equip_count);
        } else {
            int fused = fusion_result_for_cards(current, card);
            if (is_monster_card(fused)) {
                current = fused;
                current_source = -1;
                current_from_fusion = 1;
                /* Keep player-selected chain equips on the fused result. */
                fusion_keep_hand_equips(current_equips, current_equip_srcs, &current_equip_count,
                                        &current_atk_bonus, &current_def_bonus);
                performed_fusion = 1;
            } else {
                current = card;
                current_source = i;
                current_from_fusion = 0;
                fusion_keep_hand_equips(current_equips, current_equip_srcs, &current_equip_count,
                                        &current_atk_bonus, &current_def_bonus);
                failed_pair = 1;
            }
        }
    }

    if (is_monster_card(current)) {
        g_b_fusion_anim_final_card = current;
        g_b_fusion_anim_final_atk_bonus = current_atk_bonus;
        g_b_fusion_anim_final_def_bonus = current_def_bonus;
        g_b_fusion_anim_final_equip_count = current_equip_count;
        g_b_fusion_anim_final_source_index = current_source;
        for (i = 0; i < current_equip_count; ++i) {
            g_b_fusion_anim_final_equips[i] = current_equips[i];
            if (current_equip_srcs[i] >= 0 && current_equip_srcs[i] < FUSION_MAX_MATERIALS) {
                g_b_fusion_anim_material_kept[current_equip_srcs[i]] = 1;
            }
        }
        if (!current_from_fusion && current_source >= 0 && current_source < FUSION_MAX_MATERIALS) {
            g_b_fusion_anim_material_kept[current_source] = 1;
        }
    }

    g_b_fusion_anim_success = (performed_fusion && !failed_pair && is_monster_card(current));
    g_b_fusion_anim_equip_only = (!performed_fusion && !failed_pair &&
                                  is_monster_card(current) && current_equip_count > 0);
    g_b_fusion_anim_result = g_b_fusion_anim_success ? current : CARD_NONE;
    return 1;
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

static int first_live_com_monster_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (is_monster_card(g_i_com_field[i])) return i;
    return -1;
}

static int first_live_player_monster_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (is_monster_card(g_i_player_field[i])) return i;
    return -1;
}

static int first_unused_com_support_hand(void)
{
    int i;
    for (i = 0; i < I_HAND; ++i) if (!g_i_com_used[i] && is_equip_support_card(g_i_com_hand[i])) return i;
    return -1;
}

static int first_unused_com_monster_hand(void)
{
    int i;
    for (i = 0; i < I_HAND; ++i) if (!g_i_com_used[i] && is_monster_card(g_i_com_hand[i])) return i;
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
    return is_monster_card(g_i_player_field[g_b_top_col]) ? g_b_top_col : -1;
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
    if (g_b_top_row == ENEMY_CARD_ROW - 1 && g_i_com_equip_field[g_b_top_col] >= 0)
        return g_i_com_equip_field[g_b_top_col];
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

static int story_water_field_active(void)
{
    return g_story_battle_active && g_story_duel_index >= STORY_MAX_DUELS - 1;
}

static int story_water_field_bonus(int id)
{
    if (!story_water_field_active() || !is_monster_card(id)) return 0;
    if (!strcmp(waifu_card_attr[id], "Water") ||
        !strcmp(waifu_card_tribe[id], "Fish") ||
        !strcmp(waifu_card_tribe[id], "Aqua")) {
        return 500;
    }
    if (!strcmp(waifu_card_attr[id], "Fire") ||
        !strcmp(waifu_card_tribe[id], "Insect") || !strcmp(waifu_card_tribe[id], "Machine")) {
        return -500;
    }
    return 0;
}

static int field_card_atk(int owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (!is_monster_card(id)) return 0;
    return card_base_atk(id) + story_water_field_bonus(id) +
           (owner == 0 ? g_i_player_atk_bonus[slot] : g_i_com_atk_bonus[slot]);
}

static int field_card_def(int owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (!is_monster_card(id)) return 0;
    return card_base_def(id) + story_water_field_bonus(id) +
           (owner == 0 ? g_i_player_def_bonus[slot] : g_i_com_def_bonus[slot]);
}

static int com_deck_face_down_attack_threshold(void)
{
    int cards[WAIFU_DECK_SIZE];
    int count = 0;
    int i, j;

    for (i = 0; i < g_i_com_deck.count && count < WAIFU_DECK_SIZE; ++i) {
        int id = g_i_com_deck.cards[i];
        if (is_monster_card(id)) cards[count++] = card_base_atk(id);
    }
    for (i = 0; i < I_FIELD && count < WAIFU_DECK_SIZE; ++i) {
        if (is_monster_card(g_i_com_field[i])) cards[count++] = card_base_atk(g_i_com_field[i]);
    }
    for (i = 0; i < I_HAND && count < WAIFU_DECK_SIZE; ++i) {
        if (!g_i_com_used[i] && is_monster_card(g_i_com_hand[i])) cards[count++] = card_base_atk(g_i_com_hand[i]);
    }
    if (count <= 0) return 1800;
    for (i = 1; i < count; ++i) {
        int v = cards[i];
        j = i - 1;
        while (j >= 0 && cards[j] > v) { cards[j + 1] = cards[j]; --j; }
        cards[j + 1] = v;
    }
    /* Sufficiently strong means roughly the upper third of the current COM deck.
       The AI will not gamble weak cards into face-down attack-position monsters. */
    return cards[(count * 2) / 3];
}

static void fill_ai_card_view(WaifuAiCardView *dst, int card_id, int used, int faceup, int defense, int attacked, int atk, int def)
{
    dst->card_id = card_id;
    dst->used = used;
    dst->faceup = faceup;
    dst->defense = defense;
    dst->attacked = attacked;
    dst->atk = atk;
    dst->def = def;
}

static void build_com_ai_state(WaifuAiState *state)
{
    int i;
    memset(state, 0, sizeof(*state));
    state->card_count = WAIFU_CARD_COUNT;
    state->you_lp = g_you_lp;
    state->com_lp = g_com_lp;
    state->com_can_place_monster = com_can_place_monster();
    state->free_com_slot = first_free_com_slot();
    state->free_com_equip_slot = first_free_com_equip_slot();
    state->first_com_equip_target = first_live_com_monster_slot();
    state->deck_attack_threshold = com_deck_face_down_attack_threshold();
    for (i = 0; i < I_FIELD; ++i) {
        fill_ai_card_view(&state->player_field[i], g_i_player_field[i], 0, g_i_player_faceup[i], g_i_player_defense[i], g_i_player_attacked[i], field_card_atk(0, i), field_card_def(0, i));
        fill_ai_card_view(&state->com_field[i], g_i_com_field[i], 0, g_i_com_faceup[i], g_i_com_defense[i], g_i_com_attacked[i], field_card_atk(1, i), field_card_def(1, i));
    }
    for (i = 0; i < I_HAND; ++i) {
        int id = g_i_com_hand[i];
        fill_ai_card_view(&state->com_hand[i], id, g_i_com_used[i], 1, 0, 0,
                          is_monster_card(id) ? card_base_atk(id) : 0,
                          is_monster_card(id) ? card_base_def(id) : 0);
    }
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
    int base = WAIFU_BOTTOM_INFO_Y;
    rect_fill(0, base, WAIFU_FM_WIDTH, 35, IDX_UI_TEAL);
    hline(0,WAIFU_FM_WIDTH-1,base,IDX_WHITE); hline(0,WAIFU_FM_WIDTH-1,base+1,IDX_UI_LIGHT); hline(0,WAIFU_FM_WIDTH-1,base+2,IDX_DIM);
    for (int y = base+4; y < base+35; y += 3) hline(0,WAIFU_FM_WIDTH-1,y,IDX_UI_TEAL2);
    waifu_str_copy(line, (int)sizeof(line), mode ? mode : "FIELD"); waifu_str_cat(line, (int)sizeof(line), " C"); waifu_str_cat_i32(line, (int)sizeof(line), g_b_top_col + 1); waifu_str_cat(line, (int)sizeof(line), " R"); waifu_str_cat_i32(line, (int)sizeof(line), g_b_top_row + 1);
    draw_text(6, base+6, line, IDX_DIM, IDX_BLACK);
    draw_text_small(6, base+21, "EMPTY ZONE", IDX_WHITE, IDX_BLACK);
}

/* Hidden info for a face-down monster: never reveal name / attribute / stats. */
static void draw_bottom_info_facedown(const char *mode)
{
    int base = WAIFU_BOTTOM_INFO_Y;
    rect_fill(0, base, WAIFU_FM_WIDTH, 35, IDX_UI_TEAL);
    hline(0,WAIFU_FM_WIDTH-1,base,IDX_WHITE); hline(0,WAIFU_FM_WIDTH-1,base+1,IDX_UI_LIGHT); hline(0,WAIFU_FM_WIDTH-1,base+2,IDX_DIM);
    for (int y = base+4; y < base+35; y += 3) hline(0,WAIFU_FM_WIDTH-1,y,IDX_UI_TEAL2);
    draw_text(6, base+6, "SET MONSTER", IDX_WHITE, IDX_BLACK);
    draw_text_small(6, base+21, "FACE-DOWN / HIDDEN", IDX_WHITE, IDX_BLACK);
    if (mode) draw_text_small(WAIFU_FM_WIDTH - 68, base+21, mode, IDX_GOLD_HI, IDX_BLACK);
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
        if (!g_i_com_faceup[g_b_top_col]) draw_bottom_info_facedown(mode ? mode : "TARGET");
        else draw_bottom_info_field(1, g_b_top_col, mode ? mode : "TARGET");
    } else if (g_b_top_row == ENEMY_CARD_ROW - 1 && g_i_com_equip_field[g_b_top_col] >= 0) {
        draw_bottom_info(g_i_com_equip_field[g_b_top_col], mode ? mode : "EQUIP");
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
    if (is_guard_support_card(card_id)) return 250;
    return 500;
}

static int equip_def_bonus(int card_id)
{
    if (is_guard_support_card(card_id)) return 800;
    return support_card_kind(card_id) == 0 ? 300 : 0;
}


static int count_live_player_monsters(void)
{
    int i, n = 0;
    for (i = 0; i < I_FIELD; ++i) if (is_monster_card(g_i_player_field[i])) ++n;
    return n;
}

static int count_live_com_monsters(void)
{
    int i, n = 0;
    for (i = 0; i < I_FIELD; ++i) if (is_monster_card(g_i_com_field[i])) ++n;
    return n;
}

static int selected_or_first_live_player_slot(void)
{
    if (g_b_selected_player_slot >= 0 && g_b_selected_player_slot < I_FIELD &&
        is_monster_card(g_i_player_field[g_b_selected_player_slot])) {
        return g_b_selected_player_slot;
    }
    return first_live_player_monster_slot();
}

static int first_attackable_com_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (is_monster_card(g_i_com_field[i]) && !g_i_com_attacked[i] && !g_i_com_defense[i]) return i;
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

static int first_free_com_equip_slot(void)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) if (g_i_com_equip_field[i] < 0) return i;
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

static void clear_com_equips_for_target(int target_slot)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_com_equip_target[i] == target_slot) {
            g_i_com_equip_field[i] = CARD_NONE;
            g_i_com_equip_target[i] = -1;
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

static void update_music_for_current_state(void);

static void set_battle_phase(WaifuBattlePhase phase)
{
    WaifuBattlePhase prev = g_b_phase;
    g_b_phase = phase;
    g_b_phase_frame = 0;
    if (phase != prev) {
        if (phase == IB_TURN_TO_COM || phase == IB_TURN_TO_PLAYER) waifu_sound_play(WAIFU_SOUND_TURN_PASSED);
        /* Loss jingle is CD-DA on PC-FX and a music track on host; no PCM SFX. */
    }
    update_music_for_current_state();
}

static int battle_phase_accepts_player_input(void)
{
    switch (g_b_phase) {
    case IB_PLAYER_HAND:
        return 1;
    case IB_CARD_PREVIEW:
    case IB_FIELD_CARD_PREVIEW:
#if defined(WAIFU_FM_CD32X)
        return g_b_preview_art_pending == 0;
#else
        return 1;
#endif
    case IB_PLAYER_EQUIP_TARGET:
    case IB_PLAYER_FUSION_TARGET:
    case IB_PLAYER_TOP:
        return 1;
    case IB_TALLY:
        return g_b_phase_frame >= 20;
    case IB_REWARD:
        /* Brief lock so the earned card is actually seen before RUN dismisses it. */
        return g_b_phase_frame >= 6;
    default:
        return 0;
    }
}

static void suppress_battle_input_if_locked(int *press_up, int *press_down,
                                            int *press_left, int *press_right,
                                            int *press_a, int *press_b,
                                            int *press_start, int *press_tab)
{
    if (g_i_state != WAIFU_I_BATTLE) return;
    if (battle_phase_accepts_player_input()) return;
    *press_up = 0;
    *press_down = 0;
    *press_left = 0;
    *press_right = 0;
    *press_a = 0;
    *press_b = 0;
    *press_start = 0;
    *press_tab = 0;
}

static int start_press_plays_ui_sound(void)
{
    if (g_i_state == WAIFU_I_DECK_EDITOR || g_i_state == WAIFU_I_DECK_PREVIEW) return 0;
    if (g_i_state == WAIFU_I_BATTLE &&
        (g_b_phase == IB_RESULT || g_b_phase == IB_TALLY)) return 0;
    return 1;
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

static void ensure_battle_deck_rng_seeded(void)
{
#ifdef WAIFU_FM_HEADLESS_TESTS
    if (!g_i_deck_rng_seeded) {
        waifu_deck_rng_seed(&g_i_deck_rng, 17u);
        g_i_deck_rng_seeded = 1;
    }
#else
    if (!g_i_deck_rng_seeded) {
        waifu_deck_rng_seed(&g_i_deck_rng, waifu_deck_runtime_seed(0x5743464du));
        g_i_deck_rng_seeded = 1;
    }
#endif
}

static void reseed_battle_deck_rng_for_game(uint32_t salt)
{
#ifdef WAIFU_FM_HEADLESS_TESTS
    ensure_battle_deck_rng_seeded();
#else
    waifu_deck_rng_seed(&g_i_deck_rng, waifu_deck_runtime_seed(salt));
    g_i_deck_rng_seeded = 1;
#endif
}

static void sync_battle_deck_counts(void)
{
    g_i_player_deck_left = waifu_deck_remaining(&g_i_player_deck);
    g_i_com_deck_left = waifu_deck_remaining(&g_i_com_deck);
    if (g_story_battle_active) g_story_player_deck_pos = g_i_player_deck.pos;
}

#ifndef WAIFU_FM_HEADLESS_TESTS
static void cut_deck_by(WaifuDeck *deck, int cut)
{
    int tmp[WAIFU_DECK_SIZE];
    int i;
    if (!deck || deck->count <= 1) return;
    cut %= deck->count;
    if (cut < 0) cut += deck->count;
    if (cut == 0) return;
    for (i = 0; i < deck->count; ++i) tmp[i] = deck->cards[(i + cut) % deck->count];
    for (i = 0; i < deck->count; ++i) deck->cards[i] = tmp[i];
    deck->pos = 0;
}

static void randomize_battle_opening_decks(uint32_t salt)
{
    for (int pass = 0; pass < 4; ++pass) {
        WaifuDeckRng local_rng;
        uint32_t seed = waifu_deck_runtime_seed(salt ^
                                                ((uint32_t)g_i_frame << 1) ^
                                                ((uint32_t)g_story_scene_anim_frame << 9) ^
                                                ((uint32_t)pass * 0x9e3779b9u) ^
                                                waifu_deck_rng_next(&g_i_deck_rng));
        waifu_deck_rng_seed(&local_rng, seed);
        waifu_deck_shuffle(&g_i_player_deck, &local_rng);
        cut_deck_by(&g_i_player_deck, (int)(waifu_deck_rng_next(&local_rng) % WAIFU_DECK_SIZE));
        waifu_deck_shuffle(&g_i_com_deck, &local_rng);
        cut_deck_by(&g_i_com_deck, (int)(waifu_deck_rng_next(&local_rng) % WAIFU_DECK_SIZE));
    }
}
#endif


static void recalc_story_deck_counts(void);

static int story_prefix_card_count(int count, int card)
{
    int n = 0;
    if (count > STORY_DECK_SIZE) count = STORY_DECK_SIZE;
    for (int i = 0; i < count; ++i) if (g_story_player_deck[i] == card) ++n;
    return n;
}

static int story_prefix_support_count(int count)
{
    int n = 0;
    if (count > STORY_DECK_SIZE) count = STORY_DECK_SIZE;
    for (int i = 0; i < count; ++i) if (is_support_card(g_story_player_deck[i])) ++n;
    return n;
}

static int story_append_limited_card(int *idx, int card)
{
    if (!idx || *idx >= STORY_DECK_SIZE || card < 0) return 0;
    if (story_prefix_card_count(*idx, card) >= 4) return 0;
    g_story_player_deck[(*idx)++] = card;
    return 1;
}

static void story_append_guaranteed_supports(int *idx, int *seed)
{
    int support_pack[STORY_MIN_SUPPORT_CARDS] = {
        SUPPORT_EQUIP_CARD_ID, SUPPORT_EQUIP_CARD_ID, SUPPORT_EQUIP_CARD_ID,
        SUPPORT_GUARD_CARD_ID, SUPPORT_GUARD_CARD_ID,
        SUPPORT_DRAW_CARD_ID, SUPPORT_DRAW_CARD_ID,
        SUPPORT_HEAL_CARD_ID, SUPPORT_HEAL_CARD_ID
    };
    for (int i = STORY_MIN_SUPPORT_CARDS - 1; i > 0; --i) {
        int j = story_prng_next(seed) % (i + 1);
        int tmp = support_pack[i];
        support_pack[i] = support_pack[j];
        support_pack[j] = tmp;
    }
    for (int i = 0; i < STORY_MIN_SUPPORT_CARDS; ++i) {
        (void)story_append_limited_card(idx, support_pack[i]);
    }
}

static void story_repair_generated_support_floor(int *seed)
{
    int support_total = story_prefix_support_count(g_story_deck_count);
    int equip_total = story_prefix_card_count(g_story_deck_count, SUPPORT_EQUIP_CARD_ID);
    int safety = 0;
    while ((support_total < STORY_MIN_SUPPORT_CARDS || equip_total < STORY_MIN_EQUIP_CARDS) && safety++ < STORY_DECK_SIZE * 4) {
        int replace = 5 + (story_prng_next(seed) % (STORY_DECK_SIZE - 5));
        int new_card;
        if (is_support_card(g_story_player_deck[replace])) continue;
        new_card = (equip_total < STORY_MIN_EQUIP_CARDS) ? SUPPORT_EQUIP_CARD_ID
                 : (WAIFU_CARD_COUNT + (story_prng_next(seed) % SUPPORT_STANDARD_CARD_VARIANTS));
        if (story_prefix_card_count(g_story_deck_count, new_card) >= 4) {
            for (int kind = 0; kind < SUPPORT_STANDARD_CARD_VARIANTS; ++kind) {
                int candidate = WAIFU_CARD_COUNT + kind;
                if (story_prefix_card_count(g_story_deck_count, candidate) < 4) {
                    new_card = candidate;
                    break;
                }
            }
        }
        if (story_prefix_card_count(g_story_deck_count, new_card) >= 4) break;
        g_story_player_deck[replace] = new_card;
        support_total = story_prefix_support_count(g_story_deck_count);
        equip_total = story_prefix_card_count(g_story_deck_count, SUPPORT_EQUIP_CARD_ID);
    }
}

/* Starting playthrough decks must not contain any monster stronger than this. */
#define STORY_STARTER_MAX_MONSTER_ATK 2000

/* Pick a random monster whose ATK is within [min_atk, STORY_STARTER_MAX_MONSTER_ATK].
   Used so the generated starter deck never rolls a high-ATK boss monster. */
static int story_pick_capped_monster(int *seed, int min_atk)
{
    int fallback = -1;
    for (int tries = 0; tries < 64; ++tries) {
        int card = (int)(story_prng_next(seed) % WAIFU_CARD_COUNT);
        int a = (int)waifu_card_atk[card];
        if (a > STORY_STARTER_MAX_MONSTER_ATK) continue;
        if (a >= min_atk) return card;
        if (fallback < 0) fallback = card;
    }
    if (fallback >= 0) return fallback;
    for (int card = 0; card < WAIFU_CARD_COUNT; ++card) {
        if ((int)waifu_card_atk[card] <= STORY_STARTER_MAX_MONSTER_ATK) return card;
    }
    return 0;
}

static void generate_story_starter_deck(void)
{
#ifdef WAIFU_FM_HEADLESS_TESTS
    int seed = story_hash_name();
#else
    int seed = (int)(waifu_deck_runtime_seed((uint32_t)story_hash_name()) & 0x7fffffffu);
    if (seed == 0) seed = 17;
#endif
    /* Keep the opener a capable monster, but capped so it never exceeds 2000 ATK. */
    int strong = story_pick_capped_monster(&seed, 1700);
    int weak = waifu_story_starter_weak_pool[story_prng_next(&seed) % WAIFU_STORY_STARTER_WEAK_POOL_COUNT];
    int idx = 0;

    g_story_strong_card = strong;
    g_story_weak_card = weak;
    g_story_equip_count = 0;
    g_story_support_count = 0;
    g_story_deck_count = STORY_DECK_SIZE;

    for (int i = 0; i < STORY_DECK_SIZE; ++i) g_story_player_deck[i] = CARD_NONE;

    /* Guaranteed opening profile, while the support pack ensures story-mode
       generated decks cannot roll with too few spells. The pack is shuffled so
       the exact support mix near the top of the deck still varies. */
    (void)story_append_limited_card(&idx, strong);
    (void)story_append_limited_card(&idx, weak);
    (void)story_append_limited_card(&idx, weak);
    story_append_guaranteed_supports(&idx, &seed);
    /* Always seed at least one purple Trap, occasionally more: 60% one, 35% two,
       5% three. The player therefore always starts with the auto-counter card. */
    {
        int trap_roll = story_prng_next(&seed) % 100;
        int trap_count = (trap_roll < 60) ? 1 : (trap_roll < 95) ? 2 : 3;
        for (int t = 0; t < trap_count; ++t) {
            (void)story_append_limited_card(&idx, SUPPORT_TRAP_CARD_ID);
        }
    }

    while (idx < STORY_DECK_SIZE) {
        int roll = story_prng_next(&seed) % 100;
        int card;
        if (roll < 34) {
            card = weak;
        } else if (roll < 48) {
            card = waifu_story_starter_weak_pool[story_prng_next(&seed) % WAIFU_STORY_STARTER_WEAK_POOL_COUNT];
        } else if (roll < 62) {
            card = SUPPORT_EQUIP_CARD_ID;
        } else if (roll < 82) {
            card = WAIFU_CARD_COUNT + (story_prng_next(&seed) % SUPPORT_STANDARD_CARD_VARIANTS);
        } else {
            card = story_pick_capped_monster(&seed, 0);
        }
        (void)story_append_limited_card(&idx, card);
    }
    story_repair_generated_support_floor(&seed);
    story_shuffle_tail(5, &seed);
    g_story_player_deck_pos = 0;

    recalc_story_deck_counts();
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
    g_deck_preview_art_pending = 0;
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
    int cur_row, scroll_row, max_scroll_row;
    if (count <= 0) {
        g_deck_cursor = 0;
        *scroll = 0;
        return;
    }
    if (g_deck_cursor < 0) g_deck_cursor = 0;
    if (g_deck_cursor >= count) g_deck_cursor = count - 1;
    /* Scroll in whole-row steps so the grid columns stay aligned: a per-card
       scroll offset would shift every card sideways and make the cursor appear
       in a different column than the one it logically occupies. */
    cur_row = g_deck_cursor / DECK_GRID_COLS;
    scroll_row = *scroll / DECK_GRID_COLS;
    if (cur_row < scroll_row) scroll_row = cur_row;
    if (cur_row >= scroll_row + DECK_GRID_ROWS) scroll_row = cur_row - DECK_GRID_ROWS + 1;
    if (scroll_row < 0) scroll_row = 0;
    max_scroll_row = (count - 1) / DECK_GRID_COLS - DECK_GRID_ROWS + 1;
    if (max_scroll_row < 0) max_scroll_row = 0;
    if (scroll_row > max_scroll_row) scroll_row = max_scroll_row;
    *scroll = scroll_row * DECK_GRID_COLS;
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
    if (dy < 0) {
        if (g_deck_cursor < DECK_GRID_COLS) { deck_editor_switch_tab(); return; }
        g_deck_cursor -= DECK_GRID_COLS;
    } else if (dy > 0) {
        int target = g_deck_cursor + DECK_GRID_COLS;
        if (target < count) {
            g_deck_cursor = target;
        } else if (g_deck_cursor < count - 1) {
            /* The last row is only partially filled and the slot directly below
               the cursor is empty: drop onto the last existing card (which sits
               further left) instead of wrapping away to the top-right. */
            g_deck_cursor = count - 1;
        }
        /* else: already on the last card, stay put. */
    }
    if (dx != 0) {
        g_deck_cursor += dx;
        if (g_deck_cursor < 0) g_deck_cursor = count - 1;
        if (g_deck_cursor >= count) g_deck_cursor = 0;
    }
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

/* Cards that must never come out of a generic reward roll; each has a
 * single scripted source (see deck.c deck_card_is_restricted). */
static int reward_card_is_restricted(int card)
{
    return card == WAIFU_CARD_ID_MECHA_ULTIMATE_DRAGON ||
           card == WAIFU_CARD_ID_GARGOYLE_GIRL;
}

/* Battle grade of the duel that just ended, used to scale the reward odds. */
static char current_battle_rank(void)
{
    int won = g_b_result >= 0;
    int score = 0;
    return battle_rank_from_stats(won, won ? g_you_lp : 0, g_b_cards_used, g_b_turns, &score);
}

/* Roll a story win reward whose quality scales with the battle rank.  strong_pm
 * is the per-mille chance of a rare/strong card, equip_pm the chance of an equip
 * support; the remainder is a common monster.  The 1% strong base (B rank)
 * doubles to 2% at S and is 1.5% at A; C is halved and D yields no rare drop. */
static int story_reward_drop_card_ranked(char rank)
{
    int strong_pm, equip_pm;
    uint32_t roll;
    int tries;

    switch (rank) {
    case 'S': strong_pm = 20; equip_pm = 150; break;
    case 'A': strong_pm = 15; equip_pm = 120; break;
    case 'B': strong_pm = 10; equip_pm = 100; break;
    case 'C': strong_pm =  5; equip_pm =  60; break;
    default:  strong_pm =  0; equip_pm =  30; break; /* D and below: no rare */
    }

#ifdef WAIFU_FM_HEADLESS_TESTS
    int seed = story_hash_name() ^ (g_story_duel_index * 131 + g_b_turns * 17 + g_b_cards_used * 29 + 0x2468);
    #define REWARD_RNG() ((uint32_t)story_prng_next(&seed))
#else
    ensure_battle_deck_rng_seeded();
    #define REWARD_RNG() (waifu_deck_rng_next(&g_i_deck_rng))
#endif

    /* Gargoyle Girl: still only obtainable after the second-to-last opponent,
     * now at the same rank-scaled rare odds (so a D-rank win never yields her). */
    if (g_story_duel_index == STORY_MAX_DUELS - 2 && strong_pm > 0 &&
        (REWARD_RNG() % 1000u) < (uint32_t)strong_pm) {
        return WAIFU_CARD_ID_GARGOYLE_GIRL;
    }

    roll = REWARD_RNG() % 1000u;
    if ((int)roll < strong_pm) {
        return waifu_story_reward_strong_pool[REWARD_RNG() % (uint32_t)WAIFU_STORY_REWARD_STRONG_POOL_COUNT];
    }
    if ((int)roll < strong_pm + equip_pm) {
        return SUPPORT_EQUIP_CARD_ID;
    }
    for (tries = 0; tries < 64; ++tries) {
        int card = (int)(REWARD_RNG() % (uint32_t)WAIFU_CARD_COUNT);
        if (!reward_card_is_restricted(card)) return card;
    }
    #undef REWARD_RNG
    return WAIFU_CARD_ID_RAT;
}

static void award_story_win_drop(void)
{
    int card = g_b_reward_card;
    /* Use the card already revealed on the victory screen; fall back to a fresh
       rank-scaled roll when awarded outside the interactive flow (e.g. tests). */
    if (card == CARD_NONE) card = story_reward_drop_card_ranked(current_battle_rank());
    g_b_reward_card = CARD_NONE;
    if (g_story_storage_count >= STORY_STORAGE_SIZE) return;
    append_card_to(g_story_storage, &g_story_storage_count, STORY_STORAGE_SIZE, card);
}

#ifdef WAIFU_FM_PCFX

/* -----------------------------------------------------------------------
 * PC-FX BackupRAM / ExBackupRAM save via the BIOS filesystem layer.
 * Folder: WAIFCARD   File: WAIFCARD/SAVE.DAT
 * Internal BackupRAM is mounted at /SRAM, external ExBackupRAM/FX-BMP at
 * /CARD.  Folder/file creation, writes, reads and close/commit all go
 * through the BIOS filesystem dispatcher (see pcfx_biosfs.c); we no longer
 * touch BackupRAM hardware directly or mutate FAT structures app-side.
 * Tries internal BackupRAM first; falls back to external ExBackupRAM.
 * ----------------------------------------------------------------------- */

/* Binary save layout (123 bytes total):
 *   0-3   "WAIF" magic
 *   4     version 0x01
 *   5-10  name[6]  (A-Z)
 *   11    story_progress (furthest unlocked duel / frontier)
 *   12    map_cursor
 *   13    pyramid_cursor
 *   14    plaza_line (u8, clamped)
 *   15    deck_count
 *   16    storage_count
 *   17-56 deck[40]    (u8: 0=CARD_NONE, n=card_id+1)
 *   57-120 storage[64] (u8: same encoding)
 *   121-122 checksum u16 LE (sum of bytes 0..120)
 */
#define WAIFU_SAVE_DIR_INT   PCFX_BIOSFS_PATH_INTERNAL "/WAIFCARD"
#define WAIFU_SAVE_FILE_INT  PCFX_BIOSFS_PATH_INTERNAL "/WAIFCARD/SAVE.DAT"
#define WAIFU_SAVE_DIR_EXT   PCFX_BIOSFS_PATH_EXTERNAL "/WAIFCARD"
#define WAIFU_SAVE_FILE_EXT  PCFX_BIOSFS_PATH_EXTERNAL "/WAIFCARD/SAVE.DAT"
#define WAIFU_SAVE_VERSION   0x01u
#define WAIFU_SAVE_SIZE      123u

/* Working heap handed to the BIOS filesystem dispatcher.  The BIOS operates
 * on the memory-mapped BackupRAM in place, so this only holds its internal
 * scratch (open-file table, path buffers).  Kept to the size of the former
 * 32 KB volume staging buffer so RAM use is unchanged. */
static u8 g_biosfs_heap[0x8000];
static int g_biosfs_ready = 0;

/* Per-device cache for story_save_exists(): index 0 = internal BackupRAM,
 * 1 = external ExBackupRAM/FX-BMP.  -1=unchecked, 0=no, 1=yes.  Probing a
 * device walks the BIOS filesystem, so we cache it rather than do it every
 * frame from the title/menu. */
static int g_save_exists_cached[2] = { -1, -1 };

static const char *waifu_save_dir(int ext)  { return ext ? WAIFU_SAVE_DIR_EXT  : WAIFU_SAVE_DIR_INT;  }
static const char *waifu_save_file(int ext) { return ext ? WAIFU_SAVE_FILE_EXT : WAIFU_SAVE_FILE_INT; }

/* Initialise the BIOS filesystem dispatcher once before first use. */
static int waifu_biosfs_ensure(void)
{
    if (g_biosfs_ready) return 1;
    if (pcfx_biosfs_init(g_biosfs_heap, (u32)sizeof(g_biosfs_heap)) < 0) return 0;
    g_biosfs_ready = 1;
    return 1;
}

static u8 save_encode_card(int id)
{
    return (id < 0) ? 0u : (u8)(id + 1);
}

static int save_decode_card(u8 b)
{
    return (b == 0u) ? CARD_NONE : (int)(b - 1u);
}

static void save_build_blob(u8 *buf)
{
    int i;
    u16 cksum = 0;
    buf[0] = 'WAIFU_FM_WIDTH'; buf[1] = 'A'; buf[2] = 'I'; buf[3] = 'F';
    buf[4] = WAIFU_SAVE_VERSION;
    for (i = 0; i < STORY_NAME_LEN; ++i) buf[5 + i] = (u8)g_story_name[i];
    buf[11] = (u8)g_story_progress;
    buf[12] = (u8)g_story_map_cursor;
    buf[13] = (u8)g_story_pyramid_cursor;
    buf[14] = (u8)(g_story_plaza_line & 0xFF);
    buf[15] = (u8)g_story_deck_count;
    buf[16] = (u8)g_story_storage_count;
    for (i = 0; i < STORY_DECK_SIZE; ++i) buf[17 + i] = save_encode_card(g_story_player_deck[i]);
    for (i = 0; i < STORY_STORAGE_SIZE; ++i) buf[57 + i] = save_encode_card(g_story_storage[i]);
    for (i = 0; i < 121; ++i) cksum = (u16)(cksum + buf[i]);
    buf[121] = (u8)(cksum & 0xFFu);
    buf[122] = (u8)(cksum >> 8);
}

static int save_parse_blob(const u8 *buf, u32 len)
{
    int i;
    u16 cksum = 0, stored;
    if (len < WAIFU_SAVE_SIZE) return 0;
    if (buf[0] != 'WAIFU_FM_WIDTH' || buf[1] != 'A' || buf[2] != 'I' || buf[3] != 'F') return 0;
    if (buf[4] != WAIFU_SAVE_VERSION) return 0;
    for (i = 0; i < 121; ++i) cksum = (u16)(cksum + buf[i]);
    stored = (u16)buf[121] | ((u16)buf[122] << 8);
    if (cksum != stored) return 0;

    for (i = 0; i < STORY_NAME_LEN; ++i) {
        char c = (char)buf[5 + i];
        g_story_name[i] = (c >= 'A' && c <= 'Z') ? c : 'A';
    }
    g_story_name[STORY_NAME_LEN] = '\0';
    g_story_progress = (int)buf[11];
    if (g_story_progress < 0) g_story_progress = 0;
    if (g_story_progress >= STORY_MAX_DUELS) g_story_progress = STORY_MAX_DUELS - 1;
    /* Default the map selection to the frontier; the player can step back to an
       earlier opponent from the sanctum. */
    g_story_duel_index = g_story_progress;
    g_story_map_cursor = buf[12] ? 1 : 0;
    g_story_pyramid_cursor = (int)buf[13];
    if (g_story_pyramid_cursor < 0 || g_story_pyramid_cursor > 3) g_story_pyramid_cursor = 0;
    g_story_plaza_line = (int)buf[14];
    if (g_story_plaza_line < 0) g_story_plaza_line = 0;
    g_story_deck_count = (int)buf[15];
    if (g_story_deck_count < 0 || g_story_deck_count > STORY_DECK_SIZE) g_story_deck_count = 0;
    g_story_storage_count = (int)buf[16];
    if (g_story_storage_count < 0 || g_story_storage_count > STORY_STORAGE_SIZE) g_story_storage_count = 0;
    for (i = 0; i < STORY_DECK_SIZE; ++i) g_story_player_deck[i] = save_decode_card(buf[17 + i]);
    for (i = 0; i < STORY_STORAGE_SIZE; ++i) g_story_storage[i] = save_decode_card(buf[57 + i]);
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

static int bkup_try_save_vol(int ext)
{
    u8 blob[WAIFU_SAVE_SIZE];
    int rc;

    if (!waifu_biosfs_ensure()) return 0;
    /* Create the save folder; tolerate it already existing. */
    rc = pcfx_biosfs_mkdir(waifu_save_dir(ext));
    if (rc < 0 && rc != PCFX_BIOSFS_ERR_ALREADY_EXISTS) return 0;
    save_build_blob(blob);
    /* save_file opens with CREATE|TRUNCATE|WRITE and commits on close. */
    if (pcfx_biosfs_save_file(waifu_save_file(ext), blob, WAIFU_SAVE_SIZE) < 0) return 0;
    return 1;
}

static int bkup_try_exists_vol(int ext)
{
    int fd;

    if (!waifu_biosfs_ensure()) return 0;
    fd = pcfx_biosfs_open(waifu_save_file(ext), PCFX_BIOSFS_OPEN_READ);
    if (fd < 0) return 0;
    pcfx_biosfs_close(fd);
    return 1;
}

static int bkup_try_load_vol(int ext)
{
    u8 blob[WAIFU_SAVE_SIZE + 8u];
    u32 loaded_len = 0;

    if (!waifu_biosfs_ensure()) return 0;
    if (pcfx_biosfs_load_file(waifu_save_file(ext), blob, (u32)sizeof(blob), &loaded_len) < 0) return 0;
    return save_parse_blob(blob, loaded_len);
}

/* Per-device existence check (ext: 0=internal, 1=external/FX-BMP), cached. */
static int story_save_exists_device(int ext)
{
    int idx = ext ? 1 : 0;
    if (g_save_exists_cached[idx] >= 0) return g_save_exists_cached[idx];
    g_save_exists_cached[idx] = bkup_try_exists_vol(ext) ? 1 : 0;
    return g_save_exists_cached[idx];
}

static int story_save_exists(void)
{
    return (story_save_exists_device(0) || story_save_exists_device(1)) ? 1 : 0;
}

/* Per-device load (ext: 0=internal, 1=external/FX-BMP). */
static int read_story_save_device(int ext)
{
    return bkup_try_load_vol(ext);
}

/* Per-device save (ext: 0=internal, 1=external/FX-BMP). */
static int write_story_save_device(int ext)
{
    int idx = ext ? 1 : 0;
    if (bkup_try_save_vol(ext)) { g_save_exists_cached[idx] = 1; return 1; }
    return 0;
}

static int write_story_save(void)
{
    g_story_save_status = -1;
    if (write_story_save_device(0)) return 1;
    if (write_story_save_device(1)) return 1;
    return 0;
}

static int read_story_save(void)
{
    if (read_story_save_device(0)) return 1;
    return read_story_save_device(1);
}

#else

static int story_save_exists(void)
{
    return waifu_platform_storage_exists(STORY_SAVE_PATH);
}

/* Whitespace tokenizer over an in-memory save blob. Keeps the host save format
 * identical to the original fscanf-based reader while depending only on the
 * platform storage seam (no stdio in common game code). */
typedef struct StorySaveScan {
    const char *p;
    const char *end;
} StorySaveScan;

static int story_save_next_token(StorySaveScan *s, char *out, int out_size)
{
    int n = 0;
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) s->p++;
    if (s->p >= s->end) { if (out_size > 0) out[0] = '\0'; return 0; }
    while (s->p < s->end && !(*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) {
        if (n < out_size - 1) out[n++] = *s->p;
        s->p++;
    }
    if (out_size > 0) out[n] = '\0';
    return n > 0;
}

static int story_save_expect(StorySaveScan *s, const char *expect)
{
    char tok[64];
    if (!story_save_next_token(s, tok, (int)sizeof(tok))) return 0;
    return strcmp(tok, expect) == 0;
}

static int story_save_next_int(StorySaveScan *s, int *out)
{
    char tok[64];
    int i = 0, sign = 1, val = 0;
    if (!story_save_next_token(s, tok, (int)sizeof(tok))) return 0;
    if (tok[0] == '-') { sign = -1; i = 1; }
    if (tok[i] == '\0') return 0;
    for (; tok[i]; ++i) {
        if (tok[i] < '0' || tok[i] > '9') return 0;
        val = val * 10 + (tok[i] - '0');
    }
    *out = val * sign;
    return 1;
}

static int write_story_save(void)
{
    char buf[4096];
    int len, i;

    buf[0] = '\0';
    waifu_str_cat(buf, (int)sizeof(buf), "WAIFU_STORY_SAVE_V1\n");
    waifu_str_cat(buf, (int)sizeof(buf), "name ");
    waifu_str_cat(buf, (int)sizeof(buf), g_story_name);
    waifu_str_cat(buf, (int)sizeof(buf), "\nduel ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_progress);
    waifu_str_cat(buf, (int)sizeof(buf), "\nmap ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_map_cursor);
    waifu_str_cat(buf, (int)sizeof(buf), " pyramid ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_pyramid_cursor);
    waifu_str_cat(buf, (int)sizeof(buf), " plaza ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_plaza_line);
    waifu_str_cat(buf, (int)sizeof(buf), "\ndeck_count ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_deck_count);
    waifu_str_cat(buf, (int)sizeof(buf), " storage_count ");
    waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_storage_count);
    waifu_str_cat(buf, (int)sizeof(buf), "\ndeck");
    for (i = 0; i < g_story_deck_count; ++i) {
        waifu_str_cat_char(buf, (int)sizeof(buf), ' ');
        waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_player_deck[i]);
    }
    waifu_str_cat(buf, (int)sizeof(buf), "\nstorage");
    for (i = 0; i < g_story_storage_count; ++i) {
        waifu_str_cat_char(buf, (int)sizeof(buf), ' ');
        waifu_str_cat_i32(buf, (int)sizeof(buf), g_story_storage[i]);
    }
    waifu_str_cat(buf, (int)sizeof(buf), "\n");

    len = (int)strlen(buf);
    return waifu_platform_storage_write(STORY_SAVE_PATH, buf, len) == len;
}

static int read_story_save(void)
{
    char buf[4096];
    StorySaveScan sc;
    char saved_name[64];
    int duel = 0, map_cursor = 0, pyramid_cursor = 0, plaza_line = 0;
    int deck_count = 0, storage_count = 0;
    int n, i;

    n = waifu_platform_storage_read(STORY_SAVE_PATH, buf, (int)sizeof(buf) - 1);
    if (n < 0) return 0;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    sc.p = buf;
    sc.end = buf + n;

    if (!story_save_expect(&sc, "WAIFU_STORY_SAVE_V1")) return 0;
    if (!story_save_expect(&sc, "name") || !story_save_next_token(&sc, saved_name, (int)sizeof(saved_name))) return 0;
    if (!story_save_expect(&sc, "duel") || !story_save_next_int(&sc, &duel)) return 0;
    if (!story_save_expect(&sc, "map") || !story_save_next_int(&sc, &map_cursor)) return 0;
    if (!story_save_expect(&sc, "pyramid") || !story_save_next_int(&sc, &pyramid_cursor)) return 0;
    if (!story_save_expect(&sc, "plaza") || !story_save_next_int(&sc, &plaza_line)) return 0;
    if (!story_save_expect(&sc, "deck_count") || !story_save_next_int(&sc, &deck_count)) return 0;
    if (!story_save_expect(&sc, "storage_count") || !story_save_next_int(&sc, &storage_count)) return 0;
    if (deck_count < 0 || deck_count > STORY_DECK_SIZE || storage_count < 0 || storage_count > STORY_STORAGE_SIZE) return 0;

    if (!story_save_expect(&sc, "deck")) return 0;
    for (i = 0; i < deck_count; ++i) { if (!story_save_next_int(&sc, &g_story_player_deck[i])) return 0; }
    if (!story_save_expect(&sc, "storage")) return 0;
    for (i = 0; i < storage_count; ++i) { if (!story_save_next_int(&sc, &g_story_storage[i])) return 0; }

    memset(g_story_name, 0, sizeof(g_story_name));
    strncpy(g_story_name, saved_name, STORY_NAME_LEN);
    for (int i = 0; i < STORY_NAME_LEN; ++i) {
        if (g_story_name[i] < 'A' || g_story_name[i] > 'Z') g_story_name[i] = 'A';
    }
    g_story_name[STORY_NAME_LEN] = '\0';
    g_story_progress = duel;
    if (g_story_progress < 0) g_story_progress = 0;
    if (g_story_progress >= STORY_MAX_DUELS) g_story_progress = STORY_MAX_DUELS - 1;
    g_story_duel_index = g_story_progress;
    g_story_map_cursor = map_cursor ? 1 : 0;
    g_story_pyramid_cursor = pyramid_cursor;
    if (g_story_pyramid_cursor < 0 || g_story_pyramid_cursor > 3) g_story_pyramid_cursor = 0;
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
#endif

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

#ifdef WAIFU_FM_PCFX
/* PC-FX: load from one specific backup device (ext: 0=internal, 1=FX-BMP). */
static int load_story_device_to_map(int ext)
{
    if (!read_story_save_device(ext)) {
        g_story_save_status = -1;
        return 0;
    }
    g_story_save_status = 1;
    g_i_state = WAIFU_I_STORY_MAP;
    g_i_frame = -1;
    return 1;
}
#endif

/* "Begin loading a story save" action.  The platform decides how: on PC-FX
   internal Backup RAM is the normal source with FX-BMP as fallback; on other
   ports there is a single save file, so it loads directly. */
static void begin_story_load(void)
{
#ifdef WAIFU_FM_PCFX
    int internal_has = story_save_exists_device(0);
    int external_has = story_save_exists_device(1);
    if (internal_has && !external_has) {
        g_i_load_pending_device = 0;
        waifu_pcfx_video_overlay_clear();
        g_i_state = WAIFU_I_STORY_LOAD_TO_MAP;
        g_i_frame = -1;
    } else if (external_has && !internal_has) {
        g_i_load_pending_device = 1;
        waifu_pcfx_video_overlay_clear();
        g_i_state = WAIFU_I_STORY_LOAD_TO_MAP;
        g_i_frame = -1;
    } else {
        g_i_load_device_sel = 0;
        g_i_state = WAIFU_I_STORY_LOAD_DEVICE;
        g_i_frame = -1;
    }
#else
    if (!load_story_to_map()) g_deck_flash = 60;
#endif
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

static const char *story_opponent_name(void)
{
    return story_opponent_info()->name;
}

static int story_opponent_is_boss(void)
{
    return story_opponent_info()->boss;
}

static void request_story_duel_assets(void)
{
    waifu_assets_request_story_duel(story_opponent_info()->portrait_id);
}

static int story_duel_portraits_ready(void)
{
    return waifu_assets_story_portrait_ready(STORY_PORTRAIT_SERENA) &&
           waifu_assets_story_portrait_ready(story_opponent_info()->portrait_id);
}

static void enter_state_after_assets(WaifuInteractiveState target)
{
    g_i_loading_target = target;
    if (waifu_assets_needs_loading_screen()) {
        g_i_state = WAIFU_I_LOADING_ASSETS;
    } else {
        g_i_state = target;
    }
    g_i_frame = -1;
}

static void enter_title_after_assets(void)
{
    waifu_assets_request_title();
    enter_state_after_assets(WAIFU_I_TITLE);
}

static void enter_menu_after_assets(void)
{
    /* Start on STORY MODE so the standard verification path RUN, DOWN, A
       deterministically enters BATTLE MODE. */
    g_i_menu_selected = 0;
    waifu_assets_request_title();
    enter_state_after_assets(WAIFU_I_MENU);
}

static void enter_title_to_menu_fade(void)
{
    /* RUN/A on the title should reveal the title-backed menu immediately.
       Fade-to-black is reserved for actually leaving the title/menu scene
       into story or battle content. */
    enter_menu_after_assets();
}

static void enter_menu_to_story_fade(void)
{
    g_i_state = WAIFU_I_MENU_TO_STORY;
    g_i_frame = -1;
}

static void enter_menu_to_battle_fade(void)
{
    g_i_state = WAIFU_I_MENU_TO_BATTLE;
    g_i_frame = -1;
}

static void enter_menu_to_load_fade(void)
{
    g_i_state = WAIFU_I_MENU_TO_LOAD;
    g_i_frame = -1;
}

#ifdef WAIFU_FM_PCFX
static void enter_load_device_to_map_fade(int device)
{
    g_i_load_pending_device = device;
    g_i_state = WAIFU_I_STORY_LOAD_DEVICE_TO_MAP;
    g_i_frame = -1;
}
#endif

static void enter_story_intro_after_assets(void)
{
    waifu_assets_request_story_intro();
    enter_state_after_assets(WAIFU_I_STORY_INTRO);
}

static void enter_story_plaza_after_assets(void)
{
    request_story_duel_assets();
    enter_state_after_assets(WAIFU_I_STORY_PLAZA);
}

static void enter_story_ending_after_assets(void)
{
    waifu_assets_request_ending();
    enter_state_after_assets(WAIFU_I_STORY_ENDING);
}

static void enter_deck_editor_after_assets(void)
{
    waifu_assets_request_deck_editor_cards(g_story_player_deck, g_story_deck_count);
    enter_state_after_assets(WAIFU_I_DECK_EDITOR);
}

#ifdef CD32X_DEBUG_AUTOBATTLE
static void enter_debug_deck_editor_after_assets(void)
{
    generate_story_starter_deck();
    generate_story_storage_pool();
    reset_story_deck_editor();
    g_story_editor_from_pyramid = 0;
    enter_deck_editor_after_assets();
}
#endif

static void add_battle_prewarm_id(int *ids, int *count, int cap, int card_id)
{
    if (!ids || !count || *count >= cap) return;
    if (card_id < 0) return;
    for (int i = 0; i < *count; ++i) {
        if (ids[i] == card_id) return;
    }
    ids[(*count)++] = card_id;
}

static void request_battle_cards_for_known_decks(void)
{
    int ids[(I_HAND * 2) + (WAIFU_DECK_SIZE * 2)];
    int count = 0;
    int cap = (int)(sizeof(ids) / sizeof(ids[0]));

    /* These decks/hands are already fixed by init_battle_state() or
       init_story_battle_state().  Stage likely full-art reveals during the
       black loading screen instead of accepting a CD/SCSI miss during the flip
       animation.  Current hands are highest priority, then near-future draw
       order from each deck. */
    for (int i = 0; i < I_HAND; ++i) add_battle_prewarm_id(ids, &count, cap, g_i_player_hand[i]);
    for (int i = 0; i < I_HAND; ++i) add_battle_prewarm_id(ids, &count, cap, g_i_com_hand[i]);
    for (int i = g_i_player_deck.pos; i < g_i_player_deck.count; ++i) add_battle_prewarm_id(ids, &count, cap, g_i_player_deck.cards[i]);
    for (int i = g_i_com_deck.pos; i < g_i_com_deck.count; ++i) add_battle_prewarm_id(ids, &count, cap, g_i_com_deck.cards[i]);

    waifu_assets_request_cards_for_list(ids, count);
}

static void enter_battle_after_assets(void)
{
    request_battle_cards_for_known_decks();
    enter_state_after_assets(WAIFU_I_BATTLE);
}

static void init_story_battle_state(void);

static void enter_story_battle_after_assets(void)
{
    init_story_battle_state();
    enter_battle_after_assets();
}

static WaifuMusicTrack story_battle_music_track(void)
{
    if (!g_story_battle_active) return WAIFU_MUSIC_RANDOM_BATTLE;
    if (g_story_duel_index >= STORY_MAX_DUELS - 1) return WAIFU_MUSIC_FINAL_BOSS;
    if (story_opponent_is_boss() || g_story_duel_index == STORY_MAX_DUELS - 2) return WAIFU_MUSIC_BOSS;
    return WAIFU_MUSIC_RANDOM_BATTLE;
}

static WaifuMusicTrack music_track_for_current_state(void)
{
    switch (g_i_state) {
    case WAIFU_I_LOADING_ASSETS:
        return WAIFU_MUSIC_NONE;
    /* The title attract screen and the mode-select menu share the title
       theme.  On every target that reaches this point through CD-DA, the
       title track starts from the first title/menu frame and is interrupted
       only by an explicit transition.  (PC-FX additionally gates the start
       on title-asset readiness from its platform layer; CD32X layers the
       same gate in cd32x_sh2_main.c.) */
    case WAIFU_I_TITLE:
    case WAIFU_I_TITLE_TO_MENU:
    case WAIFU_I_MENU:
    case WAIFU_I_MENU_TO_STORY:
#ifndef WAIFU_FM_CD32X
    case WAIFU_I_MENU_TO_BATTLE:
#endif
    case WAIFU_I_MENU_TO_LOAD:
#ifdef WAIFU_FM_PCFX
    case WAIFU_I_STORY_LOAD_DEVICE:
    case WAIFU_I_STORY_LOAD_DEVICE_TO_MAP:
#endif
        return WAIFU_MUSIC_TITLE;
#ifdef WAIFU_FM_CD32X
    case WAIFU_I_MENU_TO_BATTLE:
        /* CD32X must have title CD-DA fully stopped before battle card assets
           start loading.  Stopping during the short black fade gives the
           supervisor multiple frames to accept/retry the stop command.  This
           is a CD32X-only NONE return; the title/menu cases above still map
           to WAIFU_MUSIC_TITLE so the title track actually starts. */
        return WAIFU_MUSIC_NONE;
#endif
#ifdef WAIFU_FM_PCFX
    case WAIFU_I_STORY_LOAD_TO_MAP:
        return WAIFU_MUSIC_NONE;
#endif
    case WAIFU_I_STORY_NAME:
    case WAIFU_I_STORY_NAME_TO_INTRO:
    case WAIFU_I_STORY_INTRO:
    case WAIFU_I_STORY_FIRE:
    case WAIFU_I_STORY_FIRE_TO_DECK:
    /* Sanctum map and the surrounding story hub (pyramid/plaza/save) share the
       overworld theme so navigation has continuous music. */
    case WAIFU_I_STORY_MAP:
    case WAIFU_I_STORY_PYRAMID:
    case WAIFU_I_STORY_SAVE:
#ifdef WAIFU_FM_PCFX
    case WAIFU_I_STORY_SAVE_DEVICE:
#endif
    case WAIFU_I_STORY_TO_PLAZA:
    case WAIFU_I_STORY_PLAZA:
    case WAIFU_I_STORY_PLAZA_TO_DECK:
    case WAIFU_I_STORY_ENDING:
        return WAIFU_MUSIC_OPENING_DREAM;
    case WAIFU_I_STORY_ENDING_CREDITS:
        return WAIFU_MUSIC_NONE;
    case WAIFU_I_DECK_EDITOR:
    case WAIFU_I_DECK_PREVIEW:
    case WAIFU_I_DECK_EDITOR_TO_PYRAMID:
    case WAIFU_I_DECK_EDITOR_TO_BATTLE:
#ifdef WAIFU_FM_CD32X
        return WAIFU_MUSIC_NONE;
#else
        return WAIFU_MUSIC_DECK_EDITOR;
#endif
    case WAIFU_I_BATTLE:
        if (g_b_phase == IB_TALLY) return (g_b_result < 0) ? WAIFU_MUSIC_LOST : WAIFU_MUSIC_RESULTS;
        if (g_b_phase == IB_REWARD) return WAIFU_MUSIC_RESULTS;
        if (g_b_phase == IB_RESULT) {
            /* Result music intentionally starts only after the frozen-field UI
               clear phase.  set_battle_phase(IB_RESULT) therefore leaves the
               current battle CD-DA running; the result drawer kicks this update
               at WAIFU_RESULT_UI_CLEAR_FRAMES. */
            if (g_b_phase_frame < WAIFU_RESULT_UI_CLEAR_FRAMES) return story_battle_music_track();
            return (g_b_result < 0) ? WAIFU_MUSIC_LOST : WAIFU_MUSIC_RESULTS;
        }
        return story_battle_music_track();
    default:
        return WAIFU_MUSIC_NONE;
    }
}

static void update_music_for_current_state(void)
{
    waifu_sound_set_music(music_track_for_current_state());
}


#ifdef WAIFU_FM_HEADLESS_TESTS
static int next_headless_lcg_draw_id(void)
{
    int id = g_i_headless_draw_seed % WAIFU_CARD_COUNT;
    g_i_headless_draw_seed = (g_i_headless_draw_seed * 13 + 7) % 997;
    return id;
}
#endif

static int next_draw_id(void)
{
#ifdef WAIFU_FM_HEADLESS_TESTS
    if (!g_story_battle_active) {
        if (g_i_player_deck_left > 0) --g_i_player_deck_left;
        return next_headless_lcg_draw_id();
    }
#endif
    int card = waifu_deck_draw(&g_i_player_deck);
    sync_battle_deck_counts();
    if (card >= 0) return card;
#ifdef WAIFU_FM_HEADLESS_TESTS
    return hand_ids[0];
#else
    return (int)(waifu_deck_rng_next(&g_i_deck_rng) % (uint32_t)WAIFU_CARD_COUNT);
#endif
}

static int next_com_draw_id(void)
{
#ifdef WAIFU_FM_HEADLESS_TESTS
    if (!g_story_battle_active) {
        if (g_i_com_deck_left > 0) --g_i_com_deck_left;
        return next_headless_lcg_draw_id();
    }
#endif
    int card = waifu_deck_draw(&g_i_com_deck);
    sync_battle_deck_counts();
    if (card >= 0) return card;
#ifdef WAIFU_FM_HEADLESS_TESTS
    return com_hand_ids[0];
#else
    return (int)(waifu_deck_rng_next(&g_i_deck_rng) % (uint32_t)WAIFU_CARD_COUNT);
#endif
}

static void init_battle_state(void)
{
    int i;
    g_story_battle_active = 0;
    waifu_fm_use_common_palette();
#ifdef WAIFU_FM_HEADLESS_TESTS
    g_i_headless_draw_seed = 17;
#endif
    reseed_battle_deck_rng_for_game(0x4241544cu ^ (uint32_t)g_i_frame ^ ((uint32_t)g_b_turns << 12));
#ifdef WAIFU_FM_HEADLESS_TESTS
    waifu_deck_build_headless_battle(&g_i_player_deck, hand_ids, 17);
    waifu_deck_build_headless_battle(&g_i_com_deck, com_hand_ids, 73);
    for (i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = hand_ids[i];
        g_i_player_used[i] = 0;
        g_i_com_hand[i] = com_hand_ids[i];
        g_i_com_used[i] = 0;
    }
#else
    waifu_deck_build_random(&g_i_player_deck, &g_i_deck_rng, 0);
    waifu_deck_build_random(&g_i_com_deck, &g_i_deck_rng, 1);
    randomize_battle_opening_decks(0x52414e44u);
    for (i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = waifu_deck_draw(&g_i_player_deck);
        g_i_player_used[i] = 0;
        g_i_com_hand[i] = waifu_deck_draw(&g_i_com_deck);
        g_i_com_used[i] = 0;
    }
#endif
    sync_battle_deck_counts();
    for (i = 0; i < I_FIELD; ++i) {
        g_i_player_field[i] = CARD_NONE;
        g_i_player_faceup[i] = 1;
        g_i_player_defense[i] = 0;
        g_i_player_atk_bonus[i] = 0;
        g_i_player_def_bonus[i] = 0;
        g_i_player_equip_field[i] = CARD_NONE;
        g_i_player_equip_target[i] = -1;
        g_i_com_equip_field[i] = CARD_NONE;
        g_i_com_equip_target[i] = -1;
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
    sync_battle_deck_counts();
    g_b_phase = IB_OPENING;
    g_b_frame = 0;
    g_b_phase_frame = 0;
    g_b_selected_hand = 0;
    g_b_selected_player_slot = 0;
    g_b_selected_com_slot = 0;
    g_b_com_return_fade = 0;
    clear_player_fusion_queue();
    for (i = 0; i < FUSION_MAX_MATERIALS; ++i) {
        g_b_fusion_anim_slots[i] = -1;
        g_b_fusion_anim_cards[i] = CARD_NONE;
        g_b_fusion_anim_material_kept[i] = 0;
    }
    g_b_fusion_anim_count = 0;
    g_b_fusion_anim_result = CARD_NONE;
    g_b_fusion_anim_success = 0;
    g_b_fusion_anim_equip_only = 0;
    g_b_fusion_anim_final_card = CARD_NONE;
    g_b_fusion_anim_final_atk_bonus = 0;
    g_b_fusion_anim_final_def_bonus = 0;
    for (i = 0; i < I_FIELD; ++i) g_b_fusion_anim_final_equips[i] = CARD_NONE;
    g_b_fusion_anim_final_equip_count = 0;
    g_b_fusion_anim_final_source_index = -1;
    g_b_fusion_anim_target_slot = -1;
    g_b_fusion_anim_has_field_card = 0;
    g_b_top_col = 0;
    g_b_top_row = PLAYER_CARD_ROW;
    g_b_top_prev_col = 0;
    g_b_top_prev_row = PLAYER_CARD_ROW;
    g_b_top_cursor_anim = 8;
    g_b_attack_attacker_slot = -1;
    g_b_place_hand = -1;
    g_b_place_slot = -1;
    g_b_place_card = -1;
    g_b_place_defense = 0;
    g_b_place_trap = 0;
    g_b_com_equip_pending_hand = -1;
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
    g_b_thunder_hand = -1;
    g_b_thunder_card = CARD_NONE;
    g_b_thunder_owner = 1;
    g_b_thunder_count = 0;
    g_b_trap_counter_active = 0;
    g_b_trap_field_slot = -1;
    g_b_support_hand = -1;
    g_b_support_card = CARD_NONE;
    g_b_support_kind = -1;
    g_b_support_lp_from = 0;
    g_b_support_lp_to = 0;
    for (i = 0; i < I_FIELD; ++i) {
        g_b_thunder_slots[i] = -1;
        g_b_thunder_cards[i] = CARD_NONE;
        g_b_thunder_backs[i] = 0;
    }
    clear_battle_snapshot();
    g_b_battle_atk_owner = 0;
    g_b_battle_outcome = BATTLE_DESTROY_DEFENDER;
    g_b_draw_count = 0;
    g_b_player_hand_intro_pending = 1;
    for (i = 0; i < I_HAND; ++i) g_b_draw_slots[i] = -1;
    strcpy(g_b_damage_text, "0");
    g_b_result = 0;
    g_b_reward_card = CARD_NONE;
    g_b_turns = 1;
    g_b_cards_used = 0;
    g_b_player_monster_played_this_turn = 0;
    g_b_com_monster_played_this_turn = 0;
    g_b_player_fused_this_turn = 0;
    invalidate_battle_composite_cache();
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
    reseed_battle_deck_rng_for_game(0x53544f52u ^ (uint32_t)story_hash_name() ^
                                    ((uint32_t)g_story_duel_index << 16) ^ (uint32_t)g_i_frame);
#ifdef WAIFU_FM_HEADLESS_TESTS
    waifu_deck_build_from_list(&g_i_player_deck, g_story_player_deck, g_story_deck_count, &g_i_deck_rng, 0);
    waifu_deck_build_opponent_story(&g_i_com_deck, g_story_duel_index, &g_i_deck_rng, 0);
#else
    waifu_deck_build_from_list(&g_i_player_deck, g_story_player_deck, g_story_deck_count, &g_i_deck_rng, 1);
    waifu_deck_build_opponent_story(&g_i_com_deck, g_story_duel_index, &g_i_deck_rng, 1);
    randomize_battle_opening_decks(0x53544f52u ^ (uint32_t)story_hash_name() ^
                                   ((uint32_t)g_story_duel_index << 16));
#endif
    for (int i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = next_draw_id();
        g_i_player_used[i] = 0;
    }
    for (int i = 0; i < I_HAND; ++i) {
        g_i_com_hand[i] = next_com_draw_id();
        g_i_com_used[i] = 0;
    }
    sync_battle_deck_counts();
    /* Bosses have higher LP. */
    if (story_opponent_is_boss()) g_com_lp = 9999;
}

static void draw_interactive_field_cards(Camera cam)
{
    int i;
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_com_equip_field[i] >= 0) draw_board_card_ex(cam, i, ENEMY_CARD_ROW - 1, g_i_com_equip_field[i], 0, 0);
    }
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_com_field[i] >= 0) draw_board_card_state(cam, i, ENEMY_CARD_ROW, g_i_com_field[i], !g_i_com_faceup[i], g_i_com_attacked[i], g_i_com_defense[i]);
    }
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_field[i] >= 0) draw_board_card_state(cam, i, PLAYER_CARD_ROW, g_i_player_field[i], !g_i_player_faceup[i], g_i_player_attacked[i], g_i_player_defense[i]);
    }
    for (i = 0; i < I_FIELD; ++i) {
        if (g_i_player_equip_field[i] >= 0)
            draw_board_card_ex(cam, i, PLAYER_CARD_ROW + 1, g_i_player_equip_field[i],
                               is_trap_support_card(g_i_player_equip_field[i]) ? 1 : 0, 0);
    }
}

static void draw_interactive_player_hand(int f, int selected, int yoff, int suppress_cursor)
{
    PROFILE_HAND_BEGIN();
    int i;
    int y = WAIFU_HAND_Y_BASE + yoff;
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        if (f < 48) {
            int32_t t = q8_smooth_ratio(f - i * 5, 18);
            x = lerp_i(282 + WAIFU_UI_EXTRA_W, x0, t);
        }
        if (g_i_player_used[i]) continue;
        PROFILE_HAND_CARD_DRAW(draw_hand_card_sprite_ex(g_i_player_hand[i], x, y, 38, 50, 0,
                                 is_monster_card(g_i_player_hand[i]) && player_hand_monster_blocked()));
        if (!suppress_cursor && i == selected) draw_red_cursor(x, y, 38, 50);
        if (!suppress_cursor) {
            int order = player_fusion_order_for_slot(i);
            if (order) {
                char badge[2];
                badge[0] = (char)('0' + order);
                badge[1] = '\0';
                rect_fill(x - 2, y - 2, 11, 11, IDX_BLACK);
                rect_outline(x - 2, y - 2, 11, 11, IDX_GOLD_HI);
                draw_text_small(x + 2, y + 1, badge, IDX_WHITE, IDX_BLACK);
            }
        }
    }
    PROFILE_HAND_END();
}

static void play_player_hand_intro_draw_sfx(void)
{
    int i;
    if (!g_b_player_hand_intro_pending) return;
    for (i = 0; i < I_HAND; ++i) {
        if (!g_i_player_used[i] && g_b_phase_frame == 1 + i * 5) {
            waifu_sound_play(WAIFU_SOUND_CARD_DRAWN);
        }
    }
}

static void draw_interactive_com_hand(int f, int selected, int yoff)
{
    PROFILE_HAND_BEGIN();
    int i;
    /* SDL/live play uses the same convention as the player view: the active
       duelist's hand is presented along the lower edge. Earlier SDL builds put
       COM's hand at the top of the screen while also using a COM-facing camera,
       which made the opponent turn look upside-down and unlike the headless
       scripted presentation. */
    int y = WAIFU_HAND_Y_BASE + yoff;
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        if (f < WAIFU_HAND_INTRO_FRAMES) {
            int delay = (WAIFU_HAND_INTRO_FRAMES * i) / 12;
            int dur = WAIFU_HAND_INTRO_FRAMES / 3;
            if (dur < 4) dur = 4;
            int32_t t = q8_smooth_ratio(f - delay, dur);
            x = lerp_i(282 + WAIFU_UI_EXTRA_W, x0, t);
        }
        if (g_i_com_used[i]) continue;
        PROFILE_HAND_CARD_DRAW(draw_hand_card_sprite(g_i_com_hand[i], x, y, 38, 50, 1));
        if (i == selected) draw_red_cursor(x, y, 38, 50);
    }
    PROFILE_HAND_END();
}

#if defined(WAIFU_FM_PCFX)
static Camera pcfx_handtop_anchor_camera(int anchor)
{
    if (anchor <= 0) return player_camera();
    if (anchor >= WAIFU_PCFX_HANDTOP_ANCHORS - 1) return battle_top_camera();
    return lerp_camera(player_camera(), battle_top_camera(), q8_ratio(anchor, WAIFU_PCFX_HANDTOP_ANCHORS - 1));
}

static int pcfx_handtop_anchor_for_frame(int frame, int dur)
{
    if (frame <= 0) return 0;
    if (frame >= dur) return WAIFU_PCFX_HANDTOP_ANCHORS - 1;
    /* Quantize a smooth camera lift onto cached real-3D keyframes.  The hand and
       cursor overlays still move every logic frame; only the expensive base
       perspective is stepped through a small cacheable set. */
    return (frame * (WAIFU_PCFX_HANDTOP_ANCHORS - 1) + dur / 2) / dur;
}

static Camera player_handtop_transition_camera(int frame, int dur, int to_top)
{
    int anchor = pcfx_handtop_anchor_for_frame(frame, dur);
    if (!to_top) anchor = WAIFU_PCFX_HANDTOP_ANCHORS - 1 - anchor;
    return pcfx_handtop_anchor_camera(anchor);
}
#else
static Camera player_handtop_transition_camera(int frame, int dur, int to_top)
{
    int32_t t = q8_ratio(frame, dur);
    return to_top ? lerp_camera(player_camera(), battle_top_camera(), t)
                  : lerp_camera(battle_top_camera(), player_camera(), t);
}
#endif

#if !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
#if defined(WAIFU_FM_PCFX)
static int pcfx_handtop_anchor_index_for_camera(Camera cam)
{
    for (int i = 0; i < WAIFU_PCFX_HANDTOP_ANCHORS; ++i) {
        if (camera_equal(cam, pcfx_handtop_anchor_camera(i))) return i;
    }
    return -1;
}

static WaifuBattleBaseCache *pcfx_handtop_cache_for_anchor(int anchor)
{
    if (anchor <= 0) return &g_b_base_cache;
    if (anchor >= WAIFU_PCFX_HANDTOP_ANCHORS - 1) return &g_b_base_cache_top;
    return &g_b_handtop_mid_cache[anchor - 1];
}
#endif

static WaifuBattleBaseCache *battle_base_cache_for_camera(Camera cam)
{
#if defined(WAIFU_FM_PCFX)
    int anchor = pcfx_handtop_anchor_index_for_camera(cam);
    if (anchor >= 0) return pcfx_handtop_cache_for_anchor(anchor);
    if (camera_equal(cam, battle_top_camera())) return &g_b_base_cache_top;
#endif
    (void)cam;
    return &g_b_base_cache;
}

static int battle_base_cache_restore(Camera cam, uint32_t key)
{
    WaifuBattleBaseCache *primary = battle_base_cache_for_camera(cam);
    if (primary->valid && primary->key == key && camera_equal(primary->cam, cam)) {
        copy_u8_fast(framebuffer, primary->pixels, (int)sizeof(primary->pixels));
        return 1;
    }
#if defined(WAIFU_FM_PCFX)
    /* The secondary top-view cache is used to remove the UP transition's first
       board miss.  Also check the normal slot so older paths remain compatible
       if it happens to contain the requested camera. */
    if (primary != &g_b_base_cache && g_b_base_cache.valid &&
        g_b_base_cache.key == key && camera_equal(g_b_base_cache.cam, cam)) {
        copy_u8_fast(framebuffer, g_b_base_cache.pixels, (int)sizeof(g_b_base_cache.pixels));
        return 1;
    }
#endif
    return 0;
}

static void battle_base_cache_store(Camera cam, uint32_t key)
{
    WaifuBattleBaseCache *cache = battle_base_cache_for_camera(cam);
    copy_u8_fast(cache->pixels, framebuffer, (int)sizeof(cache->pixels));
    cache->cam = cam;
    cache->key = key;
    cache->valid = 1;
}
#endif /* !WAIFU_BATTLE_BASE_CACHE_DISABLE */

static void draw_interactive_base(Camera cam)
{
    uint32_t key = battle_base_visual_key();
#if !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
    if (battle_base_cache_restore(cam, key)) return;
#else
    (void)key;
#endif

    render_board_cached(cam);
    draw_interactive_field_cards(cam);
    draw_hud();

#if !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
    battle_base_cache_store(cam, key);
#endif
}

#if defined(WAIFU_FM_PCFX) && !defined(WAIFU_BATTLE_BASE_CACHE_DISABLE)
static void prewarm_interactive_base(Camera cam)
{
    uint32_t key = battle_base_visual_key();
    WaifuBattleBaseCache *cache = battle_base_cache_for_camera(cam);
    if (cache->valid && cache->key == key && camera_equal(cache->cam, cam)) return;
    render_board_cached(cam);
    draw_interactive_field_cards(cam);
    draw_hud();
    battle_base_cache_store(cam, key);
}

static void prewarm_handtop_transition_bases(void)
{
    uint32_t key = battle_base_visual_key();
    /* Prewarm one missing hand->top keyframe per hand-idle frame.  This keeps
       steady hand view responsive after the short warmup and lets pressing UP
       play a smooth cached camera lift instead of a hard cut or repeated board
       re-renders.  Prefer the top endpoint first so the transition can always
       finish on a cache hit, then fill the intermediate perspectives. */
    for (int tries = 0; tries < WAIFU_PCFX_HANDTOP_ANCHORS; ++tries) {
        int seq = g_b_handtop_prewarm_index++ % WAIFU_PCFX_HANDTOP_ANCHORS;
        int anchor = (seq == 0) ? (WAIFU_PCFX_HANDTOP_ANCHORS - 1) : (seq - 1);
        Camera cam = pcfx_handtop_anchor_camera(anchor);
        WaifuBattleBaseCache *cache = pcfx_handtop_cache_for_anchor(anchor);
        if (!(cache->valid && cache->key == key && camera_equal(cache->cam, cam))) {
            prewarm_interactive_base(cam);
            return;
        }
    }
}
#endif

static void draw_interactive_field_base_no_hud(Camera cam)
{
    render_board_cached(cam);
    draw_interactive_field_cards(cam);
}

static int result_focus_card_id(void)
{
    int slot = first_live_player_slot();
    if (slot >= 0) return g_i_player_field[slot];
    if (g_b_selected_hand >= 0 && g_b_selected_hand < I_HAND) return g_i_player_hand[g_b_selected_hand];
    return hand_ids[0];
}

static void draw_player_handtop_transition_shared(int frame, int dur, int to_top,
                                                  int hand_yoff, int draw_hand,
                                                  int draw_hud)
{
    Camera cam = player_handtop_transition_camera(frame, dur, to_top);
    if (draw_hud) draw_interactive_base(cam);
    else draw_interactive_field_base_no_hud(cam);
    if (draw_hand) draw_interactive_player_hand(999, g_b_selected_hand, hand_yoff, 1);
}

static void draw_interactive_common(Camera cam, int bottom_card, const char *bottom_mode)
{
    draw_interactive_base(cam);
    if (bottom_card >= 0 && (!bottom_mode || strcmp(bottom_mode, "COM") != 0)) {
        draw_bottom_info(bottom_card, bottom_mode ? bottom_mode : "CARD");
    }
}

static void draw_preview_stat_line(int x, int y, const char *label, unsigned value)
{
    char line[32];
    fmt_label_u32(line, (int)sizeof(line), label, value);
    draw_text_small(x, y, line, IDX_GOLD_HI, IDX_BLACK);
}

static void render_interactive_card_preview_static(int card_id)
{
    int art_x = WAIFU_UI_CENTER_DX + 8;
    int tx = art_x + 128;
    int maxw = WAIFU_FM_WIDTH - tx - 10;
    int y = 17;
    int lines;
    char line[128];
    clear_screen(IDX_BLACK);

    if (maxw < 96) maxw = 96;
    draw_panel_rect(2, 10, WAIFU_FM_WIDTH - 4, WAIFU_FM_HEIGHT - 22, IDX_UI_DARK);

    if (is_support_card(card_id)) {
        tx = art_x + 132;
        maxw = WAIFU_FM_WIDTH - tx - 10;
        if (maxw < 96) maxw = 96;
        {
            int trap = is_trap_support_card(card_id);
            rect_fill(art_x - 1, 36, 128, 160, IDX_BLACK);
            draw_support_big_art_scaled(art_x - 1, 40, 128, 128);
            rect_outline(art_x - 3, 34, 132, 164, trap ? IDX_TRAP_FRAME_HI : IDX_BLUE_WHITE);
            rect_outline(art_x - 2, 35, 130, 162, trap ? IDX_TRAP_FRAME : IDX_UI_BLUE);
        }
        draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
        lines = draw_wrapped_text_small_box(tx, y, maxw, 3, 10, support_card_name(card_id), IDX_WHITE, IDX_BLACK);
        y += lines * 10 + 7;
        draw_text_small(tx, y, "TYPE", IDX_GOLD_HI, IDX_BLACK); y += 11;
        lines = draw_wrapped_text_small_box(tx, y, maxw, 2, 10, support_card_type(card_id), IDX_WHITE, IDX_BLACK);
        y += lines * 10 + 7;
        draw_text_small(tx, y, "EFFECT", IDX_GOLD_HI, IDX_BLACK); y += 11;
        draw_wrapped_text_small_box(tx, y, maxw, 6, 10, support_card_effect(card_id), IDX_WHITE, IDX_BLACK);
        return;
    }

    if (!is_monster_card(card_id)) return;
    draw_big_battle_card(card_id, art_x, WAIFU_BATTLE_CARD_Y, 0);

    draw_text_small(tx, y, "CARD CHECK", IDX_GOLD_HI, IDX_BLACK); y += 14;
    lines = draw_wrapped_text_small_box(tx, y, maxw, 4, 10, waifu_card_names[card_id], IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 6;
    draw_stars_line(tx, y, card_star_count(card_id)); y += 13;

    fmt_join2(line, (int)sizeof(line), waifu_card_attr[card_id], " / ", waifu_card_tribe[card_id]);
    lines = draw_wrapped_text_small_box(tx, y, maxw, 3, 10, line, IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 7;

    draw_text_small(tx, y, "LORE", IDX_GOLD_HI, IDX_BLACK); y += 11;
    lines = draw_wrapped_text_small_box(tx, y, maxw, 5, 10, waifu_card_desc[card_id], IDX_WHITE, IDX_BLACK);
    y += lines * 10 + 7;

    if (y < WAIFU_UI_BOTTOM_Y(194)) y = WAIFU_UI_BOTTOM_Y(194);
    draw_preview_stat_line(tx, y, "ATK", (unsigned)waifu_card_atk[card_id]);
    draw_preview_stat_line(tx, y + 12, "DEF", (unsigned)waifu_card_def[card_id]);
}

static void draw_interactive_card_preview(int card_id, int f)
{
    waifu_fm_use_common_palette();
    /* Render the card-check panel directly each frame on PC-FX.  The previous
       full-frame preview cache cost 60 KiB of RAM and, combined with the expanded
       all-card big-art cache, left too little headroom; long waits on the preview
       could corrupt the cached panel/art.  Big art is already RAM-resident, so
       this path performs no CD reads. */
#if defined(WAIFU_FM_PCFX)
    /* The card-check panel is a static inspect screen, not an animation reveal.
       Do not route its 112x112 art through the presenter's direct-KRAM big-art
       bypass: on memory-tight PC-FX builds that path can leave the two KING
       pages' shadows out of phase if the panel is held for a long time.  Render
       the art into the normal framebuffer and let the dirty presenter upload it
       once from the framebuffer, then only the blinking B prompt changes. */
    ++g_big_art_direct_note_suppressed;
    render_interactive_card_preview_static(card_id);
    --g_big_art_direct_note_suppressed;
#else
    render_interactive_card_preview_static(card_id);
#endif
    (void)f;
}

#if defined(WAIFU_FM_CD32X)
static int cd32x_load_card_check_art(int card_id)
{
    if (is_support_card(card_id)) return waifu_assets_support_big_art() != NULL;
    return waifu_assets_prewarm_big_art_pair(card_id, CARD_NONE);
}

static int cd32x_step_card_check_transition(int card_id, int *pending, int *frame, int draw_deck_source)
{
    int ff;
    if (*pending == CD32X_CARD_CHECK_LOAD_PENDING) {
        ff = *frame < 0 ? 0 : *frame;
        if (ff < CD32X_DECK_CHECK_FADE_FRAMES) {
            if (draw_deck_source) draw_deck_editor();
            apply_black_dither_fade(Q8_ONE - q8_ratio(ff + 1, CD32X_DECK_CHECK_FADE_FRAMES));
        } else {
            int art_loaded;
            draw_transition_black_hold_frame();
            art_loaded = cd32x_load_card_check_art(card_id);
            if (art_loaded) {
                *pending = CD32X_CARD_CHECK_REVEAL;
                *frame = -1;
            }
            apply_black_dither_fade(0);
        }
        return 1;
    }
    if (*pending == CD32X_CARD_CHECK_REVEAL) {
        ff = *frame < 0 ? 0 : *frame;
        draw_interactive_card_preview(card_id, ff);
        if (ff < CD32X_DECK_CHECK_FADE_FRAMES) {
            apply_black_dither_fade(q8_ratio(ff + 1, CD32X_DECK_CHECK_FADE_FRAMES));
        } else {
            *pending = 0;
        }
        return 1;
    }
    return 0;
}
#endif



static int defender_is_passive_position(int defender_owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = defender_owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (!is_monster_card(id)) return 1;
    /* Defense-position monsters are passive. Face-down attack-position
       monsters still reveal and battle with ATK. */
    return field_card_defense_position(defender_owner, slot);
}

static int defender_battle_value(int defender_owner, int slot)
{
    int id;
    if (slot < 0 || slot >= I_FIELD) return 0;
    id = defender_owner == 0 ? g_i_player_field[slot] : g_i_com_field[slot];
    if (id < 0) return 0;
    if (!is_monster_card(id)) return 0;
    if (defender_is_passive_position(defender_owner, slot)) return field_card_def(defender_owner, slot);
    return field_card_atk(defender_owner, slot);
}

typedef struct BattleCalc {
    int attacker_atk;
    int defender_value;
    int delta;
    int damage;
    int damage_owner;
    int defender_passive;
    BattleOutcome outcome;
} BattleCalc;

static BattleCalc calc_battle_state(int attacker_owner, int attacker_slot, int defender_owner, int defender_slot)
{
    BattleCalc bc;
    memset(&bc, 0, sizeof(bc));
    bc.damage_owner = -1;
    bc.attacker_atk = field_card_atk(attacker_owner, attacker_slot);
    bc.defender_value = defender_battle_value(defender_owner, defender_slot);
    bc.defender_passive = defender_is_passive_position(defender_owner, defender_slot);
    bc.delta = bc.attacker_atk - bc.defender_value;

    if (bc.defender_passive) {
        if (bc.delta > 0) {
            bc.outcome = BATTLE_DESTROY_DEFENDER;
        } else if (bc.delta < 0) {
            /* A defense-position or non-monster defender does not counter-attack
               and cannot destroy the attacking monster. The attacker only takes
               battle damage equal to the passive value gap. */
            bc.outcome = BATTLE_NO_DESTROY;
            bc.damage = -bc.delta;
            bc.damage_owner = attacker_owner;
        } else {
            bc.outcome = BATTLE_NO_DESTROY;
        }
    } else {
        if (bc.delta > 0) {
            bc.outcome = BATTLE_DESTROY_DEFENDER;
            bc.damage = bc.delta;
            bc.damage_owner = defender_owner;
        } else if (bc.delta < 0) {
            bc.outcome = BATTLE_DESTROY_ATTACKER;
            bc.damage = -bc.delta;
            bc.damage_owner = attacker_owner;
        } else {
            bc.outcome = BATTLE_DESTROY_BOTH;
        }
    }
    return bc;
}

static int try_trigger_player_trap(int com_attacker_slot);

static void prepare_battle(int attacker_owner, int attacker_slot, int defender_slot)
{
    int atk_id, def_id, defender_owner;
    BattleCalc bc;
    if (attacker_owner == 0 && player_first_turn_attack_locked()) return;
    if (attacker_slot < 0 || attacker_slot >= I_FIELD || defender_slot < 0 || defender_slot >= I_FIELD) return;
    /* COM declared an attack on the player: the player's auto-trap (if held)
       fires first, destroying the attacker and cancelling the attack. */
    if (attacker_owner == 1 && try_trigger_player_trap(attacker_slot)) return;
    atk_id = attacker_owner == 0 ? g_i_player_field[attacker_slot] : g_i_com_field[attacker_slot];
    if (!is_monster_card(atk_id)) return;
    /* Monsters in defense position cannot initiate attacks. */
    if (field_card_defense_position(attacker_owner, attacker_slot)) return;

    defender_owner = attacker_owner ? 0 : 1;
    def_id = defender_owner == 0 ? g_i_player_field[defender_slot] : g_i_com_field[defender_slot];
    if (def_id < 0) return;
    bc = calc_battle_state(attacker_owner, attacker_slot, defender_owner, defender_slot);

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
    } else {
        atk_id = g_i_com_field[attacker_slot];
        def_id = g_i_player_field[defender_slot];
        g_b_battle_atk_back = !g_i_com_faceup[attacker_slot];
        g_b_battle_def_back = !g_i_player_faceup[defender_slot];
        g_b_battle_atk_display_atk = field_card_atk(1, attacker_slot);
        g_b_battle_atk_display_def = field_card_def(1, attacker_slot);
        g_b_battle_def_display_atk = field_card_atk(0, defender_slot);
        g_b_battle_def_display_def = field_card_def(0, defender_slot);
    }
    g_b_battle_atk_card = atk_id;
    g_b_battle_def_card = def_id;
    g_b_battle_damage = bc.damage;
    g_b_battle_damage_owner = bc.damage_owner;
    g_b_battle_outcome = bc.outcome;
    fmt_i32_dec(g_b_damage_text, (int)sizeof(g_b_damage_text), bc.damage);
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_prewarm_big_art_pair(atk_id, def_id);
#else
    /* Pull big art into the small CD/SCSI cache before the battle cut-in starts.
       This keeps the reveal animation event-driven and prevents the first
       face-up frame from stalling on an art cache miss. */
    (void)card_big_art_ptr(atk_id);
    (void)card_big_art_ptr(def_id);
#endif
    set_battle_phase(attacker_owner == 0 ? IB_PLAYER_BATTLE : IB_COM_BATTLE);
}


static void prepare_direct_attack(int attacker_owner, int attacker_slot)
{
    if (attacker_owner == 0 && player_first_turn_attack_locked()) return;
    if (attacker_slot < 0 || attacker_slot >= I_FIELD) return;
    if (field_card_defense_position(attacker_owner, attacker_slot)) return;
    if (!is_monster_card(attacker_owner == 0 ? g_i_player_field[attacker_slot] : g_i_com_field[attacker_slot])) return;
    /* A COM direct attack on the player's life points is cancelled by the
       auto-trap as well, destroying the attacking monster. */
    if (attacker_owner == 1 && try_trigger_player_trap(attacker_slot)) return;

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
    waifu_str_copy(g_b_damage_text, (int)sizeof(g_b_damage_text), "-"); waifu_str_cat_i32(g_b_damage_text, (int)sizeof(g_b_damage_text), dmg);
    g_b_battle_outcome = BATTLE_DIRECT_ATTACK;
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_prewarm_big_art_pair(atk_id, CARD_NONE);
#else
    (void)card_big_art_ptr(atk_id);
#endif
    set_battle_phase(attacker_owner == 0 ? IB_PLAYER_BATTLE : IB_COM_BATTLE);
}

static void clear_monster_slot(int owner, int slot)
{
    if (slot < 0 || slot >= I_FIELD) return;
    if (owner == 0) {
        g_i_player_field[slot] = CARD_NONE;
        g_i_player_faceup[slot] = 1;
        g_i_player_defense[slot] = 0;
        g_i_player_attacked[slot] = 0;
        g_i_player_atk_bonus[slot] = 0;
        g_i_player_def_bonus[slot] = 0;
        clear_player_equips_for_target(slot);
    } else {
        g_i_com_field[slot] = CARD_NONE;
        g_i_com_faceup[slot] = 1;
        g_i_com_defense[slot] = 0;
        g_i_com_attacked[slot] = 0;
        g_i_com_atk_bonus[slot] = 0;
        g_i_com_def_bonus[slot] = 0;
        clear_com_equips_for_target(slot);
    }
}

static int thunder_intro_frames(void)
{
    return WAIFU_THUNDER_CARD_FRAMES + WAIFU_THUNDER_FADE_FRAMES;
}

static int thunder_target_frames(void)
{
    return BATTLE_BURN_DUR + WAIFU_THUNDER_TARGET_GAP_FRAMES;
}

static int thunder_total_frames(void)
{
    return thunder_intro_frames() + g_b_thunder_count * thunder_target_frames() + WAIFU_THUNDER_TARGET_GAP_FRAMES;
}

static void start_thunder(int owner, int hand_slot)
{
    int i;
    int target_owner = owner ? 0 : 1;
    int *hand = owner ? g_i_com_hand : g_i_player_hand;
    int *used = owner ? g_i_com_used : g_i_player_used;
    if (hand_slot < 0 || hand_slot >= I_HAND) return;
    if (used[hand_slot]) return;
    if (!is_thunder_support_card(hand[hand_slot])) return;
    if ((target_owner ? count_live_com_monsters() : count_live_player_monsters()) <= 0) return;

    g_b_thunder_hand = hand_slot;
    g_b_thunder_card = hand[hand_slot];
    g_b_thunder_owner = owner ? 1 : 0;
    g_b_thunder_count = 0;
    for (i = 0; i < I_FIELD; ++i) {
        g_b_thunder_slots[i] = -1;
        g_b_thunder_cards[i] = CARD_NONE;
        g_b_thunder_backs[i] = 0;
    }
    for (i = 0; i < I_FIELD; ++i) {
        int card = target_owner ? g_i_com_field[i] : g_i_player_field[i];
        if (is_monster_card(card) && g_b_thunder_count < I_FIELD) {
            int out = g_b_thunder_count++;
            g_b_thunder_slots[out] = i;
            g_b_thunder_cards[out] = card;
            g_b_thunder_backs[out] = target_owner ? !g_i_com_faceup[i] : !g_i_player_faceup[i];
#if defined(WAIFU_FM_CD32X)
            (void)waifu_assets_prewarm_big_art_pair(card, CARD_NONE);
#else
            (void)card_big_art_ptr(card);
#endif
        }
    }
    if (g_b_thunder_count <= 0) return;
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_support_big_art();
#else
    (void)support_big_art_ptr();
#endif
    used[hand_slot] = 1;
    clear_battle_snapshot();
    set_battle_phase(IB_COM_THUNDER_ANIM);
}

static void start_com_thunder(int hand_slot)
{
    start_thunder(1, hand_slot);
}

static void start_player_thunder(int hand_slot)
{
    start_thunder(0, hand_slot);
}

/* Auto-fire the player's purple Trap card against a declared COM attack.
   Returns 1 if a trap was held and triggered (the caller must then abort the
   normal attack: the trap destroys the attacker before the battle step, so no
   reveal or damage happens). The trap targets only the attacking COM monster
   and reuses the thunder destruction animation. */
static int try_trigger_player_trap(int com_attacker_slot)
{
    int hand_slot = -1;
    int field_slot = -1;
    int trap_card = CARD_NONE;
    int i;
    if (com_attacker_slot < 0 || com_attacker_slot >= I_FIELD) return 0;
    if (!is_monster_card(g_i_com_field[com_attacker_slot])) return 0;
    /* A face-down Trap set on the support row fires first; otherwise fall back
       to a Trap still held in the hand (it auto-activates either way). */
    for (i = 0; i < I_FIELD; ++i) {
        if (is_trap_support_card(g_i_player_equip_field[i])) { field_slot = i; break; }
    }
    if (field_slot >= 0) {
        trap_card = g_i_player_equip_field[field_slot];
    } else {
        for (i = 0; i < I_HAND; ++i) {
            if (!g_i_player_used[i] && is_trap_support_card(g_i_player_hand[i])) { hand_slot = i; break; }
        }
        if (hand_slot < 0) return 0;
        trap_card = g_i_player_hand[hand_slot];
    }

    g_b_thunder_hand = hand_slot;
    g_b_trap_field_slot = field_slot;
    g_b_thunder_card = trap_card;
    g_b_thunder_owner = 0; /* player reacts */
    g_b_trap_counter_active = 1;
    for (i = 0; i < I_FIELD; ++i) {
        g_b_thunder_slots[i] = -1;
        g_b_thunder_cards[i] = CARD_NONE;
        g_b_thunder_backs[i] = 0;
    }
    g_b_thunder_slots[0] = com_attacker_slot;
    g_b_thunder_cards[0] = g_i_com_field[com_attacker_slot];
    /* Show the attacker burning in whatever state it was in: a face-down
       attacker is destroyed without ever being flipped face up. */
    g_b_thunder_backs[0] = !g_i_com_faceup[com_attacker_slot];
    g_b_thunder_count = 1;
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_prewarm_big_art_pair(g_b_thunder_cards[0], CARD_NONE);
    (void)waifu_assets_support_big_art();
#else
    (void)card_big_art_ptr(g_b_thunder_cards[0]);
    (void)support_big_art_ptr();
#endif
    if (hand_slot >= 0) g_i_player_used[hand_slot] = 1;
    clear_battle_snapshot();
    set_battle_phase(IB_COM_THUNDER_ANIM);
    return 1;
}

static void finish_thunder(void)
{
    int i;
    int is_trap = g_b_trap_counter_active;
    int owner = g_b_thunder_owner;
    int target_owner = owner ? 0 : 1;
    for (i = 0; i < g_b_thunder_count; ++i) {
        int slot = g_b_thunder_slots[i];
        int card = (target_owner == 0 && slot >= 0 && slot < I_FIELD) ? g_i_player_field[slot] :
                   (target_owner == 1 && slot >= 0 && slot < I_FIELD) ? g_i_com_field[slot] : CARD_NONE;
        if (is_monster_card(card)) {
            clear_monster_slot(target_owner, slot);
        }
    }
    g_b_cards_used++;
    if (owner == 0) {
        g_b_selected_player_slot = selected_or_first_live_player_slot();
        if (g_b_selected_player_slot < 0) g_b_selected_player_slot = 0;
        set_top_selector(g_b_selected_player_slot, PLAYER_CARD_ROW);
    } else {
        g_b_selected_player_slot = selected_or_first_live_player_slot();
        if (g_b_selected_player_slot < 0) g_b_selected_player_slot = 0;
    }
    if (is_trap && g_b_trap_field_slot >= 0 && g_b_trap_field_slot < I_FIELD) {
        g_i_player_equip_field[g_b_trap_field_slot] = CARD_NONE;
        g_i_player_equip_target[g_b_trap_field_slot] = -1;
    }
    g_b_trap_field_slot = -1;
    g_b_thunder_hand = -1;
    g_b_thunder_card = CARD_NONE;
    g_b_thunder_count = 0;
    g_b_thunder_owner = 1;
    g_b_trap_counter_active = 0;
    clear_battle_snapshot();
    invalidate_battle_composite_cache();
    /* A trap cancelled a COM attack: hand control back to the COM battle loop so
       it can act with its remaining monsters or end its turn. Otherwise this was
       a normal thunder play and returns to that player's turn. */
    if (is_trap) set_battle_phase(IB_COM_BATTLE);
    else set_battle_phase(owner == 0 ? IB_PLAYER_TOP : IB_COM_BATTLE);
}

static void draw_com_thunder_anim(void)
{
    int f = g_b_phase_frame;
    int intro = thunder_intro_frames();
    int card_x = WAIFU_BIG_ART_128_X;
    int card_y = 35;
    clear_screen(IDX_BLACK);

    {
    int is_trap = g_b_trap_counter_active;
    const char *title = is_trap ? "MIRROR VEIL" : "THUNDER";
    int title_col = is_trap ? IDX_TRAP_FRAME_HI : IDX_GOLD_HI;

    if (f < intro) {
        int fade_start = WAIFU_THUNDER_CARD_FRAMES;
        draw_support_big_art_scaled(card_x, card_y, 128, 128);
        if (is_trap) rect_outline(card_x - 2, card_y - 2, 132, 132, IDX_TRAP_FRAME);
        draw_centered_text(WAIFU_UI_BOTTOM_Y(174), title, title_col, IDX_BLACK);
        draw_wrapped_text_small((WAIFU_FM_WIDTH - 200) / 2, WAIFU_UI_BOTTOM_Y(194),
                                is_trap ? "TRAP: DESTROY ATTACKER" :
                                (g_b_thunder_owner == 0 ? "ALL COM MONSTERS" : "ALL PLAYER MONSTERS"),
                                25, IDX_WHITE, IDX_BLACK);
        if (f >= fade_start) {
            apply_black_dither_fade(Q8_ONE - q8_ratio(f - fade_start, WAIFU_THUNDER_FADE_FRAMES));
        }
        return;
    }

    {
        int local = f - intro;
        int seg_frames = thunder_target_frames();
        int idx = local / seg_frames;
        int seg = local % seg_frames;
        if (idx >= 0 && idx < g_b_thunder_count) {
            int id = g_b_thunder_cards[idx];
            int back = g_b_thunder_backs[idx];
            if (seg == 0) waifu_sound_play(WAIFU_SOUND_CARD_DESTROYED);
            draw_centered_text(8, title, title_col, IDX_BLACK);
            if (seg < BATTLE_BURN_DUR) {
                draw_big_battle_card_burning(id, WAIFU_SINGLE_BATTLE_CARD_X, WAIFU_BATTLE_CARD_Y, back, seg);
            }
        }
    }
    }
}

static int player_support_total_frames(void)
{
    return WAIFU_SUPPORT_REVEAL_FRAMES + WAIFU_SUPPORT_TEXT_FRAMES;
}

static void start_player_one_shot_support(int hand_slot)
{
    int card;
    if (hand_slot < 0 || hand_slot >= I_HAND) return;
    if (g_i_player_used[hand_slot]) return;
    card = g_i_player_hand[hand_slot];
    if (!is_draw_support_card(card) && !is_heal_support_card(card)) return;
    if (is_draw_support_card(card) && g_i_player_deck_left <= 0) return;

    g_b_support_hand = hand_slot;
    g_b_support_card = card;
    g_b_support_kind = support_card_kind(card);
    g_b_support_lp_from = g_you_lp;
    g_b_support_lp_to = is_heal_support_card(card) ? g_you_lp + WAIFU_SUPPORT_HEAL_AMOUNT : g_you_lp;
    g_i_player_used[hand_slot] = 1;
    clear_player_fusion_queue();
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_support_big_art();
#else
    (void)support_big_art_ptr();
#endif
    set_battle_phase(IB_PLAYER_SUPPORT_ANIM);
}

static void finish_player_one_shot_support(void)
{
    int hand = g_b_support_hand;
    int kind = g_b_support_kind;
    int next_phase = IB_PLAYER_HAND;
    if (hand >= 0 && hand < I_HAND) {
        if (kind == 2 && g_i_player_deck_left > 0) {
            int i;
            g_i_player_hand[hand] = next_draw_id();
            g_i_player_used[hand] = 0;
            g_b_selected_hand = hand;
            /* Ancient Draw: show the card being drawn off the deck via the same
               IB_PLAYER_DRAW slide + staggered CARD_DRAWN SFX as a turn draw,
               instead of snapping the new card straight into the hand. */
            clear_player_fusion_queue();
            g_b_draw_count = 0;
            for (i = 0; i < I_HAND; ++i) g_b_draw_slots[i] = -1;
            g_b_draw_slots[g_b_draw_count++] = hand;
            next_phase = IB_PLAYER_DRAW;
        } else if (kind == 3) {
            g_you_lp = g_b_support_lp_to;
            g_b_selected_hand = next_live_hand_index(hand, 1);
            waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
        }
    }
    g_b_cards_used++;
    g_b_support_hand = -1;
    g_b_support_card = CARD_NONE;
    g_b_support_kind = -1;
    g_b_support_lp_from = 0;
    g_b_support_lp_to = 0;
    clear_battle_snapshot();
    invalidate_battle_composite_cache();
    set_battle_phase(next_phase);
}

static void draw_player_one_shot_support_anim(void)
{
    int f = g_b_phase_frame;
    int reveal = WAIFU_SUPPORT_REVEAL_FRAMES;
    clear_screen(IDX_BLACK);
    draw_support_big_art_112((WAIFU_FM_WIDTH - WAIFU_BIG_W) / 2, WAIFU_UI_BOTTOM_Y(32));
    draw_centered_text(WAIFU_UI_BOTTOM_Y(154), support_card_name(g_b_support_card), IDX_GOLD_HI, IDX_BLACK);
    /* The card and its effect text share one scene: keep the card fully visible
       the whole time and let the text appear over it.  Do not dip to black in
       between -- a fade belongs only on the transition to the next scene (e.g.
       the IB_PLAYER_DRAW slide that follows an Ancient Draw). */
    if (f < reveal) {
        return;
    }
    if (g_b_support_kind == 2) {
        draw_centered_text(WAIFU_UI_BOTTOM_Y(184), "DRAW 1 CARD", IDX_WHITE, IDX_BLACK);
        draw_centered_text(WAIFU_UI_BOTTOM_Y(205), "FROM YOUR DECK", IDX_WHITE, IDX_BLACK);
    } else if (g_b_support_kind == 3) {
        char line[48];
        int32_t t = q8_smooth_ratio(f - reveal, WAIFU_SUPPORT_TEXT_FRAMES);
        int lp = g_b_support_lp_from +
                 (int)(((g_b_support_lp_to - g_b_support_lp_from) * q8_smoothstep(t) + Q8_HALF) >> Q8_SHIFT);
        waifu_str_copy(line, (int)sizeof(line), "LP ");
        waifu_str_cat_i32(line, (int)sizeof(line), g_b_support_lp_from);
        waifu_str_cat(line, (int)sizeof(line), " > ");
        waifu_str_cat_i32(line, (int)sizeof(line), lp);
        draw_centered_text(WAIFU_UI_BOTTOM_Y(188), line, IDX_GREEN, IDX_BLACK);
        draw_centered_text(WAIFU_UI_BOTTOM_Y(207), "LIFE RESTORED", IDX_WHITE, IDX_BLACK);
    }
}

static void reveal_monster_slot(int owner, int slot)
{
    if (slot < 0 || slot >= I_FIELD) return;
    if (owner == 0) g_i_player_faceup[slot] = 1;
    else g_i_com_faceup[slot] = 1;
}

static void resolve_battle(void)
{
    int attacker_owner = g_b_battle_atk_owner;
    int defender_owner = attacker_owner ? 0 : 1;

    if (attacker_owner == 0 && g_b_battle_atk_slot >= 0) {
        g_i_player_attacked[g_b_battle_atk_slot] = 1;
        g_i_player_faceup[g_b_battle_atk_slot] = 1;
    }
    if (attacker_owner == 1 && g_b_battle_atk_slot >= 0) {
        g_i_com_attacked[g_b_battle_atk_slot] = 1;
        g_i_com_faceup[g_b_battle_atk_slot] = 1;
    }

    if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) {
        if (attacker_owner == 0) g_com_lp -= g_b_direct_damage;
        else g_you_lp -= g_b_direct_damage;
    } else {
        reveal_monster_slot(defender_owner, g_b_battle_def_slot);

        if (g_b_battle_outcome == BATTLE_DESTROY_DEFENDER) {
            clear_monster_slot(defender_owner, g_b_battle_def_slot);
        } else if (g_b_battle_outcome == BATTLE_DESTROY_ATTACKER) {
            clear_monster_slot(attacker_owner, g_b_battle_atk_slot);
        } else if (g_b_battle_outcome == BATTLE_DESTROY_BOTH) {
            clear_monster_slot(attacker_owner, g_b_battle_atk_slot);
            clear_monster_slot(defender_owner, g_b_battle_def_slot);
        }

        if (g_b_battle_damage > 0) {
            if (g_b_battle_damage_owner == 0) g_you_lp -= g_b_battle_damage;
            else if (g_b_battle_damage_owner == 1) g_com_lp -= g_b_battle_damage;
        }
    }

    if (g_you_lp <= 0) { g_you_lp = 0; g_b_result = -1; set_battle_phase(IB_RESULT); }
    else if (g_com_lp <= 0) { g_com_lp = 0; g_b_result = 1; set_battle_phase(IB_RESULT); }
}

static void draw_direct_attack_event(int f, int atk_id, int atk_col, int atk_row, int atk_back)
{
    int local = f;
    const int flip_dur = WAIFU_BATTLE_FLIP_FRAMES;
    /* Match the normal monster-battle card lanes. The previous direct-attack
       lanes were shifted inward, so the attacker appeared too far right/left
       before and after the lunge. */
    int ax = (g_b_battle_atk_owner == 0) ? WAIFU_BATTLE_CARD_X0 : WAIFU_BATTLE_CARD_X1;
    int ay = WAIFU_BATTLE_CARD_Y;
    int target_x = (g_b_battle_atk_owner == 0) ? (WAIFU_BATTLE_CARD_X1 + 22) : (WAIFU_BATTLE_CARD_X0 + 24);
    int target_y = WAIFU_BATTLE_CARD_Y;
    int card_x = ax;
    clear_screen(IDX_BLACK);
    if (local < WAIFU_BATTLE_PRELUDE_FRAMES) {
        Camera cam = side_battle_camera(atk_row);
        draw_interactive_base(cam);
        draw_zone_cursor(cam, atk_col, atk_row);
        return;
    }
    local -= WAIFU_BATTLE_PRELUDE_FRAMES;
    if (atk_back && local < flip_dur) {
        draw_big_battle_card_flip(atk_id, ax, ay, local, flip_dur);
        return;
    }
    if (atk_back) local -= flip_dur;
    if (local == WAIFU_DIRECT_SLIDE_FRAMES) waifu_sound_play(WAIFU_SOUND_LASER_SHOOT);
    {
        int hit_frame = WAIFU_DIRECT_SLIDE_FRAMES + (WAIFU_DIRECT_LUNGE_FRAMES / 2);
        if (local == hit_frame) waifu_sound_play(WAIFU_SOUND_DIRECT_HIT);
    }
    if (local < WAIFU_DIRECT_SLIDE_FRAMES) {
        int32_t e = q8_smooth_ratio(local, WAIFU_DIRECT_SLIDE_FRAMES);
        card_x = lerp_i((g_b_battle_atk_owner == 0) ? -WAIFU_BATTLE_CARD_W : WAIFU_FM_WIDTH + 8, ax, e);
    } else if (local < WAIFU_DIRECT_SLIDE_FRAMES + WAIFU_DIRECT_LUNGE_FRAMES) {
        int32_t t = q8_ratio(local - WAIFU_DIRECT_SLIDE_FRAMES, WAIFU_DIRECT_LUNGE_FRAMES);
        int32_t lunge = (t < Q8_FRAC(62,100)) ? q8_smoothstep(q8_div(t, Q8_FRAC(62,100))) : Q8_ONE - q8_smoothstep(q8_div(t - Q8_FRAC(62,100), Q8_FRAC(38,100)));
        int dir = (g_b_battle_atk_owner == 0) ? 1 : -1;
        card_x = ax + q8_to_int(q8_mul(Q8_FROM_INT(64), lunge)) * dir;
    }
    draw_cutin_battle_card(atk_id, card_x, ay, 0, 1);
    {
        int slash_start = WAIFU_DIRECT_SLIDE_FRAMES + (WAIFU_DIRECT_LUNGE_FRAMES / 2);
        int damage_start = WAIFU_DIRECT_SLIDE_FRAMES + WAIFU_DIRECT_LUNGE_FRAMES - 2;
        if (local >= slash_start && local < slash_start + WAIFU_DIRECT_DAMAGE_HOLD_FRAMES) {
            draw_direct_attack_slash(target_x, target_y, local - slash_start, g_b_battle_atk_owner);
        }
        if (local >= damage_start && local < damage_start + WAIFU_DIRECT_DAMAGE_HOLD_FRAMES) {
            draw_centered_damage_text_in_card(target_x - 20, 36, g_b_damage_text);
        }
    }
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

    if (g_b_phase_frame < WAIFU_BATTLE_PRELUDE_FRAMES) {
        prelude_cam = side_battle_camera(atk_row);
        draw_interactive_base(prelude_cam);
        draw_zone_cursor(prelude_cam, atk_col, atk_row);
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

static void draw_post_battle_return(int attacker_owner, int f, int focus_slot, int bottom_card, const char *mode, int fade_in)
{
    (void)bottom_card;
#ifdef WAIFU_FM_PCFX
    const int settle_frames = 1;
    const int fade_frames = 4;
#else
    const int settle_frames = 20;
    const int fade_frames = 8;
#endif
    Camera from = side_battle_camera(attacker_owner == 0 ? PLAYER_CARD_ROW : ENEMY_CARD_ROW);
    Camera to = (attacker_owner == 0) ? battle_top_camera() : enemy_battle_top_camera();
#ifdef WAIFU_FM_PCFX
    Camera cam = to;
    (void)from;
    (void)settle_frames;
#else
    Camera cam = lerp_camera(from, to, q8_ratio(f, settle_frames));
#endif
    render_board_cached(cam);
    draw_interactive_field_cards(cam);
    draw_hud();
    if (attacker_owner == 0 && focus_slot >= 0) draw_bottom_info_field(0, focus_slot, mode ? mode : "FIELD");
    if (focus_slot >= 0) draw_zone_cursor(cam, focus_slot, attacker_owner == 0 ? PLAYER_CARD_ROW : ENEMY_CARD_ROW);
    if (fade_in && f < fade_frames) apply_black_dither_fade(q8_ratio(f, fade_frames));
}

static void draw_interactive_result(void)
{
    int local = g_b_phase_frame;
    const char *msg = g_b_result < 0 ? "YOU LOSE" : "YOU WIN";
    const int card_id = result_focus_card_id();

    if (local == WAIFU_RESULT_UI_CLEAR_FRAMES) update_music_for_current_state();

    if (local < WAIFU_RESULT_UI_CLEAR_FRAMES) {
        int32_t e = q8_smooth_ratio(local, WAIFU_RESULT_UI_CLEAR_FRAMES);
        int hud_field_x = -q8_to_int(q8_mul(Q8_FROM_INT(86), e));
        int hud_lp_x = q8_to_int(q8_mul(Q8_FROM_INT(92), e));
        int bottom_y = q8_to_int(q8_mul(Q8_FROM_INT(52), e));
        int hand_y = q8_to_int(q8_mul(Q8_FROM_INT(126), e));

        /* Freeze the 3D field immediately after the battle cut-in.  Only the
           overlays move: HUD panels slide outward, the bottom info plate and
           the hand leave through the bottom edge. */
        draw_interactive_field_base_no_hud(battle_top_camera());
        draw_hud_offset(hud_field_x, 0, hud_lp_x, 0);
        draw_bottom_info_offset(card_id, "RESULT", bottom_y);
        draw_interactive_player_hand(999, g_b_selected_hand, hand_y, 1);
        return;
    }

    if (local < WAIFU_RESULT_ANIM_START_FRAMES) {
        /* Result CD-DA has started; hold the cleaned field briefly so the music
           lead-in is perceptible before the camera move begins. */
        draw_interactive_field_base_no_hud(battle_top_camera());
        return;
    }

    {
        int anim = local - WAIFU_RESULT_ANIM_START_FRAMES;
        int clamped_anim = anim;
        if (clamped_anim > WAIFU_PCFX_HANDTOP_FRAMES) clamped_anim = WAIFU_PCFX_HANDTOP_FRAMES;
        /* Use the same top->hand camera/keyframe helper as the normal DOWN path,
           but keep the result overlays cleared instead of reintroducing battle
           HUD panels. */
        draw_player_handtop_transition_shared(clamped_anim, WAIFU_PCFX_HANDTOP_FRAMES,
                                              0, 0, 0, 0);
        if (anim >= WAIFU_PCFX_HANDTOP_FRAMES + 8) {
            int text_f = anim - WAIFU_PCFX_HANDTOP_FRAMES - 8;
            int32_t e = q8_smooth_ratio(text_f, 42);
            int scale = (text_f < 42) ? 2 + (e > Q8_FRAC(55,100) ? 1 : 0) : 3;
            int tw = (int)strlen(msg) * 8 * scale;
            int x = (WAIFU_FM_WIDTH - tw) / 2;
            int y = 100 - q8_to_int(q8_mul(Q8_FROM_INT(10), Q8_ONE - e));
            draw_text_scaled(x, y, msg, scale, g_b_result < 0 ? IDX_RED : IDX_GOLD_HI, IDX_BLACK);
        }
    }
}

static void draw_interactive_tally(void)
{
    int won = g_b_result >= 0;
    int ox = WAIFU_UI_CENTER_DX;
    clear_screen(IDX_BLACK);
    draw_panel_rect(ox + 31, 24, 194, 178, IDX_UI_DARK);
    draw_centered_text_scaled(37, won ? "DUEL VICTORY" : "DUEL DEFEAT", 1, won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    hline(ox + 45, ox + 210, 56, IDX_UI_LIGHT);
    int score = 0;
    char rank = battle_rank_from_stats(won, won ? g_you_lp : 0, g_b_cards_used, g_b_turns, &score);
    char line[64];
    draw_text(ox + 55, 74, "RESULT", IDX_WHITE, IDX_BLACK);
    draw_text(ox + 150, 74, won ? "WIN" : "LOSE", won ? IDX_GOLD_HI : IDX_RED, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), won ? g_you_lp : 0); draw_text(ox + 55, 94, "LP LEFT", IDX_WHITE, IDX_BLACK); draw_text(ox + 160, 94, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), g_b_cards_used); draw_text(ox + 55, 112, "CARDS USED", IDX_WHITE, IDX_BLACK); draw_text(ox + 176, 112, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), g_i_player_deck_left); draw_text(ox + 55, 130, "DECK LEFT", IDX_WHITE, IDX_BLACK); draw_text(ox + 176, 130, line, IDX_GOLD_HI, IDX_BLACK);
    fmt_i32_dec(line, (int)sizeof(line), score); draw_text(ox + 55, 148, "SCORE", IDX_WHITE, IDX_BLACK); draw_text(ox + 152, 148, line, IDX_GOLD_HI, IDX_BLACK);
    waifu_str_copy(line, (int)sizeof(line), "RANK "); waifu_str_cat_char(line, (int)sizeof(line), rank); draw_centered_text_scaled(170, line, 2, IDX_GOLD_HI, IDX_BLACK);
    draw_text_small(WAIFU_UI_CENTER_DX + 50, WAIFU_UI_BOTTOM_Y(210), (g_story_battle_active && won) ? "RUN: CLAIM REWARD" :
                              (g_story_battle_active ? "RUN: RETURN TO MAP" : "RUN: RETURN TO TITLE"),
                    IDX_WHITE, IDX_BLACK);
}

/* Story-win reward reveal: shows the card the player just earned (already rolled
   into g_b_reward_card at the tally) before it is added to storage. */
static void draw_interactive_reward(void)
{
    int card = g_b_reward_card;
    const char *name;
    /* The earned card is revealed with the same face-down -> flip-up turn the
       battle cut-in uses, then settles into the static reward art. Support-card
       rewards (which the static path frames specially) skip the scaled flip. */
    int flip_dur = WAIFU_BATTLE_FLIP_FRAMES;
    int flipping = is_monster_card(card) && g_b_phase_frame < flip_dur;
    waifu_fm_use_common_palette();
    clear_screen(IDX_BLACK);
    draw_panel_rect(WAIFU_UI_CENTER_DX + 31, 18, 194, WAIFU_FM_HEIGHT - 40, IDX_UI_DARK);
    draw_centered_text_scaled(26, "CARD WON!", 1, IDX_GOLD_HI, IDX_BLACK);
    hline(WAIFU_UI_CENTER_DX + 45, WAIFU_UI_CENTER_DX + 210, 44, IDX_UI_LIGHT);

#if defined(WAIFU_FM_PCFX)
    /* Static held screen: render big art through the framebuffer, not the
       presenter's direct-KRAM bypass (see draw_interactive_card_preview). */
    ++g_big_art_direct_note_suppressed;
#endif
    if (flipping) {
        draw_big_battle_card_flip(card, WAIFU_SINGLE_BATTLE_CARD_X, WAIFU_BATTLE_CARD_Y, g_b_phase_frame, flip_dur);
    } else if (is_support_card(card)) {
        int trap = is_trap_support_card(card);
        draw_support_big_art_112((WAIFU_FM_WIDTH - WAIFU_BIG_W) / 2, WAIFU_UI_BOTTOM_Y(54));
        rect_outline((WAIFU_FM_WIDTH - WAIFU_BIG_W) / 2 - 1, WAIFU_UI_BOTTOM_Y(53), 114, 114, trap ? IDX_TRAP_FRAME_HI : IDX_BLUE_WHITE);
        rect_outline((WAIFU_FM_WIDTH - WAIFU_BIG_W) / 2, WAIFU_UI_BOTTOM_Y(54), 112, 112, trap ? IDX_TRAP_FRAME : IDX_UI_BLUE);
    } else if (is_monster_card(card)) {
        draw_card_big_art_112(card, (WAIFU_FM_WIDTH - WAIFU_BIG_W) / 2, WAIFU_UI_BOTTOM_Y(54));
    }
#if defined(WAIFU_FM_PCFX)
    --g_big_art_direct_note_suppressed;
#endif

    if (flipping) return; /* hold the name/prompt until the card has flipped up */

    name = is_support_card(card) ? support_card_name(card)
         : (is_monster_card(card) ? waifu_card_names[card] : "???");
    draw_centered_text(WAIFU_UI_BOTTOM_Y(174), name, IDX_WHITE, IDX_BLACK);
    draw_centered_text(WAIFU_UI_BOTTOM_Y(191), "ADDED TO STORAGE", IDX_GOLD_HI, IDX_BLACK);
    draw_text_small((WAIFU_FM_WIDTH - 13 * 8) / 2, WAIFU_UI_BOTTOM_Y(202), "RUN: CONTINUE", IDX_WHITE, IDX_BLACK);
}

static int draw_replacement_cards_to_hand(void)
{
    int i;
    g_b_draw_count = 0;
    clear_player_fusion_queue();
    for (i = 0; i < I_HAND; ++i) g_b_draw_slots[i] = -1;
    if (g_i_player_deck_left <= 0) return 0;

    /* Draw into every spent/empty hand slot first.  If the player ended the
       turn with a completely full live hand, the turn draw must still happen:
       slot 0 is discarded/replaced by the drawn card.  This prevents full-hand
       stalling where the deck never advances and deck-out can be avoided. */
    /* Note: the CARD_DRAWN SFX is NOT played here.  This runs on the silent
       turn-flip transition frame; instead the draw sound is staggered per drawn
       card inside the IB_PLAYER_DRAW animation (play_turn_draw_sfx), in sync with
       each card sliding off the deck, mirroring the opening-hand deal. */
    for (i = 0; i < I_HAND; ++i) {
        if (g_i_player_used[i]) {
            if (g_i_player_deck_left <= 0) {
                return g_b_draw_count > 0;
            }
            g_i_player_hand[i] = next_draw_id();
            g_i_player_used[i] = 0;
            g_b_draw_slots[g_b_draw_count++] = i;
        }
    }
    if (g_b_draw_count == 0 && g_i_player_deck_left > 0) {
        g_i_player_hand[0] = next_draw_id();
        g_i_player_used[0] = 0;
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
            drew = 1;
        }
    }
    if (!drew && g_i_com_deck_left > 0) {
        g_i_com_hand[0] = next_com_draw_id();
        g_i_com_used[0] = 0;
    }
}

static int is_recent_draw_slot(int slot)
{
    int i;
    for (i = 0; i < g_b_draw_count; ++i) if (g_b_draw_slots[i] == slot) return i;
    return -1;
}

/* Frame at which the drawn card in draw-order index `d` begins its slide off
   the deck.  Shared by the visual (draw_player_hand_turn_draw) and the audio
   (play_turn_draw_sfx) so the CARD_DRAWN SFX lands exactly as the card appears. */
static int turn_draw_slide_start(int d)
{
    return (WAIFU_PCFX_DRAW_FRAMES * 2) / 9 + d * ((WAIFU_PCFX_DRAW_FRAMES + 4) / 8);
}

/* Play the draw SFX once per newly drawn card, staggered to match the slide-in,
   like the staggered opening-hand deal (play_player_hand_intro_draw_sfx). */
static void play_turn_draw_sfx(int f)
{
    int i;
    for (i = 0; i < g_b_draw_count; ++i) {
        if (f == turn_draw_slide_start(i)) waifu_sound_play(WAIFU_SOUND_CARD_DRAWN);
    }
}

static void draw_player_hand_turn_draw(int f, int selected)
{
    PROFILE_HAND_BEGIN();
    int i;
    /* The kept cards rise quickly back into place (the hand returning after the
       turn-flip camera), then settle.  The newly drawn card(s) slide in from the
       deck off the right edge so the draw itself is clearly visible, instead of
       the whole hand sliding up as one block (which read like a re-deal). */
    int rise = q8_to_int(q8_mul(Q8_FROM_INT(92),
                  Q8_ONE - q8_smooth_ratio(f, (WAIFU_PCFX_DRAW_FRAMES + 1) / 2)));
    for (i = 0; i < I_HAND; ++i) {
        int x0 = hand_final_x(i);
        int x = x0;
        int y = WAIFU_HAND_Y_BASE;
        int d = is_recent_draw_slot(i);
        if (g_i_player_used[i]) continue;
        if (d >= 0) {
            int start = turn_draw_slide_start(d);
            int dur = (WAIFU_PCFX_DRAW_FRAMES * 4) / 9;
            if (dur < 4) dur = 4;
            int32_t t = q8_smooth_ratio(f - start, dur);
            x = lerp_i(282 + WAIFU_UI_EXTRA_W, x0, t);
        } else {
            y = WAIFU_HAND_Y_BASE + rise;
        }
        PROFILE_HAND_CARD_DRAW(draw_hand_card_sprite_ex(g_i_player_hand[i], x, y, 38, 50, 0,
                                 is_monster_card(g_i_player_hand[i]) && player_hand_monster_blocked()));
        if (f >= WAIFU_PCFX_DRAW_FRAMES && i == selected) draw_red_cursor(x, y, 38, 50);
    }
    PROFILE_HAND_END();
}

static int current_battle_anim_frames(void)
{
    if (g_b_battle_outcome == BATTLE_DIRECT_ATTACK) {
        return WAIFU_BATTLE_PRELUDE_FRAMES +
               (g_b_battle_atk_back ? WAIFU_BATTLE_FLIP_FRAMES : 0) +
               WAIFU_DIRECT_SLIDE_FRAMES + WAIFU_DIRECT_LUNGE_FRAMES +
               WAIFU_DIRECT_DAMAGE_HOLD_FRAMES + WAIFU_BATTLE_FINAL_SETTLE_FRAMES;
    }

    const int slide_dur = WAIFU_BATTLE_SLIDE_FRAMES;
    const int flip_dur = WAIFU_BATTLE_FLIP_FRAMES;
    const int pause_after_reveal = WAIFU_BATTLE_REVEAL_PAUSE_FRAMES;
    const int ram_dur = WAIFU_BATTLE_RAM_FRAMES;
    const int counter_dur = WAIFU_BATTLE_RAM_FRAMES;
    int reveal_end = slide_dur + (g_b_battle_atk_back ? flip_dur : 0) + (g_b_battle_def_back ? flip_dur : 0);
    int ram_start = reveal_end + pause_after_reveal;
    int burn_start;

    if (g_b_battle_outcome == BATTLE_DESTROY_ATTACKER) {
        int counter_start = ram_start + ram_dur + WAIFU_BATTLE_COUNTER_GAP_FRAMES;
        burn_start = counter_start + counter_dur + WAIFU_BATTLE_COUNTER_GAP_FRAMES;
    } else {
        burn_start = ram_start + ram_dur + WAIFU_BATTLE_BURN_DELAY_FRAMES;
        if (g_b_battle_outcome == BATTLE_DESTROY_BOTH) burn_start = ram_start + ram_dur + (WAIFU_BATTLE_BURN_DELAY_FRAMES / 2);
    }

    return WAIFU_BATTLE_PRELUDE_FRAMES + burn_start + BATTLE_BURN_DUR + WAIFU_BATTLE_FINAL_SETTLE_FRAMES;
}

static int battle_animation_event_complete(int end_frame)
{
    /* Progression is gated by each animation's own completion event.  The PC-FX
       build no longer uses separate shortened delay counters that can advance
       gameplay while the draw routine is still mid-flip/mid-flight. */
    return g_b_phase_frame >= end_frame;
}


static void start_equip(int owner, int hand_slot, int target_slot)
{
    int equip_slot = owner ? first_free_com_equip_slot() : first_free_player_equip_slot();
    int equip_card;
    int target_card;
    if (hand_slot < 0 || hand_slot >= I_HAND || target_slot < 0 || target_slot >= I_FIELD) return;
    equip_card = owner ? g_i_com_hand[hand_slot] : g_i_player_hand[hand_slot];
    target_card = owner ? g_i_com_field[target_slot] : g_i_player_field[target_slot];
    if (!is_equip_support_card(equip_card) || !is_monster_card(target_card)) return;
    if (equip_slot < 0) return;

    g_b_equip_owner = owner;
    g_b_equip_hand = hand_slot;
    g_b_equip_slot = target_slot;
    g_b_equip_zone_slot = equip_slot;
    g_b_equip_card = equip_card;
    g_b_equip_target_card = target_card;
    g_b_equip_target_faceup = owner ? g_i_com_faceup[target_slot] : g_i_player_faceup[target_slot];
    g_b_equip_base_atk = field_card_atk(owner, target_slot);
    g_b_equip_base_def = field_card_def(owner, target_slot);
    g_b_equip_pending_atk = equip_atk_bonus(g_b_equip_card);
    g_b_equip_pending_def = equip_def_bonus(g_b_equip_card);
    /* Pre-cache both visible pieces before the equip animation starts.  On
       CD32X the normal draw-time big-art helpers are cache-only, so use the
       explicit prewarm/load APIs here while we are still between animation
       states.  Otherwise the equip reveal can draw a blank card and the next
       attack cut-in pays the first target-art CD read. */
#if defined(WAIFU_FM_CD32X)
    (void)waifu_assets_prewarm_big_art_pair(target_card, CARD_NONE);
    (void)waifu_assets_support_big_art();
#else
    (void)card_big_art_ptr(target_card);
    (void)support_big_art_ptr();
#endif
    if (owner == 0) {
        g_i_player_field[target_slot] = CARD_NONE;
        g_i_player_faceup[target_slot] = 1;
        g_i_player_used[hand_slot] = 1;
        set_battle_phase(IB_PLAYER_EQUIP_ANIM);
    } else {
        g_i_com_field[target_slot] = CARD_NONE;
        g_i_com_faceup[target_slot] = 1;
        g_i_com_used[hand_slot] = 1;
        set_battle_phase(IB_COM_EQUIP_ANIM);
    }
}

static void start_player_equip(int hand_slot, int target_slot)
{
    start_equip(0, hand_slot, target_slot);
}

static void start_com_equip(int hand_slot, int target_slot)
{
    start_equip(1, hand_slot, target_slot);
}

static void finish_equip(void)
{
    int owner = g_b_equip_owner;
    if (g_b_equip_slot < 0 || g_b_equip_slot >= I_FIELD) return;
    if (owner == 0) {
        g_i_player_field[g_b_equip_slot] = g_b_equip_target_card;
        g_i_player_faceup[g_b_equip_slot] = 1;
        g_i_player_atk_bonus[g_b_equip_slot] += g_b_equip_pending_atk;
        g_i_player_def_bonus[g_b_equip_slot] += g_b_equip_pending_def;
        if (g_b_equip_zone_slot >= 0 && g_b_equip_zone_slot < I_FIELD) {
            g_i_player_equip_field[g_b_equip_zone_slot] = g_b_equip_card;
            g_i_player_equip_target[g_b_equip_zone_slot] = g_b_equip_slot;
        }
        g_b_selected_player_slot = g_b_equip_slot;
        set_top_selector(g_b_equip_slot, PLAYER_CARD_ROW);
        g_b_selected_hand = next_live_hand_index(g_b_equip_hand, 1);
    } else {
        g_i_com_field[g_b_equip_slot] = g_b_equip_target_card;
        g_i_com_faceup[g_b_equip_slot] = 1;
        g_i_com_atk_bonus[g_b_equip_slot] += g_b_equip_pending_atk;
        g_i_com_def_bonus[g_b_equip_slot] += g_b_equip_pending_def;
        if (g_b_equip_zone_slot >= 0 && g_b_equip_zone_slot < I_FIELD) {
            g_i_com_equip_field[g_b_equip_zone_slot] = g_b_equip_card;
            g_i_com_equip_target[g_b_equip_zone_slot] = g_b_equip_slot;
        }
    }
    g_b_cards_used++;
    waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
    g_b_equip_owner = 0;
    g_b_equip_hand = -1;
    g_b_equip_slot = -1;
    g_b_equip_zone_slot = -1;
    g_b_equip_card = CARD_NONE;
    g_b_equip_target_card = CARD_NONE;
    clear_battle_snapshot();
    set_battle_phase(owner == 0 ? IB_PLAYER_TOP : IB_COM_BATTLE);
}

static void draw_equip_stat_line_centered(int y, const char *label, int from, int to, int32_t t)
{
    char line[32];
    int value = from + (int)(((to - from) * q8_smoothstep(t) + Q8_HALF) >> Q8_SHIFT);
    waifu_str_copy(line, (int)sizeof(line), label); waifu_str_cat_char(line, (int)sizeof(line), ' '); waifu_str_cat_u32_z4(line, (int)sizeof(line), (unsigned)value);
    draw_text((WAIFU_FM_WIDTH - text_px_width(line, 1)) / 2, y, line, stat_delta_color(to - from), IDX_BLACK);
}

static void draw_player_equip_target(void)
{
    Camera cam = battle_top_camera();
    int warm_slot = top_selector_player_monster_slot();
    if (warm_slot >= 0) {
        int warm_card = g_i_player_field[warm_slot];
        if (is_monster_card(warm_card)) (void)card_big_art_ptr(warm_card);
    }
    draw_interactive_base(cam);
    draw_top_selector_cursor(cam);
    draw_text_small((WAIFU_FM_WIDTH - 19 * 6) / 2, WAIFU_UI_BOTTOM_Y(191), "SELECT EQUIP TARGET", IDX_GOLD_HI, IDX_BLACK);
    if (top_selector_player_monster_slot() >= 0) draw_bottom_info_top_selector("EQUIP");
    else draw_bottom_info(g_i_player_hand[g_b_equip_hand], "EQUIP");
}

static void draw_player_equip_anim(void)
{
    int f = g_b_phase_frame;
    int32_t t = q8_smooth_ratio(f, WAIFU_EQUIP_ANIM_FRAMES);
#ifdef WAIFU_FM_PCFX
    int reveal_frames = WAIFU_BATTLE_FLIP_FRAMES;
#else
    int reveal_frames = (WAIFU_EQUIP_ANIM_FRAMES * 2) / 5;
#endif
    int merge_start = (WAIFU_EQUIP_ANIM_FRAMES * 7) / 20;
    int merge_frames = (WAIFU_EQUIP_ANIM_FRAMES * 9) / 20;
    if (reveal_frames < 4) reveal_frames = 4;
    if (merge_frames < 4) merge_frames = 4;
    int reveal = (!g_b_equip_target_faceup && f < reveal_frames);
    int32_t merge_t = q8_smooth_ratio(f - merge_start, merge_frames);
    int card_w = WAIFU_CARD_W;
    int card_h = WAIFU_CARD_H;
    int target_x = WAIFU_SINGLE_BATTLE_CARD_X;
    int target_y = WAIFU_BATTLE_CARD_Y;
    int target_cx = target_x + 60;
    int target_cy = target_y + 80;
    int card_x = lerp_i(26, target_cx - card_w / 2, merge_t);
    int card_y = lerp_i(36, target_cy - card_h / 2, merge_t);
    int atk_to = g_b_equip_base_atk + g_b_equip_pending_atk;
    int def_to = g_b_equip_base_def + g_b_equip_pending_def;

    clear_screen(IDX_BLACK);
    if (reveal) {
        draw_big_battle_card_flip(g_b_equip_target_card, target_x, target_y, f, reveal_frames);
    } else {
        draw_big_battle_card(g_b_equip_target_card, target_x, target_y, 0);
        if (f < WAIFU_EQUIP_ANIM_FRAMES - 3) {
            rect_fill(card_x + 5, card_y + 6, card_w, card_h, IDX_BLACK);
            draw_support_sprite(g_b_equip_card, card_x, card_y, card_w, card_h);
        }
    }

    for (int i = 0; i < 18; ++i) {
        int32_t ax = (int32_t)((f + i * 13) * Q8_FRAC(11,100));
        int32_t ay = (int32_t)((f + i * 17) * Q8_FRAC(9,100));
        int cx = target_cx + q8_to_int(q8_mul(q8_sin_rad(ax), Q8_FROM_INT(18 + (i % 5) * 5)));
        int cy = target_cy + q8_to_int(q8_mul(q8_cos_rad(ay), Q8_FROM_INT(18 + (i % 4) * 3)));
        draw_disc(cx, cy, 1 + (i % 3), (i & 1) ? IDX_GREEN : IDX_WHITE);
    }
    if (f >= (WAIFU_EQUIP_ANIM_FRAMES * 23) / 30 && f < (WAIFU_EQUIP_ANIM_FRAMES * 9) / 10) rect_fill(0, 0, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, (f & 2) ? IDX_WHITE : IDX_GOLD_HI);
    if ((f & 4) == 0) rect_outline(target_x - 4, target_y - 4, 128, 168, IDX_WHITE);
    draw_centered_text(9, "EQUIP POWER", IDX_GOLD_HI, IDX_BLACK);
    draw_equip_stat_line_centered(WAIFU_UI_BOTTOM_Y(190), "ATK", g_b_equip_base_atk, atk_to, t);
    draw_equip_stat_line_centered(WAIFU_UI_BOTTOM_Y(208), "DEF", g_b_equip_base_def, def_to, t);
}

static void reset_player_fusion_anim(void)
{
    int i;
    for (i = 0; i < FUSION_MAX_MATERIALS; ++i) {
        g_b_fusion_anim_slots[i] = -1;
        g_b_fusion_anim_cards[i] = CARD_NONE;
        g_b_fusion_anim_material_kept[i] = 0;
    }
    g_b_fusion_anim_count = 0;
    g_b_fusion_anim_result = CARD_NONE;
    g_b_fusion_anim_success = 0;
    g_b_fusion_anim_final_card = CARD_NONE;
    g_b_fusion_anim_final_atk_bonus = 0;
    g_b_fusion_anim_final_def_bonus = 0;
    for (i = 0; i < I_FIELD; ++i) g_b_fusion_anim_final_equips[i] = CARD_NONE;
    g_b_fusion_anim_final_equip_count = 0;
    g_b_fusion_anim_final_source_index = -1;
    g_b_fusion_anim_target_slot = -1;
    g_b_fusion_anim_has_field_card = 0;
}

static int failed_fusion_can_place_last_card(void)
{
    int target = g_b_fusion_anim_target_slot;
    if (g_b_fusion_anim_count <= 0) return 0;
    if (target < 0 || target >= I_FIELD) return 0;
    return !g_b_fusion_anim_success && is_monster_card(g_b_fusion_anim_final_card);
}

static int fusion_landing_start_frame(void)
{
    return (WAIFU_FUSION_ANIM_FRAMES * 70) / 100;
}

static int fusion_total_frames(void)
{
    return fusion_landing_start_frame() + WAIFU_FUSION_LANDING_FRAMES + WAIFU_FUSION_LANDING_HOLD_FRAMES;
}

static void draw_player_fusion_target(void)
{
    int slot = g_b_top_col;
    int occupied = (slot >= 0 && slot < I_FIELD && is_monster_card(g_i_player_field[slot]));
    int empty_blocked = (!occupied && !player_can_place_monster());
    Camera cam = battle_top_camera();

    draw_interactive_base(cam);
    draw_top_selector_cursor(cam);
    draw_text_small(empty_blocked ? 63 : 60, 191,
                    empty_blocked ? "SELECT OCCUPIED ZONE" : "SELECT FUSION ZONE",
                    IDX_GOLD_HI, IDX_BLACK);
    if (occupied) {
        draw_bottom_info_field(0, slot, "FUSE");
    } else {
        draw_bottom_empty_field(empty_blocked ? "FIELD CARD" : "FUSE");
    }
}

static void fusion_field_card_rect(Camera cam, int target_slot, int *x, int *y, int *w, int *h)
{
    ScreenPt dst = project_point(cam, v3(zone_cx(target_slot), Q8_FRAC(10,100), zone_cz(PLAYER_CARD_ROW)));
    if (!dst.ok) {
        *x = 114;
        *y = 126;
    } else {
        *x = dst.x - 14;
        *y = dst.y - 20;
    }
    *w = 28;
    *h = 39;
}

static void draw_fusion_landing_card(Camera cam, int card_id, int sx, int sy, int sw, int sh,
                                     int target_slot, int local_frame, int face_down)
{
    int dx, dy, dw, dh;
    int32_t t = q8_smooth_ratio(local_frame, WAIFU_FUSION_LANDING_FRAMES);
    int bob = -q8_to_int(q8_mul(Q8_FROM_INT(18), q8_sin_pi(t)));
    int x, y, w, h;
    (void)sw;
    (void)sh;
    fusion_field_card_rect(cam, target_slot, &dx, &dy, &dw, &dh);
    x = lerp_i(sx, dx, t);
    y = lerp_i(sy, dy, t) + bob;
    w = dw;
    h = dh;
    rect_fill(x + 3, y + h - 2, w, 4, IDX_BLACK);
    draw_card_sprite(card_id, x, y, w, h, face_down);
    if (local_frame > (WAIFU_FUSION_LANDING_FRAMES * 7) / 10) {
        rect_outline(x - 2, y - 2, w + 4, h + 4, IDX_GOLD_HI);
        if ((local_frame & 4) == 0) rect_outline(x - 4, y - 4, w + 8, h + 8, IDX_WHITE);
    }
}

static void draw_failed_fusion_dropped_materials(int count, int first_target_x, int local_frame)
{
    int i;
    int32_t fall_t = q8_smooth_ratio(local_frame, WAIFU_FUSION_LANDING_FRAMES);
    for (i = 0; i < count; ++i) {
        int x, y, wobble;
        if (g_b_fusion_anim_material_kept[i]) continue;
        wobble = q8_to_int(q8_mul(Q8_FROM_INT(10), q8_sin_rad((local_frame * 7 + i * 37) * Q8_FRAC(8,100))));
        x = first_target_x + i * 34 + wobble;
        y = lerp_i(82, 258, fall_t) + i * 6;
        if (y < WAIFU_FM_HEIGHT + 52) {
            draw_hand_card_sprite(g_b_fusion_anim_cards[i], x, y, 38, 50, 0);
            if (g_b_fusion_anim_slots[i] == FUSION_FIELD_SLOT) draw_text_small(x + 5, y + 53, "FLD", IDX_GOLD_HI, IDX_BLACK);
        }
    }
}

static void draw_player_fusion_anim(void)
{
    int f = g_b_phase_frame;
    int fusion_merge_end = (WAIFU_FUSION_ANIM_FRAMES * 34) / 100;
    int fusion_flash_start = (WAIFU_FUSION_ANIM_FRAMES * 42) / 100;
    int fusion_flash_end = (WAIFU_FUSION_ANIM_FRAMES * 52) / 100;
    int fusion_reveal_start = fusion_flash_end;
    int fusion_landing_start = fusion_landing_start_frame();
    if (fusion_merge_end < 8) fusion_merge_end = 8;
    if (fusion_flash_end <= fusion_flash_start) fusion_flash_end = fusion_flash_start + 4;
    int32_t merge_t = q8_smooth_ratio(f, fusion_merge_end);
    int32_t reveal_t = q8_smooth_ratio(f - fusion_reveal_start, (WAIFU_FUSION_ANIM_FRAMES * 14) / 100 + 4);
    int cy = lerp_i(WAIFU_HAND_Y_BASE, 74, merge_t);
    int w = 38;
    int h = 50;
    int count = g_b_fusion_anim_count;
    int spread;
    int first_target_x;
    int pulse = (f & 4) ? IDX_GOLD_HI : IDX_WHITE;
    int i;
    if (count < 1) count = 1;
    if (count > FUSION_MAX_MATERIALS) count = FUSION_MAX_MATERIALS;
    spread = count > 1 ? (count - 1) * 34 : 0;
    first_target_x = 128 - spread / 2 - w / 2;

    if (f >= fusion_landing_start) {
        Camera cam = placement_camera();
        int target_slot = g_b_fusion_anim_target_slot;
        int local = f - fusion_landing_start;
        if (!g_b_fusion_anim_success && g_b_fusion_anim_has_field_card &&
            target_slot >= 0 && target_slot < I_FIELD) {
            int saved_field = g_i_player_field[target_slot];
            int saved_faceup = g_i_player_faceup[target_slot];
            int saved_defense = g_i_player_defense[target_slot];
            int saved_attacked = g_i_player_attacked[target_slot];
            int saved_atk_bonus = g_i_player_atk_bonus[target_slot];
            int saved_def_bonus = g_i_player_def_bonus[target_slot];
            g_i_player_field[target_slot] = CARD_NONE;
            g_i_player_faceup[target_slot] = 1;
            g_i_player_defense[target_slot] = 0;
            g_i_player_attacked[target_slot] = 0;
            g_i_player_atk_bonus[target_slot] = 0;
            g_i_player_def_bonus[target_slot] = 0;
            draw_interactive_base(cam);
            g_i_player_field[target_slot] = saved_field;
            g_i_player_faceup[target_slot] = saved_faceup;
            g_i_player_defense[target_slot] = saved_defense;
            g_i_player_attacked[target_slot] = saved_attacked;
            g_i_player_atk_bonus[target_slot] = saved_atk_bonus;
            g_i_player_def_bonus[target_slot] = saved_def_bonus;
        } else {
            draw_interactive_base(cam);
        }
        if (target_slot >= 0 && target_slot < I_FIELD) draw_zone_cursor(cam, target_slot, PLAYER_CARD_ROW);
        if (g_b_fusion_anim_success) {
            draw_fusion_landing_card(cam, g_b_fusion_anim_result, 101, 74, 54, 72, target_slot, local, 0);
            draw_centered_text(WAIFU_UI_BOTTOM_Y(191), "FUSION SUCCESS", IDX_GREEN, IDX_BLACK);
            draw_centered_text(WAIFU_UI_BOTTOM_Y(205), "PLACING RESULT", IDX_WHITE, IDX_BLACK);
        } else if (failed_fusion_can_place_last_card()) {
            int final_index = g_b_fusion_anim_final_source_index;
            int final_x = (final_index >= 0) ? first_target_x + final_index * 34 : 101;
            draw_failed_fusion_dropped_materials(count, first_target_x, local);
            draw_fusion_landing_card(cam, g_b_fusion_anim_final_card, final_x, 82, 38, 50, target_slot, local, 0);
            if (g_b_fusion_anim_equip_only) {
                draw_centered_text(WAIFU_UI_BOTTOM_Y(191), "EQUIP APPLIED", IDX_GREEN, IDX_BLACK);
                draw_centered_text(WAIFU_UI_BOTTOM_Y(205), "PLACING CARD", IDX_WHITE, IDX_BLACK);
            } else {
                draw_centered_text(WAIFU_UI_BOTTOM_Y(191), "FUSION FAILED", IDX_RED, IDX_BLACK);
                draw_centered_text(WAIFU_UI_BOTTOM_Y(205), g_b_fusion_anim_final_equip_count > 0 ? "EQUIP APPLIED" : "LAST CARD PLACED", IDX_WHITE, IDX_BLACK);
            }
        } else {
            int32_t fall_t = q8_smooth_ratio(local, WAIFU_FUSION_LANDING_FRAMES);
            for (i = 0; i < count; ++i) {
                int x = first_target_x + i * 34;
                int y = lerp_i(82, 258, fall_t) + i * 6;
                if (y < WAIFU_FM_HEIGHT + 52) draw_hand_card_sprite(g_b_fusion_anim_cards[i], x, y, 38, 50, 0);
                if (g_b_fusion_anim_slots[i] == FUSION_FIELD_SLOT && y < WAIFU_FM_HEIGHT + 52) draw_text_small(x + 5, y + 53, "FLD", IDX_GOLD_HI, IDX_BLACK);
            }
            draw_centered_text(WAIFU_UI_BOTTOM_Y(191), "FUSION FAILED", IDX_RED, IDX_BLACK);
            draw_centered_text(WAIFU_UI_BOTTOM_Y(205), "CARDS DISCARDED", IDX_WHITE, IDX_BLACK);
        }
        return;
    }

    clear_screen(IDX_BLACK);
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) hline(0, WAIFU_FM_WIDTH - 1, y, (y & 8) ? IDX_UI_DARK : IDX_BLACK);
    draw_panel_rect(20, 26, 216, 178, IDX_UI_DARK);
    draw_centered_text(39, "FUSION", IDX_GOLD_HI, IDX_BLACK);

    if (f < fusion_flash_start) {
        for (i = 0; i < count; ++i) {
            int slot = g_b_fusion_anim_slots[i];
            int sx = slot >= 0 ? hand_final_x(slot) : hand_final_x(i);
            int tx = first_target_x + i * 34;
            int x = lerp_i(sx, tx, merge_t);
            draw_hand_card_sprite(g_b_fusion_anim_cards[i], x, cy, w, h, 0);
            if (slot == FUSION_FIELD_SLOT) draw_text_small(x + 5, cy + h + 3, "FLD", IDX_GOLD_HI, IDX_BLACK);
        }
        for (i = 0; i < 18; ++i) {
            int px = 128 + q8_to_int(q8_mul(q8_sin_rad((f * 5 + i * 29) * Q8_FRAC(8,100)), Q8_FROM_INT(44)));
            int py = 103 + q8_to_int(q8_mul(q8_cos_rad((f * 7 + i * 31) * Q8_FRAC(8,100)), Q8_FROM_INT(22)));
            draw_disc(px, py, 1 + (i % 2), (i & 1) ? IDX_GREEN : IDX_GOLD_HI);
        }
    } else if (f < fusion_flash_end) {
        rect_fill(0, 0, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, (f & 2) ? IDX_WHITE : IDX_GOLD_HI);
    } else if (g_b_fusion_anim_success) {
        int rw = 38;
        int rh = 50;
        int rx = 128 - rw / 2;
        int ry = lerp_i(72, 82, reveal_t);
        draw_hand_card_sprite(g_b_fusion_anim_result, rx, ry, rw, rh, 0);
        if ((f & 4) == 0) rect_outline(rx - 4, ry - 4, rw + 8, rh + 8, pulse);
        draw_centered_text(166, "FUSION SUCCESS", IDX_GREEN, IDX_BLACK);
    } else if (g_b_fusion_anim_equip_only) {
        int ly = 82;
        for (i = 0; i < count; ++i) {
            int x = first_target_x + i * 34;
            draw_hand_card_sprite(g_b_fusion_anim_cards[i], x, ly, 38, 50, 0);
        }
        draw_centered_text(166, "EQUIP APPLIED", IDX_GREEN, IDX_BLACK);
        draw_centered_text(181, "PLACING CARD", IDX_WHITE, IDX_BLACK);
    } else {
        int ly = 82;
        for (i = 0; i < count; ++i) {
            int x = first_target_x + i * 34;
            draw_hand_card_sprite(g_b_fusion_anim_cards[i], x, ly, 38, 50, 0);
            if (g_b_fusion_anim_slots[i] == FUSION_FIELD_SLOT) draw_text_small(x + 5, ly + 53, "FLD", IDX_GOLD_HI, IDX_BLACK);
        }
        line_i(92, 85, 164, 132, IDX_RED);
        line_i(164, 85, 92, 132, IDX_RED);
        line_i(93, 85, 165, 132, IDX_BLACK);
        line_i(165, 85, 93, 132, IDX_BLACK);
        draw_centered_text(166, "FUSION FAILED", IDX_RED, IDX_BLACK);
        draw_centered_text(181, failed_fusion_can_place_last_card() ? "LAST CARD PLACED" : "CARDS DISCARDED", IDX_WHITE, IDX_BLACK);
    }
}


static void attach_player_fusion_final_equips(int target_slot)
{
    int i;
    g_i_player_atk_bonus[target_slot] = 0;
    g_i_player_def_bonus[target_slot] = 0;
    for (i = 0; i < g_b_fusion_anim_final_equip_count; ++i) {
        int equip_card = g_b_fusion_anim_final_equips[i];
        int equip_slot = first_free_player_equip_slot();
        if (!fusion_material_is_equip(equip_card) || equip_slot < 0) continue;
        g_i_player_equip_field[equip_slot] = equip_card;
        g_i_player_equip_target[equip_slot] = target_slot;
        g_i_player_atk_bonus[target_slot] += equip_atk_bonus(equip_card);
        g_i_player_def_bonus[target_slot] += equip_def_bonus(equip_card);
    }
}

static void finish_player_fusion_anim(void)
{
    int i;
    int target_slot = g_b_fusion_anim_target_slot;
    int placed_slot = -1;
    int used_hand_count = 0;
    int discarded_material = 0;
    clear_player_fusion_queue();

    if (g_b_fusion_anim_count > 0) {
        if (target_slot >= 0 && target_slot < I_FIELD && is_monster_card(g_b_fusion_anim_final_card)) {
            clear_monster_slot(0, target_slot);
            placed_slot = target_slot;
            g_i_player_field[placed_slot] = g_b_fusion_anim_final_card;
            g_i_player_faceup[placed_slot] = 1;
            g_i_player_defense[placed_slot] = 0;
            g_i_player_attacked[placed_slot] = 0;
            attach_player_fusion_final_equips(placed_slot);
            g_b_player_monster_played_this_turn = 1;
            g_b_selected_player_slot = placed_slot;
            set_top_selector(placed_slot, PLAYER_CARD_ROW);
        } else if (g_b_fusion_anim_has_field_card && target_slot >= 0 && target_slot < I_FIELD) {
            clear_monster_slot(0, target_slot);
            placed_slot = target_slot;
            g_b_selected_player_slot = target_slot;
            set_top_selector(target_slot, PLAYER_CARD_ROW);
        }
        for (i = 0; i < g_b_fusion_anim_count; ++i) {
            int slot = g_b_fusion_anim_slots[i];
            if (!g_b_fusion_anim_material_kept[i]) discarded_material = 1;
            if (slot >= 0 && slot < I_HAND) {
                g_i_player_used[slot] = 1;
                ++used_hand_count;
            }
        }
        g_b_selected_hand = next_live_hand_index(g_b_selected_hand, 1);
        g_b_cards_used += used_hand_count;
        g_b_player_fused_this_turn = 1;
        g_b_player_monster_played_this_turn = 1;
        if (discarded_material) waifu_sound_play(WAIFU_SOUND_CARD_DESTROYED);
        if (placed_slot >= 0) waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
    }

    reset_player_fusion_anim();
    set_battle_phase(placed_slot >= 0 ? IB_PLAYER_TOP : IB_PLAYER_HAND);
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
        if (press_down && player_can_start_fusion()) {
            try_queue_player_fusion_slot(g_b_selected_hand);
            g_b_player_hand_intro_pending = 0;
        }
        if (press_b) {
#if defined(WAIFU_FM_CD32X)
            g_b_preview_art_pending = CD32X_CARD_CHECK_LOAD_PENDING;
#endif
            set_battle_phase(IB_CARD_PREVIEW);
            break;
        }
        if (press_up) { clear_player_fusion_queue(); set_top_selector(g_b_selected_player_slot, PLAYER_CARD_ROW); g_b_attack_attacker_slot = -1; g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND_TO_TOP); break; }
        if (press_a && g_b_fusion_count > 0 && player_can_start_fusion()) {
            int target = player_can_place_monster() ? first_free_player_slot() : -1;
            if (target < 0) target = selected_or_first_live_player_slot();
            if (target < 0) target = 0;
            g_b_fusion_target_slot = target;
            g_b_player_hand_intro_pending = 0;
            set_top_selector(target, PLAYER_CARD_ROW);
            set_battle_phase(IB_PLAYER_FUSION_TARGET);
            break;
        }
        if (press_a && !g_i_player_used[g_b_selected_hand]) {
            int selected_card = g_i_player_hand[g_b_selected_hand];
            if (is_support_card(selected_card)) {
                int target = selected_or_first_live_player_slot();
                if (is_thunder_support_card(selected_card)) {
                    if (count_live_com_monsters() > 0) {
                        clear_player_fusion_queue();
                        g_b_player_hand_intro_pending = 0;
                        start_player_thunder(g_b_selected_hand);
                    }
                } else if (is_draw_support_card(selected_card)) {
                    if (g_i_player_deck_left > 0) {
                        g_b_player_hand_intro_pending = 0;
                        start_player_one_shot_support(g_b_selected_hand);
                    }
                } else if (is_heal_support_card(selected_card)) {
                    g_b_player_hand_intro_pending = 0;
                    start_player_one_shot_support(g_b_selected_hand);
                } else if (is_equip_support_card(selected_card) && target >= 0 && first_free_player_equip_slot() >= 0) {
                    clear_player_fusion_queue();
                    g_b_equip_hand = g_b_selected_hand;
                    g_b_selected_player_slot = target;
                    set_top_selector(target, PLAYER_CARD_ROW);
                    set_battle_phase(IB_PLAYER_EQUIP_TARGET);
                } else if (is_trap_support_card(selected_card)) {
                    /* Set the Trap face-down on the support (equip) row; it
                       auto-fires when a COM monster declares an attack.  Run the
                       same flying-card placement animation as a monster summon
                       (back=2 flips it face-down on landing) instead of snapping
                       it straight onto the field. */
                    int free_slot = first_free_player_equip_slot();
                    if (free_slot >= 0) {
                        clear_player_fusion_queue();
                        g_b_place_hand = g_b_selected_hand;
                        g_b_place_slot = free_slot;
                        g_b_place_card = selected_card;
                        g_b_place_defense = 0;
                        g_b_place_trap = 1;
                        g_b_player_hand_intro_pending = 0;
                        set_battle_phase(IB_PLAYER_PLACE);
                    }
                }
                break;
            }
            slot = first_free_player_slot();
            if (slot >= 0 && player_can_place_monster()) {
                clear_player_fusion_queue();
                g_b_place_hand = g_b_selected_hand;
                g_b_place_slot = slot;
                g_b_place_card = selected_card;
                g_b_place_defense = 0;
                g_b_place_trap = 0;
                set_battle_phase(IB_PLAYER_PLACE);
                break;
            }
        }
        if (press_start) { clear_player_fusion_queue(); g_b_attack_attacker_slot = -1; clear_com_attacks(); g_b_com_monster_played_this_turn = 0; set_battle_phase(IB_TURN_TO_COM); break; }
#if defined(WAIFU_FM_PCFX)
        prewarm_handtop_transition_bases();
#endif
        play_player_hand_intro_draw_sfx();
        draw_interactive_base(player_camera());
        draw_interactive_player_hand(g_b_player_hand_intro_pending ? g_b_phase_frame : 999, g_b_selected_hand, 0, 0);
        draw_bottom_info(g_i_player_hand[g_b_selected_hand], "HAND");
        if (g_b_phase_frame >= 48) g_b_player_hand_intro_pending = 0;
        break;

    case IB_PLAYER_HAND_TO_TOP: {
        /* PC-FX transition fast path without the hard cut: use cached 3D camera
           keyframes along the hand->top lift.  The board changes through real
           intermediate perspectives, while the hand overlay slides every logic
           frame.  This avoids the original cache-miss storm without snapping
           immediately to the top camera. */
        int dur = WAIFU_PCFX_HANDTOP_FRAMES;
        int hand_off = q8_to_int(q8_mul(Q8_FROM_INT(118), q8_smooth_ratio(g_b_phase_frame, dur)));
        draw_player_handtop_transition_shared(g_b_phase_frame, dur, 1, hand_off, 1, 1);
        if (battle_animation_event_complete(dur)) set_battle_phase(IB_PLAYER_TOP);
        break;
    }

    case IB_PLAYER_TOP_TO_HAND: {
        /* Reverse path uses the same cached camera keyframes in reverse order,
           so returning to hand is also smooth without re-rendering a unique
           3D board for every logic frame. */
        int dur = WAIFU_PCFX_HANDTOP_FRAMES;
        int hand_off = q8_to_int(q8_mul(Q8_FROM_INT(118), Q8_ONE - q8_smooth_ratio(g_b_phase_frame, dur)));
        draw_player_handtop_transition_shared(g_b_phase_frame, dur, 0, hand_off, 1, 1);
        if (battle_animation_event_complete(dur)) set_battle_phase(IB_PLAYER_HAND);
        break;
    }

    case IB_CARD_PREVIEW:
#if defined(WAIFU_FM_CD32X)
        if (cd32x_step_card_check_transition(g_i_player_hand[g_b_selected_hand], &g_b_preview_art_pending, &g_b_phase_frame, 0)) break;
#endif
        draw_interactive_card_preview(g_i_player_hand[g_b_selected_hand], g_b_phase_frame);
        if (press_b || press_a || press_start) {
#if defined(WAIFU_FM_CD32X)
            g_b_preview_art_pending = 0;
#endif
            g_b_player_hand_intro_pending = 0;
            set_battle_phase(IB_PLAYER_HAND);
        }
        break;

    case IB_FIELD_CARD_PREVIEW:
#if defined(WAIFU_FM_CD32X)
        if (cd32x_step_card_check_transition(g_b_preview_card_id, &g_b_preview_art_pending, &g_b_phase_frame, 0)) break;
#endif
        draw_interactive_card_preview(g_b_preview_card_id, g_b_phase_frame);
        if (press_b || press_a || press_start) {
#if defined(WAIFU_FM_CD32X)
            g_b_preview_art_pending = 0;
#endif
            set_battle_phase(IB_PLAYER_TOP);
        }
        break;

    case IB_PLAYER_PLACE: {
        int place_row = g_b_place_trap ? (PLAYER_CARD_ROW + 1) : PLAYER_CARD_ROW;
        draw_interactive_base(placement_camera());
        draw_zone_cursor(placement_camera(), g_b_place_slot, place_row);
        draw_flying_card(placement_camera(), g_b_place_card, g_b_place_hand, g_b_place_slot, place_row, g_b_phase_frame, 0, WAIFU_PCFX_PLACE_FRAMES, 2);
        draw_interactive_player_hand(999, g_b_place_hand, q8_to_int(q8_mul(Q8_FROM_INT(92), q8_smooth_ratio(g_b_phase_frame, WAIFU_PCFX_PLACE_SETTLE_FRAMES))), 1);
        draw_bottom_info(g_b_place_card, g_b_place_trap ? "SET" : "PLACE");
        if (battle_animation_event_complete(WAIFU_PCFX_PLACE_FRAMES)) {
            if (g_b_place_trap) {
                g_i_player_equip_field[g_b_place_slot] = g_b_place_card;
                g_i_player_equip_target[g_b_place_slot] = -1;
                g_i_player_used[g_b_place_hand] = 1;
                g_b_cards_used++;
                g_b_place_trap = 0;
                waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
                set_battle_phase(IB_PLAYER_HAND);
                break;
            }
            if (!is_monster_card(g_b_place_card)) { g_i_player_used[g_b_place_hand] = 1; set_battle_phase(IB_PLAYER_HAND); break; }
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
            waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
            set_battle_phase(IB_PLAYER_TOP);
        }
        break;
    }

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
        if (battle_animation_event_complete(WAIFU_EQUIP_ANIM_FRAMES)) finish_equip();
        break;

    case IB_PLAYER_FUSION_TARGET:
        if (press_b) { g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND); break; }
        if (press_left) move_top_selector(-1, 0);
        if (press_right) move_top_selector(1, 0);
        if (g_b_top_row != PLAYER_CARD_ROW) set_top_selector(g_b_top_col, PLAYER_CARD_ROW);
        g_b_fusion_target_slot = g_b_top_col;
        if (press_a) {
            int target = g_b_top_col;
            if (player_can_fusion_to_slot(target)) {
                if (prepare_player_fusion_anim(target)) {
                    set_battle_phase(IB_PLAYER_FUSION_ANIM);
                    break;
                }
                clear_player_fusion_queue();
                set_battle_phase(IB_PLAYER_HAND);
                break;
            }
        }
        draw_player_fusion_target();
        break;

    case IB_PLAYER_FUSION_ANIM:
        draw_player_fusion_anim();
        if (battle_animation_event_complete(fusion_total_frames())) finish_player_fusion_anim();
        break;

    case IB_PLAYER_SUPPORT_ANIM:
        draw_player_one_shot_support_anim();
        if (battle_animation_event_complete(player_support_total_frames())) finish_player_one_shot_support();
        break;

    case IB_PLAYER_TOP:
        if (press_left) move_top_selector(-1, 0);
        if (press_right) move_top_selector(1, 0);
        if (press_up) move_top_selector(0, -1);
        if (press_down) {
            if (g_b_top_row < BOARD_ROWS - 1) move_top_selector(0, 1);
            else { g_b_player_hand_intro_pending = 0; g_b_attack_attacker_slot = -1; set_battle_phase(IB_PLAYER_TOP_TO_HAND); break; }
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
#if defined(WAIFU_FM_CD32X)
                    g_b_preview_art_pending = CD32X_CARD_CHECK_LOAD_PENDING;
#endif
                    set_battle_phase(IB_FIELD_CARD_PREVIEW);
                    break;
                }
            }
        }
        atk_slot = top_selector_player_monster_slot();
        def_slot = top_selector_com_monster_slot();
        if (g_b_attack_attacker_slot >= 0) {
            if (!is_monster_card(g_i_player_field[g_b_attack_attacker_slot]) || g_i_player_attacked[g_b_attack_attacker_slot] || g_i_player_defense[g_b_attack_attacker_slot]) {
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
        } else if (press_a && atk_slot >= 0 && is_monster_card(g_i_player_field[atk_slot]) && !g_i_player_attacked[atk_slot] && !g_i_player_defense[atk_slot] && !player_first_turn_attack_locked()) {
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
        if (battle_animation_event_complete(current_battle_anim_frames())) {
            resolve_battle();
            if (g_b_phase == IB_PLAYER_BATTLE) set_battle_phase(IB_PLAYER_RETURN_TOP);
        }
        break;

    case IB_PLAYER_RETURN_TOP:
        atk_slot = selected_or_first_live_player_slot();
        draw_post_battle_return(0, g_b_phase_frame, atk_slot,
                                atk_slot >= 0 ? g_i_player_field[atk_slot] : hand_ids[0],
                                atk_slot >= 0 && g_i_player_attacked[atk_slot] ? "USED" : "FIELD",
                                1);
        if (battle_animation_event_complete(WAIFU_PCFX_RETURN_FRAMES)) {
            if (atk_slot >= 0) set_top_selector(atk_slot, PLAYER_CARD_ROW);
            g_b_attack_attacker_slot = -1;
            set_battle_phase(IB_PLAYER_TOP);
        }
        break;

    case IB_TURN_TO_COM:
        draw_interactive_base(turn_camera(g_b_phase_frame, 0, WAIFU_PCFX_TURN_FRAMES, 1));
        if (battle_animation_event_complete(WAIFU_PCFX_TURN_FRAMES)) {
            draw_replacement_cards_to_com_hand();
            g_b_selected_com_slot = 0;
            g_b_com_return_fade = 0;
            set_battle_phase(IB_COM_SELECT);
        }
        break;

    case IB_COM_SELECT:
        draw_interactive_common(enemy_camera(), first_live_com_slot() >= 0 ? g_i_com_field[first_live_com_slot()] : g_i_com_hand[0], "COM");
        g_b_selected_com_slot = (g_b_phase_frame / 4) % I_HAND;
        draw_interactive_com_hand(g_b_phase_frame, g_b_selected_com_slot, 0);
        if (battle_animation_event_complete(WAIFU_PCFX_SELECT_FRAMES)) {
            WaifuAiState ai_state;
            WaifuAiAction ai_action;
            build_com_ai_state(&ai_state);
            ai_action = waifu_ai_choose_com_select(&ai_state);
            if (ai_action.kind == WAIFU_AI_ACTION_PLAY_SUPPORT) {
                int support_card = (ai_action.hand_slot >= 0 && ai_action.hand_slot < I_HAND) ? g_i_com_hand[ai_action.hand_slot] : CARD_NONE;
                if (is_thunder_support_card(support_card)) start_com_thunder(ai_action.hand_slot);
                else start_com_equip(ai_action.hand_slot, ai_action.field_slot);
            } else if (ai_action.kind == WAIFU_AI_ACTION_PLACE_MONSTER) {
                g_b_place_hand = ai_action.hand_slot;
                g_b_place_slot = ai_action.field_slot;
                g_b_place_card = g_i_com_hand[ai_action.hand_slot];
                g_b_place_defense = ai_action.defense_position;
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
        draw_flying_card(enemy_placement_camera(), g_b_place_card, g_b_place_hand, g_b_place_slot, ENEMY_CARD_ROW, g_b_phase_frame, 0, WAIFU_PCFX_PLACE_FRAMES, 1);
        draw_interactive_com_hand(999, g_b_place_hand, q8_to_int(q8_mul(Q8_FROM_INT(82), q8_smooth_ratio(g_b_phase_frame, WAIFU_PCFX_PLACE_SETTLE_FRAMES))));
        if (battle_animation_event_complete(WAIFU_PCFX_PLACE_FRAMES)) {
            if (!is_monster_card(g_b_place_card)) { clear_battle_snapshot(); set_battle_phase(IB_COM_BATTLE); break; }
            g_i_com_field[g_b_place_slot] = g_b_place_card;
            g_i_com_faceup[g_b_place_slot] = 0;
            g_i_com_defense[g_b_place_slot] = g_b_place_defense ? 1 : 0;
            g_i_com_atk_bonus[g_b_place_slot] = 0;
            g_i_com_def_bonus[g_b_place_slot] = 0;
            g_i_com_used[g_b_place_hand] = 1;
            g_b_com_monster_played_this_turn = 1;
            waifu_sound_play(WAIFU_SOUND_CARD_PLACED);
            clear_battle_snapshot();
            {
                int equip_h = first_unused_com_support_hand();
                if (equip_h >= 0 && first_free_com_equip_slot() >= 0) {
                    /* Don't snap straight into the equip animation: let the
                       just-landed monster show on the field, then have COM
                       visually pick the equip card first (IB_COM_EQUIP_SELECT). */
                    g_b_com_equip_pending_hand = equip_h;
                    set_battle_phase(IB_COM_EQUIP_SELECT);
                } else {
                    set_battle_phase(IB_COM_BATTLE);
                }
            }
        }
        break;

    case IB_COM_EQUIP_SELECT: {
        int equip_h = g_b_com_equip_pending_hand;
        int settle = WAIFU_PCFX_SELECT_FRAMES - (WAIFU_PCFX_SELECT_FRAMES / 4);
        int sel = (g_b_phase_frame < settle) ? (g_b_phase_frame / 4) % I_HAND : equip_h;
        draw_interactive_common(enemy_camera(),
                                g_i_com_field[g_b_place_slot] >= 0 ? g_i_com_field[g_b_place_slot] : g_i_com_hand[0],
                                "COM");
        draw_interactive_com_hand(g_b_phase_frame, sel, 0);
        if (battle_animation_event_complete(WAIFU_PCFX_SELECT_FRAMES)) {
            g_b_com_equip_pending_hand = -1;
            if (equip_h >= 0 && first_free_com_equip_slot() >= 0 &&
                is_support_card(g_i_com_hand[equip_h])) {
                start_com_equip(equip_h, g_b_place_slot);
            } else {
                set_battle_phase(IB_COM_BATTLE);
            }
        }
        break;
    }

    case IB_COM_EQUIP_ANIM:
        draw_player_equip_anim();
        if (battle_animation_event_complete(WAIFU_EQUIP_ANIM_FRAMES)) finish_equip();
        break;

    case IB_COM_THUNDER_ANIM:
        draw_com_thunder_anim();
        if (battle_animation_event_complete(thunder_total_frames())) finish_thunder();
        break;

    case IB_COM_BATTLE:
        if (g_b_battle_atk_card < 0) {
            WaifuAiState ai_state;
            WaifuAiAction ai_action;
            int set_guard = 0;
            /* Position switches (ATK<->DEF) are pure bookkeeping. Resolve any of
               them silently within this frame instead of drawing a dedicated
               top-down beat: a one-frame cut to enemy_battle_top_camera() looked
               like an unwanted screen transition whenever COM rotated a monster
               to defense. Keep consulting the AI until it wants to attack or end
               the turn; the new position shows up on the normal turn-end view. */
            for (;;) {
                build_com_ai_state(&ai_state);
                ai_action = waifu_ai_choose_com_battle(&ai_state);
                if (ai_action.kind == WAIFU_AI_ACTION_SET_DEFENSE) {
                    if (ai_action.field_slot >= 0 && ai_action.field_slot < I_FIELD) {
                        g_i_com_defense[ai_action.field_slot] = 1;
                        g_b_selected_com_slot = ai_action.field_slot;
                    }
                } else if (ai_action.kind == WAIFU_AI_ACTION_SET_ATTACK) {
                    if (ai_action.field_slot >= 0 && ai_action.field_slot < I_FIELD) {
                        g_i_com_defense[ai_action.field_slot] = 0;
                        g_b_selected_com_slot = ai_action.field_slot;
                    }
                } else {
                    break;
                }
                if (++set_guard >= I_FIELD * 2) break;
            }
            if (ai_action.kind == WAIFU_AI_ACTION_ATTACK_MONSTER) {
                g_b_com_return_fade = 1;
                prepare_battle(1, ai_action.attacker_slot, ai_action.defender_slot);
            } else if (ai_action.kind == WAIFU_AI_ACTION_ATTACK_DIRECT) {
                g_b_com_return_fade = 1;
                prepare_direct_attack(1, ai_action.attacker_slot);
            } else {
                set_battle_phase(IB_COM_RETURN);
                break;
            }
        }
        draw_interactive_battle();
        if (battle_animation_event_complete(current_battle_anim_frames())) {
            resolve_battle();
            if (g_b_phase == IB_COM_BATTLE) {
                clear_battle_snapshot();
                set_battle_phase(IB_COM_BATTLE);
            }
        }
        break;

    case IB_COM_RETURN:
        atk_slot = first_live_com_slot();
        draw_post_battle_return(1, g_b_phase_frame, atk_slot,
                                atk_slot >= 0 ? g_i_com_field[atk_slot] : hand_ids[0],
                                "COM",
                                g_b_com_return_fade);
        if (battle_animation_event_complete(WAIFU_PCFX_RETURN_FRAMES)) {
            g_b_com_return_fade = 0;
            clear_player_attacks();
            g_b_player_monster_played_this_turn = 0;
            g_b_player_fused_this_turn = 0;
            g_b_turns++;
            set_battle_phase(IB_TURN_TO_PLAYER);
        }
        break;

    case IB_TURN_TO_PLAYER:
        draw_interactive_base(turn_camera(g_b_phase_frame, 0, WAIFU_PCFX_TURN_FRAMES, 0));
        {
            int yoff = 0;
            if (g_b_phase_frame < WAIFU_PCFX_TURN_FRAMES / 2) {
                yoff = 44;
            } else if (g_b_phase_frame < WAIFU_PCFX_TURN_FRAMES) {
                int t = g_b_phase_frame - WAIFU_PCFX_TURN_FRAMES / 2;
                yoff = q8_to_int(q8_mul(Q8_FROM_INT(44), Q8_ONE - q8_smooth_ratio(t, WAIFU_PCFX_TURN_FRAMES / 2)));
            }
            if (g_b_phase_frame >= WAIFU_PCFX_TURN_FRAMES / 2) {
                int card = first_live_com_slot() >= 0 ? g_i_com_field[first_live_com_slot()] : hand_ids[0];
                draw_bottom_info_offset(card, "TURN", yoff);
            }
        }
        if (battle_animation_event_complete(WAIFU_PCFX_TURN_FRAMES)) {
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
        play_turn_draw_sfx(g_b_phase_frame);
        draw_interactive_base(player_camera());
        draw_player_hand_turn_draw(g_b_phase_frame, g_b_selected_hand);
        if (g_b_phase_frame < WAIFU_PCFX_DRAW_FRAMES && g_b_draw_count > 0) draw_text_small(211, 142, "DRAW", IDX_GOLD_HI, IDX_BLACK);
        draw_bottom_info(g_i_player_hand[g_b_selected_hand], "DRAW");
        if (battle_animation_event_complete(WAIFU_PCFX_DRAW_FRAMES)) { g_b_player_hand_intro_pending = 0; set_battle_phase(IB_PLAYER_HAND); }
        break;

    case IB_RESULT:
        draw_interactive_result();
        if (g_b_phase_frame >= WAIFU_RESULT_TOTAL_FRAMES) set_battle_phase(IB_TALLY);
        break;

    case IB_TALLY:
        /* Roll the rank-scaled story reward once, here, so the reveal screen and
           the storage award (committed in story_return_to_map_after_duel) agree. */
        if (g_story_battle_active && g_b_result >= 0 && g_b_reward_card == CARD_NONE) {
            g_b_reward_card = story_reward_drop_card_ranked(current_battle_rank());
        }
        draw_interactive_tally();
        if (press_start || press_a) {
            if (g_story_battle_active && g_b_result >= 0) {
                set_battle_phase(IB_REWARD);   /* show the earned card first */
            } else if (g_story_battle_active) {
                story_return_to_map_after_duel(); /* loss: no reward */
            } else {
                init_battle_state();
                enter_title_after_assets();
            }
        }
        break;

    case IB_REWARD:
        draw_interactive_reward();
        if (press_start || press_a) story_return_to_map_after_duel();
        break;
    }

    g_b_frame++;
    g_b_phase_frame++;
}

void waifu_fm_init(void)
{
    if (g_api_initialized) return;
    initDivs();
    CfxRenderer3DConfig cfg = { framebuffer, (DEFAULT_INT)WAIFU_FM_WIDTH, (DEFAULT_INT)WAIFU_FM_HEIGHT };
    cfx_renderer3d_init(&renderer, &cfg);
    cfx_renderer3d_set_texture_atlas(&renderer, waifu_texture_atlas,
                                      WAIFU_TEX_TILE_SIZE,
                                      WAIFU_TEX_TILE_SIZE * WAIFU_TEX_TILE_SIZE);
    invalidate_board_bg_cache();
    invalidate_battle_composite_cache();
    waifu_assets_init();
#ifdef CD32X_DEBUG_AUTOBATTLE
    /* Temporary CD32X iteration shortcut: boot straight to the deck editor
       through the normal card loading screen. */
    enter_debug_deck_editor_after_assets();
#else
    waifu_assets_request_title();
    if (waifu_assets_needs_loading_screen()) {
        g_i_loading_target = WAIFU_I_TITLE;
        g_i_state = WAIFU_I_LOADING_ASSETS;
        g_i_frame = 0;
    }
#endif
    waifu_fm_use_common_palette();
    waifu_sound_init();
    update_music_for_current_state();
    g_api_initialized = 1;
}

void waifu_fm_reset_interactive(void)
{
    waifu_assets_reset();
    g_i_menu_selected = 0;
    memset(&g_prev_input, 0, sizeof(g_prev_input));
    waifu_sound_reset();
    init_battle_state();
    invalidate_board_bg_cache();
    invalidate_battle_composite_cache();
#ifdef CD32X_DEBUG_AUTOBATTLE
    /* Throwaway CD32X iteration shortcut: skip title/menu asset requests and
       boot straight into the deck editor through the normal card-loading path.
       Build with EXTRA_CFLAGS=-DCD32X_DEBUG_AUTOBATTLE. */
    enter_debug_deck_editor_after_assets();
#else
    waifu_assets_request_title();
    g_i_loading_target = WAIFU_I_TITLE;
    g_i_state = waifu_assets_needs_loading_screen() ? WAIFU_I_LOADING_ASSETS : WAIFU_I_TITLE;
    g_i_frame = 0;
#endif
    waifu_fm_use_common_palette();
    update_music_for_current_state();
}

uint8_t *waifu_fm_framebuffer(void)
{
    return framebuffer;
}

uint32_t waifu_fm_frame_dirty_serial(void)
{
    return g_frame_dirty_serial;
}

int waifu_fm_frame_dirty_full(void)
{
    return g_frame_dirty_full;
}

int waifu_fm_frame_dirty_rects(WaifuFmDirtyRect *out_rects, int max_rects)
{
    int n = g_frame_dirty_count;
    if (!out_rects || max_rects <= 0) return n;
    if (n > max_rects) n = max_rects;
    for (int i = 0; i < n; ++i) out_rects[i] = g_frame_dirty_rects[i];
    return n;
}

int waifu_fm_video_fade_q8(void)
{
    return g_video_fade_visible_q8;
}

int waifu_fm_audio_sample_rate(void)
{
    return waifu_sound_sample_rate();
}

int waifu_fm_audio_channels(void)
{
    return waifu_sound_channels();
}

int waifu_fm_audio_samples_per_frame(void)
{
    return waifu_sound_samples_per_frame();
}

void waifu_fm_audio_mix_s16(int16_t *dst, int frames)
{
    waifu_sound_mix_s16(dst, frames);
}

WaifuFmMusicTrack waifu_fm_audio_music_track(void)
{
    return (WaifuFmMusicTrack)waifu_sound_music_track();
}

const char *waifu_fm_audio_music_name(void)
{
    return waifu_sound_music_name(waifu_sound_music_track());
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
    g_story_progress = 0;
    g_story_map_cursor = 0;
    g_story_pyramid_cursor = 0;
    g_story_plaza_line = 0;
    g_story_ending_line = 0;
    g_story_ending_erasing = 0;
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
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        uint8_t c = (y & 8) ? IDX_DARK_BROWN : IDX_BLACK;
        if (y < 36 || y > WAIFU_UI_BOTTOM_Y(204)) hline(0, WAIFU_FM_WIDTH - 1, y, c);
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

    waifu_str_copy(buf, (int)sizeof(buf), "LETTER "); waifu_str_cat_char(buf, (int)sizeof(buf), g_story_name[g_story_name_pos]);
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
    int px;
    if (line < 0) line = 0;
    if (line >= line_count) line = line_count - 1;

    waifu_fm_use_dialogue_palette();
    clear_screen(IDX_BLACK);
    if ((f & 32) == 0) {
        for (int i = 0; i < 24; ++i) put_px(116 + ((i * 17 + f) & 23), 38 + ((i * 11) & 31), IDX_DIM);
    }
    px = story_slide_x(-WAIFU_STORY_PORTRAIT_W - 10, 10, f);
    draw_story_portrait(STORY_PORTRAIT_SERENA, px, WAIFU_FM_HEIGHT - WAIFU_STORY_PORTRAIT_H - 20);
    draw_story_dialog_box(g_story_name, "THE SHARDS WHISPER", story_intro_lines[line], IDX_GOLD_HI, f);
    if (f >= 0 && f < 24) apply_black_dither_fade(q8_ratio(f, 24));
}


static const char *story_fire_lines[] = {
    "SERENA... THE DREAM HAS TEETH.",
    "EIGHT GUARDIANS STAND BETWEEN YOU AND THE TRUTH.",
    "BEAT THEM ALL, AND I WILL WAIT FOR YOU IN THE VOID.",
    "YOU ARE GOING TO BURN IN HELL."
};

/* Propagation ("doom") fire.  The previous fire evaluated two q8_sin_rad plus
   several q8 muls for every one of ~43k pixels each frame, which is what made the
   story fire screen sluggish on V810.  This version keeps a half-resolution
   intensity buffer and advances it with a few integer ops per cell (xorshift RNG,
   subtract a small random decay, pull from a random neighbour below), then blits
   it 2x to the framebuffer through a colour LUT.  Cheaper and looks more like
   real flames.  (The present path already page-flips; a full-screen animation
   still needs a full KRAM upload each frame, which is unavoidable.) */
#define FIRE_Y0 40
#define FIRE_FW (WAIFU_FM_WIDTH / 2)
#define FIRE_FH ((WAIFU_FM_HEIGHT - FIRE_Y0) / 2)
#define FIRE_MAXI 32
static uint8_t g_fire_buf[FIRE_FW * FIRE_FH];
static uint32_t g_fire_rng = 0x2545f491u;

static const uint8_t g_fire_lut[FIRE_MAXI + 1] = {
    IDX_BLACK,
    IDX_RED, IDX_RED, IDX_RED, IDX_RED, IDX_RED, IDX_RED,
    IDX_FLAME3, IDX_FLAME3, IDX_FLAME3, IDX_FLAME3, IDX_FLAME3, IDX_FLAME3, IDX_FLAME3,
    IDX_FLAME2, IDX_FLAME2, IDX_FLAME2, IDX_FLAME2, IDX_FLAME2, IDX_FLAME2, IDX_FLAME2, IDX_FLAME2,
    IDX_FLAME1, IDX_FLAME1, IDX_FLAME1, IDX_FLAME1, IDX_FLAME1, IDX_FLAME1,
    IDX_GOLD_HI, IDX_GOLD_HI, IDX_GOLD_HI, IDX_GOLD_HI, IDX_GOLD_HI
};

static inline uint32_t fire_rng_next(void)
{
    uint32_t r = g_fire_rng;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    g_fire_rng = r;
    return r;
}

static void draw_oldschool_fire(int f)
{
    (void)f;
    uint8_t *bottom = g_fire_buf + (FIRE_FH - 1) * FIRE_FW;
    int x, y;

    /* Hot, flickering base row. */
    for (x = 0; x < FIRE_FW; ++x) {
        uint32_t r = fire_rng_next();
        bottom[x] = (uint8_t)((r & 7) ? FIRE_MAXI : (FIRE_MAXI - 8));
    }

    /* Propagate upward: each cell is a random lower neighbour minus a little decay. */
    for (y = FIRE_FH - 2; y >= 0; --y) {
        uint8_t *row = g_fire_buf + y * FIRE_FW;
        const uint8_t *below = row + FIRE_FW;
        for (x = 0; x < FIRE_FW; ++x) {
            uint32_t r = fire_rng_next();
            int dx = (int)(r & 3) - 1;        /* -1, 0, 1, then 2 -> clamp to 0 */
            int sx;
            int v;
            if (dx > 1) dx = 0;
            sx = x + dx;
            if (sx < 0) sx = 0; else if (sx >= FIRE_FW) sx = FIRE_FW - 1;
            v = (int)below[sx] - (int)((r >> 2) & 1);
            row[x] = (uint8_t)(v < 0 ? 0 : v);
        }
    }

    /* Blit half-res intensity to the framebuffer (2x) via the colour LUT. */
    for (y = 0; y < FIRE_FH; ++y) {
        const uint8_t *src = g_fire_buf + y * FIRE_FW;
        uint8_t *d0 = framebuffer + (FIRE_Y0 + y * 2) * WAIFU_FM_WIDTH;
        uint8_t *d1 = d0 + WAIFU_FM_WIDTH;
        for (x = 0; x < FIRE_FW; ++x) {
            uint8_t c = g_fire_lut[src[x]];
            int fx = x * 2;
            d0[fx] = c; d0[fx + 1] = c;
            d1[fx] = c; d1[fx + 1] = c;
        }
    }
}

static void draw_story_fire_screen(int f)
{
    int line_count = (int)(sizeof(story_fire_lines) / sizeof(story_fire_lines[0]));
    int line = g_story_fire_line;
    if (line < 0) line = 0;
    if (line >= line_count) line = line_count - 1;
    clear_screen(IDX_BLACK);
    draw_oldschool_fire(f);
    draw_panel_rect(8, WAIFU_UI_BOTTOM_Y(172), WAIFU_FM_WIDTH - 16, 57, IDX_UI_DARK);
    draw_text_small(18, WAIFU_UI_BOTTOM_Y(183), "DEMON", IDX_RED, IDX_BLACK);
    draw_wrapped_text_small_box(18, WAIFU_UI_BOTTOM_Y(198), WAIFU_FM_WIDTH - 38, 3, 10, story_subst_name(story_fire_lines[line]), IDX_WHITE, IDX_BLACK);
    if (((f / 16) & 1) == 0) draw_text_small(WAIFU_FM_WIDTH - 59, WAIFU_FM_HEIGHT - 24, "A/RUN", IDX_WHITE, IDX_BLACK);
}

static const char *deck_editor_card_name(int id)
{
    return is_support_card(id) ? support_card_name(id) : waifu_card_names[id];
}

static void draw_deck_editor_icon(int card, int x, int y, int selected)
{
#if defined(WAIFU_FM_CD32X)
    uint8_t rim = is_support_card(card) ? IDX_SUPPORT_FRAME : IDX_CARD_RIM;
    uint8_t mid = is_support_card(card) ? IDX_SUPPORT_FRAME_HI : IDX_GOLD_DARK;
    rect_fill(x + 2, y + 3, 26, 34, IDX_BLACK);
    rect_fill(x, y, 26, 34, rim);
    rect_outline(x, y, 26, 34, IDX_GOLD_HI);
    rect_fill(x + 3, y + 3, 20, 28, IDX_UI_DARK);
    rect_fill(x + 5, y + 5, 16, 5, mid);
    rect_fill(x + 5, y + 23, 16, 5, mid);
#else
    draw_hand_card_sprite(card, x, y, 26, 34, 0);
#endif
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
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        uint8_t c = (y < 28) ? IDX_DARK_BROWN : ((y & 8) ? IDX_UI_DARK : IDX_BLACK);
        hline(0, WAIFU_FM_WIDTH - 1, y, c);
    }
    draw_panel_rect(4, 4, WAIFU_FM_WIDTH - 8, WAIFU_FM_HEIGHT - 8, IDX_UI_DARK);
    draw_centered_text(12, "DECK EDITOR", IDX_GOLD_HI, IDX_BLACK);

    /* The deck editor is authored for the 256-wide layout.  WAIFU_UI_CENTER_DX
       cancels out at widths <= 256 (it is (W-256)/2) so PC-FX/headless/SDL are
       byte-identical; on the 320-wide CD32X display it shifts the whole editor
       block right by (320-256)/2 = 32 so tabs/grid/status/hint stop clinging to
       the left edge of the full-width blue box and sit centered inside it. */
    int ed_dx = WAIFU_UI_CENTER_DX;

    rect_fill(ed_dx + 14, 27, 102, 14, g_deck_tab == 0 ? IDX_GOLD_DARK : IDX_BLACK);
    rect_outline(ed_dx + 14, 27, 102, 14, g_deck_tab == 0 ? IDX_GOLD_HI : IDX_DIM);
    waifu_str_copy(line, (int)sizeof(line), "DECK "); waifu_str_cat_u32_z2(line, (int)sizeof(line), (unsigned)g_story_deck_count); waifu_str_cat(line, (int)sizeof(line), "/40");
    draw_text_small(ed_dx + 22, 31, line, g_deck_tab == 0 ? IDX_WHITE : IDX_DIM, IDX_BLACK);

    rect_fill(ed_dx + 140, 27, 102, 14, g_deck_tab == 1 ? IDX_GOLD_DARK : IDX_BLACK);
    rect_outline(ed_dx + 140, 27, 102, 14, g_deck_tab == 1 ? IDX_GOLD_HI : IDX_DIM);
    waifu_str_copy(line, (int)sizeof(line), "STORAGE "); waifu_str_cat_u32_z2(line, (int)sizeof(line), (unsigned)g_story_storage_count);
    draw_text_small(ed_dx + 148, 31, line, g_deck_tab == 1 ? IDX_WHITE : IDX_DIM, IDX_BLACK);

    recalc_story_deck_counts();
    waifu_str_copy(line, (int)sizeof(line), "SUPPORT "); waifu_str_cat_u32_z2(line, (int)sizeof(line), (unsigned)(g_story_support_count + g_story_equip_count)); waifu_str_cat(line, (int)sizeof(line), "  EQ "); waifu_str_cat_u32_z2(line, (int)sizeof(line), (unsigned)g_story_equip_count);
    draw_text_small(ed_dx + 76, 43, line, IDX_GOLD_HI, IDX_BLACK);

    if (count <= 0) {
        draw_centered_text(105, "EMPTY", IDX_DIM, IDX_BLACK);
    } else {
        for (int i = 0; i < DECK_VISIBLE_CARDS; ++i) {
            int idx = scroll + i;
            if (idx >= count) break;
            int col = i % DECK_GRID_COLS;
            int row = i / DECK_GRID_COLS;
            int x = ed_dx + 14 + col * 40;
            int y = 50 + row * 45;
            draw_deck_editor_icon(arr[idx], x, y, idx == g_deck_cursor);
        }
    }

    rect_fill(ed_dx + 9, WAIFU_UI_BOTTOM_Y(190), WAIFU_FM_WIDTH - (ed_dx + 9) * 2, 36, IDX_BLACK);
    rect_outline(ed_dx + 9, WAIFU_UI_BOTTOM_Y(190), WAIFU_FM_WIDTH - (ed_dx + 9) * 2, 36, IDX_UI_LIGHT);
    if (selected_card >= 0) {
        draw_text_small_ellipsis(ed_dx + 15, WAIFU_UI_BOTTOM_Y(196), deck_editor_card_name(selected_card), 25, IDX_WHITE, IDX_BLACK);
        if (is_support_card(selected_card)) {
            draw_text_small_ellipsis(ed_dx + 15, WAIFU_UI_BOTTOM_Y(208), support_card_type(selected_card), 23, IDX_GOLD_HI, IDX_BLACK);
        } else {
            fmt_label_u32(line, (int)sizeof(line), "ATK", (unsigned)waifu_card_atk[selected_card]); waifu_str_cat(line, (int)sizeof(line), " DEF "); waifu_str_cat_u32(line, (int)sizeof(line), (unsigned)waifu_card_def[selected_card]);
            draw_text_small(ed_dx + 15, WAIFU_UI_BOTTOM_Y(208), line, IDX_GOLD_HI, IDX_BLACK);
        }
    }
    if (g_deck_flash > 0 && ((g_deck_flash / 8) & 1) == 0) {
        const char *msg = g_deck_flash_reason == 1 ? "MAX 4 COPIES" :
            (g_story_deck_count == STORY_DECK_SIZE ? "DECK IS FULL" : "DECK MUST BE 40");
        draw_centered_text(WAIFU_UI_BOTTOM_Y(181), msg, IDX_RED, IDX_BLACK);
    }
    draw_text_small(ed_dx + 15, WAIFU_FM_HEIGHT - 14, "A MOVE  B CHECK  BTN4 TAB", IDX_WHITE, IDX_BLACK);
}

static void transition_draw_deck_editor_source(int frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    draw_deck_editor();
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
#ifdef WAIFU_FM_PCFX
    /* Floor disabled on PC-FX: the KING software floor (per-pixel 230KB-LUT reads
       through KING I/O with wait states) is too slow.  Evaluating a hardware
       ground layer (KING affine BG / VDC) instead; background is black for now. */
    (void)cam; (void)floor_y; (void)tile_a; (void)tile_b; (void)tile_size;
    return;
#else
    Vec3 ffwd = vnorm(vsub(cam.target, cam.eye));
    Vec3 fright = vnorm(vcross(ffwd, cam.up));
    Vec3 fup = vcross(fright, ffwd);

    int tw = WAIFU_TEX_TILE_SIZE;
    const uint8_t *atlas = waifu_texture_atlas;
#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
    FloorSampleCache *sample_cache = floor_sample_cache_for(tile_size);
    if (!sample_cache) return;
#else
    int32_t direct_period = tile_size << (Q8_SHIFT + 1);
    if (direct_period <= 0) return;
#endif

    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        int32_t dy = q8_div(Q8_FROM_INT(WAIFU_FM_HEIGHT / 2 - y) - Q8_HALF, cam.focal);
        int32_t ray_y = q8_mul(fup.y, dy) + ffwd.y;
        if (ray_y >= 0) continue;
        int32_t t = q8_div(floor_y - cam.eye.y, ray_y);
        if (t <= Q8_FRAC(1,100)) continue;

        int32_t dx_l = q8_div(-Q8_FROM_INT(WAIFU_FM_WIDTH / 2), cam.focal);
        int32_t dx_r = q8_div(Q8_FROM_INT(WAIFU_FM_WIDTH - 1 - WAIFU_FM_WIDTH / 2), cam.focal);

        int32_t wl_x = cam.eye.x + q8_mul(t, q8_mul(fright.x, dx_l) + ffwd.x);
        int32_t wl_z = cam.eye.z + q8_mul(t, q8_mul(fright.z, dx_l) + ffwd.z);
        int32_t wr_x = cam.eye.x + q8_mul(t, q8_mul(fright.x, dx_r) + ffwd.x);
        int32_t wr_z = cam.eye.z + q8_mul(t, q8_mul(fright.z, dx_r) + ffwd.z);

#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
        int32_t period = sample_cache->period_q16;
        const uint8_t *samp = sample_cache->sample;
#else
        int32_t period = direct_period;
#endif
        uint8_t *row = framebuffer + y * WAIFU_FM_WIDTH;
        int32_t px = wrap_floor_sample_phase(wl_x << Q8_SHIFT, period);
        int32_t pz = wrap_floor_sample_phase(wl_z << Q8_SHIFT, period);
        int32_t dx = ((wr_x - wl_x) << Q8_SHIFT) / (WAIFU_FM_WIDTH - 1);
        int32_t dz = ((wr_z - wl_z) << Q8_SHIFT) / (WAIFU_FM_WIDTH - 1);
        /* Reduce the per-pixel phase step into [0, period) ONCE per row.  The
           sample LUT is periodic, so px only matters mod period; pre-reducing the
           step makes the per-pixel wrap a single compare+subtract instead of two
           hardware divides.  Also write the framebuffer directly: x in [0,WAIFU_FM_WIDTH) and
           y is in range, so put_px's bounds test is redundant. */
        dx %= period; if (dx < 0) dx += period;
        dz %= period; if (dz < 0) dz += period;

        for (int x = 0; x < WAIFU_FM_WIDTH; ++x) {
#if !defined(WAIFU_FLOOR_SAMPLE_CACHE_DISABLE)
            uint8_t ux = samp[px];
            uint8_t vz = samp[pz];
#else
            uint8_t ux = floor_sample_direct(px, tile_size);
            uint8_t vz = floor_sample_direct(pz, tile_size);
#endif
            int tile = ((ux ^ vz) & 32) ? tile_b : tile_a;
            const uint8_t *src = atlas + (size_t)tile * tw * tw;
            row[x] = src[(vz & 31) * tw + (ux & 31)];
            px += dx; if (px >= period) px -= period;
            pz += dz; if (pz >= period) pz -= period;
        }
    }
#endif /* WAIFU_FM_PCFX */
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
    /* Desert world-map floor: repeat one sand tile across the whole ground. */
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 2, 2, Q8_FRAC(176,100));

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
    /* The sanctum hub reflects how far Serena has travelled (the frontier), not
       whichever earlier opponent she may have selected to rematch. */
    if (g_story_progress >= 4) return STORY_SCENE_VOID;
    if (g_story_progress >= 3) return STORY_SCENE_VOLCANO;
    if (g_story_progress >= 2) return STORY_SCENE_TEMPLE;
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
    /* Temple pillars: two rows of columns forming a corridor. */
    int32_t pillar_y0 = 0, pillar_y1 = Q8_FRAC(26,10);
    for (int row = 0; row < 2; ++row) {
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
    draw_floor_tiled(cam, -Q8_FRAC(7,100), 6, 6, Q8_FRAC(176,100));
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
    for (int i = 0; i < 4; ++i) {
        draw_tri3d_pyramid_face(cam, vf[i].p0, vf[i].p1, apex_v, 7, vf[i].flip, 6, 4);
        /* Match the desert-road pyramid's painter ordering: outline each
           sorted face immediately after its real surface, so nearer faces
           naturally overwrite the far-side wire edges. */
        ScreenPt p0 = project_point(cam, vf[i].p0);
        ScreenPt p1 = project_point(cam, vf[i].p1);
        ScreenPt p2 = project_point(cam, apex_v);
        if (p0.ok && p1.ok) line_i(p0.x, p0.y, p1.x, p1.y, IDX_BLACK);
        if (p1.ok && p2.ok) line_i(p1.x, p1.y, p2.x, p2.y, IDX_BLACK);
        if (p2.ok && p0.ok) line_i(p2.x, p2.y, p0.x, p0.y, IDX_BLACK);
    }
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
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        uint8_t c = y < 52 ? IDX_UI_BLUE : (y < 93 ? IDX_UI_TEAL : (y < 143 ? IDX_GOLD_DARK : IDX_DARK_BROWN));
        hline(0, WAIFU_FM_WIDTH - 1, y, c);
    }
    for (int x = 0; x < WAIFU_FM_WIDTH; x += 6) {
        int yy = 142 + ((x * 13) & 7);
        hline(x, x + 5 < WAIFU_FM_WIDTH ? x + 5 : WAIFU_FM_WIDTH - 1, yy, IDX_GOLD_HI);
    }
}

static void draw_temple_sky(void)
{
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        uint8_t c = y < 40 ? IDX_UI_BLUE : (y < 80 ? IDX_UI_TEAL : (y < 120 ? IDX_DIM : IDX_DARK_BROWN));
        hline(0, WAIFU_FM_WIDTH - 1, y, c);
    }
}

static void draw_volcano_sky(void)
{
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) {
        uint8_t c = y < 45 ? IDX_BLACK : (y < 85 ? IDX_RED : (y < 120 ? IDX_FLAME3 : IDX_DARK_BROWN));
        hline(0, WAIFU_FM_WIDTH - 1, y, c);
    }
    /* Embers drifting upward. */
    for (int i = 0; i < 40; ++i) {
        int x = (i * 53 + 17) % WAIFU_FM_WIDTH;
        int y = 120 - ((i * 31 + 7) & 63);
        put_px(x, y, (i & 1) ? IDX_FLAME1 : IDX_GOLD_HI);
    }
}

static void draw_void_sky(void)
{
    for (int y = 0; y < WAIFU_FM_HEIGHT; ++y) hline(0, WAIFU_FM_WIDTH - 1, y, IDX_BLACK);
    /* Stars. */
    for (int i = 0; i < 60; ++i) {
        int x = (i * 67 + 13) % WAIFU_FM_WIDTH;
        int y = (i * 41 + 5) & 127;
        put_px(x, y, (i & 3) ? IDX_DIM : IDX_WHITE);
    }
}

static WaifuBackgroundKind story_background_kind(void)
{
    switch (story_scene_kind()) {
    case STORY_SCENE_TEMPLE:  return WAIFU_BACKGROUND_STONE;
    case STORY_SCENE_VOLCANO: return WAIFU_BACKGROUND_EMBER;
    case STORY_SCENE_VOID:    return WAIFU_BACKGROUND_SKY;
    default:                  return WAIFU_BACKGROUND_DESERT;
    }
}

static int story_background_hscroll(int f)
{
    int32_t phase;
    switch (story_scene_kind()) {
    case STORY_SCENE_TEMPLE:  phase = f * Q8_FRAC(22,1000); break;
    case STORY_SCENE_VOLCANO: phase = f * Q8_FRAC(24,1000); break;
    case STORY_SCENE_VOID:    phase = f * Q8_FRAC(20,1000); break;
    default:                  phase = f * Q8_FRAC(25,1000); break;
    }
    return q8_to_int(q8_mul(Q8_FROM_INT(4), q8_sin_rad(phase))) & 0x01ff;
}

static void draw_story_sky(int f)
{
    /* Ask the platform for a hardware background layer (PC-FX RAINBOW). If it
       presents one, leave the framebuffer transparent for it to show through;
       otherwise composite the sky into the framebuffer in software. */
    if (waifu_platform_background_request(story_background_kind(), story_background_hscroll(f))) {
        clear_screen(0);
        return;
    }
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

static void draw_story_sanctum_background(void)
{
    /* Use the free-running ambient frame, not g_i_frame: the sanctum menu, save
       status, and save-device screens each reset g_i_frame to 0 on entry, which
       made the 3D scene visibly snap/reset behind the panels.  A continuous
       frame keeps the scene steady across those sub-screens. */
    draw_story_sky(g_story_scene_anim_frame);
    draw_story_scene_3d(g_story_scene_anim_frame);
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
    draw_story_sky(f);
    draw_story_scene_3d(f);
    draw_panel_rect(WAIFU_FM_WIDTH - 130, WAIFU_UI_BOTTOM_Y(146), 121, 76, IDX_UI_DARK);
    draw_text_small(WAIFU_FM_WIDTH - 121, WAIFU_UI_BOTTOM_Y(155), "DESTINATION", IDX_GOLD_HI, IDX_BLACK);
    draw_text(WAIFU_FM_WIDTH - 113, WAIFU_UI_BOTTOM_Y(174), "SANCTUM", g_story_map_cursor == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(WAIFU_FM_WIDTH - 113, WAIFU_UI_BOTTOM_Y(194), "BATTLE", g_story_map_cursor == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    if (g_story_map_cursor == 0) draw_text(WAIFU_FM_WIDTH - 124, WAIFU_UI_BOTTOM_Y(174), ">", IDX_RED, IDX_BLACK);
    else draw_text(WAIFU_FM_WIDTH - 124, WAIFU_UI_BOTTOM_Y(194), ">", IDX_RED, IDX_BLACK);
    /* Earlier opponents are unlocked: show that BATTLE can cycle foes and which
       one is currently picked (FOE n/total). */
    if (g_story_progress > 0) {
        char foe[24];
        if (g_story_map_cursor == 1) draw_text_small(WAIFU_FM_WIDTH - 52, WAIFU_UI_BOTTOM_Y(196), "< >", IDX_GOLD_HI, IDX_BLACK);
        waifu_str_copy(foe, (int)sizeof(foe), "FOE ");
        waifu_str_cat_i32(foe, (int)sizeof(foe), g_story_duel_index + 1);
        waifu_str_cat(foe, (int)sizeof(foe), "/");
        waifu_str_cat_i32(foe, (int)sizeof(foe), g_story_progress + 1);
        draw_text_small(WAIFU_FM_WIDTH - 121, WAIFU_UI_BOTTOM_Y(210), foe, IDX_WHITE, IDX_BLACK);
    }

    draw_panel_rect(8, WAIFU_UI_BOTTOM_Y(181), 108, 41, IDX_UI_DARK);
    if (g_story_duel_index < g_story_progress) {
        /* Selecting an already-cleared opponent: a rematch. */
        waifu_str_copy(line, (int)sizeof(line), "Rematch: "); waifu_str_cat(line, (int)sizeof(line), story_opponent_name()); waifu_str_cat(line, (int)sizeof(line), ".");
        draw_wrapped_text_small_box(16, WAIFU_UI_BOTTOM_Y(190), 91, 3, 9, line, IDX_GOLD_HI, IDX_BLACK);
    } else if (g_story_duel_index >= STORY_MAX_DUELS - 1) {
        draw_wrapped_text_small_box(16, WAIFU_UI_BOTTOM_Y(190), 91, 3, 9, "The demon waits in the void. This is the final duel.", IDX_WHITE, IDX_BLACK);
    } else if (story_opponent_is_boss()) {
        waifu_str_copy(line, (int)sizeof(line), "A boss: "); waifu_str_cat(line, (int)sizeof(line), story_opponent_name()); waifu_str_cat(line, (int)sizeof(line), ". Prepare well.");
        draw_wrapped_text_small_box(16, WAIFU_UI_BOTTOM_Y(190), 91, 3, 9, line, IDX_RED, IDX_BLACK);
    } else {
        waifu_str_copy(line, (int)sizeof(line), "Next: "); waifu_str_cat(line, (int)sizeof(line), story_opponent_name()); waifu_str_cat(line, (int)sizeof(line), " awaits.");
        draw_wrapped_text_small_box(16, WAIFU_UI_BOTTOM_Y(190), 91, 3, 9, line, IDX_WHITE, IDX_BLACK);
    }
    if (g_story_map_cursor == 1 && g_story_progress > 0)
        draw_text_small(11, WAIFU_FM_HEIGHT - 14, "A/RUN GO  L/R FOE", IDX_WHITE, IDX_BLACK);
    else
        draw_text_small(11, WAIFU_FM_HEIGHT - 14, "A/RUN SELECT", IDX_WHITE, IDX_BLACK);
}

static void draw_story_map_screen(int f)
{
    draw_story_map_screen_content(f);
    if (f >= 0 && f < 24) apply_black_dither_fade(q8_ratio(f, 24));
}

static void draw_story_plaza_scene_content(int anim_frame);

static void transition_draw_story_map_source(int frame, void *ctx)
{
    (void)ctx;
    draw_story_map_screen_content(frame);
}

static void draw_story_to_plaza_transition(int f)
{
    draw_fade_to_black_transition(f, WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                  WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                  transition_draw_story_map_source, NULL);
}

static void transition_draw_story_fire_source(int frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    draw_story_fire_screen(g_story_fire_line);
}

static void draw_story_fire_to_deck_transition(int f)
{
    /* Fade the fire scene out to black, then HOLD black for the second half.
       Do NOT reveal the deck editor here: the deck screen must only appear
       after the LOADING screen (entered via enter_deck_editor_after_assets()
       once this transition finishes).  Fading the deck editor in here made it
       flash for a few frames before LOADING ran. */
    draw_fade_to_black_transition(f, WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                  WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                  transition_draw_story_fire_source, NULL);
}

static void draw_story_pyramid_menu(void)
{
    draw_story_sanctum_background();
    draw_blue_gradient_box(126, 42, 122, 160);
    draw_text(158, 55, "SANCTUM", IDX_GOLD_HI, IDX_BLACK);
    draw_wrapped_text_small_box(138, 76, 99, 4, 10, story_subst_name("A place of rest. Serena can prepare before the next duel."), IDX_WHITE, IDX_BLACK);
    draw_text(151, 122, "SAVE", g_story_pyramid_cursor == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(151, 140, "DECK EDITOR", g_story_pyramid_cursor == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(151, 158, "QUIT", g_story_pyramid_cursor == 2 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(151, 176, "BACK", g_story_pyramid_cursor == 3 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(139, 122 + g_story_pyramid_cursor * 18, ">", IDX_RED, IDX_BLACK);
}

static void draw_story_save_screen(void)
{
    draw_story_sanctum_background();
    draw_blue_gradient_box(31, 78, 194, 82);
    if (g_story_save_status < 0) {
        draw_centered_text(95, "SAVE FAILED", IDX_RED, IDX_BLACK);
        draw_centered_text(118, "The memory seal is broken.", IDX_WHITE, IDX_BLACK);
    } else if (g_story_save_status > 0) {
        draw_centered_text(95, "PROGRESS SAVED", IDX_GOLD_HI, IDX_BLACK);
        draw_wrapped_text_small_box(57, 115, 143, 2, 10, story_subst_name("The sanctum remembers Serena."), IDX_WHITE, IDX_BLACK);
    } else {
        draw_centered_text(95, "NO SAVE DATA", IDX_RED, IDX_BLACK);
        draw_centered_text(118, "Nothing is written yet.", IDX_WHITE, IDX_BLACK);
    }
    if (((g_i_frame / 16) & 1) == 0) draw_centered_text(143, "A/RUN/B BACK", IDX_WHITE, IDX_BLACK);
}

#ifdef WAIFU_FM_PCFX
static void draw_story_save_device_screen(void)
{
    draw_story_sanctum_background();
    draw_blue_gradient_box(126, 54, 122, 132);
    draw_text(161, 70, "SAVE TO", IDX_GOLD_HI, IDX_BLACK);
    draw_text(151, 106, g_i_save_device_sel == 0 ? "> INTERNAL" : "  INTERNAL", g_i_save_device_sel == 0 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(151, 126, g_i_save_device_sel == 1 ? "> FX-BMP" : "  FX-BMP", g_i_save_device_sel == 1 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
    draw_text(151, 146, g_i_save_device_sel == 2 ? "> BACK" : "  BACK", g_i_save_device_sel == 2 ? IDX_GOLD_HI : IDX_WHITE, IDX_BLACK);
}
#endif

static const char *story_battle_intro_lines(void)
{
    switch (g_story_duel_index) {
    case 0: return "The dream shifts. A shade rises from the sand.";
    case 1: return "The plaza bustles, but one duelist blocks the path.";
    case 2: return story_subst_name("Stone columns tower above. An adept tests Serena.");
    case 3: return "The sandstorm parts. A reaver grins behind her veil.";
    case 4: return "The crater glows. A burning soul rises from the lava.";
    case 5: return "Reality fractures. Something walks the void between dreams.";
    case 6: return "The ancient guardian awakens. The sphinx does not blink.";
    default: return story_subst_name("THE DEMON. It remembers Serena. This ends now.");
    }
}

/* Animation frame used to freeze the story-dialogue scene (3D camera sway and
   the portrait slide-in) while it fades to black.  A fade transition resets
   g_i_frame to 0, which otherwise made the portraits visibly slide in again and
   the camera snap back -- the scene must hold its last live pose instead. */
static int g_story_plaza_freeze_frame = 0;

static void draw_story_plaza_scene_content(int anim_frame)
{
    int line = g_story_plaza_line;
    int line_count = 0;
    const StoryDialogueLine *dialog = story_dialogue_for_duel(g_story_duel_index, &line_count);
    const StoryOpponentInfo *opp = story_opponent_info();
    int serena_x, opp_x, serena_y, opp_y;
    const char *speaker = g_story_name;
    const char *subhead = opp->title;
    uint8_t speaker_color = IDX_GOLD_HI;

    if (line < 0) line = 0;
    if (line_count <= 0) line_count = 1;
    if (line >= line_count) line = line_count - 1;

    waifu_fm_use_dialogue_palette();
    clear_screen(IDX_BLACK);
    draw_story_sky(anim_frame);
    draw_story_scene_3d(anim_frame);
    draw_panel_rect(8, 8, 102, 18, IDX_UI_DARK);
    draw_text_small(14, 14, story_scene_name(), IDX_GOLD_HI, IDX_BLACK);

    serena_x = story_slide_x(-WAIFU_STORY_PORTRAIT_W - 14, 2, anim_frame);
    opp_x = story_slide_x(WAIFU_FM_WIDTH + 14, WAIFU_FM_WIDTH - WAIFU_STORY_PORTRAIT_W - 2, anim_frame);
    /* Raise portraits so their hands and upper torsos read more naturally,
       while leaving the textbox directly over their lower bodies. */
    serena_y = WAIFU_FM_HEIGHT - WAIFU_STORY_PORTRAIT_H - 20;
    opp_y = WAIFU_FM_HEIGHT - WAIFU_STORY_PORTRAIT_H - 14;
    draw_story_portrait(STORY_PORTRAIT_SERENA, serena_x, serena_y);
    draw_story_portrait(opp->portrait_id, opp_x, opp_y);
    if (dialog[line].speaker == STORY_SPK_OPPONENT) {
        speaker = opp->name;
        speaker_color = story_opponent_is_boss() ? IDX_RED : IDX_GOLD_HI;
        subhead = opp->title;
    } else if (dialog[line].speaker == STORY_SPK_NARRATOR) {
        speaker = "CHRONICLE";
        speaker_color = IDX_UI_LIGHT;
        subhead = story_scene_name();
    } else {
        speaker = g_story_name;
        speaker_color = IDX_GOLD_HI;
        subhead = opp->title;
    }

    draw_story_dialog_box(speaker, subhead, story_subst_name(dialog[line].text), speaker_color, anim_frame);
}

static void draw_story_plaza_scene(void)
{
    draw_story_plaza_scene_content(g_i_frame);
    if (g_i_frame >= 0 && g_i_frame < 24) apply_black_dither_fade(q8_ratio(g_i_frame, 24));
}

static void transition_draw_story_plaza_source(int frame, void *ctx)
{
    (void)frame;
    (void)ctx;
    /* Freeze-frame: render the scene at its last live frame so the portraits
       and 3D camera hold still under the fade instead of re-sliding from zero. */
    draw_story_plaza_scene_content(g_story_plaza_freeze_frame);
}

#define STORY_ENDING_CREDITS_FRAMES 300
#define STORY_ENDING_TEXT_ERASE_FRAMES 36
#define STORY_ENDING_TEXT_TYPE_FRAME_SPAN 72

static const char *story_ending_lines[] = {
    "The last shard is silent. No enemy answers its call.",
    "I finally defeated them. The ones who turned dreams into chains.",
    "The deck is whole again. Every stolen card has found its way home.",
    "When morning comes, I will carry these cards beyond the ruins."
};

static int story_ending_line_count(void)
{
    return (int)(sizeof(story_ending_lines) / sizeof(story_ending_lines[0]));
}

static int story_ending_line_visible_chars(int line)
{
    int len;
    int visible;
    if (line < 0) line = 0;
    if (line >= story_ending_line_count()) line = story_ending_line_count() - 1;
    len = (int)strlen(story_ending_lines[line]);
    if (g_story_ending_erasing && g_i_frame >= 0) {
        visible = len - q8_to_int(q8_mul(Q8_FROM_INT(len), q8_ratio(g_i_frame, STORY_ENDING_TEXT_ERASE_FRAMES)));
    } else {
        visible = q8_to_int(q8_mul(Q8_FROM_INT(len), q8_ratio(g_i_frame + 1, STORY_ENDING_TEXT_TYPE_FRAME_SPAN)));
    }
    if (visible < 0) visible = 0;
    if (visible > len) visible = len;
    return visible;
}

static int story_ending_line_fully_typed(void)
{
    int line = g_story_ending_line;
    if (line < 0) line = 0;
    if (line >= story_ending_line_count()) line = story_ending_line_count() - 1;
    return story_ending_line_visible_chars(line) >= (int)strlen(story_ending_lines[line]);
}

static void draw_story_ending_screen(void)
{
    int line_count = story_ending_line_count();
    int line = g_story_ending_line;
    int visible_chars;
    int fully_typed;
    if (line < 0) line = 0;
    if (line >= line_count) line = line_count - 1;
    visible_chars = story_ending_line_visible_chars(line);
    fully_typed = !g_story_ending_erasing && story_ending_line_fully_typed();

    waifu_fm_use_ending_palette();

    {
        WaifuTextOverlayParams ov = {0};
        ov.name = g_story_name;
        ov.page = line;
        ov.prompt_visible = fully_typed && ((g_i_frame / 16) & 1) == 0;
        ov.visible_chars = visible_chars;
        if (waifu_platform_text_overlay(WAIFU_TEXT_OVERLAY_ENDING_STORY, &ov)) {
            /* Hardware text layer carries the narration; framebuffer stays black. */
            clear_screen(IDX_BLACK);
            return;
        }
    }

    /* Software path: ending artwork with the narration composited as text. */
    {
        const uint8_t *ending_img = waifu_assets_ending_screen_img();
        if (ending_img) draw_card_raw(ending_img, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, 0, 0, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT);
        else clear_screen(IDX_BLACK);
    }
    {
        char visible_line[160];
        waifu_str_copy_n(visible_line, (int)sizeof(visible_line), story_ending_lines[line], visible_chars);
        draw_wrapped_text_small_box(10, WAIFU_FM_HEIGHT - 42, WAIFU_FM_WIDTH - 20, 4, 10, visible_line, IDX_WHITE, IDX_BLACK);
    }
}

static void draw_story_ending_credits_screen(void)
{
    if (waifu_platform_text_overlay(WAIFU_TEXT_OVERLAY_ENDING_CREDITS, 0)) {
        /* Hardware text layer carries the credits over a black framebuffer. */
        clear_screen(IDX_BLACK);
        waifu_fm_use_ending_black_palette();
    } else {
        clear_screen(IDX_BLACK);
        draw_centered_text(82, "THANK YOU FOR PLAYING.", IDX_WHITE, IDX_BLACK);
        draw_centered_text(112, "SHATTERED DECKS.", IDX_WHITE, IDX_BLACK);
        draw_centered_text(142, "A GAME BY GAMEBLABLA.", IDX_WHITE, IDX_BLACK);
        draw_centered_text(172, "(C) 2026", IDX_WHITE, IDX_BLACK);
    }
    if (g_i_frame >= 0 && g_i_frame < 24) apply_black_dither_fade(q8_ratio(g_i_frame, 24));
    if (g_i_frame >= STORY_ENDING_CREDITS_FRAMES - 36) {
        apply_black_dither_fade(Q8_ONE - q8_ratio(g_i_frame - (STORY_ENDING_CREDITS_FRAMES - 36), 36));
    }
}

static void story_return_to_map_after_duel(void)
{
    if (g_b_result >= 0) {
        /* A win always grants a reward drop, whether it is a fresh duel or a
           rematch against an already-cleared opponent. */
        award_story_win_drop();
        /* Only beating the frontier duel advances the story.  Rematching an
           earlier opponent (duel_index < progress) leaves progress untouched. */
        if (g_story_duel_index >= g_story_progress) {
            if (g_story_progress >= STORY_MAX_DUELS - 1) {
                /* The final duel was the frontier: clearing it ends the story. */
                g_story_battle_active = 0;
                g_story_ending_line = 0;
                g_story_ending_erasing = 0;
                init_battle_state();
                enter_story_ending_after_assets();
                return;
            }
            ++g_story_progress;
            /* Move the selection forward to the newly unlocked opponent. */
            g_story_duel_index = g_story_progress;
        }
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
    waifu_assets_big_art_draw_queue_reset();
    frame_dirty_reset();
    g_video_fade_visible_q8 = Q8_ONE;
    /* Free-running ambient frame for the story 3D sanctum/scene sway.  Unlike
       g_i_frame it never resets on a state change, so opening a sanctum
       sub-screen (SAVE, save-device, deck editor) no longer snaps the camera
       back to its start pose -- the scene holds its motion instead of visibly
       resetting. */
    ++g_story_scene_anim_frame;
    waifu_fm_use_common_palette();
    update_music_for_current_state();
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

    suppress_battle_input_if_locked(&press_up, &press_down, &press_left, &press_right,
                                    &press_a, &press_b, &press_start, &press_tab);

    int suppress_ui_sfx = (g_i_state == WAIFU_I_STORY_ENDING ||
                           g_i_state == WAIFU_I_STORY_ENDING_CREDITS);
    if (!suppress_ui_sfx && (press_up || press_down || press_left || press_right)) waifu_sound_play(WAIFU_SOUND_SELECT);
#ifdef WAIFU_FM_PCFX
    /* RUN/START on the title is the same user intent as A: confirm/start the
       game.  Route it to the 037-style confirm PSG effect, not the 038-style
       exit/cancel effect. */
    if (suppress_ui_sfx) {
        /* Ending text advances silently. */
    } else if (g_i_state == WAIFU_I_TITLE && (press_a || press_start)) {
        waifu_sound_play(WAIFU_SOUND_CONFIRM);
    } else if (g_i_state == WAIFU_I_MENU && g_i_menu_selected == 2 && (press_a || press_start)) {
        /* Load Story enters BackupRAM I/O; keep this path silent so a menu
           confirm PSG cannot latch while the title overlay and backup state
           are being torn down. */
    } else if (g_i_state == WAIFU_I_STORY_LOAD_DEVICE && (press_a || press_start) && g_i_load_device_sel == 2) {
        waifu_sound_play(WAIFU_SOUND_CONFIRM_ALT);
    } else if (g_i_state == WAIFU_I_STORY_LOAD_DEVICE && (press_a || press_start) && g_i_load_device_sel != 2) {
        /* Same as the direct Load Story row: loading a chosen backup device is
           silent. */
    } else if (g_i_state == WAIFU_I_STORY_SAVE_DEVICE && (press_a || press_start) && g_i_save_device_sel == 2) {
        waifu_sound_play(WAIFU_SOUND_CONFIRM_ALT);
    } else {
        if (press_a) waifu_sound_play(WAIFU_SOUND_CONFIRM);
        if ((press_start && start_press_plays_ui_sound()) || press_b || press_tab) waifu_sound_play(WAIFU_SOUND_CONFIRM_ALT);
    }
#else
    if (!suppress_ui_sfx && press_a) waifu_sound_play(WAIFU_SOUND_CONFIRM);
    if (!suppress_ui_sfx && ((press_start && start_press_plays_ui_sound()) || press_b || press_tab)) waifu_sound_play(WAIFU_SOUND_CONFIRM_ALT);
#endif

    switch (g_i_state) {
    case WAIFU_I_LOADING_ASSETS:
        if (g_i_loading_target == WAIFU_I_STORY_PLAZA &&
            waifu_assets_ready() && !story_duel_portraits_ready()) {
            request_story_duel_assets();
        }
        if (waifu_assets_ready()) {
            if (g_i_loading_target == WAIFU_I_STORY_PLAZA &&
                !story_duel_portraits_ready()) {
                request_story_duel_assets();
                break;
            }
            g_i_state = g_i_loading_target;
            g_i_frame = -1;
            break;
        }
        draw_asset_loading_screen();
#ifdef CD32X_DEBUG_AUTOBATTLE
        if (waifu_assets_load_step()) {
#else
        if (g_i_frame >= 24 && waifu_assets_load_step()) {
#endif
            if (g_i_loading_target == WAIFU_I_STORY_PLAZA &&
                !story_duel_portraits_ready()) {
                request_story_duel_assets();
                break;
            }
            g_i_state = g_i_loading_target;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_TITLE:
    {
        WaifuTextOverlayParams ov = {0};
        ov.prompt_visible = ((g_i_frame / 24) & 1) == 0;
        ov.has_save = story_save_exists();
        if (waifu_platform_text_overlay_is_hardware()) {
            /* Title runs as a static (KING 16M) surface with the mutable prompt
               on the hardware text layer. Blink changes touch only the overlay;
               the resident surface is composed once on entry. */
            if (g_i_frame == 0) draw_title_full_event(0);
            else waifu_fm_use_title_palette();
            waifu_platform_text_overlay(WAIFU_TEXT_OVERLAY_TITLE_PROMPT, &ov);
        } else {
            clear_screen(IDX_BLACK);
            draw_title_background();
            draw_title_logo();
            draw_title_prompt(g_i_frame);
        }
    }
        if (g_i_frame < WAIFU_TITLE_FADE_FRAMES) apply_black_dither_fade(q8_ratio(g_i_frame, WAIFU_TITLE_FADE_FRAMES));
        if (press_start || press_a) {
            enter_title_to_menu_fade();
        }
        break;

    case WAIFU_I_TITLE_TO_MENU:
        /* Compatibility fallback only.  The normal title RUN path now goes
           straight to MENU, without a black fade. */
        enter_menu_after_assets();
        break;

    case WAIFU_I_MENU:
    {
        int old_menu_selected = g_i_menu_selected;
        if (press_up) g_i_menu_selected = (g_i_menu_selected + 2) % 3;
        if (press_down) g_i_menu_selected = (g_i_menu_selected + 1) % 3;
        if (waifu_platform_text_overlay_is_hardware()) {
            /* Menu text lives on the hardware overlay; recompose the resident
               background only on entry or when the selection changed. */
            WaifuTextOverlayParams ov = {0};
            ov.selected = g_i_menu_selected;
            ov.has_save = story_save_exists();
            if (g_i_frame <= 0 || old_menu_selected != g_i_menu_selected) {
                draw_title_full_event(0);
            } else {
                waifu_fm_use_title_palette();
            }
            waifu_platform_text_overlay(WAIFU_TEXT_OVERLAY_MENU, &ov);
        } else {
            draw_menu_screen(g_i_menu_selected);
        }
    }
        if (press_start || press_a) {
            if (g_i_menu_selected == 0) {
                reset_story_entry();
                enter_menu_to_story_fade();
            } else if (g_i_menu_selected == 1) {
                enter_menu_to_battle_fade();
            } else {
                enter_menu_to_load_fade();
            }
        }
        break;

    case WAIFU_I_MENU_TO_STORY:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_TITLE_FADE_FRAMES, 4,
                                          transition_draw_menu_source, &g_i_menu_selected)) {
            g_i_state = WAIFU_I_STORY_NAME;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_MENU_TO_BATTLE:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_TITLE_FADE_FRAMES, 4,
                                          transition_draw_menu_source, &g_i_menu_selected)) {
            init_battle_state();
            enter_battle_after_assets();
        }
        break;

    case WAIFU_I_MENU_TO_LOAD:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_TITLE_FADE_FRAMES, 4,
                                          transition_draw_menu_source, &g_i_menu_selected)) {
            begin_story_load();
        }
        break;

#ifdef WAIFU_FM_PCFX
    case WAIFU_I_STORY_LOAD_DEVICE:
    {
        /* Device picker stays on the static 16M title surface; the menu text
           is drawn on the front VDC overlay layer, like the main menu. */
        int internal_has = story_save_exists_device(0);
        int external_has = story_save_exists_device(1);
        if (press_up) g_i_load_device_sel = (g_i_load_device_sel + 2) % 3;
        if (press_down) g_i_load_device_sel = (g_i_load_device_sel + 1) % 3;
        waifu_fm_use_title_palette();
        waifu_pcfx_video_overlay_load_menu(g_i_load_device_sel, internal_has, external_has);
        if (press_a || press_start) {
            if (g_i_load_device_sel == 2) {
                /* BACK: return to the standard menu. */
                g_i_state = WAIFU_I_MENU;
                g_i_frame = -1;
            } else {
                int ext = g_i_load_device_sel; /* 0 internal, 1 external */
                int has = ext ? external_has : internal_has;
                if (has) {
                    enter_load_device_to_map_fade(ext);
                }
                /* No save on the chosen device: stay so the player can pick
                   the other one or BACK. */
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_MENU;
            g_i_frame = -1;
        }
        break;
    }

    case WAIFU_I_STORY_LOAD_DEVICE_TO_MAP:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_TITLE_FADE_FRAMES, 4,
                                          transition_draw_load_device_source, &g_i_load_device_sel)) {
            waifu_pcfx_video_overlay_clear();
            g_i_state = WAIFU_I_STORY_LOAD_TO_MAP;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_LOAD_TO_MAP:
        waifu_pcfx_video_overlay_clear();
        draw_backup_loading_screen();
        if (g_i_frame >= 8) {
            if (!load_story_device_to_map(g_i_load_pending_device)) {
                int fallback = g_i_load_pending_device ? 0 : 1;
                if (!story_save_exists_device(fallback) ||
                    !load_story_device_to_map(fallback)) {
                    g_story_save_status = -1;
                    g_i_state = WAIFU_I_MENU;
                    g_i_frame = -1;
                }
            }
        }
        break;
#endif

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
            enter_menu_after_assets();
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
            enter_story_intro_after_assets();
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
            enter_menu_after_assets();
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
            enter_menu_after_assets();
        }
        break;

    case WAIFU_I_STORY_FIRE_TO_DECK:
        draw_story_fire_to_deck_transition(g_i_frame);
        if (g_i_frame >= WAIFU_FAST_TRANSITION_HALF_FRAMES * 2) {
            enter_deck_editor_after_assets();
        }
        break;

    case WAIFU_I_STORY_MAP:
        if (press_up || press_down) g_story_map_cursor ^= 1;
        /* Keep the selection inside the unlocked range. */
        if (g_story_duel_index > g_story_progress) g_story_duel_index = g_story_progress;
        if (g_story_duel_index < 0) g_story_duel_index = 0;
        /* On the BATTLE row, LEFT/RIGHT picks which unlocked opponent to face:
           the frontier or any earlier opponent for a rematch. */
        if (g_story_map_cursor == 1 && g_story_progress > 0) {
            int span = g_story_progress + 1;
            if (press_left)  g_story_duel_index = (g_story_duel_index + span - 1) % span;
            if (press_right) g_story_duel_index = (g_story_duel_index + 1) % span;
        }
        draw_story_map_screen(g_i_frame);
        if (press_a || press_start) {
            if (g_story_map_cursor == 0) {
                g_story_pyramid_cursor = 0;
                g_i_state = WAIFU_I_STORY_PYRAMID;
                g_i_frame = -1;
            } else if (g_story_duel_index < g_story_progress) {
                enter_story_battle_after_assets();
            } else {
                g_story_plaza_line = 0;
                g_i_state = WAIFU_I_STORY_TO_PLAZA;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            enter_menu_after_assets();
        }
        break;

    case WAIFU_I_STORY_PYRAMID:
        if (press_up) g_story_pyramid_cursor = (g_story_pyramid_cursor + 3) % 4;
        if (press_down) g_story_pyramid_cursor = (g_story_pyramid_cursor + 1) % 4;
        draw_story_pyramid_menu();
        if (press_a || press_start) {
            if (g_story_pyramid_cursor == 0) {
#ifdef WAIFU_FM_PCFX
                g_i_save_device_sel = 0;
                g_i_state = WAIFU_I_STORY_SAVE_DEVICE;
                g_i_frame = -1;
#else
                g_story_saved_flash = 60;
                g_story_save_status = write_story_save() ? 1 : -1;
                g_i_state = WAIFU_I_STORY_SAVE;
                g_i_frame = -1;
#endif
            } else if (g_story_pyramid_cursor == 1) {
                reset_story_deck_editor();
                g_story_editor_from_pyramid = 1;
                enter_deck_editor_after_assets();
            } else if (g_story_pyramid_cursor == 2) {
                /* QUIT: abandon the story session and return to the title. */
#ifdef WAIFU_FM_PCFX
                waifu_pcfx_video_overlay_clear();
#endif
                g_story_battle_active = 0;
                g_story_pyramid_cursor = 0;
                enter_title_after_assets();
            } else {
                g_story_map_cursor = 0;
                g_i_state = WAIFU_I_STORY_MAP;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_story_map_cursor = 0;
            g_i_state = WAIFU_I_STORY_MAP;
            g_i_frame = -1;
        }
        break;

#ifdef WAIFU_FM_PCFX
    case WAIFU_I_STORY_SAVE_DEVICE:
        if (press_up) g_i_save_device_sel = (g_i_save_device_sel + 2) % 3;
        if (press_down) g_i_save_device_sel = (g_i_save_device_sel + 1) % 3;
        draw_story_save_device_screen();
        if (press_a || press_start) {
            if (g_i_save_device_sel == 2) {
                g_i_state = WAIFU_I_STORY_PYRAMID;
                g_i_frame = -1;
            } else {
                g_story_saved_flash = 60;
                g_story_save_status = write_story_save_device(g_i_save_device_sel) ? 1 : -1;
                g_i_state = WAIFU_I_STORY_SAVE;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_STORY_PYRAMID;
            g_i_frame = -1;
        }
        break;
#endif

    case WAIFU_I_STORY_SAVE:
        draw_story_save_screen();
        if (press_a || press_b || press_start || g_i_frame > 90) {
            g_i_state = WAIFU_I_STORY_PYRAMID;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_TO_PLAZA:
        draw_story_to_plaza_transition(g_i_frame);
        if (g_i_frame >= WAIFU_FAST_TRANSITION_HALF_FRAMES * 2) {
            enter_story_plaza_after_assets();
        }
        break;

    case WAIFU_I_STORY_PLAZA:
        if (!story_duel_portraits_ready()) {
            enter_story_plaza_after_assets();
            draw_asset_loading_screen();
            break;
        }
        draw_story_plaza_scene();
        if (press_a || press_start) {
            int line_count = 0;
            story_dialogue_for_duel(g_story_duel_index, &line_count);
            ++g_story_plaza_line;
            if (g_story_plaza_line >= line_count) {
                reset_story_deck_editor();
                g_story_editor_from_pyramid = 0;
                g_deck_flash = (g_story_deck_count == STORY_DECK_SIZE) ? 0 : 60;
                /* Hold the scene at this last live frame through the fade. */
                g_story_plaza_freeze_frame = g_i_frame;
                g_i_state = WAIFU_I_STORY_PLAZA_TO_DECK;
                g_i_frame = -1;
            }
        }
        if (press_b) {
            g_i_state = WAIFU_I_STORY_MAP;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_STORY_PLAZA_TO_DECK:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          transition_draw_story_plaza_source, NULL)) {
            enter_deck_editor_after_assets();
        }
        break;

    case WAIFU_I_STORY_ENDING:
        draw_story_ending_screen();
        if (g_story_ending_erasing) {
            if (g_i_frame >= STORY_ENDING_TEXT_ERASE_FRAMES) {
                g_story_ending_erasing = 0;
                ++g_story_ending_line;
                if (g_story_ending_line >= story_ending_line_count()) {
                    g_i_state = WAIFU_I_STORY_ENDING_CREDITS;
                }
                g_i_frame = -1;
            }
        } else if (press_a || press_start) {
            if (!story_ending_line_fully_typed()) {
                g_i_frame = STORY_ENDING_TEXT_TYPE_FRAME_SPAN;
            } else {
                g_story_ending_erasing = 1;
                g_i_frame = -1;
            }
        }
        break;

    case WAIFU_I_STORY_ENDING_CREDITS:
        draw_story_ending_credits_screen();
        if (press_a || press_start || g_i_frame >= STORY_ENDING_CREDITS_FRAMES) {
#ifdef WAIFU_FM_PCFX
            waifu_pcfx_video_overlay_clear();
#endif
            enter_title_after_assets();
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
#if defined(WAIFU_FM_CD32X)
            /* Do not issue a CD seek in the editor input frame.  The preview
               state paints first, then streams the selected full art. */
            g_deck_preview_art_pending = is_support_card(g_deck_preview_card)
                ? (waifu_assets_support_big_art_cached() == NULL)
                : (waifu_assets_card_big_art_cached(g_deck_preview_card) == NULL);
#endif
            g_i_state = WAIFU_I_DECK_PREVIEW;
            g_i_frame = -1;
        }
        if (press_start) {
            if (g_story_deck_count == STORY_DECK_SIZE) {
                recalc_story_deck_counts();
                if (g_story_editor_from_pyramid) {
                    g_i_state = WAIFU_I_DECK_EDITOR_TO_PYRAMID;
                    g_i_frame = -1;
                    break;
                }
                /* Story-dialogue deck editing commits the deck and starts the
                   next duel; sanctum editing above is pure maintenance and
                   returns to the sanctum menu. */
                g_story_editor_from_pyramid = 0;
                g_i_state = WAIFU_I_DECK_EDITOR_TO_BATTLE;
                g_i_frame = -1;
            } else {
                g_deck_flash = 60;
            }
        }
        if (g_deck_flash > 0) --g_deck_flash;
        draw_deck_editor();
        break;

    case WAIFU_I_DECK_EDITOR_TO_PYRAMID:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          transition_draw_deck_editor_source, NULL)) {
            g_story_editor_from_pyramid = 0;
            g_i_state = WAIFU_I_STORY_PYRAMID;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_DECK_EDITOR_TO_BATTLE:
        if (draw_fade_to_black_transition(g_i_frame, WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          WAIFU_FAST_TRANSITION_HALF_FRAMES,
                                          transition_draw_deck_editor_source, NULL)) {
            init_story_battle_state();
            enter_battle_after_assets();
        }
        break;

    case WAIFU_I_DECK_PREVIEW:
#if defined(WAIFU_FM_CD32X)
        if (cd32x_step_card_check_transition(g_deck_preview_card, &g_deck_preview_art_pending, &g_i_frame, 1)) break;
#endif
        draw_interactive_card_preview(g_deck_preview_card, g_i_frame);
        if (press_b || press_a || press_start) {
            g_i_state = WAIFU_I_DECK_EDITOR;
            g_i_frame = -1;
        }
        break;

    case WAIFU_I_BATTLE:
        waifu_fm_use_common_palette();
        step_battle_interactive(input, press_up, press_down, press_left, press_right, press_a, press_b, press_start, press_tab);
        if (press_b && g_b_phase != IB_CARD_PREVIEW && g_b_phase != IB_TALLY && g_b_phase_frame > 12) {
            /* Back is only a local action inside battle screens; no hidden auto-quit. */
        }
        break;
    }

    update_music_for_current_state();
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

static void debug_jump_to_story_cutscene(int duel_index)
{
    waifu_fm_reset_interactive();
    if (duel_index < 0) duel_index = 0;
    if (duel_index >= STORY_MAX_DUELS) duel_index = STORY_MAX_DUELS - 1;
    g_story_duel_index = duel_index;
    if (g_story_progress < duel_index) g_story_progress = duel_index;
    g_story_map_cursor = 1;
    g_story_plaza_line = 0;
    request_story_duel_assets();
    enter_state_after_assets(WAIFU_I_STORY_PLAZA);
}

static void debug_setup_music_demo_state(const char *name)
{
    waifu_fm_reset_interactive();
    if (!name) return;
    if (!strcmp(name, "title")) {
        g_i_state = WAIFU_I_TITLE;
    } else if (!strcmp(name, "opening-dream") || !strcmp(name, "dream")) {
        g_story_intro_line = 0;
        g_i_state = WAIFU_I_STORY_INTRO;
    } else if (!strcmp(name, "deck-editor") || !strcmp(name, "editor")) {
        generate_story_starter_deck();
        generate_story_storage_pool();
        reset_story_deck_editor();
        g_story_editor_from_pyramid = 0;
        g_i_state = WAIFU_I_DECK_EDITOR;
    } else if (!strcmp(name, "random-battle") || !strcmp(name, "battle")) {
        init_battle_state();
        g_story_battle_active = 0;
        g_i_state = WAIFU_I_BATTLE;
    } else if (!strcmp(name, "boss")) {
        init_battle_state();
        g_story_battle_active = 1;
        g_story_duel_index = STORY_MAX_DUELS - 2;
        g_i_state = WAIFU_I_BATTLE;
    } else if (!strcmp(name, "final-boss") || !strcmp(name, "final")) {
        init_battle_state();
        g_story_battle_active = 1;
        g_story_duel_index = STORY_MAX_DUELS - 1;
        g_i_state = WAIFU_I_BATTLE;
    } else if (!strcmp(name, "results") || !strcmp(name, "result")) {
        init_battle_state();
        g_story_battle_active = 0;
        g_i_state = WAIFU_I_BATTLE;
        g_b_result = 1;
        g_b_phase = IB_TALLY;
        g_b_phase_frame = 0;
    } else if (!strcmp(name, "reward")) {
        init_battle_state();
        g_story_battle_active = 1;
        g_story_duel_index = 1;
        g_i_state = WAIFU_I_BATTLE;
        g_b_result = 1;
        g_b_reward_card = WAIFU_CARD_ID_GARGOYLE_GIRL;
        g_b_phase = IB_REWARD;
        g_b_phase_frame = 0; /* start at 0 so the face-down -> flip-up reveal plays */
    } else if (!strcmp(name, "reward-support")) {
        init_battle_state();
        g_story_battle_active = 1;
        g_story_duel_index = 1;
        g_i_state = WAIFU_I_BATTLE;
        g_b_result = 1;
        g_b_reward_card = SUPPORT_EQUIP_CARD_ID;
        g_b_phase = IB_REWARD;
        g_b_phase_frame = 10;
    } else if (!strcmp(name, "ending")) {
        g_story_ending_line = 0;
        g_story_ending_erasing = 0;
        enter_story_ending_after_assets();
    } else if (!strcmp(name, "ending-credits") || !strcmp(name, "credits")) {
        g_i_state = WAIFU_I_STORY_ENDING_CREDITS;
    } else if (!strcmp(name, "result-sequence") || !strcmp(name, "victory-sequence")) {
        init_battle_state();
        g_story_battle_active = 0;
        g_i_state = WAIFU_I_BATTLE;
        g_i_player_field[0] = 12; g_i_player_faceup[0] = 1; g_i_player_used[0] = 0;
        g_i_player_field[2] = 37; g_i_player_faceup[2] = 1; g_i_player_used[2] = 0;
        g_i_com_field[1] = 15; g_i_com_faceup[1] = 1; g_i_com_used[1] = 0;
        g_i_com_field[3] = 28; g_i_com_faceup[3] = 1; g_i_com_used[3] = 0;
        g_b_selected_hand = 0;
        g_b_result = 1;
        set_battle_phase(IB_RESULT);
    }
    g_i_frame = 0;
    update_music_for_current_state();
}


#ifdef WAIFU_FM_HEADLESS_TESTS
static uint32_t debug_story_persistent_hash(void)
{
    uint32_t h = 0x53544f52u; /* "STOR" */
    h = waifu_hash_step_u32(h, (uint32_t)g_story_duel_index);
    h = waifu_hash_step_u32(h, (uint32_t)g_story_map_cursor);
    h = waifu_hash_step_u32(h, (uint32_t)g_story_pyramid_cursor);
    h = waifu_hash_step_u32(h, (uint32_t)g_story_plaza_line);
    h = waifu_hash_step_u32(h, (uint32_t)g_story_deck_count);
    h = waifu_hash_step_u32(h, (uint32_t)g_story_storage_count);
    for (int i = 0; i < STORY_NAME_LEN; ++i) h = waifu_hash_step_u32(h, (uint32_t)(unsigned char)g_story_name[i]);
    for (int i = 0; i < g_story_deck_count; ++i) h = waifu_hash_step_u32(h, (uint32_t)(g_story_player_deck[i] + 2));
    for (int i = 0; i < g_story_storage_count; ++i) h = waifu_hash_step_u32(h, (uint32_t)(g_story_storage[i] + 2));
    return h;
}

static void debug_prepare_story_save_fixture(void)
{
    int i;
    generate_story_starter_deck();
    generate_story_storage_pool();
    memset(g_story_name, 0, sizeof(g_story_name));
    strncpy(g_story_name, "SAVEOK", STORY_NAME_LEN);
    g_story_name[STORY_NAME_LEN] = '\0';
    g_story_duel_index = 3;
    g_story_progress = 3;
    g_story_map_cursor = 1;
    g_story_pyramid_cursor = 2;
    g_story_plaza_line = 5;
    g_story_deck_count = STORY_DECK_SIZE;
    g_story_storage_count = 9;
    for (i = 0; i < STORY_DECK_SIZE; ++i) g_story_player_deck[i] = (i * 7 + 3) % WAIFU_CARD_COUNT;
    for (i = 0; i < g_story_storage_count; ++i) g_story_storage[i] = (i * 11 + 5) % WAIFU_CARD_COUNT;
    sanitize_story_deck_copy_limit();
    recalc_story_deck_counts();
}

static void debug_setup_fusion_equip_scenario(const char *name);

static int debug_regression_story_save_roundtrip(void)
{
#ifndef WAIFU_FM_PCFX
    uint32_t before_hash, after_hash;
    remove(STORY_SAVE_PATH);
    waifu_fm_reset_interactive();
    debug_prepare_story_save_fixture();
    before_hash = debug_story_persistent_hash();
    if (!write_story_save()) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: write_story_save failed\n");
        return 1;
    }
    if (!story_save_exists()) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: save_exists false after write\n");
        return 1;
    }
    memset(g_story_name, 0, sizeof(g_story_name));
    g_story_duel_index = 0;
    g_story_progress = 0;
    g_story_map_cursor = 0;
    g_story_pyramid_cursor = 0;
    g_story_plaza_line = 0;
    g_story_deck_count = 0;
    g_story_storage_count = 0;
    memset(g_story_player_deck, 0, sizeof(g_story_player_deck));
    memset(g_story_storage, 0, sizeof(g_story_storage));
    if (!read_story_save()) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: read_story_save failed\n");
        return 1;
    }
    after_hash = debug_story_persistent_hash();
    if (before_hash != after_hash) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: hash mismatch before=%08x after=%08x\n",
                (unsigned)before_hash, (unsigned)after_hash);
        return 1;
    }
    if (g_story_battle_active != 0 || g_story_duel_index != 3 || g_story_progress != 3 ||
        g_story_map_cursor != 1 || g_story_pyramid_cursor != 2 || g_story_plaza_line != 5 ||
        g_story_deck_count != STORY_DECK_SIZE || g_story_storage_count != 9) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: bad loaded state state=%d story=%d duel=%d progress=%d map=%d pyramid=%d plaza=%d deck=%d storage=%d\n",
                (int)g_i_state, g_story_battle_active, g_story_duel_index, g_story_progress, g_story_map_cursor,
                g_story_pyramid_cursor, g_story_plaza_line, g_story_deck_count, g_story_storage_count);
        return 1;
    }
    if (!load_story_to_map() || g_i_state != WAIFU_I_STORY_MAP || g_story_save_status != 1) {
        fprintf(stderr, "REGRESSION story_save_roundtrip FAIL: load_story_to_map state=%d status=%d\n",
                (int)g_i_state, g_story_save_status);
        return 1;
    }
    printf("REGRESSION story_save_roundtrip OK hash=%08x duel=%d deck=%d storage=%d save_exists=%d\n",
           (unsigned)after_hash, g_story_duel_index, g_story_deck_count, g_story_storage_count,
           story_save_exists());
    return 0;
#else
    fprintf(stderr, "REGRESSION story_save_roundtrip SKIP: use PC-FX BackupRAM smoke for WAIFU_FM_PCFX builds\n");
    return 0;
#endif
}

static int debug_regression_story_duel_loads(void)
{
    int prev_loaded = 0;
    int ok = 1;
    waifu_fm_reset_interactive();
    waifu_assets_reset();
    generate_story_starter_deck();
    generate_story_storage_pool();
    for (int duel = 0; duel < STORY_MAX_DUELS; ++duel) {
        int guard;
        g_story_duel_index = duel;
        g_story_map_cursor = 1;
        g_story_plaza_line = 0;
        g_story_battle_active = 0;
        init_story_battle_state();
        if (waifu_fm_palette_id() != WAIFU_FM_PALETTE_COMMON) {
            fprintf(stderr, "REGRESSION story_duel_loads FAIL palette duel=%d got=%d expected=%d\n",
                    duel, (int)waifu_fm_palette_id(), (int)WAIFU_FM_PALETTE_COMMON);
            return 1;
        }
        enter_battle_after_assets();
        for (guard = 0; guard < 720 && (g_i_state == WAIFU_I_LOADING_ASSETS || !waifu_assets_ready()); ++guard) {
            WaifuFmInput in;
            memset(&in, 0, sizeof(in));
            waifu_fm_step(&in);
        }
        for (int settle = 0; settle < 8; ++settle) {
            WaifuFmInput in;
            memset(&in, 0, sizeof(in));
            waifu_fm_step(&in);
        }
        if (g_i_state != WAIFU_I_BATTLE || !waifu_assets_ready() || !waifu_assets_ram_budget_ok() ||
            !g_story_battle_active || g_story_duel_index != duel || g_story_deck_count != STORY_DECK_SIZE ||
            g_i_com_deck.count <= 0 || g_i_player_deck.count <= 0 ||
            waifu_assets_big_art_cache_loaded_count() < prev_loaded) {
            fprintf(stderr, "REGRESSION story_duel_loads FAIL duel=%d state=%d ready=%d budget_ok=%d story=%d loaded=%d prev=%d pdeck=%d cdeck=%d\n",
                    duel, (int)g_i_state, waifu_assets_ready(), waifu_assets_ram_budget_ok(),
                    g_story_battle_active, waifu_assets_big_art_cache_loaded_count(), prev_loaded,
                    g_i_player_deck.count, g_i_com_deck.count);
            ok = 0;
            break;
        }
        prev_loaded = waifu_assets_big_art_cache_loaded_count();
        printf("REGRESSION story_duel_loads duel=%d opponent=%s state=%d phase=%d cache=%d/%d ram=%lu high=%lu lp=%d OK\n",
               duel, story_opponent_name(), (int)g_i_state, (int)g_b_phase,
               waifu_assets_big_art_cache_loaded_count(), waifu_assets_big_art_cache_slot_count(),
               (unsigned long)waifu_assets_ram_used_bytes(),
               (unsigned long)waifu_assets_ram_high_water_bytes(), g_com_lp);
    }
    if (!ok) return 1;
    printf("REGRESSION story_duel_loads OK duels=%d final_cache=%d/%d high=%lu budget=%lu\n",
           STORY_MAX_DUELS,
           waifu_assets_big_art_cache_loaded_count(), waifu_assets_big_art_cache_slot_count(),
           (unsigned long)waifu_assets_ram_high_water_bytes(),
           (unsigned long)waifu_assets_ram_budget_bytes());
    return 0;
}


static int debug_regression_sanctum_editor_battle_entry(void)
{
    WaifuFmInput in;
    int guard;

    memset(&in, 0, sizeof(in));
    waifu_fm_reset_interactive();
    waifu_assets_reset();
    generate_story_starter_deck();
    generate_story_storage_pool();
    reset_story_deck_editor();

    g_story_duel_index = 1;
    g_story_progress = 1;
    g_story_map_cursor = 0;
    g_story_pyramid_cursor = 1;
    g_story_editor_from_pyramid = 1;
    g_story_battle_active = 0;
    g_i_state = WAIFU_I_DECK_EDITOR;
    g_i_frame = 0;

    in.start = 1;
    waifu_fm_step(&in);
    memset(&in, 0, sizeof(in));
    for (guard = 0; guard < 120 && g_i_state == WAIFU_I_DECK_EDITOR_TO_PYRAMID; ++guard) {
        waifu_fm_step(&in);
    }

    if (g_i_state != WAIFU_I_STORY_PYRAMID || g_story_battle_active || g_story_editor_from_pyramid ||
        g_story_duel_index != 1 || g_story_progress != 1) {
        fprintf(stderr, "REGRESSION sanctum_editor_battle_entry FAIL: sanctum return state=%d story=%d from_pyr=%d duel=%d progress=%d guard=%d\n",
                (int)g_i_state, g_story_battle_active, g_story_editor_from_pyramid,
                g_story_duel_index, g_story_progress, guard);
        return 1;
    }

    g_story_editor_from_pyramid = 0;
    g_story_battle_active = 0;
    g_i_state = WAIFU_I_DECK_EDITOR;
    g_i_frame = 0;

    in.start = 1;
    waifu_fm_step(&in);
    memset(&in, 0, sizeof(in));
    for (guard = 0; guard < 900 && (g_i_state == WAIFU_I_DECK_EDITOR_TO_BATTLE ||
                                    g_i_state == WAIFU_I_LOADING_ASSETS ||
                                    !waifu_assets_ready()); ++guard) {
        waifu_fm_step(&in);
    }
    for (int settle = 0; settle < 4; ++settle) waifu_fm_step(&in);

    if (g_i_state != WAIFU_I_BATTLE || !g_story_battle_active || g_story_editor_from_pyramid ||
        g_story_duel_index != 1 || g_story_progress != 1 ||
        g_i_player_deck.count <= 0 || g_i_com_deck.count <= 0) {
        fprintf(stderr, "REGRESSION sanctum_editor_battle_entry FAIL: story entry state=%d story=%d from_pyr=%d duel=%d progress=%d ready=%d pdeck=%d cdeck=%d guard=%d\n",
                (int)g_i_state, g_story_battle_active, g_story_editor_from_pyramid,
                g_story_duel_index, g_story_progress, waifu_assets_ready(),
                g_i_player_deck.count, g_i_com_deck.count, guard);
        return 1;
    }

    g_b_result = 1;
    g_b_phase = IB_TALLY;
    g_b_phase_frame = 20;
    memset(&in, 0, sizeof(in));
    in.start = 1;
    waifu_fm_step(&in);                       /* tally -> reward reveal */
    if (g_b_phase == IB_REWARD) {             /* claim the earned card, then continue */
        int rf;
        memset(&in, 0, sizeof(in));
        for (rf = 0; rf < 10; ++rf) waifu_fm_step(&in); /* release START, pass reveal lock */
        in.start = 1; waifu_fm_step(&in);               /* reward -> map */
    }

    if (g_i_state == WAIFU_I_TITLE || g_i_state == WAIFU_I_MENU || g_story_battle_active ||
        g_story_duel_index != 2 || g_story_progress != 2) {
        fprintf(stderr, "REGRESSION sanctum_editor_battle_entry FAIL: result returned to bad state=%d story=%d duel=%d progress=%d\n",
                (int)g_i_state, g_story_battle_active, g_story_duel_index, g_story_progress);
        return 1;
    }

    printf("REGRESSION sanctum_editor_battle_entry OK state=%d duel=%d progress=%d story=%d\n",
           (int)g_i_state, g_story_duel_index, g_story_progress, g_story_battle_active);
    return 0;
}

/* Replaying previous opponents: beating the frontier advances the story, but a
   rematch against an already-cleared opponent still pays a reward without moving
   progress, and the sanctum map lets the player cycle through unlocked foes. */
static int debug_regression_story_rematch(void)
{
    WaifuFmInput in;
    int storage_before;

    waifu_fm_reset_interactive();
    waifu_assets_reset();
    generate_story_starter_deck();
    generate_story_storage_pool();

    /* Frontier win advances progress and moves the selection forward. */
    g_story_progress = 0;
    g_story_duel_index = 0;
    g_story_battle_active = 1;
    g_b_result = 1;
    story_return_to_map_after_duel();
    if (g_story_progress != 1 || g_story_duel_index != 1 ||
        g_i_state != WAIFU_I_STORY_MAP || g_story_battle_active) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: frontier win progress=%d duel=%d state=%d active=%d\n",
                g_story_progress, g_story_duel_index, (int)g_i_state, g_story_battle_active);
        return 1;
    }

    /* Sanctum map: on the BATTLE row, LEFT/RIGHT cycles unlocked foes [0,progress]. */
    g_story_progress = 3;
    g_story_duel_index = 3;
    g_story_map_cursor = 1;
    g_i_state = WAIFU_I_STORY_MAP;
    g_i_frame = 0;
    memset(&in, 0, sizeof(in));
    in.right = 1; waifu_fm_step(&in);             /* 3 -> wrap to 0 */
    if (g_story_duel_index != 0) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: right-wrap duel=%d\n", g_story_duel_index);
        return 1;
    }
    /* A neutral frame is required between presses: held buttons are not edges. */
    memset(&in, 0, sizeof(in)); waifu_fm_step(&in);
    in.left = 1; waifu_fm_step(&in);              /* 0 -> wrap to 3 */
    if (g_story_duel_index != 3) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: left-wrap duel=%d\n", g_story_duel_index);
        return 1;
    }
    memset(&in, 0, sizeof(in)); waifu_fm_step(&in);
    in.left = 1; waifu_fm_step(&in);              /* 3 -> 2 */
    if (g_story_duel_index != 2) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: left-step duel=%d\n", g_story_duel_index);
        return 1;
    }
    memset(&in, 0, sizeof(in)); waifu_fm_step(&in);
    in.start = 1; waifu_fm_step(&in);
    memset(&in, 0, sizeof(in));
    for (int guard = 0; guard < 900 && (g_i_state == WAIFU_I_LOADING_ASSETS || !waifu_assets_ready()); ++guard) {
        waifu_fm_step(&in);
    }
    if (g_i_state != WAIFU_I_BATTLE || !g_story_battle_active ||
        g_story_duel_index != 2 || g_story_progress != 3) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: rematch entry state=%d active=%d duel=%d progress=%d ready=%d\n",
                (int)g_i_state, g_story_battle_active, g_story_duel_index, g_story_progress, waifu_assets_ready());
        return 1;
    }

    /* Rematch win: reward granted, progress unchanged. */
    g_story_storage_count = 0;
    storage_before = g_story_storage_count;
    g_story_progress = 3;
    g_story_duel_index = 1;
    g_story_battle_active = 1;
    g_b_result = 1;
    story_return_to_map_after_duel();
    if (g_story_progress != 3 || g_i_state != WAIFU_I_STORY_MAP || g_story_battle_active) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: rematch progress=%d state=%d active=%d\n",
                g_story_progress, (int)g_i_state, g_story_battle_active);
        return 1;
    }
    if (g_story_storage_count <= storage_before) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: rematch gave no reward storage=%d before=%d\n",
                g_story_storage_count, storage_before);
        return 1;
    }

    /* Losing a rematch keeps progress and the selected foe (so it can be retried). */
    g_story_progress = 3;
    g_story_duel_index = 1;
    g_story_battle_active = 1;
    g_b_result = -1;
    story_return_to_map_after_duel();
    if (g_story_progress != 3 || g_story_duel_index != 1 || g_i_state != WAIFU_I_STORY_MAP) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: loss progress=%d duel=%d state=%d\n",
                g_story_progress, g_story_duel_index, (int)g_i_state);
        return 1;
    }

    /* Clearing the final frontier triggers the ending rather than the map. */
    g_story_progress = STORY_MAX_DUELS - 1;
    g_story_duel_index = STORY_MAX_DUELS - 1;
    g_story_battle_active = 1;
    g_b_result = 1;
    story_return_to_map_after_duel();
    if (g_i_state != WAIFU_I_STORY_ENDING || g_story_battle_active) {
        fprintf(stderr, "REGRESSION story_rematch FAIL: final win state=%d active=%d\n",
                (int)g_i_state, g_story_battle_active);
        return 1;
    }

    printf("REGRESSION story_rematch OK frontier+rematch+loss+ending\n");
    return 0;
}

static int debug_regression_card_check_cache_no_cd(void)
{
#if defined(WAIFU_ASSET_USE_CDROM)
    WaifuFmInput in;
    unsigned long reads_after_load;
    unsigned long reads_after_preview;
    int guard;

    memset(&in, 0, sizeof(in));
    waifu_fm_reset_interactive();
    waifu_assets_reset();
    init_battle_state();
    request_battle_cards_for_known_decks();
    enter_state_after_assets(WAIFU_I_BATTLE);
    for (guard = 0; guard < 1800 && (g_i_state == WAIFU_I_LOADING_ASSETS || !waifu_assets_ready()); ++guard) {
        waifu_fm_step(&in);
    }
    if (g_i_state != WAIFU_I_BATTLE || !waifu_assets_ready()) {
        fprintf(stderr, "REGRESSION card_check_cache_no_cd FAIL: battle did not finish loading state=%d ready=%d guard=%d\n",
                (int)g_i_state, waifu_assets_ready(), guard);
        return 1;
    }
    if (waifu_assets_big_art_cache_loaded_count() < WAIFU_CARD_COUNT ||
        !waifu_assets_support_big_art_loaded()) {
        fprintf(stderr, "REGRESSION card_check_cache_no_cd FAIL: cache incomplete cards=%d/%d support=%d\n",
                waifu_assets_big_art_cache_loaded_count(), WAIFU_CARD_COUNT,
                waifu_assets_support_big_art_loaded());
        return 1;
    }

    waifu_assets_debug_reset_platform_read_count();
    set_battle_phase(IB_CARD_PREVIEW);
    g_b_selected_hand = next_live_hand_index(0, 1);
    for (int f = 0; f < 90; ++f) {
        waifu_fm_step(&in);
    }
    reads_after_load = waifu_assets_debug_platform_read_count();

    /* Also hit a card that is not guaranteed to be in the current hand/decks,
       mirroring fusion/story preview misses.  It must already be resident after
       the all-monster prewarm, so repeated card-check rendering cannot touch CD. */
    g_deck_preview_card = WAIFU_CARD_COUNT - 1;
    g_i_state = WAIFU_I_DECK_PREVIEW;
    g_i_frame = -1;
    for (int f = 0; f < 90; ++f) {
        waifu_fm_step(&in);
    }
    reads_after_preview = waifu_assets_debug_platform_read_count();

    if (reads_after_preview != 0 || reads_after_load != 0) {
        fprintf(stderr, "REGRESSION card_check_cache_no_cd FAIL: card-check render performed CD reads hand=%lu total=%lu\n",
                reads_after_load, reads_after_preview);
        return 1;
    }
    printf("REGRESSION card_check_cache_no_cd OK cache=%d/%d support=%d cd_reads_during_preview=%lu\n",
           waifu_assets_big_art_cache_loaded_count(), waifu_assets_big_art_cache_slot_count(),
           waifu_assets_support_big_art_loaded(), reads_after_preview);
    return 0;
#else
    printf("REGRESSION card_check_cache_no_cd SKIP: CD-ROM asset backend not enabled\n");
    return 0;
#endif
}

static int debug_regression_result_music_tracks(void)
{
    waifu_fm_reset_interactive();
    init_battle_state();
    g_i_state = WAIFU_I_BATTLE;

    g_b_result = 1;
    set_battle_phase(IB_RESULT);
    if (waifu_sound_music_track() != WAIFU_MUSIC_RANDOM_BATTLE) {
        fprintf(stderr, "REGRESSION result_music_tracks FAIL: victory preclear music=%d\n",
                (int)waifu_sound_music_track());
        return 1;
    }
    g_b_phase_frame = WAIFU_RESULT_UI_CLEAR_FRAMES;
    update_music_for_current_state();
    if (waifu_sound_music_track() != WAIFU_MUSIC_RESULTS) {
        fprintf(stderr, "REGRESSION result_music_tracks FAIL: victory music=%d\n",
                (int)waifu_sound_music_track());
        return 1;
    }

    g_b_result = -1;
    set_battle_phase(IB_RESULT);
    if (waifu_sound_music_track() != WAIFU_MUSIC_RANDOM_BATTLE) {
        fprintf(stderr, "REGRESSION result_music_tracks FAIL: loss preclear music=%d\n",
                (int)waifu_sound_music_track());
        return 1;
    }
    g_b_phase_frame = WAIFU_RESULT_UI_CLEAR_FRAMES;
    update_music_for_current_state();
    if (waifu_sound_music_track() != WAIFU_MUSIC_LOST) {
        fprintf(stderr, "REGRESSION result_music_tracks FAIL: loss music=%d\n",
                (int)waifu_sound_music_track());
        return 1;
    }

    printf("REGRESSION result_music_tracks OK preclear=%d victory=%d loss=%d\n",
           (int)WAIFU_MUSIC_RANDOM_BATTLE, (int)WAIFU_MUSIC_RESULTS, (int)WAIFU_MUSIC_LOST);
    return 0;
}

static int debug_regression_thunder_support(void)
{
    WaifuFmInput in;
    WaifuAiState ai_state;
    WaifuAiAction ai_action;
    WaifuDeck deck;
    WaifuDeckRng rng;
    int thunder_count[3] = {0, 0, 0};
    int final_angel_count = 0;
    int final_opening_thunder = 0;
    int weak_water_fusion = 0;
    int mixed_water_fusion = 0;
    int strong_water_fusion = 0;
    int random_player_thunder = 0;
    int random_com_thunder = 0;
    int guard;

    waifu_deck_rng_seed(&rng, 123u);
    waifu_deck_build_opponent_story(&deck, STORY_MAX_DUELS - 3, &rng, 0);
    for (int i = 0; i < deck.count; ++i) if (deck.cards[i] == SUPPORT_THUNDER_CARD_ID) ++thunder_count[0];
    waifu_deck_build_opponent_story(&deck, STORY_MAX_DUELS - 2, &rng, 0);
    for (int i = 0; i < deck.count; ++i) if (deck.cards[i] == SUPPORT_THUNDER_CARD_ID) ++thunder_count[1];
    waifu_deck_build_opponent_story(&deck, STORY_MAX_DUELS - 1, &rng, 0);
    for (int i = 0; i < deck.count; ++i) {
        if (deck.cards[i] == SUPPORT_THUNDER_CARD_ID) ++thunder_count[2];
        if (deck.cards[i] == WAIFU_CARD_ID_ANGEL_FISHWOMAN) ++final_angel_count;
    }
    waifu_deck_build_opponent_story(&deck, STORY_MAX_DUELS - 1, &rng, 1);
    for (int i = 0; i < 5 && i < deck.count; ++i) {
        if (deck.cards[i] == SUPPORT_THUNDER_CARD_ID) final_opening_thunder = 1;
    }
    if (thunder_count[0] != 0 || thunder_count[1] != 3 || thunder_count[2] != 3) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: deck thunder counts prefinal=%d final2=%d final=%d\n",
                thunder_count[0], thunder_count[1], thunder_count[2]);
        return 1;
    }
    if (!final_opening_thunder) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: final opening hand has no thunder\n");
        return 1;
    }
    weak_water_fusion = fusion_result_for_cards(WAIFU_CARD_ID_PENGUIN, WAIFU_CARD_ID_SLIME);
    mixed_water_fusion = fusion_result_for_cards(WAIFU_CARD_ID_THIN_BLUE_DRAGON, WAIFU_CARD_ID_EEL);
    strong_water_fusion = fusion_result_for_cards(WAIFU_CARD_ID_SEA_SERPENT, WAIFU_CARD_ID_THIN_BLUE_DRAGON);
    if (final_angel_count <= 0 ||
        weak_water_fusion != WAIFU_CARD_ID_SEA_SERPENT ||
        mixed_water_fusion != -1 ||
        strong_water_fusion != WAIFU_CARD_ID_ANGEL_FISHWOMAN) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: angel final_count=%d weak_water=%d expected=%d mixed_water=%d expected=-1 strong_water=%d expected=%d\n",
                final_angel_count,
                weak_water_fusion,
                WAIFU_CARD_ID_SEA_SERPENT,
                mixed_water_fusion,
                strong_water_fusion,
                WAIFU_CARD_ID_ANGEL_FISHWOMAN);
        return 1;
    }

    memset(&in, 0, sizeof(in));
    waifu_fm_reset_interactive();
    init_battle_state();
    for (int i = 0; i < g_i_player_deck.count; ++i) {
        if (g_i_player_deck.cards[i] == SUPPORT_THUNDER_CARD_ID) ++random_player_thunder;
    }
    for (int i = 0; i < g_i_com_deck.count; ++i) {
        if (g_i_com_deck.cards[i] == SUPPORT_THUNDER_CARD_ID) ++random_com_thunder;
    }
    if (random_player_thunder != 3 || random_com_thunder != 3) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: random thunder counts player=%d com=%d\n",
                random_player_thunder, random_com_thunder);
        return 1;
    }
    g_i_state = WAIFU_I_BATTLE;
    g_b_phase = IB_COM_SELECT;
    for (int i = 0; i < I_HAND; ++i) {
        g_i_com_hand[i] = 0;
        g_i_com_used[i] = 1;
    }
    g_i_com_hand[0] = SUPPORT_THUNDER_CARD_ID;
    g_i_com_used[0] = 0;
    build_com_ai_state(&ai_state);
    ai_action = waifu_ai_choose_com_select(&ai_state);
    if (ai_action.kind == WAIFU_AI_ACTION_PLAY_SUPPORT) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: AI used thunder with no player monsters\n");
        return 1;
    }

    g_i_player_field[0] = 15;
    g_i_player_faceup[0] = 1;
    g_i_player_defense[0] = 0;
    g_i_player_attacked[0] = 0;
    g_i_player_atk_bonus[0] = 0;
    g_i_player_def_bonus[0] = 0;
    build_com_ai_state(&ai_state);
    ai_action = waifu_ai_choose_com_select(&ai_state);
    if (ai_action.kind == WAIFU_AI_ACTION_PLAY_SUPPORT) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: AI used thunder against one weak monster\n");
        return 1;
    }

    g_i_player_field[0] = WAIFU_CARD_ID_ULTIMATE_GOLD_DRAGON;
    build_com_ai_state(&ai_state);
    ai_action = waifu_ai_choose_com_select(&ai_state);
    if (ai_action.kind != WAIFU_AI_ACTION_PLAY_SUPPORT || ai_action.hand_slot != 0) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: AI skipped thunder against one strong monster kind=%d hand=%d\n",
                (int)ai_action.kind, ai_action.hand_slot);
        return 1;
    }

    g_i_player_field[0] = 15;
    g_i_player_field[2] = 22;
    g_i_player_faceup[2] = 0;
    g_i_player_defense[2] = 1;
    g_i_player_attacked[2] = 0;
    g_i_player_atk_bonus[2] = 0;
    g_i_player_def_bonus[2] = 0;
    build_com_ai_state(&ai_state);
    ai_action = waifu_ai_choose_com_select(&ai_state);
    if (ai_action.kind != WAIFU_AI_ACTION_PLAY_SUPPORT || ai_action.hand_slot != 0) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: AI did not choose thunder kind=%d hand=%d\n",
                (int)ai_action.kind, ai_action.hand_slot);
        return 1;
    }
    start_com_thunder(0);
    if (g_b_phase != IB_COM_THUNDER_ANIM || g_b_thunder_count != 2 || !g_i_com_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: start phase=%d count=%d used=%d\n",
                (int)g_b_phase, g_b_thunder_count, g_i_com_used[0]);
        return 1;
    }
    for (guard = 0; guard < 240 && g_b_phase == IB_COM_THUNDER_ANIM; ++guard) {
        waifu_fm_step(&in);
    }
    if (g_b_phase == IB_COM_THUNDER_ANIM || is_monster_card(g_i_player_field[0]) ||
        is_monster_card(g_i_player_field[2]) || count_live_player_monsters() != 0) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: after phase=%d guard=%d p0=%d p2=%d live=%d\n",
                (int)g_b_phase, guard, g_i_player_field[0], g_i_player_field[2], count_live_player_monsters());
        return 1;
    }

    init_battle_state();
    g_i_state = WAIFU_I_BATTLE;
    g_b_phase = IB_PLAYER_HAND;
    g_i_player_hand[0] = SUPPORT_THUNDER_CARD_ID;
    g_i_player_used[0] = 0;
    g_i_com_field[0] = WAIFU_CARD_ID_ULTIMATE_GOLD_DRAGON;
    g_i_com_faceup[0] = 1;
    g_i_com_field[2] = WAIFU_CARD_ID_INSECT_SOLDIER;
    g_i_com_faceup[2] = 0;
    g_b_selected_hand = 0;
    start_player_thunder(0);
    if (g_b_phase != IB_COM_THUNDER_ANIM || g_b_thunder_owner != 0 ||
        g_b_thunder_count != 2 || !g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: player start phase=%d owner=%d count=%d used=%d\n",
                (int)g_b_phase, g_b_thunder_owner, g_b_thunder_count, g_i_player_used[0]);
        return 1;
    }
    for (guard = 0; guard < 240 && g_b_phase == IB_COM_THUNDER_ANIM; ++guard) {
        waifu_fm_step(&in);
    }
    if (g_b_phase == IB_COM_THUNDER_ANIM || is_monster_card(g_i_com_field[0]) ||
        is_monster_card(g_i_com_field[2]) || count_live_com_monsters() != 0) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: player after phase=%d guard=%d c0=%d c2=%d live=%d\n",
                (int)g_b_phase, guard, g_i_com_field[0], g_i_com_field[2], count_live_com_monsters());
        return 1;
    }

    if (!is_equip_support_card(SUPPORT_GUARD_CARD_ID) ||
        is_equip_support_card(SUPPORT_DRAW_CARD_ID) ||
        is_equip_support_card(SUPPORT_HEAL_CARD_ID) ||
        equip_atk_bonus(SUPPORT_GUARD_CARD_ID) != 250 ||
        equip_def_bonus(SUPPORT_GUARD_CARD_ID) != 800) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: support kinds guard_equip=%d draw_equip=%d heal_equip=%d guard_bonus=%d/%d\n",
                is_equip_support_card(SUPPORT_GUARD_CARD_ID),
                is_equip_support_card(SUPPORT_DRAW_CARD_ID),
                is_equip_support_card(SUPPORT_HEAL_CARD_ID),
                equip_atk_bonus(SUPPORT_GUARD_CARD_ID),
                equip_def_bonus(SUPPORT_GUARD_CARD_ID));
        return 1;
    }

    init_battle_state();
    g_i_state = WAIFU_I_BATTLE;
    g_b_phase = IB_PLAYER_HAND;
    g_you_lp = 7000;
    g_i_player_hand[0] = SUPPORT_HEAL_CARD_ID;
    g_i_player_used[0] = 0;
    start_player_one_shot_support(0);
    if (g_b_phase != IB_PLAYER_SUPPORT_ANIM || !g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: heal start phase=%d used=%d\n",
                (int)g_b_phase, g_i_player_used[0]);
        return 1;
    }
    for (guard = 0; guard < 180 && g_b_phase == IB_PLAYER_SUPPORT_ANIM; ++guard) waifu_fm_step(&in);
    if (g_b_phase == IB_PLAYER_SUPPORT_ANIM || g_you_lp != 8000 || !g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: heal after phase=%d lp=%d used=%d guard=%d\n",
                (int)g_b_phase, g_you_lp, g_i_player_used[0], guard);
        return 1;
    }

    init_battle_state();
    g_i_state = WAIFU_I_BATTLE;
    g_b_phase = IB_PLAYER_HAND;
    g_i_player_hand[0] = SUPPORT_DRAW_CARD_ID;
    g_i_player_used[0] = 0;
    g_i_player_deck_left = 1;
    start_player_one_shot_support(0);
    if (g_b_phase != IB_PLAYER_SUPPORT_ANIM || !g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: draw start phase=%d used=%d\n",
                (int)g_b_phase, g_i_player_used[0]);
        return 1;
    }
    for (guard = 0; guard < 180 && g_b_phase == IB_PLAYER_SUPPORT_ANIM; ++guard) waifu_fm_step(&in);
    if (g_b_phase == IB_PLAYER_SUPPORT_ANIM || g_i_player_hand[0] == SUPPORT_DRAW_CARD_ID ||
        g_i_player_used[0] || g_i_player_deck_left != 0) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: draw after phase=%d hand=%d used=%d deck=%d guard=%d\n",
                (int)g_b_phase, g_i_player_hand[0], g_i_player_used[0], g_i_player_deck_left, guard);
        return 1;
    }

    init_battle_state();
    g_i_state = WAIFU_I_BATTLE;
    g_b_phase = IB_PLAYER_HAND;
    g_i_player_hand[0] = SUPPORT_DRAW_CARD_ID;
    g_i_player_used[0] = 0;
    g_i_player_deck_left = 0;
    start_player_one_shot_support(0);
    if (g_b_phase == IB_PLAYER_SUPPORT_ANIM || g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: draw empty deck phase=%d used=%d\n",
                (int)g_b_phase, g_i_player_used[0]);
        return 1;
    }

    g_story_battle_active = 1;
    g_story_duel_index = STORY_MAX_DUELS - 1;
    g_i_player_field[0] = WAIFU_CARD_ID_WATER_ELEMENT;
    g_i_player_atk_bonus[0] = 0;
    g_i_player_def_bonus[0] = 0;
    g_i_player_field[1] = WAIFU_CARD_ID_INSECT_SOLDIER;
    g_i_player_atk_bonus[1] = 0;
    g_i_player_def_bonus[1] = 0;
    g_i_player_field[2] = WAIFU_CARD_ID_INSECT_BOMB;
    g_i_player_atk_bonus[2] = 0;
    g_i_player_def_bonus[2] = 0;
    if (field_card_atk(0, 0) != card_base_atk(WAIFU_CARD_ID_WATER_ELEMENT) + 500 ||
        field_card_def(0, 0) != card_base_def(WAIFU_CARD_ID_WATER_ELEMENT) + 500 ||
        field_card_atk(0, 1) != card_base_atk(WAIFU_CARD_ID_INSECT_SOLDIER) - 500 ||
        field_card_def(0, 2) != card_base_def(WAIFU_CARD_ID_INSECT_BOMB) - 500) {
        fprintf(stderr, "REGRESSION thunder_support FAIL: water field stats water=%d/%d insect=%d fire_insect_def=%d\n",
                field_card_atk(0, 0), field_card_def(0, 0), field_card_atk(0, 1), field_card_def(0, 2));
        return 1;
    }

    printf("REGRESSION thunder_support OK deck_counts=%d/%d/%d opening=%d angel=%d guard=%d\n",
           thunder_count[0], thunder_count[1], thunder_count[2], final_opening_thunder, final_angel_count, guard);
    return 0;
}

static int debug_regression_prepare_battle_cards(const char *label)
{
    WaifuFmInput in;
    int guard;

    memset(&in, 0, sizeof(in));
    waifu_fm_reset_interactive();
    init_battle_state();
    request_battle_cards_for_known_decks();
    enter_state_after_assets(WAIFU_I_BATTLE);
    for (guard = 0; guard < 1800 && g_i_state == WAIFU_I_LOADING_ASSETS; ++guard) {
        waifu_fm_step(&in);
    }
    if (g_i_state != WAIFU_I_BATTLE || !waifu_assets_ready()) {
        fprintf(stderr, "REGRESSION %s FAIL: card assets not ready state=%d ready=%d guard=%d\n",
                label, (int)g_i_state, waifu_assets_ready(), guard);
        return 0;
    }
    return 1;
}

static int debug_regression_trap_counter(void)
{
    WaifuFmInput in;
    int guard;
    int lp_before;
    int starter_traps = 0;
    memset(&in, 0, sizeof(in));

    /* The purple Trap is a distinct support kind that is never an equip/draw/
       heal/thunder, and the player's generated starter deck seeds exactly one. */
    if (!is_trap_support_card(SUPPORT_TRAP_CARD_ID) ||
        is_thunder_support_card(SUPPORT_TRAP_CARD_ID) ||
        is_equip_support_card(SUPPORT_TRAP_CARD_ID) ||
        is_draw_support_card(SUPPORT_TRAP_CARD_ID) ||
        is_heal_support_card(SUPPORT_TRAP_CARD_ID)) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: trap kind classification wrong kind=%d\n",
                support_card_kind(SUPPORT_TRAP_CARD_ID));
        return 1;
    }
    waifu_str_copy(g_story_name, (int)sizeof(g_story_name), "SERENA");
    generate_story_starter_deck();
    for (int i = 0; i < g_story_deck_count; ++i) {
        if (g_story_player_deck[i] == SUPPORT_TRAP_CARD_ID) ++starter_traps;
    }
    if (starter_traps < 1 || starter_traps > 3) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: starter deck trap count=%d (expected 1..3)\n", starter_traps);
        return 1;
    }

    /* The starter deck always holds at least one Trap, and the count follows a
       60%/35%/5% split for 1/2/3 traps. Sample many generated decks (the seed is
       derived from the player name) and check the distribution and bounds. */
    {
        const int samples = 4000;
        int dist[4] = {0, 0, 0, 0};
        char name[STORY_NAME_LEN + 1];
        for (int s = 0; s < samples; ++s) {
            int traps = 0;
            unsigned v = (unsigned)s * 2654435761u + 12345u;
            for (int n = 0; n < STORY_NAME_LEN; ++n) {
                name[n] = (char)('A' + (v % 26u));
                v = v / 26u + (unsigned)(s + n) * 7919u;
            }
            name[STORY_NAME_LEN] = '\0';
            waifu_str_copy(g_story_name, (int)sizeof(g_story_name), name);
            generate_story_starter_deck();
            for (int i = 0; i < g_story_deck_count; ++i) {
                if (g_story_player_deck[i] == SUPPORT_TRAP_CARD_ID) ++traps;
            }
            if (traps < 1 || traps > 3) {
                fprintf(stderr, "REGRESSION trap_counter FAIL: sampled deck has %d traps (expected 1..3)\n", traps);
                return 1;
            }
            ++dist[traps];
        }
        /* Expected ~2400/1400/200; allow wide bands for hash non-uniformity. */
        if (dist[1] < samples * 52 / 100 || dist[1] > samples * 68 / 100 ||
            dist[2] < samples * 27 / 100 || dist[2] > samples * 43 / 100 ||
            dist[3] < samples *  2 / 100 || dist[3] > samples * 10 / 100) {
            fprintf(stderr, "REGRESSION trap_counter FAIL: trap distribution 1=%d 2=%d 3=%d of %d\n",
                    dist[1], dist[2], dist[3], samples);
            return 1;
        }
    }
    waifu_str_copy(g_story_name, (int)sizeof(g_story_name), "SERENA");

    /* --- Scenario 1: COM attacks a face-down player monster. The trap fires
       before the battle step: COM attacker is destroyed, the player's face-down
       defender is never revealed, no damage is dealt, control returns to COM. */
    if (!debug_regression_prepare_battle_cards("trap_counter")) return 1;
    g_i_state = WAIFU_I_BATTLE;
    for (int i = 0; i < I_FIELD; ++i) { clear_monster_slot(0, i); clear_monster_slot(1, i); }
    for (int i = 0; i < I_HAND; ++i) { g_i_player_used[i] = 1; g_i_com_used[i] = 1; }
    g_b_phase = IB_COM_BATTLE;
    g_b_turns = 2;
    g_you_lp = 8000;
    clear_battle_snapshot();
    g_i_com_field[0] = WAIFU_CARD_ID_ULTIMATE_GOLD_DRAGON;
    g_i_com_faceup[0] = 1;
    g_i_com_defense[0] = 0;
    g_i_com_attacked[0] = 0;
    g_i_player_field[0] = WAIFU_CARD_ID_INSECT_SOLDIER;
    g_i_player_faceup[0] = 0;   /* face-down: must stay hidden */
    g_i_player_defense[0] = 1;
    g_i_player_hand[0] = SUPPORT_TRAP_CARD_ID;
    g_i_player_used[0] = 0;
    lp_before = g_you_lp;

    prepare_battle(1, 0, 0);
    if (g_b_phase != IB_COM_THUNDER_ANIM || !g_b_trap_counter_active ||
        g_b_thunder_count != 1 || g_b_thunder_owner != 0 ||
        g_b_thunder_slots[0] != 0 || !g_i_player_used[0]) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: trap not armed phase=%d trap=%d count=%d owner=%d slot=%d used=%d\n",
                (int)g_b_phase, g_b_trap_counter_active, g_b_thunder_count, g_b_thunder_owner,
                g_b_thunder_slots[0], g_i_player_used[0]);
        return 1;
    }
    for (guard = 0; guard < 240 && g_b_phase == IB_COM_THUNDER_ANIM; ++guard) waifu_fm_step(&in);
    if (g_b_phase != IB_COM_BATTLE || is_monster_card(g_i_com_field[0]) ||
        !is_monster_card(g_i_player_field[0]) || g_i_player_faceup[0] != 0 ||
        g_you_lp != lp_before || g_b_trap_counter_active) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: after melee phase=%d com0=%d p0=%d p0_faceup=%d lp=%d/%d guard=%d\n",
                (int)g_b_phase, g_i_com_field[0], g_i_player_field[0], g_i_player_faceup[0],
                g_you_lp, lp_before, guard);
        return 1;
    }

    /* --- Scenario 2: COM direct attack on the player's life points. The trap
       still fires and destroys the attacker; the player takes no damage. */
    if (!debug_regression_prepare_battle_cards("trap_counter")) return 1;
    g_i_state = WAIFU_I_BATTLE;
    for (int i = 0; i < I_FIELD; ++i) { clear_monster_slot(0, i); clear_monster_slot(1, i); }
    for (int i = 0; i < I_HAND; ++i) { g_i_player_used[i] = 1; g_i_com_used[i] = 1; }
    g_b_phase = IB_COM_BATTLE;
    g_you_lp = 8000;
    clear_battle_snapshot();
    g_i_com_field[1] = WAIFU_CARD_ID_DRAGON;
    g_i_com_faceup[1] = 1;
    g_i_com_defense[1] = 0;
    g_i_com_attacked[1] = 0;
    g_i_player_hand[3] = SUPPORT_TRAP_CARD_ID;
    g_i_player_used[3] = 0;
    lp_before = g_you_lp;

    prepare_direct_attack(1, 1);
    if (g_b_phase != IB_COM_THUNDER_ANIM || !g_b_trap_counter_active || !g_i_player_used[3]) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: direct trap not armed phase=%d trap=%d used=%d\n",
                (int)g_b_phase, g_b_trap_counter_active, g_i_player_used[3]);
        return 1;
    }
    for (guard = 0; guard < 240 && g_b_phase == IB_COM_THUNDER_ANIM; ++guard) waifu_fm_step(&in);
    if (g_b_phase != IB_COM_BATTLE || is_monster_card(g_i_com_field[1]) || g_you_lp != lp_before) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: after direct phase=%d com1=%d lp=%d/%d guard=%d\n",
                (int)g_b_phase, g_i_com_field[1], g_you_lp, lp_before, guard);
        return 1;
    }

    /* --- Scenario 3: no trap in hand. A COM attack must resolve as a normal
       battle (the trap must not fire and the attacker survives the cut-in). */
    if (!debug_regression_prepare_battle_cards("trap_counter")) return 1;
    g_i_state = WAIFU_I_BATTLE;
    for (int i = 0; i < I_FIELD; ++i) { clear_monster_slot(0, i); clear_monster_slot(1, i); }
    for (int i = 0; i < I_HAND; ++i) { g_i_player_used[i] = 1; g_i_com_used[i] = 1; }
    g_b_phase = IB_COM_BATTLE;
    g_you_lp = 8000;
    clear_battle_snapshot();
    g_i_com_field[0] = WAIFU_CARD_ID_DRAGON;        /* 2400 ATK */
    g_i_com_faceup[0] = 1;
    g_i_com_defense[0] = 0;
    g_i_com_attacked[0] = 0;
    g_i_player_field[0] = WAIFU_CARD_ID_RAT;        /* 900 ATK, face-up */
    g_i_player_faceup[0] = 1;
    g_i_player_defense[0] = 0;
    lp_before = g_you_lp;

    prepare_battle(1, 0, 0);
    if (g_b_phase != IB_COM_BATTLE || g_b_trap_counter_active) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: no-trap attack diverted phase=%d trap=%d\n",
                (int)g_b_phase, g_b_trap_counter_active);
        return 1;
    }
    for (guard = 0; guard < 600 && g_b_phase == IB_COM_BATTLE && g_b_battle_atk_card >= 0; ++guard) {
        waifu_fm_step(&in);
    }
    if (is_monster_card(g_i_com_field[0]) == 0) {
        fprintf(stderr, "REGRESSION trap_counter FAIL: no-trap attacker wrongly destroyed guard=%d\n", guard);
        return 1;
    }

    printf("REGRESSION trap_counter OK starter_traps=%d\n", starter_traps);
    return 0;
}

static int debug_regression_fusion_equip_only(void)
{
    debug_setup_fusion_equip_scenario("equip-last");
    if (try_queue_player_fusion_slot(0) != 1 || try_queue_player_fusion_slot(1) != 2) {
        fprintf(stderr, "REGRESSION fusion_equip_only FAIL: queue_count=%d\n", g_b_fusion_count);
        return 1;
    }
    if (!prepare_player_fusion_anim(0)) {
        fprintf(stderr, "REGRESSION fusion_equip_only FAIL: prepare failed\n");
        return 1;
    }
    if (!g_b_fusion_anim_equip_only || g_b_fusion_anim_success ||
        g_b_fusion_anim_final_card != 12 || g_b_fusion_anim_final_equip_count != 1) {
        fprintf(stderr, "REGRESSION fusion_equip_only FAIL: equip_only=%d success=%d final=%d equips=%d\n",
                g_b_fusion_anim_equip_only, g_b_fusion_anim_success,
                g_b_fusion_anim_final_card, g_b_fusion_anim_final_equip_count);
        return 1;
    }
    finish_player_fusion_anim();
    if (g_i_player_field[0] != 12 || g_i_player_equip_field[0] != SUPPORT_EQUIP_CARD_ID ||
        g_i_player_equip_target[0] != 0 || g_i_player_atk_bonus[0] != equip_atk_bonus(SUPPORT_EQUIP_CARD_ID)) {
        fprintf(stderr, "REGRESSION fusion_equip_only FAIL: field=%d equip=%d target=%d atk_bonus=%d\n",
                g_i_player_field[0], g_i_player_equip_field[0],
                g_i_player_equip_target[0], g_i_player_atk_bonus[0]);
        return 1;
    }
    printf("REGRESSION fusion_equip_only OK field=%d equip=%d atk_bonus=%d\n",
           g_i_player_field[0], g_i_player_equip_field[0], g_i_player_atk_bonus[0]);

    /* Successful fusion (two monsters) with an equip LAST in the chain: the
       equip must end up on the fused monster, not vanish. */
    debug_setup_fusion_equip_scenario("fusion-then-equip");
    if (try_queue_player_fusion_slot(0) != 1 || try_queue_player_fusion_slot(1) != 2 ||
        try_queue_player_fusion_slot(2) != 3) {
        fprintf(stderr, "REGRESSION fusion_then_equip FAIL: queue_count=%d\n", g_b_fusion_count);
        return 1;
    }
    if (!prepare_player_fusion_anim(0)) {
        fprintf(stderr, "REGRESSION fusion_then_equip FAIL: prepare failed\n");
        return 1;
    }
    if (!g_b_fusion_anim_success || g_b_fusion_anim_equip_only ||
        g_b_fusion_anim_final_card != WAIFU_CARD_ID_STONE_DRAGON ||
        g_b_fusion_anim_final_equip_count != 1) {
        fprintf(stderr, "REGRESSION fusion_then_equip FAIL: success=%d equip_only=%d final=%d equips=%d\n",
                g_b_fusion_anim_success, g_b_fusion_anim_equip_only,
                g_b_fusion_anim_final_card, g_b_fusion_anim_final_equip_count);
        return 1;
    }
    finish_player_fusion_anim();
    if (g_i_player_field[0] != WAIFU_CARD_ID_STONE_DRAGON ||
        g_i_player_equip_field[0] != SUPPORT_EQUIP_CARD_ID ||
        g_i_player_equip_target[0] != 0 ||
        g_i_player_atk_bonus[0] != equip_atk_bonus(SUPPORT_EQUIP_CARD_ID)) {
        fprintf(stderr, "REGRESSION fusion_then_equip FAIL: field=%d equip=%d target=%d atk_bonus=%d\n",
                g_i_player_field[0], g_i_player_equip_field[0],
                g_i_player_equip_target[0], g_i_player_atk_bonus[0]);
        return 1;
    }
    printf("REGRESSION fusion_then_equip OK field=%d equip=%d atk_bonus=%d\n",
           g_i_player_field[0], g_i_player_equip_field[0], g_i_player_atk_bonus[0]);

    /* Occupied-zone fusion (field monster prepended as material) with an equip
       LAST in the hand chain: field GOLEM_IDOL(12) + hand ELECTRIC(9) fuse into
       STONE_DRAGON(33), and the trailing equip must land on the result. */
    {
        int k;
        debug_setup_fusion_equip_scenario("fusion-then-equip");
        for (k = 0; k < I_HAND; ++k) g_i_player_used[k] = 1;
        g_i_player_field[0] = WAIFU_CARD_ID_GOLEM_IDOL;
        g_i_player_faceup[0] = 1;
        g_i_player_defense[0] = 0;
        g_i_player_atk_bonus[0] = 0;
        g_i_player_def_bonus[0] = 0;
        g_i_player_hand[0] = WAIFU_CARD_ID_ELECTRIC;
        g_i_player_hand[1] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
        g_b_player_monster_played_this_turn = 1;
        clear_player_fusion_queue();
        /* One monster action per turn: after a Set, neither occupied-zone
           transforms nor empty-zone summons may start. */
        if (player_can_fusion_to_slot(0) || player_can_fusion_to_slot(1)) {
            fprintf(stderr, "REGRESSION fusion_occupied_equip FAIL: target gates occupied=%d empty=%d\n",
                    player_can_fusion_to_slot(0), player_can_fusion_to_slot(1));
            return 1;
        }
        /* The fusion mechanic itself (equip lands on the result) is exercised on
           a fresh turn where the monster action has not yet been used. */
        g_b_player_monster_played_this_turn = 0;
        if (try_queue_player_fusion_slot(0) != 1 || try_queue_player_fusion_slot(1) != 2) {
            fprintf(stderr, "REGRESSION fusion_occupied_equip FAIL: queue_count=%d\n", g_b_fusion_count);
            return 1;
        }
        if (!prepare_player_fusion_anim(0)) {
            fprintf(stderr, "REGRESSION fusion_occupied_equip FAIL: prepare failed\n");
            return 1;
        }
        if (g_b_fusion_anim_final_card != WAIFU_CARD_ID_STONE_DRAGON ||
            g_b_fusion_anim_final_equip_count != 1) {
            fprintf(stderr, "REGRESSION fusion_occupied_equip FAIL: final=%d equips=%d success=%d\n",
                    g_b_fusion_anim_final_card, g_b_fusion_anim_final_equip_count, g_b_fusion_anim_success);
            return 1;
        }
        finish_player_fusion_anim();
        if (g_i_player_field[0] != WAIFU_CARD_ID_STONE_DRAGON ||
            g_i_player_equip_field[0] != SUPPORT_EQUIP_CARD_ID ||
            g_i_player_equip_target[0] != 0) {
            fprintf(stderr, "REGRESSION fusion_occupied_equip FAIL: field=%d equip=%d target=%d\n",
                    g_i_player_field[0], g_i_player_equip_field[0], g_i_player_equip_target[0]);
            return 1;
        }
        printf("REGRESSION fusion_occupied_equip OK field=%d equip=%d atk_bonus=%d\n",
               g_i_player_field[0], g_i_player_equip_field[0], g_i_player_atk_bonus[0]);
    }

    /* Occupied-zone chain fusion: two weak WATER hand monsters first assemble
       into Thalassa, then combine with the field Thalassa into Seraphina. */
    {
        int k;
        debug_setup_fusion_equip_scenario("fusion-then-equip");
        for (k = 0; k < I_HAND; ++k) g_i_player_used[k] = 1;
        g_i_player_field[0] = WAIFU_CARD_ID_SEA_SERPENT;
        g_i_player_faceup[0] = 1;
        g_i_player_defense[0] = 0;
        g_i_player_atk_bonus[0] = 0;
        g_i_player_def_bonus[0] = 0;
        g_i_player_hand[0] = WAIFU_CARD_ID_PENGUIN;
        g_i_player_hand[1] = WAIFU_CARD_ID_SLIME;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
        g_b_player_monster_played_this_turn = 0;
        clear_player_fusion_queue();
        if (try_queue_player_fusion_slot(0) != 1 || try_queue_player_fusion_slot(1) != 2) {
            fprintf(stderr, "REGRESSION fusion_occupied_thalassa_chain FAIL: queue_count=%d\n", g_b_fusion_count);
            return 1;
        }
        if (!prepare_player_fusion_anim(0)) {
            fprintf(stderr, "REGRESSION fusion_occupied_thalassa_chain FAIL: prepare failed\n");
            return 1;
        }
        if (!g_b_fusion_anim_success ||
            g_b_fusion_anim_final_card != WAIFU_CARD_ID_ANGEL_FISHWOMAN) {
            fprintf(stderr, "REGRESSION fusion_occupied_thalassa_chain FAIL: success=%d final=%d expected=%d\n",
                    g_b_fusion_anim_success,
                    g_b_fusion_anim_final_card,
                    WAIFU_CARD_ID_ANGEL_FISHWOMAN);
            return 1;
        }
        finish_player_fusion_anim();
        if (g_i_player_field[0] != WAIFU_CARD_ID_ANGEL_FISHWOMAN ||
            !g_i_player_used[0] || !g_i_player_used[1]) {
            fprintf(stderr, "REGRESSION fusion_occupied_thalassa_chain FAIL: field=%d used=%d/%d expected=%d\n",
                    g_i_player_field[0],
                    g_i_player_used[0],
                    g_i_player_used[1],
                    WAIFU_CARD_ID_ANGEL_FISHWOMAN);
            return 1;
        }
        printf("REGRESSION fusion_occupied_thalassa_chain OK field=%d\n", g_i_player_field[0]);
    }

    /* Equip queued BEFORE a later fusion in the chain (chain order
       monster, equip, monster): the fusion used to clear the just-added equip,
       so a successful summon dropped the equip entirely.  It must now survive
       the fusion and land on the result. */
    {
        debug_setup_fusion_equip_scenario("fusion-then-equip"); /* hand [12, 9, equip] */
        clear_player_fusion_queue();
        if (try_queue_player_fusion_slot(0) != 1 ||   /* GOLEM_IDOL */
            try_queue_player_fusion_slot(2) != 2 ||   /* equip (middle of chain) */
            try_queue_player_fusion_slot(1) != 3) {   /* ELECTRIC -> fuses after equip */
            fprintf(stderr, "REGRESSION fusion_equip_mid FAIL: queue_count=%d\n", g_b_fusion_count);
            return 1;
        }
        if (!prepare_player_fusion_anim(0)) {
            fprintf(stderr, "REGRESSION fusion_equip_mid FAIL: prepare failed\n");
            return 1;
        }
        if (!g_b_fusion_anim_success ||
            g_b_fusion_anim_final_card != WAIFU_CARD_ID_STONE_DRAGON ||
            g_b_fusion_anim_final_equip_count != 1) {
            fprintf(stderr, "REGRESSION fusion_equip_mid FAIL: success=%d final=%d equips=%d\n",
                    g_b_fusion_anim_success, g_b_fusion_anim_final_card, g_b_fusion_anim_final_equip_count);
            return 1;
        }
        finish_player_fusion_anim();
        if (g_i_player_field[0] != WAIFU_CARD_ID_STONE_DRAGON ||
            g_i_player_equip_field[0] != SUPPORT_EQUIP_CARD_ID ||
            g_i_player_equip_target[0] != 0 ||
            g_i_player_atk_bonus[0] != equip_atk_bonus(SUPPORT_EQUIP_CARD_ID)) {
            fprintf(stderr, "REGRESSION fusion_equip_mid FAIL: field=%d equip=%d target=%d atk_bonus=%d\n",
                    g_i_player_field[0], g_i_player_equip_field[0],
                    g_i_player_equip_target[0], g_i_player_atk_bonus[0]);
            return 1;
        }
        printf("REGRESSION fusion_equip_mid OK field=%d equip=%d atk_bonus=%d\n",
               g_i_player_field[0], g_i_player_equip_field[0], g_i_player_atk_bonus[0]);
    }
    return 0;
}

#endif /* WAIFU_FM_HEADLESS_TESTS */

static void debug_setup_asset_load_demo(const char *name)
{
    if (!name) return;
    waifu_assets_reset();
    if (!strcmp(name, "title")) {
        waifu_assets_request_title();
        enter_state_after_assets(WAIFU_I_TITLE);
    } else if (!strcmp(name, "story-intro") || !strcmp(name, "intro")) {
        g_story_intro_line = 0;
        waifu_assets_request_story_intro();
        enter_state_after_assets(WAIFU_I_STORY_INTRO);
    } else if (!strncmp(name, "story-duel", 10) || !strncmp(name, "duel", 4)) {
        const char *suffix = !strncmp(name, "story-duel", 10) ? name + 10 : name + 4;
        int duel = (*suffix == '-' || *suffix == '_') ? atoi(suffix + 1) : atoi(suffix);
        if (duel < 0) duel = 0;
        if (duel >= STORY_MAX_DUELS) duel = STORY_MAX_DUELS - 1;
        g_story_duel_index = duel;
        if (g_story_progress < duel) g_story_progress = duel;
        g_story_plaza_line = 0;
        request_story_duel_assets();
        enter_state_after_assets(WAIFU_I_STORY_PLAZA);
    } else if (!strcmp(name, "deck-editor") || !strcmp(name, "cards")) {
        generate_story_starter_deck();
        generate_story_storage_pool();
        reset_story_deck_editor();
        g_story_editor_from_pyramid = 0;
        waifu_assets_request_cards();
        enter_state_after_assets(WAIFU_I_DECK_EDITOR);
    } else if (!strcmp(name, "battle")) {
        init_battle_state();
        request_battle_cards_for_known_decks();
        enter_state_after_assets(WAIFU_I_BATTLE);
    }
    update_music_for_current_state();
}

static void debug_put_player_monster(int slot, int card, int faceup, int defense)
{
    if (slot < 0 || slot >= I_FIELD) return;
    g_i_player_field[slot] = card;
    g_i_player_faceup[slot] = faceup;
    g_i_player_defense[slot] = defense;
    g_i_player_attacked[slot] = 0;
    g_i_player_atk_bonus[slot] = 0;
    g_i_player_def_bonus[slot] = 0;
}

static void debug_put_com_monster(int slot, int card, int faceup, int defense)
{
    if (slot < 0 || slot >= I_FIELD) return;
    g_i_com_field[slot] = card;
    g_i_com_faceup[slot] = faceup;
    g_i_com_defense[slot] = defense;
    g_i_com_attacked[slot] = 0;
    g_i_com_atk_bonus[slot] = 0;
    g_i_com_def_bonus[slot] = 0;
}

static void debug_setup_fusion_equip_scenario(const char *name)
{
    int i;
    waifu_fm_reset_interactive();
    for (i = 0; i < I_FIELD; ++i) {
        clear_monster_slot(0, i);
        clear_monster_slot(1, i);
        g_i_player_equip_field[i] = CARD_NONE;
        g_i_player_equip_target[i] = -1;
    }
    for (i = 0; i < I_HAND; ++i) {
        g_i_player_hand[i] = 0;
        g_i_player_used[i] = 1;
        g_i_com_used[i] = 1;
    }

    if (!strcmp(name, "equip-reveal")) {
        g_i_player_hand[0] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_used[0] = 0;
        debug_put_player_monster(0, 12, 0, 0);
    } else if (!strcmp(name, "equips-only")) {
        g_i_player_hand[0] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_hand[1] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
    } else if (!strcmp(name, "equip-last")) {
        g_i_player_hand[0] = 12; /* Galatea. */
        g_i_player_hand[1] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
    } else if (!strcmp(name, "equip-first")) {
        g_i_player_hand[0] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_hand[1] = 12; /* Galatea. */
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
    } else if (!strcmp(name, "equip-first-two-monsters")) {
        g_i_player_hand[0] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_hand[1] = 12; /* Galatea gets the equip, then is discarded on failed pair. */
        g_i_player_hand[2] = 15; /* Mirelle remains. */
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
        g_i_player_used[2] = 0;
    } else if (!strcmp(name, "fusion-then-equip")) {
        g_i_player_hand[0] = 12; /* Galatea. */
        g_i_player_hand[1] = 9;  /* Voltara -> Petra. */
        g_i_player_hand[2] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
        g_i_player_used[2] = 0;
    } else {
        g_i_player_hand[0] = SUPPORT_EQUIP_CARD_ID;
        g_i_player_hand[1] = 12;
        g_i_player_used[0] = 0;
        g_i_player_used[1] = 0;
    }

    g_i_state = WAIFU_I_BATTLE;
    g_i_frame = 0;
    g_b_phase = IB_PLAYER_HAND;
    g_b_phase_frame = 60;
    g_b_player_hand_intro_pending = 0;
    g_b_selected_hand = 0;
    g_b_selected_player_slot = 0;
    g_b_top_col = 0;
    g_b_top_row = PLAYER_CARD_ROW;
    g_b_player_monster_played_this_turn = 0;
    g_b_player_fused_this_turn = 0;
    g_b_com_monster_played_this_turn = 0;
    clear_player_fusion_queue();
    clear_battle_snapshot();
    if (!strcmp(name, "equip-reveal")) {
        start_equip(0, 0, 0);
    }
}

static void debug_setup_ai_demo_scenario(const char *name)
{
    int i;
    waifu_fm_reset_interactive();
    for (i = 0; i < I_FIELD; ++i) {
        clear_monster_slot(0, i);
        clear_monster_slot(1, i);
    }
    for (i = 0; i < I_HAND; ++i) {
        g_i_player_used[i] = 1;
        g_i_com_used[i] = 1;
    }
    g_i_state = WAIFU_I_BATTLE;
    g_i_frame = 0;
    g_b_turns = 2;
    g_b_phase_frame = 0;
    g_b_com_monster_played_this_turn = 1;
    g_b_player_monster_played_this_turn = 0;
    g_b_player_fused_this_turn = 0;
    clear_battle_snapshot();

    if (!strcmp(name, "field-used-dither")) {
        debug_put_player_monster(0, 15, 1, 0);
        debug_put_player_monster(1, 22, 1, 0);
        debug_put_player_monster(2, 36, 1, 0);
        g_i_player_attacked[0] = 1;
        g_i_player_attacked[1] = 1;
        g_i_player_attacked[2] = 0;
        g_b_selected_player_slot = 1;
        g_b_top_col = 1;
        g_b_top_row = PLAYER_CARD_ROW;
        g_b_phase = IB_PLAYER_TOP;
    } else if (!strcmp(name, "direct-switch")) {
        debug_put_com_monster(0, 15, 0, 1); /* 1500 ATK, starts in defense. */
        debug_put_com_monster(1, 22, 0, 1); /* 1600 ATK, starts in defense. */
        debug_put_com_monster(2, 36, 0, 1); /* 2300 ATK, starts in defense. */
        g_b_phase = IB_COM_BATTLE;
    } else if (!strcmp(name, "defense-guard")) {
        debug_put_player_monster(0, 40, 0, 0); /* Face-down attack-position threat. */
        debug_put_com_monster(0, 15, 0, 0);    /* Below deck-strength threshold. */
        g_b_phase = IB_COM_BATTLE;
    } else {
        debug_put_player_monster(0, 15, 0, 0); /* Face-down attack-position target. */
        debug_put_player_monster(1, 22, 0, 0); /* Another face-down attacker. */
        debug_put_com_monster(0, 37, 0, 0);    /* Strong face-down attacker. */
        debug_put_com_monster(1, 40, 0, 0);    /* Strong face-down attacker. */
        debug_put_com_monster(2, 36, 0, 0);    /* Strong enough for its deck. */
        g_b_phase = IB_COM_BATTLE;
    }
}

int main(int argc, char **argv)
{
    int frames = 3600;
    int dump_every = 1;
    int showcase = 0;
    int scripted_render = 0;
    const char *out_dir = "headless_frames";
    const char *record_mkv = NULL;
    const char *record_wav = NULL;
    const char *commands_path = NULL;
    int no_png = 0;
    int dump_state = 0;
    int story_cutscene_preview = -1;
    const char *ai_demo_scenario = NULL;
    const char *fusion_equip_scenario = NULL;
    const char *music_demo_state = NULL;
    const char *asset_load_demo = NULL;
    int regression_story_save = 0;
    int regression_story_duels = 0;
    int regression_card_check = 0;
    int regression_result_music = 0;
    int regression_sanctum_entry = 0;
    int regression_story_rematch = 0;
    int regression_thunder_support = 0;
    int regression_trap_counter = 0;
    int regression_fusion_equip = 0;
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
        else if (!strcmp(argv[i], "--record-wav") && i + 1 < argc) record_wav = argv[++i];
        else if (!strcmp(argv[i], "--no-png")) no_png = 1;
        else if (!strcmp(argv[i], "--dump-state")) dump_state = 1;
        else if (!strcmp(argv[i], "--story-cutscene-preview") && i + 1 < argc) story_cutscene_preview = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ai-demo-scenario") && i + 1 < argc) ai_demo_scenario = argv[++i];
        else if (!strcmp(argv[i], "--fusion-equip-scenario") && i + 1 < argc) fusion_equip_scenario = argv[++i];
        else if (!strcmp(argv[i], "--music-demo-state") && i + 1 < argc) music_demo_state = argv[++i];
        else if (!strcmp(argv[i], "--asset-load-demo") && i + 1 < argc) asset_load_demo = argv[++i];
#ifdef WAIFU_FM_HEADLESS_TESTS
        else if (!strcmp(argv[i], "--regression-story-save")) regression_story_save = 1;
        else if (!strcmp(argv[i], "--regression-story-duels")) regression_story_duels = 1;
        else if (!strcmp(argv[i], "--regression-card-check-cache")) regression_card_check = 1;
        else if (!strcmp(argv[i], "--regression-result-music")) regression_result_music = 1;
        else if (!strcmp(argv[i], "--regression-sanctum-entry")) regression_sanctum_entry = 1;
        else if (!strcmp(argv[i], "--regression-story-rematch")) regression_story_rematch = 1;
        else if (!strcmp(argv[i], "--regression-thunder-support")) regression_thunder_support = 1;
        else if (!strcmp(argv[i], "--regression-trap-counter")) regression_trap_counter = 1;
        else if (!strcmp(argv[i], "--regression-fusion-equip")) regression_fusion_equip = 1;
        else if (!strcmp(argv[i], "--regression-story-all")) { regression_story_save = 1; regression_story_duels = 1; regression_card_check = 1; regression_result_music = 1; regression_sanctum_entry = 1; regression_story_rematch = 1; regression_thunder_support = 1; regression_trap_counter = 1; regression_fusion_equip = 1; }
#endif
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
        else if (!strcmp(argv[i], "--profile-render")) g_profile_render_enabled = 1;
#endif
        else if (!strcmp(argv[i], "--deckout-demo")) g_force_deckout_demo = 1;
        else if (!strcmp(argv[i], "--lp-loss-demo")) g_force_lp_loss_demo = 1;
    }
    if (frames < 1) frames = 1;
    if (dump_every < 1) dump_every = 1;

    waifu_fm_init();

#ifdef WAIFU_FM_HEADLESS_TESTS
    if (regression_story_save || regression_story_duels || regression_card_check || regression_result_music || regression_sanctum_entry || regression_story_rematch || regression_thunder_support || regression_trap_counter || regression_fusion_equip) {
        int rc = 0;
        if (regression_story_save) rc |= debug_regression_story_save_roundtrip();
        if (regression_story_duels) rc |= debug_regression_story_duel_loads();
        if (regression_card_check) rc |= debug_regression_card_check_cache_no_cd();
        if (regression_result_music) rc |= debug_regression_result_music_tracks();
        if (regression_sanctum_entry) rc |= debug_regression_sanctum_editor_battle_entry();
        if (regression_story_rematch) rc |= debug_regression_story_rematch();
        if (regression_thunder_support) rc |= debug_regression_thunder_support();
        if (regression_trap_counter) rc |= debug_regression_trap_counter();
        if (regression_fusion_equip) rc |= debug_regression_fusion_equip_only();
        return rc ? 1 : 0;
    }
#endif

    if (showcase) {
        write_showcase(out_dir);
        return 0;
    }

    if (commands_path) {
        event_count = load_command_file(commands_path, events, MAX_COMMAND_EVENTS);
        if (event_count < 0) return 1;
    }

    zmbv_mkv_recorder_t *rec = NULL;
    WaifuSoundWavWriter wav;
    int wav_open = 0;
    int16_t audio_frame[WAIFU_SOUND_SAMPLES_PER_FRAME * WAIFU_SOUND_CHANNELS];
    if (record_mkv) {
        char err[256] = {0};
        rec = zmbv_mkv_open(record_mkv, WAIFU_FM_WIDTH, WAIFU_FM_HEIGHT, 60, 1, err, sizeof(err));
        if (!rec) {
            fprintf(stderr, "record-mkv failed: %s\n", err[0] ? err : "unknown error");
            return 1;
        }
    }
    if (record_wav) {
        if (!waifu_sound_wav_open(&wav, record_wav)) {
            fprintf(stderr, "record-wav failed: %s\n", record_wav);
            if (rec) zmbv_mkv_abort(rec);
            return 1;
        }
        wav_open = 1;
    }

    if (!no_png) ensure_dir(out_dir);
    waifu_fm_reset_interactive();
    if (story_cutscene_preview >= 0) debug_jump_to_story_cutscene(story_cutscene_preview);
    if (fusion_equip_scenario) debug_setup_fusion_equip_scenario(fusion_equip_scenario);
    if (ai_demo_scenario) debug_setup_ai_demo_scenario(ai_demo_scenario);
    if (music_demo_state) debug_setup_music_demo_state(music_demo_state);
    if (asset_load_demo) debug_setup_asset_load_demo(asset_load_demo);
    for (f = 0; f < frames; ++f) {
        if (scripted_render) {
            render_frame(f);
        } else {
            WaifuFmInput in = input_for_frame_from_events(f, events, event_count);
            waifu_fm_step(&in);
        }
        if (rec && !zmbv_mkv_add_indexed_frame(rec, framebuffer, waifu_fm_palette_rgb(), (uint64_t)f)) {
            fprintf(stderr, "record-mkv frame %d failed: %s\n", f, zmbv_mkv_error(rec));
            zmbv_mkv_abort(rec);
            return 1;
        }
        if (!no_png && (f % dump_every) == 0) write_frame_png(out_dir, f);
        if (wav_open) {
            waifu_fm_audio_mix_s16(audio_frame, WAIFU_SOUND_SAMPLES_PER_FRAME);
            if (!waifu_sound_wav_write(&wav, audio_frame, WAIFU_SOUND_SAMPLES_PER_FRAME)) {
                fprintf(stderr, "record-wav write failed at frame %d\n", f);
                waifu_sound_wav_abort(&wav);
                if (rec) zmbv_mkv_abort(rec);
                return 1;
            }
        }
    }
    if (wav_open) {
        if (!waifu_sound_wav_close(&wav)) {
            fprintf(stderr, "record-wav close failed\n");
            if (rec) zmbv_mkv_abort(rec);
            return 1;
        }
        printf("wrote %s (%d Hz stereo PCM, %d frames)\n", record_wav, WAIFU_SOUND_SAMPLE_RATE, frames);
    }
    if (rec) {
        if (!zmbv_mkv_close(rec)) {
            fprintf(stderr, "record-mkv close failed\n");
            return 1;
        }
        printf("wrote %s (%d ZMBV MKV frames at 256x240)\n", record_mkv, frames);
    }
    if (dump_state) {
        printf("STATE frame=%d state=%d music=%s music_id=%d phase=%d phase_frame=%d turns=%d you_lp=%d com_lp=%d ",
               frames, (int)g_i_state, waifu_fm_audio_music_name(), (int)waifu_fm_audio_music_track(), (int)g_b_phase, g_b_phase_frame, g_b_turns, g_you_lp, g_com_lp);
        printf("player_field0=%d player_field1=%d player_field2=%d player_field3=%d player_field4=%d ",
               g_i_player_field[0], g_i_player_field[1], g_i_player_field[2], g_i_player_field[3], g_i_player_field[4]);
        printf("player_faceup0=%d player_defense0=%d com_field0=%d com_field1=%d com_field2=%d com_faceup0=%d com_defense0=%d com_defense1=%d com_defense2=%d player_attacked0=%d com_attacked0=%d com_attacked1=%d com_attacked2=%d ",
               g_i_player_faceup[0], g_i_player_defense[0], g_i_com_field[0], g_i_com_field[1], g_i_com_field[2], g_i_com_faceup[0],
               g_i_com_defense[0], g_i_com_defense[1], g_i_com_defense[2],
               g_i_player_attacked[0], g_i_com_attacked[0], g_i_com_attacked[1], g_i_com_attacked[2]);
        printf("player_atk0=%d player_def0=%d player_atk_bonus0=%d player_def_bonus0=%d ",
               field_card_atk(0, 0), field_card_def(0, 0), g_i_player_atk_bonus[0], g_i_player_def_bonus[0]);
        printf("player_equip0=%d player_equip_target0=%d com_equip0=%d com_equip_target0=%d ",
               g_i_player_equip_field[0], g_i_player_equip_target[0], g_i_com_equip_field[0], g_i_com_equip_target[0]);
        printf("player_monster_played=%d player_fused=%d com_monster_played=%d result=%d top_col=%d top_row=%d attack_target=%d preview_card=%d ",
                g_b_player_monster_played_this_turn, g_b_player_fused_this_turn, g_b_com_monster_played_this_turn, g_b_result,
                g_b_top_col, g_b_top_row, g_b_attack_attacker_slot, g_b_preview_card_id);
        printf("istate=%d story=%d story_line=%d story_fire_line=%d story_name=%s story_strong=%d story_weak=%d story_equips=%d story_supports=%d story_total_supports=%d ",
               (int)g_i_state, g_story_battle_active, g_story_intro_line, g_story_fire_line, g_story_name,
               g_story_strong_card, g_story_weak_card, g_story_equip_count, g_story_support_count,
               g_story_equip_count + g_story_support_count);
        printf("deck_count=%d storage_count=%d max_deck_copies=%d deck_tab=%d deck_cursor=%d story_duel=%d map_cursor=%d pyramid_cursor=%d plaza_line=%d editor_from_pyramid=%d save_status=%d save_exists=%d opponent=%s ",
               g_story_deck_count, g_story_storage_count, story_deck_max_card_copies(), g_deck_tab, g_deck_cursor,
               g_story_duel_index, g_story_map_cursor, g_story_pyramid_cursor, g_story_plaza_line,
               g_story_editor_from_pyramid, g_story_save_status, story_save_exists(), story_opponent_name());
        printf("asset_backend=%s asset_req=%s asset_ready=%d title_ready=%d cards_ready=%d serena_ready=%d opp_ready=%d asset_ram=%lu asset_high=%lu asset_budget=%lu asset_budget_ok=%d bigcache_loaded=%d bigcache_slots=%d ",
               waifu_assets_backend_name(), waifu_assets_request_name(waifu_assets_pending_request()),
               waifu_assets_ready(), waifu_assets_title_ready(), waifu_assets_cards_ready(),
               waifu_assets_story_portrait_ready(STORY_PORTRAIT_SERENA),
               waifu_assets_story_portrait_ready(story_opponent_info()->portrait_id),
               (unsigned long)waifu_assets_ram_used_bytes(),
               (unsigned long)waifu_assets_ram_high_water_bytes(),
               (unsigned long)waifu_assets_ram_budget_bytes(),
               waifu_assets_ram_budget_ok(),
               waifu_assets_big_art_cache_loaded_count(),
               waifu_assets_big_art_cache_slot_count());
        printf("hand0=%d hand1=%d hand2=%d hand3=%d hand4=%d used0=%d used1=%d used2=%d used3=%d used4=%d fusion_count=%d fusion0=%d fusion1=%d fusion2=%d fusion3=%d fusion4=%d deck_pos=%d deck_left=%d\n",
               g_i_player_hand[0], g_i_player_hand[1], g_i_player_hand[2], g_i_player_hand[3], g_i_player_hand[4],
               g_i_player_used[0], g_i_player_used[1], g_i_player_used[2], g_i_player_used[3], g_i_player_used[4],
               g_b_fusion_count, g_b_fusion_hand_slots[0], g_b_fusion_hand_slots[1], g_b_fusion_hand_slots[2], g_b_fusion_hand_slots[3], g_b_fusion_hand_slots[4],
               g_story_player_deck_pos, g_i_player_deck_left);
    }
#if defined(WAIFU_FM_HEADLESS_TESTS) && defined(WAIFU_PROFILE_RENDER)
    if (g_profile_render_enabled) {
        double hand_avg_us = g_profile_hand_calls ? (double)g_profile_hand_total_us / (double)g_profile_hand_calls : 0.0;
        double hand_card_avg_us = g_profile_hand_card_draws ? (double)g_profile_hand_card_us / (double)g_profile_hand_card_draws : 0.0;
        double cards_per_hand = g_profile_hand_calls ? (double)g_profile_hand_card_draws / (double)g_profile_hand_calls : 0.0;
        double board_hit_copy_avg_us = g_profile_board_cache_hits ? (double)g_profile_board_cache_copy_us / (double)(g_profile_board_cache_hits + g_profile_board_cache_misses) : 0.0;
        double board_miss_render_avg_us = g_profile_board_cache_misses ? (double)g_profile_board_cache_render_us / (double)g_profile_board_cache_misses : 0.0;
        double card2d_generic_avg_us = g_profile_card2d_generic_calls ? (double)g_profile_card2d_generic_us / (double)g_profile_card2d_generic_calls : 0.0;
        printf("PROFILE_RENDER board_cache_hits=%llu board_cache_misses=%llu board_render_us=%llu board_copy_us=%llu board_miss_render_avg_us=%.2f board_copy_avg_us=%.2f ",
               g_profile_board_cache_hits, g_profile_board_cache_misses,
               g_profile_board_cache_render_us, g_profile_board_cache_copy_us,
               board_miss_render_avg_us, board_hit_copy_avg_us);
        printf("hand_calls=%llu hand_total_us=%llu hand_avg_us=%.2f hand_card_draws=%llu hand_card_us=%llu hand_card_avg_us=%.2f cards_per_hand=%.2f ",
               g_profile_hand_calls, g_profile_hand_total_us, hand_avg_us,
               g_profile_hand_card_draws, g_profile_hand_card_us, hand_card_avg_us, cards_per_hand);
        printf("card2d_fast=%llu card2d_generic=%llu card2d_generic_us=%llu card2d_generic_avg_us=%.2f ui_fast_fills=%llu\n",
               g_profile_card2d_fast_calls, g_profile_card2d_generic_calls,
               g_profile_card2d_generic_us, card2d_generic_avg_us, g_profile_ui_fast_fill_calls);
    }
#endif
    return 0;
}
#endif /* WAIFU_FM_NO_HEADLESS_MAIN */
