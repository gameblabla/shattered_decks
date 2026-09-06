/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_scene.c — scene dispatch, plus the bring-up self test.
 *
 *  atarist_main.c owns the frame loop and knows nothing about what is on
 *  screen; this decides which scene is running and hands the next one its
 *  state.  Scenes never call each other.
 *
 *  The bring-up scene is not a placeholder for its own sake: it is what proved,
 *  on a blind headless run, that the four things the rest of the port sits on
 *  were working -- the chunky->planar path into the top half, direct planar
 *  drawing into the bottom half, the Timer B raster split, and the IKBD latch.
 *  It stays in the tree behind ATARIST_DEBUG_BRINGUP for exactly that reason.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdint.h>

#include "atarist_scene.h"
#include "atarist_video.h"
#include "atarist_input.h"
#include "atarist_probe.h"
#include "atarist_assets.h"
#include "atarist_duel.h"
#include "atarist_title.h"

#include "msxgl.h"
#include "msx2_duel.h"

#ifndef ATARIST_DEBUG_BRINGUP
#define ATARIST_DEBUG_BRINGUP 0
#endif

static uint8_t g_scene;
static uint16_t g_anim;
/* How far the player has got in story mode.  It is not saved anywhere yet --
 * there is no save file on this target -- so it lasts as long as the session. */
static uint8_t g_story_progress;

/* A seed that is unpredictable to a player and repeatable to the harness: the
 * vblank counter at the moment the duel opens, which a scripted run reaches on
 * a fixed frame. */
static uint32_t scene_seed(void)
{
    return g_atarist_vbl * 2654435761u + 1u;
}

#if ATARIST_DEBUG_BRINGUP
/* Two visibly different sixteens, so a screenshot shows at a glance whether the
 * split fired: the board half is a warm desert ramp, the card half a cool one. */
static void Atarist_BringupPalettes(void)
{
    uint16_t arena[16], card[16];
    int i;
    for (i = 0; i < 16; ++i) {
        int t = i * 17;
        arena[i] = Atarist_PackRGB((uint8_t)t,
                                   (uint8_t)((t * 3) / 4),
                                   (uint8_t)(t / 3));
        card[i]  = Atarist_PackRGB((uint8_t)(t / 3),
                                   (uint8_t)((t * 3) / 4),
                                   (uint8_t)t);
    }
    Atarist_SetArenaPalette(arena);
    Atarist_SetCardPalette(card);
    Atarist_SetSplitEnabled(1);
}

/* A moving gradient in the chunky buffer at half resolution, so the doubling
 * C2P is the thing under test. */
static void Atarist_BringupBoard(void)
{
    uint8_t *chunky = Atarist_Chunky();
    int x, y;
    for (y = 0; y < ATARIST_SPLIT_Y / 2; ++y) {
        uint8_t *row = chunky + y * ATARIST_CHUNKY_STRIDE;
        for (x = 0; x < ATARIST_SCREEN_W / 2; ++x) {
            int v = ((x >> 2) + (y >> 2) + (g_anim >> 1)) & 15;
            /* The chunky buffer holds index*4 -- see atarist_c2p.S. */
            row[x] = (uint8_t)(v << 2);
        }
    }
    Atarist_C2P_Double(chunky, Atarist_BackBuffer(), ATARIST_SPLIT_Y / 2,
                       ATARIST_CHUNKY_STRIDE);
}

/* Sixteen planar colour bars plus an input read-out, drawn straight into the
 * back buffer at full 320x200 resolution -- the path card art will use. */
static void Atarist_BringupCards(void)
{
    uint8_t *back = Atarist_BackBuffer();
    int y, g, bit;

    for (y = ATARIST_SPLIT_Y; y < ATARIST_SCREEN_H; ++y) {
        uint16_t *row = (uint16_t *)(back + y * ATARIST_SCREEN_STRIDE);
        for (g = 0; g < ATARIST_SCREEN_W / 16; ++g) {
            int colour = g & 15;
            /* Below the input read-out band, one colour per 16-pixel group. */
            if (y >= ATARIST_SCREEN_H - 16) {
                uint16_t held = g_atarist_input.held;
                colour = (g < 10 && (held & (1u << g))) ? 15 : 0;
            }
            for (bit = 0; bit < 4; ++bit)
                *row++ = (colour & (1 << bit)) ? 0xffffu : 0u;
        }
    }
}
#endif /* ATARIST_DEBUG_BRINGUP */

static void enter_title(void)
{
    g_scene = ATARIST_SCENE_TITLE;
    Atarist_TitleEnter();
    g_atarist_probe.scene = g_scene;
}

static void enter_duel(uint8_t story_index)
{
    g_scene = ATARIST_SCENE_DUEL;
    ATARIST_STAGE(ATARIST_STAGE_DUEL);
    Atarist_DuelEnter(scene_seed(), story_index);
    g_atarist_probe.scene = g_scene;
}

void Atarist_SceneInit(void)
{
    g_anim = 0;
    g_story_progress = 0;
#if ATARIST_DEBUG_BRINGUP
    g_scene = ATARIST_SCENE_BRINGUP;
    Atarist_BringupPalettes();
    g_atarist_probe.scene = g_scene;
#else
    enter_title();
#endif
}

void Atarist_SceneStep(int vblanks)
{
    g_anim = (uint16_t)(g_anim + vblanks);
    switch (g_scene) {
    case ATARIST_SCENE_TITLE:
        Atarist_TitleStep(vblanks);
        switch (Atarist_TitleChoice()) {
        case ATARIST_TITLE_FREE_BATTLE:
            enter_duel(MSX2_STORY_NONE);
            break;
        case ATARIST_TITLE_STORY:
            enter_duel(g_story_progress);
            break;
        default:
            break;
        }
        break;

    case ATARIST_SCENE_DUEL:
        Atarist_DuelStep(vblanks);
        if (Atarist_DuelFinished()) {
            if (Atarist_DuelResult() > 0) {
                ++g_atarist_probe.wins_player;
                if (g_story_progress < MSX2_STORY_MAX_DUELS - 1)
                    ++g_story_progress;
            } else {
                ++g_atarist_probe.wins_com;
            }
            ++g_atarist_probe.duels;
            enter_title();
        }
        break;

#if ATARIST_DEBUG_BRINGUP
    default:
        Atarist_BringupBoard();
        Atarist_BringupCards();
        break;
#endif
    default:
        break;
    }
    g_atarist_probe.scene = g_scene;
}
