# FM TOWNS Marty port -- status

Branch/worktree: `.claude/worktrees/agent-abf2922f9621eb1c6` (branch
`worktree-agent-abf2922f9621eb1c6`, based on `fix/revert-vdc-text-rainbow`).

## What this is

A from-scratch bare-metal FM TOWNS Marty port, following the same
platform-seam pattern as `src/platform/cd32x/` and `src/platform/pcfx/`.
Boots straight off CD via a hand-written IPL4 boot sector into 32-bit flat
("unreal") protected mode -- no DOS, no TOWNS OS, no RUN386 extender. The
boot path, CRTC/palette/VRAM helpers, CD-ROM/ISO9660 reader and CD-DA code
are adapted from `FMTOWNSCD_EXAMPLE_Cube/` (a working reference toolkit kept
at the repo root **read-only** -- nothing builds out of that directory
itself; everything needed was copied into `src/platform/fmtowns/`).

Build entry point: `Makefile.fmtowns` at the repo root (`make -f
Makefile.fmtowns`, `make -f Makefile.fmtowns run`). Verified emulator:
`FMTOWNSCD_EXAMPLE_Cube/Tsugaru_CUI.elf` (TOWNSTYPE MARTY), the same one
that example's own `run.sh` uses.

## Directory layout

- `src/platform/fmtowns/boot/` -- IPL4 boot sector (`bootsect.S`/`setup.S`),
  the real-mode -> 32-bit flat-mode transition and payload self-relocation
  (`head.S`, `reloc.c`), linker scripts. Copied verbatim from
  `FMTOWNSCD_EXAMPLE_Cube/src/boot`; not yet modified for this game.
- `src/platform/fmtowns/common/` -- "libfmt": CRTC/VIDEO port I/O and
  palette (`common.c`), video-mode + VRAM present (`libfmt.c`/`.h`),
  CD-ROM sector reads (`cdrom.c`), ISO9660 lookup (`iso9660.c`), CD-DA
  (`cdda.c`), pad/keyboard input (`pad.c`), plus not-yet-wired extras
  (`fmt_layers.c`, `fmt_sprite.c`, `mp2*.c`, `mbv*.c`, `dacout.c`,
  `pcmstream.c`, `vgmplay.c`, `sound.c`) copied wholesale for later
  milestones to draw on. Also copied verbatim.
- `src/platform/fmtowns/fmtowns_main.c` -- **new**, this port's own C entry
  point (`start_main()`, called by `head.S`). Currently milestone 1 only:
  draws a synthetic 256x240 8bpp test pattern and idles on vsync. Does not
  yet call the portable game core (`src/game/`, `src/engine/`).
- `Makefile.fmtowns` (repo root) -- milestone-1 build: compiles the boot
  trampoline + libfmt + CD/ISO9660 + `fmtowns_main.c` into
  `build/fmtowns/output.iso`.
- `tools/fmtowns/mkcd.sh` + `inject_ipl.py` + `iso_sort.txt` -- copied from
  the example's `tools/`, `mkcd.sh` adjusted to resolve its own
  `iso_sort.txt`/`inject_ipl.py` by script location (the original assumed
  cwd == repo root; here it runs from `build/fmtowns/`).

Not yet touched, and out of scope for this session's changes:
`src/platform/cd32x/`, `src/platform/pcfx/`, `src/platform/sdl3/`,
`src/game/`, `src/engine/*` (other than reading them for reference).

## Milestones

