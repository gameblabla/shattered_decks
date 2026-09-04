# Real-time filled polygons on MSX2 — findings

Date: 2026-09-04
Scope: can this port render a live 3-D board — roughly 256x144, about 20
flat-shaded tiles — on a stock 3.58 MHz MSX2, and at what frame rate?

Short answer: **yes, but only as flat-colour fills issued by the VDP command
engine, and the honest number is 8–12 fps in the port's current GRAPHIC 7 mode,
or 15–19 fps if the board moves to GRAPHIC 4 (SCREEN 5).** A textured board at
that size is 3 fps and is not a real option. The size of any single polygon is
not a constraint; the *total filled pixel count per frame* is the only thing
that matters, and it is a hard wall.

This document supersedes the fill-cost reasoning in
`MSX2_RT3D_ASSESSMENT.md` §"Flat filled polygons", which under-states the case
by assuming CPU spans. It does not change that document's conclusions about
textured cards or about keeping the authored bitmaps.

---

## 1. The one thing the earlier assessment got wrong

`MSX2_RT3D_ASSESSMENT.md` says:

> The V9938 does not provide a triangle or polygon fill primitive, so the
> renderer must either (1) walk two edges and emit horizontal spans, or
> (2) use the VDP LINE command for outlines and a span/fill pass for the
> interior.

Both halves are true, but the sentence quietly implies the span is emitted by
the Z80. It should not be. `MSX_docs/RT3D/extra_notes.txt` (Grauw and Metalion,
msx.org, Aug 2019) settles the primitive question, and the answer is **HMMV** —
one rectangle-fill command per scanline span. That thread is the most valuable
file in the RT3D drop for this question; the model files are not.

Measured cost per pixel, V9938 command engine, in VDP cycles
(21,477,273 Hz; 1 Z80 T-state = 6 VDP cycles):

| Primitive | Per pixel | Per line | Source |
| --- | --- | --- | --- |
| `LINE` | 88 R + 24 W = 112 | 32 | Grauw, quoted in `extra_notes.txt` |
| `LMMV` | 72 W + 24 = 96 | 64 | Grauw, quoted in `extra_notes.txt` |
| `HMMV` | 48 W (per *byte*) | 56 | Grauw's timing table |
| Z80 `outi`+pad | 32 T = 192 | — | `msx2_stream.c:298` `Msx2_PokeBlock` |

The last row is this codebase's own inner loop (`outi / nop / jr nz` = 32
T-states per byte, the safe GRAPHIC 7 display-on spacing). **HMMV is four times
faster per byte than the fastest span loop the port currently owns**, and it
leaves the Z80 free while it runs. Any live-polygon path that writes pixels with
`Msx2_PokeBlock` is starting a factor of four in the hole.

Metalion's own experiment in the same thread confirms the ordering on real
silicon: 96 filled 16x16 right triangles in SCREEN 5 took **12 frames with
LINE, 7 with LMMV, 4 with HMMV**.

---

## 2. Calibrating the per-span overhead (the number that actually decides this)

Grauw's per-pixel figures are the floor. Metalion's frame counts are the reality,
and the gap between them is per-span command setup: writing the command
registers and waiting for CE. Backing that constant out of his measurement:

96 triangles x 16 spans = 1,536 spans; 13,056 pixels = 6,528 SCREEN 5 bytes.
Frame = 357,954 VDP cycles.

* **LMMV, 7 frames** = 2,505,678 cycles.
  Fill 96 x 13,056 = 1,253,376. Per-line 64 x 1,536 = 98,304.
  Remainder / 1,536 = **751 VDP cycles = 125 Z80 T per span**.
* **HMMV, 4 frames** = 1,431,816 cycles.
  Fill 48 x 6,528 = 313,344. Per-line 56 x 1,536 = 86,016.
  Remainder / 1,536 = **672 VDP cycles = 112 Z80 T per span**.

Two independent commands converge on the same answer, which is a good sign the
model is right:

> **A hand-written span emitter costs ~120 Z80 T-states (~720 VDP cycles) per
> HMMV, including the Bresenham edge step and the CE poll.**

That constant is what makes small triangles hopeless and large tiles cheap.
Metalion's 16-pixel spans were ~85% setup; the fill was almost free.

### The port's current C path is 5x worse than that

`Msx2_Fill()` (`msx2_video.c:129`) goes `VDP_CommandWait()` then MSXgl's
`VDP_CommandHMMV`, which lands in `VDP_CommandSetupR36()`:

* `VDP_CommandWait` reselects status register S#2 **on every poll iteration** —
  4 `out` + 1 `in` + branch, ~80 T per iteration, and it spins.
