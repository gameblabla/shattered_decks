# FM TOWNS Marty 2-D Battle Performance Plan

Date: 2026-08-27

Status: source-backed implementation plan; no code, build, or emulator run was performed while writing it.

Primary target: FM TOWNS Marty / 16 MHz 80386SX / 16-bit external bus / 2 MiB RAM.

Scaling target: the same binary must also take advantage of 386DX and 486/Pentium-class FM TOWNS machines selected at runtime. Faster tiers may deliver more of the existing animation frames, but must not receive different art, reduced effects, lower colour depth, or other visual substitutions.

## 1. Required outcome

Speed up the interactive two-card 2-D battle cut-in substantially on a Marty while preserving the exact 256x240, indexed 8bpp image produced by the current game.

The optimization must not:

- reduce card-art resolution;
- remove, shorten, simplify, or thin the flip, ram, flash, damage-text, or burn effects;
- reduce the 256-colour presentation to 4bpp;
- replace the current indexed presentation with a lossy approximation;
- introduce additional intentional render skipping;
- alter the 60 Hz game/animation timeline;
- change battle outcomes, SFX cue crossings, or phase durations;
- regress the two-page dirty-update history;
- spend another full-screen cache, because the Marty link currently has only 7,936 bytes between `.bss` and the 2 MiB stack ceiling.

“Faster” means less CPU and bus work per rendered cut-in frame, followed by more rendered frames per second. It does not mean making the animation finish early or hiding work by increasing the existing logic-frame step.

## 2. Evidence and limits of the current numbers

The previous session established the following last usable calibrated-Marty figures before the emulator display service failed:

| Scene | Calibrated rate | `waifu_fm_step()` | Present work |
| --- | ---: | ---: | ---: |
| Card placement | 14.9-15.9 fps | 34.4-37.0 ms | 21.2-22.0 ms |
| Two-card battle cut-in | 9.0-11.0 fps | 56.8-75.3 ms | 24.7-27.1 ms |

These figures are retained as the historical reference because they describe the reported problem and split it into game-step and presentation costs. They are not a fresh baseline for the current `b030e87` tree: they were captured before the follow-up battle retention, clipped-card, scaler, turn-anchor, and runtime-tier commits were all complete.

Structural evidence after those timings is stronger than “nothing changed,” but weaker than a new frame-rate claim:

- normal cut-ins declare an average 705 of 960 presenter groups instead of forcing all 960;
- direct attacks declare about 473 of 960 groups;
- the FM TOWNS 112x112 art path already uses `rep movsl`, four indexed pixels per 32-bit CPU transfer;
- the dirty presenter already writes each eight source pixels as one 32-bit store to each of the two swizzled VRAM banks;
- 420 deterministic cut-in frames matched the forced-full-restore reference byte for byte;
- the damage verifier reported no undeclared pixels;
- gameplay time advances by the measured number of 60 Hz periods, so a 60-100 ms rendered frame legitimately advances several logic frames.

The physical-hardware ground truth is also narrow and important: the current game and CD-ROM path run on real FM TOWNS hardware, as confirmed by the owner and `IMG_6472.mov`. That proves the port is real, but it is not a measured frame-time capture of this exact cut-in. Tsugaru’s default timing string remains a comparison proxy:

```text
-FREQ 16 -CPUCLOCKSCALE 220 -BUSWAIT 2 -VRAMBUSWAIT 6 -DATABUSWIDTH 16
```

All emulator performance conclusions must therefore be phrased as like-for-like changes under the calibrated proxy. A final physical-Marty stamp is required before claiming a real-hardware fps number.

## 3. Current source path and diagnosis

### 3.1 Frame construction

The interactive battle reaches `draw_interactive_battle()` in `src/main.c`. After the tactical prelude it calls `draw_battle_cutin_event_ex()`.

Each visible cut-in frame currently does the following:

