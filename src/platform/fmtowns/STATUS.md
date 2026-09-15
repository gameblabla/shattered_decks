# FM TOWNS Marty port -- status

> **Physical-hardware update (2026-08-27):** The owner has confirmed that the
> current game runs on real FM TOWNS hardware and that CD-ROM access works after
> the latest CD-ROM-access update. `IMG_6472.mov` is also a phone recording of
> the game running on real hardware. Older statements below that “nothing has
> run on physical hardware” are historical and must not be treated as current
> blanket truth. This confirmation is scoped: it does not by itself verify
> every scene, audio output, save persistence, controller path, or frame-time
> measurement.

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

4. **Input -- DONE, verified booting.** `fmtowns_input.c`/`.h` wrap
   `src/platform/fmtowns/common/pad.c`'s `fmt_pad_read(0)` (unused since
   milestone 1) behind `fmtowns_input_read_pad1()`. `fmtowns_main.c`'s
   `fmtowns_pad_status_loop()` reloads the title asset off CD exactly like
   milestone 3, then stays live: every vblank it re-reads the pad and
   repaints a 12-cell status strip across the bottom of the screen (one
   cell per pad bit, palette indices 254/255 repurposed for lit/unlit --
   a debug overlay, not final HUD art). Verified booting in Tsugaru_CUI:
   the strip renders as 12 distinct dark cells, the expected idle
   (all-released) state with no controller attached, and the boot stays
   alive through the frame capture rather than hanging on the pad
   driver's I/O strobe wait loop. **Caveat**: this environment has no way
   to inject a real button press into the emulator (no `SENDKEY`-style
   console command was found -- see the verification section below), so a
   strip reacting to actual input was not observed, only that the read
   call runs safely every frame and returns a plausible idle value. A
   session with real hardware or a GUI Tsugaru build with a bound gamepad
   should confirm the strip actually lights up on a press.

