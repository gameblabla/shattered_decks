/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_scene.h — the scene machine.
 *
 *  atarist_main.c owns the loop and knows nothing about what is on screen; this
 *  is where the title / story / duel scenes are dispatched from, so a scene can
 *  be added without touching boot or presentation.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_SCENE_H
#define WAIFU_ATARIST_SCENE_H

enum AtaristScene {
    ATARIST_SCENE_BRINGUP = 0,   /* the video/input self test */
    ATARIST_SCENE_TITLE,
    ATARIST_SCENE_STORY,
    ATARIST_SCENE_DUEL
};

void Atarist_SceneInit(void);
/* Advance and draw one frame.  `vblanks` is how long the previous frame took,
 * so animation clocks run on real time rather than on frame count. */
void Atarist_SceneStep(int vblanks);

#endif /* WAIFU_ATARIST_SCENE_H */
