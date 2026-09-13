# SNES issue resolution plan

Prepared 2026-09-13 for [supernes_issues.txt](supernes_issues.txt), using the current worktree and [the supplied session](codex-session-01a09849-916c-7be2-a87b-4fa17072af87.md).

This is an implementation and verification plan. It does not implement the fixes. Line references below refer to the files as inspected on this date, on top of HEAD `776a01c056d8b804fb87fc29ab838ae50b115b7f`; many SNES files already contain uncommitted changes. Function names are included so references remain useful after edits move the lines.

## 1. Required outcomes and scope

| ID | Reported issue | Required result |
| --- | --- | --- |
| S1 | Title's final scanline is blue | All 224 visible lines show their intended artwork; menu plate and prompt blinking remain correct. |
| S2 | Selected card sprite glitches | The selected hand card retains its complete tiles and correct palette throughout selection, bobbing, uploads, placement, and scene returns. Also test the separate held 3D card. |
| S3 | Separate top-view Mode 3 screen | Overhead inspection becomes a camera pose of the same 3D board renderer, with projected field cards and cursor. |
| S4 | Renderer glitches, especially during movement | Remove the 128×72 motion target and all 2× enlargement. Render the board at 256×144 throughout; optimize that path and display completed pictures safely. |
| S5 | Slow, buggy 2D battle | Implement Mode 4 offset-per-tile vertical card movement: player's card moves down, opponent's moves up. Animate at display-field cadence. |
| S6 | Unelaborate direct attack | Provide a distinct sequence based on the PC/SDL3 choreography: anticipation/lunge, blade, contact flash, expanding burst, rays/sparks, large damage readout, and return. |

Implementation should remain in `src/snes/`, `tools/snes/`, and `Makefile.snes`. The SNES entry point explicitly does not compile `src/main.c` ([snes_main.c](src/snes/snes_main.c), lines 1–8; [Makefile.snes](Makefile.snes), lines 87–112). Use the shared/PC implementation as a behavioral reference, not as code to modify for an SNES-only effect.

Preserve the existing dirty worktree. Regenerate asset headers, assembly, and blobs through the Makefile. The historical instructions to finish and commit in the supplied transcript are context, not part of this planning request.

The new decisions supersede `SNES_MODE3_PLAN.md` where it specifies reduced motion resolution, the line doubler, progressive sharpening, or the resident top-view screen. Keep the useful full-resolution assets, arithmetic fixes, and regression work already present.

## 2. Baseline evidence and remaining uncertainty

A small baseline was actually run while preparing this plan, using the existing ROM after `verify.check_rom_fresh()` passed. It was not rebuilt or modified. Captures and isolated test SRAM are under `build/snes/supernes-plan-baseline/`.

ROM SHA-256: `94fb055f30c4ec00f3f614ac128c90b119e280bfc81b15eb41ed4b08b2352db8`.

| Observation | Evidence and implication |
| --- | --- |
| Existing title check passes despite the reported line | `check_title()` reports 256×224 artwork, 100% non-black. Its checks at [verify.py](tools/snes/verify.py), lines 273–303, compare tile bytes and selected interior rows, not the full displayed image. |
| Last title line is wrong | `title.ppm` row 223 has exactly one RGB color, `(56,120,176)`; rows 221 and 222 have 58 and 52 colors. The entire bad row exactly matches repeated row zero of generated tile zero after normalization to five-bit RGB. |
| The title has a concrete indexing explanation | `title_upload()` writes BG1 VOFS zero, and generated map row 28 consists of tile-zero references. With the observed one-line PPU offset, the last visible line reaches that unused row. Fix the scroll alignment, not the bottom line's color. |
| Existing hand-card test fails | `check_hand_is_per_face()` reports selected hand card 0 matching only **959/1024 pixels**. `still.png` was visually inspected. This establishes a mismatch, not yet whether the cause is tile DMA, palette DMA, OAM overlap, or the comparison's assumptions. |
| Full-resolution rest assets are sparse | Both generated resting floor images occupy **236 of 576** screen cells. This supports investigating sparse frame storage; it does not prove the peak occupancy of turns, lifts, or held cards. |
| Battle currently reveals static cards | [snes_duel.c](src/snes/snes_duel.c), lines 1325–1333, expands horizontal windows over stationary cards. Lines 1851–1858 increment a 90-iteration timer. There is no vertical lane animation. |
| Uploaded board work can outlive entry into art mode | `enter_mode3_art()` / `snesCardArtEnter()` do not suspend the enabled framebuffer NMI drain. An in-flight board job must not survive a VRAM ownership change. Reproduce this under queued-work stress; do not assume it explains every observed glitch. |

The supplied session records substantial Mode 3 migration work, converter tests, and unfinished performance/debugging work. Its final activity is an image inspection, not a completed verification/commit report. Its old reduced-resolution timings must not be reused as measurements of the replacement renderer.

Only the freshness, title, and hand checks above were executed for this plan. The remaining tests described below are implementation gates, not claimed passes. No physical SNES verification was performed.

## 3. Implementation order

1. **P0 — Capture and instrumentation:** retain the baseline, add reliable event/presentation stamps and targeted test selection.
2. **P1 — Small visible fix:** correct title alignment and add a test that catches the actual bottom-line failure.
3. **P2 — Presentation ownership and selected card:** establish a shared VBlank budget, stop obsolete uploads at scene boundaries, and isolate/fix the selected-card mismatch.
4. **P3 — Full-resolution renderer/presenter:** prove tile and memory budgets, implement completed-frame publication, then remove the reduced-resolution path.
5. **P4 — Overhead camera:** use the renderer from P3 for overhead inspection and all transitions.
6. **P5 — Mode 4 battle:** implement independent vertical lanes, fixed-rate sequencing, outcome beats, and restoration.
7. **P6 — Direct-attack effects:** add the PC-inspired sequence on the stable presentation system.
8. **P7 — Integration:** complete visual, timing, gameplay, build, and hardware checks; update documentation.

Do not remove old entry points before their replacement is wired through callers and tests. Do not leave the old low-resolution path as a runtime fallback in the final implementation.

## 4. S1 — Correct the title's final scanline

### Files and changes

