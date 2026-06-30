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

- [Makefile.cd32x](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/Makefile.cd32x:1)
  - `make -f Makefile.cd32x` builds the Mega CD32X `.cue`.
  - CD32X CD/file handling is split between SH-2-side request mapping in [src/platform/cd32x/waifu_cd32x_cdrom.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/platform/cd32x/waifu_cd32x_cdrom.c:1) and Sega-CD supervisor reads in [src/platform/cd32x/cd32x_boot_main.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/platform/cd32x/cd32x_boot_main.c:1). Keep offset/slice semantics here, not in common gameplay code.
  - Big card art on CD32X is streamed per card from sector-padded `CARD_BIG_ART_CD.BIN` slots into a two-slot 32X SDRAM cache. Battle cut-ins prewarm the attacker/defender pair before the animation; hand/field/deck previews prewarm the selected card before entering preview; render-time 112x112 card/support art access is cached-only. Do not read the whole big-art atlas into Word RAM.
  - CD32X is a 320x224 NTSC target. Keep gameplay layout platform-agnostic by deriving bottom HUDs, hand rows, story/card-check text boxes, and battle cut-in lanes from `WAIFU_FM_WIDTH` / `WAIFU_FM_HEIGHT`; do not add new 240-line assumptions to common gameplay code. The CD32X title asset remains 320x240 and is cropped by the CD32X video backend for the 224-line framebuffer.
  - CD32X 256-color composition can treat palette/color index 0 as transparent/black. `tools/gen_assets.py` remaps card faces, 112x112 monster/support art, support faces, and card backs away from index 0; keep CD32X-visible card art free of palette index 0.
  - CD32X RF5C164 audio keeps streaming music on channel 4 in the lower wave-RAM ring and preloaded one-shot SFX on channels 0-3 in the high wave-RAM bank. Do not upload SFX samples during playback or overlap the music/SFX channel and wave-RAM regions; on-demand PCM copies can starve the INT2 music pump and cause brief music dropouts.
  - CD32X streamed PCM music chunks are intentionally 64 KiB at 8 kHz. Smaller chunks can starve during the battle-entry asset-read burst before the next music load gets CD time; huge chunks can take longer to load than the RF5C164 ring can play unattended, causing loop-marker dropouts at source chunk boundaries.
  - Story portraits and masks are streamed by generated `WAIFU_STORY_PORTRAIT_CD_STRIDE`.
  - CD32X rendering follows Blastem's `32x_video.c` page model: framebuffer writes always hit `video->back`, changing FS swaps `front`/`back`, and `0x24020000` is the overwrite aperture for the same back page, not page 1. Common CD32X drawing uses the current back page directly as `waifu_fm_framebuffer()`, so `waifu_cd32x_video_present_8bpp()` must not repack-copy that framebuffer onto itself; both pages get their line tables during init/clear, and present normally only updates palette state before the flip. Title/menu use the CD32X hardware-overlay hook again, but the backend warms both flipped pages after each title/menu mode change and restores only the prompt/menu rectangles from the resident title asset; do not return to a one-page title/menu composition, fixed physical page pointers, or per-frame title/menu redraw.

- Resolution config (all of PC-FX / SDL / headless):
  - `src/engine/cfx_screen_config.h` `WAIFU_FM_WIDTH` / `WAIFU_FM_HEIGHT` are the single source of truth (default 256x240). `game_api.h` and `main.c` derive `W`/`H` from these; no other file hardcodes the screen size.
  - Full-screen 2D art (title + ending) is resolution-driven: edit the two numbers, then `make assets` (host) — `tools/gen_title_asset.py` reads the resolution, sets `TITLE_SCREEN_W/H`, and picks the matching source PNG. Selection order: canonical `assets/source/title/title_<W>x<H>.png` & `assets/source/ending/ending_<W>x<H>.png`, then the explicit `RESOLUTION_TITLE`/`RESOLUTION_ENDING` maps in that script, then the default. Sources exist for 256x240, 320x240, 384x240, 640x400, 640x480, 704x480, 704x512. Add a resolution by dropping in `title_<W>x<H>.png`/`ending<W>x<H>.png` (or a map entry).
  - 256x240 keeps the original shipped title (`titlescreen_shardsofcards.png`) for a byte-identical default; `ending.png` was renamed to `ending256x240.png` (same image). PC-FX 16M title KRAM padding still assumes 256-row pages — non-256 heights on real PC-FX hardware need VCE/KING mode work beyond asset generation. Card/portrait sprites are fixed-size and do not vary by resolution.