5. **CD-DA audio -- DONE, verified booting with a real playback-state
   round trip.** `fmtowns_audio.c`/`.h` wrap
   `src/platform/fmtowns/common/cdda.c` (unused since milestone 1):
   `fmtowns_audio_start_music()` initializes the mixer, reads the disc
   TOC, finds the first audio track and starts it looping;
   `fmtowns_audio_state()` polls `fmt_cdda_state()`. `Makefile.fmtowns`
   now builds a real mixed-mode disc by default (`$(OUTPUT_CUE)`,
   `output.bin`/`output.cue` instead of a plain `.iso`): a 20-second clip
   of `Music/Titlescreen_MoonlitCipher.wav` (the same title music
   PC-FX/CD32X play, trimmed only so local build/boot iteration stays
   fast) staged as Red Book audio track 2 behind the ISO9660 data track 1,
   via `tools/fmtowns/mkcd.sh`'s existing multi-track path. Per
   `cdda.h`'s own documentation, the drive cannot read data sectors while
   a track plays, so playback starts only after
   `fmtowns_load_title_asset_cdrom()`'s CD reads are done, right before
   the live pad loop begins. A top-left 8x8 corner swatch repaints every
   frame from the live `fmt_cdda_state()` poll -- green while playing, red
   otherwise -- since nothing in this headless setup can otherwise confirm
   audio came out of the (virtual) speakers.

   Verified booting in Tsugaru_CUI with the CD-DA disc: **the swatch is
   green**, meaning the emulator's own CDC model accepted the play command
   and is reporting `FMT_CDDA_PLAYING` back through the same polling path
   real hardware uses -- a genuine round trip through the playback-state
   machinery, not just "didn't crash." (Actual audio *output* -- samples
   reaching a speaker/WAV capture -- was not separately verified; Tsugaru
   was run with default audio settings and no capture was attempted. The
   state round trip is the strongest signal available without one.)
   **Known cosmetic side effect**: the swatch reuses palette index 253,
   which the title art also happens to use elsewhere (visible as green
   speckling on the character's clothing in the milestone 5 screenshot) --
   harmless for a debug overlay, but a real HUD would need a reserved
   index the way `tools/gen_cd32x_title_asset.py` reserves index 0 for
   CD32X's title asset.
   `Makefile.fmtowns run-iso`/`iso` still build and boot the plain
   data-only disc from milestones 1-4 if a CD-DA-free build is needed.

6. **Flat-shaded 3D renderer -- DONE (standalone demo), verified booting.**
   Two separate things landed together, deliberately kept apart:
   - `src/engine/renderer3d_fmtowns.c` + a new `WAIFU_FM_FMTOWNS` branch in
     `src/engine/renderer3d_port.h` (copying CD32X's portable-C,
     flat-shaded-only span-filler settings, minus the SH-2 asm leaves)
     register the seam the task brief asks for -- the same
     shared-geometry-in-`renderer3d.c` / target-specific-span-filler split
     CD32X and PC-FX use. **This is not yet compiled by anything**:
     `Makefile.fmtowns` does not build `src/engine/*.c` at all (the
     portable game core doesn't compile for this target yet -- see item 5
     of "Next steps"), so this file is unverified beyond "it is valid C
     that mirrors a working pattern."
   - `src/platform/fmtowns/fmtowns_cube_demo.c`/`.h`: a genuinely
     standalone, boot-verified flat-shaded rasterizer -- a rotating cube,
     6 faces each one flat colour, backface-culled via a 2D signed-area
     test, integer-only fixed-point math throughout (no FPU is assumed;
     rotation uses Bhaskara I's integer sine approximation rather than a
     lookup table or floats). `fmtowns_main.c`'s new
     `fmtowns_cube_demo_loop()` replaces the title art as the main scene
     while keeping the same pad-status strip and CD-DA swatch overlays
     from milestones 4-5 running underneath/alongside it, and starts
     CD-DA immediately (no CD reads needed for this demo, so no
     read-before-play ordering constraint to satisfy first).

   Verified booting in Tsugaru_CUI: the captured frame shows three
   correctly backface-culled faces (purple/red/yellow, each a solid flat
   colour) forming a partially-rotated cube, with the CD-DA swatch
   (green, still playing) and pad strip both composing correctly on top.
   **Known rendering artifact**: a thin 1-pixel gap/seam is visible
   between two adjacent faces in the captured frame -- almost certainly
   an off-by-one in the scanline edge-intersection fill
   (`fmtowns_cube_edge()`'s `y < y1` / interpolation rounding), not a
   structural problem; worth a closer look before this code is ever
   promoted beyond "proof of concept."

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

## Feature completion pass (this session)

Every milestone above was a seam proven in isolation, and milestone 7 got
the real game core booting to its real title screen -- but nothing past the
title had ever been verified, music was one hard-coded 20-second clip, sound
effects did not exist, and saves lived in RAM until power-off.  This pass
closed all of that.  What follows is what changed, and how each part was
actually confirmed rather than assumed.

### The build was broken: `.bss` did not fit under 0xC0000

`.bss` had grown to 333 KB and the link failed outright: "payload overruns
main RAM".  The fix was not to shrink anything but to stop putting `.bss`
there at all.  `.bss` is NOBITS -- nothing is read off CD for it, `head.S`
just zeroes it -- so it does not have to share the sub-0xC0000 window with
the loaded image.  `boot/mygame_shared.lds` now skips to link address
0xF0000, which with the 0x10000 load bias puts `.bss` at 0x100000: the start
of extended RAM, where it has the whole 0x100000-0x200000 megabyte of a
2 MB Marty to itself.  The gap costs nothing in the binary (objcopy drops
NOBITS) and nothing at relocation time (`reloc.c` applies one uniform bias).

That freed enough RAM to raise the card big-art cache from 2 resident slots
to 40 of the game's 72 cards (`WAIFU_ASSET_BIG_CACHE_SLOTS`), which also
matters for music -- see the CD-DA note below.

### The 512 KiB boot-sector ceiling (previously undocumented)

`Makefile.fmtowns` used to check the loaded image against 0xC0000, the FMR
VRAM window.  That is the wrong ceiling and it hides a much lower one:
`bootsect.S` relocates itself *and its stack* (SP = 0x3FF4) to INITSEG =
0x9000 and then loads the payload upward from 0x10000, so a payload reaching
0x90000 overwrites the loader while it is still running.  The machine then
boots to a black screen with no diagnostic whatsoever.

This was found the hard way: building the game core at `-O2` grew the image
by ~64 KB to 545 KB, it linked cleanly, and it would not boot.  The check
now enforces 0x90000 so this is a build error instead of a silent black
screen, and `GAME_OPT` stays at `-Os` with that reason written down.
Raising INITSEG to buy headroom was tried and does not work: both 0xA000 and
0xB000 fail to boot (the boot ROM appears to use that memory), so 512 KiB is
the real limit until someone reworks the loader.  Current headroom: ~45 KB.

### Headless input injection -- the gap that blocked everything

No previous session could press a button, so nothing past the title screen
had ever been verified.  Tsugaru_CUI cannot help here: its monitor's `TYPE`
command reaches the keyboard controller but not the game ports, `AUTOSHOT`
only drives a physical host gamepad, and `-GAMEPORT0 KEY` reads *host* key
state.  All three were tried and none moves the emulated pad.

So the payload presses its own buttons.  `FMTOWNS_DEBUG_INPUT=1` compiles a
frame-indexed button script (`src/platform/fmtowns/debug_input.txt`, turned
into a C table by `tools/fmtowns/gen_input_script.py`) that
`fmtowns_main.c` ORs into the real pad read every frame -- so the real
`fmt_pad_read()` path still runs underneath, and a human at a real pad is
never locked out.  Being indexed by game frame rather than wall clock, a
script replays identically however fast or slow the emulator runs.  It is
off by default; the shipping payload contains none of it.

Debug builds also stamp state into the top-left 58 pixels of every frame
(frame counter, current CD-DA track, playing flag, and three frame-time
averages), which `tools/fmtowns/read_frame_stamp.py` reads back out of a
screenshot.  That is what makes captures self-describing: scripts get
calibrated against real runs instead of guessed at, a screenshot can prove
which music track was playing when it was taken, and every capture doubles
as a profile.

The timings come from I/O 0x26, the machine's own free-running 1 us counter
(the same one `common/dacout.h` paces the DAC off).  That matters: it
measures the *emulated* machine, so a `-NOWAIT` run -- which emulates ~2.3x
faster than real time -- still reports the frame rate a real Marty would
show.  Every earlier frame-rate number on this port was wall-clock divided
by a frame count instead, and they were all wrong (see below).

`tools/fmtowns/headless_shot.sh` wraps the whole recipe (xvfb, `-MEMSIZE 2`
so tests match a real Marty, timeout guard, SLEEP-interleaved monitor
script).

**Verified with this**: title -> menu -> BATTLE MODE -> deck editor -> a
real duel, with the flat-shaded 3D board, CD-streamed card art, the HUD and
life points all rendering correctly; and separately the STORY MODE path
through name entry into the story deck editor.  This is the first time
anything past the title screen has been confirmed on this port.

### Performance: the present path was costing ~3x

Three real problems, all in the present path, none of them the game's code:

1. `fmtowns_video_present_8bpp()` flipped the page (which already waits for
   vblank) and then the game loop waited for vblank *again*, capping the
   game at half the display rate no matter how fast it rendered.
2. The palette -- 256 entries, four I/O port writes each, ~1000 port
   accesses -- was re-uploaded every single frame, including the vast
   majority where it had not changed.  It is now compared first and only
   sent when it differs.
3. `fmt_put_image()` wrote VRAM one byte at a time, recomputing the
   single-page address swizzle per byte: 61440 stores per frame.  The
   swizzle is not as scattered as it looks -- writing off = 8k + j gives
   `trans = ((j&4) << 16) | 4k | (j&3)`, so each aligned group of 8 source
   bytes lands as two contiguous 4-byte runs, one in each of two banks.
   The full-width case is now two dword stores per 8 bytes with no
   per-byte address arithmetic at all.

Measured on the menu screen: ~5.9 -> ~19.8 frames per second.

### Performance: the duel, actually measured

The "~6.6 fps in a duel" that stood here for a while was a wall-clock
estimate: emulator seconds divided by a frame count.  The in-machine
microsecond timer says otherwise -- the duel was already running at the
display cap in the hand view.  What was true is that the game *step* was
expensive, and that is what got fixed.

**How to measure this, because getting it wrong is easy.**  Two builds never
reach the same scene at the same second: CD load times are wall-clock, so a
faster build arrives everywhere earlier and the two runs desynchronise within
seconds.  Comparing a screenshot from each therefore compares two different
scenes and means nothing -- an early attempt at this "showed" a 3x regression
that turned out to be one run holding four cards and the other three.  Use a
script that walks to a scene and then **stops** (`90 START / 300 DOWN /
400 A`, no further input, parks in the duel's hand view), so both builds
render the same thing forever.  Everything below is measured that way.

> **Every number in this subsection is wrong.**  They were taken with the
> 1 us counter at I/O 0x26, which wraps every 65.536 ms -- inside the range
> this port's frames actually occupied, so a 70 ms frame reported as 4.5 ms
> and a 116 ms one as 50.7.  The conclusions built on them (that `rep stosl`
> memset was an 11x loss, that the hand view's board render was cheap, that
> the duel "was already running at the display cap") are all artefacts.  The
> section is kept because the *method* it describes -- park the scene, compare
> like with like -- is right, and because the failure is worth recognising.
> See "Frame pacing and the 3D rasterizer" below for the real figures.

| build                                        | game step per frame |
| -------------------------------------------- | ------------------- |
| baseline (byte-loop `memcpy`, no caches)      | 4.5 ms  (aliased)   |
| `rep movsl` memcpy                            | 1.7 ms  (aliased)   |
| ... plus `rep stosl` memset                   | 47 ms   (aliased)   |
| shipped (memcpy only + top-view composite)    | 1.8 ms  (aliased)   |

1. **`memcpy` was a C byte loop** (`waifu_fmtowns_runtime.c`).  The game
   core's `copy_u8_fast()` has hand-written backends for PC-FX and CD32X and
   falls through to plain libc everywhere else, so on this target every card,
   panel and glyph blit was a per-byte load/store/increment/branch.  It is
   now `rep movsl` with the destination aligned first, and blocks under 32
   bytes still take the byte loop because three `rep` instructions cannot be
   repaid by a handful of bytes.  This one is real, though the two numbers
   attached to it are not.
2. **`memset` now gets the same treatment.**  It did not, for a long time, on
   the strength of the aliased "11x loss" above -- and the comment above
   `memset()` explaining why the in-payload microbenchmark disagreed should
   have been read as evidence against the measurement.  Re-measured on the
   wrap-proof clock, `rep stosl` takes the uncached hand view's step from
   50.4 ms to 30.2 ms.
3. **The battle-composite cache is on** (`WAIFU_BATTLE_BASE_CACHE_DISABLE`
   dropped from `Makefile.fmtowns`; the `WAIFU_FM_FMTOWNS` branch in
   `battle_base_cache_for_camera()` picks the cameras).  It covered only the
   top-down view at the time, on the aliased reasoning that the hand view's
   board render was cheaper than the restore copy; it now covers both resting
   views in two slots.
   `WAIFU_BG_CACHE_DISABLE` and `WAIFU_FLOOR_SAMPLE_CACHE_DISABLE` stay on
   for the same kind of reason (the second is 256 KiB per tile size and
   simply does not fit).

**The frame rate is paced in software, and it has to be.**  The present path
ends in `fmt_flip_page()`, which waits on a vsync edge, and that was the only
thing holding the game to a sane speed.  Once the step and the VRAM blit got
cheap, that wait started returning in ~3 ms instead of a display field and
the whole game ran at **325 fps** -- every animation five times too fast.
`fmtowns_frame_pace()` in `fmtowns_main.c` holds the loop to 60 Hz off a
free-running hardware counter, which is a property of the game rather than of
whatever the CRTC registers make Tsugaru do.  (It measured that counter with
the wrapping 1 us clock at first, which is the subject of the section below.)

**What is not established.**  None of this has run on a physical Marty.  The
`present` figure in the stamp is dominated by the vsync wait and is not
stable run to run; `step` is, and is what the tables report.

### Music: the whole soundtrack, switching per scene

The disc now carries all eight tracks (`MUSIC_STEMS` in Makefile.fmtowns),
full length by default -- a ~190 MB image.  `MUSIC_SECS=20` trims every
track for fast iteration, and that setting is now part of the dependency via
a stamp file, because without it switching back to full length silently
reused the trimmed clips and shipped truncated music.

Track numbers are generated from `MUSIC_STEMS`'s order into
`fmtowns_cdda_tracks.h`, so the disc layout and the code that plays it
cannot drift apart.  `fmtowns_audio.c` is a per-frame reconciliation step
rather than a play() call, because on this hardware music dying is normal
traffic: the drive has one head, so every card-art read stops CD-DA
(cdda.h).  `waifu_fmtowns_cdrom.c` reports its reads, and the next update
restarts the track.  Keeping reads rare is what keeps that inaudible, which
is the other reason the 40-slot art cache matters.

**Verified**: the debug stamp shows CD track 2 (title music) on the title
and menu, track 4 (Battle) once a duel starts, and track 3 (Overworld) on
the story path -- exactly the intended mapping, read straight out of
screenshots.

### Sound effects: RF5C68 wave-table PCM

Effects go through the FM TOWNS PCM chip, which plays from its own 64 KiB
wave RAM with no CPU involvement -- the only affordable option here, and a
completely separate output path from the CD audio line, so effects and music
genuinely coexist.  `tools/fmtowns/gen_sfx_pcm.py` packs the same
`sounds/*.wav` the other ports use into RF5C68 sign-magnitude samples
(0x80 is silence, 0x00 and 0xFF are reserved, so magnitudes are clamped to
1..126), each resampled to the highest rate at which it still fits a 7935-byte
channel slot and then loudness-normalised.  They are linked into the payload
rather than staged on the CD, because reading them off disc would stop the
music every time the game beeped.  `fmtowns_sfx.c` voices them round-robin
over 6 channels and skips re-uploading a sample that is already in the target
slot; `sounds.c`'s `waifu_sound_play()` reaches it the same way it reaches the
PC-FX and CD32X players.  `common/sound.c`'s wave-RAM upload was also fixed
to select the 4 KiB bank once per bank instead of once per byte.

**Normalisation is not optional here.**  Without it the two longest effects,
CardDestroyed and TurnPassed, were inaudible in play, and the reason is worth
recording because it looked like a hardware bug and was not one.  The source
WAVs are mixed at very different levels (SlashAttack's RMS is 73 of a possible
127; TurnPassed's is 9.7, CardPlaced's 0.9), and any effect too long to fit a
slot at 8 kHz is resampled down until it does -- so the longest effects take
the widest box filter and lose the most amplitude to it.  TurnPassed reached
the chip at an encoded peak of 20 against SlashAttack's 125.  A wave-RAM
read-back audit confirmed every uploaded byte, both bank crossings and the end
markers, for all ten effects: the upload and the chip programming were always
correct, the data was just too quiet to hear over CD-DA.  Because these are
percussive sounds with high crest factors, peak normalisation buys almost
nothing (under 4 dB for CardDestroyed); the generator normalises against the
99.5th-percentile magnitude instead, clipping above it, capped at 12x so a
near-silent source is not amplified into its own noise floor.

**Verified three ways**: with CD-DA muted, Tsugaru's FM/PCM recording contains
discrete effect bursts exactly where the script pressed buttons; a wave-RAM
dump matches the generated sample bytes exactly, in distinct channel slots;
and a probe that plays all ten effects three seconds apart records nine bursts
(YOU_LOST has no sample) at the right durations and all at full scale --
TurnPassed measured +18.4 dB against its pre-normalisation capture.

### Saves: battery-backed CMOS, not session-only RAM

Story saves now persist for real.  A Marty has no Backup RAM chip, but every
FM TOWNS has 8 KiB of battery-backed CMOS, and unlike an IC memory card it
is not optional hardware the player might not own.

Two things had to be right.  The CMOS is invisible until bit 0 of the memory
switch register at I/O 0x0480 maps the dictionary/learning RAM in -- without
it, writes to 0xD8000 land nowhere, which is exactly what the first attempt
did.  And the BIOS owns the low part of the CMOS (a stock Marty dump has
data scattered up to ~0x10B0), so the game's block sits in the last 256
bytes, where it cannot touch the user's machine settings.  The block carries
magic, length and checksum, and is written body-first so an interrupted
write reads back as "no save" rather than as a valid header over garbage.

**Verified across a cold power cycle**: first boot found no save and wrote
one; a second boot of the same CMOS image read it back byte-identical; and
every byte of the CMOS below the game's block was unchanged from the stock
BIOS contents.

## Frame pacing and the 3D rasterizer

### The clock was lying, and everything downstream of it was wrong

The machine's free-running 1 us counter at I/O 0x26 is 16 bits, so it wraps
every 65.536 ms.  This port's frames were longer than that.  A 134 ms frame
read back as 3 ms, an 80 ms one as 14.5, and nothing in the payload could
tell the difference: there is one clock and it is sampled once at each end of
a section, with the CPU inside `waifu_fm_step()` for the whole interval
between.

That single fact produced, at various times: a "the duel already runs at the
display cap" conclusion about a 7.5 fps scene; an 11x regression attributed to
`rep stosl`; a cache policy built on the hand view's board render costing
4 ms when it cost over 100; and a frame-pacing fix that could not work,
because it asked the same clock how long the frame took and was told "about
one 60 Hz period" every time.

**Everything now times off PIT channel 1** (`fmtowns_clock_ticks()` in
fmtowns_main.c): 307,200 Hz, free-running, wrapping every 213.3 ms.
307200/60 is exactly 5120, so a 60 Hz frame is a whole number of ticks and
the pacing divides nothing on a 386SX.  Only a frame over 213 ms aliases now,
and in practice that means a CD read, which the pacing refuses to charge the
game for anyway.

If you take one thing from this file: **a measurement that contradicts a
microbenchmark of its own code is a reason to distrust the measurement.**

### Where the duel actually went

Parked duel scene, measured with `./fmtowns.sh profile`, before and after:

| scene                                   | before   | after   |
| --------------------------------------- | -------- | ------- |
| hand view, composite cache on           | 128 ms   | 7.6 ms  |
| top-down board, composite cache on      | 10.2 ms  | 7.2 ms  |
| hand view, cache off (= a moving camera)| 129.3 ms | 30.2 ms |

Both resting views hold 60 fps; a moving camera runs at 30.  Three changes,
in order of size:

1. **The board was never on the compact affine rasterizer.**
   `WAIFU_BOARD_FAST_AFFINE_ENABLE` listed PC-FX and CD32X and not FM TOWNS,
   so the Marty took the generic desktop path: all four corners of all 64
   cells reprojected independently, filled with per-pixel barycentric
   texture math.  A whole optimisation pass aimed at the span fillers' inner
   loops moved almost nothing, which was the clue -- stubbing every textured
   span out of the frame took it from 129.3 ms to 122.1 ms.  On the shared
   pre-projected path the same frame's step is 50.4 ms, pixel-identical.
2. **`rep stosl` memset**: 50.4 -> 30.2 ms.  See above.
3. **The composite cache now covers both resting views**, in two slots, not
   just the top-down one.  The hand camera looks *along* the board, so its
   near rows are magnified across most of the screen: it is the more
   expensive view to rasterize, not the cheaper one.  Two slots rather than
   one because UP/DOWN toggles between exactly these two views, and sharing a
   slot would make every toggle a re-render.

What is left in a 30.2 ms uncached frame: board 20.6 ms (12.8 setup,
7.8 fill), everything else 9.6 ms.  The setup is the per-scanline edge walk
and its two reciprocal-LUT divides; that is the next thing to attack if 30
is not good enough, and `CFX_MEASURE_SKIP_SPANS` /
`WAIFU_MEASURE_SKIP_BOARD` are the knobs that split it.

### Fades were dithering every pixel

`apply_black_dither_fade()` walks all 61440 pixels with a Bayer compare and a
conditional store.  PC-FX and CD32X opt out and fade the palette instead;
FM TOWNS did not, so every frame of every transition paid a full-screen walk
on top of re-rendering the scene under it, and a transition crawled.  It now
fades the palette in `fmtowns_video_present_8bpp()`, which had to compare and
upload that palette anyway.

### Game speed is untied from the render rate

`waifu_fm_step()` advances the game by however many 60 Hz frames the platform
reports.  `fmtowns_frame_pace()` returns the real count off the PIT, carrying
leftover ticks so a steady 25 ms frame alternates 1,2,1,2 instead of losing a
third of the game's clock, and capping the count so a stall cannot bank debt
and then fast-forward.  `frame_logic_step()` in main.c multiplies the phase
and UI counters by it; six cues that fired on an exact frame number became
`frame_cue_crossed()` tests, since a step above 1 steps over `==`.

Note the core clamps the count at 4 (`waifu_fm_set_frame_vblanks()`), so a
frame slower than ~66 ms still runs in slow motion by construction.  That is
a deliberate anti-teleport limit, and the answer to it is to not have frames
that slow.

### Reading a capture

`tools/fmtowns/read_frame_stamp.py` reports, per screenshot: the game frame,
the CD-DA track, the frame time split into step and present, **the 60 Hz
period count the loop charged the core**, the **worst whole frame in the last
256**, and the **fade level**.  The last three exist because of specific
failures: a slow-motion complaint is now diagnosable from one screenshot (a
60 ms frame stamped "paced 1" means the clock is lying again); an average
over 16 frames hides the one camera sweep that stutters; and a fade is
otherwise completely invisible to a headless run.

Two harness traps, both of which produced wrong answers before they were
fixed:

- **Scripts are indexed by game frames, not rendered ones.**  A script
  written against a 60 fps build parks in one phase at 30 fps, because every
  tap lands twice as late in game time.  One capture sat re-selecting the
  same square for 10,000 frames while reporting itself as a duel
  walkthrough.
- **`INPUT_SCRIPT` is part of the flags stamp.**  The generated header's only
  other prerequisite is the script file's timestamp, so switching back from
  the parked profiling script left make with nothing to do and
  `fmtowns.sh test` silently re-measured the parked scene.

Also: `Makefile.fmtowns` had no header dependency tracking until recently
(`-MMD` now).  The first capture of a rewritten hot loop measured the old
object and reported "no change".

## What is left

- **Moving cameras cost 30 ms.**  The resting duel views are cached and hold
  60 fps; anything that moves the camera (the hand<->top lift, card flights,
  attack sweeps) renders live at 30.  Of that frame, 12.8 ms is the affine
  walker's per-scanline setup -- four edge tests and two reciprocal-LUT
  divides per row, per quad.  Specialising that loop for the i386, or
  quantising the lift onto cached anchors the way PC-FX does (60 KiB a slot,
  ~117 KiB of `.bss` spare), are the two obvious moves.
- **The present path is 5-11 ms** and its variance is a vsync wait, so a
  frame that finishes a millisecond late costs a whole extra field.  Whether
  the blit itself can be cut (or the flip's wait dropped, leaving pacing
  entirely to `fmtowns_frame_pace()`) is unmeasured; the latter risks tearing
  on a real Marty and nobody has one.
- **Heavy scenes still are not profiled.**  The captures park in the duel.
  The card-vs-card battle screen with its two large art panels, story
  dialogue, and the sanctum map are all unmeasured -- park the debug script
  on one and read the stamp.  The worst frames a full scripted run catches
  (80-230 ms) are CD reads, not renders.
- **A duel played to completion** (and the story path through to the sanctum
  save screen) has not been driven end to end.  Getting *into* a duel is no
  longer the obstacle -- `debug_input.txt` now assembles the 40-card deck
  (the default is 34/40 with 6 in storage, and START silently refuses a
  short deck) and plays several turns.  Finishing one needs a script that
  picks legal targets rather than tapping A and a direction, since an
  invalid choice just leaves the phase where it was.
- **Real hardware.**  Everything here is Tsugaru at `-MEMSIZE 2`.  Nothing
  has run on a physical Marty, and no one has heard the audio -- the CD-DA
  evidence is playback state read back through the drive, and the SFX
  evidence is the emulator's own PCM recording.

## CDC command ordering and MAME compatibility

The polled MODE1 reader queues all eight bytes in the CDC parameter FIFO and
then writes the command register.  This is the hardware sequence used by the
TBIOS-compatible `FM/TOWNS/EXPERIMENTS/CDREAD/CDREAD.ASM` reference
(`CDC_PUSH_PARAMS`, wait-ready, then `CDC_SHOOT_MODE1READ`).  The command write
fires the already-parameterized request; it is not the first byte of a
nine-byte packet.

The previous command-first sequence happened to work in TOWNSEMU, whose state
machine waits until it has seen both a command and eight parameters.  MAME
executes MODE1READ immediately on the command-register write, so it consumed
the stale parameter FIFO and the game remained at `LOADING... TITLE 0%`.
Parameter-first ordering is both hardware-correct and accepted by both
emulators.  It is used by the blocking asset reader and by the nonblocking PCM
stream command issuer; leaving the stream path command-first would reintroduce
the same stale-LBA bug when streamed music starts.

Verified from the same `build/fmtowns/output.cue` on 2026-08-11: MAME's
`fmtowns -cdrom` driver and TOWNSEMU/Tsugaru `-TOWNSTYPE MARTY` both load past
the progress screen and render the title image.

## The hardware-only CD hang: the A0 setup command

A build that loaded fine in both emulators still hung on a real FM TOWNS Marty
at `LOADING... TITLE 0%`, on the very first asset read.  The cause is a
command the driver never sent.

`FM/TOWNS/EXPERIMENTS/CDREAD/CDREAD.ASM` -- CaptainYS's own hardware
experiment, and the closest thing to a specification that exists, since the
Technical Databook declines to publish the CDC command set at all ("please use
the CD-ROM BIOS") -- issues an undocumented command `A0H` with the fixed
parameters `08 01 00 00 00 00 00 00` immediately before its `MODE1READ`, with
the comment "TBIOS uses this command before reading sectors.  Effect is
undocumented and unknown".  Tracing the Marty boot ROM's own CDC traffic shows
36 of them, each one paired with a read.  No driver known to work on hardware
skips it.  A bare `MODE1READ` is *accepted* -- the sub-MPU takes the command
and posts status `00H` -- and then simply never hands over a sector, which is
what a driver polling for `22H` sees as a permanent freeze on the frame it drew
before the read.  That is exactly the hardware symptom.

`fmt_cdc_setup_read()` sends it, and both readers in `cdrom.c` call it before
every read command (the drive requires it per read, not once per session).
The two other handshake rules the same reference implies are also honoured
now: `fmt_cdc_issue()` waits for DRY (`4C0H` bit 0, "sub-MPU can accept a
command") before loading the parameter FIFO and again before the command
register, since a byte written while DRY is clear is dropped, and it spaces
the parameter writes with the 1us wait register (I/O `6CH`) the way the BIOS
does.  DRY *before* a command is the only correct use of that flag; it stays
clear for the whole duration of a read, so waiting on it afterwards can only
be satisfied by the drive's lost-data timeout (see the comment block at the
top of `cdrom.c`).

This cannot be confirmed without a Marty, and it is a deduction, not a
measurement.  Two other candidates that looked at least as strong were killed
by evidence: a >=1us gap between parameter bytes (the boot ROM writes them
500ns apart) and keeping SMIM set when acknowledging (the boot ROM acks with
it clear).  Tsugaru's `-CDCSTRICT` option models the A0 and DRY rules so the
failure can be reproduced without hardware; on the pre-fix build it froze at
the same `LOADING...` screen the Marty photo shows.

Verified 2026-08-22 from `build/fmtowns/output.cue`:

- Tsugaru `-TOWNSTYPE MARTY -CDCSTRICT`: boots to the title, and a scripted
  run through the deck editor into a duel reports zero `[CDCSTRICT]`
  violations.
- Tsugaru without the flag, and MAME `fmtownsux -cdrom`: unchanged, both still
  reach the title.  The A0 command is a no-op in both.

## CD diagnostics: what the next hardware hang will report

The A0 fix above is a deduction, not a measurement, so this port has to
assume it can still hang on a real drive.  What made the first hang
undebuggable was not the hang: it was that the frozen screen said
`LOADING...` / `TITLE 0%` and nothing else.  No file, no sector, no attempt
count, no CDC register.  `src/platform/fmtowns/fmtowns_cd_diag.[ch]` exists
so that never costs another round trip through a photograph.

It is game code, not libfmt.  libfmt's `cdrom.c` stays a driver that returns
0 or -1; all the policy -- retry, report, prompt -- lives here.

**Boot self-test.**  `fmtowns_cd_diag_selftest()` runs before the game core
and walks the CD path one labelled step at a time: the CDC status port, the
A0 setup command alone, a read of sector 16, the `CD001` signature in what
came back, the directory lookup for `TITLE.BIN`, and a read at its real LBA.
Each step's label is painted **before** the step runs, so a step that never
returns leaves its own name on screen as the diagnosis.  It prints the raw
`4C0` value and the file's LBA/size either way, holds for ~1.5s on success,
and on failure holds a report with a visible countdown and re-runs itself
when it expires (A starts the game anyway).  An unattended machine keeps
trying rather than sitting dead.

**Breadcrumbs.**  Before every read attempt during the asset load, a line
like `RD 0003B2 N1E T1 TITLE.BIN` (LBA, sector count, attempt, file) is
stamped into the bottom row of VRAM.  Two details matter: it goes into
*both* pages, because a hang stops the page flip and the draw page may not
be the one on screen; and it is written straight to VRAM rather than to the
game's framebuffer, because a framebuffer only reaches the glass when
something presents it, and the case this exists for is the case where
nothing does.  Breadcrumbs switch off once `waifu_assets_ready()` and wipe
their line on the way out, so they cost nothing in a running duel.

**Retry and report.**  `fmtowns_cd_diag_read()` retries a failed read
`DIAG_READ_TRIES` (3) times, draining the CDC status FIFO between attempts
so the next command does not match on the previous one's wreckage.  If all
three fail, `fmtowns_cd_diag_report_failure()` paints the file, the sector,
the count, the attempts, the sampled `4C0`, a plain-language reading of it
("NO DRIVE RESPONDING" / "DRIVE READY, READ REFUSED" / "DRIVE BUSY OR
STALLED"), and the running counters, then offers RUN to retry or A to
continue without the data.  The counters (`READS`/`SECTORS`/`RETRY`/`FAILED`)
are cumulative, so an intermittent drive shows up as a non-zero retry count
on a self-test that otherwise passed.

Note what bounds a hang today: the driver's poll limits are spin counts, not
wall-clock timeouts (`CD_POLL_LIMIT`), so a drive that stops answering costs
on the order of ten seconds per attempt and about a minute before the report
appears.  That is the intended trade -- slow and legible beats fast and
wrong -- but it is why the retry count is 3 and not 10.

**Verifying the failure path.**  No emulator will produce the fault this is
for, so building with `FMTOWNS_CD_FORCE_FAIL=n` makes the first *n* reads
fail without asking the drive.  `n=1` exercises the report screen, the
countdown, and the recovery.  Verified 2026-08-22 under Tsugaru `-CDCSTRICT`:
the self-test reports `READ SECTOR 16 FAIL` / `VOLUME DESCRIPTOR FAIL`,
`RETRY 3 FAILED 1`, holds the prompt with a live countdown, then re-runs
itself and boots to the title.  With the hook off, the self-test reports six
PASSes and `REG 4C0 = 41 READY`, the breadcrumb was captured mid-load, and a
scripted run into a duel is unchanged (zero `[CDCSTRICT]` violations, no
breadcrumb residue on the board).  MAME `fmtownsux -cdrom` unaffected.

## Single-page 256-colour mode uses *both* CRTC register sets

Page flipping wrote `FA0` only.  On hardware that produced a stripe pattern
over the whole screen with a 4-pixel period, half of it the cream of palette
index 0 -- the never-drawn page showing through.

Single-page mode is not "one layer driven by one register set".  The CRTC
still fetches the picture from both VRAM banks, alternating every 16 bits, and
each bank is addressed by its own set: bank 0 by `FA0`/`HAJ0`/`FO0`/`LO0`,
bank 1 by `FA1`/`HAJ1`/`FO1`/`LO1`.  With `FA0` flipped to the second page and
`FA1` left at 0, every other pair of 8bpp pixels came from page 0, which at
that moment still held the `fmt_set_mode()` clear.

`fmt_flip_page_poll()` now writes the page address to both, and
`fmt_set_mode()` zeroes both.  Emulators that model only one register set in
this mode show nothing wrong either way -- Tsugaru did not until it was taught
the 16-bit cadence.  Every other CRTC register in the mode tables already
carries identical values in both sets, so `FA` was the only one out of step.

## Runtime Marty / standard-model detection

This port was Marty-only until now: everything above was built and verified
solely against `-TOWNSTYPE MARTY`. This section adds runtime detection so
the *same built image* also boots and renders correctly on a standard
(non-Marty) FM TOWNS, without a build-time flag or a second ISO.

### Why the white screen happened

Booting the existing Marty-only image against a plain FM TOWNS ROM set
(`FMT/ROMS/FMT_{DIC,DOS,FNT,SYS}.ROM`, non-Marty) produced a permanent
white screen. Two things were confirmed by reading TOWNSEMU's own source
(`FMTOWNSCD_EXAMPLE_Cube/TOWNSEMU/src/`):

1. `-TOWNSTYPE FMTOWNS` (tried first) is not a valid Tsugaru model string --
   see `StrToTownsType()` in `towns/townsdef/townsdef.cpp`. It parses to
   `TOWNSTYPE_UNKNOWN`, which skips memory-map setup entirely. Minor, not
   the root cause: a *valid* string (`-TOWNSTYPE MX`) reproduced the same
   white screen.
2. **The real cause**: `FMT_VRAM0_BASE` (`common/fmt_pixel.h`,
   `common/libfmt.h`) was the compile-time constant `0xA00000`, which is
   the VRAM window *only* on 80386SX-class machines (Marty, UX -- 24-bit
   physical address space, Fujitsu gave them a completely different
   physical memory map from every 386DX/486/Pentium-class model). See
   `FMTOWNSCD_EXAMPLE_Cube/docs/HOWFMTOWNS_BOOTS_FROM_CD.txt` and
   `TOWNSEMU/src/towns/memory/physmem.cpp`'s `SetUpMemoryAccess()`, which
   branches VRAM/ROM/CMOS addresses on `cpuType==TOWNSCPU_80386SX`: one
   `TOWNSADDR_386SX_*` constant set for Marty/UX, a completely different
   `TOWNSADDR_*` set (VRAM0 at `0x80000000`, not `0xA00000`) for everyone
   else. On a standard model, `0xA00000` is just ordinary RAM -- every pixel
   the game wrote vanished into unused memory, the CRTC never got fed real
   data, and the display sat at its power-on-default (white).

### The detection mechanism: I/O port 0x30, the machine ID register

Real FM TOWNS software distinguishes the two memory maps off a genuine
hardware register: I/O port `0x30` (low byte) / `0x31` (high byte),
`TOWNSIO_MACHINE_ID_LOW`/`_HIGH` in TOWNSEMU's `townsdef.h`. Reading
`FMTownsCommon::MachineID()` in `towns/towns.cpp` (the function that
supplies these two bytes) shows the low byte is a small CPU-class field for
genuine (non-legacy-FMR) models: `0`=80286-class (never a real FM TOWNS),
`1`=80386(DX)-class, `2`=80486/Pentium-class, `3`=80386SX-class -- and
`TOWNSTYPE_2_UX` and `TOWNSTYPE_MARTY` are the *only* two cases in that
switch that produce `3`. That is exactly the narrow/wide memory-map split
this port needs, straight off one I/O read, no per-model table to keep in
sync.

(Real `TBIOS.SYS` is documented -- per TOWNSEMU's author, from disassembly,
confirmed by a third party in 2024 -- to test the *high* byte's `0x40` bit
to detect Marty specifically (`highByte=0x4A` for Marty vs. `0x1`-`0x11`
for everything else including UX's `0x3`). That identifies Marty alone, not
the shared 386SX narrow-map class UX also belongs to, so it is the wrong
test for this port's purposes even though it is the "real" BIOS-internal
Marty check. The low byte's CPU-class field is the one this port reads.)

Confirmed experimentally, not just read off the emulator source: booted the
same build under `-TOWNSTYPE MARTY`, `MX`, `CX` and `UX` and checked the
captured title screen renders correctly under all four (see "Verification"
below) -- MX/CX are wide-map (486DX/386-class) models, UX is narrow-map
like Marty, so this exercises the branch both ways.

### What changed

- **New `src/platform/fmtowns/common/machine.h`/`machine.c`.**
  `fmt_machine_detect()` reads I/O port `0x30` once and sets
  `g_fmt_vram0_base` to `0xA00000` (narrow, TOWNSADDR_386SX_VRAM0_BASE) or
  `0x80000000` (wide, TOWNSADDR_VRAM0_BASE) accordingly.
  `fmt_machine_is_narrow_map()` exposes the same result as a bool for any
  future call site that needs it (nothing but VRAM base needed it this
  pass -- see "What was checked and found not to need a branch" below).
  Defaults to the narrow map before detection runs, preserving this port's
  original Marty-only behaviour as the fallback.
- **`FMT_VRAM0_BASE` stopped being a compile-time constant.** It was defined
  in three places (`common/fmt_pixel.h`, `common/libfmt.h`, and
  `common/libfmt.c`'s now-removed `TOWNS_VRAM0_BASE_MARTY` alias) and read
  directly by `common/mbvplay.c` and (unused/uncompiled)
  `common/fmt_layers.c`. All five now read `g_fmt_vram0_base` instead.
- **`fmtowns_main.c`'s `start_main()` calls `fmt_machine_detect()` as its
  first line**, before `fmt_media_init()` or anything that could touch
  VRAM (`fmt_set_mode()`, any present call).
- **The same startup call now explicitly enables FAST mode** through I/O
  `0x5EC` (bit 0).  Firmware can leave a standard-model boot with six
  main-RAM and VRAM wait states even when Marty happened to boot fast.  The
  bare-metal payload must not inherit that BIOS choice: it selects the
  documented fast bus setting before the title/game loop, on both memory-map
  classes.  This is independent of (and does not alter) the VRAM-base branch.
- **`Makefile.fmtowns`** builds `$(COMMONDIR)/machine.o` into the payload.

### What was checked and found not to need a branch

The physical VRAM *base* differs, but the byte-swizzle math on top of it
does not: `fmt_vram_singlepage_offset()` (`fmt_pixel.h`) mirrors TOWNSEMU's
`VRAM1Trans::SinglePageOffsetToLinearOffset()`
(`towns/render/render.h`), which is defined purely in terms of the offset
*within* the window, with no dependency on which base the window is
mapped at -- and both windows are the same size (`0x80000`, 512 KiB), so
no size-driven behaviour differs either. CRTC/palette I/O ports are
ordinary port-mapped I/O, identical across every model -- only the
*memory-mapped* windows (VRAM, ROM, CMOS) move. This game's CMOS save path
(`waifu_fmtowns_platform.c`) already goes through the low-1MB
FMR-compatible bank-switched window at `0xD8000` (gated by I/O `0x0480` bit
0), which is the legacy access path common to all models -- not the
"native CMOS RAM" window that *does* move between narrow/wide maps
(`0xF40000` vs `0xC2140000`) but that this port never used. So the save
path needed no change.

Also checked and not a concern: the 32-bit flat data segment `head.S`'s GDT
sets up (`0x00cf92000000ffff` -- base 0, 4 GiB limit, granularity bit set)
already covers all the way to `0x80000000`; no segment/paging change was
needed to reach the wide map's VRAM window.

CD-ROM access (`common/cdrom.c`) was flagged as a possible concern (`towns/
scsi/scsi.cpp` notes "Marty did not have a SCSI I/F") but did not need
changes: the boot ROM's `call far 0xFFFB:0x14` low-level sector-read
routine that this bare-metal payload's `cdrom.c` uses is model-independent
per `HOWFMTOWNS_BOOTS_FROM_CD.txt` -- the SCSI-vs-Marty-CDC difference only
matters to OS-level access, not raw IPL-time sector reads. Confirmed
indirectly: the non-Marty captures below show the title art and text that
only exist after a successful CD read (title/palette assets, the "PRESS RUN
TO START" prompt), so the read path worked without modification.

### Verification

Built once with `make -f Makefile.fmtowns` (no `-TOWNSTYPE`-specific build
flags exist or were added -- this is genuinely one binary). Confirmed no
regression to the boot-sector ceiling check (still 45480 bytes spare,
unchanged from before this change) and no regression on Marty:
`tools/fmtowns/headless_shot.sh` (which hardcodes `-TOWNSTYPE MARTY`)
captured the correct title screen, matching the pre-change baseline exactly
(same checksum-equivalent capture).

Then booted the *identical* `build/fmtowns/output.cue` against the
non-Marty `FMT/ROMS/` ROM set and `FMT/Tsugaru_CUI.elf`, under three valid
non-Marty `-TOWNSTYPE` values:

```sh
{ sleep 10; printf 'SS /tmp/shot.png\nQUIT\n'; } | xvfb-run -a timeout -s KILL 40 \
  ./FMT/Tsugaru_CUI.elf "FMT/ROMS" -TOWNSTYPE MX -MEMSIZE 2 -CD build/fmtowns/output.cue \
  -NORMALFD -DONTUSEFPU -NOWAITBOOT -GAMEPORT0 KEY -KEYBOARD DIRECT
```

- `MX` (wide map, 486DX-class): title screen renders correctly, **pixel-
  identical** (same PNG bytes) to the Marty capture at the same boot
  timing.
- `CX` (wide map, 386-class): title screen renders correctly (captured a
  frame with the blinking "PRESS RUN TO START" prompt on-screen -- a
  benign UI-animation timing difference from the other captures, not a
  rendering defect).
- `UX` (narrow map, like Marty): title screen renders correctly, pixel-
  identical to the `CX` capture at the same boot timing.

All four captures (`MARTY`, `MX`, `CX`, `UX`) show the correct art,
correct palette, and correct text -- proving both branches of
`fmt_machine_detect()` (narrow map on Marty/UX, wide map on MX/CX) present
real production pixels through the same code path. This clears the
session's minimum bar (title-screen parity on both machines from one
binary).

### What is left

- Only the title screen has been verified on non-Marty. Milestones 3-7
  (CD-DA, input, the real game core, a full duel) were all verified on
  Marty only; nothing about `fmt_machine_detect()` should affect them
  differently, since they all route VRAM writes through the same
  `g_fmt_vram0_base`-based helpers already exercised here, but none of them
  has actually been captured running past the title on a non-Marty model.
- **Non-Marty performance remains a verification scope, not a separate
  renderer/code-path project.** The startup FAST-mode write removes the
  known BIOS-inherited wait-state discrepancy that appears exactly when the
  live title loop begins.  The normal FM TOWNS target still needs a full
  title-to-duel capture and frame-stamp comparison against Marty before it
  can make a parity claim; no model-specific render duplication or second
  binary is warranted unless that controlled check finds a remaining gap.
- **`fmt_machine_is_narrow_map()` has no caller yet** beyond
  `g_fmt_vram0_base` internally choosing between the two constants -- it is
  exposed for any future model-specific behaviour (e.g. if a
  performance or ROM/CMOS-window difference surfaces once milestones
  past the title are verified on non-Marty) but nothing needed it this
  pass.
- Real hardware, as above: nothing here has run on a physical machine of
  either class.

## Marty speed pass (2026-08-26)

A phone capture of a real machine (`IMG_6472.mov`) showed the game running
in single digits in a duel and the *story text* crawling, while Tsugaru
reported a locked 60 fps.  This section is why the two disagreed, what was
done about it, and what is left.

### The emulator was never a speed reference, and now can be

Tsugaru's CPU core is an i486 with a single global MHz number applied to
486 instruction clock counts.  There is no cache model, no DRAM wait state
and no VRAM penalty: `mainRAMWait`/`VRAMWait` (I/O `5E0H`/`5E2H`/`5E6H`)
existed only to flip `state.currentFreq` between two values, and both
default to zero.  So **every profile this port has ever taken -- including
all the millisecond figures further up this file -- describes a 25 MHz 486
with perfect memory**, not a 16 MHz 386SX with no cache behind a 16-bit
bus.  That is the whole 3-6x gap.

`FMTOWNSCD_EXAMPLE_Cube/TOWNSEMU` now models it, off four new switches:

| switch | what it does |
| --- | --- |
| `-CPUCLOCKSCALE percent` | scales the core's per-instruction clock count (386 microcode + no instruction cache) |
| `-BUSWAIT clocks` | clocks charged per data bus cycle to main RAM |
| `-VRAMBUSWAIT clocks` | ditto for VRAM, sprite RAM and the FMR window |
| `-DATABUSWIDTH 16\|32` | a dword access costs two bus cycles on 16 |

`Memory` (`src/ramrom/ramrom.h`) keeps a per-4KB-slot wait table and an
accumulator that `FMTownsCommon::RunOneInstruction()` charges alongside the
core's own clocks.  Two details matter if you touch it:

* Instruction fetch deliberately does **not** go through the counter -- it
  uses the const memory window, and prefetch overlaps execution on real
  hardware.  The cost of running 386 code is `-CPUCLOCKSCALE`'s job.
* `TownsMainRAMAccess::GetMemoryWindow()` refuses to hand out a direct
  pointer while the model is on (`refuseMemoryWindow`).  Without that, the
  CPU's operand-pointer fast path bypasses `Memory` entirely and a main-RAM
  wait state would only ever be charged to string instructions.

The game's own `5E0H`/`5E2H`/`5E6H` writes add to the CLI floor, so a
program's wait-state setup is finally visible in a profile.

`fmtowns.sh` passes `FMTOWNS_TIMING`, defaulting to
`-FREQ 16 -CPUCLOCKSCALE 220 -BUSWAIT 2 -VRAMBUSWAIT 6 -DATABUSWIDTH 16`.
**Those four numbers are a calibration, not a measurement.**  Nobody has
profiled the real machine.  They were chosen because they put a parked duel
frame in the range the phone capture shows, and they should be re-fitted the
moment someone photographs a real Marty's frame stamp
(`tools/fmtowns/read_frame_stamp.py` reads a photo as happily as a
screenshot).  Compare builds, not absolute numbers.  `FMTOWNS_TIMING=`
(empty) restores the old zero-wait behaviour exactly.

### Measurement fixes that had to come first

Three separate things were making before/after comparisons lie, and each
one produced at least one confidently wrong conclusion in this session
alone:

1. **`present` included the vsync wait.**  A faster blit just waited longer
   and measured the same.  `fmtowns_video_take_vblank_wait_us()` now hands
   the wait back to `fmtowns_main.c`, which charges it to `other`, so
   `present` means work.
2. **The walk-in script desynchronised the two builds.**  A script step
   fires on a *game* frame, but the CD reads between the title screen and
   the duel take wall time -- so a faster build has loaded *less* at any
   given frame number.  At the old `400 A` the `-Os` build sat in the duel
   and the `-O2` build sat on the menu for the entire capture, and `-O2`
   was written down as an 8% regression when it is a 20% win.
   `./fmtowns.sh profile` now builds `-DWAIFU_DEBUG_AUTODUEL` (or
   `-DWAIFU_DEBUG_AUTOSTORY`) and boots straight into the scene, pressing
   nothing.
3. **The deck was dealt from `time()`/`clock()`.**  Different cards on
   screen cost different amounts to blit, worth ~5 ms of scatter.
   `waifu_deck_runtime_seed()` drops its entropy in any `WAIFU_DEBUG_AUTO*`
   build.

`./fmtowns.sh profile [hand|board|story|name]` is now reproducible to the
tenth of a millisecond across runs.

### What changed in the game, and what each was worth

Parked scenes, 386SX model, `step` and `present` in ms:

| | hand: step | hand: present | fps |
| --- | --- | --- | --- |
| before | 70.1 | (not separable) | 10.0 |
| after | 47.0 | 15.9 | 14.9 |
| after the 2-D pass below | 38.1 | 15.9 | 14.9 |

1. **~190 KB of unreachable PCM stopped being linked in.**
   `src/game/sounds.c` gated its software mixer for PC-FX and CD32X but not
   FM TOWNS, and `--gc-sections` could not drop it because the payload's
   final link was `-shared -Bsymbolic`, which makes every global an export
   and therefore a GC root.  The bytes were physically in the shipped image.
2. **`-fPIC` and the self-relocation are gone.**  The payload is loaded at
   `0x10000` and nowhere else, so it is now a plain static link at that
   address (`boot/mygame_shared.lds`, `-DFMT_NO_RELOC`, `reloc.o` dropped).
   PIC was costing `%ebx` -- on a machine with eight registers, in a
   rasterizer written against all of them -- plus a GOT indirection per
   global.  Worth 70.1 -> 59.0 ms of step on its own, and ~58 KB of image.
3. **`-O2` everywhere** (game core *and* platform layer).  59.0 -> 47.4 ms
   of step; libfmt's present loop alone was ~4 ms/frame slower at `-Os`.
   This is what 1 and 2 bought: the image went from 41 KB of headroom under
   the 512 KiB boot ceiling to ~280 KB.
4. **Dirty-region present** (`fmt_put_image_dirty()`, libfmt.c).  Hash each
   64-byte group of the framebuffer, store only the groups whose hash
   changed, and dirty the union of the last *two* frames' changes because
   the page being written is two frames old.  23.0 -> 13.2 ms for the blit.
   The first version of this cost *more* than the blit it replaced; see the
   comment on `fmt_dirty_fold()` for why, it is the most transferable thing
   learned here.
5. **The story plaza dialogue cache**, previously PC-FX-only, now covers FM
   TOWNS (`plaza_dialogue_cache_*` in `src/main.c`).  Worth ~4 ms of step,
   less than expected, and it is the dirty present that it really unlocks:
   with the scene settled the story screen writes almost no VRAM at all.
   Story plaza went 12.1 -> 14.9 fps.
6. **Flat floor tiles fill instead of raycast** on FM TOWNS as well as
   CD32X (`floor_tile_flat()`), for the void sanctum's solid-colour floor.
7. **Wait-state registers are written at boot** (`common/machine.c`):
   `5E0H`, `5E2H` and `5E6H` as well as the `5ECH` FAST/SLOW switch that
   was already there.  `5ECH` is documented as 3rd-generation-and-later, so
   on a 2nd-generation 386SX it is very likely a no-op and the machine has
   been running with whatever wait profile the boot ROM left.  This is
   invisible in the emulator unless the model above is on, and it is the
   single largest *hardware-only* unknown left.

### The flat 2-D screens: it was `put_px`, and it is fixed

The open question above -- why a screen with no 3-D in it cost 60 ms of step
-- has an answer, and it was not the palette rebuild or the hardware-2D
hook (that hook compiles to `return 0` everywhere but SDL3).  It was the
per-texel cost of the shared drawing primitives.

`./fmtowns.sh profile name` was added to measure it: `WAIFU_DEBUG_AUTONAME`
boots straight onto the story name-entry screen, which is the flattest
screen in the game -- one `clear_screen`, two striped bands, one panel and
about eighty glyphs, no 3-D, no asset blits, nothing animating.  Whatever it
costs is what the shared 2-D path costs, and that is charged to every other
screen too.  Cutting the draw down piece by piece gave this, in ms of step:

| drawn | before | after |
| --- | --- | --- |
| nothing (`step` floor) | 0.0 | 0.0 |
| `clear_screen` only (61440-byte memset) | 12.3 | 12.3 |
| + the striped bands | 14.2 | 14.2 |
| + `draw_panel_rect` (216x180) | 28.5 | 25.9 |
| + corners, both headings, the name field | 49.7 | 33.2 |
| + the three help lines (46 glyphs) | **60.5** | **38.9** |

So text alone was 32.0 ms of a 60.5 ms frame -- about 0.24 ms per
character.  Three things were doing it, and all three are the same mistake:

1. **`draw_text`/`draw_text_small` plotted every glyph pixel through
   `put_px`.**  Two calls per lit texel, each re-clipping the point against
   two bounds and recomputing `y*WIDTH+x`, 64 bit tests per character.  They
   now clip once for the whole glyph when it lands wholly inside the
   framebuffer (`blit_glyph_unclipped()`) and walk a row pointer; on FM
   TOWNS the row goes out four pixels at a time through a nibble ->
   byte-mask table, two read-modify-write dwords per row per pass.  That
   part is x86-only on purpose: it writes unaligned 32-bit words, which the
   V810 (whose `ld.w`/`st.w` ignore the low two address bits) and the SH-2
   cannot do, so those targets take the portable byte-store version.
2. **`draw_text_scaled` at scale 1 drew each lit texel as a 1x1
   `rect_fill`** -- a clip, a hardware-2D probe and a one-byte `memset`
   call, twice per pixel.  It is the most expensive way in the file to set a
   pixel, and it drew every heading in the game.  Scale 1 now takes the
   glyph blit above (its shadow sits two pixels down-right, not one, which
   is why it could not simply call `draw_text`).
3. **`rect_outline` walked its two vertical edges through `put_px`.**
   `draw_panel_rect` runs three of those down a 180-pixel panel every frame.
   Clipped once, written through a row pointer.

`draw_masked_bitmap`'s portable path had the same shape (a bounds test and
an address computation per texel, over the ~25000 texels of a story
portrait) and got the same treatment -- CD32X already had a clip-once
version of its own; the new one is byte stores only, so PC-FX shares it.

None of this changes a pixel.  The host build renders the story walk-in, a
duel, the deck editor and the card-detail screens byte-identical to the
build before the change, and the FM TOWNS capture of the name screen differs
only in the debug stamp that holds the timings.

What the whole change is worth on the parked scenes (`step`, ms):

| | before | after |
| --- | --- | --- |
| name entry | 60.5 | 38.9 |
| duel, hand view | 47.0 | 38.1 |
| story plaza | 44.8 | 31.7 |

The rest of the name screen's 38.9 ms is not a mystery any more: 12.3 of it
is `clear_screen` writing 61440 bytes, which at the model's 16-bit bus and
two wait states is about 5 MB/s -- the fill is already at the machine's
memory bandwidth.  Another 7.8 ms is the panel's 38880 bytes.  Making those
cheaper means drawing fewer bytes, not drawing them faster.

### Attribution knobs

All via `EXTRA_CORE_DEFINES`:

| define | what it isolates |
| --- | --- |
| `WAIFU_BATTLE_BASE_CACHE_DISABLE` | the live board render (hand view: 47 ms cached, 167 ms live) |
| `WAIFU_PLAZA_CACHE_DISABLE` | the live story plaza render |
| `FMTOWNS_MEASURE_BLIT_EVERY=n` | run the VRAM blit 1 frame in n, so `present = (palette+flip) + blit/n` and the blit's share can be solved for.  Skipping it outright is useless -- the stamp then never updates |
| `FMTOWNS_MEASURE_DIRTY_BAR` | paint the number of groups the dirty present actually wrote as a bar across row 1.  This is what tells "it is skipping nothing" apart from "the comparison itself is the cost", and those have opposite fixes |
| `CFX_MEASURE_SKIP_SPANS`, `WAIFU_MEASURE_SKIP_BOARD`, ... | as before |

### The second layer: what the hardware actually allows

The obvious next move -- put the cards, the HUD text and the battle
animation on VRAM layer 1, as sprites, and leave the 3D board on layer 0 so
it only redraws when the camera moves -- **cannot be done in this game's
colour depth**, and the reason is in the CRTC, not the software.

From `TOWNSEMU/src/towns/crtc/crtc.cpp`:

* `TownsCRTC::GetPageBitsPerPixel()` returns 8 **only** when
  `LowResCrtcIsInSinglePageMode()` is true.  In two-page mode the only
  encodings are 4bpp (16 colours) and 16bpp (RGB555).  There is no 256-colour
  two-layer mode on this machine.
* `TownsCRTC::UpdateSpriteHardware()` refuses sprites unless the machine is
  **not** in single-page mode and page 1 is 16bpp with a 512-byte line.

So the choice is:

* keep 256 colours -> single page -> exactly one layer, no sprites (today);
* two layers -> layer 0 becomes 16 colours (a drastic art regression, and
  every asset in the game is 8bpp indexed) or 16bpp (which doubles the
  background's VRAM traffic on the bus that is already the bottleneck, and
  breaks the palette-scaling fade in `fmtowns_video_present_8bpp()` --
  fades would go back to costing 61440 dithered pixels a frame).

`common/fmt_layers.c` and `common/fmt_sprite.c` are already in the tree for
whoever wants to try the 16bpp variant; `fmt_set_background_and_sprites()`
sets up exactly that configuration.  It is a port-scale rewrite, not a
change, and it should not be started without first measuring a 16bpp
background blit under the timing model above -- if that alone costs more
than the ~13 ms the dirty present now spends, the whole idea is dead before
the sprites help anything.

### Left to do, in the order they look worth doing

1. **Get one real number off the machine.**  `FMTOWNS_DEBUG_INPUT=1` stamps
   frame/step/present into the top-left pixels; photograph the screen and
   run `tools/fmtowns/read_frame_stamp.py` on the photo.  Then re-fit
   `FMTOWNS_TIMING` and everything below becomes measurable instead of
   modelled.
2. **The composite restore is a whole-screen copy.**  `draw_interactive_base()`
   restores 61440 bytes so that overlays covering maybe a third of the
   screen can be redrawn.  Restoring only the damaged band (the PC-FX
   placement path already does this for its own case) is the largest single
   step win left in a duel.
3. **Flat 2-D UI screens cost as much as 3-D ones** -- answered, see "The
   flat 2-D screens" below.  It was `put_px`, and it is fixed; what is left
   on those screens is the 61440-byte `clear_screen` and the dirty-present
   scan, both of which are at the memory bandwidth the model gives a 16-bit
   bus and neither of which gets cheaper without drawing less.
4. **Two linear passes instead of alternating banks in the blit.**  The blit
   alternates between VRAM `0x00000` and `0x40000` every 4 bytes, which is
   a DRAM row miss per store on real page-mode VRAM and free in an emulator
   that models no such thing.  Two passes would cost one extra read of the
   source per group to make every store sequential.  Deliberately *not*
   done: it cannot be measured here, and the timing model does not model
   DRAM pages.  Adding a row-miss penalty to `Memory`'s wait table would
   make it measurable, and is probably the right next emulator change.
5. **Moving cameras** (the hand<->top lift, card flights, attack sweeps)
   still render live, at the 167 ms/frame the cache-disabled measurement
   shows.  PC-FX quantises the lift onto cached anchors
   (`WAIFU_PCFX_HANDTOP_ANCHORS`); the same trick fits here -- ~59 KB of
   `.bss` spare, one 61440-byte slot.

### 2026-08-27 battle/transition follow-up

Items 2 and 5 immediately above are historical now.  The duel compositor
restores only prior overlay groups, black battle cut-ins use the same retained
base mechanism, hand<->top uses three cached real-3D anchors, and turn changes
use seven retained half-orbit anchors in the existing reusable slot.  No extra
full-screen cache was added: the current 2 MiB link leaves only 7936 bytes
between `.bss` and the stack ceiling, not the older ~59 KiB figure.

Deterministic profiler scenes were added for `lift`, `turn`, `battle`, and
`direct`.  The last calibrated-Marty captures made before the container's X
socket service failed were:

| scene | calibrated rate | frame split |
| --- | --- | --- |
| placement | 14.9-15.9 fps | step 34.4-37.0 ms, present 21.2-22.0 ms |
| two-card battle cut-in | 9.0-11.0 fps | step 56.8-75.3 ms, present 24.7-27.1 ms |

Those are honest **before-follow-up** baselines, not post-change claims.  The
host damage/profiling build verifies the structural reductions while emulator
capture is unavailable:

- battle cut-ins declare an average 705/960 presenter groups instead of a
  forced 960; direct attacks average 473/960;
- the 240-frame hand<->top loop moves all 750 visible/clipped hand-card draws
  through fixed-map fast paths (`generic: 325 -> 0`);
- the dynamic placement scaler's mapped-row implementation halves its host
  attribution time (544 us -> 273 us over 240 frames), and the actual i386
  object contains the intended `lodsw`/indexed-load/`stosb` loop;
- the 300-frame turn loop reduces board rasterizations from 234 to 32 (86%).

Visual verification was deliberately stronger than spot checks.  Production
and forced-full-restore builds produced byte-identical PNGs for every frame of
420 normal battle frames, 300 direct-attack frames, 240 hand<->top frames, 240
placement frames, and 300 anchored-turn frames.  Contact sheets were inspected
across entry, intermediate poses, hit/flash/burn/removal, clipping, flip,
landing, both orbit directions, and loop boundaries.  The damage verifier
reported no undeclared pixels.  Re-run `./fmtowns.sh profile` on calibrated
Tsugaru (and ultimately physical hardware) before attaching a post-change fps
number to these wins.

### Runtime camera fidelity by CPU class

The renderer tier is detected once during `waifu_fm_init()` and stored in the
core; render loops never repeat the PIT-timed 386DX speed probe.  The same binary
selects tier 0 for 386SX-class Marty/UX machines, tier 1 for ordinary 386DX
machines, and tier 2 for fast/cache-equipped 386DX plus 486/Pentium machines.
Gameplay and animation durations remain on the same 60 Hz logic timeline.

| tier | moving board width | hand/top views | turn views |
| --- | ---: | ---: | ---: |
| 386SX | 64 | 3 retained anchors | 5 retained anchors |
| 386DX | 128 | continuous | 9 retained anchors |
| fast 386DX / 486 / Pentium | 256 | continuous | continuous |

Held turn anchors retain the complete board/cards/HUD frame, so they do no
drawing at all; changed anchors seek the XOR board cache from the currently
decoded pose instead of restarting at pose zero.  Under the calibrated 16 MHz
386SX model, the looping turn profile improved from 9.1-11.0 fps (58-79 ms
step) to 16.9-17.4 fps (24-26 ms step on sampled retained frames).  The looping
hand/top profile improved from 8.3-9.3 fps (72-88 ms step) to 13.5-15.4 fps
(20-23 ms step).  A repeat after moving tier detection entirely out of the hot
path measured 14.5-14.9 fps and 20.6-20.9 ms step.  These are emulator comparison figures, not physical-hardware
measurements; anchor-change worst cases remain substantially slower.

`WAIFU_FMTOWNS_FORCE_PERFORMANCE_TIER` exists only as a measurement override.

The normal FM Towns image still links with 7168 bytes below the BSS/stack
ceiling.  The common story regression suite passes, PC-FX passes a fresh
three-pass build and accurate-backend title capture, and CD32X passes at 125696
bytes (5376 bytes below its staging limit) with a fresh `32xcd` title capture.
These checks establish correctness and structural scaling, not real-machine
frame rates; faster-hardware timing still needs a calibrated emulator or
physical capture.
