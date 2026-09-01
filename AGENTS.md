# Agent Guide

This repository is a multi-target C card game. Shared gameplay and rendering live mainly in
`src/main.c`, `src/game/`, and `src/engine/`. Platform frontends live under `src/platform/`.

Keep this file small: it is injected into every coding-agent request. Detailed, conditional
knowledge belongs in `.claude/skills/` and must be read only when relevant.

## Required skill routing

Read the relevant `SKILL.md` completely before editing or diagnosing that area:

- Shared core, assets, or headless tests: `.claude/skills/headless-core/SKILL.md`
- FM TOWNS build, emulator capture, profiling, or image inspection:
  `.claude/skills/fmtowns-build-verify/SKILL.md`
- FM TOWNS boot, memory, CD-ROM, video, audio, input, or performance code:
  `.claude/skills/fmtowns-architecture/SKILL.md`
- Local pi/ninfer startup, tool calls, context faults, or vision:
  `.claude/skills/local-pi-ninfer/SKILL.md`
- PC-FX: `.claude/skills/pcfx-build-verify/SKILL.md` and
  `.claude/skills/pcfx-architecture/SKILL.md`
- CD32X: `.claude/skills/cd32x-build-verify/SKILL.md`,
  `.claude/skills/cd32x-architecture/SKILL.md`, and, for optimization work,
  `.claude/skills/cd32x-improvements/SKILL.md`
- MSX2: `MSX2_PORT_PLAN.md` (design) and `src/msx2/STATUS.md` (state, build,
  verification). This target is a fork: it never compiles `src/main.c`, and MSX
  code stays inside `src/msx2/` and `tools/msx2/`.

## Project ground truth

- Physical hardware is authoritative over emulator inference.
- As of 2026-08-27, the owner has confirmed that the current FM TOWNS game runs on real
  hardware and that CD-ROM access works after the latest CD-ROM-access update. A phone video,
  `IMG_6472.mov`, also records the game running on real hardware. Do not repeat stale claims in
  `src/platform/fmtowns/STATUS.md` that nothing has run on physical hardware.
- Emulators remain essential regression tools, but Tsugaru timing is calibrated rather than a
  physical measurement. Do not turn emulator timing into a real-hardware claim.

## High-value entry points

- Shared game/frame machine: `src/main.c`, `src/game/game_api.h`
- Platform seams: `src/engine/platform.h`, `src/game/assets.c`
- Host builds: `Makefile`; SDL 1.2 glue: `src/platform/sdl12_main.c`
- FM TOWNS: `Makefile.fmtowns`, `fmtowns.sh`, `src/platform/fmtowns/`,
  `tools/fmtowns/`, `src/platform/fmtowns/STATUS.md`
- PC-FX: `Makefile.pcfx`, `src/platform/pcfx/`
- CD32X: `Makefile.cd32x`, `src/platform/cd32x/`, `docs/cd32x/`
- MSX2: `Makefile.msx2`, `msx2.sh`, `src/msx2/`, `tools/msx2/`,
  `src/msx2/STATUS.md`. Verify with real openMSX, never with the
  openmsx-headless bundled in MSXgl -- its Z80 does not implement
  `LD r,(IX+d)`/`LD (IX+d),r`, so no SDCC-compiled C survives on it.

## Working rules

- Preserve unrelated user changes in the dirty worktree.
- Generated headers and blobs are outputs of `tools/gen_*.py`; regenerate them through the
  appropriate Makefile rather than editing them by hand.
- Keep common code platform-agnostic and use the seams in `src/engine/platform.h`. Console
  builds define `WAIFU_FM_NO_HEADLESS_MAIN`; headless-only APIs and tests must remain guarded.
- For a shared-code change, run the relevant headless regression first, then build and verify
  every affected console target using its skill.
- Verification means checking observable output. A successful build, a live emulator process,
  or an all-black screenshot is not by itself visual proof.