- [README.md](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/README.md:1)
  - Build and run notes.
  - Existing regression coverage and historical behavior notes.

- [INSTRUCTIONS.txt](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/INSTRUCTIONS.txt:1)
  - User-requested backlog and bug list. Treat this as the change brief.

## Behavior Map

- Story mode:
  - Runs 5 duels (`STORY_MAX_DUELS`). Opponents: KASEM, ANPU, RAHOTEP, NADIRA, ISYRA. ISYRA is the final boss (`story_opponent_is_boss`).
  - Opponent decks come from `waifu_opponent_story_pools` (`src/generated/deck_pools.h`, sourced from `assets/source/cards/card_data.txt` `pool|opponent_*` lines via `tools/gen_assets.py`). The first opponent (KASEM, `opponent_dream`) uses `Pretty_snake` (Saphira, 1550/1350) in place of the old `Stone_Tablet_Woman` (Menat, 1100/2200 wall) so the opening duel is a weaker, lower-DEF deck. Edit both `card_data.txt` and the generated header when changing a pool.
  - Winning the fifth duel no longer returns to the map. It enters `WAIFU_I_STORY_ENDING`, shows `assets/source/ending/ending.png`, advances to `WAIFU_I_STORY_ENDING_CREDITS`, then returns to the title. PC-FX presents the ending image as a direct 16M/YUV422 KING surface from `assets/generated/ending_screen_pcfx_yuv422.bin`; ending narration and credits are VDC overlay text with the same white glyph / black outline style as the title. Do not convert the PC-FX ending image to an 8bpp palette screen.
  - PC-FX title CD-DA must not start while title assets are still loading. `src/platform/pcfx/pcfx_main.c` gates `WAIFU_FM_MUSIC_TITLE` on `waifu_assets_title_ready()`, and `waifu_pcfx_audio_create()` explicitly stops any stray BIOS/drive CD-DA before raising the music mixer volume.
  - PC-FX Battle Mode selection must finish the title/menu fade before `init_battle_state()` runs. That initializer switches to the common 8bpp palette; running it on the A-press menu frame makes the title cut to black, reveal the title again, then fade a second time.
  - Menu-to-story, menu-to-random-battle, menu-to-load, PC-FX load-device-to-map, story-dialogue-to-deck, and deck-editor exits all use the shared fade-to-black helper in `src/main.c` before loading or destination setup. Keep asset prewarming and battle initialization after the black fade so PC-FX cannot hitch during the visible fade.
  - PC-FX 8bpp fade-to-black "one frame fully lit before black" glitch (two parts, both needed):
    1. `apply_black_dither_fade()` on PC-FX records a palette fade level only, but when the fade reaches fully black (`visible <= 0`) it must ALSO `clear_screen(IDX_BLACK)`, exactly like the host dither path (`threshold >= 64`). A palette-only black leaves the faded-out 8bpp scene sitting in the framebuffer; the next state restores the palette before its first frame is uploaded, flashing that stale scene. This covers the ad-hoc palette-only fades that hit `visible == 0` (e.g. `draw_story_name_entry` to_intro).
    2. The shared `draw_transition_black_hold_frame()` (the hold tail of `draw_fade_to_black_transition`, used by deck-editor exits, story fire-to-deck, plaza-to-deck, and menu transitions) must KEEP the palette black (`apply_black_dither_fade(0)`) instead of letting the per-frame `waifu_fm_use_common_palette()` reset leave the fade at full. The PC-FX palette write lands on the VCE immediately, but the cleared (black) framebuffer only reaches the screen on the next KRAM page flip; resetting the fade to full while the displayed page still holds the just-dimmed scene flashes it fully lit for one frame on real hardware (pcfxemu presents atomically so it cannot be seen there). The title/menu fade uses the VDC mask, not the palette fade, which is why it never had this flash.
  - Story-dialogue (plaza) scene freeze: `draw_story_plaza_scene_content(anim_frame)` takes an explicit frame. The live scene passes `g_i_frame`; the fade transition passes `g_story_plaza_freeze_frame` (the last live frame, captured on START) so the portraits and 3D camera hold still under the fade instead of re-sliding from zero. Sanctum screens (`draw_story_sanctum_background`) use the free-running `g_story_scene_anim_frame` (incremented every `waifu_fm_step`, never reset on a state change) so opening SAVE / save-device / deck-editor sub-screens does not snap the 3D camera back to its start pose.
  - PC-FX title/menu fade-out must not stop already-playing title CD-DA until the screen has gone black or changed states. `src/platform/pcfx/pcfx_main.c` only suppresses starting title music while assets/fade are not ready; stopping the active track during the visible fade issues SCSI commands and makes Battle Mode selection stall.
  - Ending narration page advances are silent and do not fade through black. Pressing A/RUN erases the current page's text characters, then the next page appears over the same ending image.
  - The fourth and fifth story opponents each carry exactly three THUNDER support cards. The final opponent deck also includes AngelFishwoman, the strongest monster.
  - Four 3D environments selected by `story_scene_kind()` based on duel progress: DESERT (pyramid, `draw_map_pyramid_3d`), TEMPLE (stone pillars, `draw_map_temple_3d`), VOLCANO (cone with glowing crater, `draw_map_volcano_3d`), VOID (floating obsidian platform with crystals, `draw_map_void_3d`). Each has its own sky function via `draw_story_sky`.
  - Pyramid faces use gold tile (1) on all sides, per-face `flip` prevents texture swimming, `draw_tri3d_pyramid_face` tiles the texture in 5 rows x 3 columns (stacked brick courses). Ground grid extended to 8x7 with solid fill below the horizon to prevent sky bleed.
  - PC-FX story pyramid/volcano faces must not be culled by screen-space winding inside `draw_tri3d_pyramid_face`: normalize negative projected winding and leave occlusion to the painter-sorted face order, or the pyramid can disappear.
  - PC-FX Load Story checks both internal Backup RAM and external FX-BMP: if exactly one device has a save, it loads that device directly through `WAIFU_I_STORY_LOAD_TO_MAP`; if both have saves, it shows the title-backed device picker; if neither has a save, it stays on the picker so the user can see both empty slots and back out. The load-to-map state is a short visible BACKUP RAM LOADING, music-silent transition that clears the title VDC overlay before BackupRAM is read and the first story-map 3D frame is drawn.
  - PC-FX Sanctum SAVE opens `WAIFU_I_STORY_SAVE_DEVICE` and lets the user choose INTERNAL or FX-BMP before writing. Do not silently prefer internal storage for saves.
  - Story BATTLE from the map must stay on the asset `LOADING...` screen until both Serena and the current opponent portrait report ready. Do not draw `WAIFU_I_STORY_PLAZA` dialogue as a fallback while either portrait is still loading.
  - PC-FX story map skies request `waifu_pcfx_video_request_rainbow_backdrop`; the 3D scene and menu UI still render through transparent KING BG0, with RAINBOW behind it. Do not let the VDC background `NONE` clear run after a map RAINBOW request in the same present call, or pcfxemu shows `RAINBOWTransferControl=0` / no `0x4000` picture-mode bit and the pyramid scene falls back to black.
  - PC-FX Sanctum screens (`draw_story_pyramid_menu`, save status, save device picker) render through the normal CPU framebuffer on top of `draw_story_sanctum_background()`, so the current 3D story scene and RAINBOW sky remain visible behind the blue dialogue-style panels. Do not route these screens through the old VDC-only `waifu_pcfx_video_request_sanctum` overlay path; that bypass made the pyramid/scene disappear from save and deck-editor entry windows.
  - The Sanctum menu (`draw_story_pyramid_menu`, `g_story_pyramid_cursor`) has four options: 0 SAVE, 1 DECK EDITOR, 2 QUIT, 3 BACK (cursor wraps mod 4; the saved/loaded cursor is clamped to [0,3]). QUIT abandons the story session and returns to the title via `enter_title_after_assets()` (clearing `g_story_battle_active` and the PC-FX VDC overlay first); BACK returns to the map.
  - Returning from the Sanctum menu via BACK forces `g_story_map_cursor = 0` before `WAIFU_I_STORY_MAP`, so the player lands on SANCTUM and cannot accidentally press into BATTLE/dialogue. START from a Sanctum-launched deck editor is maintenance-only: it commits the deck and returns to `WAIFU_I_STORY_PYRAMID`; story-dialogue deck editing is the path that starts the next duel.
  - The fire intro has 4 lines describing the 8-guardian gauntlet (spoken by THE DEMON).
  - `draw_story_fire_to_deck_transition` fades the fire scene out to black and then HOLDS black for its second half; it must NOT draw `draw_deck_editor()` during the transition. The deck editor only appears after the LOADING screen (entered via `enter_deck_editor_after_assets()` once the transition completes). Cross-fading the deck editor in here made the deck screen flash for a few frames before LOADING ran.
  - Save text overflow is handled in `draw_story_save_screen`.
  - Dialog wrapping uses `draw_wrapped_text_small_box`.

