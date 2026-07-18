# PC-FX performance findings (V810 profiler)

Measured with the pcfxemu **V810 profiler** build. This is the emulator's own
cache/cycle report mode, not a guess.

## How to reproduce the measurement

Build the profiling emulator (adds `-DV810_PROFILE`, instruments the V810 core —
1 KB icache hits/misses, per-opcode cycles, DRAM 2 KiB page penalties, flag-use
stalls, and a hot-PC histogram):

```sh
cd pcfxemu
make -f Makefile.headless PROFILE=1 PRGNAME=pcfx-headless-prof -j"$(nproc)"
```

Run it on a real gameplay state; the report prints to stderr at exit and a
per-PC histogram CSV goes to `$V810_PROF_OUT`:

```sh
V810_PROF_OUT=prof.csv pcfxemu/pcfx-headless-prof --bios-dir . \
  --state-in in_duel.sav --frames 600 --pcfx --commands anim.cmds waifupcfx.cue
```

Resolve hot-PC buckets to functions with the game ELF's **symbol table**
(`v810-nm`, not `v810-addr2line` — addr2line's `.debug_line` for this
`-O2 -mprolog-function` ELF is degenerate and mis-reports every address as
`___divdi3`):

```sh
v810-nm -n build/pcfx/waifupcfx.elf | grep -E ' [tTwW] ' \
  | awk '{print strtonum("0x"$1), $3}' > syms.txt
# nearest preceding symbol for a bucket address = the enclosing function
```

## Baseline — in-duel with card/summon animation (600 fields)

CPU is essentially **saturated**: 357,625 cyc/field attributed = **99.91 % of a
field**, CPI 2.607. So every cycle removed is a real frame-time win.

Cost centres, per field (a field = 357,950 cyc @ 21.477 MHz / 60):

| Cost centre | cyc/field | % of field | Notes |
|---|---:|---:|---|
| **2 KiB DRAM page penalties** | 80,341 | **22.4 %** | +3 cyc/change; **data=77,632**, code=2,709. Data ping-pong. |
| **flag-use stalls** | 45,866 | **12.8 %** | 99.9 % of Bcc/SETF/STSR pay +2 (compare-then-branch). Codegen. |
| icache misses (fixed) | 4,666 | 1.3 % | 2,333 misses/field @ +2, miss-rate 1.22 %; DRAM refill extra |
| hardware DIV | 5,244 | 1.5 % | 138/field |
| hardware MUL | 3,349 | 0.9 % | 251/field |

Top opcodes by cycles: `LD_W` 30.0 % (CPI 4.65 — load-use + DRAM stalls),
`ST_W` 8.3 %, `BNE`/`BE` 14 %, `ST_B` 6.7 %.

### Hottest routines (hot-PC buckets → nm)

| cyc/field | icache-miss (600f) | routine | file |
|---:|---:|---|---|
| **~108,800 (30 %)** | ~6,400 | **`pcfx_dirty_plan_stats`** | waifu_pcfx_video.c |
| ~70,100 | ~5,400 | `draw_interactive_base` | main.c |
| ~21,200 | ~11,200 | `draw_text_small` | main.c |
| ~29,000 | ~18,300 | `rect_fill` (`.part`) | main.c |
| ~12,300 | **44,467** | `put_px` (`.part`) | main.c |
| ~4,300 | 96 | `draw_card_raw` | main.c |
| ~4,300 | 18,954 | `side_battle_camera` | main.c |

`pcfx_dirty_plan_stats` is the single biggest sink — **~30 % of the whole
field**. It diffs the full 256×240 CPU framebuffer against the page-shadow in
16-byte blocks (`cur` vs `old` read interleaved). `cur` and `old` are two
separate ~60 KB DRAM arrays in different 2 KiB pages, so the interleaved reads
are the main source of the 22 %-of-field data-page ping-pong. It is also a
*separate* full pass from `pcfx_present_dirty_bands`, so a partial-update frame
scans the framebuffer **twice**.

The worst **icache** offenders are the per-pixel/per-primitive helpers
(`put_px` 44 k misses, `rect_fill`, `draw_text_small`, `side_battle_camera`):
small functions called in tight loops that evict each other from the 1 KB
icache.

## libgcc / libm dependency audit

The ELF links **no libm** (no `sin/cos/sqrt`, no float) and **no soft-float**
(`__*df3/__*sf3` absent) — the renderer is already fixed-point. The only libgcc
runtime helpers linked are the **64-bit integer** routines:

- `___divdi3` (64-bit signed divide) — 0x9ad8
- `___muldi3` (64-bit multiply) — 0x9ac4

