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
#include "snes_audio.h"
#include "snes_cardart.h"
#include "snes_battle.h"

static u8  cur_scene = SNES_SCENE_BOOT;
static u16 scene_frames = 0;   /* explicit: see snes_duel.c on .bss */
static u8  screen_on_pending = 0;
static u8  scene_pending = SNES_SCENE_BOOT;

u8  snesSceneCurrent(void) { return cur_scene; }
u16 snesSceneFrames(void)  { return scene_frames; }

void snesSceneSet(u8 scene)
{
    u8 initial_title = (cur_scene == SNES_SCENE_BOOT &&
                        scene == SNES_SCENE_TITLE);
    cur_scene = scene;
    scene_frames = 0;

    /* Music is a scene resource just like the bitmap.  Keep the display in
     * force blank while the SPC stream replaces the resident song data. */
    if (scene != SNES_SCENE_TITLE) setScreenOff();
    /* The title is already resident from snesAudioInit(); avoid re-streaming
     * it during the first scene setup, while still restoring it on a later
     * return from the game. */
    if (!initial_title) {
        switch (scene) {
        case SNES_SCENE_DUEL:
            snesAudioPlay(SNES_AUDIO_ALTBATTLE);
            break;
        case SNES_SCENE_STORY_TALK:
        case SNES_SCENE_DECK:
            snesAudioPlay(SNES_AUDIO_OVERWORLD);
            break;
        case SNES_SCENE_ENDING:
            snesAudioPlay(SNES_AUDIO_VICTORY);
            break;
        default:
            snesAudioPlay(SNES_AUDIO_TITLEALT);
            break;
        }
    }

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
        next = snesDuelFrame();
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
    /* The board's upload runs inside the NMI (snes_fb.asm); it does nothing
     * until a duel enables the drain. */
    nmiSet(snesFbNmi);
    snesAudioInit();
    snesStampInit();

    snesSceneSet(SNES_SCENE_TITLE);

    while (1) {
        padsCurrent(0);
        snesSceneRun();

        g_stamp.scene = cur_scene;
        g_stamp.frames = scene_frames;
        g_stamp.frame_gen = snesVideoPresentedGeneration();
        g_stamp.deck_slot = snesDeckActiveSlot();
        g_stamp.deck_count = snesDeckCount();
        g_stamp.deck_head = snesDeckHead();
        g_stamp.storage_count = snesDeckStorageCount();
        g_stamp.save_valid = snesDeckSaveValid();
        snesStampCommit();

        /* The sprite layer's share of the coming vblank, reserved out of
         * the board drain's allowance before the NMI runs. */
        snesFbReserve(cur_scene == SNES_SCENE_DUEL ? snesObjVblankBytes() :
                      cur_scene == SNES_SCENE_DECK ? snesDeckVblankBytes() : 0);
        /* A camera pose in progress runs straight on: the pad is read
         * again after the next slice and the NMI does the uploads. */
        if (cur_scene == SNES_SCENE_DUEL && snesDuelBusy()) continue;
        WaitForVBlank();
        if (scene_pending != SNES_SCENE_BOOT) {
            u8 next = scene_pending;
            scene_pending = SNES_SCENE_BOOT;
            /* Scene changes that upload a complete screen happen at this
             * boundary, never halfway through the title's active display.
             * A scene set is many fields long (the duel bakes its board),
             * so the vblank work below waits for the NEXT vblank: run here
             * it landed in active display with the screen just turned on,
             * and the first hand card's tiles were dropped on the floor. */
            snesSceneSet(next);
            continue;
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
            /* The duel opens from black (snes_duel.c's intro fade): the
             * screen comes on at ITS level, here in vblank, rather than
             * through setScreenOn's full brightness. */
            if (cur_scene == SNES_SCENE_DUEL && snesDuelBrightness() < 15)
                REG_INIDISP = snesDuelBrightness();
            else
                setScreenOn();
            screen_on_pending = 0;
        }
        if (cur_scene == SNES_SCENE_DUEL) {
            /* The board's tiles went up inside the NMI (snes_fb.asm), which
             * left the reserved share of the window for this: OAM and the
             * card rows, then the card presentation's maps if one is up. */
            snesDuelVblank();
            snesObjVblank();
            switch (snesDuelMode3Active()) {
            case 1:  snesCardArtVblank(); break;
            case 2:  snesBattleVblank(); break;
            default: snesVideoPresent(); break;
            }
        } else if (cur_scene == SNES_SCENE_TITLE ||
                   cur_scene == SNES_SCENE_STORY_TALK ||
                   cur_scene == SNES_SCENE_ENDING) {
            snesSceneVblank();
        } else if (cur_scene == SNES_SCENE_DECK) {
            snesDeckVblank();
        }
    }
    return 0;
}