- Deck editor:
  - Tab switching is now explicit via `tab`.
  - Storage starts empty in story mode.
  - Deck construction enforces a four-copy cap.
  - Story/deck card-check previews draw support/equip art in a full 128x128 region; do not shrink support previews back to the old 112x112 monster-art window.

- Battle:
  - Random Battle decks for both player and COM contain exactly three THUNDER support cards before shuffling. The headless fixed random-battle deck also appends three THUNDER cards after the scripted opening hand.
  - PC-FX random deck seeding mixes the FX VCE status halfword at I/O `0x300` and the timer halfword at I/O `0xFC0` inside `waifu_deck_runtime_seed()`, then avalanches the result. Keep this as hardware entropy because the console has no realtime clock.
  - THUNDER support is usable by both sides: COM THUNDER destroys all player monsters, and player THUNDER from hand destroys all COM monsters before returning to top view. The shared animation state is still named `IB_COM_THUNDER_ANIM`.
  - Opponent-turn bottom HUD is suppressed by `draw_interactive_common` / `draw_post_battle_return`.
  - Equip cards have a separate target-select phase and animation phase.
  - Tactical top-view movement uses `g_b_top_col` / `g_b_top_row` and can select empty zones. Press A on a player monster to enter attack-target mode; target selection then confirms the chosen COM monster instead of always using the first live defender.
  - Button 4/TAB toggles the selected player monster into or out of defense position from top view. Position toggle no longer flips the card face-up; a face-down card stays face-down when rotated.
  - Battle input is gated before sound playback and phase handling by `suppress_battle_input_if_locked()`. Only player decision phases (`IB_PLAYER_HAND`, top view, card previews, equip/fusion target) and the delayed final tally acknowledgement accept controls; COM turns, transitions, draw/place/equip/battle animations, and win/lose result animation must be silent and non-interactive.
  - Defense-position field cards swap the quad half-extents in `draw_board_card_state` so the rotated 38x54 texture keeps its aspect ratio instead of stretching.
  - B from top view checks the card under the cursor (player monster, opponent monster, or player equip row) via `IB_FIELD_CARD_PREVIEW`, reusing the hand card preview. B in attack-target mode still cancels targeting instead. `top_selector_preview_card()` resolves the cursor card; `g_b_preview_card_id` carries it into the preview phase.
  - Fusion confirmation is two-step: DOWN queues hand materials, A enters `IB_PLAYER_FUSION_TARGET`, then A on a player monster zone starts the fusion animation. Empty zones place the hand-fusion result there; occupied zones prepend that field monster as material and overwrite the same zone with the result.
  - One monster action per turn: a Set (`g_b_player_monster_played_this_turn`) and a fusion summon are mutually exclusive. `player_can_start_fusion()` requires BOTH `!g_b_player_fused_this_turn` AND `!g_b_player_monster_played_this_turn`, so once the player Sets a monster they cannot also fuse (and vice versa) — including occupied-zone transforms. This is the fix for "set more than 2 monsters manually": a turn commits at most one monster to the field. Equip/heal/draw/thunder/trap supports still do not count as the monster action. The `fusion_occupied_equip` regression now asserts both occupied and empty targets are blocked once the monster action is used.
  - A DOWN-selected chain containing one monster plus support/equip cards is treated as equip placement, not failed fusion: the monster lands on the chosen zone and receives the equip ATK/DEF bonuses. Two Water monsters fuse into AngelFishwoman.
  - Equip cards the player DOWN-selects into a fusion chain always land on the final summoned monster, regardless of their position in the chain. A fusion (or a failed pair) replacing the current monster keeps the hand-selected chain equips (`fusion_keep_hand_equips`, `equip_src >= 0`) and only drops the consumed monster's field-copied equips (`equip_src == -1`). Before this, an equip queued BEFORE a later fusion was silently discarded (`fusion_clear_local_equips` wiped it), so a successful summon could equip neither the fused monster nor the field. Covered by `--regression-fusion-equip` (`fusion_then_equip`, `fusion_occupied_equip`, `fusion_equip_mid`).
  - Only Bronze Equip and Desert Guard are equip-style support cards. Desert Guard (the `is_guard_support_card` kind) adds 250 ATK and 800 DEF via `equip_atk_bonus`/`equip_def_bonus`; Bronze Equip adds 500 ATK / 300 DEF. Oasis Light and Ancient Draw are one-shot hand supports, not equips; Oasis restores 1000 LP after a black-background support-card reveal, and Ancient Draw draws one card after the same reveal and cannot be used with an empty player deck.
  - Attack-target / top-view status bar must not reveal a face-down monster: `draw_bottom_info_top_selector` routes a face-down COM monster (`!g_i_com_faceup`) to `draw_bottom_info_facedown` ("SET MONSTER / FACE-DOWN / HIDDEN") instead of `draw_bottom_info_field`, so hovering a set defender while choosing an attack target does not leak its name/ATK/DEF.
  - Story-win reward reveal (`draw_interactive_reward`, `IB_REWARD`) plays the same face-down -> flip-up turn as the battle cut-in: it calls `draw_big_battle_card_flip(card, 68, 46, g_b_phase_frame, WAIFU_BATTLE_FLIP_FRAMES)` for monster rewards and holds the card name / "ADDED TO STORAGE" / "RUN: CONTINUE" prompt until the flip completes, then settles into the static 112x112 art. Support-card rewards skip the scaled flip (the static path frames them specially).
  - Fusion animations avoid per-frame software resizing for hand/result cards and wait for the full landing segment before `finish_player_fusion_anim()` commits the field state. Do not shorten the completion gate back to `WAIFU_FUSION_ANIM_FRAMES`; that can make the placed card snap onto the field before the landing animation visually finishes.
  - Equip animations use a black background and draw the support/equip card at the native 38x54 card size while it moves onto the monster. Do not reintroduce scaled support cards or patterned equip backdrops.
  - COM position switches (`WAIFU_AI_ACTION_SET_DEFENSE` / `WAIFU_AI_ACTION_SET_ATTACK`) are now resolved silently inside `IB_COM_BATTLE`: a `for(;;)` loop applies any number of ATK<->DEF flips within a single frame (guarded by `set_guard >= I_FIELD * 2`) before the COM either attacks or ends the turn. Previously each flip drew a one-frame beat with `enemy_battle_top_camera()`, which read as an unwanted screen cut/transition whenever the opponent rotated a monster to defense. The new position shows up on the normal turn-end view, with no dedicated camera move. Do not arm `g_b_com_return_fade` for stance-only turns; `draw_post_battle_return()` must skip the black fade when COM merely changes ATK/DEF position and ends.
  - PC-FX post-attack return should stay on cached top-camera background plus palette fade; do not reintroduce a unique-camera return render for every frame.

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
- Sanctum RAINBOW stills are generated by [tools/gen_pcfx_rainbow_bg.py](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/tools/gen_pcfx_rainbow_bg.py:1) from [assets/source/bg/desert.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/desert.png), [assets/source/bg/stone.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/stone.png), [assets/source/bg/ember.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/ember.png), and [assets/source/bg/sky.png](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/assets/source/bg/sky.png). `Makefile.pcfx` regenerates `assets/generated/rainbow_*.bin` and `src/generated/rainbow_bg_assets.h` at quality 95 before the PC-FX objects that include the header; quality 100 overdrives the current HuC6271 stream enough to corrupt the rightmost desert macroblock in pcfxemu. The RAINBOW page bit is `0x1000` in pcfxemu's KING `PageSetting`; preserve it when flipping KING BG pages or the decoder reads the wrong KRAM page.
- pcfxemu treats KING RAINBOW transfer control as a level-triggered per-frame start at `RAINBOWTransferStartPosition`: once register `0x40` is set to `1`, the decoder restarts each frame from the programmed KRAM address/block count. For a 256x240 image, use transfer start line 6 and block count 15 so the decoder's 16-line delay maps source row 0 to visible line 22 and source row 239 to visible line 261. Do not write `0`/`1` every present while the backdrop is already active; resetting control during active display can cancel the current decode.
- RAINBOW does not participate in KING palette fading. While `g_rainbow_backdrop_active` is set, `waifu_pcfx_video_present_8bpp` routes `waifu_fm_video_fade_q8()` through the VDC fade-mask tiles instead. RAINBOW map/sanctum modes use `TETSU_LINES_262`, matching the title/menu VDC overlay timing; 263-line mode caused the sanctum save/deck menu BATs to wrap/duplicate in pcfxemu. VDC overlay tiles use their own 16-color palette offset (entry base 256) so title/menu/fade colors cannot overwrite KING's low 8bpp card palette; if low palette entries change, story-mode deck editor/card-check colors degrade. When clearing RAINBOW/sanctum mode, rewrite KING `PageSetting` after clearing `g_king_page_setting_extra` so the `0x1000` RAINBOW page bit is not left set before deck editor/loading screens.
- PC-FX title/menu fade masks must keep the VDC fade BAT installed and update only the single fade tile pattern for each fade step. Rewriting the full 64x32 BAT on both VDCs every step makes the visible fade hitch before Battle Mode loading starts.
- The VDC overlay palette selector uses absolute VCE entry indexes: select entry base 256 and write overlay colors at base 256. Do not select 128 while writing 256; title/menu glyphs will read the wrong palette bank and can turn green.
- Title-menu VDC text leaking onto the story map (the "remnant title text over the pyramid" glitch): the overlay *intent* (`g_vdc_overlay_mode`) is only set to `MENU` by the menu screen and back to `OFF` by `overlay_clear`. Leaving the title into gameplay used to leave it at `MENU`, so a later RAINBOW story-map fade re-ran `pcfx_vdc_overlay_flush()` at fade level 0 with the stale `MENU` mode and re-printed "SELECT MODE / STORY MODE / ..." into the VDC BAT over the map. `waifu_pcfx_video_begin_8bpp()` now forces `g_vdc_overlay_mode = WAIFU_PCFX_OVERLAY_OFF` (the canonical "entering 8bpp gameplay" point), covering every path. Do not rely on per-state `overlay_clear()` calls alone.
- Story-dialogue pyramid palette match: the texture atlas is quantized against the COMMON palette, but the plaza dialogue/intro switch to the DIALOGUE palette for portrait fidelity. `tools/gen_assets.py` reserves exactly the palette indices the texture atlas uses (`tex_reserved_indices`) to their common-palette RGB inside `waifu_dialogue_palette_rgb`, then re-quantizes the portraits around them, so the 3D pyramid/sky renders identically on the map and in the dialogue. Do not let the dialogue palette free-quantize all 256 entries from the portraits, or the pyramid texture hue shifts between the map and the dialogue.
- `Makefile.pcfx cd` intentionally runs a third build/link pass. The first generated `lbas.h` can make the boot program grow by one 2048-byte sector once the LBA/CDDA constants are compiled in; without the third pass, the final disc can shift RAINBOW/portrait assets by one sector while the executable still reads the stale pass-1 LBAs.
- PC-FX ending text is VDC overlay text over the 16M/YUV422 ending image. Keep it within the visible 32-tile width, not centered over the 64x32 BAT, and advance the story text with A/RUN pages before showing the black credit screen.

