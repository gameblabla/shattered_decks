/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_title.h — the title screen and its menu.
 *
 *  This is the port's first screen that exists to look like something rather
 *  than to prove something, and it is where the split list earns its keep: the
 *  backdrop is a vertical gradient made of a dozen palettes, so the screen
 *  carries well over sixteen colours while still being a 16-colour mode.
 * ───────────────────────────────────────────────────────────────────────────── */
#ifndef WAIFU_ATARIST_TITLE_H
#define WAIFU_ATARIST_TITLE_H

#include <stdint.h>

enum AtaristTitleChoice {
    ATARIST_TITLE_NONE = 0,
    ATARIST_TITLE_FREE_BATTLE,
    ATARIST_TITLE_STORY
};

void Atarist_TitleEnter(void);
void Atarist_TitleStep(int vblanks);
/* What the player picked, or ATARIST_TITLE_NONE while they are still choosing.
 * Reading it does not clear it; re-entering the screen does. */
int  Atarist_TitleChoice(void);

#endif /* WAIFU_ATARIST_TITLE_H */
