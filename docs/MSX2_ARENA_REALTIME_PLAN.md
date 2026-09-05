# Plan: a fast MSX2 arena without bitmap animation strips

Date: 2026-09-05. Source baseline: `859711b5a81381cc04873e5d1b5805c22fc28c9f`.
Target: stock 3.58 MHz MSX2, V9938, 128 KiB VRAM, existing SCREEN 8 presentation.
This is a research and implementation plan; no renderer changes or new emulator
timing measurements were made for this document.

## Recommendation

Replace the per-quad arena renderer with a **scanline span compositor**, then
teach it to repaint only the colors that differ from the contents of the hidden
page. Keep the shared game's projected board and live card mapping.

There are two sensible ways to supply its spans:

1. **Smallest ROM:** traverse the projected mesh using shared edges, producing
   the final visible color runs one row at a time. Keep the existing 11.5 KiB
   mesh asset. This is the appropriate route if no extra pose data is wanted.
2. **Recommended first performance prototype:** derive compact color runs from
   that same mesh offline. The current 23 poses need approximately **29 KiB**
   of row data, potentially fitting in **two additional 16 KiB asset banks**
   with a compact index. The Z80 then compares and emits runs without walking
   polygon edges. This is a bounded substitute for repeated geometry work,
   not a return to multi-megabyte bitmap strips.

Both feed the same HMMV backend and per-page damage model. Prototype the second
to establish the attainable speed without edge-walking costs; implement the
first if the extra two banks are undesirable or a freely moving camera becomes
a requirement. Do not build both production paths unless there is a use for both.

Aim first for a several-fold reduction from the reported **490 ms arena draw**.
An empty-board full redraw around **100–150 ms** is a useful engineering target,
not a promised result. Faster incremental poses are plausible. A fully populated
rotating board must be measured separately: it also redraws textured cards.
Full-band SCREEN 8 redraw at 30 or 60 fps is not a credible target for this path.

## 1. What the logs and current code establish

Read alongside this plan:

- [Latest session log](../2026-09-05-000500-see-homeanonymousdocumentsdevanimecardwai.txt).
- [Previous session log](../2026-09-04-225747-follow-msx2bugstxt-and-fix-the-reported-issues.txt).
- [MSX2 state](../src/msx2/STATUS.md) and [original port design](../MSX2_PORT_PLAN.md).
- [Earlier polygon findings](MSX2_REALTIME_POLYGON_FINDINGS.md).

The latest log takes precedence over older performance diagnoses. Its 1,413
game-loop iterations in 150 emulated seconds, versus 942 before, are a whole-game
throughput result. They are **not 9.42 camera frames per second**. Likewise,
490 ms is a reported routine timing, not a newly reproduced hardware measurement.

| History | Already done; preserve this work |
| --- | --- |
| `8147c86`, `beb31e2`, `98cef43`, `705c94b` | Live flat arena and card rasterization; projected cards remain attached during rotation; obsolete board bitmaps removed |
| `03abe39` | Unsigned card screen-X arithmetic and clipping fix; preserve the x=128 regression case |
| `86dc82d` | Card texel loop reduced from about 63 to 39 T-states per pixel |
| `dc6da87` | VRAM font-mask command rendering; reported string cost 44 → 8.7 ms |
| `c59136c` | Constant-time edge stepping, specialized chain setup, merged empty backdrop rows |
| `3f97992` | View cut draws one page and clones it instead of rendering both |
| `859711b` | Register-held row state and pointer-based coverage tracking |

Do not repeat the rejected experiments without changing their cost structure:

- C-level vertical span merging regressed from 529 to 686 ms despite reducing
  command count. An offline merge or a cheap assembly merge is a different
  experiment; another general C comparison loop is not.
- Clearing the entire band first removed coverage bookkeeping but added fill
  traffic. The latest session measured no ArenaDraw improvement.
- A cheaper CE polling loop did not improve the measured arena time.
- The earlier “0.5 ms per HMMV” deduction counted a polling label as if it were
  a function invocation. It is not a valid command-setup cost.

The previous findings document also derives a roughly 120-T span setup constant
from Metalion's triangle test. That test **modified DX/NX during an executing
command**, rather than launching an independent HMMV for each row. It cannot
calibrate the current safe CE-wait-and-launch protocol. Its advertised frame
rates should be treated as hypotheses, not established performance.

### Current renderer and memory constraints

