/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_main.c — entry point and the scene machine.
 *
 *  src/main.c is never compiled for this target; this is the SNES fork's own
 *  top level, the way src/msx2/msx2_main.c and src/atarist/atarist_main.c are
 *  for theirs.  What it shares with them is the rules model
 *  (src/msx2/msx2_duel.c) and the shared AI and deck construction, which are
 *  compiled in unchanged.
 * ───────────────────────────────────────────────────────────────────────────── */
#include <snes.h>
#include "snes_video.h"
#include "snes_scene.h"
#include "snes_stamp.h"
#include "snes_duel.h"
#include "snes_obj.h"
#include "snes_deck.h"

static u8  cur_scene = SNES_SCENE_BOOT;
static u16 scene_frames = 0;   /* explicit: see snes_duel.c on .bss */
static u8  screen_on_pending = 0;
static u8  scene_pending = SNES_SCENE_BOOT;

u8  snesSceneCurrent(void) { return cur_scene; }
u16 snesSceneFrames(void)  { return scene_frames; }

void snesSceneSet(u8 scene)
{
    cur_scene = scene;
    scene_frames = 0;

    /* Every boundary is a mode change, and a mode change is a bulk VRAM and
     * CGRAM rewrite, so it happens under force blank -- never during active
     * display.  See SNES_PORT_PLAN.md §11. */
    switch (scene) {
    case SNES_SCENE_TITLE:
        screen_on_pending = 0;
        snesTitleInit();
        break;
    case SNES_SCENE_STORY_TALK:
        snesStoryInit();
        screen_on_pending = 1;
        break;
    case SNES_SCENE_DECK:
        snesDeckInit();
        screen_on_pending = 1;
        break;
    case SNES_SCENE_DUEL:
        snesVideoInitDuel();
        snesObjInit();
        snesDuelEnter();
        /* Do not let the matrix/colour HDMA consume the long force-blanked
         * asset upload.  Prime it only after the framebuffer and sprite data
         * are ready, immediately before the first visible frame. */
        snesVideoRestartHdma();
        /* A Mode 7/HDMA scene can be entered from an active Mode 3 frame.
         * Keep force blank through that remainder so HDMA reloads its tables
         * on the next frame boundary instead of beginning halfway through a
         * scanline table. */
        screen_on_pending = 1;
        break;
    case SNES_SCENE_ENDING:
        snesEndingInit();
        screen_on_pending = 1;
        break;
    default:
        snesTitleInit();
        break;
    }
}

void snesSceneRun(void)
{
    u8 next = SNES_SCENE_COUNT;

    switch (cur_scene) {
    case SNES_SCENE_TITLE:
        next = snesTitleFrame();
        break;
    case SNES_SCENE_STORY_TALK:
        next = snesStoryFrame();
        break;
    case SNES_SCENE_DUEL:
        snesDuelFrame();
        break;
    case SNES_SCENE_ENDING:
        next = snesEndingFrame();
        break;
    case SNES_SCENE_DECK:
        next = snesDeckFrame();
        break;
    default:
        break;
    }
    if (next < SNES_SCENE_COUNT) scene_pending = next;
    ++scene_frames;
}

int main(void)
{
    consoleInit();
    snesStampInit();

    snesSceneSet(SNES_SCENE_TITLE);

    while (1) {
        padsCurrent(0);
        snesSceneRun();

        g_stamp.scene = cur_scene;
        g_stamp.frames = scene_frames;
        g_stamp.board_res = snesVideoBoardRes();
        g_stamp.deck_slot = snesDeckActiveSlot();
        g_stamp.deck_count = snesDeckCount();
        g_stamp.deck_head = snesDeckHead();
        g_stamp.storage_count = snesDeckStorageCount();
        g_stamp.save_valid = snesDeckSaveValid();
        snesStampCommit();

        WaitForVBlank();
        if (scene_pending != SNES_SCENE_BOOT) {
            u8 next = scene_pending;
            scene_pending = SNES_SCENE_BOOT;
            /* Scene changes that upload a complete screen happen at this
             * boundary, never halfway through the title's active display. */
            snesSceneSet(next);
        }
        if (screen_on_pending) {
            /* Re-prime the table at the actual frame boundary.  This is
             * needed when a Mode 3 title handed control over during active
             * display; the initial boot path already starts at a boundary. */
            if (cur_scene == SNES_SCENE_DUEL)
                snesVideoRestartHdma();
            else if (cur_scene == SNES_SCENE_STORY_TALK ||
                     cur_scene == SNES_SCENE_ENDING)
                snesVideoRestartSceneHdma();
            else if (cur_scene == SNES_SCENE_DECK)
                snesVideoRestartDeckHdma();
            setScreenOn();
            screen_on_pending = 0;
        }
        if (cur_scene == SNES_SCENE_DUEL) {
            /* OAM and the card tiles FIRST, then whatever the bitmap's staged
             * upload wants from what is left of the window: the sprite layer
             * is the HUD, and a HUD that arrives a vblank late while the board
             * finishes is the wrong way round. */
            snesObjVblank();
            snesVideoPresent();
        } else if (cur_scene == SNES_SCENE_STORY_TALK ||
                   cur_scene == SNES_SCENE_ENDING) {
            snesSceneVblank();
        } else if (cur_scene == SNES_SCENE_DECK) {
            snesDeckVblank();
        }
    }
    return 0;
}
