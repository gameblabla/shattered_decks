# FM TOWNS Marty board-renderer optimization instructions

## Objective

Optimize the actual FM TOWNS software-rendering routines, not the compiler settings. The previous
work already established fixed-address non-PIC/non-PIE i386 code, `-O2`, section garbage collection,
and shipping LTO. Do not spend another iteration changing or re-measuring those flags.

The target is a complete moving-board frame at no more than 33.3 ms under the repository's calibrated
Marty profile, without lowering visual quality, reducing camera density, skipping rendering, or
changing gameplay timing. Treat Tsugaru timings as comparison data, not physical-hardware measurements.

Read these files completely before editing:

- `AGENTS.md`
- `.claude/skills/headless-core/SKILL.md`
- `.claude/skills/fmtowns-architecture/SKILL.md`
- `.claude/skills/fmtowns-build-verify/SKILL.md`
- `codex-session-01a04593-e72f-7d71-a2fa-6df65bf83853.md` (analysis and proposed renderer work)
- `codex-session-01a045a1-999a-7f11-a2a1-8ebdf35766f7.md` (compiler work already completed)

## Correct stale assumptions from the sessions

The current source is authoritative. In particular:

- `src/main.c` defines `BOARD_COLS` as 5 and `BOARD_ROWS` as 4.
- The board top is therefore 20 textured quads over a 6x5 projected vertex grid, not 64 quads over
  a 9x9 grid. Do not implement an 8x8-board specialization.
- `render_board()` is near `src/main.c:4951`.
- Its FM TOWNS top loop is near `src/main.c:5004-5013` and calls
  `draw_quad3d_fast_projected()` 20 times.
- `draw_quad3d_fast_projected()` is near `src/main.c:1619-1639` and calls
  `cfx_renderer3d_draw_quad_fast_affine()`.
- `cfx_renderer3d_draw_quad_fast_affine()` is near `src/engine/renderer3d.c:869-884`.
- Its generic fast walker, `cfx_draw_textured_quad_fast_affine_direct()`, is near
  `src/engine/renderer3d.c:791-867`. It builds four edges for every cell, scans all four edges on
  every row, calculates U/V increments, and calls the span filler.
- The current parked `profile board` result is invalid unless top-view retention is disabled:
  `src/main.c:13305-13313` can break out without drawing. A roughly 0.5 ms `step` means this retained
  path ran; it is not board-renderer performance.

Use function names as anchors if line numbers move.

## Files that must not be changed

Do not edit any compiler, linker, emulator, virtual-display, capture, or generated-asset code during
this task. Specifically, leave all of these untouched:

- `Makefile.fmtowns`, all boot/linker files, and all compiler/linker flags
- `fmtowns.sh`
- `tools/fmtowns/headless_shot.sh` and `tools/fmtowns/run.sh`
- `FMTOWNSCD_EXAMPLE_Cube/**`, including Tsugaru/TOWNSEMU sources and binaries
- Xvfb, virtual Xorg, system Xorg configuration, or display startup scripts
- `src/generated/**` and `assets/generated/**`
- FM TOWNS CD-ROM, CD-DA, audio, input, boot, CRTC, and VRAM-presentation routines

The Xvfb capture path is already the required runner. Use it exactly through `./fmtowns.sh`; do not
"fix", replace, instrument, or rewrite it. If it reports `Cannot Open Display`, request the needed GUI
permission and rerun the same command unchanged. Do not debug that failure by editing Xorg/Xvfb code.

Expected code scope for the first optimization milestones:

- `src/main.c`
- `src/engine/renderer3d.c`
- `src/engine/renderer3d.h` only if a board-mesh entry point is added
- `src/engine/renderer3d_port.h` only if a capability flag is genuinely required

Preserve every unrelated dirty-worktree change. Never clean, reset, restore, or broadly stage the
worktree.

## Milestone 1: make the performance oracle truthful

Add a measurement-only define named `WAIFU_MEASURE_FORCE_LIVE_BOARD`. Do not add it to any Makefile;
it must only be supplied through `EXTRA_CORE_DEFINES`.

