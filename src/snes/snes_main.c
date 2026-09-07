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

static u8  cur_scene = SNES_SCENE_BOOT;
static u16 scene_frames = 0;   /* explicit: see snes_duel.c on .bss */

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
    case SNES_SCENE_DUEL:
        snesVideoInitDuel();
        snesObjInit();
        snesDuelEnter();
        setScreenOn();
        break;
    default:
        snesVideoInitDuel();
        snesObjInit();
        setScreenOn();
        break;
    }
}

void snesSceneRun(void)
{
    switch (cur_scene) {
    case SNES_SCENE_DUEL:
        snesDuelFrame();
        break;
    default:
        break;
    }
    ++scene_frames;
}

int main(void)
{
    consoleInit();
    snesStampInit();

    /* The duel is the scene the port is built around and the only one M2
     * exercises; the title and the rest arrive with the Mode 3 work. */
    snesSceneSet(SNES_SCENE_DUEL);

    while (1) {
        padsCurrent(0);
        snesSceneRun();

        g_stamp.scene = cur_scene;
        g_stamp.frames = scene_frames;
        g_stamp.board_res = snesVideoBoardRes();
        snesStampCommit();

        WaitForVBlank();
        /* OAM and the card tiles FIRST, then whatever the bitmap's staged
         * upload wants from what is left of the window: the sprite layer is
         * the HUD, and a HUD that arrives a vblank late while the board
         * finishes is the wrong way round. */
        snesObjVblank();
        snesVideoPresent();
    }
    return 0;
}
