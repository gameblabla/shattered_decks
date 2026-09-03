// ─────────────────────────────────────────────────────────────────────────────
//  waifu_msx2_s4_b0.c — the story-screen code bank
//
//  Cartridge segment 4, mapped at 0x0000 while the run between duels is on
//  screen: the opening, the sanctum map, dialogue, the deck editor, rewards,
//  continue codes and the ending.
//
//  It is a bank of its own rather than a lodger in segment 2 because the duel
//  screen was already most of that bank and the two together left under three
//  hundred bytes.  Nothing is lost by separating them: the story and the duel
//  never run at the same time and call each other not at all -- the scene loop
//  in msx2_main.c is the only code that knows both -- so the split is free and
//  each gets a whole 16 KB.  msx2_bank.h has the rule that governs what code in
//  here may call.
//
//  msx2_story_utils.c is here too, and only for room: nothing outside the story
//  calls it, and it was 1.8 KB of _CODE that the disk layer needed instead.
// ─────────────────────────────────────────────────────────────────────────────

#include "msx2_story_utils.c"
#include "msx2_story.c"
