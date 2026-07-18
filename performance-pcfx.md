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
