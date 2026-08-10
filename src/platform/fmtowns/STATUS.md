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

2. **Real framebuffer/palette present with the game's actual art -- DONE,
   verified booting.** `src/platform/fmtowns/fmtowns_video.c` wraps
   libfmt's CRTC/palette/VRAM calls into a `fmtowns_video_init()` /
   `fmtowns_video_present_8bpp()` / `fmtowns_video_wait_vblank()` shape
   matching the CD32X/PC-FX ports' present call. `fmtowns_main.c` now
   presents the game's real 256x240 8bpp title-screen art (the same
   `src/generated/title_asset.h` every other platform's title screen comes
   from) instead of the milestone-1 synthetic pattern.
   `tools/fmtowns/extract_title_asset.py` repacks that header's two arrays
   to raw `.bin` files at build time; `fmtowns_title_asset.S` links them
   directly into the payload with `.incbin` (CD-ROM loading is
   deliberately deferred to milestone 3 so this milestone isolates "does
   the present pipeline render real production pixels/palette correctly,"
   separately from "does the CD reader work"). Verified booting in
   Tsugaru_CUI with the same headless `SS` recipe -- the captured frame is
   the correct title art, matching the source PNG exactly (checked
   visually).

3. **CD-ROM asset loading -- DONE, verified booting.** The same title
   asset milestone 2 linked in with `.incbin` is now staged onto the CD
   image as `CD/TITLE.BIN` / `CD/TITLE.PAL` (`Makefile.fmtowns`) and read
   back at runtime by `fmtowns_present_title_asset_cdrom()` in
   `fmtowns_main.c`, through `media.h`'s `fmt_media_load()` ->
   `fmt_iso9660_load()` -> `fmt_cdrom_read()`
   (`src/platform/fmtowns/common/iso9660.c` + `cdrom.c`, both already
   copied in since milestone 1 but unused until now). On a short read of
   either file it presents a solid diagnostic colour and halts rather than
   showing a half-loaded frame (there is no console output on this target
   to report a failure through otherwise). Verified booting in
   Tsugaru_CUI: the captured `SS` screenshot is **byte-identical** (same
   PNG checksum) to milestone 2's linked-in version, proving the CD read
   reproduced the exact same bytes.

4-6. **Not started.** See "Next steps" below for what each needs.

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

1. Implement `src/engine/platform.h`'s full seam (storage, background
   layer, widescreen HUD, text overlay, story portraits) in a new
   `waifu_fmtowns_platform.c`, following `waifu_cd32x_*` as the template
   the task brief calls out. Storage needs a decision on where FM TOWNS
   Marty save data lives (the Marty has no standard battery-backed SRAM
   like the CD32X's Backup RAM; likely candidates: an IC memory card, if
   one is assumed present, or accept saves are session-only on this
   target -- needs a call from whoever picks this up next). CD asset
   loading itself (the mechanics) is proven by milestone 3 above; what's
   left here is wiring it to the game's actual blob/asset table the way
   `waifu_cd32x_cdrom.c` does, once the core exists to ask for blobs.
2. Input: `src/platform/fmtowns/common/pad.c` is already copied in and
   unused; wire it to `WaifuFmInput` the way
   `waifu_cd32x_input.c` does.
3. Audio: `src/platform/fmtowns/common/cdda.c` is copied in and unused;
   wire music through it (CD-DA is the primary path per the brief; DAC-PCM
   via `pcmstream.c`/`dacout.c`/`mp2*.c` is also already present as the
   documented fallback but should stay unused unless CD-DA turns out not to
   fit).
4. Flat-shaded 3D renderer: write `src/engine/renderer3d_fmtowns.c`
   analogous to `src/engine/renderer3d_cd32x.c` (read that file first --
   it is the actual current source of truth for the flat-shaded approach,
   the CPU budget assumptions there likely need re-deriving for the
   TOWNS' 386SX/DX class CPU rather than the 32X's SH-2), and register it
   in `src/engine/renderer3d.c`'s per-platform dispatch.
5. Once 1-4 land, get the actual game core (`src/main.c` equivalent to
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

Attempt to get the portable game core compiling freestanding (item 7 above)
as soon as milestone 3 (CD loading) lands -- that is the step most likely to
surface fundamental blockers (missing libc pieces, `.bss`-over-0xC0000
overflow given the game's much larger asset/state footprint than this
milestone's ~90 KB payload) that should be discovered early rather than
after building out the whole asset/audio/input/renderer pipeline around a
core that turns out not to fit. Everything built so far (video present,
soon CD loading, input, CD-DA, flat-shaded renderer) can be validated
standalone, but it only becomes the actual game once the core compiles for
`-m32 -march=i386 -ffreestanding`, and that has not been attempted at all
yet.