In the `IB_PLAYER_TOP` case in `src/main.c`, change both preprocessor guards surrounding top-view
retention from:

```c
#if defined(WAIFU_FM_FMTOWNS) || defined(WAIFU_FB_DAMAGE_VERIFY)
```

to:

```c
#if (defined(WAIFU_FM_FMTOWNS) || defined(WAIFU_FB_DAMAGE_VERIFY)) && \
    !defined(WAIFU_MEASURE_FORCE_LIVE_BOARD)
```

These are the guard before `top_visual_key = battle_top_visual_key();` and the guard before the
`g_b_top_cursor_anim >= 8` retention block. This prevents `UI_TAG_BATTLE_TOP` from bypassing the
renderer in the measurement build. Do not change shipping behavior.

Continue passing `WAIFU_BATTLE_BASE_CACHE_DISABLE` in the profile command. That existing define
removes the battle composite/camera-anchor caches; the new define removes the later whole-UI retained
early exit. Both are required.

Run the truthful baseline exactly as follows from the repository root:

```sh
mkdir -p /tmp/fmt-board-before
FMTOWNS_SHOTS=/tmp/fmt-board-before ./fmtowns.sh profile board \
  'EXTRA_CORE_DEFINES=-DWAIFU_BATTLE_BASE_CACHE_DISABLE -DWAIFU_MEASURE_FORCE_LIVE_BOARD'
```

Do not add `--iso` to a profile: the capture helper defaults to `build/fmtowns/output.cue`. Do not run
`make clean`. `fmtowns.sh` and the flags stamp already rebuild the required objects.

Acceptance for this milestone:

- All three decoded captures reached the same parked top-board state.
- `step` is materially above the bogus retained value of about 0.5 ms.
- Open at least `shot1.png` and confirm it visibly contains the occupied top board, cards, selector,
  and HUD. Filenames and decoded stamps alone are not visual verification.
- No measurement define appears in a normal `./fmtowns.sh build` flags stamp.

Commit only `src/main.c` for this milestone, if commits are requested/authorized:

```sh
git add src/main.c
git diff --cached --check
git commit -m 'FM TOWNS: add forced-live board profile'
```

## Milestone 2: exact axis-aligned fast-affine path

First optimize the tactical top view without changing its pixels. Do not simply call the existing
`cfx_draw_axis_rect_direct_tile()` from `cfx_renderer3d_draw_quad_fast_affine()`: that helper uses
inclusive bottom coverage and width/height-minus-one interpolation, while the current fast-affine
walker uses `y < max_y` and divides the horizontal UV delta by the emitted span. A blind call can add
an edge row or change texture rounding.

In `src/engine/renderer3d.c`, immediately before
`cfx_draw_textured_quad_fast_affine_direct()`, add a static helper with this contract:

```c
static uint8_t cfx_draw_axis_rect_fast_affine_exact(
    const CfxRenderer3DState *state, const uint8_t *tile,
    const CfxVertexIn *v0, const CfxVertexIn *v1,
    const CfxVertexIn *v2, const CfxVertexIn *v3);
```

Implement it as follows:

1. Recognize an axis-aligned rectangle in any vertex order using
   `cfx_get_axis_rect_corners()`. Return 0 if it is not one.
2. Match the old walker exactly: rows are `[top, bottom)`, not inclusive of `bottom`.
3. For each row, use `x_start = left`, `x_end = right`, and
   `span = x_end - x_start + 1`, matching lines around `853-856` of the old walker.
4. Derive the row U/V from the left edge with the same Q8.8 edge stepping and
   `cfx_fast_div_tz_i32_u16_q15()` truncation used by `cfx_fast_quad_build_edge()`.
5. Derive `step_u` and `step_v` using `denom = min(span, 256)`, exactly as the old walker does near
   lines `858-864`.
6. Emit through `cfx_draw_span_direct_tile()`. Do not introduce a function pointer or indirect call.
7. Return 1 after handling the rectangle, including a clipped/empty rectangle.

