# Agent Guide

This repo is a small C game with two entry points:
- `src/main.c` for the full game core, headless runner, and most gameplay/rendering logic.
- `src/platform/sdl12_main.c` for SDL 1.2 input/display glue.

Use this file as the first stop when you need to change behavior. It is written to save context tokens: jump to the listed files and line anchors instead of re-traversing the repo.

## High-Value Files

- [src/main.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/main.c:1)
  - Story scenes: `draw_story_intro_screen` around line 4050, `draw_story_fire_screen` around 4117, `draw_story_map_screen` around 4288, `draw_story_pyramid_menu` around 4321, `draw_story_save_screen` around 4336, `draw_story_plaza_scene` around 4361.
  - Pyramid 3D: `draw_map_pyramid_3d` around 4213.
  - Deck editor: `draw_deck_editor` around 4146, `deck_editor_move_selected_card` around 2877, `reset_story_deck_editor` around 2635.
  - Story deck/storage generation: `generate_story_starter_deck` around 2573, `generate_story_storage_pool` around 2629, `sanitize_story_deck_copy_limit` around 2742, `story_reward_drop_card` / `award_story_win_drop` around 2758.
  - Battle flow: `step_battle_interactive` around 3678, `prepare_battle` around 3189, `prepare_direct_attack` around 3227, `resolve_battle` around 3262, `draw_interactive_common` around 3092, `draw_bottom_info_offset` around 536.
  - Equip/support handling: `start_player_equip` around 3587, `finish_player_equip` around 3610, `draw_player_equip_target` / `draw_player_equip_anim` around 3640, equip phase handling inside `step_battle_interactive`.
  - Headless scripts / state dump: `waifu_fm_step` around 4395, `load_command_file` around 4695, `main` around 4747.

- [src/game/game_api.h](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/game/game_api.h:1)
  - Shared input struct. `WaifuFmInput` includes `tab` for button 4.

- [src/platform/sdl12_main.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/platform/sdl12_main.c:1)
  - SDL key mapping and command injection.
  - `SDLK_TAB` is mapped to `WaifuFmInput.tab`.
  - Command-file parsing mirrors the headless parser.

- [Makefile](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/Makefile:1)
  - `make` builds headless.
  - `make sdl12` builds the SDL frontend.

- [README.md](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/README.md:1)
  - Build and run notes.
  - Existing regression coverage and historical behavior notes.

- [INSTRUCTIONS.txt](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/INSTRUCTIONS.txt:1)
  - User-requested backlog and bug list. Treat this as the change brief.

## Behavior Map

- Story mode:
  - Pyramid scene uses `draw_map_pyramid_3d`.
  - Save text overflow is handled in `draw_story_save_screen`.
  - Dialog wrapping uses `draw_wrapped_text_small_box`.

- Deck editor:
  - Tab switching is now explicit via `tab`.
  - Storage starts empty in story mode.
  - Deck construction enforces a four-copy cap.

- Battle:
  - Opponent-turn bottom HUD is suppressed by `draw_interactive_common` / `draw_post_battle_return`.
  - Equip cards have a separate target-select phase and animation phase.

## Editing Rules

- Prefer the smallest file that owns the behavior.
- Keep changes localized to `src/main.c` unless the change is pure input glue or build wiring.
- Use `apply_patch` for edits.
- Do not revert untracked or unrelated local changes.
- Do not use `vexp` for this repository.

## Delegation Strategy

Use smaller agents for bounded tasks, not for broad exploration.

- Use an explorer for one question at a time:
  - "Where is story save text drawn?"
  - "Where is deck editor tab switching handled?"
  - "Where is SDL input mapped?"
- Use a worker for a disjoint file slice:
  - One worker for `src/platform/sdl12_main.c`
  - One worker for `src/main.c` battle/story logic
  - One worker for scripts/tests
- Keep workers on non-overlapping write sets.
- Give each worker exact file ownership and tell them not to revert other agents' edits.

Suggested delegation pattern:

1. Explorer: locate symbols and line anchors.
2. Worker A: implement gameplay logic in `src/main.c`.
3. Worker B: update SDL/input glue in `src/platform/sdl12_main.c` and `src/game/game_api.h`.
4. Worker C: add or adjust regression scripts under `scripts/`.
5. Main agent: integrate, run `make`, `make sdl12`, and the relevant scripts.

## Regression Coverage

Prefer script-based headless checks when possible:
- `scripts/deck_editor_button4_storage_test.txt`
- `scripts/equip_spell_target_animation_test.txt`
- `scripts/story_map_to_plaza_transition_test.txt`
- Existing story/battle scripts under `scripts/`

Use `--dump-state` for deterministic state checks instead of image-only checks when the behavior is logic-heavy.
