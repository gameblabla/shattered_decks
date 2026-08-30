# Shattered Decks — C/headless + SDL 1.2 build v35

## PC build (SDL3)

`make -f Makefile.sdl3` builds `./shattered_decks`, the desktop release: an
SDL_GPU renderer (true colour 3D board, full-resolution card art, FreeType text)
plus the release shell (settings, gamepad input, options menu) around the same
game core every console target runs. `make sdl3` forwards to the same file.

    make -f Makefile.sdl3 toolchain-check   # are the libraries installed?
    make -f Makefile.sdl3                   # build ./shattered_decks
    make -f Makefile.sdl3 run               # build and play
    make -f Makefile.sdl3 debug             # ./shattered_decks-debug, -O0 -g3
    make -f Makefile.sdl3 clean

It needs SDL3, libpng, libwebp, FreeType and zlib (via pkg-config). The build is
incremental with header dependency tracking, in `build/sdl3/`. `glslc` is only
needed by `make -f Makefile.sdl3 shaders`, which regenerates the embedded SPIR-V
header — that header is checked in, so a normal build needs no shader tools.

**Display.** The view is resolution-agnostic and widescreen-native. The window
opens at 1280x720 and follows whatever you resize it to, up to 32:9; the duel
HUD, story scenes and the arena backdrop spread to the real screen edges, while
menus and other fixed-layout screens stay centred in the authored column. On a
display wide enough to leave room beside the board, the duel flanks carry the
turn/cards-played/deck/hand dashboards.

**Options** (ESC or the gamepad BACK button, in game or at the title):

- *Video* — display mode (windowed / borderless / exclusive fullscreen),
  resolution or window size, monitor, v-sync, frame limit, screen fit
  (widescreen / pillarbox / stretch), widest view, render scale, FPS readout.
- *Audio* — master, music and effects volume.
- *Controls* — rebind every action to a key or a gamepad button (select a row
  and press what you want to use), stick deadzone, vibration.

The menu takes keyboard, gamepad and mouse. Every connected controller drives
the game at once, and both sticks work as a d-pad.

**Hotkeys.** F11 toggles fullscreen, F12 saves a screenshot.

**Settings and screenshots** live in the platform preferences directory
(`~/.local/share/ShatteredDecks/ShatteredDecks/` on Linux); `WAIFU_SDL3_CONFIG`
points the game at a different `settings.cfg`. The file is plain `key value`
text and an unknown or damaged line simply falls back to the default.


## v35 fixes

- Failed fusion targeting an occupied player monster zone now places the surviving last hand material face-up, matching successful fusion result placement.
- Updated `scripts/player_fusion_failed_field_overwrite_test.txt` to assert the face-up failed-overwrite fallback.

This package contains the platform-agnostic game core, the headless recorder/command backend, and an SDL 1.2 interactive frontend using a 256x240 8-bpp framebuffer.

## v34 fixes

- Failed fusion targeting an occupied player monster zone now discards the previous field monster and places the last hand material into that same slot instead of leaving the field empty.
- Added `scripts/player_fusion_failed_field_overwrite_test.txt` to verify the failed occupied-zone fallback.

## v33 fixes

- Extended the player fusion animation with a board-facing landing segment before the field state is committed.
- Successful fusion results now fly smoothly from the reveal card to the selected field zone.
- Failed fusion that places the last material now flies that last card to the selected field zone while the discarded materials fall offscreen.

## v32 fixes

- Successful fusion results now enter the selected player monster zone face-up, including occupied-zone overwrite fusion.
- Added `scripts/player_fusion_faceup_test.txt` to verify `player_faceup0=1` after a hand fusion resolves.

## v31 fixes

- Fusion is now governed by the same one-monster-action-per-turn rule as normal monster placement in the actual interactive gameplay path.
- After the player places a monster, DOWN no longer queues fusion materials and A cannot start fusion during that same turn.
- Any fusion attempt that reaches the animation consumes the turn's monster action, even if the fusion fails or discards materials.
- Updated `scripts/player_fusion_field_overwrite_test.txt` so occupied-zone overwrite fusion happens on a later player turn, not after a same-turn placement.
- Added `scripts/normal_place_blocks_fusion_test.txt` to verify that normal placement blocks same-turn fusion selection.

## v30 fixes

- Player fusion confirmation now opens a player field-zone selector before the fusion animation begins.
- Fusion results are placed directly onto the selected zone instead of returning to hand first.
- Selecting an occupied player monster zone uses that field monster as the first fusion material and overwrites the same zone with the result, allowing field-card fusion when the monster zones are full.
- Added `scripts/player_fusion_field_overwrite_test.txt` for the new occupied-zone fusion path.