1. **Build skeleton boots to a test pattern -- DONE, verified booting.**
   `make -f Makefile.fmtowns` produces `build/fmtowns/output.iso`.
   Confirmed booting for real in Tsugaru_CUI.elf (headless, see "Headless
   emulator verification" below): a 256x240 8bpp checkerboard test pattern
   with a white centre block renders correctly, matching
   `fmtowns_draw_test_pattern()` exactly. This proves the whole chain end to
   end: IPL4 boot sector -> protected-mode head.S -> payload
   self-relocation -> `start_main()` -> CRTC mode set -> palette load ->
   VRAM present -> page flip -> vsync wait, all on real (emulated) FM TOWNS
   Marty hardware timing, not just "it compiles."

2-7. **Not started.** See "Next steps" below for what each needs.

## Headless emulator verification (how, and a caveat)

Tsugaru_CUI.elf needs a real GL-capable display; this container has none, so
verification runs under `xvfb-run` and drives the emulator's stdin command
console (it is an interactive machine monitor when run without `-UNITTEST`).
The command that captures a frame is `SS <path.png>` (see `-HELP`'s "Save a
screenshot" and "Quick Screen Shot" entries). Working recipe:

```sh
{ sleep 8; printf 'SS /tmp/shot.png\nQUIT\n'; } | \
  xvfb-run -a timeout -s KILL 30 \
  ./FMTOWNSCD_EXAMPLE_Cube/Tsugaru_CUI.elf \
  /abs/path/to/FMTOWNSCD_EXAMPLE_Cube/MARTY_ROM/ \
  -TOWNSTYPE MARTY -CD build/fmtowns/output.iso -NORMALFD -DONTUSEFPU -NOWAITBOOT
```

**Caveat**: the very first attempts at this (booting the *unmodified*
`FMTOWNSCD_EXAMPLE_Cube` example itself, before any of this port's own code
existed) produced an all-black, byte-identical `SS` screenshot no matter how
long the sleep before capture (tried 0s/15s/20s, and three captures 8s apart
mid-run all hashed identical). That was never root-caused -- it might be a
genuine timing/BOOTKEY issue with that particular example configuration, or
a quirk of the xvfb/software-GL screenshot path in this environment. It
matters here only as a warning: **a black `SS` screenshot alone does not
prove a boot failure** in this setup, and conversely a real terminated
process (`Tsugaru_CUI.elf` running at high CPU, `SS` executing without
error) does not by itself prove that video came up. Milestone 1 above is
trusted specifically because the *captured pixels matched the source code's
intended pattern* -- that is the verification bar to use for every future
milestone in this environment, not just "the emulator didn't crash." If a
future screenshot ever comes back black/wrong, re-run with a longer sleep
and a fresh xvfb server before concluding the payload itself is broken.

Also note: `timeout -s KILL` is required -- plain `timeout` (SIGTERM) did
not reliably stop a hung Tsugaru_CUI.elf in this environment; kill -9 by PID
was needed once. Always background/timeout-guard interactive Tsugaru runs.

## Next steps, in priority order

1. **Palette + framebuffer present wired to the game's real 8bpp asset
   pipeline** (milestone 2). Needs: deciding how `WAIFU_FM_WIDTH`/`HEIGHT`
   (check `src/engine/`) map onto the 256x240 8bpp mode `libfmt.c` already
   supports, and a `waifu_fmtowns_video.c` seam file analogous to
   `src/platform/cd32x/waifu_cd32x_video.c` that calls `fmt_set_mode()` /
   `fmt_load_palette()` / `fmt_put_image()` / `fmt_flip_page()` against the
   game's actual framebuffer + palette rather than a synthetic pattern.
2. Implement `src/engine/platform.h`'s full seam (storage, background
   layer, widescreen HUD, text overlay, story portraits) in a new
   `waifu_fmtowns_platform.c`, following `waifu_cd32x_*` as the template
   the task brief calls out. Storage needs a decision on where FM TOWNS
   Marty save data lives (the Marty has no standard battery-backed SRAM
   like the CD32X's Backup RAM; likely candidates: an IC memory card, if
   one is assumed present, or accept saves are session-only on this
   target -- needs a call from whoever picks this up next).
3. CD asset loading wired through `src/common/cdrom.c` + `iso9660.c`
   (already copied in, unused so far) into the game's asset seam --
   mirror `waifu_cd32x_cdrom.c`'s blob-table approach, adapted to
   ISO9660 filenames instead of Sega CD's BIOS load-file calls.
4. Input: `src/platform/fmtowns/common/pad.c` is already copied in and
   unused; wire it to `WaifuFmInput` the way
   `waifu_cd32x_input.c` does.
5. Audio: `src/platform/fmtowns/common/cdda.c` is copied in and unused;
   wire music through it (CD-DA is the primary path per the brief; DAC-PCM
   via `pcmstream.c`/`dacout.c`/`mp2*.c` is also already present as the
   documented fallback but should stay unused unless CD-DA turns out not to
   fit).
6. Flat-shaded 3D renderer: write `src/engine/renderer3d_fmtowns.c`
   analogous to `src/engine/renderer3d_cd32x.c` (read that file first --
   it is the actual current source of truth for the flat-shaded approach,
   the CPU budget assumptions there likely need re-deriving for the
   TOWNS' 386SX/DX class CPU rather than the 32X's SH-2), and register it
   in `src/engine/renderer3d.c`'s per-platform dispatch.
7. Once 2-6 land, get the actual game core (`src/main.c` equivalent to
   `cd32x_sh2_main.c`'s `waifu_fm_init()`/`waifu_fm_step()` loop) compiling
   freestanding for `-m32 -march=i386 -ffreestanding`: this is likely the
   single largest remaining unknown -- the portable core was written
   assuming a hosted C library in places (malloc, libc string/math calls);
   expect to need freestanding shims or `-DWAIFU_FM_NO_HEADLESS_MAIN`-style
   feature gates the way the CD32X SH-2 build needed (see
   `Makefile.cd32x`'s `SH2_CFLAGS_BASE` for the flags it needed:
   `-DWAIFU_FM_NO_HEADLESS_MAIN -DWAIFU_ASSET_NO_STDIO
   -DWAIFU_ASSET_RAM_BUDGET=...` etc). Budget real time for this; it was
   not attempted in this session at all.

## The single most important thing to do next

Wire a `waifu_fmtowns_video.c` (milestone 2) that presents the *actual*
game framebuffer/palette instead of the synthetic test pattern, so the next
session's own "does it boot" bar is meaningful for real game pixels, and
then immediately attempt to get the portable game core compiling
freestanding (next-next step, item 7 above) -- that is the step most likely
to surface fundamental blockers (missing libc pieces, `.bss`-over-0xC0000
overflow given the game's much larger asset/state footprint than this
milestone's ~30 KB payload) that should be discovered early rather than
after building out the whole asset/audio/input pipeline around a core that
turns out not to fit.