Verify PC-FX present changes by booting the new CD fresh (a `.mcr`/state snapshot bakes in the *old* program code, so it will not exercise rebuilt binaries): see `pcfx-build-and-capture` memory. Boot ~1100 frames to the title, then drive `START` / `DOWN` / `A` to reach a duel.

## PC-FX Audio

- Gameplay SFX now use KING/SoundBox ADPCM samples generated from `sounds/` by `tools/gen_pcfx_sfx_adpcm.py`; `waifu_pcfx_sfx_play()` should not start PSG scripts for these effects. `YOU_LOST` intentionally has a zero-length ADPCM slot because loss audio is CD-DA music.
- The ADPCM metadata table in `src/generated/pcfx_sfx_adpcm.h` must match `WaifuSoundEffect` enum order. Current sample aliases: `LASER_SHOOT` and `DIRECT_HIT` both use `SlashAttack.wav`, and `CARD_DRAWN` uses `CardDrawn.wav`.
- Player card draws play `WAIFU_SOUND_CARD_DRAWN` once per card, including the opening hand slide-in and each turn-replacement draw. COM/opponent draws stay silent.
- Turn-replacement draw animation (`IB_PLAYER_DRAW` / `draw_player_hand_turn_draw`): the kept hand cards rise quickly into place and settle; only the newly drawn card(s) slide in from the deck off the right edge, so the draw reads as a card being drawn (like the opening deal) instead of the whole hand sliding up as one block. The `CARD_DRAWN` SFX is NOT played in `draw_replacement_cards_to_hand()` (that runs on the silent turn-flip frame); `play_turn_draw_sfx()` staggers it per drawn card, timed to `turn_draw_slide_start()` so each beep lands as its card appears.
- Ancient Draw support (one-shot draw, `support_card_kind` 2): `finish_player_one_shot_support()` draws the card into the support hand slot, arms `g_b_draw_slots`/`g_b_draw_count`, and routes to `IB_PLAYER_DRAW` (not straight to `IB_PLAYER_HAND`) so the drawn card uses the same deck slide-in animation + staggered SFX, instead of snapping the new card into the hand.
- ADPCM samples are loaded into KING physical KRAM page 1 at `WAIFU_PCFX_SFX_ADPCM_KRAM_BASE_WORD` (`0x20000` words). RAINBOW stills, the 8bpp frame pages, and 16M title surfaces use low KRAM addresses, so keep the ADPCM base 256-word aligned and high enough that video/RAINBOW reloads cannot overwrite resident SFX. PCFXEMU/HuC6272 PageSetting uses bit `0x100` for ADPCM page 1, bit `0x10` for BG page 1, and bit `0x1000` for RAINBOW page 1; do not flip the ADPCM bit as part of BG page flipping.
- The SFX ADPCM bank normally loads by direct CD-to-KRAM DMA onto KING physical page 1. The RAM scratch / `king_kram_write_buffer()` loader is kept only as an opt-in debug fallback behind `WAIFU_PCFX_ADPCM_RAM_LOAD`; keep that non-DMA path hidden behind the define.
- PC-FX Load Story backup actions suppress menu confirm SFX while the title overlay and backup state are being torn down. BACK still uses the normal cancel/confirm-alt path.