1. `restore_solid_screen(IDX_BLACK)` restores the previous overlay footprint to black.
2. Both battle cards are rebuilt by `draw_cutin_battle_card()` / `draw_big_battle_card_stats()`.
3. Each card rebuild draws:
   - drop shadow;
   - outer 120x160 frame fills and outlines;
   - the 112x112 art;
   - the 110x28 stat panel;
   - ATK, DEF, and tribe text.
4. The attack and defender names are cleared and redrawn in fixed black bands every frame.
5. Dynamic phases additionally draw one or more of:
   - horizontally scaled flip silhouettes/art;
   - lunge/shake positions;
   - a per-pixel checker flash over the 112x114 art region;
   - centered damage text;
   - a bottom-to-top burn erase and 84 ordered procedural flame discs.

`blit_art112_fast()` is already a sound packed path on FM TOWNS. Its `rep movsl` loop moves 28 dwords per row, preserving four 8bpp pixels in a 32-bit register at a time. Replacing that with another ordinary copy loop is not the solution.

The remaining problem is that the packed art copy, frame construction, glyph work, and damage declarations are repeated even when a card and its labels are unchanged. On a cacheless 386SX, redrawing fewer bytes is worth more than shaving a few instructions from a byte loop.

### 3.2 Expensive exact effects

Two effect helpers reintroduce per-pixel overhead:

- `draw_big_battle_card_hit_flash()` loops over roughly 112x114 pixels and calls `put_px()` for three pixels out of every four.
- `draw_disc()` tests every point in a square and calls `put_px()` for every pixel inside the disc. The burn path invokes it 84 times in strict painter order.

These helpers repeatedly clip, mark damage, compute a framebuffer address, and write one byte. They are ideal for packed, byte-identical kernels after retained composition removes the larger redundant redraw.

### 3.3 Presentation

`fmt_put_image_dirty_rows()` in `src/platform/fmtowns/common/libfmt.c` presents the 256x240 framebuffer in 64-byte groups. The FM TOWNS single-page 8bpp swizzle maps each aligned eight source bytes to:

- pixels 0-3: one 32-bit store in the low VRAM bank;
- pixels 4-7: one 32-bit store in the high VRAM bank.

This is already the desired four-pixels-per-register packing. A 386SX performs each 32-bit VRAM store as two external 16-bit bus cycles; that is still preferable to four byte stores.

The remaining hardware concern is destination ordering. The current writer alternates low-bank and high-bank stores for every source pair:

```text
low dword, high dword, low dword, high dword, ...
```

If physical VRAM benefits from page-mode locality, that ordering can cause a bank/row transition every store. A bank-linear writer would preserve the same packed pixels but issue:

```text
all low-bank dwords for a dirty run, then all high-bank dwords for the run
```

No source dword needs to be loaded twice. The low pass reads source dwords 0, 2, 4, ...; the high pass reads 1, 3, 5, .... The total remains one source load and one VRAM store per four pixels. The trade is two strided source traversals and an extra pass branch, which must be measured.

Tsugaru’s present timing models bus waits but not this proposed VRAM row/bank locality. Therefore the bank-linear writer is a physical-hardware hypothesis with an emulator non-regression gate, not an emulator-proven win.

### 3.4 Memory

The FM TOWNS build currently owns three full-screen battle cache objects:

- `g_b_base_cache`: resting hand-side composite;
- `g_b_base_cache_top`: top/enemy-top composite and endpoint cache;
- `g_b_fmtowns_work_cache`: one reusable scratch camera composite used for placement, lift, and turn anchors.

The cut-in does not need the scratch camera image and a moving camera image simultaneously. Its `pixels[61440]` allocation can be borrowed during the black 2-D cut-in, then invalidated so the next 3-D user rebuilds it normally. This is the only acceptable source of a full-screen cut-in cache; adding a fourth screen would overflow the Marty memory budget.

