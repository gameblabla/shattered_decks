# PC-FX 3D Pixel-Order Fix Plan for GPT-5.6 Luna

## Mission and hard constraints

Fix the PC-FX 3D board's horizontal pixel-pair corruption, prove the result with
current-build image comparisons, and commit the implementation unsigned.

This document is a plan, not the implementation. The planning task that created
it deliberately changes no C, assembly, generated asset, build script, or image.

When executing the plan:

- Read `AGENTS.md`, `.claude/skills/pcfx-architecture/SKILL.md`,
  `.claude/skills/pcfx-build-verify/SKILL.md`, and
  `.claude/skills/headless-core/SKILL.md` completely before editing.
- Read `.claude/skills/fmtowns-build-verify/SKILL.md` before producing or
  interpreting the FM TOWNS reference capture.
- Preserve every unrelated dirty-worktree change. Do not stage or rewrite it.
- Do not edit FM TOWNS, CD32X, generated assets, palettes, or KING presentation
  code merely to make the PC-FX comparison look closer.
- Keep common code platform-agnostic. Platform variation belongs in renderer
  capability/configuration and the split renderer backend.
- Use the default accurate `pcfx-headless` video backend. Never use
  `--fast-video` for acceptance captures.
- A successful build is only gate B. Gate V requires opening and inspecting the
  intended board frame.
- Commit only the reviewed implementation and its focused regression support,
  using `git commit --no-gpg-sign`.

## What the inspection found

The likely defect is more specific than “KING is big-endian.” There are two
different storage boundaries, and the current renderer partially treats them as
one:

1. The V810 is little-endian and the 3D renderer currently draws into a
   byte-linear CPU framebuffer.
2. Packed V810 span paths use `st.h` after constructing a halfword as
   `(left << 8) | right`.
3. A little-endian halfword store places the low byte at the lower address, so
   that value reaches CPU RAM as `right, left`.
4. `waifu_pcfx_video.c` later uploads the CPU framebuffer to a 512-wide KING
   affine source. It deliberately duplicates each logical framebuffer byte into
   a complete KRAM word. Therefore the presenter preserves the already-swapped
   CPU byte order; it does not repair it.
5. KING's direct KRAM data port is a separate case: a word sent through
   `out.h` wants the left logical pixel in the high byte. That rule must not be
   confused with the layout of a halfword stored into little-endian CPU RAM.

The current configuration supports this diagnosis:

- `src/engine/renderer3d_port.h` sets `CFX_RENDERER_DIRECT_KRAM` to `0` for
  PC-FX, so the live renderer is not currently drawing the board straight to
  KING.
- The same PC-FX branch leaves `CFX_RENDERER_PAIR_LOW_BYTE_LEFT` at its default
  `0`, although its active destination is a little-endian byte-linear CPU
  framebuffer.
- `src/engine/renderer3d_internal.h` documents high-byte-left as the PC-FX rule,
  but that comment describes a halfword handed to KING, not the active
  CPU-framebuffer path.
- Several V810 `st.h` loops in `src/engine/renderer3d_spans.inc` manually use
  high-byte-left packing. The perspective floor hot loop
  `cfx_draw_board_pair_nowrap()` is the highest-value first check. Generic LUT
  spans and direct-row spans contain the same pattern.
- Scalar `st.b` paths, including the current V810 branch of `cfx_board_fill()`,
  naturally retain byte order. A symptom that appears only in packed paths is
  therefore expected.
- Commit `b79fcae` fixed V810 `ld.b` sign-extension corruption for palette
  indices at or above 128. Preserve that masking behavior; pixel order and
  sign extension are independent issues.

This is the leading hypothesis, not permission to patch blindly. Prove the
destination and the byte sequence first.

## Existing image comparison and its limits

The planning pass opened these existing captures:

- `screenshots/waifupcfx_shot_11700_top_view_with_card.png`
- `screenshots/waifupcfx_shot_12365_battle_two_cards_facing.png`
- `screenshots/fmtowns-accepted-board/shot1.png`
- `artifacts/pcfx_retail_pdf/captures/battle-board.png`

