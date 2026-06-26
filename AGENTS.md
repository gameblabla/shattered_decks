# Agent Guide

This repo is a small C game with two entry points:
- `src/main.c` for the full game core, headless runner, and most gameplay/rendering logic.
- `src/platform/sdl12_main.c` for SDL 1.2 input/display glue.

Use this file as the first stop when you need to change behavior. It is written to save context tokens: jump to the listed files and line anchors instead of re-traversing the repo.

## High-Value Files

- [src/main.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/main.c:1)
  - Story scenes: `draw_story_intro_screen` around line 4232, `draw_story_fire_screen` around 4300, `draw_story_map_screen` around 4497, `draw_story_pyramid_menu` around 4516, `draw_story_save_screen` around 4531, `draw_story_plaza_scene` around 4575.
  - Pyramid 3D: `draw_map_pyramid_3d` around 4435. Tiled face renderer: `draw_tri3d_pyramid_face` around 1004.
  - Deck editor: `draw_deck_editor` around 4329, `deck_editor_move_selected_card` around 2972, `reset_story_deck_editor` around 2730.
  - Story deck/storage generation: `generate_story_starter_deck` around 2668, `generate_story_storage_pool` around 2724, `sanitize_story_deck_copy_limit` around 2837, `story_reward_drop_card` / `award_story_win_drop` around 2853.
  - Battle flow: `step_battle_interactive` around 3812, `prepare_battle` around 3291, `prepare_direct_attack` around 3329, `resolve_battle` around 3364, `draw_interactive_common` around 3195, `draw_bottom_info_offset` around 585.
  - Tactical top-view selector: globals near the field row constants, smooth cursor drawing around `draw_top_selector_cursor` near line 1084, selector movement helpers around line 2417, and free-field / attack-target handling inside `IB_PLAYER_TOP` in `step_battle_interactive`. `top_selector_preview_card` (around line 2455) backs the B-button field card check, which runs in the `IB_FIELD_CARD_PREVIEW` phase next to `IB_CARD_PREVIEW`.
  - Equip/support handling: `start_player_equip` around 3703, `finish_player_equip` around 3726, `draw_player_equip_target` around 3757, `draw_player_equip_anim` around 3767, equip phase handling inside `step_battle_interactive`.
  - Player fusion handling: `clear_player_fusion_queue`, `try_queue_player_fusion_slot`, and `prepare_player_fusion_anim` near the hand/fusion helpers; `IB_PLAYER_FUSION_TARGET` and `IB_PLAYER_FUSION_ANIM` cases inside `step_battle_interactive`; `finish_player_fusion_anim` writes the final result to the selected player field zone.
  - Headless scripts / state dump: `waifu_fm_step` around 4595, `load_command_file` around 4907, `main` around 4959.

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
  - Extended to 8 duels (`STORY_MAX_DUELS`). Opponents: DREAM SHADE, PLAZA NOVICE, TEMPLE ADEPT, SAND REAVER, BURNING SOUL (boss), VOID WALKER, SPHINX GUARDIAN (boss), THE DEMON (final boss). Bosses have 10000 LP (`story_opponent_is_boss`).
  - Four 3D environments selected by `story_scene_kind()` based on duel progress: DESERT (pyramid, `draw_map_pyramid_3d`), TEMPLE (stone pillars, `draw_map_temple_3d`), VOLCANO (cone with glowing crater, `draw_map_volcano_3d`), VOID (floating obsidian platform with crystals, `draw_map_void_3d`). Each has its own sky function via `draw_story_sky`.
  - Pyramid faces use gold tile (1) on all sides, per-face `flip` prevents texture swimming, `draw_tri3d_pyramid_face` tiles the texture in 5 rows x 3 columns (stacked brick courses). Ground grid extended to 8x7 with solid fill below the horizon to prevent sky bleed.
  - PC-FX story pyramid/volcano faces must not be culled by screen-space winding inside `draw_tri3d_pyramid_face`: normalize negative projected winding and leave occlusion to the painter-sorted face order, or the pyramid can disappear.
  - PC-FX Load Story checks both internal Backup RAM and external FX-BMP: if exactly one device has a save, it loads that device directly through `WAIFU_I_STORY_LOAD_TO_MAP`; if both have saves, it shows the title-backed device picker; if neither has a save, it stays on the picker so the user can see both empty slots and back out. The load-to-map state is a short visible BACKUP RAM LOADING, music-silent transition that clears the title VDC overlay before BackupRAM is read and the first story-map 3D frame is drawn.
  - PC-FX Sanctum SAVE opens `WAIFU_I_STORY_SAVE_DEVICE` and lets the user choose INTERNAL or FX-BMP before writing. Do not silently prefer internal storage for saves.
  - Story BATTLE from the map must stay on the asset `LOADING...` screen until both Serena and the current opponent portrait report ready. Do not draw `WAIFU_I_STORY_PLAZA` dialogue as a fallback while either portrait is still loading.
  - PC-FX story map skies request `waifu_pcfx_video_request_rainbow_backdrop`; the 3D scene and menu UI still render through transparent KING BG0, with RAINBOW behind it. Do not let the VDC background `NONE` clear run after a map RAINBOW request in the same present call, or pcfxemu shows `RAINBOWTransferControl=0` / no `0x4000` picture-mode bit and the pyramid scene falls back to black.
  - PC-FX Sanctum screens (`draw_story_pyramid_menu`, save status, save device picker) do not render the story 3D scene. They request `waifu_pcfx_video_request_sanctum`, which streams a RAINBOW YUV/DCT still (`desert.png`, `stone.png`, or `ember.png`) into the non-BG KING KRAM page and draws the overlay with both VDCs on top. VDC0 owns the text-box/panel tiles; VDC1 owns actual glyph tiles with black outline/white foreground, matching the title overlay split.
  - The fire intro has 4 lines describing the 8-guardian gauntlet (spoken by THE DEMON).
  - `draw_story_fire_to_deck_transition` fades the fire scene out to black and then HOLDS black for its second half; it must NOT draw `draw_deck_editor()` during the transition. The deck editor only appears after the LOADING screen (entered via `enter_deck_editor_after_assets()` once the transition completes). Cross-fading the deck editor in here made the deck screen flash for a few frames before LOADING ran.
  - Save text overflow is handled in `draw_story_save_screen`.
  - Dialog wrapping uses `draw_wrapped_text_small_box`.