[`msx2_arena.c`](../src/msx2/msx2_arena.c) draws five pieces of one wall, four
pieces of the other, and twenty checker tiles: **29 quad calls**. Each wall's
pieces have the same color. [`msx2_poly.c`](../src/msx2/msx2_poly.c) sets up two
chains per quad, walks them, clips their spans, records coverage, and later
fills the uncovered surround. It already overlaps some edge work with the
previous command; proposing “asynchronous VDP drawing” alone adds nothing.

The band is x=0..255, y=14..127: **29,184 pixels**. The mesh has 52 points,
visibility flags, and forty card quads per pose. There are 23 × 512-byte records,
or **11,776 bytes**. It already avoids runtime matrix math and projection.

Read-only `./msx2.sh ram` against the existing output map reported:

| Area | Existing output, not a fresh build |
| --- | ---: |
| `_CODE` | 32,262 bytes |
| `_HOME` immediately following it | 362 bytes |
| End of these areas | `0xBF70`: only 144 bytes to `0xC000` |
| Page-0 duel bank, segment 2 | 16,064 bytes: 320 bytes spare |
| Modal / story banks | 3,152 / 10,699 bytes |
| Static RAM allocation | 6,428 bytes |
| Space to HIMEM | 6,756 bytes, also needed by stack and locals |

The arena and polygon implementation are included in
[`waifu_msx2_s2_b0.c`](../src/msx2/waifu_msx2_s2_b0.c). Replacing that code can
recover space; adding an independent renderer beside it cannot fit casually.
Plan cold-code relocation or a reserved code bank before growing the backend.
Never switch away the page-0 bank containing a running caller except through
the existing trampoline contract.

## 2. What RT3D and the V9938 actually contribute

[`RT3D/notes.txt`](../MSX_docs/RT3D/notes.txt) describes shared-coordinate transform
caches and rejecting invisible work before calculating vertices. For this port,
apply that principle to **shared tile edges and final visible spans**: projection
is already baked, so a faster multiply does not accelerate ArenaDraw.

The readable [`deel2.asc`](../MSX_docs/RT3D/deel2.asc) contains SCREEN 8 setup,
HMMV command packets, page handling, detail controls, and a line-interrupt display
list. Use this readable source rather than assuming every `.asm` is plain text.
Its [readme](../MSX_docs/RT3D/readme.txt) describes incomplete experiments that
lock up on exit. No stock-machine fps measurement of that demo was made here.

