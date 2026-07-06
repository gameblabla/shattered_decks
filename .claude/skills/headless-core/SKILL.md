---
name: headless-core
description: Working on the platform-agnostic game core (src/main.c, src/game/, src/engine/) using the headless host build — building, scripted runs, regressions, recording, and the rules that keep the core portable. Use BEFORE touching any common code, and to verify game-logic changes cheaply before rebuilding console targets.
---

# Headless core: build, test, and portability rules

The game core is a pure frame machine (`src/game/game_api.h`). Every frontend —
headless runner, SDL 1.2, PC-FX, CD32X — drives the SAME API:

```
waifu_fm_init();                    /* once */
waifu_fm_step(&input);              /* one 60 FPS tick (WaifuFmInput: up/down/left/right/a/b/start/tab) */
waifu_fm_framebuffer();             /* 8-bit indexed, WAIFU_FM_WIDTH x WAIFU_FM_HEIGHT */
waifu_fm_palette_rgb();             /* 256*3 bytes; waifu_fm_palette_id() selects among WaifuFmPaletteId */
waifu_fm_audio_mix_s16(dst, n);     /* pull-model audio; rate/channels via waifu_fm_audio_* getters */
waifu_fm_frame_dirty_serial/_full/_rects();  /* dirty tracking for partial presents (max 8 rects) */
waifu_fm_set_frame_vblanks(n);      /* frontend reports real frame duration for pacing */
```

Resolution is per-target in `src/engine/cfx_screen_config.h`: 320x224 on CD32X,
256x240 everywhere else. Nothing else may hardcode dimensions.

## Build + run (fastest loop in the repo — use it before any console build)

```
make                    # ./waifu_fm_headless (compiles with -DWAIFU_FM_HEADLESS_TESTS)
make sdl12              # interactive ./waifu_fm_sdl12 (needs SDL 1.2)
make headless-cdrom-assets       # staged CD-ROM asset path over assets/generated/*.bin
make headless-cdrom-assets-2mb   # same + -DWAIFU_ASSET_RAM_BUDGET=2097152 (regression binary)
make headless-cart-assets        # -DWAIFU_ASSET_USE_CART_ROM path
make assets             # regenerate src/generated + assets/generated (python3 tools/gen_*.py)
```

Scripted runs (deterministic; SDL and headless produce identical frames):
```
./waifu_fm_headless --frames 900 --commands scripts/battle_mode_demo.txt --out out_frames --dump-every 10
./waifu_fm_headless --frames 3600 --commands scripts/... --no-png --record-mkv demo.mkv   # ZMBV video via src/record/
```
Command script format (`scripts/*.txt`): `<frame> <hold_frames> <BUTTON>` per
line, buttons `UP DOWN LEFT RIGHT A B START TAB`; `#` comments. There are ~40
ready-made scenario scripts under `scripts/` — reuse one before writing a new one.

Other useful flags: `--showcase`, `--dump-state`, `--record-wav <f>`,
`--profile-render` (with `WAIFU_PROFILE_RENDER`), `--story-cutscene-preview N`,
`--ai-demo-scenario`, `--fusion-equip-scenario`, `--music-demo-state`,
`--asset-load-demo`, `--deckout-demo`, `--lp-loss-demo`.

## Asset pipeline: regenerate-or-desync (silent-corruption class)

`src/generated/*.h` and `assets/generated/*.bin` are produced by `tools/gen_*.py`
and are checked into git. They compile fine when STALE, so a mismatch corrupts
output without a build error. The generated `.h` headers are tracked; regenerate
with `make assets` (host) or the target Makefile's asset rules, never by hand.