- Deck editor:
  - Tab switching is now explicit via `tab`.
  - Storage starts empty in story mode.
  - Deck construction enforces a four-copy cap.

- Battle:
  - Opponent-turn bottom HUD is suppressed by `draw_interactive_common` / `draw_post_battle_return`.
  - Equip cards have a separate target-select phase and animation phase.
  - Tactical top-view movement uses `g_b_top_col` / `g_b_top_row` and can select empty zones. Press A on a player monster to enter attack-target mode; target selection then confirms the chosen COM monster instead of always using the first live defender.
  - Button 4/TAB toggles the selected player monster into or out of defense position from top view. Position toggle no longer flips the card face-up; a face-down card stays face-down when rotated.
  - Battle input is gated before sound playback and phase handling by `suppress_battle_input_if_locked()`. Only player decision phases (`IB_PLAYER_HAND`, top view, card previews, equip/fusion target) and the delayed final tally acknowledgement accept controls; COM turns, transitions, draw/place/equip/battle animations, and win/lose result animation must be silent and non-interactive.
  - Defense-position field cards swap the quad half-extents in `draw_board_card_state` so the rotated 38x54 texture keeps its aspect ratio instead of stretching.
  - B from top view checks the card under the cursor (player monster, opponent monster, or player equip row) via `IB_FIELD_CARD_PREVIEW`, reusing the hand card preview. B in attack-target mode still cancels targeting instead. `top_selector_preview_card()` resolves the cursor card; `g_b_preview_card_id` carries it into the preview phase.
  - Fusion confirmation is two-step: DOWN queues hand materials, A enters `IB_PLAYER_FUSION_TARGET`, then A on a player monster zone starts the fusion animation. Empty zones place the hand-fusion result there; occupied zones prepend that field monster as material and overwrite the same zone with the result. Empty zones are blocked when the one-monster placement limit has already been used, but occupied-zone transformation remains available.
  - COM position switches (`WAIFU_AI_ACTION_SET_DEFENSE` / `WAIFU_AI_ACTION_SET_ATTACK`) are now resolved silently inside `IB_COM_BATTLE`: a `for(;;)` loop applies any number of ATK<->DEF flips within a single frame (guarded by `set_guard >= I_FIELD * 2`) before the COM either attacks or ends the turn. Previously each flip drew a one-frame beat with `enemy_battle_top_camera()`, which read as an unwanted screen cut/transition whenever the opponent rotated a monster to defense. The new position shows up on the normal turn-end view, with no dedicated camera move.

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
- `scripts/top_view_free_selector_defense_test.txt`
- `scripts/top_view_field_card_check_test.txt`
- `scripts/player_fusion_field_overwrite_test.txt`
- `scripts/position_toggle_no_faceup_test.txt`
- `scripts/story_map_to_plaza_transition_test.txt`
- Existing story/battle scripts under `scripts/`

Use `--dump-state` for deterministic state checks instead of image-only checks when the behavior is logic-heavy.

## PC-FX Presentation (KRAM / page flip)

[src/platform/pcfx/waifu_pcfx_video.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/platform/pcfx/waifu_pcfx_video.c:1) owns how the 256x240 8bpp CPU framebuffer reaches KING KRAM and the screen.

