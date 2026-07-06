---
name: pcfx-build-verify
description: Build the NEC PC-FX CD image and verify changes in the pcfx-headless emulator — the build command, the accurate-backend capture flow, boot timing, BackupRAM/save-dir handling, driving the menu with command scripts, and a failure→recovery table. Use whenever building or confirming any PC-FX change; this is the loop that closes the "I cannot verify PC-FX behavior" gap.
---

# PC-FX build & headless verification

Use this whenever you build or verify the PC-FX target. Green build ≠ correct —
only a looked-at capture counts. Mirrors the CD32X gate protocol.

## Agent protocol (MANDATORY)

1. PLAN the files you will edit and why (one short list) BEFORE editing.
2. EDIT the smallest change; touch only planned files.
3. BUILD (gate B). Red build → fix compile errors only, rebuild.
4. VERIFY (gate V) → capture a frame and LOOK at the PNG at the intended state.
5. COMMIT only after B and V pass; remove throwaway debug first; commit unsigned
   (`git commit --no-gpg-sign`).

If the same gate fails twice with the same symptom: STOP, re-read this skill +
pcfx-architecture + AGENTS.md, write a new plan. Do not retry the identical action.

## Build (gate B)

```sh
make -f Makefile.pcfx cd V810GCC=/opt/v810-gcc PREFIX=v810
```
- Toolchain at `/opt/v810-gcc` (`v810-gcc`). Outputs `waifupcfx.cue` + `waifupcfx.bin`
  (+ CDDA `waifupcfx_t0*.bin`), and a `waifupcfx_pyramid.cue` alias.
- The `cd` target LINKS THREE TIMES on purpose (LBA/CDDA table must match the
  final image — enabling generated LBAs can shift the boot binary a sector). It
  is slower than it looks; don't interrupt it or collapse it to one pass.
- Pre-existing warnings are noisy (`waifu_card_names` unused, cdrom implicit
  decls, etc.). Do not chase them — but DO watch for NEW warnings your change
  introduced, especially `-Wcharacter-constant-too-long` / "comparison is always
  true" (that class silently broke saves once — see pcfx-architecture).
- IDE clang diagnostics like `'defines.h'/'title_asset.h' file not found` on
  main.c are just missing include paths in the IDE, NOT real build errors.

## Verify (gate V) — pcfx-headless

Emulator: `pcfx-headless` (upstream-accurate backend). Capture flow:

```sh
# 1. Boot to a state once (boot is slow — see timing note). --auto-run pulses START past BIOS.
pcfx-headless --bios-dir . --frames 2700 --auto-run --pcfx \
  --state-out boot.sav --y4m boot.y4m waifupcfx.cue
# 2. Drive inputs from a saved state (fast iteration):
pcfx-headless --bios-dir . --state-in boot.sav --frames N --pcfx \
  --commands cmds.txt --y4m out.y4m waifupcfx.cue
# 3. Extract + crop (y4m is 512px wide; content is the LEFT 256):
ffmpeg -y -i out.y4m -vf "crop=256:240:0:0" f_%03d.png
```

Command file lines are `<frame> <STATE-or-±BTN>`: `10 START` (absolute press),
`13 NONE` (release all), `40 +A` / `70 -A` (relative). Buttons:
`A B C X Y Z START SELECT UP DOWN LEFT RIGHT`. RUN == START.

CRITICAL backend note: do NOT pass `--fast-video` when verifying title/menu/fades.
The legacy fast RAINBOW backend does not render the VDC overlay layer, so the
dither fade is invisible and the KRAM garbage the VDC mask hides shows through —
you would "verify" a screen real hardware never shows. Use the default backend.

### Boot timing — do NOT hardcode frame numbers

Boot to the title takes a long, build-dependent stretch (roughly 1000–2700
emulated frames; the "LOADING… TITLE 0%" screen shows first, then the 16M title).
It shifts with every code/asset change. So: capture a BROAD window and sample it,
or `--state-in` a known-good boot state and drive relative to that — never assert
a fixed "title is at frame X" across builds.

### Driving the menu (gotchas that cost real iterations)

- One `START` on the title reveals the menu. A SECOND `START` then CONFIRMS the
  currently-highlighted option — do not add a "just in case" second START or you
  will select STORY MODE instead of navigating. Reveal once, then `DOWN`×N, then
  `START`.