* `ASM_REG_WRITE_INC(..., 36, 11)` pushes 11 bytes by `otir` = 231 T.
* Plus the SDCC call, five `u16` arguments, and the `g_VDP_Command` struct
  stores.

Realistically **500–700 T-states per span** through this path. Select S#2 once
outside the loop and the poll drops to `in a,(#99) / rra / jr c` ≈ 30 T; write
only the registers that changed (DX, NX, DY-low, CMD) and the `otir` shrinks.

**A live polygon board must not be built on `Msx2_Fill()`.** It needs a
dedicated assembly span emitter that owns the loop. This is the single largest
implementable difference between "5 fps" and "10 fps" below.

---

## 3. Throughput model

Per span of N destination bytes:

```
T_span = 48*N + 56 + 720   VDP cycles      (tight asm emitter)
T_span = 48*N + 56 + 3600  VDP cycles      (current Msx2_Fill path)
```

SCREEN 8 (GRAPHIC 7) is 1 byte per pixel. SCREEN 5 (GRAPHIC 4) is 1 byte per
**two** pixels, so N halves and so does the fill term.

Useful constants, per second rather than per frame so PAL/NTSC drops out:

| Path | Pixels/second |
| --- | --- |
| HMMV, SCREEN 5 | **894,000** |
| HMMV, SCREEN 8 (optimistic) | **447,000** |
| HMMV, SCREEN 8 (pessimistic, see §6) | **224,000** |
| Z80 `Msx2_PokeBlock`, SCREEN 8 | **112,000** |

### 3.1 The asked-for case: 256x144, ~20 tiles, flat colours

20 tiles laid out 5-across x 4 rows over 144 lines means about **5 span
crossings per scanline = 720 spans/frame**, and 36,864 filled pixels.

| Mode / path | Fill | Span overhead | Total | **fps** |
| --- | --- | --- | --- | --- |
| SC8, tight asm emitter | 1,769,472 | 558,720 | 2,328,192 | **9.2** |
| SC8, tight asm, 2-line span doubling | 1,769,472 | 279,360 | 2,048,832 | **10.5** |
| SC8, current `Msx2_Fill()` | 1,769,472 | 2,632,320 | 4,401,792 | **4.9** |
| SC8, `Msx2_PokeBlock` CPU spans | 7,077,888 | — | 7,077,888 | **3.0** |
| **SC5, tight asm emitter** | 884,736 | 558,720 | 1,443,456 | **14.9** |
| **SC5, tight asm + span doubling** | 884,736 | 279,360 | 1,164,096 | **18.5** |
| SC8 pessimistic, tight asm | 3,538,944 | 558,720 | 4,097,664 | **5.2** |

(VDP cycles per rendered frame; fps = 21,477,273 / total.)

**Answer to the framerate question: 9–10 fps in the port's current SCREEN 8, and
15–19 fps if the board moves to SCREEN 5.** Neither number includes game logic;
see §5 for why that is nearly free anyway.

### 3.2 The port's actual board band is 256x114, not 144

`src/generated/msx2_scenes.h` already defines the duel board band as
`MSX2_BAND_Y 14`, `MSX2_BAND_H 114`, with `MSX2_FIELD_SLOTS 20` and — crucially
— `g_msx2_slot_quad[view][slot][8]`, the projected corners of all twenty slots,
already baked. **The geometry for exactly the board being asked about exists
today.** For 256x114 (29,184 px, ~570 spans):

| Mode / path | **fps** |
| --- | --- |
| SC8, tight asm emitter | **11.7** |
| SC8, tight asm + span doubling | **13.2** |
| SC5, tight asm emitter | **18.8** |
| SC8 pessimistic, tight asm | **6.6** |

---

## 4. How large can a polygon be?

Two separate limits, and only the second one bites.

**Register limits (not a problem).** HMMV's NX is 9 bits (1..512), NY is 10 bits
(1..1024). The command coordinate space in GRAPHIC 7 is 256 wide x 512 tall
(both pages; `Msx2_PageY()` in `msx2_video.c:122` already exploits exactly
this), and 512x512 in GRAPHIC 4. A command may not wrap that space. So a single
polygon may legally be the whole screen. There is no small-polygon ceiling to
design around.

**Budget limits (the real answer).** At 256 pixels wide, the tallest full-width
band you can repaint completely at a given rate, using a tight asm emitter over
~5 tiles per scanline:

| Target | SC8 | SC8 + span doubling | SC5 | SC5 + span doubling |
| --- | --- | --- | --- | --- |
| 60 fps | 256x22 | 256x25 | 256x35 | 256x44 |
| 30 fps | 256x44 | 256x50 | 256x71 | 256x88 |
| 20 fps | 256x66 | 256x75 | 256x107 | 256x133 |
| 15 fps | 256x88 | 256x100 | 256x143 | 256x177 |
| 10 fps | 256x132 | 256x151 | 256x214 | 256x266 |

