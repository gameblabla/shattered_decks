---
name: fmtowns-architecture
description: "Work safely on the bare-metal FM TOWNS/Marty platform implementation: boot memory, 386 code, CRTC/VRAM, CD-ROM and CD-DA ordering, RF5C68 audio, CMOS saves, input, and target-specific performance paths."
---

# FM TOWNS architecture

Read `fmtowns-build-verify/SKILL.md` as well whenever a change will be built or tested. Consult the
relevant section of `src/platform/fmtowns/STATUS.md` for history, but treat its old blanket
real-hardware disclaimers as superseded by the dated ground truth in `AGENTS.md`.

## Layer map

- `src/platform/fmtowns/boot/`: IPL4 boot, real-to-protected-mode transition, fixed-address payload
- `src/platform/fmtowns/common/`: low-level CRTC, VRAM, machine detection, CDC/ISO9660, CD-DA, pad,
  RF5C68, CMOS helpers
- `fmtowns_main.c`: frame loop, platform initialization, debug input and frame stamps
- `fmtowns_video.c`, `fmtowns_audio.c`, `fmtowns_input.c`, `fmtowns_sfx.c`: frontend services
- `waifu_fmtowns_platform.c`, `waifu_fmtowns_runtime.c`, `waifu_fmtowns_cdrom.c`: shared-core seams
- `renderer3d_fmtowns.c`: target renderer backend; shared geometry stays in `renderer3d.c`

`FMTOWNSCD_EXAMPLE_Cube/` is a reference/toolkit and contains Tsugaru/TOWNSEMU. Do not casually edit
or build game behavior out of copied reference sources when the maintained implementation is under
`src/platform/fmtowns/`.

## Boot and memory invariants

- The payload is fixed at `0x10000`, built freestanding for i386 with no DOS, TOWNS OS, RUN386, or
  FPU. It is deliberately non-PIC; restoring `-fPIC` costs `%ebx` and hot-path indirections.
- Loaded bytes must end before `0x90000`, where the boot loader and stack live. Do not weaken the
  Makefile check or assume the broader sub-`0xC0000` region is safe.
- `.bss` begins in extended RAM near `0x100000`; the 2 MiB Marty link is the compatibility floor.
  Check the linker result before adding framebuffer-sized caches. Do not rely on the user's larger
  non-Marty RAM configuration unless behavior is runtime-sized and Marty-safe.
- Avoid libc/OS dependencies. Target-local optimized paths may use i386 properties such as
  unaligned dword stores, but keep them behind `WAIFU_FM_FMTOWNS`; V810 and SH-2 do not share those
  semantics.

## Video and presentation

The game uses 256x240 indexed 8bpp in single-page mode. FM TOWNS 8bpp single-page VRAM is swizzled
across two banks/register sets; both CRTC register sets must be programmed. Runtime machine-ID port
`0x30` selects the narrow Marty/UX VRAM map versus standard models. Do not return to a compile-time
VRAM base.

There is no 256-color two-layer/sprite configuration: two-page modes are 4bpp or 16bpp, and sprite
hardware requires a compatible page-1 16bpp setup. Treat “put UI on sprites/layer 1” as a port-scale
color-depth redesign requiring measurement, not a small optimization.

Presentation relies on:

- palette comparison so unchanged palettes are not re-uploaded;
- page flip plus exactly one vblank wait;
- dirty 64-byte framebuffer groups, unioned across the previous two frames because the writable
  page is two frames old;
- correct retained-region declarations/restores for cached board, story, battle, and transition
  compositions.

When modifying damage/caches, use the damage verifier and byte-identical before/after frame runs.
Stripes, stale hand fragments, one-page-only updates, or every-other-frame errors usually indicate
wrong page history or incomplete damage rather than scene logic.

## CD-ROM, music, and diagnostics

Assets are ISO9660 2048-byte-sector blobs resolved by `waifu_fmtowns_cdrom.c`. Preserve slice
semantics: aligned whole sectors go directly to the destination; unaligned heads/tails use a
one-sector scratch buffer.

The physical game/CD-ROM path is owner-confirmed working as of 2026-08-27. Preserve the current CDC
ordering and diagnostics rather than reopening the obsolete “has this ever worked?” question:

- the undocumented `A0` setup sequence was derived from Marty boot-ROM traffic and precedes reads;
- `fmtowns_cd_diag` labels stages before potentially blocking work, writes breadcrumbs to both VRAM
  pages while loading, retries reads, drains status, and exposes retry/continue UI;
- `FMTOWNS_CD_FORCE_FAIL=n` tests failure policy without requiring a bad drive.

CD-DA and data reads share one drive. Asset reads stop playback; the CD backend notifies the audio
layer and per-frame reconciliation restarts the desired track after the burst. Do not make arbitrary
render-time CD reads or assume music continues across data traffic. Track constants are generated
from `MUSIC_STEMS`; keep disc order and code mapping generated together.

## Input, saves, and time

- Shipping input always reads the real pad. `FMTOWNS_DEBUG_INPUT=1` only ORs deterministic scripted
  input into it; keep this compile-time-only and do not bypass the real path.
- Saves use battery-backed CMOS through the platform storage seam. Emulator CMOS tests do not by
  themselves establish persistence on every physical setup.
- Game logic remains a 60 Hz frame machine even when rendering is slower. The platform paces/skips
  rendering without changing gameplay time.
- Frame stamps use the machine timer and compensate for its narrow counter/aliasing behavior.
  Preserve the existing timing helpers rather than subtracting raw samples ad hoc.

## Performance discipline

The main bottleneck is bytes moved on a cacheless 386-class CPU/16-bit bus, especially framebuffer
clears, composite copies, and VRAM writes. Prefer drawing/restoring fewer bytes and stable retained
regions over clever per-pixel arithmetic. Measure isolated parked scenes with `fmtowns.sh profile`.

The runtime machine-ID CPU class chooses retained-camera density (386SX, 386DX, 486/Pentium) while
gameplay timing remains identical. `WAIFU_FMTOWNS_FORCE_PERFORMANCE_TIER` is a measurement override,
not shipping policy. Preserve ordered endpoints and validate every tier when changing anchor logic.

Useful measurement defines include `WAIFU_BATTLE_BASE_CACHE_DISABLE`, `WAIFU_PLAZA_CACHE_DISABLE`,
`FMTOWNS_MEASURE_BLIT_EVERY=n`, `FMTOWNS_MEASURE_DIRTY_BAR`, and the renderer skip defines. A
measurement define must not leak into the shipping build.