Together ~216 cyc/field (small), but they pull in ~1 KB of image and the goal is
to drop the dependency. Call sites (31 total): `draw_tri3d_pyramid_face` (24,
story-map pyramid), `line_i` (4, line clipper), `step_battle_interactive` (3).

## Optimization plan (milestones)

1. **Drop `___divdi3` / `___muldi3`.** Narrow the 64-bit int div/mul in
   `draw_tri3d_pyramid_face`, `line_i`, `step_battle_interactive` to 32-bit
   where the operands provably fit (they do in the projected coordinate ranges),
   or route through the existing `lldiv_trunc` 32-bit fast path. Pixel-identical.
2. **Cut `pcfx_dirty_plan_stats` cost + DRAM page ping-pong.** Reduce the
   interleaved `cur`/`old` reads and the double full-scan while keeping dirty
   detection exact. Directly attacks the 30 %-of-field sink and the 22 %
   data-page penalty.
3. **Icache: tighten the per-pixel primitives** (`put_px`, `rect_fill`) and hot
   2D helpers so the inner render loops stop thrashing the 1 KB icache.

Each milestone is verified in `pcfx-headless` (accurate backend — capture and
look) for visual correctness, then re-measured with `pcfx-headless-prof`, and
committed unsigned. Results are appended below as milestones land.

## Results

_(appended per milestone)_

## Results (appended per milestone)

### Migration baseline (libpcfx build, in-duel summon-animation state, 600 fields)

After the liberis->libpcfx migration the in-duel cost profile is essentially
unchanged (the render routines are the same code): CPI 2.489, field 99.91%
saturated. 2 KiB DRAM page penalties **74,644 cyc/field (20.85%)**, data=69,868;
flag-use stalls 36,023 (10.06%); icache miss-rate 1.93% (4,040 misses/field).
Hottest DRAM sinks: `draw_interactive_base`, `pcfx_dirty_plan_stats` (the
full-frame diff), `rect_fill`/`put_px`/`draw_text_small` (icache thrash),
`battle_base_cache_store`.

### Milestone: single-scan dirty present (batched reads already in place)

`pcfx_dirty_plan_stats` (plan) and `pcfx_present_dirty_bands` (upload) both diffed
the full 256x240 framebuffer against the page-shadow -- the plan pass computed
per-row dirty runs and threw them away, then the present pass re-read the whole
frame + shadow to rebuild them. The plan pass now stores its runs
(`g_plan_runs` / `g_plan_row_run_count`, capped at the dirty-band run budget) and
the present pass reuses them, so a dirty-band frame diffs the frame **once**
instead of twice. Verified **pixel-identical** (md5 of in-duel frames unchanged).

Scope note: the profiled summon-animation and settled-board states are
full-upload or unchanged frames, which do not take the dirty-band path, so this
does not move their numbers -- it removes one full frame+shadow read (~120 KB of
2 KiB-page-crossing traffic) on the partial-update frames that do (cursor / LP
counter over a static board). The dominant in-duel DRAM sinks for future work
are `draw_interactive_base` and the plan-pass scan itself.

### Milestone: drop int64 (___divdi3/___muldi3) + clean pyramid (commit 8a3a11a)

Goal: eliminate the libgcc 64-bit int helpers and make the PC-FX story-map
pyramid use the same clean renderer as the CD32X build. Three changes, all
verified in `pcfx-headless` (accurate backend): title, LOAD STORY sanctum
pyramid, in-duel board + hand-card rims.

1. **PC-FX pyramid faces -> `cfx_renderer3d_draw_quad_board`.** The story-map
   pyramid (`draw_tri3d_pyramid_face`) was the only PC-FX 3D primitive still on
   the legacy per-face barycentric rasterizer in main.c, whose gradient/seed
   setup used 64-bit int mul/div (the `2LL*Aa*U0 / den2` and
   `(int64_t)wa2_row*U0 / den2` numerators reach ~1e11 because texcoords are
   `<<16` and screen deltas up to ~4000). The board already routes through the
   compact, edge-stepped, division-free `cfx_board_tri` (renderer3d.c) on both
   PC-FX and CD32X; the pyramid now does too (degenerate quad, apex doubled), so
   the two targets share the identical clean path. The legacy int64 `#else`
   branch stays only for host/SDL builds. This is aligned with the renderer's
   own I-cache strategy (one small span rasterizer that fits the 1 KB icache).
2. **`line_i` clip -> 32-bit.** `project_point`/`project_point_basis` already
   clamp every projected vertex to +-8192, so the "coords in the millions" case
   the 64-bit Liang-Barsky clip guarded against can no longer happen; `q<<16`
   (<=~5.5e8) and `dx*t` (<=~1.07e9) both fit int32. A defensive +-8192 clamp
   keeps it overflow-proof for any caller. Truncation-toward-zero preserved, so
   bit-identical for all in-range inputs.