The PC-FX top view and the FM TOWNS board show broadly corresponding checker
geometry and material orientation. A nearest-neighbour magnified crop makes the
pixel structure inspectable, but these files are not a valid pass/fail oracle:
the states and card populations differ, the FM TOWNS emulator capture is scaled,
several files are untracked, and none establishes that it came from the current
PC-FX build. Treat them as symptom/context evidence only. Produce fresh,
deterministic, same-revision captures for the actual decision.

Also note that `Makefile.pcfx` currently names regression scripts under
`scripts/pcfx/`, while that directory is absent from the inspected worktree.
Before relying on those targets, search history and the worktree to determine
whether the scripts were intentionally removed, live on another branch, or are
untracked elsewhere. Do not silently invent replacements or modify the Makefile
as part of the pixel-order fix. A raw `pcfx-headless --commands` flow is an
acceptable fallback, but verify every input step visually.

## Phase 0: freeze the baseline

1. Record `git status --short`, the branch, and `git rev-parse --short HEAD`.
2. List the exact files proposed for editing before touching them. The expected
   implementation scope is renderer configuration/internal packing and the
   PC-FX span backend; expand it only when evidence requires it.
3. Build a fresh PC-FX CD with the canonical three-link target:

   ```sh
   make -f Makefile.pcfx cd V810GCC=/opt/v810-gcc PREFIX=v810
   ```

4. Fresh-boot that exact image. Do not reuse a state created by another build,
   because PC-FX emulator states embed the loaded program.
5. Drive to two deterministic board views:

   - the tactical/top-down board, which makes alternating horizontal pixel
     pairs easiest to see;
   - a tilted/perspective board frame, which exercises
     `cfx_draw_board_pair_nowrap()` and clipping/odd-edge handling.

6. Capture a broad frame window, extract several frames, crop the left 256x240
   content, and open the images. Do not infer correctness from logs or hashes.
7. Save baseline captures and command files under a task-specific `/tmp`
   directory so the dirty repository is not polluted.

If the current build does not reproduce the reported defect, stop. Compare the
exact revision, build defines, capture backend, and user/physical-hardware
evidence. Do not manufacture a fix for an absent symptom.

## Phase 1: prove the byte-order boundary

Use a tiny, temporary diagnostic that makes a four-pixel logical sequence
unambiguous, for example palette indices `0x11, 0x22, 0x33, 0x44`. It must test
the renderer's CPU-framebuffer destination, not just a C expression on the host.
Keep the diagnostic isolated behind a task-only build define or in a focused
test harness, and remove throwaway display/debug code before the final build.

Establish all three expected representations:

| Boundary | Logical left-to-right pixels | Expected numeric halfwords | Expected bytes at increasing CPU addresses |
|---|---|---|---|
| Little-endian CPU framebuffer | `11 22 33 44` | `0x2211 0x4433` | `11 22 33 44` |
| KING KRAM data port | `11 22 33 44` | `0x1122 0x3344` sent by `out.h` | Not a CPU byte-array rule |
| Current suspected packed CPU path | `11 22 33 44` | `0x1122 0x3344` stored by `st.h` | `22 11 44 33` (failure) |

Confirm that the scalar byte path produces the first row and the V810 packed
path produces either the same row (hypothesis rejected) or the failure row
(hypothesis confirmed). A capture with alternating one-pixel vertical colors is
also useful: the bug should visibly swap each `(x, x+1)` pair without shifting
the overall span by one pixel.

Before editing, audit every packing sink and label it by destination:

- `st.h` or C `uint16_t` store into the CPU framebuffer;
- `out.h` through the KING KRAM data register;
- byte store into the CPU framebuffer;
- dead code excluded by the current capability flags.

Do not decide byte order from the function name “pcfx”; decide it from the
actual destination of that store.

## Phase 2: implement the smallest complete fix

Preferred design: make destination semantics explicit so a future re-enable of
direct KRAM cannot silently reverse the bug again.