Only small metadata may be added. The initial target is less than 256 bytes of additional `.bss`; the hard acceptance gate is that the linked image retains a meaningful stack margin and never crosses `0x200000`.

## 4. Chosen optimization architecture

The main change should be a retained canonical cut-in composite stored in the existing `g_b_fmtowns_work_cache.pixels` buffer.

### 4.1 Canonical composite

The canonical image is the exact current black cut-in frame containing:

- the attacker card face at its resting `ax, ay` position;
- the defender card face at its resting `dx, dy` position;
- live displayed ATK/DEF values;
- both current card names in their existing locations;
- no transient flash, damage text, burn, flip, slide, shake, or lunge overlay.

The first time the stable face-up arrangement is fully rendered for a battle event, copy the resulting 61,440-byte framebuffer into `g_b_fmtowns_work_cache.pixels` and attach a cut-in key. This one-time RAM copy is acceptable if it removes dozens of later card rebuilds and most VRAM changes.

The key must include every input that can change a canonical pixel:

- attacker and defender card IDs;
- attacker and defender displayed ATK and DEF;
- face/back state represented by the canonical image;
- attacker/defender ownership if it changes layout or displayed stats;
- palette/layout-affecting mode information if any is introduced later;
- 256-wide UI geometry (or the existing extra-width input if the helper is made generic).

The key is metadata only. Do not use a probabilistic hash as the sole correctness check if the full fields fit in a small struct.

### 4.2 Retained frame rules

Each rendered cut-in frame is classified before drawing:

1. **Canonical hold**
   - Both cards are at rest.
   - No transient overlay changes.
   - The canonical framebuffer is already present.
   - Retain it and draw nothing.

2. **Overlay-only change**
   - The canonical base remains valid.
   - Restore only the prior overlay groups from the canonical cache.
   - Draw the new flash, damage text, burn overlay, or other transient pixels.

3. **One moving card**
   - Restore the union of the card’s previous and new footprints from the canonical base or black, as appropriate.
   - Remove the canonical copy at its resting position when the moving card is away from rest.
   - Redraw the moving card.
   - Redraw the stationary card only if the restored/moving region overlaps it.

4. **Two moving cards / slide-in / flip transition**
   - Use the existing black retained base during the incoming slide.
   - Draw only the visible cards and fixed labels.
   - Switch to the canonical composite at the first exact stable face-up frame.

5. **Base/key change**
   - Render the complete reference frame once.
   - Capture it as the new canonical cache.
   - Reset overlay history so neither VRAM page can retain pixels from the old event.

The retained state must track previous card bounds, previous dynamic phase, and whether the canonical base is actually the framebuffer currently in RAM. A valid cache object alone is insufficient: a full-screen draw by another phase invalidates framebuffer retention even if the cached bytes remain valid.

### 4.3 Painter-order and overlap rule

The current draw order is attacker, defender, damage/effect overlays, then name bands. That order is part of the output.

During a lunge, the attacker can overlap the defender. Any restored rectangle that intersects the defender must redraw the defender before the moving attacker is redrawn, preserving the original order. It is not safe to update “only the moving sprite” without this overlap test.

The simplest correct region algorithm is:

1. Calculate the union of previous and current dynamic card/effect bounds.
2. Restore that union from the canonical composite.
3. Blank the canonical resting card footprint if that card is currently displaced.
4. Redraw any canonical card whose pixels were affected by steps 2 or 3.
5. Draw displaced cards in the original painter order.
6. Draw flash, damage, burn, and names in their original order.

The implementation may later narrow this further, but it must first pass byte-exact comparison in this conservative form.

### 4.4 Scratch-cache ownership

Add explicit ownership for `g_b_fmtowns_work_cache`, for example:

```text
WORK_CACHE_NONE
WORK_CACHE_CAMERA
WORK_CACHE_CUTIN
```

Rules:

- entering a cut-in may evict only the reusable work cache;
- `g_b_base_cache` and `g_b_base_cache_top` remain intact;
- any camera-cache lookup must reject a `WORK_CACHE_CUTIN` buffer;
- leaving the cut-in clears cut-in metadata and marks the work camera cache invalid;
- the next placement/lift/turn camera rebuilds on demand;
- cache ownership changes must also reset the framebuffer damage base when the source pointer changes.

This makes the memory reuse explicit and prevents a valid camera key from accidentally interpreting cut-in pixels as a 3-D board.

## 5. Implementation work packages

Each package is independently measurable and should land only after its own visual gate.

### Package A: Cut-in-specific attribution

Purpose: establish where the current post-`b030e87` frame time goes without changing output.

Add counters/timers for:

- full battle-card construction calls;
- 112x112 art-copy calls and bytes;
- card-frame/stat/text construction time;
- flip scaler calls, widths, and time;
- flash calls/pixels;
- burn calls/disc spans;
- solid/composite restore bytes;
- canonical-cache builds, restores, retained frames, and evictions;
- declared groups, hashed groups, forced groups, and VRAM-written groups;
- low/high bank bytes written.

Host profiling can use the existing `WAIFU_PROFILE_RENDER` mechanism. FM TOWNS diagnostics must stay compact enough for the frame stamp or an optional measurement bar; no `stdio` or filesystem calls may enter the console core.

Measurement switches must be compile-time-only and must not enter the shipping build.

### Package B: Canonical retained cut-in base

Purpose: remove redundant rebuilding of static cards and labels.

Steps:

1. Add work-cache ownership and cut-in metadata.
2. Detect the first exact stable face-up frame.
3. Render it through the unchanged current code.
4. Copy the whole framebuffer into the borrowed work cache once.
5. Use `fb_restore_composite()` semantics against the canonical source on following dynamic frames.
6. Retain the framebuffer entirely on unchanged hold frames.
7. Restore only prior overlay groups before changed overlays.
8. Invalidate cleanly at phase exit, battle snapshot change, result transition, and global composite-cache invalidation.

Expected effect:

- static reveal pauses and post-impact holds should approach zero game-step drawing work;
- stationary cards should stop declaring their full 120x160 footprints every frame;
- the name bands should stop declaring fixed top/bottom regions every frame;
- present work should fall with step work, because fewer framebuffer groups actually change.

This package should be implemented before writing more assembly. It attacks both halves of the measured 9-11 fps frame.

### Package C: Dynamic card-region compositor

Purpose: avoid rebuilding the whole pair when only one card moves.

Add a small `CutinVisualState` describing:

- card positions and visibility;
- back/face/flip width state;
- flash parity/state;
- burn frame/state;
- damage-text visibility and content key;
- calculated bounds;
- canonical-base validity.

Compare previous and current states to calculate the minimal conservative redraw region. Use rectangle intersections to determine which static card must be re-composited after a restore.

Do not start with per-pixel region subtraction. Rectangle unions aligned outward to the presenter’s 64-pixel groups are simpler and safer. Once byte identity and performance are established, finer regions can be considered if the presenter remains dominant.

Special cases:

- The incoming slide is clipped at screen edges; bounds must be clipped before damage marking.
- Shakes move in two-pixel increments and create unaligned 32-bit destinations; x86 permits these, but they may cost an extra 386SX bus cycle.
- The lunge can overlap the defender and must preserve attacker-over-defender painter order.
- The destruction outcome may remove one or both cards after burn completion.
- `BATTLE_NO_DESTROY` and direct attack do not share all normal pair assumptions.
- A face-down attacker or defender must keep the current flip silhouette/art threshold exactly.

### Package D: Packed, pixel-exact flash kernel

Purpose: replace thousands of `put_px()` calls without changing the checker.

The flash condition repeats every four x pixels:

```c
((x + y + phase * 3) & 3) != 0
```

For each row:

1. Compute the four-byte keep/white mask from `y` and `phase`.
2. Process the 112-pixel art span as 28 dwords.
3. Read one dword, preserve the one non-white byte, and set the other three to `IDX_WHITE` in registers.
4. Store the dword back.
5. Handle any clipped head/tail bytes with the scalar reference logic.

The destination may be unaligned during shake. Keep a scalar/reference define and compare every flash phase at each x alignment used by the battle.

Do not force every flash group as changed blindly unless its output is known to differ from the prior frame. The damage system can declare the region while the hash rejects unchanged groups; the retained compositor should ensure the region is already narrow.

### Package E: Span-based, pixel-exact discs and burn overlay

Purpose: remove per-pixel clipping/address overhead from flames.

For each `yy` from `-r` to `r`, calculate the maximum `xx` satisfying:

```text
xx*xx + yy*yy <= r*r
```

Then draw one inclusive horizontal span with the existing clipped row-fill primitive. Preserve:

- the exact set of pixels selected by the old inequality;
- disc invocation order;
- colour overwrite order;
- all random seeds and positions;
- the white-hot fringe;
- the exact burn erase edge.

Once the canonical card is present, split burning into:

- canonical card pixels supplied by the retained base;
- the current black erase rectangle;
- flame-disc spans;
- white-hot fringe.

The upper, still-visible portion of the card must not be copied again.

### Package F: Bank-linear packed VRAM writer

Purpose: exploit four-pixel CPU registers and sequential writes to each half of the split 8bpp VRAM organization.

Implement a helper that writes a contiguous run of one or more 64-byte dirty groups:

1. Low-bank pass:
   - load source dwords 0, 2, 4, ...;
   - store sequential dwords to the low VRAM bank.
2. High-bank pass:
   - load source dwords 1, 3, 5, ...;
   - store sequential dwords to the high VRAM bank.

Properties that must be visible in the compiled i386 object:

- one 32-bit load per four source pixels;
- one 32-bit store per four destination pixels;
- no byte-at-a-time swizzle loop;
- no multiply/divide in the inner loop;
- destination stores monotonically increase within a bank pass;
- unrolling sufficient to amortize loop control without exhausting the 386’s eight registers;
- no FPU or 486-only instructions in the Marty binary.

Integrate it in two places:

- dirty/forced group writes in `fmt_put_image_dirty_rows()`;
- the full-width fallback in `fmt_put_image()`.

For the dirty path, first resolve the row’s hash/force decisions into a four-bit write mask, then merge adjacent set bits into runs. This avoids switching bank-pass setup for every single group.

Keep `FMTOWNS_VRAM_INTERLEAVED_REFERENCE` (name may vary) as a measurement and pixel-reference switch. It must be off in the final shipping build only after evidence supports the new default.

This package has three separate gates:

1. VRAM contents/page output are pixel-identical under Tsugaru.
2. The calibrated Marty proxy does not regress materially.
3. Physical Marty evidence shows equal or better present time/visible delivery. If physical evidence shows no benefit, retain the simpler/faster measured ordering instead of assuming page-mode behavior.

### Package G: 386SX through 486 scaling

Purpose: let faster machines turn the same work reduction into smoother delivery without maintaining divergent graphics.

Rules:

- The canonical compositor and packed effects are identical on all CPU classes.
- Do not give Marty fewer particles, lower-resolution art, shorter cards, or a reduced effect sequence.
- Do not add new tier-based animation constants.
- Existing runtime camera anchor counts remain 3/7/5 for 386SX, 4/9/7 for 386DX, and 5/15/9 for 486/Pentium; this plan does not lower those values.
- The 2-D cut-in should have one visual implementation. Faster hardware naturally renders more intermediate 60 Hz logic frames because it misses fewer fields.

The VRAM writer may select a CPU-class-specific unroll or destination ordering only if:

- the branch occurs outside the hot inner loop;
- all paths write identical VRAM bytes;
- each path wins on its intended machine/model;
- unknown machine IDs take the safe 386SX path.

