/* ─────────────────────────────────────────────────────────────────────────────
 *  snes_scene.h — the scene machine.
 *
 *  The same states as the MSX2 fork, with a Mode 3 / Mode 7 switch at each
 *  boundary: title -> menu -> story map -> VN dialogue -> duel -> battle
 *  close-ups -> victory/defeat -> ending.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_SNES_SCENE_H
#define WAIFU_SNES_SCENE_H

#include "snes_types.h"

enum SnesScene {
    SNES_SCENE_BOOT = 0,
    SNES_SCENE_TITLE,
    SNES_SCENE_MENU,
    SNES_SCENE_STORY_MAP,
    SNES_SCENE_STORY_TALK,
    SNES_SCENE_DUEL,
    SNES_SCENE_BATTLE_ART,
    SNES_SCENE_RESULT,
    SNES_SCENE_DECK,
    SNES_SCENE_ENDING,
    SNES_SCENE_COUNT
};

void snesSceneSet(u8 scene);
u8   snesSceneCurrent(void);
/* Frames spent in the current scene; the typewriter and the menu repeat both
 * key off it, and the frame stamp reports it. */
u16  snesSceneFrames(void);
void snesSceneRun(void);

#endif /* WAIFU_SNES_SCENE_H */
