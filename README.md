# Shattered Decks — C/headless + SDL 1.2 build v29

This package contains the platform-agnostic game core, the headless recorder/command backend, and an SDL 1.2 interactive frontend using a 256x240 8-bpp framebuffer.

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