| File / current lines | Modification |
| --- | --- |
| [snes_scene.c](src/snes/snes_scene.c), 349–390, `title_upload()` | Write the title BG1 vertical offset as the calibrated minus-one value (`0x03FF`, low byte then high byte). Test BG2 separately: align its intended text rows using the same screen-coordinate convention, rather than shifting it blindly. |
| [snes_scene.c](src/snes/snes_scene.c), 393–407 and 754–773 | Explicitly initialize title map/prompt/plate state on entry; ensure subsequent prompt/menu uploads preserve the corrected scroll. Keep visible writes inside the owned VBlank window. |
| [snes_video.c](src/snes/snes_video.c), 258–300, title window construction and plate enable | Check the plate's first and last visible lines after the scroll fix. Seed and reset window/color-math state consistently on attract/menu transitions and entry from another scene. The plate must stop at its intended rectangle. |
| [gen_snes_scenes.py](tools/snes/gen_snes_scenes.py), 605–661 | Retain all 28 artwork tile rows. Add asset assertions for map dimensions and tile-index bounds. Change generated padding only if separately justified; padding the bottom with black would conceal the indexing error. |
| [verify.py](tools/snes/verify.py), 273–315 | Extend `check_title()` and add `check_title_scanlines()` / `check_title_menu_plate()`. Decode tilemap + tile bytes + CGRAM into an expected visible image and compare the actual first and last lines. |

### Verification

- Capture attract mode with prompt on and off, menu entry, all three menu selections, menu cancellation, and return from deck/story/duel/result.
- Compare all 256 pixels on rows 0, 1, 222, and 223 against the intended generated artwork. Also compare the whole painting outside intentional prompt and menu regions.
- For menu captures, check plate bounds and actual menu text, not only the absence of a blue line.
- Inspect a continuous capture around mode changes for one-field offsets or color-math leakage.
- Check story/ending alignment if changing a helper shared with those scenes. Prefer a title-local fix unless broader evidence warrants more.

**Exit gate:** the baseline ROM fails the new line test; the fixed ROM passes it and the existing title/input tests. The final line contains the real image content, not a substituted black line.

## 5. P2 / S2 — Own VBlank and repair selected-card rendering

### A. Establish one presentation owner and one budget

The framebuffer drain currently executes inside NMI, while custom OBJ/card-art work executes after `WaitForVBlank()`. The comment claiming OBJ goes first in [snes_main.c](src/snes/snes_main.c), lines 173–180, does not describe that actual order. The stock runtime calls its custom handler after its own OAM handling ([vblank.asm](SNES/pvsneslib/pvsneslib/source/vblank.asm), lines 649–695).

| File / current lines | Modification |
| --- | --- |
| [snes_main.c](src/snes/snes_main.c), 126–187 | Route visible PPU work through one SNES presentation dispatcher. Replace the ambiguous `Mode3Active` ownership check with explicit BOARD / CARD_CHECK / BATTLE state. Preserve title/story/deck handling while applying the same budget discipline. |
| [snes_fb.asm](src/snes/snes_fb.asm), 158–205, 255–313, 563–767 | Add bounded suspend/reset/drain semantics and an epoch for queued work. Reject stale jobs after a scene switch. Account for remaining VBlank time before each transfer, including setup and other owners' reservations. |
| [snes_fb.inc](src/snes/snes_fb.inc), 1–25 | Replace independent assumptions about `FB_BUDGET` with a documented total allowance and reserves for OAM, card palettes, map commits, HDMA setup, and safety margin. The present 3072-byte value is a baseline, not proof of safety. |
| [snes_obj.c](src/snes/snes_obj.c), 327–375 and 401–478 | Prepare immutable transfer descriptions outside VBlank. The uploader consumes descriptors and remaining allowance; it does not decide its allowance merely from whether board work is pending. |
| [snes_duel.c](src/snes/snes_duel.c), 959–1003 | Before card check/battle takes VRAM: stop producing board jobs, finish or cancel them under a defined epoch, disable the drain, then load art. On return, restore resources and a complete board picture before publishing it. |
| [snes_cardart.c](src/snes/snes_cardart.c), 64–107, 303–332 | Use the presentation owner's budget. No obsolete board jobs or duel OBJ-cache uploads may write into the art screen. |

Implement the bounded transfer core in SNES assembly or another demonstrably bounded path. Do not simply call large existing C routines from NMI: they can use compiler scratch registers shared with interrupted C. Audit D, DBR, accumulator/index widths, saved registers, compiler temporaries, and stack use. Keep scene construction, loops over maps, animation logic, and palette decisions outside the interrupt.

Use front/back descriptors or a completed flag written last so NMI cannot observe partially built OAM or commands. Conversely, the producer must not reuse a ring slot, map shadow, or palette buffer while a queued DMA still references it. Keep the renderer's reserved DMA-register scratch channels 1–3 unavailable to DMA/HDMA, and keep NMI away from the foreground multiplier registers.

### B. Isolate and fix the hand sprite

| File / current lines | Modification / audit |
| --- | --- |
| [snes_obj.c](src/snes/snes_obj.c), 19–62 | Treat face, sheet, complete row count, and palette generation as a residency record. Verify `CARD_TILE()` / `CARD_WORD()` against the 16-tile-wide OBJ addressing layout. |
| [snes_obj.c](src/snes/snes_obj.c), 160–187, 305–375 | Inspect OAM coordinates, name high bit, size, palette, priority, and flips. Keep a replacement sprite hidden until all four rows and the matching palette are complete. Restart partial uploads if the requested face/sheet changes. |
| [snes_obj.c](src/snes/snes_obj.c), 401–478 | Capture and compare each of the four 128-byte rows and 32-byte palettes. Verify the last row really finishes in VBlank. Make gray-to-color changes palette-only and prevent stale `next_palette` work surviving a partial-upload early return. |
| [snes_duel.c](src/snes/snes_duel.c), 1411–1449 | Keep selection bob, outline, gray state, and flight coordinates in the same presentation snapshot. Verify outlines do not overwrite card interior pixels through OAM priority. |
| [gen_snes_obj.py](tools/snes/gen_snes_obj.py), 271–354, 680–701 | Check generated tile order and per-face gray/color palettes only if VRAM equals the generated bytes but the decoded asset itself is wrong. Regenerate; do not hand-edit blobs. |
| [verify.py](tools/snes/verify.py), 1174–1192, 1269–1304, 1363–1383, 1510–1562 | Extend the hand test to identify the actual OAM sprite first, compare VRAM/CGRAM residency, and produce a mismatch mask. Preserve strict art checks; model transparent pixels and intentional overlays correctly. |

Diagnostic decision tree:

1. If VRAM rows differ from the requested generated face, fix transfer lifetime, bank/offset arithmetic, row destination, or VBlank overrun.
2. If VRAM matches but CGRAM differs, fix palette identity, ownership, or upload ordering.
3. If both match but the screenshot differs, inspect OAM, sprite evaluation limits, overlapping sprites/windows, and timing of the capture.
4. If the expected image treats a transparent or deliberately occluded pixel incorrectly, correct the comparison only for that documented condition. Do not relax the threshold to make 959/1024 pass.

