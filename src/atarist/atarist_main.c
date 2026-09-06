/* ─────────────────────────────────────────────────────────────────────────────
 *  Shattered Decks — Atari ST/STE port
 *  atarist_main.c — boot and the frame loop.
 *
 *  The shape is the one every fork of this game uses: take the machine, build
 *  the screen, then loop { latch input, step the scene, present }.  Presenting
 *  reports how many vblanks the frame actually took, and the scene advances its
 *  clocks by that, so a board render that overruns slows the picture down
 *  without slowing the game down.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdint.h>

#include "atarist_os.h"
#include "atarist_hw.h"
#include "atarist_video.h"
#include "atarist_input.h"
#include "atarist_audio.h"
#include "atarist_probe.h"
#include "atarist_scene.h"
#include "atarist_assets.h"
#include "atarist_disk.h"

/* Boot bring-up in the order the probe stages name, so a machine that dies
 * early still reports where. */
static int Atarist_Boot(void)
{
    /* SUPERVISOR MODE FIRST, BEFORE ANY OTHER LINE OF THE PORT.
     * The bottom 2 KB of an ST holds the exception vectors and the TOS system
     * variables, and reading one of those from user mode is a bus error, not a
     * zero.  Machine detection reads the cookie jar pointer at $5a0, so a port
     * that switches later than this dies before it can say why -- which is how
     * this one first booted: a silent return to the desktop with the probe
     * frozen at its second boot marker. */
    atarist_supervisor_enter();
    Atarist_ProbeInit();
    ATARIST_STAGE(ATARIST_STAGE_VIDEO);
    if (!Atarist_VideoInit()) {
        g_atarist_probe.status = ATARIST_PROBE_NOMEM;
        return 0;
    }
    ATARIST_STAGE(ATARIST_STAGE_INPUT);
    Atarist_InputInit();
    ATARIST_STAGE(ATARIST_STAGE_AUDIO);
    Atarist_AudioInit();
    Atarist_DiskInit();
    ATARIST_STAGE(ATARIST_STAGE_ASSETS);
    if (!Atarist_AssetsInit()) {
        g_atarist_probe.status = ATARIST_PROBE_NOMEM;
        return 0;
    }
    Atarist_SceneInit();
    return 1;
}

static void Atarist_Quit(void)
{
    ATARIST_STAGE(ATARIST_STAGE_EXIT);
    Atarist_AudioShutdown();
    Atarist_InputShutdown();
    Atarist_VideoShutdown();
}

void atarist_main(void)
{
    int vblanks = 1;

    if (!Atarist_Boot()) {
        /* Nothing to show and nowhere to show it; leave the desktop intact. */
        return;
    }

    ATARIST_STAGE(ATARIST_STAGE_LOOP);
    for (;;) {
        Atarist_InputUpdate();
        if (g_atarist_input.pressed & ATARIST_BTN_QUIT) break;

        Atarist_SceneStep(vblanks);
        Atarist_ProbeUpdate();

        vblanks = Atarist_VideoPresent();
        g_atarist_probe.frame_vbls = (uint16_t)vblanks;
        /* The first few frames are boot: palettes, the first full board and a
         * cold cache, and they are not what the renderer should be judged on. */
        if (g_atarist_probe.frame > 8 &&
            (uint16_t)vblanks > g_atarist_probe.worst_vbls)
            g_atarist_probe.worst_vbls = (uint16_t)vblanks;
    }

    Atarist_Quit();
}
