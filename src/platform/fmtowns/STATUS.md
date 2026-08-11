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

| build                                        | game step per frame |
| -------------------------------------------- | ------------------- |
| baseline (byte-loop `memcpy`, no caches)      | 4.5 ms              |
| `rep movsl` memcpy                            | 1.7 ms              |
| ... plus `rep stosl` memset                   | 47 ms  (!)          |
| shipped (memcpy only + top-view composite)    | 1.8 ms              |

1. **`memcpy` was a C byte loop** (`waifu_fmtowns_runtime.c`).  The game
   core's `copy_u8_fast()` has hand-written backends for PC-FX and CD32X and
   falls through to plain libc everywhere else, so on this target every card,
   panel and glyph blit was a per-byte load/store/increment/branch.  It is
   now `rep movsl` with the destination aligned first, and blocks under 32
   bytes still take the byte loop because three `rep` instructions cannot be
   repaid by a handful of bytes.  2.6x off the game step.
2. **The same change to `memset` is an 11x LOSS** and is not shipped.  That
   is not a typo, and the attribution is not in doubt -- it was measured with
   memcpy left alone.  A standalone benchmark of the identical routine inside
   the payload says `rep stosl` beats the byte loop at every size tried, so
   whatever this is, it is about how thousands of small fills interleave with
   a real frame under Tsugaru, not about the instruction.  Read the comment
   above `memset()` before touching it.
3. **The battle-composite cache is on, for the top-down board cameras only**
   (`WAIFU_BATTLE_BASE_CACHE_DISABLE` dropped from `Makefile.fmtowns`; the
   `WAIFU_FM_FMTOWNS` branch in `battle_base_cache_for_camera()` picks the
   cameras).  60 KiB of `.bss` to skip a whole software-3D board render on
   the one view where that render costs more than the 61440-byte restore
   copy.  Caching *every* camera, which is what the generic branch does, made
   the cheap hand view slower than no cache at all.
   `WAIFU_BG_CACHE_DISABLE` and `WAIFU_FLOOR_SAMPLE_CACHE_DISABLE` stay on
   for the same kind of reason (the second is 256 KiB per tile size and
   simply does not fit).

**The frame rate is now paced in software, and it has to be.**  The present
path ends in `fmt_flip_page()`, which waits on a vsync edge, and that was the
only thing holding the game to a sane speed.  Once the step and the VRAM
blit got cheap, that wait started returning in ~3 ms instead of a display
field and the whole game ran at **325 fps** -- every animation five times too
fast.  `fmtowns_frame_pace()` in `fmtowns_main.c` now holds the loop to one
60 Hz period per `waifu_fm_step()` off the same 1 us counter, which is a
property of the game rather than of whatever the CRTC registers make Tsugaru
do.  The parked-scene frame time with it is 19.8 ms.

**What is not established.**  None of this has run on a physical Marty.  The
`memset` result in particular contradicts the 386's published instruction
timings, so it may be an emulator artefact -- but the emulator is the only
oracle there is, and shipping the version that measures faster on it is the
only defensible call until someone has real hardware.  The `present` figure
in the stamp is dominated by that vsync wait and is not stable run to run;
`step` is, and is what the table above reports.

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
channel slot.  They are linked into the payload rather than staged on the
CD, because reading them off disc would stop the music every time the game
beeped.  `fmtowns_sfx.c` voices them round-robin over 6 channels and skips
re-uploading a sample that is already in the target slot;
`sounds.c`'s `waifu_sound_play()` reaches it the same way it reaches the
PC-FX and CD32X players.  `common/sound.c`'s wave-RAM upload was also fixed
to select the 4 KiB bank once per bank instead of once per byte.

**Verified two ways**: with CD-DA muted, Tsugaru's FM/PCM recording contains
discrete effect bursts exactly where the script pressed buttons; and a
wave-RAM dump matches the generated sample bytes exactly, in distinct
channel slots (SELECT, CONFIRM, CONFIRM_ALT and CARD_DRAWN -- the four the
run actually triggered).

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

## What is left

- **Frame rate.**  The game step is down to ~1.8 ms in the parked duel scene
  (see "Performance: the duel, actually measured"), and the frame is now
  bounded by the present path -- mostly by the vsync wait inside
  `fmt_flip_page()`, whose duration under Tsugaru is neither stable nor
  obviously related to a 60 Hz field.  Whether the remaining ~18 ms is a real
  hardware cost or an emulator artefact of this custom CRTC mode is the next
  thing worth finding out; if it is the latter, dropping the flip's wait and
  leaving pacing entirely to `fmtowns_frame_pace()` is the obvious move, but
  it risks tearing on a real Marty and nobody has one.
- **Heavy scenes have not been profiled.**  The captures park in the duel's
  hand view.  The card-vs-card battle screen with its two large art panels
  measured 28-62 ms of game step on the baseline build and is the worst scene
  anyone has caught; story dialogue, the sanctum map, and attack animations
  with moving cameras (which miss the composite cache by construction) are
  unmeasured.  Park the debug script on one and read the stamp.
- **Animation pacing.**  `waifu_fm_set_frame_vblanks(1)` is hard-coded, so a
  frame that overruns its budget makes battle animations run slow rather than
  dropping steps.  `fmtowns_frame_pace()` now supplies the free-running time
  source this needs (I/O 0x26, no YM2612 timer required), so the remaining
  work is feeding a real elapsed-frame count into the core.
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