### C. Include the held 3D card

The report calls it a sprite, but also cover the separate held-card rendering path so a hand fix does not leave a second selected-card glitch.

- [snes_duel.c](src/snes/snes_duel.c), `draw_held_card()`, lines 780–840: restore the previous footprint on **every** exit path, including cancellation, invalid selection, out-of-view bounds, and an oversized backup. At present the size check at 826 can return after restoring RAM without publishing that restoration.
- Keep the backup keyed to camera pose and board generation. Invalidate/rebuild it when a card below it changes or a presentation takes VRAM.
- Verify the union of old/new dirty cells, including old-only cells, and do not leave the old marker in the saved base.
- [snes_board3d.c](src/snes/snes_board3d.c), 476–529 and 554–679: test quad bounds, affine gradients, clipping, and texture coordinates.
- [snes_raster.asm](src/snes/snes_raster.asm), 496–601: test both 32×32 texture banks and edges of a face. [snes_fb.asm](src/snes/snes_fb.asm), 323–377: guard zero/odd rectangle dimensions according to the actual API contract and ensure the 64×64 backup cannot overflow.

### Tests and gate

- Select each hand slot; run a complete bob cycle; move rapidly between slots while uploads are pending.
- Select during another card's four-row upload; replace a slot mid-upload; change gray state without changing its face.
- Exercise draw, hand removal, fusion queue changes, placement flight, card-check entry/return, overhead entry/return, and consecutive duels.
- Capture all fields during a stressed board upload. Verify all five visible hand cards against their requested faces and palettes.
- Test held cards over empty and occupied slots at both seats, every relevant bob phase, and texture IDs 63/64 and the final/back face. Cancel at each phase and compare restored pixels with the saved base.
- Decode per-scanline OBJ and tile-sliver counts, including cursor and HUD. Read overflow flags where the emulator exposes them.

**Exit gate:** no unexplained card-pixel mismatch, palette flash, stale footprint, queue race, or write outside the permitted display interval. Captures must show the correct card, not just a non-black rectangle.

## 6. S4 — One full-resolution renderer with safe publication

### A. Resolve the VRAM constraint before enlarging any buffer

A dense 256×144 8bpp picture occupies 36,864 bytes. Two such pictures alone occupy 73,728 bytes, before maps and sprites. Changing `SNES_MOVING_W/H` to 256/144 while retaining the old pools cannot work.

**Planned design: a sparse 8bpp tile allocator with two complete tilemaps and copy-on-write updates.** Keep the 256×144 sampling grid and current direct-color board format. Empty surround cells reference one permanent blank tile; allocate physical tiles only for cells touched by board geometry, markers, and raised cards. Use conservative coverage, not a lossy image heuristic.

Proposed duel VRAM layout, in **word addresses**:

| Words | Bytes | Owner |
| --- | ---: | --- |
| `$0000–$57FF` | 45,056 | 704 8bpp tiles: permanent blank 0, allocatable 1–702, reserved tile 703 |
| `$5800–$5BFF` | 2,048 | BG1 map A, 32×32 |
| `$5C00–$5FFF` | 2,048 | BG1 map B, 32×32 |
| `$6000–$7FFF` | 16,384 | Existing duel OBJ sheet |
| **Total** | **65,536** | Exactly the VRAM capacity |

The guaranteed budget is **at most 351 occupied cells per completed board frame**: even two completely disjoint frames fit in 702 allocated tiles. Existing rest-floor occupancy is 236; all camera positions and raised-card bounds still need proof. Test conservative raster coverage, not only the four corner bounding rectangle or screenshot non-black counts.

Before implementation, add an offline camera/coverage sweep. Include every discrete turn/lift pose, both seats, all legal raised-card positions and bob extrema, marker extents, wall faces, and near-plane clipping. If any frame exceeds 351 cells, adjust camera distance/height smoothly and regenerate its projection tables while preserving 1:1 sampling and readable cards. Do not crop the board, drop cells, halve sampling, or silently overwrite front-frame tiles. Treat failure to fit an acceptable camera composition as a failed design gate, not a runtime fallback.

The whole-screen converter test pattern can exceed the sparse scene budget. Test that diagnostic in batches under blanking or through a diagnostic-only layout; do not weaken the production allocation invariant to accommodate it.

Preserve these observed WRAM allocations unless the linker audit justifies moving them:

| Address / allocation | Planned use |
| --- | --- |
| `$7E7000–$7EFFFF`, 36,864 bytes | The single full-resolution chunky working image; a second full chunky framebuffer is unnecessary. |
| `$7F0000–$7F7FFF`, 32,768 bytes | Existing world-space floor/card texture; retain the base required by the DDA or update that contract explicitly. |
| Former `$7F8000–$7FA3FF`, 9,216 bytes | Reuse the deleted motion buffer's space for map shadows and allocator metadata. Initial allowance: maps 4,096 bytes, free tile IDs 1,404, reference counts 704, retirement IDs 702, dirty masks 144, row spans 72; 7,122 bytes total, leaving 2,094 for bounded transaction state. |
| `$7FA400–$7FB3FF`, 4,096 bytes | Existing held-card backup, with explicit bounds and generation ownership. |
| Low RAM `$0400–$13FF`, 4,096 bytes | Existing conversion ring; preserve stack separation and measure the remaining stack margin. |

These are allocation contracts to verify against the rebuilt `.sym`, not permission to alias other C/statics. Remove the obsolete map/doubler allocations as their replacement lands, and account for the compiler's other sections. Reserve tile zero permanently; reference counts for other tiles must reflect both maps, including cancellation and retirement. Initialize all new static state explicitly because this runtime's BSS-clear assumptions have already caused failures.

### B. Completed-frame transaction

1. Snapshot camera, field faces, marker, and raised-card pose for a render generation.
2. Build the inactive logical map from the current map. Unchanged cells retain their physical tile references.
3. For changed cells, allocate unused physical tiles. Restore and render the corresponding full-resolution pixels; convert into the existing immutable upload ring. New empty cells reference tile zero.
4. DMA new tiles and the inactive map over as many VBlanks as required. Neither is visible yet. Preserve the old complete frame and responsive HUD meanwhile.
5. After all tile/map jobs for that generation complete, change BG1SC in one VBlank and acknowledge the generation as presented. Publish camera-dependent OAM/cursor coordinates at this same boundary.
6. Retire tiles referenced only by the previous map **after** that acknowledgment. Keep shared tiles alive. Do not reuse the inactive map shadow until its DMA finishes.