## Host / Headless Audio (SDL 1.2 + headless)

- All audio mixes through the single software multiplexer `waifu_sound_mix_s16()` in [src/game/sounds.c](/home/anonymous/Documents/DEV/Anime_card/waifu_card_game/src/game/sounds.c:1) (44100 Hz, signed-16 stereo). SDL 1.2 feeds it from `sdl_audio_callback`; the headless runner pulls it per frame for `--record-wav`. Do NOT add SDL_mixer — both music and SFX go through this one mixer.
- SFX are embedded PCM. `tools/gen_sound_assets.py` bakes `sounds/*.wav` (unsigned 8-bit mono 44100) into `src/generated/sound_assets.h`; the `SOUNDS` list order MUST match the `WaifuSoundEffect` enum in `src/game/sounds.h`. `LASER_SHOOT` and `DIRECT_HIT` both alias `SlashAttack.wav` and `CARD_DRAWN` uses `CardDrawn.wav`, mirroring the PC-FX ADPCM aliases. `YOU_LOST` is a deliberately empty (`{0,0}`) slot. The generator dedupes aliased files so a shared WAV emits one PCM array. Regenerate after editing the list: `python3 tools/gen_sound_assets.py`.
- Music is streamed from disk, never embedded (the WAVs are tens of MB). The `#if !defined(WAIFU_FM_PCFX)` block in `sounds.c` (`WaifuMusicStream`, `music_stream_open/close`, `music_read_*`, `music_next_frame`) parses a WAV header (PCM, 8/16-bit, mono/stereo, any rate), reads it `WAIFU_MUSIC_READ_BUF` (8 KB) at a time, converts to s16 stereo, and resamples to 44100 with a fixed-point (.16) nearest-neighbour phase accumulator. Tracks loop seamlessly back to the data chunk. `music_track_path()` maps each `WaifuMusicTrack` to a file under `Music/` (DECK_EDITOR reuses Overworld.wav; OPENING_DREAM uses Overworld.wav). `WAIFU_MUSIC_DIR` env var overrides just the directory.
- `waifu_sound_set_music()` opens/closes the stream on track change (called from `waifu_fm_step` under SDL's audio lock, so no race with the callback). The PSG `placeholder_music_sample()` generator is now compiled only for the PC-FX build (which plays real music via CD-DA and never reaches this mixer).
- Verify: `./waifu_fm_headless --music-demo-state <title|battle|results|...> --frames N --record-wav out.wav --no-png`, then inspect `out.wav` for real stereo content. Note `Music/Battle.wav` fades in (near-silent for ~1s), so capture ≥600 frames to see it ramp up.