3. **`slide_x` support-card interpolations -> 32-bit** (products are tiny).

`lldiv_trunc` is now compiled only for the CD32X affine card path that still
needs it. With no 64-bit int mul/div left in compiled PC-FX code, the linker
drops ___divdi3 / ___muldi3 entirely.

Result: PC-FX ELF **404276 -> 365488 bytes (-38 KB)** -- removing the legacy
pyramid rasterizer + the two libgcc helpers -- which also eases 1 KB-icache
pressure. Re-measured in-duel (fresh state from this build, cursor animation,
600 fields): CPI 2.566, icache miss-rate **1.08%** (2246 misses/field), no
___divdi3/___muldi3 (remaining DIV/DIVU/MUL are hardware 32-bit ops). The 64-bit
helpers were only ~216 cyc/field, so this is a code-size/dependency/correctness
win, not a cycle win -- as intended.

### Current dominant sinks (post-milestone, in-duel + cursor anim, 600 fields)

| Sink | share | routine (nm) | notes |
|---|---:|---|---|
| **2 KiB DRAM page penalties** | **24.3 %** | (data=83761 cyc/field) | V810 has no data cache; +3 cyc/2 KiB-page change |
| `draw_interactive_base` | top cycles | main.c | in-duel board base; biggest single cycle bucket |
| `pcfx_dirty_plan_stats` | 2nd | waifu_pcfx_video.c | present diff (already batched + single-scan) |
| flag-use stalls | 9.9 % | codegen | 99.9 % of Bcc/SETF/STSR pay +2 |
| icache misses (fixed) | 1.25 % | rect_fill / side_battle_camera / draw_text_small | small hot fns thrash 1 KB icache |

The floor (`draw_floor_tiled`) is disabled on PC-FX, so in-duel the DRAM penalty
is dominated by `draw_interactive_base` and the plan-pass scan. Per the renderer
perf history, camera-keyed board caches were measured as ~1 % dead-ends (unique
camera every animation frame) and reverted; the fundamental cost is the
full-board re-render + near-full KRAM upload on animation/transition frames.

### Milestone: batch copy_u8_fast -> kill the DRAM page ping-pong (commit b8a3d40)

`draw_interactive_base` was the top hot-PC bucket, and its cost was **not** board
rendering -- on a cursor-move frame the camera is unchanged, so the camera-keyed
`battle_base_cache` HITS and `draw_interactive_base` just `copy_u8_fast`s the
~60 KB cached composite into the framebuffer (then overlays the LP counters).
`copy_u8_fast`'s V810 inner loop interleaved `ld.w src; st.w dst; ld.w src; ...`.
`src` (cache) and `dst` (framebuffer) are separate ~60 KB arrays in different
2 KiB DRAM pages, and the V810 has no data cache (a single last-page register,
+3 cyc per 2 KiB page *change*), so **every one of the 16 accesses per 32-byte
group changed page** -- ~15 page changes/group. That single copy was most of the
24 % data-page penalty.

Fix: batch all eight loads (into r10..r17) then all eight stores, so the loads
are one contiguous `src` run and the stores one contiguous `dst` run -> ~2 page
changes/group. Non-overlapping memcpy semantics unchanged (all callers copy
between distinct arrays); tail byte loop untouched. Same trick the dirty-diff
already uses.

Re-measured (fresh in-duel state from this build, cursor animation, 600 fields):

| metric | before (8a3a11a) | after (b8a3d40) |
|---|---:|---:|
| 2 KiB DRAM page penalty | 86908 cyc/field (24.28 %) | **56362 cyc/field (15.75 %)** |
| ...data component | 83761 | **52862** |
| CPI (mean) | 2.566 | **2.306** |
| top hot bucket | `draw_interactive_base` | `pcfx_dirty_plan_stats` |

Verified pixel-identical in `pcfx-headless` (the cached base is copied
byte-for-byte; in-duel board/HUD/cards render the same). This copy is shared by
every framebuffer<->cache copy (`g_board_bg_cache`, the hand<->top keyframe
caches, row blits), so all of them get the same DRAM reduction.

After this, the top in-duel sink is `pcfx_dirty_plan_stats` (the present diff,
already batched + single-scan) and the small icache-thrashing 2D helpers
(`rect_fill`, `draw_text_small`, `draw_text`, `side_battle_camera`). The
remaining data-page traffic is the genuinely unavoidable full-frame diff + KRAM
upload on frames that actually change the board.