`WAIFU_FMTOWNS_FORCE_PERFORMANCE_TIER` remains a measurement override, never a shipping policy.

## 6. Skip-aware performance method

The existing wall-clock pacing makes naïve frame-count comparisons invalid at low speed.

At the historical 9-11 fps rate, one rendered cut-in frame can consume roughly five to seven 60 Hz periods. The core correctly advances by that amount. If an optimization reaches 20 or 30 fps, it will render more distinct animation states during the same logic-time cut-in. That means:

- total rendered frames in the scene increase;
- average work per rendered frame may change because more intermediate flip/lunge/burn states become visible;
- a fixed count of rendered frames no longer represents a fixed portion of the animation;
- a faster build can legitimately do more total rendering work over the phase while still completing it more smoothly in the same wall-clock duration.

Every performance report must include both per-render and per-logic-timeline metrics:

| Metric | Why it is needed |
| --- | --- |
| Average `step` time per rendered frame | CPU compositor cost |
| Worst `step` time | visible hitch / phase boundary cost |
| Average present work per rendered frame | VRAM path cost |
| Worst present work | full-page or cache-transition spikes |
| Pacing-step histogram (1, 2, 3, ...) | actual skipped 60 Hz states |
| Rendered frames from cut-in start to end | delivered visual density |
| Logic frames from start to end | confirms unchanged animation duration |
| Wall time from start to end | user-visible duration |
| Declared / hashed / written groups | explains present changes |
| Canonical cache hit/build counts | explains compositor changes |

Use two modes for different questions:

1. **Production pacing**: normal FM TOWNS wall-clock step. This is the performance result.
2. **Fixed logic step (`WAIFU_FM_FIXED_LOGIC_STEP`)**: one logic state per rendered frame. This is only the deterministic pixel oracle and per-state workload study; it is not a shipping performance number.

Compare equivalent logic ranges, not “the next 420 rendered frames,” in production-pacing captures.

## 7. Measurement matrix

### 7.1 Marty proxy

Use the existing default `FMTOWNS_TIMING`, `-TOWNSTYPE MARTY`, and `-MEMSIZE 2`.

Required comparisons for every performance package:

- current tree/reference define;
- candidate implementation;
- candidate with its central optimization disabled;
- three captures after the parked scene reaches stable repeatable logic ranges;
- decoded frame/state/timing stamps;
- same deck and battle outcome.

The first execution pass must establish a fresh post-`b030e87` baseline. Historical 9-11 fps remains the problem reference, not a substitute for this fresh number.

### 7.2 Faster models

Use the same binary and appropriate ROM/model combinations for at least:

- Marty/386SX-class;
- a 386DX-class TOWNS model;
- a 486-class TOWNS model.

Do not call a Marty-calibrated wait configuration an accurate 486 measurement. Use it for controlled relative scaling only, clearly labelled. Physical/model-appropriate timing is needed for absolute faster-machine claims.

On all three classes verify:

- runtime CPU detection selects the expected tier;
- cut-in pixels are the same for the same fixed logic state;
- phase duration in logic frames is unchanged;
- faster hardware delivers at least as many intermediate frames;
- no machine takes the narrow/wide VRAM base incorrectly.

### 7.3 Physical Marty

After emulator and headless gates:

1. Run the exact deterministic battle profile on physical hardware.
2. Capture a clear photo/video containing the debug stamp.
3. Decode it with `tools/fmtowns/read_frame_stamp.py`.
4. Record average/worst step, present, and pacing step.
5. Capture both reference and candidate if practical; otherwise compare the candidate to a reference capture made from the immediately preceding build on the same setup.
6. Watch specifically for alternating-page artifacts, horizontal stripes, stale card remnants, palette tearing, and burn/flash corruption.

Physical hardware is authoritative for the bank-linear VRAM-order decision.