Then change `cfx_renderer3d_draw_quad_fast_affine()` near `src/engine/renderer3d.c:879-883` from:

```c
CfxVertexIn v3 = cfx_make_vertex_endpoint(p3);
return cfx_draw_textured_quad_fast_affine_direct(state, tile, v0, v1, v2, v3);
```

to:

```c
CfxVertexIn v3 = cfx_make_vertex_endpoint(p3);
if (cfx_draw_axis_rect_fast_affine_exact(state, tile, &v0, &v1, &v2, &v3)) {
    return 1;
}
return cfx_draw_textured_quad_fast_affine_direct(state, tile, v0, v1, v2, v3);
```

Keep the generic path unchanged as the fallback for hand, placement, orbit, walls, and any clipped or
non-axis-aligned geometry.

Quick compile gate after each small edit:

```sh
make -j2
./fmtowns.sh build
```

Then run the like-for-like after profile exactly:

```sh
mkdir -p /tmp/fmt-board-axis-after
FMTOWNS_SHOTS=/tmp/fmt-board-axis-after ./fmtowns.sh profile board \
  'EXTRA_CORE_DEFINES=-DWAIFU_BATTLE_BASE_CACHE_DISABLE -DWAIFU_MEASURE_FORCE_LIVE_BOARD'
```

Compare equivalent settled frames outside the two physical screenshot rows containing the debug stamp:

```sh
convert /tmp/fmt-board-before/shot1.png -crop 640x478+0+2 +repage /tmp/fmt-board-before/scene1.png
convert /tmp/fmt-board-axis-after/shot1.png -crop 640x478+0+2 +repage /tmp/fmt-board-axis-after/scene1.png
compare -metric AE /tmp/fmt-board-before/scene1.png \
  /tmp/fmt-board-axis-after/scene1.png null:
```

The required result is `0` different pixels. Also open the after image. Reject the change if it adds
a bottom/right edge, changes grid thickness, shifts texture sampling, or produces stale pixels on
either page. A speedup with any pixel difference does not pass this milestone.

If the tactical camera does not hit the new helper, instrument only with a compile-time measurement
counter/stamp and remove that instrumentation before committing. Do not weaken the rectangle predicate
merely to manufacture a hit.

## Milestone 3: replace per-cell setup with one board-mesh call

Proceed only after Milestone 2 is pixel-identical. The main structural cost is repeated setup for the
20 cells. Keep ordinary object/wall quads on the existing function.

Add this small public point type and API to `src/engine/renderer3d.h`:

```c
typedef struct CfxBoardPoint {
    int16_t x;
    int16_t y;
} CfxBoardPoint;

uint8_t cfx_renderer3d_draw_board_mesh_fast_affine(
    CfxRenderer3D *renderer, const CfxBoardPoint *points,
    DEFAULT_INT point_stride, DEFAULT_INT rows, DEFAULT_INT cols,
    DEFAULT_INT even_tile, DEFAULT_INT odd_tile);
```

Implement the function in `src/engine/renderer3d.c`. Requirements:

- Accept the current row-major 6x5 vertex grid; do not hardcode an 8x8 board.
- Validate `rows`, `cols`, pointers, framebuffer, atlas, and tile indices before drawing.
- Build/reuse geometric boundary stepping for the mesh rather than invoking the generic quad setup 20
  times. The goal is to share cell-boundary work.
- Preserve each cell's independent UV mapping: a shared boundary is U=255 for the cell on its left
  and U=0 for the cell on its right. Geometry can be shared; UV state cannot be blindly shared.
- Preserve the old loop's cell order (`r` outer, `c` inner), top-left coverage, `[min_y,max_y)` rule,
  `ceil(left)`/`floor(right)` conversion, Q8.8 truncation, `denom=min(span,256)`, and tile pattern
  `((r + c) & 1) ? odd_tile : even_tile`.
- Emit directly through `cfx_draw_span_direct_tile()`; do not add a per-span callback.
- Use automatic/local storage. Do not add a framebuffer-sized or edge-table global; Marty BSS has a
  hard 2 MiB ceiling and limited margin.