- Menu order: STORY MODE (0), BATTLE MODE (1), LOAD STORY (2). LOAD STORY is
  dimmed unless a save exists.
- Sample several frames across your input window and read them; a black frame is
  usually a legitimate fade/transition, not a hang — extend the run and re-sample
  before concluding "stuck".

### BackupRAM / save testing (`--save-dir`)

- BackupRAM persists as `<save-dir>/sram/<cuebasename>.srm` (default save-dir is
  `$HOME/.pcfxemu`). So a save written in one run is visible to the next.
- To test loading WITHOUT touching the user's real BRAM, copy an `.srm` into an
  isolated dir and pass `--save-dir /tmp/mydir`. The save blob inside is the
  123-byte `WAIF`-magic record (grep the `.srm` for `WAIF\x01`; verify the u16
  checksum of bytes 0..120 before trusting it).
- The BRAM read via the BIOS filesystem is SLOW in the emulator (it blocks the
  CPU across many video frames, holding the "BACKUP RAM LOADING…" screen); that
  is emulator timing, not a hang — extend the run to see it reach the map.
- `--dump saveram FILE` dumps BRAM; `--state-in/--state-out` snapshot full state
  (state embeds the loaded program image, so a state made by an OLD build runs
  the OLD code — rebuild + fresh-boot to test a code change, don't reuse a stale state).

## Ready-made PC-FX scripts

`scripts/pcfx/`: `hand_card_render_regression.sh`, `battle_full_card_render_regression.sh`,
`battle_card_stripe_regression.sh`, `ingame_card_render_regression.sh`,
`story_save_srm_smoke.sh` (save/BRAM), `story_sanctum_rainbow_hscroll_regression.sh`,
`capture_random_battle_playthrough.sh`, plus `*.commands` input files. `Makefile.pcfx`
also exposes them as targets (`make -f Makefile.pcfx <name>`). Reuse one before
writing a new command script.

## Failure → recovery table

| Symptom | Likely cause | Action |
|---|---|---|
| Title/ending diagonally SHEARED | title YUV422 blob width ≠ 256 (wrong resolution baked) | check `TITLE_SCREEN_W` in `src/generated/title_asset.h` = 256 and the `.bin` = 131072 B; see headless-core asset-pipeline note |
| Pink wash / diagonal seam on boot | 16M mode switched before VDC black mask / title upload | raise VDC mask BEFORE `eris_king_set_bg_mode(16M)` (pcfx-architecture) |
| Stripe band / white edge line mid-transition | KRAM offset-0 shown across 16M↔8bpp switch | keep offset 0 as 16M-black; display the SECOND 8bpp page |
| Scene flashes fully-lit for 1 frame before black | palette fade reset to full during black hold | keep `apply_black_dither_fade(0)` through the hold + clear at black |
| WHITE flash at boot / as the loading screen appears | 8bpp KRAM filled with `IDX_BLACK` index but VCE palette entries not written until first present (undefined→bright at cold boot) | `begin_8bpp` must blacken all 256 VCE entries after `set_king_8bpp_video` (pcfxemu hides it — reason about init ordering) |
| Comb garbage / card flicker | 8bpp blit desynced `page_shadow` from KRAM | update/invalidate the matching shadow band on every KRAM write |
| Title CD-DA silent / Battle-Mode select stalls | CD-DA started before assets ready, or stopped during a visible fade | gate on `waifu_assets_title_ready()`; never SCSI-stop mid-fade |
| Sanctum scene disappears on save/deck-editor | routed through VDC-only overlay bypass | render on the CPU framebuffer over `draw_story_sanctum_background()` |
| LOAD STORY always fails / "glitches to menu" | save magic / checksum mismatch | verify `save_build_blob`/`save_parse_blob` magic is `'W'` and checksum math matches |
| Fade invisible / KRAM garbage visible in capture | used `--fast-video` | recapture with the default accurate backend |

## What NOT to do

- Do NOT verify a PC-FX change only by reading code — capture and look.
- Do NOT use `--fast-video` for title/menu/fade verification.
- Do NOT hardcode boot frame numbers across builds; drive to a UI state.
- Do NOT reuse a `--state-in` snapshot from a different build to test a code change.
- Do NOT commit with debug hacks in place; remove and rebuild first.
- Do NOT "fix" a PC-FX bug by editing CD32X or host files.
- Do NOT edit `src/generated/*` by hand to make a capture look right — fix the
  source/generator (headless-core skill) and rebuild.