- The game renders the whole composited frame (3D board + field cards + HUD + text) into the single CPU framebuffer (`waifu_fm_framebuffer()`); KRAM is not CPU-addressable, so 2D primitives cannot draw to it directly. `waifu_pcfx_video_present_8bpp` then streams that framebuffer to the hidden KRAM page and flips BG0 to it (`pcfx_king_set_bg0_page_inline` + `front_page`/`back_page`) for tear-free double buffering.
- All KRAM writes in the present hot path are inline `out.h` (no fastking jal/rts). `pcfx_kram_write_frame_inline` is the full-frame writer (one seek + a 16-byte-unrolled byte-swapping stream); `pcfx_kram_upload_rect_bytes_inline` / `pcfx_kram_clear_rect_black_inline` are the per-rect inline writers. The framebuffer is little-endian byte order and KING wants the bytes swapped within each 16-bit word, so every writer byte-swaps. `fastking.s` (`king_kram_write_buffer*`) now only backs cold paths (16M title upload, `clear_black`) and the non-`__v810__` host fallback.
- `WAIFU_PCFX_DIRTY_PRESENT` (default 1) keeps two `page_shadow` mirrors and uploads only changed 16px bands. This is a speed optimization, not a software back buffer: a full 256x240 KRAM upload is ~30k `out.h` (near a whole frame's budget), so the diff is what keeps partially-changed frames (cursor blink, HUD tick) cheap. Set it to 0 only to force a full inline upload every changed frame (simpler, but slower on partial frames).
- Big-art (112x112 card art: card-check preview, equip/battle/summon card displays) is blitted into the CPU framebuffer like everything else and goes through the normal dirty-band path. The old "direct big-art" optimization (dirty scanner *skips* the big-art blocks; a separate `pcfx_upload_direct_big_art_rects` uploads them) was buggy and was removed entirely (function, `pcfx_shadow_rect_equals`, the `g_pcfx_direct_big_art_*` globals, and the `g_pcfx_dirty_scan_y` skip). It caused two distinct regressions: (1) whole-screen comb garbage during equip/battle animations, and (2) cards dropping to black with page-flip flicker during summon/equip animations — both because the separate uploader wrote the rect to `page_shadow` while blanking/corrupting the actual KRAM page, desyncing shadow from KRAM so the dirty scanner then permanently skipped those regions.
- The dirty-band merger's active table is sized `WAIFU_PCFX_DIRTY_MAX_BANDS` (2*BLOCKS_X): one row can produce up to BLOCKS_X runs when dirty blocks alternate black/non-black (swirl/portal over black), and the merge transiently holds the previous row's bands plus the new row's runs. A too-small table silently dropped runs (stale page -> flicker); a flush-immediately fallback also guarantees a run is never dropped.
- 3D renderer board pixels still go through the CPU framebuffer (the disabled `CFX_RENDERER_DIRECT_KRAM` board-to-KRAM path in `renderer3d.c` cannot composite with the overlapping 2D UI). The remaining per-frame cost is `render_board` re-running on animation/transition frames (unique camera each frame), which no KRAM-write change addresses; see the memory note `pcfx-3d-renderer-perf-state`.
- Sanctum RAINBOW stills are generated by [tools/gen_pcfx_rainbow_bg.py](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/tools/gen_pcfx_rainbow_bg.py:1) from [assets/source/bg/desert.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/desert.png), [assets/source/bg/stone.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/stone.png), and [assets/source/bg/ember.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/ember.png). `Makefile.pcfx` regenerates `assets/generated/rainbow_*.bin` and `src/generated/rainbow_bg_assets.h` at quality 100 before the PC-FX objects that include the header. The RAINBOW page bit is `0x1000` in pcfxemu's KING `PageSetting`; preserve it when flipping KING BG pages or the decoder reads the wrong KRAM page.
- pcfxemu treats KING RAINBOW transfer control as a level-triggered per-frame start at `RAINBOWTransferStartPosition`: once register `0x40` is set to `1`, the decoder restarts each frame from the programmed KRAM address/block count. Do not write `0`/`1` every present while the backdrop is already active; resetting control during active display can cancel the current decode.

Verify PC-FX present changes by booting the new CD fresh (a `.mcr`/state snapshot bakes in the *old* program code, so it will not exercise rebuilt binaries): see `pcfx-build-and-capture` memory. Boot ~1100 frames to the title, then drive `START` / `DOWN` / `A` to reach a duel.

## PC-FX Audio

- Gameplay SFX for card placed, card drawn, turn passed, and direct/slash hits are SoundBox PSG scripts in `src/platform/pcfx/waifu_pcfx_audio.c`; keep them PSG-only unless a future task explicitly switches an effect to ADPCM/PCM.
- `wav2vox` is the PC-FX ADPCM/VOX conversion reference for PCM assets. If a WAV is intentionally converted, match the source/output rate explicitly (`CardPlaced.wav` and `TurnPassed.wav` are 44100 Hz; `sfx_039_bank02_clip07.wav` is 22050 Hz). The current card/gameplay SFX tables are timbre matches, not embedded samples.
- PSG voices carry a hard max-age guard in addition to per-step durations. Starting a new voice must clear the channel without arming the multi-frame off guard; stopping a voice should still repeat off writes briefly so a SoundBox channel cannot latch until another effect stops it.
- PC-FX Load Story backup actions suppress menu confirm PSG while the title overlay and backup state are being torn down. BACK still uses the normal cancel/confirm-alt path.