(From `cost = 48*bytes_per_px*256*h + spans*776`, spans = 5h or 2.5h.)

Read that table as the design contract. In SCREEN 8 a 256x144 board is a ~9 fps
object; in SCREEN 5 it is a ~15 fps object; and a 256x64 board — a horizon strip
rather than a full arena floor — is a 30 fps object in either mode.

**Per-tile cost is dominated by height, not area**, because every scanline a
tile spans costs a command. A single 256x144 tile costs 144 spans (111,744
cycles of overhead) whether it is 4 pixels wide or 256. Wide-and-short tiles are
much cheaper than tall-and-thin ones. Twenty tiles are not twenty times the cost
of one; they are the cost of the pixels plus 5 spans per scanline.

---

## 5. What is genuinely free, and what is not

**Geometry is free.** This is the RT3D drop's actual contribution, and it holds
up. `notes.txt`'s indexed-coordinate trick — a vertex stores
`(index_in_x_array, index_in_y_array, index_in_z_array)`, and the nine rotation
products are computed once per *distinct coordinate value* and cached — reduces
a 20-tile grid (roughly 30 shared vertices, maybe 8 distinct values per axis) to
24 multiplies and a few dozen adds per frame. Call it 3,000 T-states. Against a
9 fps frame (~400,000 T available) that is under 1%.

