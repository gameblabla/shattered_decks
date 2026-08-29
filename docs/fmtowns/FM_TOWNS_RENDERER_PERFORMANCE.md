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
- Replaced the FM tilted-span boundary divisions with an exact 9x7 lookup for
  the useful fixed-point magnitudes, including the zero and `-128` cases.
- Replaced repeated `255 / span` and signed ceiling divisions in the board
  trapezoid paths with runtime-built exact lookup tables.
- Reused board X/Z projection contributions across the 5x4 mesh while keeping
  the original fixed-point multiply, add, divide, and truncation order.

The host scripted field and moving-opening renders were byte-identical to the
starting checkpoint after the renderer changes. The damage verifier also
reported no undeclared pixels.

## Timing captures

The existing Marty plan contains the last historical calibrated reference from
before its later cache and clipped-card follow-ups:

| Historical scene | FPS | Step | Present |
| --- | ---: | ---: | ---: |
| Placement | 14.9–15.9 | 34.4–37.0 ms | 21.2–22.0 ms |
| Two-card battle cut-in | 9.0–11.0 | 56.8–75.3 ms | 24.7–27.1 ms |

Fresh captures of this tree were made with `./fmtowns.sh profile ...`. The
script takes samples at wall-clock intervals, so battle and attack samples can
land on different authored effects. The settled placement samples were:

| Capture | Sample 0 | Sample 1 | Sample 2 | Result |
| --- | --- | --- | --- | --- |
| `profile place` | 56.4 ms | 57.5 ms | 56.4 ms | stable settled samples |
| `profile battle` | 73.2 ms | 47.1 ms | 26.1 ms | phase-sensitive; not a stable median |
| `profile direct` | 65.9 ms | 64.3 ms | 122.5 ms | phase-sensitive; not a stable median |

The parked board capture reported `16.6 ms = 0.4 ms step + 8.6 ms present`
with and without `-DWAIFU_BATTLE_BASE_CACHE_DISABLE`; it is dominated by a
retained/static frame and the stamp warns that the sub-5 ms step can be a
65.5 ms counter wrap. It is therefore a harness sanity result, not a raw
rasterizer speed claim. The inspected PNGs showed the intended board, card,
battle, and attack scenes.

Capture artifacts were written under `build/fmtowns/shots/` and are generated
outputs, not release assets.

## Memory and build gates

The normal final FM build linked successfully with:

```text
SYSTEM.BIN end: 0x52710 / 0x90000; spare: 252144 bytes
.bss end:    0x1fd400 / 0x200000; spare: 11264 bytes
```

No additional full-screen BSS allocation was introduced. PC-FX also rebuilt
successfully. CD32X rebuilt from `clean-build`; its SH-2 image was 128,576
bytes, below the strict 131,072-byte staging limit.

## Verification limits and next work

- `make regression-story` remains blocked by the pre-existing missing
  `scripts/story_save_duels_regression.sh`. The direct current binary
  `--regression-story-all --frames 1 --no-png` run passed every listed story,
  cache, CD, support, trap, fusion, and equip regression; the damage-verifier
  build passed the same set.
- The accurate PC-FX headless backend booted the rebuilt image and produced an
  inspected title PNG. The documented PC-FX regression scripts are absent in
  this checkout.
- No CD32X headless capture tool is present in this checkout, so its required
  visual gate could not be run after the successful build/size gate.
- Tsugaru cannot establish physical hardware timing. A future physical-Marty
  run should capture the same frame stamp for board, placement, battle, and
  direct scenes before publishing FPS claims.
- Further renderer work should follow the plan’s attribution order: collect
  fixed-pose stage counters, validate reciprocal projection only where it is
  still significant, then consider wall batching and live 3-D damage unions.
  Any 5x4 planar-mesh specialization must remain behind the byte-identical
  reference gate.