For a cursor or held-card patch, allocate only changed cells; unchanged cells are shared across maps. This avoids converting the entire board for a small change. For a camera move, all occupied cells can change and still fit the two-frame bound. Keep only one pending board transaction initially; adding a third would invalidate the proof.

This design removes the motion/rest pool-parity rule and progressive sharpening entirely. Slow frames repeat the last complete full-resolution image. They do not expose half-written tiles or mixed camera generations.

### C. Source changes

| File / current lines | Modification |
| --- | --- |
| [snes_video.h](src/snes/snes_video.h), 36–98, 120–140 | Define one 256×144 render target and the new VRAM/map/allocator constants. Replace resolution/pool APIs with render generation, presented generation, and camera-view APIs. Remove doubler/motion-job definitions after caller migration. |
| [snes_video.c](src/snes/snes_video.c), 132–160, 357–418, 432–484 | Remove BG1VOFS doubling and top-screen register switching. Set a calibrated fixed BG1 scroll; retain the HUD gradient and, if useful, its BG1 band mask. Apply map switches only for completed transactions. |
| [snes_fb.asm](src/snes/snes_fb.asm), 80–147, 255–313, 432–557, 563–767; [snes_fb.inc](src/snes/snes_fb.inc) | Replace motion maps/doubler/group jobs with two full map shadows, tile-allocation metadata, and explicit frame-commit jobs. Retain ring producer/consumer separation. Delete the 9,216-byte motion framebuffer; preserve the full-resolution framebuffer and backup. |
| [snes_conv_drivers.inc](src/snes/snes_conv_drivers.inc), 216–411, 412–end | Delete the motion converter driver. Change rest conversion to accept destination tile IDs/runs from the allocator instead of deriving destination from fixed `cell + 1`. Keep ROM-floor tile bypass for unchanged camera poses. |
| [gen_snes_conv.py](tools/snes/gen_snes_conv.py), 37–94, 97–126 | Emit a single 1:1 converter family. Start with the already-tested rest converter. Optimize this generator, then regenerate `snes_conv_gen.asm`; never edit the generated routine directly. |
| [gen_snes_planar.py](tools/snes/gen_snes_planar.py), 38–59, 85–104, 137–225, 228–end | Remove doubled-pixel lookup data after its last caller disappears; retain/rework full-resolution floor assets, palette conversion, and generated bounds. Add shared camera descriptors and conservative coverage limits. |
| [snes_duel.c](src/snes/snes_duel.c), 163–166, 242–264, 299–334 | Remove `vp_motion`, SHOWN_MOTION state, next-pool parity, and progressive flags. Store camera pose and board-generation validity independently of resolution. |
| [snes_duel.c](src/snes/snes_duel.c), 602–770, 843–951 | Convert bake/patch/upload and motion functions to the same full-resolution transaction system. Camera changes use complete new maps; stable-camera changes use cell diffs. |
| [snes_duel.c](src/snes/snes_duel.c), 1815–1848, 1959–1988, 2041–2076 | Remove reduced-resolution debug selection and rendering dispatch. A force-render fixture may remain, but it must exercise the production 1:1 path. |
| [snes_stamp.h](src/snes/snes_stamp.h), 20–48; [snes_stamp.c](src/snes/snes_stamp.c), initialization/commit | Version the stamp and replace ambiguous pool/resolution fields with viewport size, camera pose, frame generation, occupied/dirty cells, and presentation timings. Update the Python decoder in the same change. |
| [Makefile.snes](Makefile.snes), 79–85, 186–191, 272–291 | Remove obsolete generated motion artifacts from explicit lists and wildcard inclusion. Make new tables, includes, and binary outputs rebuild their consumers. |

### D. Fix geometry assumptions, not just dimensions

- **Coordinate representation:** [snes_board3d.c](src/snes/snes_board3d.c), lines 52–103, uses an internal half-size coordinate domain so Q8.8 screen values fit signed 16 bits. Full-resolution sampling can retain that internal domain (`sub=1`). Do not double the signed Q8.8 focal length from 64 to 128: `128 << 8` does not fit a positive `s16`.
- **Inverse projection:** lines 628–654 explicitly assume the 128-wide motion viewport. Derive screen-ray scale and texture step for the 256-wide pixel grid; remove literal `*4` / `>>6` assumptions where inappropriate. At a 128-pixel effective focal length, per-pixel ray increments differ from the old path. Verify the result against forward projection.
- **Frame clearing:** lines 694–698 initialize nine occupancy rows and clear `SNES_MOVING_W * SNES_MOVING_H`. Replace these with 18 full-resolution cell rows and the actual target bounds; otherwise old pixels survive below/right of the old buffer extent.
- **Destination bank:** [snes_raster.asm](src/snes/snes_raster.asm), lines 615–620, hardcodes WMDATA destination bank `$7F`. Honor the selected destination bank or provide a dedicated `$7E` full-resolution writer. The current `snesRasterTarget()` does not change that hardcoded write.
- **Flat cards during camera movement:** `motion_frame()` changes stamped card width/height with pitch ([snes_duel.c](src/snes/snes_duel.c), 861–892). Replace this camera-dependent texture morphing. Cards should have fixed world-space footprints and consistent faces across camera poses. A cached floor-plus-flat-cards texture is acceptable if it matches the projected geometry; raised cards and effects still use actual projected quads.
- **Card sampling:** retain the 32×32 face sources for the full-resolution path. Update `snesBoardTextureCard()` ([snes_raster.asm](src/snes/snes_raster.asm), 689–825) if using a composite texture; do not silently continue sampling only the 16×16 motion sheet. Match rest and moving orientation, UV footprint, face-down backs, and empty slots.
- **Clipping:** test left/right edges, horizontal edges, degenerate quads, near-plane intersections, negative increments, and intermediate yaw quadrants. At lines 711–712 of `snes_board3d.c`, check failed wall-vertex projections before using their output.
- **Uncovered pixels:** blank all cells vacated by a moved object, even if the new camera's occupied span is smaller. Include the union of old/new footprints in dirty tracking.

### E. Optimize only the surviving path

Measure mapper, cards, converter, queue waiting, DMA completion, and visible presentation separately. Existing `render_lines` excludes some backlog and does not establish when the new picture reaches the screen.

Optimization order:

1. Sparse cells and stable-camera dirty patches; never convert the empty surround or redraw an unchanged board.
2. Reuse ROM planar floor tiles for supported stable poses and update the world texture only when faces change.
3. Precompute camera/per-row coefficients for the finite turn/lift sequences; keep arithmetic per row or vertex, not per texel.
4. Add constant-V and signed U/V DDA specializations where measurements justify them. Test carry/borrow and texture wrapping before benchmarking.
5. Benchmark a **non-doubling** pair-table converter against the current full-resolution single-texel LUT. Reclaim the old 512 KiB doubled-pair tables; account for the replacement's banks before emitting it. Select the faster verified implementation, not the larger table by assumption.
6. Batch contiguous uploads and bounded conversion work. Make rendering resumable at row/tile boundaries so OAM/HUD/input preparation runs every field while a board transaction is pending.

Do not optimize by dropping pixels, using a smaller motion viewport, displaying incomplete frames, or lowering test thresholds. Preserve PPU multiplier ownership and the compiler/bank calling convention.

**Performance gates:** establish fresh measured baselines first. Initial acceptance targets are a completed camera frame within 12 NTSC fields, a cursor/single-slot patch within 3, and a held-card update within 5. These are engineering targets to verify, not measured promises. Report p50/p95/max and complete turn/lift duration; passing an average while a worst case stalls is insufficient. If these targets are missed, continue profiling the 1:1 path and report the remaining bottleneck explicitly.

### F. Renderer verification

- Exhaustively check byte-value/bit-position conversion, all four ring-slot phases, and any replacement pair-table bank split against an independent bitplane encoder.
- Validate 1:1 output with patterns containing one-pixel horizontal and vertical changes. Natural card art alone cannot prove that doubling is absent.
- Capture every field of turn, lift, held-card motion, placement, cancellation, and scene restoration. Each board image must belong to a completed generation; no visible tile may belong to an unpublished or recycled generation.
- Stress full job/ring queues, counter wrap, late NMI, cancellation, and new scene entry while conversion is pending. No deadlock; no partial descriptor; no zero-length DMA interpreted as a full-bank transfer.
- Compare forward and inverse projection for known world points and all twenty slots. Test both seats, all supported camera poses, face-down, empty, support/equip, and populated fixtures.
- Verify peak physical allocations, frame occupancy ≤351, map tile bounds, WRAM section bounds, stack high-water mark, ROM bank ranges, and no changes to OBJ tiles during board uploads.
- After cancel/return/round trip, compare the restored image and logical field to their originals, allowing only documented animation phases.

## 7. S3 — Overhead inspection as an in-game camera pose

### Changes

| File / current lines | Modification |
| --- | --- |
| [snes_duel.c](src/snes/snes_duel.c), 359–471 | Replace queueing twenty top-view OBJ cards and switching to a resident map with interpolation of the production 3D camera. `finish_view_transition()` selects a camera state, not a different PPU display. |
| [snes_duel.c](src/snes/snes_duel.c), 503–521, 1124–1203 | Preserve logical row/column inspection and hidden-card information rules. Project the cursor through the active camera; use top-row/top-column selection when overhead. |
| [snes_duel.c](src/snes/snes_duel.c), 1338–1380 | Remove the twenty field-card sprites and hardcoded `TOP_CARD_X/Y` positions. Keep readable HUD/stat information in the normal HUD band; draw the board cursor through the 3D marker path. |
| [snes_duel.c](src/snes/snes_duel.c), 1876–1903, 1951–1988, 2041–2048 | Remove the “top view renders nothing” branches. Cursor motion marks relevant projected cells dirty. Preserve the current inspection controls: UP enters; d-pad navigates; A checks; B returns. |
| [snes_obj.c](src/snes/snes_obj.c), 84–107; [gen_snes_obj.py](tools/snes/gen_snes_obj.py), 601–678, 727–end | Remove resident top-map uploads and unused top-view assets/header exports after their last consumers are migrated. Preserve clustered card assets needed by the deck editor. |
| [snes_video.c](src/snes/snes_video.c), 445–461 | Remove the switch to top characters, top tilemap, and indexed BG1 color. Both camera poses use the same 8bpp direct-color board display and map publication mechanism. |
| [gen_snes_planar.py](tools/snes/gen_snes_planar.py), camera/bounds generation | Generate an overhead resting pose and its bounds, or use the generic cached full-resolution render. Keep floor, cards, and marker on one projection model. |

At exact overhead pitch, validate the horizontal-plane intersection explicitly; avoid a horizon-based division that becomes invalid at the endpoint. Keep the board and card footprints continuous during the final approach. The overhead image may fit fewer physical screen pixels than the old 240×192 table because it now occupies the 256×144 board viewport; preserve readability and document the chosen camera composition in captures.

Inspection remains an inspection state; this request does not require changing gameplay controls to play cards from overhead. It must display current game state and preserve the prior UI/cursor when returning or closing a card check.

### Gate

- At every intermediate and settled pose: 256×144 at 1:1, BGMODE 3, direct color on, the same character base, and only completed board map generations.
- All 20 logical slots project to their correct places; empty slots remain selectable. Opponent face-down cards do not reveal hidden details.
- A changed logical field changes the overhead board image, proving it is not a leftover static table.
- Repeat hand→overhead→check→overhead→hand and enter/exit from placement and attack selection. No lost selection, duplicated input, popping to the old table, hand-cache corruption, or unnecessary full-screen black frame.
- Rewrite `check_top_view()` at [verify.py](tools/snes/verify.py), lines 1654–1737, to match projected geometry and texture art rather than requiring 20 OAM card sprites. Extend lines 1740–1867 for continuous transitions and card-check restoration.

## 8. S5 — Mode 4 battle with independent vertical lanes

### A. Hardware model and layout