## v29 fixes

- Added a one-monster-placement-per-turn rule for the player battle flow.
- Support-card use is explicitly exempt from the monster placement limit.
- Added `scripts/one_monster_per_turn_test.txt`, a headless regression script that places one monster, returns to hand, and attempts an illegal second monster placement during the same turn.
- Extended `--dump-state` to include all five player monster zones and summon-limit flags for regression checks.

## v28 fixes

- Fixed first-turn attack legality: the opening player turn can no longer start a battle or direct attack.
- Added `scripts/first_turn_attack_lock_test.txt`, a headless regression script that places a monster and attempts a first-turn direct attack.
- Added `--dump-state` to the headless runner for deterministic regression checks of LP, turn, field, and attack-flag state.

## v27 fixes

- Fixed equal battle handling: when two monsters have equal battle value, both cards now flash/burn in the battle cut-in and both are removed from the field during resolution.
- Fixed player draw duplication: after the player draw animation completes, the hand no longer restarts the full slide-in animation on the next `IB_PLAYER_HAND` frame.
- Added a hard direct-attack legality guard in `prepare_direct_attack()`: a direct attack is impossible while the opposing side controls any live monster, regardless of stale selection state.
- Battle value calculation now uses ATK for face-up defenders and DEF only for face-down defenders. This makes equal-ATK face-up battles resolve correctly.
- When destroyed cards are removed from the field, their attack flags are cleared too, avoiding stale slot/attack state after battle.
- SDL/headless parity was re-tested through SDL's own event queue using dummy video.

## Build headless

```sh
make clean
make
```

## Run headless command playback

```sh
./waifu_fm_headless --frames 1450 --commands scripts/battle_mode_demo.txt --out headless_frames --dump-every 20
```

## Build SDL 1.2 frontend

With a normal SDL 1.2 install:

```sh
make sdl12
./waifu_fm_sdl12
```

With a manually built SDL 1.2 tree, override the SDL flags, for example:

```sh
make sdl12 \
  SDL_CFLAGS="-I/path/to/SDL/include -D_GNU_SOURCE=1 -D_REENTRANT" \
  SDL_LIBS="/path/to/SDL/libSDL.a -ldl -lpthread"
```

## SDL command playback validation

The SDL frontend can receive the same command file through synthetic SDL key events:

```sh
SDL_VIDEODRIVER=dummy ./waifu_fm_sdl12 \
  --frames 1450 \
  --commands scripts/battle_mode_demo.txt \
  --out sdl_frames \
  --dump-every 20 \
  --no-delay
```

The v27 validation compared the SDL-dumped frames with the headless-dumped frames for the same script: 73 frames vs 73 frames, zero pixel differences.

## Controls

- Cursor keys: move selection
- LCTRL: confirm / A
- LALT: back / B / card preview exit
- SPACE: Run / Start / pass turn
- TAB: button 4 / toggle defense position in top view
- ESC: quit SDL frontend

## Notes

The game core remains SDL-free. SDL only polls input, updates the palette/display surface, copies the finished 8-bpp framebuffer, and flips the surface.

### CPU AI module

The opponent battle decisions now live in `src/game/ai.c` with the public interface in
`src/game/ai.h`. `main.c` builds a compact `WaifuAiState` snapshot and applies the
returned `WaifuAiAction`, keeping AI policy separate from rendering and battle state.

Current COM heuristics:
- COM uses one modular action picker for selection/placement and one for battle.
- COM places/equips through the AI action interface rather than hard-coded first-card logic.
- COM evaluates face-down attack-position player monsters against a deck-strength threshold
  before gambling into them.
- COM avoids attacking known bad targets and changes vulnerable attack-position monsters to
  defense when it cannot make a safe attack.
- COM continues its battle phase until every eligible monster has attacked or no legal/safe
  action remains.
- COM can manually switch defense-position monsters with ATK greater than 0 into attack
  position and direct-attack when the player field is empty and the attack is not a lethal
  retaliation risk.

Headless AI demo setups:
- `./waifu_fm_headless --ai-demo-scenario multi-attack --frames 1200 --record-mkv cpu_ai_multi_attack.mkv --no-png`
- `./waifu_fm_headless --ai-demo-scenario direct-switch --frames 1200 --record-mkv cpu_ai_direct_switch.mkv --no-png`
- `./waifu_fm_headless --ai-demo-scenario defense-guard --frames 720 --record-mkv cpu_ai_defense_guard.mkv --no-png`
