# CD32X build & headless verification

Use this skill whenever building or verifying the Sega Mega CD32X (Mega-CD + 32X) target of the waifu card game.

## Agent protocol (MANDATORY — written for both strong and low-capability agents)

Follow this loop exactly. Do not improvise, do not skip gates, do not proceed past a FAILED gate.

1. PLAN: state which files you will edit and why, in one short list, BEFORE editing.
2. EDIT: make the smallest change that implements the plan. Touch only files in the plan.
3. BUILD (gate B): run the build command. If it fails → fix compile errors only, rebuild. Do not start new features while the build is red.
4. SIZE (gate S): run the size check. If FAIL → revert or shrink (see "Size recovery" below). Never ship past this gate.
5. VERIFY (gate V): run a headless capture and LOOK at the PNG. If wrong → go back to step 1 with a new plan; do not stack more edits on an unverified state.
6. COMMIT: only after B, S, V all pass. Remove all throwaway debug defines/hacks first. Commit unsigned (`git commit --no-gpg-sign`).

If the same gate fails twice with the same symptom: STOP, re-read the relevant section below and `docs/cd32x/`, and write a new plan. Do not retry the identical action a third time.

## Build (gate B)

```sh
make -f Makefile.cd32x            # default target `cue` → waifucd32x.iso + waifucd32x.cue
make -f Makefile.cd32x program    # only toolchain-check + M68K/SH-2 binaries (faster)
make -f Makefile.cd32x clean-build   # REQUIRED before rebuilding with changed EXTRA_CFLAGS
```

- Toolchain root: `ROOTDIR` (defaults to `$(GENDEV)`), expected at `/opt/toolchains/sega`. `make -f Makefile.cd32x toolchain-check` verifies it.
- Output tree: `build/cd32x/` (`sh2/`, `m68k/`, `cdroot/`, `cdda/`). ISO writer: `tools/cd32x_iso/bin/genisoimage` (fallback `tools/cd32x_iso/make_cd_iso.py`).
- `CD32X_ASSET_MODE ?= cdrom` selects `-DWAIFU_ASSET_USE_CDROM`; any other value selects `-DWAIFU_ASSET_USE_CART_ROM`. Leave it at `cdrom`.
- PITFALL: dependencies are NOT flag-aware. Changing `EXTRA_CFLAGS` without `clean-build` silently produces a stale mixed build.

## Size check (gate S) — HARD LIMIT

The staged SH-2 image must be **strictly less than 131072 bytes** (BlastEm 128 KiB staging limit). An oversized image does not error — it **hangs at "Uploading SH2 app..."** in the emulator.

```sh
stat -c %s build/cd32x/sh2/waifucd32x.sh2.bin   # PASS if < 131072
```

Size recovery (in order): (1) revert your change and rethink; (2) move const tables to BSS + runtime init (BSS is not staged); (3) check the map file `build/cd32x/sh2/waifucd32x.sh2.map` for what grew; (4) avoid 64-bit `/` or `%` — they link the 620-byte `___divdi3`; use `lldiv_trunc` in main.c (keep its 32-bit fast path — the plain bit-loop version regressed void scene 6→11 vblanks); (5) watch for per-TU `static` arrays in headers (each including TU gets a copy).

## Headless verify (gate V) — BlastEm

The emulator lives in the repo: `CD32X/blastem-headless` (a dependency-free
headless BlastEm build; see `CD32X/HEADLESS.md`), with all six BIOS images
next to it. Always drive it through the wrapper — it writes its own private
config and prepends the BIOS-window STARTs:

```sh
CD32X_BIOS_START_END=3000 BLASTEM_TIMEOUT=500 \
  scripts/cd32x/blastem_headless_capture.sh waifucd32x.cue /tmp/battle.png 14000 \
  scripts/cd32x/battle_mode_input.txt
```

Args: `<cue> <out.png> <frames> [input-script]`.

Env: `BLASTEM` (default `CD32X/blastem-headless`), `BLASTEM_BIOS_DIR` (default:
the emulator's directory), `BLASTEM_HOME` (default `build/cd32x/blastem-home` —
the run never touches `~/.config/blastem`), `BLASTEM_TIMEOUT` (default 900 s),
`BLASTEM_LOG` (default `<out.png>.log`), `BLASTEM_WAV` / `BLASTEM_MKV` for audio
/ video capture, `CD32X_BIOS_START_END` (default 5400) to bound the
auto-generated Sega-CD BIOS-window START presses, and `CD32X_NO_BIOS_SKIP=1` to
suppress them when the supplied script has its own.

