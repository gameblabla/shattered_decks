# FM TOWNS renderer and performance results

Date: 2026-08-29
Target: FM TOWNS Marty / 16 MHz 80386SX / 16-bit bus / 2 MiB RAM

This note records the implementation that follows `PLAN_OF_ACTION.txt`. The
game is confirmed to boot and read CD-ROM media on real FM TOWNS hardware, but
the timing figures below are from Tsugaru’s calibrated Marty proxy, not a
physical-Marty measurement. The proxy used by `fmtowns.sh` is:

```text
-FREQ 16 -CPUCLOCKSCALE 220 -BUSWAIT 2 -VRAMBUSWAIT 6 -DATABUSWIDTH 16
```

## Implemented changes

- Restored the FM battle cache ownership map: player and enemy top cameras use
  `g_b_base_cache_top`, the hand view uses `g_b_base_cache`, and placement plus
  the tactical battle prelude share the existing 61,440-byte work slot. The
  prelude is keyed by its attacker/row/late-frame inputs and does not allocate
  another full-screen buffer.
- Split elapsed-vblank handling from displayed moving-camera poses. Static and
  2-D effects consume measured wall-clock vblanks; moving battle cameras and
  the result top-to-hand motion advance one authored pose per rendered step.
  New phases render their frame-zero pose before a preceding stall can advance
  them.
- Added an explicit dense-present hint. Only a final live board/floor render
  may request an unconditional full upload; cleared, retained, and sparse
  frames continue through the two-page row-mask comparison path.
- Replaced repeated `255 / span` and signed ceiling divisions in the board
  trapezoid paths with runtime-built exact lookup tables.
- Reused board X/Z projection contributions across the 5x4 mesh while keeping
  the original fixed-point multiply, add, divide, and truncation order.
- Added an FM-only specialized moving 5x4 grid walker. It rebuilds the 49
  unique projected grid edges for each current pose and advances shared edges
  once per scanline; it is selected only after the existing strict-convex,
  screen-bounded proof and falls back to the cached generic walker otherwise.
- Added an FM-only constant-V row filler for the board's top-view rows. It
  feeds the packed direct row loop instead of the general board-address loop,
  which is safe for the fixed 32x32 / constant-V surface and leaves other
  targets on their existing capability path.
- Restricted the exact tactical top endpoint to the visible top mesh: its
  opaque surface covers every side-wall pixel, so the side-wall pass is omitted
  only for those authored endpoint cameras. Moving and placement cameras keep
  their textured, segmented walls.
- Removed three redundant `fb_damage_all()` calls from the fast projected
  primitive path. `render_board()` already clears the complete framebuffer and
  marks the live board as dense; primitive-level full damage invalidation did
  not change pixels and only repeated the damage-mask work.
- Normalized the FM affine card triangle winding once per triangle. The hot
  loop now uses one sign test for the three barycentric weights, while keeping
  the same fixed-point gradients, clamps, and source-pixel selection.
- Measured the FM/i386 tilted board spans and routed them through the existing
  exact packed affine filler. The old texture-boundary splitter averaged only
  1.25--1.34 pixels per run in the moving poses, so its lookup/branch work was
  more expensive than its repeated-color stores. The new path keeps the same
  uint8 UV wrapping and packed stores without splitting every scanline into
  tiny runs.

The profiling-only `--fixed-pose-bench` harness reports camera basis, point
transformation, projection, clear, walls, mesh setup, span fill, grid,
overlay, and presentation-compare regions. It also reports the selected board
path, scanline/pixel/run counts, logical reciprocal-division calls, cleared
bytes, damage groups, and an FNV framebuffer hash. The harness is compiled as
a 32-bit host executable so its stage attribution follows the FM i386 code
path; its wall-clock values are not Marty measurements.

The host scripted field and moving-opening renders remained byte-identical to
their exact reference hashes after the renderer changes. The damage verifier
also reported no undeclared pixels.

## Timing captures

The existing Marty plan contains the last historical calibrated reference from
before its later cache and clipped-card follow-ups:

| Historical scene | FPS | Step | Present |
| --- | ---: | ---: | ---: |
| Placement | 14.9–15.9 | 34.4–37.0 ms | 21.2–22.0 ms |
| Two-card battle cut-in | 9.0–11.0 | 56.8–75.3 ms | 24.7–27.1 ms |

Fresh captures of this tree were made with `./fmtowns.sh profile ...`. The
script takes samples at wall-clock intervals, so battle and attack samples can
land on different authored effects. The current moving-turn samples were:

| Capture | Sample 0 | Sample 1 | Sample 2 | Result |
| --- | --- | --- | --- | --- |
| `profile turn` | 116.5 ms (`82.7 + 22.1`) | 120.3 ms (`82.9 + 22.1`) | 144.4 ms (`108.5 + 22.1`) | intact battle board; phase-sensitive |
| `profile lift` | 140.0 ms (`104.8 + 24.7`) | 108.9 ms (`76.8 + 22.9`) | 82.3 ms (`50.2 + 25.0`) | continuous Up hand-to-top transition |

