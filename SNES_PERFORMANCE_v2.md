# SNES performance plan v2

Date: 2026-09-14. Source review: worktree at `9f97a48`.

The objective is to reduce the current **177 master clocks per sampled texel**
and shorten the time until a completed board reaches the screen. The best next
steps are a different allocation of A/B/X for bounded spans, less work in the
chunky-to-planar converter, and a documented allocation of additional DMA
registers. Merely finding more writable registers will not accelerate a loop
whose three scratch bytes are already in fast MMIO.

This is an implementation plan, not a report of completed optimizations. Cycle
figures below are static estimates unless explicitly identified otherwise;
none is a new real-hardware measurement. Rebuild and establish the baseline in
phase 0 before accepting improvements.

Planning checks confirmed the current loop's instruction count and branch
sizes, the reverse-fold identity for all 65,536 packed plane-pair combinations,
and the proposed one-step fractional carry model for 131,072 combinations.
These algebra/layout checks do not validate an assembled replacement kernel,
its boundary dispatcher, or its performance on a console.

## 0. Status (2026-09-14, later the same day)

The first batch (section 11's phases 0, 2 and 3, with a documented slice of
phase 1) is implemented in `src/snes/snes_raster.asm`, `snes_fastdp.inc`,
`tools/snes/gen_snes_conv.py`, and one C-side constant (`snes_video.h`,
`snes_board3d.c`, `snes_duel.c`).  What landed, and how it differs from
the text below:

* **Phase 0.**  Baseline = the debug ROM of the unmodified tree (sha256
  `0821bfd0…`), profiled with `prof.py` on the `snes-headless` core over three
  windows: the fixture bake (`R@2200`, fields 2200-2299), the lift (`R@2200
  UP@2400`, 2400-2440) and the Y-pinned 128x72 rest frame (`R@2200 Y@2400`,
  2700-2799).  The denominator is SAMPLED TEXELS (executions of `sta.w $2180`
  inside the sampling symbols), summed over walkers, runs, stamp rows and
  their setup; the window totals alone mislead because a faster renderer
  does more work in the same fields.
* **Phase 2, the B-fraction run.**  Implemented as section 5.1 proposes, but
  the "eight/seven-texel block with a loop" was measured and REPLACED by the
  alternative the section names: a 128-body fully unrolled block with the
  whole-texel step BAKED INTO EACH BODY'S ADDRESS (`lda.l TEX-(127-i)*k,x`),
  entered n bodies from its end, X pre-advanced by n*k.  With loop bodies
  the unconditional `inx` per whole texel cost 12 master cycles each, so
  k = 2 saved 4.5% and k = 3 lost; the baked block costs the same for
  k = 0..3 (150-153 measured on the WRAM world texture against the walk's
  189).  The dispatcher: cheap fit test, exact `cnt*du` end via the CPU
  multiplier, the CPU divider only for k = 0 spans that really wrap (their
  pieces save 27+/texel); a wrapping k >= 1 span is walked whole, since two
  pieces plus a wrap step cost what the run saved.  Spans under 24 texels
  or with du >= 4 keep the walk.  Blocks are 128 bodies because 256 x four
  k x five textures did not fit the code's bank; a longer piece is entered
  twice.  The card stamp has its own 32-body block with a fixed entry.
* **The world texture's origin moved** (`SNES_WORLD_U_CENTRE`, 16 -> 144):
  at 16 the board straddled the u wrap and EVERY row through it was two
  pieces (or two block moves overhead).  144 = 16 + 128, a multiple of the
  cleared ground's 64-texel checker, so the pictures are unchanged; 80 and
  96 were tried first and rejected (80: rows whose first u rounds under the
  slab's edge wrapped; 96: the ground pattern's phase changed the overhead
  picture).  This is a platform-file change, not a shared gameplay change.
* **Phase 3.**  Both converter reductions as written (6.1 and 6.2): emitted
  `lsr` 1792 -> 1216 (executed halved), the fold identity checked on all
  65,536 packed combinations, the half converter's duplicate row stored once.
* **Phase 1, partially.**  The allocation is documented in `snes_fastdp.inc`
  (FD_RUN_*, FD_BT_ENTRY/RUN); no assembler assertions or probe ROM yet.

Measured, same sampled texels and byte-identical frames (rest frame, a
mid-lift frame, the settled overhead; the world texture identical modulo its
128-texel rotation), `verify.py` all 40 checks passing:

| Window | Sampling, master cycles per sampled texel | Conversion (tile routines) |
| --- | ---: | ---: |
| Fixture bake | 233.6 -> 195.7 (-16.2%) | -12.6% (full tiles) |
| Lift | 202.2 -> 181.5 (-10.2%) | -9.0% |
| Pinned 128x72 rest frame | 202.7 -> 182.2 (-10.1%) | -16.7% (half tiles) |

End to end (`verify.py` render cost): still bake 8876 -> 8323 lines, a
motion frame 3184 -> 3005.  The motion frame remains bound by the ring wait
on the NMI drain (`_rr_wait`), which is section 8's territory.

Left for later phases: 5.2 immediate-operand kernels, 5.3's MVN/copy
comparisons and the turned-card stamp, 7 (calling contracts, scheduled
arithmetic, fixed-camera descriptors), 8, 9, and the differential harness
of 10.4 (the host model in the session notes covered 2.9 M dispatcher cases
against the eight-bit walker; the assembled kernels were checked by the ROM
decode of every body and by frame identity).

## 1. Scope and constraints

Use the constraints recorded in [Ideas.txt](Ideas.txt): a stock SNES with a
FastROM cartridge, small SRAM for saves, no spare SPC capacity, and willingness
to test unusual techniques on real hardware. Preserve the current graphics,
camera poses, audio, and controls while optimizing. Keep the existing generic
renderer as the correctness reference and fallback.