Ready-made input scripts in `scripts/cd32x/`:
- `bios_skip_only_input.txt` — the BIOS-window STARTs alone (template / manual use).
- `battle_mode_input.txt` — title → menu → Battle Mode; use with `CD32X_BIOS_START_END=3000`.

Rules:
- ALWAYS open and inspect the output PNG. A capture that ran is not a capture that passed.
- Do NOT spam START past the BIOS window — it enters the menu (and then story mode) and you capture the wrong screen. Pass `CD32X_BIOS_START_END=3000` whenever the script does its own menu navigation.
- Input script syntax is `<frame>f:<token>`, e.g. `6800f:DOWN`. Tokens: `P`/`START`, `A`, `B`, `C`, `X`, `Y`, `Z`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `MODE`; prefix `+` to hold, `-` to release. Explicit binding names (`6800f:gamepads.1.down`) also work. `#` starts a comment.
- Boot timing shifts with every code change → fixed frame numbers are NOT comparable across builds. Drive to a known UI state instead, and capture adjacent frames N and N+1 to detect page-flip flashing (32X has two framebuffer pages; anything not redrawn every frame appears in only one page and flashes).
- Known-good reference points for the current build: title screen at ~frame 6000 with `CD32X_BIOS_START_END=3000`; Battle Mode field at ~frame 14000 with `battle_mode_input.txt`.

## Debug defines for fast iteration (EXTRA_CFLAGS only; throwaway — NEVER commit)

| Define | Effect |
|---|---|
| `CD32X_DEBUG_AUTOBATTLE` | auto-enters battle after boot; cuts iteration ~8 min → ~3 min |
| `CD32X_DEBUG_ENDING` | boots straight into the story ending scene (src/main.c ~line 11000) |
| `CD32X_DEBUG_VOID` | boots straight into the final void scene (src/main.c ~line 11004) |
| `CD32X_DEBUG_BOOT_SCENE=N` | NOT in tree — re-add ad hoc in the `WAIFU_I_TITLE` case: jump to `WAIFU_I_STORY_MAP` with `g_story_progress = g_story_duel_index = N` (0 desert / 2 temple / 3 volcano / 4 void) after ~120 title frames |
| `WAIFU_CD32X_DEBUG_FPS` | on-screen vblanks-per-frame overlay (the perf metric) |
| `WAIFU_PROFILE_RENDER` | render profiling counters |

Example: `make -f Makefile.cd32x clean-build && make -f Makefile.cd32x EXTRA_CFLAGS="-DCD32X_DEBUG_AUTOBATTLE -DWAIFU_CD32X_DEBUG_FPS" all`. For boot-scene perf captures use `CD32X_BIOS_START_END=3000` so no START leaks past the BIOS window.

## Failure → recovery table

| Symptom | Cause | Action |
|---|---|---|
| Hangs at "Uploading SH2 app..." | staged image ≥ 131072 B | gate S recovery |
| Black screen after boot | framebuffer flip removed / FS locked | restore per-frame `cd32x_wait_fb_flip()`; the CPU-visible page is always the back page |
| 1-frame white flashes | screen composed once but pages flip, or palette entry 0 written without priority bit | full redraw every frame OR see palette rule in cd32x-architecture skill |
| Bright dots on MD layer | MD CRAM written during active display | hold CRAM writes to vblank (`set_palette` waits for VDP vblank flag) |
| Music stops when data loads | CD-DA not re-asserted after read | supervisor must re-assert active track/loop after each CD read |
| Input script has no effect | missing BIOS-window STARTs, or the wrong line syntax | use the wrapper (it prepends them) and `<frame>f:<token>` lines, e.g. `6800f:DOWN` |
| Behavior didn't change after flag edit | stale objects | `make -f Makefile.cd32x clean-build` |

## What NOT to do

- Do NOT commit with debug defines, boot hacks, or `EXTRA_CFLAGS` experiments still in place.
- Do NOT edit anything under `src/generated/` — those headers are produced by the asset pipeline.
- Do NOT "fix" CD32X problems by editing PC-FX (`src/platform/pcfx/`) or host (`src/platform/host_*`, `sdl12_main.c`) code.
- Do NOT trust a green build as proof of correctness — only gate V counts.
- Do NOT modify `scripts/cd32x/blastem_headless_capture.sh` to make a failing capture "pass".

## Docs

`docs/cd32x/`: `PORT_SCAFFOLD.md` (structure), `CD32X_RENDER_PERF.md` (perf analysis + implementation status), `CD32X_FIXES_PLAN.md` (known-issue plan), `CDDA.md` (music track map), `CD32X_STREAMING_AUDIO_AND_LZ4W.md` (feasibility study), `ISO_TOOLS.md`.