The parked board capture reported `16.6 ms = 0.4 ms step + 8.6 ms present`
with and without `-DWAIFU_BATTLE_BASE_CACHE_DISABLE`; it is dominated by a
retained/static frame and the stamp warns that the sub-5 ms step can be a
65.5 ms counter wrap. It is therefore a harness sanity result, not a raw
rasterizer speed claim. The inspected PNGs showed the intended board, card,
battle, and attack scenes.

These samples are still far above the 33.3 ms target and are phase-sensitive,
but the Up hand-to-top transition advances through live poses instead of
appearing hung. The inspected PNGs show continuous board perspectives with
intact board, wall, HUD, and card rendering.

For a non-retained board measurement, the final-code `profile board` was run with
`-DWAIFU_BATTLE_BASE_CACHE_DISABLE -DWAIFU_MEASURE_FORCE_LIVE_BOARD`. The
constant-V row filler plus exact top-wall cull produced three identical samples
of `93.1 ms` total (`54.7 ms` step + `22.1 ms` present). The inspected PNG
retained the full board and six projected field cards. The result is a valid
visual regression capture, but it is still above budget; the moving turn and
lift captures remain the more representative performance evidence.

Capture artifacts were written under `build/fmtowns/shots/` and are generated
outputs, not release assets.

## Fixed-pose renderer attribution

The fixed-pose harness was built with `cc -m32 -O2`,
`-DWAIFU_FM_FMTOWNS -DWAIFU_PROFILE_RENDER -DWAIFU_FIXED_POSE_BENCH`, and a
weak host stub for the FM sound callback. The accepted span-filler change was
compared with the pre-change executable and with
`-DWAIFU_MEASURE_BOARD_MESH_REFERENCE`.

| Pose | Board path | Total | Span fill | Pixels | Runs | Hash |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| top | trapezoid rows | 129 us | 38 us | 95,546 | 985 | `3e7c64ce` |
| turn middle | specialized grid | 93 us | 16 us | 10,704 | 418 | `8ebdc7b5` |
| turn tilted | specialized grid | 103 us | 23 us | 11,720 | 609 | `8b7920ef` |

The three hashes matched the exact reference mesh hashes. `Runs` is the number
of emitted filler invocations in the moving path; it is one per board scanline
rather than a count of texture-boundary sub-runs. These host microsecond values
are attribution data only, not Marty timing.

## Memory and build gates

The normal final FM build linked successfully with:

```text
SYSTEM.BIN end: 0x53710 / 0x90000; spare: 248048 bytes
.bss end:    0x1fd400 / 0x200000; spare: 11264 bytes
```

No additional full-screen BSS allocation was introduced. PC-FX also rebuilt
successfully. CD32X rebuilt from `clean-build`; its SH-2 image was 128,896
bytes, below the strict 131,072-byte staging limit.

## Verification limits and next work

- `make regression-story` remains blocked by the pre-existing missing
  `scripts/story_save_duels_regression.sh`. The direct current binary
  `--regression-story-all --frames 1 --no-png` run passed every listed story,
  cache, CD, support, trap, fusion, and equip regression. A separate
  damage-verifier build completed the 900-frame battle-mode demo with zero
  undeclared-pixel violations.
- The accurate PC-FX headless backend booted the rebuilt image and produced an
  inspected title PNG. The documented PC-FX regression scripts are absent in
  this checkout.
- No CD32X headless capture tool is present in this checkout, so its required
  visual gate could not be run after the successful build/size gate.
- Tsugaru cannot establish physical hardware timing. A future physical-Marty
  run should capture the same frame stamp for board, placement, battle, and
  direct scenes before publishing FPS claims.
- A trial card edge-to-span mapper was rejected: although its fixed-pose hashes
  matched, its calibrated FM turn step grew to roughly 180--202 ms because of
  extra setup division/64-bit work. It is not present in this tree.
- A trial dense-present damage early-out was also rejected. Two repeated turn
  captures regressed to 168--191 ms of calibrated step work versus the accepted
  83--109 ms range, despite unchanged pixels; the committed dense/damage
  boundary therefore remains as before.
- The full plan’s remaining renderer work is wall occlusion/batching if its
  nine segmented walls can be reduced without changing their visible edge
  pixels, plus live 3-D damage-union clearing. Any further specialization must
  remain behind the byte-identical reference gate. The fixed-pose attribution
  shows projection is currently a small stage on the 32-bit harness; the FM row
  filler and endpoint wall cull are accepted, but the live Marty-proxy
  measurements remain above the frame budget and require another optimization
  cycle plus physical Marty timing before claiming completion.