**The VDP runs asynchronously, so hide the geometry behind it.** An average tile
in a 256x144 board is ~1,800 px; one span of it is ~50 bytes = 2,400 VDP cycles
= 400 Z80 T-states during which the CPU has nothing to do but poll. Metalion
says exactly this in `extra_notes.txt` ("I'm using the time needed by the VDP to
draw it to make computation for a next frame"). Transform next frame's vertices
inside the CE poll and the geometry term vanishes entirely. This is the correct
architecture and it is why §3's tables ignore transform cost.

**Back-face culling and early rejection are free and mandatory.** `notes.txt`'s
second rule — "first check if a triangle is visible, only then calculate the
corner points" — costs one cross-product sign per tile.

**What is NOT free:**

* **Clearing.** `Msx2_ClearPage()` (`msx2_video.c:134`) HMMVs 256x256 = 65,536
  bytes = 3,145,728 VDP cycles = **146 ms**. Two of them at
  `Msx2_VideoInit()` is 293 ms. That is fine at boot and fatal in a frame loop.
  A board whose tiles tile the whole band needs **no clear at all** — paint the
  tiles over the previous frame. If a clear is unavoidable, clear only the band.
* **Texture.** Any per-tile gradient, dither or texture drops off the command
  engine onto `Msx2_PokeBlock` at 32 T/byte. 256x144 textured = 7.08 M cycles =
  **3.0 fps**. The gap between flat and textured is 4x in SCREEN 8 and 8x in
  SCREEN 5. This is the sharpest line in the whole analysis: **flat colours are
  a VDP job, anything else is a CPU job, and the CPU cannot afford a board.**
* **Logical operations.** `LMMV` with an XOR/AND op must read before it writes.
  Keep every fill at `VDP_OP_IMP` / HMMV.
* **`Msx2_QuadOutline()`** (`msx2_video.c:213`) issues four LINE commands per
  quad through `Msx2_LineOp`, each with a full `VDP_CommandWait`. At 112 cycles
  per pixel LINE is the most expensive primitive available; twenty outlined
  quads is ~80 commands and several thousand pixels of the slowest fill on the
  chip. Fine for a single selection ring, not for structure.

---

## 6. The two open uncertainties

**(a) Does GRAPHIC 7 halve the command engine?** Grauw's timing table is
measured in GRAPHIC 4/5. In GRAPHIC 6/7 the display itself consumes twice the
VRAM bandwidth (one byte per pixel instead of half a byte), and the port's own
CPU write loop already pays for this — `Msx2_PokeBlock`'s 32 T-state spacing is
the GRAPHIC 7 figure, against roughly 20 T in GRAPHIC 4/5. It is likely, but
**not established here**, that the command engine loses proportionally. If it
does, every SCREEN 8 number above halves, which is the "pessimistic" row.
Every SCREEN 8 result in this document should therefore be read as a band:
**9.2 fps optimistic / 5.2 fps pessimistic** for 256x144.

**(b) Metalion's frame counts are coarse.** 4, 7 and 12 whole frames, from an
unstated PAL/NTSC machine, with a self-admitted "my changing loop may have been
longer than the draw itself". The derived 120 T span constant should be treated
as ±30%. It agrees across two commands, which is reassuring, but it is not a
measurement of *this* code.

Both are exactly what `STATUS.md`'s **M1b — timing truth ROM** line item exists
to settle, and that line still reads `not started`. Until it runs, nothing here
is hardware data.

---

## 7. Recommendation

A live polygon board is **feasible and worth building**, but only under all of
these:

1. **Flat colours only.** No per-tile texture on the board surface.
2. **A dedicated assembly span emitter**, not `Msx2_Fill()`. Select S#2 once,
   poll CE tightly, write only the changed command registers, and step the two
   Bresenham edges inside the poll. This is worth ~2x on its own (§2).
3. **Never clear.** Full-coverage tiles overwrite the previous frame.
4. **Accept ~10 fps in SCREEN 8** for a 256x144 board, or **change the duel
   screen to SCREEN 5 for ~15–19 fps.** SCREEN 5 is the single biggest lever
   available and also frees ~64 KB of VRAM for card caches, which SCREEN 8 does
   not have (two 256x256 pages consume all 128 KB; only rows 212–255 of each
   page are spare today). Its costs are real: 16 colours instead of 256, all
   GRB332 art must be re-authored or re-quantised, and HMMV quantises horizontal
   edges to 2 pixels. Grauw's suggested fix for that last one — draw the odd
   edge pixel with the CPU, or run a LINE down the hypotenuse — costs one extra
   touch per span and is affordable.
5. **Redraw only what moved.** A settled board costs nothing; a rotating camera
   in discrete authored steps costs nothing (bake it, as the port already does
   with `MSX2_BOARD_VIEWS` and the camera-move strip). The 9–19 fps figures are
   the price of a *continuously* moving camera, which is a design choice, not a
   requirement.
6. **Consider 2-line span doubling** (NY=2) for the far half of the board, where
   perspective compresses detail anyway. It buys 1–4 fps and costs almost
   nothing visually.

The pre-condition for any of it is the M1b timing ROM. Build it to measure, in
this order: HMMV bytes/second in GRAPHIC 7 with display on and sprites on
(settles §6a); the same in GRAPHIC 4; the cost of one span through a tight asm
emitter versus through `Msx2_Fill()` (settles §2); and `Msx2_PokeBlock`'s real
spacing. Four numbers. Everything above is then either confirmed or replaced.

---

## Appendix: what the SandStone disk itself shows

`deel2.asm` is a byte-damaged dump — roughly every 32nd byte is missing — but
enough survives to read the architecture, and it corroborates the model:

* It is a **double-buffered, HMMV-based** renderer. `init_scr1` is
  `db 0,0, 0,0, 0,1, 195,0, 0,0,%11000000` — DX=0, DY=0, NX=256, NY=195,
  CLR=0, CMD=0xC0, i.e. HMMV. `wispage` is the same shape at NY=180 and is
  called from `wisnovisib` ("clear the non-visible page") inside the page-swap.
* It clears **180 lines per frame** before drawing. That alone is 3.09 frames of
  HMMV in SCREEN 5, which caps the demo near 15 fps before a single polygon is
  drawn — consistent with a 1990s real-time MSX2 3-D demo, and a direct warning
  against per-frame clears (§5).
* It runs a **line-interrupt display list** (`INITLIST`, `spiegellijn`,
  `newinterrupt` -> `forcopybegin`, the `wait2` loop on S#0 bit 5) to split the
  screen — the 3-D window sits inside a static `.sr8` console frame
  (`control.vrm`, `remote.sr8`, `plate3.sr8`, all 54,279 bytes = a SCREEN 8
  page + header). **The 3-D was a window, not the screen.** That matches §4's
  budget table: a 256x64-ish viewport is where 30 fps lives.
* The models are tiny: `bird.txt` is 26 polygons / 18 points, `ballpoin.txt` is
  27 polygons / 17 points. There is a runtime `MK_lower_detail` level and a
  `MK_do_sort` flag, i.e. painter ordering was already a toggle. A 20-tile board
  is *within* this complexity class, which is the encouraging part.
* Textures exist but are one small 3-vertex map per model with baked UV corners,
  into a 16,391-byte `.map` page — an effect on one facet, not a surfaced scene.

The RT3D drop's real transferable content is therefore: the indexed-coordinate
rotation cache (§5), the "reject before you project" rule (§5), the
HMMV-per-span primitive (§1), and the empirical calibration in `extra_notes.txt`
(§2). Its model format contributes nothing this port needs — the projected slot
quads are already baked in `g_msx2_slot_quad`.