The relevant registers are **DMA/HDMA registers**, plus the WRAM access ports
and arithmetic units. WRAM is DRAM, but there is no software setting that turns
all 128 KiB into six-clock RAM or removes its refresh overhead. The useful
distinction is the cost of each CPU bus access, not one clock rate for an entire
instruction. [SNES timing documentation](https://snes.nesdev.org/wiki/Timing)

Implementation belongs in `src/snes/`, `tools/snes/`, and `Makefile.snes`.
Generated assembly and tables must be changed through their generators. No
shared gameplay change is needed for this plan.

## 2. What the current code actually does

| Area | Current implementation | Consequence for this plan |
| --- | --- | --- |
| Cartridge | `Makefile.snes`: 4 MiB HiROM/FastROM, map byte `$31`, 8 KiB SRAM; SDK startup writes `$420D = 1` | FastROM is already enabled. Audit placement rather than proposing to enable it again. |
| Constant-v sampling | `snes_raster.asm`, `RS_WALK_TEXEL` and `RS_WALK` | Scratch is already in DMA channel 1, destination already uses WMDATA, and the loop already emits two texels per iteration. |
| Register allocation | `snes_fastdp.inc` | D is `$4300`; channels 1–3 are shared scratch. The implementation avoids each channel's DMAP byte, so its conservative capacity is **33 bytes**, not the 36 advertised in comments. |
| Texture source | ROM floor/card sheets; dynamic `snes_board_texture` at `$7F0000–$7F7FFF` | A dynamic world-texture read still costs a WRAM access. The ROM and world walkers must be measured separately. |
| Framebuffer | `snes_fb.asm`: 36,864 bytes at `$7E7000–$7EFFFF` | Rest rendering is 256×144. The moving path also has a 128×72 representation in the top of this allocation, expanded by the half converter. |
| Overhead sampling | `snesSpanFloorTex`, `du == $0100`, target bank `$7E` | Already uses `MVN $7F,$7E`, with up to two pieces for texture-row wrapping. This path is not a 177-clock DDA. |
| Fixed-point multiply | `snesQMul` and pitch-row origin calculation | Already use `$211B/$211C → $2134–$2136`. Adding the PPU multiplier is not a new optimization. |
| Other arithmetic | `snesMulHi`/`snesMulLo`; `snesUQDiv`; world-card stamp setup | CPU multipliers still have explicit NOP delays. General Q division is software; stamp setup already uses the hardware divider. |
| Conversion | `gen_snes_conv.py`, `snes_conv_gen.asm` | Unrolled pair-LUT conversion; D points at the WRAM ring page, with four generated slot-phase variants. Repeated intermediate stores and shifts remain. |
| Presentation | `snes_conv_drivers.inc`, `snes_fb.asm`, `snes_fb.inc` | Sparse dirty cells, ROM-planar floor bypass, copy-on-write tiles, a 4 KiB ring, 32 jobs, 702 physical tiles and a 351-cell frame limit already exist. |
| Interrupts | SDK `vblank.asm` plus `snesFbNmi` | Channel 7 uploads OAM/tiles; the wrapper saves A/B/X/Y, D and DB. Foreground WMDATA and multiplier state must remain untouched. |
| Other hot assembly | `snes_oam.asm`, `snes_deck_ring.asm` | Sprite emission already uses DMA scratch; the deck cursor already uses word operations. Optimize remaining work from profiles. |

The current [RENDERER.md](src/snes/RENDERER.md) is useful context, but several
comments lag the implementation: the moving representation exists; Q multiply
uses the PPU unit; `$4202` is a CPU register, not a PPU register; and transfers
do mutate some DMA registers. The older [HIGHCOLOR_FB.md](src/snes/HIGHCOLOR_FB.md)
describes a different renderer and includes proposals that must not be treated
as the current implementation.

Existing `.prof.txt` files and `build/snes/verify_full.log` are historical
artifacts without a matching source/ROM identity recorded here. They are not
the v2 baseline; that log also contains a failure. Do not import its timings
or its claims about removed paths as current results.

## 3. Reconstruct the 177-clock baseline

For `RS_WALK_TEXEL`, with FastROM code/source, aligned D, A8, X/Y16 and DB=0:

| Instruction | CPU bus/internal cycles | Master clocks |
| --- | ---: | ---: |
| `lda.b <FD_UFRAC` | 3 | 18 |
| `clc` | 2 | 12 |
| `adc.b <FD_DUFRAC` | 3 | 18 |
| `sta.b <FD_UFRAC` | 3 | 18 |
| `txa` | 2 | 12 |
| `adc.b <FD_DUINT` | 3 | 18 |
| `tax` | 2 | 12 |
| `lda.l TEX,x` | 5 | 30 |
| `sta.w $2180` | 4 | 24 |
| **Body** | **27** | **162** |
| `dey; bne`, taken, shared by two texels | 5 / 2 | 15 |
| **Steady-state total** | **29.5** | **177** |

This is an instruction-level reconstruction from the source, using the
[W65C816S instruction specification](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf).
It excludes entry/exit, tail handling, final branch differences, row setup,
refresh, interrupts, DMA, conversion and upload. A WRAM texture adds two master
clocks per source byte: approximately **179** for the corresponding loop.
`snesBoardTextureCard` currently uses a one-texel loop around the same body,
so its steady-state ROM sampling cost is approximately **192**, not 177.

Two useful limits follow:

* Unlimited unrolling of the unchanged body only approaches **162**: an 8.5%
  reduction. A four-texel block approaches 169.5. Eight/sixteen copies of this
  18-byte body exceed BNE's backward reach; with `DEY; BEQ exit; JMP loop`,
  they approach 167.25/164.625, rather than the optimistic 165.75/163.875
  obtained by assuming a short loop branch still fits.
* Moving this unchanged loop to D=`$2100` makes four scratch accesses
  absolute, adding 24 clocks, while its WMDATA store saves six. That gives
  approximately **195**, before switching costs. It is a regression.

Track distinct measurements: sampled source texels, destination chunky
texels, converted screen pixels, converted tiles, and completed generations.
One half-frame input texel expands to four displayed pixels; reporting that
as four equally cheap sampled texels would misstate the improvement.

## 4. Establish register ownership before expanding it

### 4.1 The physical windows

Each channel has `$43n0–$43nB`. `$43nF` aliases `$43nB`; `$43nC–$43nE` are
not extra storage. An indexed array cannot run across the gaps. DMA changes
its source offset and count; HDMA changes its current table pointer and line
state, and indirect HDMA also changes its indirect address. The unused B byte
is distinct from those live fields. See the
[DMA register reference](https://snes.nesdev.org/wiki/DMA).

The allocation below comes from the game's actual register writes and the
provided notes. It is a proposed ownership policy, not permission to use all
96 bytes simultaneously:

| Channel | Owner and mode in this code | Candidate scratch | Must retain |
| --- | --- | --- | --- |
| 0 | Foreground general DMA, SDK transfer helpers and WMDATA fills | `$4307–$430B`: five bytes, after auditing every helper | `$4300–$4306` from setup through transfer completion |
| 1 | Foreground scratch; never enabled | `$4311–$431B` now; `$4310` after a DMAP retention probe | Both DMA/HDMA enable bits remain clear |
| 2 | Foreground scratch; never enabled | `$4321–$432B` now; `$4320` after probe | Same |
| 3 | Foreground scratch; never enabled | `$4331–$433B` now; `$4330` after probe | Same |
| 4 | Direct HDMA: duel/panel gradient, title window, destruction wipe | `$434B`; later `$4345–$4347` after direct-mode retention tests | `$4340–$4344`, `$4348–$434A` while armed |
| 5 | Disabled during board HDMA; direct HDMA for dialogue sky | `$435B`; later `$4355–$4357`; entire window only under an explicit board-only lease | Full configuration restored before dialogue uses it |
| 6 | Direct HDMA: board layer clip or dialogue sky | `$436B`; later `$4365–$4367` | `$4360–$4364`, `$4368–$436A` while armed |
| 7 | SDK NMI OAM copy and game NMI drain, normal DMA | `$4377–$437B`: five **NMI-private** bytes | `$4370–$4376` owned by the uploader |

`arm_hdma()` enables `$50` (4 and 6); `arm_scene_hdma()` can enable `$70`
(4–6). Title/wipe paths use channel 4. A disabled channel 5 is therefore useful
during board work, but it is not globally unused.

Capacity, counting each physical byte once:

| Level | Foreground bytes | NMI-private bytes | Aggregate |
| --- | ---: | ---: | ---: |
| Existing channels 1–3, excluding DMAP | 33 | 0 | 33 |
| Add channel 0 tail, B bytes of 4–6, channel 7 tail | 41 | 5 | 46 |
| Qualify DMAP on channels 1–3 | 44 | 5 | 49 |
| Qualify indirect-only fields of direct HDMA 4–6 | 53 | 5 | 58 |
| Board-only full channel 5 lease, including its DMAP | 61 | 5 | 66 |

The last step adds eight bytes, because four channel-5 bytes were already
counted. A foreground routine never receives the five NMI-private bytes.
An implementation need not reach the largest capacity to achieve the speed
targets.

### 4.2 Make the allocation executable documentation

Extend `snes_fastdp.inc` with named regions, per-kernel clobber lists and
assembler assertions. Include a host-side allocation description if generators
need it. Check byte/word widths, physical aliases, window boundaries and
overlapping live ranges. DMA-register words may start at odd addresses: it is
**D's low byte**, not a word's alignment, that adds the direct-page penalty.

Keep D=`$4300` for register-heavy code. Generate offsets at build time; avoid
runtime translation of a logical 36-byte array. Overlay only states whose
lifetimes do not overlap. Existing emitters and converter helpers do nest, so
the current broad “leaf scratch” comment is not a sufficient call contract.

Suggested allocation order:

1. Keep current walker/math/OAM offsets working while documenting them.
2. Give the NMI two words at `$4377` and `$4379`, plus a byte at `$437B`:
   candidate `nm_budget`, `nm_n`, and flags.
3. Use channel 0's tail for foreground state that must survive math/walker
   calls, provided every channel-0 helper preserves that tail.
4. Give each row renderer a dedicated set of live accumulators; keep cold
   configuration and state that survives a cooperative yield in WRAM.
5. Lease channel 5 only after all scene transitions release/reinitialize it.
   Do not depend on an already-armed HDMA channel reaching its terminator.

Watch actual MDMAEN/HDMAEN writes, including SDK code, rather than just register
names in `snes_video.c`. No optimization may accidentally enable channels 1–3.
Keep a software shadow of write-only enable registers where ownership needs it.

## 5. Highest-value sampling change: move the fraction into B

### 5.1 Bounded spans with integer step zero or one

The current walker reserves B for the source index's high byte because `tax`
reassembles the index after every byte-sized addition. For a span that cannot
cross a texture-row boundary, X can instead retain the **whole source index**
throughout. That frees B for the fractional accumulator and removes its load
and store on every texel.

Start with `du.integer == 0`. Use X16 for the source index, B for the fraction,
Y16 for the block count, D=`$4300`, DB=0 and A8 for texel data. The conceptual
body is:

```asm
; Preconditions: X is the previous sample's full index; B holds its fraction.
; The dispatcher has proved this piece cannot cross its texture row.
    xba
    clc
    adc.b <FD_DUFRAC
    xba
    bcc +
    inx
+   lda.l TEX,x
    sta.w $2180
```

`XBA` preserves carry, and the texel load preserves the new fraction in B.
For integer step one, insert one unconditional `inx` before the conditional
increment. Never use `tax` to rebuild X while B contains the fraction.

With `k` the integer step (0 or 1), `p` the fraction of samples that carry,
and unroll factor `U`, the proposed steady-state ROM cost, when the short loop
branch fits, is:

```text
138 + 12*k + 6*p + 30/U master clocks per sampled texel
```

For U=8 this is about **141.75–147.75** for k=0. Its 15-byte body fits a
backward BNE at that unroll factor. The k=1 body grows to 16 bytes and needs
a different branch arrangement: an eight-texel `DEY; BEQ exit; JMP loop`
footer gives **155.25–161.25**. Compare a seven-texel block or fully unrolled
entry table as alternatives. Add two for WRAM samples. These ranges exclude
dispatch, boundary splits, setup, tails, and interruptions. They are candidates
to measure, not promised whole-frame speedups.

Implementation tasks in `snes_raster.asm`:

* Dispatch once per span; retain the general DDA for negative steps, larger
  integer steps, boundary cases and short spans that cannot amortize setup.
* Preserve the current “step before reading” convention exactly. Initialize
  X and B independently, including the borrow when the first u is zero.
* Prove the complete sampled coordinate interval, including the fraction.
  For floor/world wrapping, split into valid pieces and reset X to the same
  row's start; an `inx` at column 255 must not silently increment v.
* For card sheets, certify that u remains in its own 16/32-wide row and that
  the sheet/page selector is unchanged. A 256-byte page check alone is not
  sufficient for a 32-wide card.
* Handle counts 0/1 and every tail length explicitly. Use the generic path if
  boundary analysis is more expensive than the saved work.
* Check that NMI restores all 16 accumulator bits; the current SDK wrapper
  does. Include interruption at both XBA positions in differential testing.

### 5.2 Immediate-operand FastROM variants and D=`$2100`

Once profiles identify common exact `du.fraction` values, generate selected
copies with `adc #fraction`. Under D=`$4300` that saves six clocks per texel.
With no hot scratch read left, D=`$2100` also makes `sta.b $80` save six.
The combined modeled cost becomes:

```text
126 + 12*k + 6*p + 30/U, plus 2 for a WRAM source
```

For k=0/U=8, this is approximately **129.75–135.75** from ROM. D=`$2100`
is useful here because the register allocation changed; relocating the old
walker was slower. With a non-immediate fractional step, the extra absolute
scratch-read cycle cancels the cheaper WMDATA store, so there is no inherent
reason to switch D.

The direct-page output reduces the immediate variant's body to 14 bytes
(15 for k=1), so eight copies plus a short loop footer fit. Verify emitted
branch displacements after every generator change; extra boundary checks or
instrumentation can invalidate both the layout and its cycle estimate.

Generate the most frequently used variants first, not every combination of
width, sheet, orientation and fraction. Keep the selected fraction exact;
quantizing du changes the picture. Measure code size, dispatch and the minimum
profitable span length. Same-bank indexed jump tables must remain in the
program bank; use an explicit cross-bank wrapper where necessary.

### 5.3 Other span classes

| Class | Planned implementation | Acceptance requirement |
| --- | --- | --- |
| General positive/negative DDA | Four/eight-texel blocks; retain exact carry/wrap behavior | Lower weighted cost including short tails, without excessive ROM growth |
| Constant sample, `du == 0` | Read once and stream the repeated byte; longer fills may use existing ROM-ramp DMA | Exact count and value; include DMA setup and safe scheduling |
| Unit step, `du == $0100` | Keep MVN reference; compare fully unrolled 16-bit copies with baked offsets | Correct fractional convention, row split, odd byte and bank boundary behavior |
| World card stamps | Apply block unrolling and bounded B-fraction kernels to `_bt_pixel` and `_bt_pixel_hi` | Include the current one-texel-loop baseline and both sheets |
| Turned card stamps | Replace the remaining C inner loop in `texture_stamp_turned` with a strided assembly reader and WMDATA output | Pixel-identical rotations and both face orientations |
| Affine held/defence cards | Separate certified interior spans from boundary/clamped spans | Remove per-texel clamps only where monotonic endpoint/range analysis proves safety |
| Yawed floor quads | Audit absolute versus long WMDATA access, row setup and signed DDA layout | Keep 256×128 wrapping and generic signed steps |

For the overhead WRAM copy, `MVN` executed from FastROM costs about 46 master
clocks per byte before external stalls. A fully unrolled A16
`LDA long,X` / `STA absolute,Y` pair can approach 80 clocks per two bytes
(40/byte) with source/destination offsets baked in. A small repeated block
may lose that advantage to pointer updates; benchmark the complete row,
including both pieces. General DMA cannot copy WRAM to WRAM, and DMA registers
cannot supply the source byte for a DMA fill. These constraints also apply to
the experiments later in this plan. [WRAM_tricks.txt](WRAM_tricks.txt)

## 6. Convert fewer times and do less work per conversion

The conversion loop is a separate cost from 177-clock sampling. Keep the
existing sparse maps, ROM floor bypass, ring backpressure and atomic generation
commit. First optimize within their existing memory layout.

### 6.1 Fold pair contributions in reverse order

The full converter currently builds each plane-pair word as:

```text
T0 | (T1 >> 2) | (T2 >> 4) | (T3 >> 6)
```

Change `frame_tile()` in `tools/snes/gen_snes_conv.py` to process source pairs
3, 2, 1, 0 and fold:

```text
acc = T3
acc = (acc >> 2) | T2
acc = (acc >> 2) | T1
acc = (acc >> 2) | T0
```

Keep the previous value in the same ring word initially. For later pairs,
`LDA dp; LSR; LSR; ORA long,X; STA dp` replaces the old shifted-LUT sequence.
Branch on the pair index's plane-7 carry **before** shifting the accumulator;
otherwise the shift destroys the half-table selection.

This halves the shift instructions: 48 to 24 per tile row across four plane
pairs. The unchanged lookup representation has sufficient zero bits to prevent
unwanted movement between its packed plane bytes during these shifts. With
the other instructions kept equivalent, the arithmetic saving is **288 master
clocks per row, 2,304 per full tile, or 36 per full-resolution source texel**.
Confirm the generated instruction count and exact planar output before taking
credit for this saving.

### 6.2 Delay duplicate writes in the half converter

`half_tile()` stores each intermediate plane word into both duplicate rows.
Only the first row is read back to combine the second pair. Remove the second
row's store for the first pair; write both copies only after the final pair.

That removes 16 A16 direct-page WRAM stores per tile. With aligned D and
FastROM instructions, the modeled saving is **448 master clocks per tile**:
28 per unique half-frame input texel, or seven per displayed pixel. This is
safe only because the ring slot is published after the complete tile is built;
the NMI must never see a partially constructed slot.

### 6.3 DMA-register accumulation: benchmark the complete schedule

Reserve eight bytes in channel 1 for four row accumulators; the existing
converter driver's live values are in channels 2–3. Prototype keeping
intermediate plane words in these fast bytes and storing each completed output
word once.

This is an experiment, not an automatic win. An A16 DP read/write saves only
four master clocks when its two data bytes move from WRAM to MMIO. Changing D
away from the ring also makes destination addressing harder. X is the LUT
index, Y is the source pointer, and there is no `LDA long,Y`. Final flushes,
pointer spills, extra loads and D/DB switches can cost more than the saved
scratch traffic.

Compare complete generated alternatives: current ring accumulation plus the
reverse fold; fast row accumulators with a final flush; and any scheme that
keeps one live plane word in a CPU register. Count all instructions and all
slow bytes. Retain the register variant only if it beats the already-improved
ring version on whole tiles, with all four ring phases.

### 6.4 WMDATA reads and larger layout changes

WMDATA is a useful sequential **read** pointer as well as the existing write
pointer. It has one shared auto-incrementing address, not separate source and
destination pointers. CPU A16 access to `$2180` reaches `$2180` and `$2181`;
it does not transfer two consecutive WRAM bytes. Use deliberate A8 accesses.
[Work RAM access specification](https://problemkaputt.de/fullsnes.htm#snesmemoryworkramaccess)

For the current converter, one source row is only eight bytes and the next is
256 bytes away. Eight WMADD setups per tile, byte assembly into pairs, and D/DB
changes may erase the gain. Compare measured schedules before adding this path.
Do not use WMDATA for both source and destination inside one streaming loop.

If conversion remains dominant, prototype a separate tile-band producer that
gathers multiple adjacent cells and retains intermediate results across a
larger unit of work. Evaluate a tile-oriented scratch layout or fused
sampling/conversion for fixed camera poses. Charge for gather/scatter, retained
backgrounds, partial dirty cells and any new memory allocation. Keep the general
chunky path for patches and arbitrary quads until equivalence is established.

Avoid simply multiplying the current pair LUT by four positions: the full
pair tables already occupy five 64 KiB banks, and doubled tables four more.
The reverse fold saves shifts without that table expansion.

## 7. Reduce row setup and arithmetic overhead

### 7.1 Add private assembly entry points

`snesFloorRowsPitch` and `snesCardRows` are assembly, but they still push C ABI
arguments, call math/walkers through `JSL`, clean up the stack, and read
`tcc__r0`. Several row values remain in WRAM solely because these callees reuse
the DMA scratch page.

Keep public C wrappers. Add internal entries with explicit A/X/Y/D/DB and
scratch contracts, returning results in A where appropriate. Put only hot,
callee-preserved row terms in the new fast allocation. For example, retain
row edges, denominator accumulators and counts across pixel spans, while math
uses a disjoint temporary range. Store resumable state back before `job_slice`
returns to code that may draw sprites or change scenes.

Move `fr_*` and `cr_*` incrementally after measuring access frequency. Do not
copy the entire row structure in and out for every short span. For calls that
stay in one bank, compare JSR with JSL; do not assume WLA's `SUPERFREE` sections
remain colocated without a linker constraint.

### 7.2 Schedule the arithmetic units already present

Optimize `snesMulHi` and `snesMulLo` by doing independent operand preparation
and accumulation during the CPU multiplier delay. A16 writes to `$4202`
can submit adjacent 8-bit operands when the packed operand order is already
available. Charge for packing; do not replace two cheap loads with expensive
byte shuffling just to use one store.

The CPU multiply and divide share hardware/result state: do not launch one
before the previous operation is consumed. Their completion requirements are
8 and 16 CPU cycles. Count cycles through the actual result-data read, including
the reading instruction's fetches. The PPU unit is a separate signed 16×8
operation available in the modes used here.
[Hardware multiplication](https://snes.nesdev.org/wiki/Multiplication)

Tasks:

* Audit each explicit NOP sequence in `snesMulHi`, `snesMulLo`, and stamp
  division setup independently. Reschedule useful work before shortening waits.
* Benchmark a PPU-based unsigned high/low product implementation against the
  scheduled CPU implementation. Correct signed-byte decomposition and high-word
  carries explicitly; the existing Q multiply is not a drop-in unsigned multiply.
* Keep an M7A multiplicand loaded across a batch only when no called routine
  can replace it. Consider using the CPU unit during independent PPU work.
* Compare `8192/width` and `4096/height` tables for world stamps with divider
  setup; generate only the supported footprint domain.
* Keep `snesUQDiv`'s saturation and zero-divisor behavior. Its 24-bit numerator
  and 16-bit divisor cannot be replaced generally by the CPU's 16/8 divider.
* Preserve Q multiply's truncation toward zero, including negative fractional
  products, and existing unsigned wrap/carry semantics.

M7A requires two A8 writes to **the same address**. An A16 store to `$211B`
also writes `$211C`; it is not a 16-bit M7A load. Likewise, do not confuse
write-twice PPU registers with adjacent low/high CPU registers.
[PPU register formats](https://snes.nesdev.org/wiki/PPU_registers)

The existing comment in `snes_duel.c` records an actual regression from HDMA
scroll doubling corrupting the shared Mode-7 write latch. Preserve the rule
that NMI/HDMA never writes the multiplier or its shared BG1-scroll latch while
foreground math uses it. `SEI` does not disable either NMI or HDMA.

### 7.3 Precompute exact fixed-camera work

The rest cameras and three lift poses are a small domain. Generate exact
fixed-point row descriptors containing final values used by the walker:
clipped x range, initial source index/fraction, step, texture row, destination
offset, and the span specialization selector. Separate geometry from dynamic
card content so changing a face does not invalidate camera tables.

Start with rest-row depth/du and lift floor rows. Retain dynamic setup for yawed
motion, held-card movement and unsupported poses. Match the current integer
rounding and clipping; do not generate these descriptors with floating-point
approximations to the intended geometry. Compare every supported descriptor
against the existing setup routine.

A proposed initial ROM allowance is 8–16 KiB for selected span variants and
16–64 KiB for row descriptors, expanded only after measured benefit. The
existing debug symbol file's ROM section sizes sum to about 3.07 MB, but that
is an old artifact and not a placement guarantee. Phase 0 must record actual
per-bank free ranges; fixed asset banks and bank-local jump tables constrain
usable space even below the 4 MiB limit.

## 8. NMI, transfers, sprites and other registers

### 8.1 NMI-private fast state

Benchmark moving `nm_budget` and `nm_n` to the channel-7 tail allocation.
`snesFbNmi` currently enters through the SDK with A/X/Y16, DB=`$7E`, and the
SDK's interrupt DP. If the callback changes D or DB, restore both before its
RTL; the wrapper performs further work after the callback. The wrapper itself
already saves the interrupted foreground state.

Keep persistent ring/job indices in WRAM unless their new lifetime is proved.
Never use foreground scratch in the NMI, and never use WMDATA in it while a
foreground stream is suspended. The WMADD registers are write-only, so a
casual save/read/restore sequence cannot protect that pointer.

Measure callback entry cost, OAM completion line, drain completion line,
`nmi_skips`, ring wait time and bytes transferred. The current budget is 3,584
bytes before reservations; raising it just because setup got shorter requires
a separate measured vblank budget. DMA still costs eight master clocks per
byte, and stalls the CPU. Faster CPU work does not make DMA faster.

### 8.2 Transfers and sprite emission

Batch channel-0 DMA setup with correctly sized adjacent-register stores where
possible. Preserve the existing ROM source for fills. Keep long runs in the
sparse job queue and investigate allocator fragmentation only if it creates
many small transfers or ring waits.

For `oam_put`, compare two packed word stores with four byte stores, including
attribute construction and DB setup. Keep high-table updates correct and the
sprite order unchanged. Consider WMDATA only for a complete contiguous shadow
build; random sprite patches and the separate high table are poor streams.
The NMI may DMA a published shadow while foreground work continues, so any
new buffering/publication scheme must preserve that contract.

Audit new DMA scheduling against the S-CPU revision-1 DMA/HDMA overlap issue.
Force blank alone does not stop HDMA. For newly added transfers, use an existing
verified transfer window or explicitly disable HDMA and restore its scene state
at a valid boundary. This is particularly relevant to replacing short CPU fills
with DMA. [SNES hardware errata](https://snes.nesdev.org/wiki/Errata)

### 8.3 WRIO/RDIO and the remaining register inventory

| Resource | Proposed disposition |
| --- | --- |
| `$4201` writes / `$4213` reads, bits 0–5 | Optional six-bit flag storage. Probe retention and ensure every SDK/NMI WRIO writer preserves these bits. |
| WRIO bits 6–7 | Preserve controller policy; bit 7 also controls external counter latching. Do not use these as arbitrary scratch. |
| `$4207–$420A`, IRQ timers | Write-only configuration, not readable RAM. Use timer IRQs only for an independently justified scheduling experiment. |
| `$4202–$4206`, math inputs | Write-only operands; use for computation, not general storage. |
| `$4214–$4217`, CPU math results | Consume as results; the next operation invalidates their previous meaning. |
| `$211B/$211C`, M7 inputs and `$2134–$2136` results | Already computational resources; not general-purpose read/write registers. |
| Other BG/Mode-7/window registers | Mostly write-only or have rendering/latch side effects. An unused layer does not make its registers RAM. |
| `$4218–$421F`, automatic joypad results | Read-only input state; keep input responsive. |
| `$2140–$2143` and mirrors | Directional CPU/SPC communication latches; occupied by audio and not additional RAM through their mirrors. |
| VRAM/CGRAM/OAM data ports | Stateful video memory access, with display restrictions. Not a replacement for CPU scratch RAM. |
| Unmapped gaps, `$43nC–E`, unused upper bits | Do not infer storage from an open-bus read that resembles the last value. |
| Save SRAM | Retain its save role. No custom fast-RAM cartridge or WRAM→SRAM→WRAM staging dependency. |

WRIO's low six I/O bits are unconnected on a standard console, but a masked
write/read can cost more instructions than a DMA-scratch flag. Use it when it
releases a valuable word allocation, not merely because it exists. The SDK's
multitap path writes the **whole** WRIO byte, which is an actual ownership
conflict to resolve even though Super Famicom Box compatibility is out of scope.
[CPU MMIO reference](https://snes.nesdev.org/wiki/MMIO_registers)

## 9. Hardware experiments with bounded scope

These remain worthwhile given the owner's willingness to test unusual behavior,
but the main performance work does not depend on them.

### 9.1 Qualify DMAP and active-channel dead fields

Build a small probe cartridge that tests every byte pattern, walking bits,
word accesses and the B/F alias. Test `$43n0` retention explicitly, including
bit 5, before expanding from 33 to 36 foreground bytes. Run the tail/dead-field
checks during repeated normal DMA, direct HDMA initialization, HDMA line
reloads, table termination, NMI, and scene reconfiguration. Prove that changing
the candidate bytes neither changes output nor gets overwritten unexpectedly.

Use channel-specific patterns to catch unintended aliasing. Report a visible
pass/fail table and checksums in WRAM. Repeat on available S-CPU revisions and
1CHIP hardware; record the actual tested models. Never describe agreement
between emulators as evidence that all physical consoles support a trick.

### 9.2 Tiny code fragments in `$43xx`

Compare a four-byte `MVN`/`RTL` fragment with a FastROM equivalent, or a tiny
dynamic-operand fragment that eliminates real address setup. Use a long call
to bank `$00`: `JSR $4330` from bank `$C0` calls `$C0:4330`, not DMA MMIO.
For a bank-zero RTS fragment, use a bank-zero trampoline instead.

Opcode fetches from this region need a dedicated hardware probe. Exclude
unqualified DMAP bytes and never fetch across the C–E gap or into the F alias.
Keep the ordinary WRAM stack and permit interrupts only after verifying the
entire fragment's ownership and CPU-state behavior. Write all bytes before
publishing its entry point.

There is no fetch-speed advantage over FastROM. The potential benefit is
dynamic specialization or a small self-modifying block-move operand; include
construction, calls and bridges between windows in the benchmark. A fragment
that merely duplicates FastROM code should be discarded.

### 9.3 A temporary MMIO stack

Test separately from the renderer. A native-mode stack placed at `$433B` has
only a tiny valid descending range: through `$4331` conservatively, or `$4330`
after DMAP qualification. Save the original S outside the temporary stack and
restore it before returning. Probe push/pull ordering, all operand widths and
maximum depth without crossing a window.

NMI must not arrive there: its entry plus the SDK's register saves exhaust the
window. `SEI` is insufficient. Use an isolated test with a defined NMITIMEN
shadow and carefully managed re-enabling; re-enabling NMI during vblank can
trigger it immediately. Avoid any normal calls or implicit interrupt stack
use while S points at MMIO.

Each stack data byte saves only two clocks. Include relocation and protection
costs, lost register-file capacity, and delayed input/audio/upload service. Keep
this out of normal gameplay unless a narrowly bounded leaf routine produces
a substantial measured benefit. Do not move the normal game stack into page
zero merely to get a convenient TSC value; its existing ABI and RAM allocations
matter more than that micro-optimization.

## 10. Measurement and verification

### 10.1 Establish a reproducible baseline

Build current retail and debug ROMs, record source revision/diff, ROM and symbol
hashes, SDK identity, emulator identity and actual per-bank placement. Archive
profiles and input scripts with those identities. Existing build artifacts
must not silently stand in for a fresh baseline.

The current profile hook attributes elapsed clocks between instruction fetches;
it can charge DMA stalls to the initiating instruction and include refresh or
interrupt entry at boundaries. Its totals are not automatically pure kernel
execution time. Add symbol-range/kernel markers and emitted-texel counts, with
counters updated outside the timed loop. Record pure instruction costs and
wall-clock phase costs separately.

Instrument whole generations, not just a fixed field interval: after an
optimization, the same interval may contain a different pose or idle time.
Capture setup, sampling, conversion, producer waits, NMI/DMA and final commit.
Use ablations and the existing `map_lines`, `conv_lines`, `render_lines`,
`nmi_skips`, `dropped`, `occupied` and generation fields as cross-checks.

### 10.2 Required benchmark matrix

| Workload | What it distinguishes |
| --- | --- |
| Empty board and full fixture, both seats | Floor-only work versus cards; initial bake versus idle |
| Cursor move, placement, held card, rotated defence | Small patches, clipped/affine spans and turned texture stamps |
| Each lift/descent pose and yawed turn | Constant-v versus general DDA; 1:1 versus doubled conversion |
| Overhead at unit step | MVN/copy path, row wrapping and 320-cell presentation |
| Full and half converter pattern fixtures | Every LUT half, pixel position, ring phase and duplicate row |
| Long and fragmented upload runs | Ring pressure, allocation, NMI setup and commit latency |
| Title, dialogue, deck, card check, battle, wipe, return to board | Every channel-5/HDMA ownership transition |
| Audio playing during heavy rendering; save/reload | No input/audio/SRAM regression from register reuse |

For each kernel record: implementation, source kind, count distribution,
step/fraction distribution, boundary splits, setup clocks, steady-state clocks,
tail clocks, total sampled texels, code/table bytes and output hash. Report
weighted totals, not an average of per-span averages.

### 10.3 Correctness gates

* Differential-test all supported specialized fractions, counts 0–256,
  initial fractional carries, row ends and both source banks. Exercise signed
  fallback steps, sheet switches, wrapping and clipped cards.
* Compare chunky bytes and decoded planar pixels, including all eight color
  bits. For the reverse fold, exhaust the packed plane-pair combinations and
  compare against the independent reference encoder.
* Test all four ring phases, wraparound, backpressure, dirty ROM-floor cells,
  copy-on-write allocation, cancellation and commits. No partial tile may
  become visible; retain the 351-cell overflow behavior.
* Check arithmetic at zero, powers of two, `$7F/$80/$FF` byte boundaries,
  `$7FFF/$8000/$FFFF`, sign combinations, high-product carry cases,
  truncation and saturating division. Compare full results, not just pictures.
* Inject NMI around XBA pairs, D/DB transitions, M7A writes and DMA setup;
  exercise HDMA line/frame reloads with scratch canaries.
* Inspect observable output from the existing scene/renderer harness. A build
  success or emulator that remains running is not sufficient.

The bundled accurate core's CPU ALU implementation computes a result and hides
it behind `alu_lock`; early reads return zero. It does not model all hardware
intermediate products. Therefore emulator acceptance alone cannot establish
the minimum safe arithmetic delay. Confirm that schedule with bus-cycle
accounting and a dedicated hardware latency probe.

### 10.4 Existing commands and planned additions

These commands exist today; the profiling tool's field windows are starting
points from its own usage text, not fixed benchmark boundaries:

```sh
make -f Makefile.snes verify
python3 tools/snes/prof.py v2_rest 2500 2599 R@2200
python3 tools/snes/prof.py v2_lift 2400 2440 R@2200 UP@2400
python3 tools/snes/verify.py --emulator SNES/snesfaust/snesfaust-mednafen --output build/snes/verify-v2-faust
```

`prof.py` currently uses `SNES/snes-headless/snes-mednafen`, while `verify.py`
defaults to a different accurate-core binary. Record which binary produced
each result. Cross-core checks must also record core limitations.

Add, as implementation work, a SNES-only register probe ROM, a differential
kernel/math harness, generated allocation assertions and phase-aware profiling.
Do not present these as existing Makefile targets. Extend the current render
cost check with explicit baseline-relative thresholds: today it mostly checks
that timing exists and the ablated board is cheaper, not that an optimization
met a performance target.

## 11. Delivery order and acceptance criteria

| Phase | Deliverable and main files | Gate before proceeding |
| --- | --- | --- |
| 0 | Baseline manifest, per-stage profiles, span histograms, bank map; `prof.py`, `verify.py`, stamp/probe tooling | Correct denominator, matching ROM/symbols, known existing failures separated from new regressions |
| 1 | Register ownership map and assertions; `snes_fastdp.inc`, scene/DMA audit, probe | No scratch clobber at NMI/HDMA/scene boundaries; qualify new bytes individually |
| 2 | B-fraction k=0/k=1 paths, eight-texel generic/stamp blocks, exact dispatcher; `snes_raster.asm` | Pixel-identical; profitable spans approach the modeled 142–148 ROM clocks for k=0; no short-span regression from dispatch |
| 3 | Reverse-fold full conversion and delayed half-row stores; `gen_snes_conv.py` | Exact full/half planar output; expected instruction savings survive complete tile/driver measurements |
| 4 | Assembly calling contracts, hot row state and scheduled arithmetic; `snes_math.asm`, `snes_raster.asm`, `snes_board3d.c` | Exact math, hardware-safe waits, lower setup/row cost with normal interrupts |
| 5 | Selected immediate kernels and fixed-camera descriptors; new generators plus `Makefile.snes` | Measured coverage justifies ROM cost; modeled 130–136 path demonstrated where selected; all banks remain valid |
| 6 | Converter scratch experiment, NMI-private words, copies/OAM and upload tuning | Each change beats the improved baseline including setup, stalls and final publication |
| 7 | WRIO flags, MMIO code and stack experiments | Hardware-qualified and worth their complexity; otherwise retain them only as documented experiments |

Proposed performance gates, to lock after phase 0:

* At least **10% lower weighted sampling time** over the fixture/lift corpus,
  with the same sampled output; aim for more as bounded spans gain coverage.
* At least **10% lower full-tile conversion time**, without trading it for
  additional ring waits or extra sampling. Track half conversion separately.
* Aim for **15–25% lower time to completed board presentation** on expensive
  bakes/motion poses after the combined phases. This is a target, not an
  extrapolation of the fastest microkernel to the whole game.
* No changed image, reduced resolution, omitted camera pose, extra dropped
  frame, increased NMI overrun, or degraded audio/input responsiveness counted
  as a performance success. Keep a fallback where a specialization loses.

Weight the projected benefit by observed workload coverage. If sampling is a
fraction `f` of total work and becomes `s` times faster, its isolated total
speedup is `1 / ((1 - f) + f / s)`. Conversion and vblank publication can become
the next limit; recompute the profile after each accepted phase rather than
adding independent percentage claims together.

The first implementation batch should therefore pair **the B-fraction span
kernel and exact boundary dispatcher** with **the two converter reductions**.
Expand the register file where it removes measured spills or preserves useful
state across calls. This gives the register experiments a concrete job and
provides a credible route below 177 without depending on custom hardware or
unverified MMIO behavior.
