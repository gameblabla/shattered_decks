---
name: fmtowns-build-verify
description: Build, run, profile, capture, and visually verify the FM TOWNS/Marty target. Use for Makefile.fmtowns, fmtowns.sh, Tsugaru, frame stamps, screenshots, physical-hardware evidence, or FM TOWNS regressions.
---

# FM TOWNS build and verification

Use `./fmtowns.sh` as the canonical entry point; it preserves the flags and safeguards that are
easy to get subtly wrong.

## Ground truth and evidence

The owner confirmed on 2026-08-27 that the current game runs on real FM TOWNS hardware and that
CD-ROM access works after the latest CD-ROM-access update. `IMG_6472.mov` is a phone recording of
the game on real hardware. This supersedes older blanket statements in `STATUS.md` that nothing
has run physically. Do not generalize it into confirmation of every scene, audio output, saving,
or frame-time number.

Rank evidence for a claim as follows:

1. A current physical-hardware observation of the exact behavior.
2. A deterministic emulator run whose frame pixels/state stamp were checked.
3. A host/headless regression of the same shared behavior.
4. Source inspection or a successful build alone.

Name which level supports the conclusion. Never silently replace owner-provided physical evidence
with an older document's uncertainty.

## Build and run

```sh
./fmtowns.sh build
./fmtowns.sh build --iso                 # data-only image, no CD-DA tracks
./fmtowns.sh run                         # interactive Tsugaru, Marty by default
./fmtowns.sh test                        # debug input, captures, decoded stamps
./fmtowns.sh profile hand                # stable parked-scene comparison
./fmtowns.sh profile board
./fmtowns.sh profile place
./fmtowns.sh profile lift
./fmtowns.sh profile turn
./fmtowns.sh profile battle
./fmtowns.sh profile direct
./fmtowns.sh profile story
./fmtowns.sh profile name
./fmtowns.sh profile fire
```

Extra make variables follow the command, for example:

```sh
./fmtowns.sh profile board 'EXTRA_CORE_DEFINES=-DWAIFU_BATTLE_BASE_CACHE_DISABLE'
FMTOWNS_SHOTS=/tmp/fmt-after ./fmtowns.sh profile hand
FMTOWNS_TOWNSTYPE=MX FMTOWNS_ROM=/path/to/plain/ROMS ./fmtowns.sh test
```

`FMTOWNS` is not a valid Tsugaru `-TOWNSTYPE`; use a real model such as `MARTY`, `MX`, or `UX`
with a matching ROM directory. Keep `-MEMSIZE 2` when testing Marty compatibility.

The build enforces the payload ceiling before `0x90000`: the loader begins at `0x90000`, so a
larger payload overwrites the live loader and otherwise yields only a black boot. Also inspect the
linker's `.bss`/stack assertion; current optimization work left only a small margin.

## Capture and visual inspection

`tools/fmtowns/headless_shot.sh` runs Tsugaru under Xvfb, feeds monitor commands, and writes PNGs.
The monitor screenshot command is `SS path.png`. Always timeout-guard Tsugaru with SIGKILL; plain
SIGTERM has hung before.

`test` and `profile` compile `FMTOWNS_DEBUG_INPUT=1`. The target merges a frame-indexed script with
the real pad read and stamps frame/state/music/timing data into the top-left pixels.
`tools/fmtowns/read_frame_stamp.py` decodes those pixels from emulator screenshots or a sufficiently
clear physical-screen photo.

For visual verification:

1. Read the decoded stamp to establish which state the capture actually reached.
2. Open the PNG with an image-capable viewer/model; do not infer appearance from filenames,
   dimensions, hashes, or logs.
3. Compare equivalent states and inspect transitions with multiple frames/contact sheets when the
   change moves, fades, clips, page-flips, or restores cached regions.
4. If a capture is black, retry with a fresh Xvfb instance and longer boot wait. A historical Xvfb
   path produced black images even for a live payload.

With local pi vision, start ninfer using `tools/local_ai/serve_ninfer.sh`, ensure pi's model declares
`input: ["text", "image"]`, then use:

```sh
pi --provider local-llm --model qwen3.8-27b --no-tools --no-session -p \
  @build/fmtowns/shots/shot0.png \
  'Describe the visible state and any clipping, stale regions, stripes, or palette errors.'
```

## Performance comparisons

Use `profile`, not the menu walk-in. CD loads take wall time while debug input is keyed to game
frames, so two builds can reach different screens and produce a convincing but invalid comparison.
Parked auto-scenes remove that desynchronization and deterministic auto builds fix deck entropy.

The default `FMTOWNS_TIMING` models a 16 MHz 386-class machine with a 16-bit bus and calibrated
RAM/VRAM waits. It is a comparison proxy, not a physical measurement. Compare like-for-like builds,
prefer `step` over noisy vsync time, and attach real-hardware fps only to a physical frame-stamp
capture. Set `FMTOWNS_TIMING=` only when intentionally testing the old zero-wait behavior.

After shared-code changes, first apply the `headless-core` regression gate, then rebuild/capture FM
TOWNS. Pixel-sensitive work should compare output frames, not just stamp numbers.