Known desync traps (each has bitten a real build):
- **Resolution is single-source-of-truth in `src/engine/cfx_screen_config.h`**,
  but that header is now a preprocessor branch (`#if defined(WAIFU_FM_CD32X)` →
  320x224, `#else` → 256x240). `tools/gen_title_asset.py` must select the SAME
  branch the target build compiles: `Makefile.cd32x` runs it with
  `WAIFU_FM_CD32X=1`; PC-FX/host leave it unset. A naive "first `#define
  WAIFU_FM_WIDTH`" read grabbed the CD32X 320 for every build and baked 320-wide
  title/ending YUV422 that the 256-wide PC-FX KING upload displayed SHEARED.
  Sanity: `TITLE_SCREEN_W` in `title_asset.h` and the `title_screen_pcfx_yuv422.bin`
  size (256*256*2 = 131072 for PC-FX) must match the target.
- **`tools/gen_sound_assets.py` / `gen_pcfx_sfx_adpcm.py` must be re-run when the
  `WaifuSoundEffect` enum or `sounds/*.wav` change.** Add an SFX enum entry
  without regenerating and the build still links but the new slot plays the wrong
  PCM (array-index-vs-enum mismatch). No compile error warns you.
- **Downstream generators read upstream generated headers**: e.g.
  `gen_pcfx_palette_assets.py` reads `title_asset.h`; `gen_assets.py` feeds the
  common palette. Regenerate in dependency order (the Makefiles encode it) or a
  header can carry stale palette data.
- When unsure a generated file matches its source, read the file AND the
  generator and cross-check the struct layout / array length — do not assume the
  on-disk header is fresh.

## Regression gate (run after ANY common-code change)

```
make regression-story           # scripts/story_save_duels_regression.sh over the 2mb cdrom binary
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
```
`--regression-story-all` runs all of the below; each also exists as an individual
`--regression-<name>` flag for bisecting (they live under `#ifdef WAIFU_FM_HEADLESS_TESTS`
in `src/main.c` as `debug_regression_*`; each prints `REGRESSION <name> OK/FAIL`
and returns nonzero on failure):

| Flag | Asserts |
|---|---|
| `story-save` | save→load roundtrip restores name/progress/deck/storage |
| `story-duels` | each story duel's opponent + portrait assets load |
| `sanctum-entry` | deck-editor→pyramid (maintenance) and deck-editor→battle transitions land in the right state with decks populated |
| `story-rematch` | rematch a cleared opponent (`g_story_duel_index` < `g_story_progress`) works from the map |
| `card-check-cache` | card-check art resolves from cache with NO CD read (cache-hit path) |
| `result-music` | win/lose battle outcome selects the correct RESULTS/LOST track |
| `thunder-support` | thunder support-card effect resolves |
| `trap-counter` | trap-card counter timing/resolution |
| `fusion-equip` | fusion/equip battle mechanic |

A change is NOT done until the relevant flags pass AND the affected console target
still builds. Run the closest-matching flag(s) for your change, plus `-all` before
committing anything touching story/battle/save/asset flow.

## Portability rules (binding for all common code)

1. **No platform APIs in common code.** No `<stdio.h>`/file/console calls in
   `src/game/` or shared `src/engine/` paths. Platform access goes through the
   seams in `src/engine/platform.h`:
   - storage: `waifu_platform_storage_{exists,read,write}[_dev]` (device 0/1)
   - background layer: `waifu_platform_background_request(kind, hscroll)` — return
     0 means "caller composites software sky"
   - hardware text overlay: `waifu_platform_text_overlay(kind, params)` /
     `_clear()` / `_is_hardware()` — return 0 means "caller software-draws the panel"
   - asset bytes: `waifu_assets_platform_read_blob_slice(blob, dst, offset, bytes)`
     (catalogue/slicing/caching live in common `src/game/assets.c`; the platform
     only supplies raw bytes)
   Seam pattern: platform returns 0 → the SAME call site does the software
   fallback. Never `#ifdef` a platform name at a call site.
2. **stdio in `src/main.c` is allowed only under `#ifndef WAIFU_FM_NO_HEADLESS_MAIN`**
   (the headless runner/regression harness). Console builds define
   `WAIFU_FM_NO_HEADLESS_MAIN` and must compile main.c with zero file/stdio API.
3. **Test hooks** go under `#ifdef WAIFU_FM_HEADLESS_TESTS` (e.g.
   `waifu_assets_debug_platform_read_count()`).
4. **Renderer variation** is capability flags in `src/engine/renderer3d_port.h`
   (`CFX_RENDERER_*`, selected by `WAIFU_FM_CD32X` / `WAIFU_FM_PCFX` / default
   headless), with per-target files `renderer3d_{generic,pcfx,cd32x}.c`. New
   renderer behavior = new capability flag + default, not target `#ifdef`s inline.
5. Asset-mode defines (`WAIFU_ASSET_USE_CDROM` / `USE_CART_ROM` /
   `EXTERNAL_*` / `RAM_BUDGET`) are the SAME on host and console — that's why the
   headless cdrom/cart binaries can validate console load paths without hardware.

After headless passes, verify on the console target the change actually affects,
using its skill: **CD32X** → cd32x-build-verify / cd32x-architecture /
cd32x-improvements; **PC-FX** → pcfx-build-verify / pcfx-architecture. Each has
hardware rules and a verify loop headless cannot exercise.

## What NOT to do
- Do NOT verify a game-logic change only on a console emulator — headless first
  (seconds), console second.
- Do NOT add a new global in main.c without checking the console RAM budgets
  (see cd32x-improvements skill); the core is compiled into every target.
- Do NOT change `game_api.h` signatures without updating ALL frontends:
  `src/main.c` (headless runner), `src/platform/sdl12_main.c`,
  `src/platform/pcfx/pcfx_main.c`, `src/platform/cd32x/cd32x_sh2_main.c`.
- Do NOT write new scenario scripts blind: replay with `--dump-every 1` and read
  the PNGs to confirm the menu path actually reached the intended state.