## 8. Pixel and gameplay verification

### 8.1 Deterministic framebuffer comparison

For each candidate package, build a production path and a reference path from the same source revision. Run with fixed logic step and compare the 256x240 indexed pixel payload for every logic frame, not selected screenshots.

Required sequences:

- normal attacker destroys defender;
- defender survives and destroys attacker;
- both destroyed;
- no-destroy/damage case;
- direct attack;
- face-up attacker and defender;
- face-down attacker;
- face-down defender;
- player attacks;
- COM attacks;
- slide clipping at both screen edges;
- every flip width reached by Marty logic skipping and by step=1;
- lunge overlap;
- every flash parity;
- damage-text entry, hold, and exit;
- every burn frame through full disappearance;
- result/return transition after the cut-in.

The pass condition is zero changed pixels over the whole sequence.

### 8.2 Damage verifier

Run `WAIFU_FB_DAMAGE_VERIFY` for all the sequences above.

The pass condition is:

- zero undeclared pixels;
- no forced-full fallback introduced to hide missing damage;
- no ever-growing overlay mask;
- no retained overlay surviving a cache-owner/base change;
- correct current+previous-page debt handling.

Also compare against `WAIFU_FB_DAMAGE_FULLRESTORE`. Production and full-restore output must be identical frame for frame.

### 8.3 Visual inspection

Byte comparison proves equivalence to the current software output but does not replace inspection of transitions and page behavior.

Inspect contact sheets and consecutive frames for:

- complete gold rims and shadows;
- unchanged 112x112 art sharpness;
- correct ATK/DEF colours and values;
- tribe and name text;
- exact flip silhouettes;
- no clipped shake/lunge edges;
- attacker/defender overlap order;
- checker flash coverage;
- damage text centering;
- bottom-to-top burn shape and colours;
- disappearance timing;
- black restoration around old positions;
- every-other-frame or one-page-only corruption.

### 8.4 Shared-core regression gate

Because most compositor work lives in `src/main.c`, run the headless regression gate after each common-code milestone:

```text
make regression-story
./waifu_fm_headless_cdrom_2mb --regression-story-all --frames 1 --no-png
```

Then build and visually verify every console target affected by the shared change. FM TOWNS is mandatory; PC-FX and CD32X must also be rebuilt if their compiled shared path changes, even when the optimized branch is FM-only.

Generated assets are not part of this plan and must not be hand-edited.

## 9. Acceptance targets

### Correctness: mandatory

- Zero pixel differences against the reference for every deterministic cut-in frame in the test matrix.
- Zero framebuffer damage-verifier violations.
- No missing SFX cue caused by multi-frame stepping.
- Unchanged logic-frame phase durations and battle results.
- Correct pixels on both alternating VRAM pages.
- No palette, colour-depth, art-resolution, effect-density, or animation-content downgrade.
- FM TOWNS payload remains below `0x90000`.
- `.bss` plus stack remains below `0x200000` with a reviewed safety margin.
- No measurement define in the shipping build.

### Performance: milestone gates

Because the historical number predates the latest structural changes, use relative and absolute gates together:

1. **Fresh baseline gate**: record current post-`b030e87` Marty-proxy `battle` and `direct` profiles before implementation.
2. **Retained-compositor gate**: at least 35% lower average cut-in `step` work than that fresh baseline over the same logic range, with lower or equal present work.
3. **First user-visible gate**: at least 2x the historical 9-11 fps delivery in the representative two-card cut-in, or a documented explanation if the fresh baseline already exceeds that due to the completed follow-up.
4. **Primary target**: sustained representative cut-in delivery at or above 20 fps on the calibrated Marty proxy, with no multi-frame worst-case hitch above 100 ms.
5. **Stretch target**: approach 30 fps by bringing typical combined step+present work below 33.3 ms.
6. **Present target**: drive unchanged/overlay-light holds below one 16.7 ms field of work; dynamic full-card frames may exceed it but must improve from the fresh baseline.
7. **Scaling gate**: 386DX and 486-class runs deliver monotonically more frames and never select a lower visual path.