Turbor explicitly says SandStone omitted perspective depth correction. Its
normal-Z culling shortcut therefore is not a replacement for this game's
perspective projection. The transferable ideas are data reuse, bounded detail,
and overlapping CPU preparation with drawing, not a promise that its renderer
can be dropped into this game. [Author's explanation](https://www.msx.org/forum/msx-talk/development/3d-rasterisation?page=1).

V9938 timing research gives HMMV an ideal byte interval of 48 VDP clocks, with
56 clocks between rows. Actual timing depends on VRAM access availability;
CPU VRAM traffic competes with commands, and sprite enable state matters.
These are lower-bound components, not complete draw timings.
[Original hardware timing research](https://map.grauw.nl/articles/vdp-vram-timing/vdp-timing.html).

For this band, the ideal byte term alone is
`29,184 × 48 / 21,477,273 = 65.2 ms`. Using the latest log's approximately
2.75 µs/byte observation gives **80.3 ms**, still excluding command setup and
unhidden CPU work. Thus a 30-fps full-band HMMV redraw fails even the optimistic
byte budget. Incremental painting has to reduce the number of bytes written.

The manufacturer's command protocol supplies CE status, byte-fill parameters,
and command-register behavior. Preserve that protocol. SCREEN 5's HMMV works
on packed pixel pairs and ignores the low X bit; changing mode needs an edge
and asset strategy, not just a new mode constant.
[Yamaha V9938 Technical Data Book, command sections](https://map.grauw.nl/resources/video/yamaha_v9938.pdf).

## 3. New offline evidence from the current mesh

A temporary host analysis decoded `src/msx2/assets/board_mesh.bin`, reproduced
the current edge rounding (`dy/2` initial error), half-open X/Y spans, band
clipping, and wall-before-top order. It assembled the resulting five-color
rows and counted adjacent color runs. This was **not compared against emulator
VRAM**, so use it for planning sizes and test hypotheses, not pixel-equivalence
certification or Z80 timing.

Input SHA-256:
`abdeb43033bc453f2985cb14300b8ad759383395a23ddfd5c981954091539a2e`.

| Pose | Nonempty quad spans, excluding backdrop | Final color runs, including black | Compact row bytes |
| --- | ---: | ---: | ---: |
| Player chair, 0 | 515 | 520 | 1,154 |
| COM chair, 1 | 514 | 516 | 1,146 |
| Opening start, 2 | 452 | 593 | 1,300 |
| Opening intermediate, 9 | 507 | 664 | 1,442 |
| Turn middle, 20 | 412 | 474 | 1,062 |
| Turn oblique, 21 | 611 | 679 | 1,472 |

Encoding counted here: each of 114 rows has a one-byte run count, then
`(exclusive_end_x, color)` pairs. Start X is implied by the preceding end;
the final endpoint 256 is encoded as zero **only for the final run**. A full
row is always represented, including its black surround.

Across all 23 poses:

- **29,194 bytes**, before indexing, segment padding, and any compression.
- At most **12 runs per row** in this analysis; verify and enforce a bound in
  the eventual generator rather than assuming all future meshes satisfy it.
- Nineteen unique final arena images; exact duplicate deduplication can reduce
  storage further. Card-quad records must remain associated with their own pose.

This does not turn a checkerboard into 114 HMMVs. It still contains hundreds
of differently colored intervals. The win is eliminating duplicate edge work,
quad setup, coverage bookkeeping, hidden overdraw, and eventually unchanged
intervals—not assuming “one traversal per row” means “one command per row.”

The same analysis compared the target pose with the pose normally left in its
hidden page **two animation steps earlier**, excluding cards:

| Sequence | Changed pixels per page update |
| --- | ---: |
| Opening, after the first two pages are initialized | 588–8,436 |
| Five-pose turn, interior updates | 12,208–13,217 |
| Unconditional full band | 29,184 |

At 2.75 µs/pixel, 8,436 pixels consume about 23 ms of fill traffic; 13,217
consume about 36 ms. Neither number includes interval comparison, setup,
cards, or presentation. Fragmented damage can lose to larger fills despite
writing fewer pixels. Measure both changed **bytes and runs**.

## 4. Implementation sequence

### A. Establish a trustworthy baseline and a lower bound

Add a focused MSX2 benchmark fixture, using the real linked routines and symbols
from each exact build. Record ROM hash, machine profile, video frequency,
sprite enable state, audio state, and compiler configuration.

Measure all 23 poses, empty and populated. Split the timeline into mesh read,
edge preparation, span preparation, command launch, CE waiting, backdrop, card
drawing, synchronization copies, and time to the completed visible page.
Include the last command's completion: returning after its launch is not the
same as finishing the draw.

Count commands at writes to R#46, or at a unique launch site. Do not count a
poll-loop target as a routine call. Use emulated elapsed time; PC sampling
identifies hot code but does not alone separate useful work from wait loops.

Benchmark a single 256×114 HMMV and a representative list of short spans with
display on. Compare sprites enabled, sprites disabled, and the actual game
configuration. Test both 50 and 60 Hz. Blanked-display throughput is useful
diagnostically but does not represent a visible camera animation.

Deliverable: per-pose CSV plus decoded VRAM reference images. These replace
the historical average and establish whether later code approaches the VDP
limit or remains CPU-bound.

### B. Supply final visible runs cheaply

**Recommended two-bank prototype:** extend
[`gen_msx_views.py`](../tools/msx2/gen_msx_views.py) to produce the encoding in
section 3 from the existing projected mesh and flat colors. Resolve painter
order offline. Do not sample a newly synthesized lookalike board.

Keep a pose directory and a sparse row index, for example every eight rows.
A two-byte offset for every row of every pose alone costs 5,244 bytes, pushing
the undeduplicated format beyond 32 KiB; do not accidentally spend that budget.
Sequential animation needs no dense row index. Pack complete pose records into
segments or explicitly support boundary crossing. Fail generation when the
selected two-bank budget is exceeded.

Read each approximately 1.0–1.5 KiB pose into a RAM buffer through existing
bank-safe readers, then emit from RAM. For damage comparison, two pose buffers
need at most about 3 KiB with the observed format; initially allow **3.5 KiB
total new scratch and state**, leaving over 3 KiB of the reported free space
for stack/headroom. Confirm against every build and runtime stack watermark.
Use short reads or row blocks if copying whole records has poor interrupt
latency. Reuse buffers only where scene lifetimes prove it safe.

**Mesh-only alternative:** replace independent tile walkers with an active
shared-edge traversal. The top grid has 49 unique undirected segments, versus
80 tile-edge references. At each row, advance active edges once, form ordered
tile intervals, resolve walls beneath the top, and append black outside the
silhouette. Preserve intermediate wall vertices: replacing a segmented rounded
edge by one endpoint-to-endpoint line can change pixels.

Precompute edge starts and events per pose in RAM; avoid sorting all polygons
on every row. Near-horizontal edges, coincident crossings, clipping, and
degenerate quads need explicit handling. Keep hot active state compact and
implement the row loop in assembly. No general framebuffer or depth buffer is
needed. Share one boundary value between adjacent tiles to prevent cracks.

A smaller intermediate improvement is to combine same-color wall intervals
before emission. The complete wall perimeter can contain several rounded
segments; “replace nine wall quads with two quads” is not automatically exact.

### C. Make command submission fit the new representation

Start with the existing safe HMMV emitter as a correctness baseline. Then
benchmark a specialized packet loop with register-held row state and no
per-span C call/argument setup. Keep full-width 256 representable; never let
an 8-bit length zero accidentally become an empty span or a special VDP size.

Investigate reduced register writes only after checking Yamaha's post-command
register-state table. In particular, do not assume DY and NY survive a fill
unchanged. Skipping a byte in an autoincrementing packet can require extra
register-select writes, costing more than the byte saved. Compare actual
instruction cycles for full and partial packets.

Keep S#2 polling and S#0 restoration compatible with the RAM ISR. Restore S#0
before interrupts can run; preserve the two-write control-port transaction.
Do not hold DI over a whole arena or a large rectangle's completion. If merging
creates long commands, yield or poll in short safe windows with interrupts
enabled between them. Reestablish any command context after another VDP client.

Overlap the **next row's RAM work** with the current HMMV. Concurrent card
pixel uploads are a separate contention experiment, not free overlap.
Test hardware sprite disable during camera transitions when all sprites are
intentionally hidden; restore it on every exit path. Hiding sprite attributes
alone is not a measured substitute for disabling sprite processing.

### D. Repaint the hidden page's actual differences

Store an arena pose ID and validity flag per VRAM page. Compare the incoming
row runs with that page's recorded pose, merging the two ordered boundary
lists. Where colors agree, write nothing. Where they differ, emit the new
color. This requires interval arithmetic, not reading pixels back from VRAM.

Compare against the **draw page**, not the currently visible pose. Initialize
both pages explicitly at transition start. Copy operations must update page
metadata; scene loads, overhead cuts, effects, and interrupted transitions must
invalidate it when the arena-only model no longer describes the page.

Cards require an additional damage source. Before drawing a new pose, restore
the old cards' footprints on that page using the **new** arena runs, even where
old and new arena colors agree. Then redraw the current cards in their new
quads. Track previous per-page footprints or conservative boxes and include
outlines/effects that touched the bitmap. Otherwise a perfect arena delta
leaves card trails. Preserve established card ordering until overlapping-card
cases have been explicitly verified.

Use a measured cost heuristic to merge nearby dirty intervals, accepting a few
extra same-color writes when they cost less than another command. Likewise,
use a full row/full arena redraw when damage is too fragmented. A byte-minimal
delta is not necessarily time-minimal.

Once settled, retain the current cheap cursor path. For `ArenaDrawBox`, decode
or generate only the requested rows and clip their intervals, avoiding setup
and stepping for all 29 quads on every small repair.

### E. Make presentation responsive and include the cards

Turn a long pose draw into a bounded main-loop job: prepare runs, submit a
budgeted batch, resume later, draw cards, wait for completion, then request the
V-blank flip. Keep the old complete page visible throughout. Music and input
must continue. This improves responsiveness even when pixel throughput does
not change; report latency and camera fps separately.

[`Msx2_BoardStepCameraMove`](../src/msx2/msx2_board.c) currently redraws every
occupied field card before each flip. Re-profile it after the arena improves.
For example, ten cards at the earlier reported 31 ms/card would alone add
310 ms; that historical average must not be treated as a current per-slot
constant, but it shows why empty-board fps cannot certify the game.

Reject offscreen/degenerate cards before texture loading, reuse consecutive
identical source textures where it pays, and avoid copying unchanged HUD bands.
The 1,920-byte texture buffer already exists; any larger cache must fit the
scratch budget. Keep detailed cards visible throughout motion by default.
Lower-detail card backs/faces during motion would be a separate visual tradeoff,
not an invisible optimization.

The five turn samples are widely spaced. Faster rendering alone can make them
look like five fast cuts. Add intermediate **geometry** samples only after
measuring draw cost, with card quads from the same shared projection. Each
existing-format sample costs 512 bytes plus any optional span record. Avoid
blind screen-space interpolation that changes perspective or detaches cards.

At transition completion, synchronize only regions whose content actually
needs synchronizing. Existing full-page HMMM copies also include offscreen
state; narrow them only after enumerating those dependencies. Font mask lines
496–511, page-0 sprite tables, and the flight caches beginning at local row 216
must survive. There is no spare SCREEN 8 full-frame cache in 128 KiB VRAM.

## 5. Larger levers, only if the measured result still misses the target

| Option | Benefit | Cost / decision |
| --- | --- | --- |
| Offline vertical merging of identical runs | Less packet traffic, no runtime merge search | Add a bounded rectangle format only if it wins against row data; complicates delta/repair lookup |
| Two-line fills during motion | Fewer edge samples and commands | Same destination byte count; visibly coarser diagonals. Restore exact detail at rest and review captures |
| Smaller moving arena region | Fewer CPU and VDP operations | Changes composition; preserve readable cards and shared board proportions |
| SCREEN 5 for the duel | Packed pixels and more VRAM cache room | Sixteen colors, new card/font assets, packed-edge handling and mode transitions; a distinct presentation project |
| Runtime projection via indexed coordinates | Arbitrary camera without pose growth | RT3D supplies useful ideas, but this addresses flexibility, not the present raster bottleneck |

Do not use LINE as the default fill, a full RAM framebuffer, prewarped images
for every card/pose, or full bitmap camera strips. They solve a different
problem or spend the wrong resource here. A raster mode split between SCREEN 5
and SCREEN 8 also needs its own VRAM/interleaving/interrupt design; it is not a
small variant of this plan.

Live DX/NX changes during a running HMMV are an experimental branch only.
[`extra_notes.txt`](../MSX_docs/RT3D/extra_notes.txt) specifically documents
sensitivity to span width, active display, and blanking. Normal music interrupts
make cycle-locked updates still harder. Do not make shipping correctness depend
on that technique without physical validation and a safe fallback.

## 6. Milestones and acceptance gates

| Milestone | Deliverable and gate |
| --- | --- |
| 1. Baseline | Exact-build timing CSV for every pose; command and byte counts; empty and populated VRAM captures |
| 2. Span source | Host reference agrees with current emulator arena pixels for all poses; compact asset ≤32 KiB including indexes/padding, or mesh-only traversal with bounded RAM |
| 3. Full redraw | Safe packet loop, no audio/input regression; materially below 490 ms, with 100–150 ms empty-board target tested rather than assumed |
| 4. Page deltas | Image identical to full rendering after every step, reverse turn, cut, interruption, and small repair; measured gain on both opening and turn |
| 5. Presented game | Empty, typical, and twenty-occupied-cell camera timings; smoothness judged from timed sequences; correct card attachment and responsive input |
| 6. Hardware | Physical V9938 test at supported 50/60-Hz configurations, with music and sprites; record separately from emulator evidence |

Maintain assembly edge cases: X crossing 127/128, negative mesh X, X=255 and
endpoint 256, thin/zero spans, horizontal edges, clipped top rows, both pages,
all wall visibility flags, defense cards, and `ArenaDrawBox` at boundaries.
For exact paths, compare every frame's decoded VRAM to the full-render oracle.
For optional coarse modes, state the allowed visual difference and inspect it.

Build shipping, soak, regression, and story-soak variants that remain supported
by `Makefile.msx2`; check code banks, static RAM, stack watermark, mapper state,
and audio progress. Run the normal soak after focused visual tests. Use real
`/usr/local/bin/openmsx`, never the bundled incomplete headless Z80. Rebuild the
shipping ROM after verification, since `verify` leaves a soak build in `out/`.
Do not replay a savestate across changed RAM layouts or ROM contents as proof.

If implementation changes the shared capture code, first follow the shared-core
skill and regression requirements. Prefer deriving runs inside `tools/msx2/`
from existing mesh output, keeping the game renderer and other targets out of
the implementation scope.

The first implementation should therefore be a measurable, bounded **run-list
backend**, followed by **per-page color deltas with card damage**. It attacks
both remaining costs—Z80 span preparation and VDP bytes written—while keeping
extra ROM in tens of kilobytes and preserving the existing MSX2 presentation.