1. Give byte-linear framebuffer pairs and KING-port pairs distinct helpers or
   equally clear destination-specific operations.
2. For PC-FX CPU-framebuffer halfword stores, place the logical left pixel in
   the low byte and the logical right pixel in the high byte.
3. For direct KING `out.h` stores, retain left in the high byte and right in the
   low byte.
4. Update the PC-FX capability/commentary so it describes the active
   destination. Do not encode CPU endianness as “KING word order.”
5. Audit all active V810 CPU `st.h` packers, not only the first visible board
   loop. At minimum inspect:

   - `cfx_draw_scanline_fast_pair_pcfx_nowrap()`;
   - `cfx_draw_row_lut_pair_pcfx_nowrap()` if compiled by the configuration;
   - the direct-tile constant-V row loop;
   - `cfx_draw_board_pair_nowrap()`;
   - portable fallback calls to the pair-packing helper;
   - odd/even read-modify-write edge helpers.

6. Retain the unsigned-byte masks required after V810 `ld.b`; when reversing
   lanes, mask whichever loaded value becomes the unshifted low lane so sign
   extension cannot overwrite the other pixel.
7. Keep CD32X's big-endian CPU-framebuffer behavior unchanged, and keep FM
   TOWNS low-byte-left behavior unchanged.
8. Do not touch `waifu_pcfx_video.c` unless the Phase 1 evidence disproves the
   CPU-renderer hypothesis. The current affine uploader's duplicated-byte KRAM
   layout is intentional and affects all 2D presentation, not just 3D.

The tempting one-line change `CFX_RENDERER_PAIR_LOW_BYTE_LEFT=1` is insufficient
by itself because active V810 assembly packers construct their halfwords
manually. Conversely, reversing only `cfx_draw_board_pair_nowrap()` is too narrow
and can leave block faces, generic textured quads, or edge pixels inconsistent.

After the first edit, inspect the diff immediately. If it touches unrelated
platform paths or generated data, narrow it before building.

## Phase 3: fast regression and build gate B

Because the likely robust fix touches shared renderer helpers/configuration,
run the cheap host gate before the console build:

```sh
make regression-story
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
```

Add or run the focused pixel-pair regression from Phase 1. It must assert exact
byte values, include palette values above 127 to retain the sign-extension fix,
and cover even-aligned, odd-aligned, even-length, and odd-length spans. Do not
claim that a host-only test executes V810 assembly; keep the actual V810 result
as a separate gate.

Then rebuild PC-FX:

```sh
make -f Makefile.pcfx cd V810GCC=/opt/v810-gcc PREFIX=v810
```

Review new warnings, especially character-width and always-true warnings, while
ignoring known pre-existing noise. Confirm that the `cd` target completed all
three links. If gate B fails twice with the same symptom, stop, reread the PC-FX
skills and this plan, and revise the hypothesis instead of retrying unchanged.

## Phase 4: visual gate V and image comparisons

Fresh-boot the rebuilt image; never use the baseline save state. Reach the same
two deterministic board views using the same relative input sequence. Capture a
broad window and sample multiple frames so a fade, page flip, or camera motion
cannot masquerade as a stable result.

For each view, create a contact sheet containing:

1. PC-FX before;
2. PC-FX after;
3. FM TOWNS current-build reference at the equivalent state;
4. a nearest-neighbour 8x or greater crop of a high-detail board tile;
5. an absolute before/after difference image for the PC-FX crop.

Normalize only presentation framing:

- crop PC-FX headless Y4M to the left 256x240 content;
- crop the FM TOWNS emulator border using the decoded frame/state stamp, then
  downsample its integer display scaling with nearest-neighbour sampling;
- never blur, resample with interpolation, recolor, or shift individual pixels
  to force agreement.

Use FM TOWNS as a semantic reference for left/right texture orientation and
board geometry, not as an exact pixel oracle. The two targets use different
renderer capabilities and may legitimately differ in shading, sampling, and
camera-era optimizations. The strongest pixel-order oracle is the known
asymmetric pattern and the same PC-FX build before/after.