Do not claim 60 fps unless both typical and worst representative cut-in frames fit one field and the result is observed, not extrapolated.

## 10. Stop/rollback criteria

Stop or revert an approach if any of the following occurs:

- it changes a production pixel without an explicit user-approved visual redesign;
- it needs another 61,440-byte allocation;
- it reduces the Marty stack margin below a defensible level;
- it makes the fast path depend on 486-only instructions;
- it makes unknown CPU IDs unsafe;
- it adds a third page of framebuffer-history state that is not proved necessary;
- it improves emulator numbers only by increasing logic skipping;
- it reduces average time but introduces visible worst-frame stalls;
- the bank-linear VRAM writer is slower on physical Marty;
- the retained state cannot be made robust across cache eviction and phase transitions.

Fallback order:

1. keep canonical retention, drop overly fine region tracking;
2. keep packed effects, retain scalar reference behind a define;
3. keep the measured VRAM store order, regardless of theory;
4. preserve exact graphics and accept a smaller speedup rather than introduce a downgrade.

## 11. Execution order and commit boundaries

Recommended sequence:

1. **Instrumentation and fresh baseline**
   - no visual change;
   - record current post-`b030e87` Marty, direct, and fixed-step data.
2. **Work-cache ownership**
   - no visual change;
   - explicit camera/cut-in lifetime tests.
3. **Canonical retained cut-in**
   - static holds and labels;
   - byte-identical 420-frame normal cut-in gate.
4. **Dynamic region compositor**
   - slide, flip, lunge, overlap, result transition;
   - full outcome matrix.
5. **Packed flash**
   - all phase/x-alignment comparisons.
6. **Span burn/discs**
   - all burn frames and painter-order comparisons.
7. **Bank-linear VRAM run writer**
   - disassembly, emulator page checks, physical Marty decision.
8. **CPU-class scaling audit**
   - Marty/386DX/486 same-state pixels and delivered-frame counts.
9. **Full cross-target regression and documentation**
   - update `src/platform/fmtowns/STATUS.md` with observed evidence;
   - distinguish emulator proxy from physical results.

Each commit should contain one performance idea plus its verification evidence. Do not combine retained composition, effect assembly, and VRAM ordering in one commit; they have different failure modes and need to be bisectable.

## 12. Agent and tool policy for execution

This document was planned by Sol through source inspection only.

For the implementation phase:

- Sol should remain responsible for architecture, code review, result interpretation, and plan updates.
- Any compilation, headless run, emulator run, capture, or disassembly-producing build must be delegated to a super-cheap sub-agent as requested.
- The cheap agent must run the canonical commands from `fmtowns.sh` and the relevant headless/console skill, return raw timing/stamp/build evidence, and avoid making design decisions from noisy results.
- Sol must not convert a successful build, live emulator, or black screenshot into a visual claim.
- Existing unrelated worktree changes must remain untouched.

## 13. Expected end state

The intended final renderer still looks exactly like the current battle cut-in. The difference is where the work happens:

- static cards and labels come from one retained canonical composite instead of being rebuilt every frame;
- moving/effect regions restore and redraw only what actually changed;
- flash and flame pixels are emitted in packed spans/dwords rather than thousands of clipped one-byte calls;
- dirty VRAM runs retain four 8bpp pixels per 32-bit CPU register/store and, if physical evidence supports it, write each split VRAM bank linearly;
- Marty receives the full-quality effect with far fewer missed fields;
- 386DX and 486-class machines run the same exact imagery and naturally deliver more intermediate frames.

That order attacks the measured 56.8-75.3 ms game-step cost and 24.7-27.1 ms present cost without changing the artwork or asking a 16 MHz 386SX to move unchanged pixels again.