- Keep an exact fallback which loops over cells and calls
  `cfx_renderer3d_draw_quad_fast_affine()` if the mesh preconditions are not met.

In `src/main.c`, add a local conversion helper next to `draw_quad3d_fast_projected()` which clamps
projected top vertices exactly as that function currently does and fills:

```c
CfxBoardPoint mesh_points[BOARD_ROWS + 1][BOARD_COLS + 1];
```

In `render_board()` near `src/main.c:5003`, replace only the non-CD32X nested top-cell loop with one
call:

```c
fb_damage_all();
cfx_renderer3d_draw_board_mesh_fast_affine(
    &renderer, &mesh_points[0][0], BOARD_COLS + 1,
    BOARD_ROWS, BOARD_COLS, 1, 5);
```

Do not change slab sides or projected grid lines. Keep the CD32X parallel branch unchanged. If it is
cleaner to leave PC-FX on the old loop, select the new routine through a renderer capability with a
zero default in `renderer3d_port.h`; do not scatter new platform conditionals through shared code.

Before deleting the old top loop, keep it behind a measurement/reference define such as
`WAIFU_MEASURE_BOARD_MESH_REFERENCE`. This permits byte-identical A/B captures from one source tree.
The reference define must not enter shipping flags and may be removed only after the mesh is proven.

Run the same exact forced-live profile twice, once with the reference define and once with the mesh,
using separate `FMTOWNS_SHOTS` directories. Require zero scene-pixel differences and compare the
median settled `step` values, not `present`. Do not claim success from one noisy frame.

## Do not start partial clears yet

Do not remove `clear_screen(IDX_BLACK)` at `src/main.c:4961` in the same change as the rasterizer.
Do not change `fb_damage_all()` or the FM TOWNS dirty VRAM presenter yet. Partial clearing has a much
higher stale-region/two-page artifact risk and must be a separate later milestone after the mesh path
is byte-identical. Likewise, do not rewrite i386 span assembly until a new forced-live attribution
shows that fill, rather than setup, is dominant.

## Required verification before each renderer commit

Fast gates:

```sh
make -j2
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
./fmtowns.sh build
git diff --check -- src/main.c src/engine/renderer3d.c src/engine/renderer3d.h src/engine/renderer3d_port.h
```

If `waifu_fm_headless_cdrom_2mb` does not exist, build it first:

```sh
make headless-cdrom-assets-2mb
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
```

Renderer acceptance gates:

- Canonical forced-live `profile board` command completed with three matching parked states.
- Settled scene pixels compare exactly after excluding only the debug stamp rows.
- The after PNG was visually inspected, including board edges, grid, occupied cards, selector, and HUD.
- Both alternating VRAM pages show no stripes, holes, stale fragments, or every-other-frame errors.
- `./fmtowns.sh build` still reports the payload ending below `0x90000` and BSS ending below `0x200000`.
- No measurement define appears in the shipping build.
- No compiler/linker flags, Xvfb/Xorg code, emulator code, virtual-display scripts, or generated files changed.

Only after all gates pass, stage the exact files changed—not `git add -A` or `git add .`:

```sh
git add src/main.c src/engine/renderer3d.c src/engine/renderer3d.h src/engine/renderer3d_port.h
git diff --cached --check
git diff --cached --stat
```

Do not stage a listed file if it was not changed. Use a separate commit for the exact axis path and
for the mesh walker so either can be reverted independently.

## Stop conditions

Stop and report evidence instead of broadening the task if:

- truthful forced-live profiling cannot be produced after rerunning the canonical command unchanged;
- an optimization cannot match the reference pixels exactly;
- the code approaches either FM TOWNS memory ceiling;
- completing it would require changes to Xvfb/Xorg, Tsugaru/TOWNSEMU, `fmtowns.sh`, compiler flags,
  presentation, CD-ROM, or generated assets;
- the measured improvement is within run-to-run noise.

Report before/after `step`, exact commands, decoded states, pixel-difference count, payload/BSS end
addresses, files changed, and the evidence level. Do not describe calibrated emulator timing as a
physical Marty measurement.