Mode 4 provides 8bpp BG1, 2bpp BG2, and BG3 offset data. Its vertical offset entries retain pixel precision; horizontal per-tile offsets lose their bottom three bits. Encode a vertical BG1-only offset as `0xA000 | (vofs & 0x03FF)`; leave the BG2-enable bit clear for stationary text. [Fullsnes register specification](https://problemkaputt.de/fullsnes.htm#snesppubgmaps).

The first visible tile column is not affected by offset-per-tile. The vendored renderer also demonstrates the one-column lookup displacement and vertical selection ([bg.cpp](SNES/snes-headless/src/snes/src/ppu/render/bg.cpp), lines 110–139). Reserve screen x=0–7 as blank and start the left battle lane at x=8. Prove this with a small stripe-pattern test before placing real art.

Planned battle-only VRAM layout, word addresses:

| Words | Bytes | Owner |
| --- | ---: | --- |
| `$0000–$3FFF` | 32,768 | Two existing 8bpp big-card sheets, shared feet, permanent blank tile; assert generated end address |
| `$4000–$5FFF` | 16,384 | Battle/direct-attack OBJ effects atlas; duel OBJ cache is inactive |
| `$6000–$67FF` | 4,096 | BG1 card map, **32×64** |
| `$6800–$6BFF` | 2,048 | BG2 stationary text map, 32×32 |
| `$6C00–$6FFF` | 2,048 | BG3 offset map; one active 32-entry row |
| `$7000–$71FF` | 1,024 | 64-glyph **2bpp** battle font |
| `$7200–$7FFF` | 7,168 | Reserved until an explicitly budgeted use exists |

A 32×64 BG1 map gives a 512-pixel vertical period. A 256-pixel period is too short to move a 160-pixel card fully off a 224-line display without it wrapping back into view. Initialize all unused map rows to the real blank tile.

Use battle card columns 1–15 (x=8–127) and 17–31 (x=136–255), with BG1 horizontal scroll zero. BG3 offset columns are shifted by the hardware fetch rule: with zero fine H scroll and BG3 H scroll zero, visible tile column `c>=1` uses offset entry `c-1`. Fill the gutter/unused entries explicitly. Test all 32 entries rather than assuming column alignment.

Apply one vertical offset to the entire width of each lane. Player card starts above the viewport and moves **down**; COM card starts below it and moves **up**, irrespective of who initiated the attack. The existing code always assigns attacker to the left slot; replace that with physical player/COM lane assignment while separately storing attacker/defender identity.

Use the existing `screen line = map line - scroll - latch` convention to compute offsets from the desired card top, and verify the sign/latch with markers. Keep BG2 text stationary initially. If stats must travel with cards later, add that deliberately with matching offsets and timing rather than accidentally applying BG1's offsets to BG2.

### B. Separate card check from battle

| File / current lines | Modification |
| --- | --- |
| [snes_cardart.c](src/snes/snes_cardart.c), 12–43, 64–107, 134–153, 297–332 | Preserve card-check Mode 3 behavior. Share the big-card loader, but introduce explicit battle initialization with the Mode 4 layout and offset-row updater. Remove the horizontal-window reveal from the battle caller. |
| [snes_cardart.h](src/snes/snes_cardart.h), 24–60 | Separate card-check coordinates/API from battle lane coordinates/API. Do not retime or relocate card check merely to support battle. |
| New `src/snes/snes_battle.c` / `.h` | Own battle snapshot, phases, vertical offsets, outcome effects, displayed LP values, cue events, skip policy, and completion. Keep rendering state separate from duel rules. |
| New `src/snes/snes_battle_video.asm` or bounded equivalent | Upload the prepared 64-byte offset row and small text/OAM updates through the VBlank dispatcher. Define and preserve its interrupt/compiler ABI. |
| [snes_duel.c](src/snes/snes_duel.c), 1043–1074, 1325–1333, 1720–1729, 1851–1858, 2026–2037 | Replace the 90-iteration reveal/hold with the battle sequencer. Route player and AI attacks through the same snapshot and lifecycle. |
| [snes_main.c](src/snes/snes_main.c), 173–180 | Dispatch battle presentation explicitly. Do not service duel card-cache uploads while battle owns its VRAM/CGRAM regions. |
| [gen_snes_scenes.py](tools/snes/gen_snes_scenes.py), 373–447, or new `tools/snes/gen_snes_battle.py` | Generate a real 2bpp font from the existing glyph source. Mode 3's 4bpp font cannot simply be reinterpreted as Mode 4 BG2 data. Emit suitable palette mapping and a preview. |
| [Makefile.snes](Makefile.snes), 50–85, 202–236, 265–267 | Add all new asset outputs and generation dependencies, including first-build assembly discovery. Add Mode 4 test targets/check registration. |

Preserve the existing two 80-color card palettes where possible. Current art uses indices 32–111 and 160–239; frame/text occupy low entries and repeated frame colors at 128–143. Reserve effect OBJ palettes explicitly, for example 144–159 and 240–255, and verify they do not alter either card. Repack the 2bpp text into four-color palette groups instead of retaining 4bpp assumptions.

### C. Battle timeline and outcomes

Prepare art once before the visible sequence; after that, move cards by offsets rather than retransmitting their pixels. Begin timing at the first displayed battle frame, not before asset loading.

Proposed normal battle sequence, measured in displayed NTSC fields:

| Fields | Beat |
| --- | --- |
| 0–19 | Player lane enters downward; COM lane upward; eased motion with one-pixel precision. |
| 20–27 | Short readable hold with the actual attack/defense figures. |
| 28–43 | Attack anticipation and contact/recoil; defender response follows the recorded outcome. |
| 44–67 | Damage/readout and appropriate destroyed-card fade, tie, or defense result. |
| 68–83 | Exit/settle and restore the board. |

These timings are initial authored values to compare on video with PC/SDL3 and the user's desired pace. Enforce a 60 Hz update opportunity; do not make a nominal 84-field sequence last 84 expensive game-loop iterations.

Copy the rules' attack event before clearing it: attacker/defender owners and faces, stance, effective values, damage, destruction/outcome, and LP before/after. Use the already-resolved rules result exactly once. Display LP may count toward the final value, but the visual sequence must not apply damage again. A failed/blocked attack must not create a false successful-impact sequence.

Use elapsed display fields and cue-crossing detection, so late frames do not lose or duplicate sound/impact events. Consume held/entry A/B/START edges; skipping after the minimum readable interval must restore one consistent final state and clear the event once. Handle both lethal and nonlethal outcomes before result/story transitions.

### D. Mode 4 tests

- First run a synthetic two-lane stripe image through offsets 0–15, negative values, and wrap boundaries. Assert motion in 1-pixel steps, correct sign, all lane columns together, and an unaffected first-column gutter.
- Verify BGMODE=4, BG1 map size 32×64, intended map bases, BG2 2bpp text, BG3 offset encoding, and direct color **off** for indexed big-card art.
- Match the actual player and COM paintings at their animated positions on every captured field; compare against each lane's expected crop during entry/exit. Check no wraparound copy appears on the opposite edge.
- Verify full colored art, transparent/background cells, frame feet, stats, and text priority. No full-map or full-card DMA in the steady animation loop.
- Measure visible updates and total duration. Target one update per field throughout vertical motion; unexplained multi-field pauses fail even if endpoint images are correct.
- Exercise player and COM attacks, attack/defense stances, modified stats, face-down reveal, stronger/weaker attacker, tie, destruction, blocked/trap response, nonlethal/lethal damage, and back-to-back battles.
- Stress entering battle with pending board jobs and returning with pending battle jobs. Repeat inspection/battle/duel transitions and confirm complete restoration of maps, palettes, sprite residency, scrolls, windows, color math, and HDMA.
- Replace `check_battle_art_mode3()` ([verify.py](tools/snes/verify.py), 1869–1916). Its current acceptance of independently matching cards somewhere near the end is insufficient; new checks must establish identity, movement, order, pacing, and outcome.

## 9. S6 — Elaborate direct attack inspired by PC/SDL3

### Reference behavior

Read the actual PC path, not only the older console slash:

- [src/main.c](src/main.c), lines **295–324**: PC-specific direct-attack timing, 72-field FX beat, contact calculation, and damage-text start.
- [src/main.c](src/main.c), lines **8493–8606**: software description of the blade, whiteout, core, shock ring, rays, cooling, and damage punch-in.
- [src/main.c](src/main.c), lines **8609–8660**: the shipped hardware-effects/text dispatch.
- [src/main.c](src/main.c), lines **15330–15424**: slide/lunge choreography, target-relative impact point, and sound synchronization.
- [impact.frag](src/platform/sdl3/shaders/impact.frag), lines **16–24, 38–55, 75–end**: actual SDL3 burst layers and warm color progression.

The usual headless build does not compile the PC-only `WAIFU_PLATFORM_HW3D` timing/effect path. A generic headless direct-attack recording therefore is not proof of visual agreement with SDL3. Use an SDL3 recording for that comparison, and read the shader/source when matching its phases. The existing [direct-attack script](scripts/direct_attack_animation_test.txt) can be a starting point, but replay and confirm it actually reaches a legal direct attack.

### SNES implementation

Use the P5 sequencer and vertical player/COM lane convention, with only the attacker card present. Give direct attacks their own event type and timeline; do not present them as a normal two-card battle with a blank defender.

| Phase | Intended SNES presentation |
| --- | --- |
| Entry / anticipation | Attacker enters its player/COM lane, briefly settles, then accelerates toward a target point along its vertical attack direction. |
| Blade | Bright warm sweep reaches the eventual impact point. Use prepared tile/metasprite animation and a small precomputed motion table. |
| Contact | Brief white/warm flash at the lunge's deepest point, paired with the hit cue. Avoid timing it several fields after the card has already recoiled. |
| Burst | Expanding ring, hot core, directional rays, and sparks; warm colors cool toward red and ember. Darken the background behind the burst with controlled color math/windowing. |
| Damage | Large outlined digits punch in, settle, and hold while the displayed LP counts to the already-resolved value. |
| Decay / return | Core and sparks fade; restore the board once. A lethal hit proceeds to the proper result after the visual event finishes. |

Initial direct timeline should retain the PC's 20-field entry, 32-field lunge, and 72-field overlapping FX beat, with contact and readout derived from named constants. The PC calculation gives contact at local field 39, FX start at 31, and damage text at 59 before any optional prelude/reveal. Adapt spatial direction to the SNES vertical lanes; keep those relationships explicit in generated timing data.

Add direct-effect state to new `snes_battle.c/.h`, and new `tools/snes/gen_snes_battle_fx.py` for deterministic tile/metasprite variants, easing curves, spark positions, and digit sizes. Add generated exports and banks through `Makefile.snes`; generated previews and atlas occupancy must be inspectable.

Use hardware movement, palette changes, prepared tiles, and sparse small tile updates. The SNES does not scale ordinary OBJ sprites, so precompute the required ring/core/digit sizes or build them from resident tile variants. Do not try to execute the SDL shader or redraw its discs/rays over the 3D framebuffer every field.

Budget each effect frame for total OAM entries, objects per line, and tile slivers per line. Large rings should use sparse perimeter pieces rather than a densely tiled transparent square. Keep the large damage readout within the same budget as the burst, with deliberate priority. Optional extra rays/sparks may be reduced to meet the budget; the mandatory blade/contact/burst/readout phases may not disappear.

Use the SNES limits of 128 OAM entries, 32 objects per scanline, and 34 eight-pixel slivers per scanline as ceilings, with margin for HUD/text and the actual sprite-evaluation order. Verify the counts on the composed effect frame, including transparent portions of large sprites. [Fullsnes sprite specification](https://problemkaputt.de/fullsnes.htm#snesppuspritesobjs).

Use the resident audio API ([snes_audio_driver.c](src/snes/snes_audio_driver.c), 81–87), scheduling laser/hit cues at their visual phase boundaries. Inspect existing effect IDs in `snes_audio.h` first. If new SFX are necessary, route them through the SNES audio generator and run the dedicated audio checks; do not modify shared WAV files merely to retime an SNES cue.

### Gate

- Separate deterministic direct-attack fixtures for each owner, several damage amounts, zero/blocked outcomes where applicable, and a lethal hit.
- Verify no defender artwork appears and that the correct attacker's identity survives the full sequence.
- Capture every field; prove visible blade → contact → ring/core/rays/sparks → readable damage → decay/return ordering.
- Read damage digits and final LP from pixels/state; ensure the displayed amount matches the rules event and is applied once.
- Measure cue alignment, phase duration, OBJ limits, transfer budget, and absence of rendering stalls. Compare a synchronized SDL3/SNES video or contact sheet, allowing hardware-specific rendering but preserving the elaborate choreography.
- Test skip/held buttons and a scene change at phase boundaries. No stuck white/black screen, lingering color math, stale sprites, repeated sounds, or interrupted result state.

## 10. Test harness, measurements, and build integration

### Extend existing infrastructure

| Existing area | Required change |
| --- | --- |
| [verify.py](tools/snes/verify.py), 39–49, 162–179 | Version and decode the revised stamp. Record displayed-frame generation/pose and battle event/phase, not just the last requested UI. Reject torn or wrong-version records. |
| [verify.py](tools/snes/verify.py), 115–151 | Add subprocess timeouts and named/selectable checks. Keep isolated SRAM. Cache by ROM identity and scenario, and ensure expected images, stamps, and PPU snapshots refer to the same field. |
| [verify.py](tools/snes/verify.py), 495–614 | Replace assumptions about 128×72 movement. Keep distinct still/moving scenarios but require 256×144 output for both. |
| [verify.py](tools/snes/verify.py), 802–830 | Turn the timing report into assertions for separate measured operations. A stale `render_lines` value or comparing unlike bake/patch operations must not pass as an ablation. |
| [verify.py](tools/snes/verify.py), 839–1108 | Update projection and held-card tests for the unified camera and current 32×32 faces. Use known fixture identities, not only nearest-looking artwork. |
| [verify.py](tools/snes/verify.py), 1954–1997 | Replace motion-doubler pattern checks with 1:1 converter, tile allocation, and completed-map publication tests. |
| [verify.py](tools/snes/verify.py), 1999–2023 | Validate actual map/tile/OBJ regions, `.sym` allocations, and runtime registers rather than requiring the obsolete fixed addresses and pool layout. |
| [verify.py](tools/snes/verify.py), 2026–2077 | Register every S1–S6 gate, including direct attacks. Add a fast focused selection mechanism while preserving the full-suite default. |

Suggested new host checks under `tools/snes/`:

- `test_video_layout.py`: generated dimensions, VRAM/WRAM/ROM ranges, asset tile indices, font formats, scene ownership, and sparse capacity.
- `test_planar.py`: independent reference encoder and converter vectors.
- `test_camera_coverage.py`: camera continuity, fixed-point bounds, clipping, and ≤351-cell proof.
- `test_battle_timeline.py`: cue crossings, owner/lane mapping, skipped fields, skip/cancel, damage once, and lethal completion.

These tests cover arithmetic/resource/state invariants; they do not replace emulator output checks. Production rasterizer and uploader changes must also be exercised on the actual 65816 binary.

For event-driven capture, extend the runtime stamp/trace and harness so the capture records the transition event and shown generation. Until that exists, record a sufficiently long field-by-field interval and identify the event from observable state and pixels. Do not keep choosing a late fixed frame until a flaky test happens to pass.

### Metrics to record

- Render request, CPU render complete, conversion complete, final DMA complete, map published.
- Display fields elapsed, map/card/conversion work, DMA bytes by owner, latest transfer completion line, skipped/deferred VBlank jobs.
- Peak allocated/occupied/dirty tiles, ring/queue high-water marks, and stack high-water mark.
- Battle/direct event ID, phase, displayed field, owner, actual damage, displayed/final LP, and cue counts.

Use a sufficiently wide monotonic field counter or explicit high/low words for longer operations. The existing `snesClock()` ([snes_math.asm](src/snes/snes_math.asm), 370–410) returns a wrapping 16-bit scanline value; distinguish elapsed timing from counter wrap and verify frame-boundary reads. Long scenes and repeated attacks must not alias to implausibly small timings.

### Commands and execution policy

Existing full SNES gate:

```sh
make -f Makefile.snes
make -f Makefile.snes banks
make -f Makefile.snes verify
```

Once added, run focused host checks before the expensive emulator scenarios:

```sh
python3 -m unittest discover -s tools/snes -p 'test_*.py'
```

The baseline reproduction used for this plan can be rerun without altering normal verification saves:

```sh
python3 -B - <<'PY'
import os, sys
sys.path.insert(0, 'tools/snes')
import verify as v
v.check_rom_fresh()
v.OUT = os.path.join(v.ROOT, 'build/snes/supernes-plan-baseline')
for label, check in [('title', v.check_title), ('hand', v.check_hand_is_per_face)]:
    try:
        print(label, 'PASS', check(), flush=True)
    except v.Failure as exc:
        print(label, 'FAIL', exc, flush=True)
PY
```

This diagnostic command deliberately reports both outcomes; it is not a CI success gate. CI/full verification must return nonzero for any failure.

Use the accurate headless Mednafen executable already selected by `verify.py`; independently repeat the new Mode 4 and publication probes on the bundled snesfaust build where its capture interface supports them. Do not infer physical behavior merely because both emulators agree. Record emulator versions, ROM hash, exact input, fresh SRAM versus persistence scenario, and capture interval.

For a clean-build check, first preserve the dirty SNES source/assets in an isolated snapshot; a checkout of HEAD alone omits the current implementation. Verify generation from missing outputs and incremental rebuilding after a generator/header/blob change. Do not clean away the owner's work or rely on stale untracked INCBIN blobs.

If implementation expands into shared gameplay/assets, read the relevant repository skill and run the relevant headless regression **before** affected console verification. For changes to shared battle/story/save flow, run:

```sh
make regression-story
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
```

Then build and visually verify every affected console using its required skill. SNES-only changes do not justify unrelated shared-code edits.

## 11. Integration matrix and definition of done

| Scenario | Observable checks |
| --- | --- |
| Cold boot / title menu | Correct first/final artwork rows, prompt blink, plate bounds, all menu routes |
| Hand and placement | Every selected card intact, gray transitions, bob, fusion markers, held-card restoration, flight identity |
| Full board / both seats | Twenty slot identities/backs/empty cells, 5×4 geometry, no clipped walls, full-resolution sampling |
| Turns and camera lifts | Only complete published frames, stable card footprints, no doubling/sharpening, measured pace |
| Overhead inspection | Same renderer, current field, projected cursor, hidden-card policy, full round-trip restoration |
| Normal battle | Mode 4, downward player/upward COM motion, 2bpp text, correct outcome/stats, display-field cadence |
| Direct attacks | Distinct elaborate sequence, correct owner and damage, synchronized hit, readable LP change |
| Scene ownership stress | Enter/leave check/battle with pending work; no writes from an obsolete scene |
| Result/story/next duel | Lethal outcome resolves once, correct music/progression, no old PPU/cache state |
| Long soak / repeat | Queue/counter wrap, no memory drift or tile leak, no stuck effects, stable saves |

Before calling the implementation complete:

- All six issues have named passing tests plus reviewed continuous visual captures.
- No 128×72 render target, doubled-pixel converter, BG1 line doubler, pool-parity sharpen path, or resident overhead screen remains in production call paths.
- The sparse occupancy/camera proof, actual linker memory map, per-scene VRAM/CGRAM layout, and bounded VBlank transfers pass their gates.
- Performance is reported from completed displayed frames, with worst cases and full animation durations. Outstanding timing failures are listed rather than hidden by the old tests.
- Card check, deck editor, title/story/ending, gameplay rules, and saves still behave correctly after repeated scene transitions.
- A clean generated build and the full SNES verification suite pass. Run the dedicated audio verification if audio assets or the audio driver changed.
- Verify the title edges, selected card, full-resolution camera movement, Mode 4 one-pixel scrolling, and direct-attack effects on physical SNES when hardware is available. Record this separately; until then label results emulator-verified.
- Update [RENDERER.md](src/snes/RENDERER.md), [HIGHCOLOR_FB.md](src/snes/HIGHCOLOR_FB.md), [SNES_PORT_PLAN.md](SNES_PORT_PLAN.md), and [SNES_MODE3_PLAN.md](SNES_MODE3_PLAN.md) to describe the final renderer and presentation ownership. Remove obsolete performance claims and stale comments such as “top view renders nothing.”

The review package should contain the source diff, generated-asset/build changes, ROM hash, test results, timing table, memory map, and before/after captures for S1–S6. A successful build or a single attractive endpoint screenshot is not sufficient evidence for these animation and publication fixes.