Acceptance criteria:

- the asymmetric diagnostic arrives in logical left-to-right byte order;
- alternating `(x, x+1)` swaps disappear on textured board spans;
- top-down and perspective board textures have the intended orientation;
- odd left/right span edges do not overwrite their neighboring pixels;
- block faces, live pieces, card faces, HUD, and non-3D 2D drawing are intact;
- palette indices above 127 render without the older sign-extension corruption;
- page flips show neither comb garbage nor stale bands;
- at least two inspected frames per moving/transitioning state agree;
- accurate-backend captures, not `--fast-video`, provide the evidence.

If the pair swaps disappear but geometry still looks wrong, do not keep changing
endianness. Split the remaining symptom into UV orientation, winding/culling,
edge clipping, texture-atlas pitch, or stale page-shadow hypotheses and test one
at a time.

## Phase 5: iteration rules

Use short evidence-driven iterations:

1. State one hypothesis and its predicted image/byte signature.
2. Make the smallest change that distinguishes it.
3. Run the focused byte-order check.
4. Run gate B.
5. Fresh-boot, capture, and look at gate V.
6. Record pass/fail and the next hypothesis in the task notes.

Failure pivots:

| Observation | Interpretation | Next action |
|---|---|---|
| Scalar pixels correct, packed pairs reversed | CPU halfword packing confirmed | Fix all active CPU `st.h` packers; preserve KING packing |
| Both scalar and packed CPU bytes correct, screen reversed | Presenter/KING boundary implicated | Trace one known byte through affine upload and KRAM fetch before editing video code |
| Only palette indices >=128 corrupt | Sign-extension regression | Recheck lane masks around every V810 `ld.b` |
| Interior correct, odd edges corrupt neighbors | Edge read/modify/write semantics wrong | Test all four alignment/length combinations and destination-specific edge helpers |
| Top view correct, tilted board wrong | Perspective direct-tile path differs | Trace `cfx_draw_board_pair_nowrap()` and UV stepping, not global presentation |
| Every 2D element is also horizontally wrong | Not isolated to 3D packing | Re-evaluate capture scaling and affine uploader configuration |
| Accurate emulator passes, physical PC-FX fails | Emulator is insufficient for exact hardware behavior | Treat physical hardware as authoritative and inspect timing/KRAM assumptions |

If the same gate fails twice with the same symptom, stop and rewrite the next
iteration rather than repeating it.

## Phase 6: cleanup, final audit, and unsigned commit

1. Remove throwaway diagnostic drawing, debug defines, temporary captures, and
   stale emulator states. Keep a focused automated regression only if it is
   deterministic, maintainable, and genuinely exercises the fixed contract.
2. Re-run the focused tests, headless gate when shared code changed, full PC-FX
   gate B, and accurate-backend gate V after cleanup.
3. Inspect `git diff --check`, `git diff --stat`, and the full diff.
4. Confirm the staged set contains only the intended renderer fix, its focused
   regression support, and intentional documentation. Never stage unrelated
   dirty files with `git add -A` or `git add .`.
5. Commit unsigned, for example:

   ```sh
   git commit --no-gpg-sign -m "Fix PC-FX 3D framebuffer pixel order"
   ```

6. Report the commit hash, exact tests/build commands, capture paths, and the
   evidence level. Emulator captures are deterministic emulator evidence; do
   not describe them as physical-hardware proof. If the owner can run the build
   on a real PC-FX, request the same top-down and tilted-board observations as
   the final hardware-authoritative check.

## Definition of done

The work is done only when the pixel-order contract is explicit at both the CPU
framebuffer and KING boundaries, every active PC-FX packed 3D path follows that
contract, focused byte tests pass, headless regressions pass when applicable,
the three-link PC-FX CD build succeeds, accurate-backend before/after captures
have been opened and compared at equivalent states, unrelated changes remain
untouched, and the verified implementation is committed unsigned.
