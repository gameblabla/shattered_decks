# Shattered Decks — Super Nintendo port plan

Target: a real-time, software-texture-mapped SNES port on a **4 MB HiROM /
FastROM** cartridge with **no enhancement chips**.  Laid out as a fork, the same
way the MSX2 and Atari ST ports are: everything target specific lives in
`src/snes/` and `tools/snes/`, driven by `Makefile.snes`.  `src/main.c` is never
compiled for this target.

---

## 1. Cartridge and toolchain

### 1.1 Mapping

| choice | value | why |
|---|---|---|
| mapper | **HiROM**, `.BASE $C0` | banks `$C0-$FF` are the only 4 MB region that is *entirely* FastROM |
| speed | **FastROM** (3.58 MHz, 6 master cycles/access) | required |
| size | **4 MB / 32 Mbit** — `ROMBANKS 64`, `ROMSIZE $0C` | largest chipless ROM that stays fast everywhere |
| SRAM | 8 KB, `SRAMSIZE $03`, `CARTRIDGETYPE $02` | story progress + deck slots |
| region | NTSC, 224 active lines | |

**Why not ExHiROM (6 MB).**  Tales of Phantasia proves a 48 Mbit chipless cart
is possible, but ExHiROM's *upper* 4 MB is mapped into banks `$40-$7D`, and
those banks are **SlowROM on every SNES** — no `$420D` bit makes them fast.  A
6 MB ExHiROM would therefore be 4 MB fast + 2 MB slow, which contradicts the
FastROM requirement and would put the texture/rasteriser data at 2.68 MHz.
4 MB HiROM is the largest *uniformly fast* chipless configuration, so that is
the target.  The build must stay inside it: no bank-switching tricks, and the
linker script fails the build if a section spills past bank `$FF`.

### 1.2 Toolchain

`SNES/pvsneslib/` is checked in but **not built** — `devkitsnes/bin`,
`devkitsnes/tools` and `pvsneslib/lib` are all empty.  First step is:

```sh
cd SNES/pvsneslib && make            # 816-tcc, wla-dx, gfx4snes, smconv, snesbrr, 816-opt
export PVSNESLIB_HOME=$PWD
```

The library builds four variants; we consume `pvsneslib/lib/HiROM_FastROM`.

**816-tcc language limits that shape the whole port** (`pvsneslib/include/snes/snestypes.h`):

* `int`, `short`, `long` are all **16 bit**; 32-bit is `long long` (`s32`).
* 32-bit multiply/divide exist but are the software routines `tcc__mull`,
  `tcc__divl`, `tcc__divdi3` in `pvsneslib/source/libtcc.asm` — hundreds of
  cycles a call.
* No `float`, no `int64`, no `stdint.h`.

So: **all geometry is Q8.8 in 16-bit words**, and every inner loop is hand
written 65816.  This is also why the rules core we reuse is the MSX2 one — it
was already written against a 16-bit-`int` compiler (SDCC Z80).

### 1.3 Layout

```
Makefile.snes                 HIROM=1 FASTROM=1 ROMBANKS=64
src/snes/                     the fork (C + .asm)
  snes_main.c                 scene machine
  snes_duel.c                 duel presentation (asks the rules, never drives them)
  snes_board3d.c              camera, projection, slot geometry (Q8.8)
  snes_raster.asm             the texture mapper (floor DDA + quad spans)
  snes_m7fb.c/.asm            Mode-7 chunky framebuffer + DMA presenter
  snes_scene.c                Mode 3 full-screen scenes (title/story/ending)
  snes_obj.c                  the sprite layer: HUD, hand, top-view cards
  snes_story.c                VN flow + typewriter
  snes_deck.c                 deck editor
  snes_audio.c                snesmod driver glue
  snes_save.c                 SRAM
tools/snes/
  gen_snes_scenes.py          Mode 3 8bpp scenes from assets/source
  gen_snes_cards.py           card face textures (Mode 7 direct colour)
  gen_snes_obj.py             card sprites + OBJ palettes + HUD font + top view
  gen_snes_textures.py        arena floor texture + palette
  gen_snes_music.py           VGM -> IT -> smconv soundbank
  gen_snes_sfx.py             sounds/*.wav -> BRR
  gen_snes_tables.py          reciprocal / trig / span LUTs
  verify.py                   snesfaust harness
```

---

## 2. What is shared, what is forked

Compiled into the SNES build unchanged:

* `src/msx2/msx2_duel.c` / `.h` — the render-free duel rules model (1339 lines):
  board state, life points, decks, battle resolution, support cards, turn order.
  16-bit `int`, `u8` card ids, no allocation — it is already the right shape for
  816-tcc.  Only `msxgl.h`'s typedefs need replacing, which
  `src/snes/snes_types.h` does (mirroring `src/atarist/atarist_types.h`).
* `src/game/ai.c`, `src/game/deck.c` — opponent AI, deck construction.
* `src/generated/msx2_card_tables.h` — 72 monsters' ATK/DEF/attribute/tribe.
* The story text records from `src/msx2/msx2_story.c` (data only, re-emitted by
  `gen_snes_scenes.py` as bank-placed tables).

Everything else — video, 3D, input, audio, story presentation, deck UI, saves —
is new code in `src/snes/`.

---

## 3. Video architecture

Two display models, switched per scene.  A scene change is always done under
force-blank (`$2100 = $8F`), which is the only safe window for a bulk VRAM/CGRAM
rewrite; nothing writes PPU registers during active display except the scroll
and matrix registers the presenter touches inside vblank.

### 3.1 Duel — Mode 7 used as a 128x128 chunky bitmap

This is the requested model, and it fits VRAM exactly:

```
Mode 7 tilemap   = low  byte of VRAM words $0000..$3FFF   16384 entries (128x128)
Mode 7 char data = high byte of VRAM words $0000..$3FFF   256 tiles x 64 bytes
```

Tile *n* is 64 bytes of the constant *n*, so **tilemap entry = pixel colour**
and the 128x128 tilemap is a 256-colour chunky framebuffer whose texels are
8x8 blocks in Mode 7 source space.  Both halves land in the same 16384 words,
so the *whole bitmap costs exactly half of VRAM* and words `$4000..$7FFF`
(32 KB) stay free — OBJ character data goes at `$4000` (two 8 KB name tables,
512 tiles), leaving 16 KB spare.

**Presenting it.**  `VMAIN = $00` (increment after a write to `$2118`), so a DMA
in mode 0 to `$2118` streams one framebuffer byte per VRAM word — a straight
`memcpy` from WRAM to the tilemap's low bytes.

**Scale, and fixed-detail motion.**  With `M7B = M7C = 0`, `M7X/M7Y = 0`,
`M7HOFS/M7VOFS = 0`, the matrix is a pure scale and each screen pixel samples
source `x = A * screen_x`:

| mode | `M7A = M7D` | buffer | screen px per texel | bytes to DMA |
|---|---|---|---|---|
| **every 3D board state** | `$0400` (4.0) | 128 x 80 | 2 x 2 | 10240 |
| **HUD refresh** | `$0400` (4.0) | 128 x 32 | 2 x 2 | 4096 |

The board stays at the exact power-of-two 2x2 scale during motion, so movement
does not introduce a coarse 4x4 picture or resampling shimmer.  The software
renderer admits at most one complete motion update every twelve NTSC fields;
a card-heavy frame takes longer to render and upload, but keeps the same
128x80 source and fixed-detail cadence while the PPU performs the pixel
doubling for free.

The switch is per *band*, not per frame: two HDMA channels rewrite `M7A` and
`M7D` at line 160 so the board and HUD both stay at 2x2.  **`M7VOFS` is not one
of them.**  A vertical offset written by HDMA does not take effect band by band
— writing the HUD band's offset shifted the whole frame — so the HUD strip
reads framebuffer rows 80..111, where `D = 4.0` maps lines 160..223 with no
offset at all.

**Upload budget.**  NTSC vblank is 38 lines ≈ 51,800 master cycles ≈ 6.4 KB of
DMA.  So:

* every board update (10240 B) — **five bounded vblanks**, uploaded top-down;
* the HUD (4096 B) — one separate vblank when its sprites change.

Mode 7 has no second tilemap base — there is no page flip — so this staged
upload *is* the double-buffering story.  Motion waits between complete uploads
instead of changing the board's texel size.

**Colour.**  Two options, chosen per scene:

* **CGRAM palette** — the 256 CGRAM entries are the board palette, but OBJ
  palettes live in CGRAM `$80..$FF`, so the upper half must be laid out as the
  eight 16-colour sprite palettes.
* **Direct colour** (`$2130` bit 0) — the 8-bit texel *is* the colour, BBGGGRRR,
  and CGRAM is left entirely to the sprites.

The duel uses **direct colour**: it frees all 256 CGRAM entries for the hand
cards and HUD sprites, and the project already owns a GRB332 quantiser
(`tools/msx2/msx2_grb332.py`) that produces exactly this colour space, so the
arena and card textures can be converted with a proven pipeline.  Blue is only
2 bits in direct colour; the quantiser is retuned for that (`gen_snes_textures.py`).

**Screen layout in the duel**

```
lines   0..159   3D board, software texture mapped   (texels 0..79 of the buffer)
lines 160..223   flat black in the bitmap, HUD SPRITES over it
```

The 3D viewport is always 128x80 texels.  The board band stays at 2x2 screen
pixels per texel during motion; only the cadence changes.

**Nothing in the HUD is in the bitmap.**  Life points, the phase prompt, the
hand and both cursors are sprites, and the reason is resolution: the board band
shows one texel on 2x2 screen pixels throughout, so a
letter drawn into it is a letter at half the console's resolution and a hand
card is sixteen texels stretched over thirty-two pixels.  The band under the
board is therefore filled black once, uploaded once and never touched again.
See section 3.3.

**Everything the slab does not cover is black.**  There is no backdrop picture
and no horizon band: the board is the only textured object on the screen, which
is what the FM TOWNS, PC-FX and Atari ST duel views show, and what two bits of
blue can actually hold.  What the slab does have is a front WALL under its near
rim — a flat band of shadowed stone, four to seven rows tall — because that is
most of what says the board is an object standing on the ground.

**Where the camera stands is fixed by the focal length.**  The focal length has
to be half the viewport width (that is what makes the floor's texture step a
shift instead of a divide), so the slab's 2.5-unit half width fills the half
screen exactly when the near edge is 2.5 units away and overflows it at
anything closer.  The camera stands 2.75 units in front of the near edge and
2.75 up: the whole slab fits across with pixels to spare and the near/far depth
ratio is 2.4, which is four board rows the player can count.

**The floor texture is offset half a cell.**  Slot centres are at whole world
x, the checkerboard breaks at whole world x, so without the offset the grid
line runs down the middle of every slot: four whole tiles with a half at each
end, and the middle card sitting on a seam.  u is Q8.8 texels, so half a cell
is `16 << 8` — sixteen alone moves the grid by a sixteenth of a texel and
changes nothing visible.  `check_board_is_five_by_four` in the harness exists
because that second mistake looked exactly like the fix.

### 3.1.1 The top view — Mode 3, and no black frame

UP walks the camera up into the tactical top view the other ports have and DOWN
walks it back down.  It is Mode 3: the 5x4 table as an 8bpp BG1 at the full
256x224, the twenty field slots as 32x32 sprites, no hand (the hand is not on
the board), and the same HUD sprites still up.  There is no software rendering
in it at all, so it runs at sixty fields a second.

**The switch is three register writes and no force blank.**  Both pictures are
resident and one CGRAM serves both:

```
VRAM words $0000-$3FFF   the Mode 7 bitmap (tilemap in low bytes, chars in high)
           $4000-$5FFF   OBJ: 20 card sprites of 32x32, then font + cursor tiles
           $6000-$6FFF   the top view's 8bpp background characters
           $7000-$73FF   the top view's tilemap
CGRAM      0..127        the top view's background palette
           128..255      eight OBJ palettes
```

Mode 7 direct colour reads no CGRAM, and Mode 7 ignores `BG1SC`/`BG12NBA`, so
those are armed once at boot and the change is `$2105` (the mode), `$2130`
(direct colour off — it applies to any 256-colour BG and Mode 3's BG1 is one)
and `$420C` (the Mode 7 matrix HDMA off).  Nothing is rewritten, so nothing has
to be blanked, so no field between the two views is black.  The perspective
board stays in the bitmap untouched while the top view is up and is back on
screen the instant the player walks down.

### 3.2 Title, story, ending — Mode 3

Mode 3 gives BG1 at 8bpp (256 colours from CGRAM) plus BG2 at 4bpp, and sprites.
No software rendering at all; these are real pictures out of `assets/source/`.

| scene | BG1 (8bpp) | BG2 (4bpp) | OBJ |
|---|---|---|---|
| title | 256x224 painting, 896 unique tiles, 57 KB VRAM + 2 KB map | — | menu cursor |
| story / VN | 256x144 portrait window, 576 tiles, 36 KB | text window + font | speaker tag |
| battle close-up | the big card art | frame / numbers | sparks |
| ending | 256x224 painting | narration text | — |

Full-screen 8bpp leaves little VRAM spare, which is fine because these scenes
have no 3D.  Story scenes deliberately use a 144-line picture so BG2's font and
tilemap fit alongside.  Scene images are LZSS packed in ROM (`lzsss.asm`
decompressor ships with pvsneslib) and blown into VRAM under force blank.

### 3.3 Sprites

128 OBJ, sizes 16x16 / 32x32 (`OBSEL`), hard limits 32 sprites and 34 8x8
slivers per scanline.  Budget for the duel:

| what | sprites | VRAM |
|---|---|---|
| card faces, 32x32 — the hand in the board view, the field in the top view | 20 | 10 KB |
| HUD font, ASCII 32..95, 8x8 with its shadow baked in | ~40 | 2 KB |
| cursor corner brackets | 4 | 128 B |

**The hand IS sprites, and that changed after M4.**  M4 drew it into the HUD
band on the argument that a sprite hand costs a second conversion of every face
into 4bpp tiles and ten kilobytes of OBJ VRAM.  It does, and the argument was
still wrong: the band is chunky texels shown at 2x2, so it was showing 16x16
card art where the console can show 32x32, and the same ten kilobytes buys the
top view's twenty field cards as well — one card-sprite system, two views.

A card sprite's tiles are FOUR ROWS OF FOUR, because a 32x32 sprite at name `n`
covers `n..n+3` on each of four rows of the sixteen-wide name table.  Four
cards therefore share a 64-name group and a face is uploaded as four 128-byte
chunks 512 bytes apart, never as one 512-byte block.

Fifteen colours is what an OBJ palette is, and seven palettes have to carry
seventy-nine faces, so the faces are clustered by colour and each cluster gets
one.  Within a palette **the frame is quantised and the painting is dithered**
— four fixed entries for the card template, eleven fitted to that cluster's art
— which is the same split `gen_atarist_assets.py` makes: fit all fifteen to the
whole card and the frame, one or two pixels wide at this size, loses every
entry to the painting and the card stops having an edge.

**The vblank window is shared and the sprite layer is paid first.**  NTSC
vblank carries something under 6 KB of DMA in total.  OAM is 544 bytes whenever
the list changed and a card face is 512, so the bitmap's still upload is 16
rows — 2048 bytes — leaving room for OAM plus one card row during a camera
handoff.  Two further rules,
both learned by breaking them:

* Overspend and the rows past the end of the window are simply not written: a
  black board under a framebuffer that is perfectly correct in WRAM, which
  reads as a renderer bug and is not one.
* **CPU time in vblank is part of the budget.**  Scanning twenty slots for the
  next card to upload is a 816-tcc loop that costs more of the window than the
  512-byte transfer it is looking for.  The scan happens with the rest of the
  frame's work; the vblank routine does one comparison and four card-row DMAs.

---

## 4. The texture mapper

All of it is 65816 in `src/snes/snes_raster.asm`; C only sets up per-frame
geometry.  Everything is **Q8.8** (`s16`, 8 integer + 8 fractional bits), which
matches `SNES/q88_mul.txt`'s macros and keeps every value in one register.

### 4.1 Arithmetic

* **Q8.8 x Q8.8** uses the PPU's 8x8 hardware multiplier (`$4202/$4203` ->
  `$4216`, 8 cycles latency) four times, exactly as the `mult_8p8_8p8` macro in
  `SNES/q88_mul.txt` does — ~45 cycles, and only ever per *vertex* or per
  *span*, never per pixel.
* **Reciprocal** — 1/z for the perspective divide is a **ROM lookup table**, not
  a divide: `recip[z]` for z in Q8.8 over the camera's depth range, 8192 entries
  x 2 bytes = 16 KB in FastROM.  A ROM read is 6 master cycles; `tcc__divl` is
  several hundred.  Generated by `tools/snes/gen_snes_tables.py`.
* **Sin/cos** — 256-entry Q8.8 table, same file.
* **No 32-bit arithmetic anywhere in the renderer.**  The Atari ST port's
  `fxdiv` bug (a `<< 16` that overflowed once the focal length exceeded 1.0) is
  the cautionary tale; here the range is enforced by construction because Q8.8
  cannot hold the intermediate at all, so the reciprocal table is the only
  division path and its range is asserted at generation time.

### 4.2 Geometry

Ported from `src/atarist/atarist_board3d.c`, which is already the 32-bit-free,
libgcc-free version of the board model: one world unit is one slot pitch,
columns -2..2, rows -1.5..1.5, camera with no roll.  Q8.8 shrinks the working
range but the board is only ~5 units across, so 8 integer bits is ample.

### 4.3 Two rasterisers

**(A) The constant-v row mapper — floor and every card resting on the board.**

The camera never rolls, and every resting quad is axis-aligned in world space.
Therefore *a screen row of such a quad maps to a texture row of constant v*.
Per screen row we do one reciprocal lookup and two Q8.8 multiplies to get the
row's starting `u` and its `du`, point a 16-bit pointer at the texture row, and
the inner loop is a 1-D DDA:

```
    ; A=16-bit, X=16-bit (framebuffer index), Y=8-bit (texel index)
loop:
    lda  <uacc          ; 8.8 texture u
    adc  <du
    sta  <uacc
    ldy  <uacc+1        ; integer part == byte index into the 256-byte row
    lda  [texrow],y     ; ROM read, 6 master cycles  (ROM is FASTER than WRAM)
    sta  fb,x           ; WRAM store, 8 master cycles
    inx
```

~36 CPU cycles a texel, measured (section 4.4).  Textures live **uncompressed and 256-byte aligned in
ROM** precisely so this loop can index a row with an 8-bit `Y` and read at
FastROM speed; WRAM would be 8 cycles per access instead of 6.  The loop is
unrolled 8x with the increments in direct page.

**(B) The general affine quad mapper — cards in the air.**

A card that lifts, tilts and flies during a play or a battle is not axis
aligned, so both u and v vary along a span.  These use two edge chains (the
MSX2 port's lesson: a card is a *convex quad*, not a trapezoid — assuming a
trapezoid is what put a card off the board rim), an affine gradient solved once
from three vertices, and a span walker that assembles the texture index from
two Q8.8 accumulators on every texel.  It costs about twice a floor texel and
runs only while a card is in the air.

**A face is 16x16 texels, and that is the rasteriser's constraint.**  The
walker keeps its index as `page << 8 | (v << 4) | u`, so the eight-bit add that
steps u stays inside the card's own 256-byte page — the same trick the floor's
256-byte row plays.  One face is one page, the sheet is an array of pages, and
a card id indexes it directly (`tools/snes/gen_snes_cards.py`, 79 faces =
20,224 bytes in bank `$C8`).

Two things about the solve are arithmetic, not taste:

* **The edge vectors are taken in sixteenths of a pixel.**  A card is forty
  pixels across, 10240 in Q8.8, and the cross product of two of those is four
  hundred thousand — the Atari ST `fxdiv` overflow wearing different clothes,
  and it comes back as a card painted in one flat colour because every gradient
  divides by a wrapped determinant.  Numerators and determinant are scaled
  alike, so the ratios are untouched.
* **A card covers four fifths of its tile.**  Sixteen texels over 0.8 of a
  one-unit tile is twenty texels a world unit against the floor's thirty-two,
  and 20/32 is 5/8: a *resting* card's texture step is the floor's own step
  shifted twice and added, and its v is the row's depth shifted — no multiply
  and no divide in the row.  A card covering the whole tile would be free in
  the same way, but then every slot touches its neighbour and the board reads
  as a carpet of cards.

**Resting cards go through (A), five at a time.**  Five slots of a board row
sit at the same depth, so they share the row's depth, texture step and v; the
row carries the left and right edges of the CENTRE card plus the slot pitch,
all three linear in the row index, and a column then costs one add rather than
a pair of edges of its own.  It walks outwards from the middle because an edge
accumulator latches at the side of the viewport, and a chain seeded on a
latched value is a chain built on a clamp — which put the whole near row in the
wrong place until it was seeded on the centre column, the one card the camera's
sway can never push off screen.

Painter's order, no z-buffer: floor, far support row, far monster row, near
monster row, near support row, then any animating card.

### 4.4 Frame budget — **measured at M2, re-measured with cards at M3**

The estimates this section used to carry have been replaced by measurements off
the frame stamp (`render_lines` in `src/snes/snes_duel.c`, taken from the V
counter plus the vblank count, read out of `wram.bin` by `tools/snes/verify.py`
and printed by its `render cost` check).  One NTSC field is 262 scanlines.

| board | viewport | render | fields | fps |
|---|---|---|---|---|
| moving, floor only | 128 x 80 | 1528 lines | 5.8 | 10.4 |
| moving, 21 cards | 128 x 80 | 4128 lines | 15.8 | 3.8 |
| still, floor only | 128 x 80 | 1528 lines | 5.8 | 10.4 |
| still, 21 cards | 128 x 80 | 4128 lines | 15.8 | 3.8 |

The floor-only rows are an **ablation, not an estimate**: SELECT compiles
nothing out but draws no cards (`show_cards` in `src/snes/snes_duel.c`), and
`verify.py` drives it.  That is also how the slab's own shape is measured, since
twenty cards cover almost all of it.

Attributed by ablation at M2, for the floor alone — the same run with the three
span calls compiled out, which leaves only the per-row C setup:

| | still | moving |
|---|---|---|
| per-row setup (C) | 626 lines, 25% | 334 lines, 39% |
| span walking (asm) | 1862 lines, 75% | 517 lines, 61% |

That works out at **~250 master cycles a texel** in the span walker, against
the 170 this section originally guessed: the estimate counted the load and the
store and forgot the `clc`/`adc`/`txa`/`tax`/`iny`/`cpy`/`bne` around them, and
the walk is about 36 CPU cycles a texel, not 26.

The second number is the surprise, and it sets the order of the M8 levers.
**816-tcc's code is roughly a hundred CPU cycles per framebuffer byte** — a
measured 12.3 million master cycles for a 16384-byte `for` loop of `fb[i] = c`,
which is why `snesVideoClear` now calls the span filler instead (593 lines, a
15x difference for the same 16 KB).

**M3 sharpens that finding rather than adding a new one.**  The cards are 59%
of a still frame and 71% of a moving one, for about 7000 and 1750 texels — 647
and 1573 master cycles a texel, against the floor walker's 250.  The texels are
not what costs: a moving frame makes about 200 per-column span calls, so at
816-tcc's call cost a card's *span call* is several times the price of the eight
texels it draws.  So the levers, in order:

1. move the per-row geometry AND the per-column walk into `snes_raster.asm`, so
   a board row is one call rather than fifty — the biggest single lever in the
   port, worth most of that 59%/71%;
2. unroll the span walker and hoist its loop control (~17% of the walk);
3. then, and only then, the geometric levers this section already listed:
   a shorter 3D viewport, skipping the floor texels a card will cover, a 32x32
   floor texture, dropping the still mode to 128x64.

None of that is M3 work: what M3 owes is the number, and the number is above.

## 5. Memory map

**VRAM (64 KB)** — `$0000-$3FFF` words = Mode 7 bitmap (both halves),
`$4000-$5FFF` = OBJ tiles, `$6000-$6FFF` = the top view's 8bpp BG characters,
`$7000-$73FF` = its tilemap.  All four are resident together, which is what
makes the duel's two views a three-register change; see section 3.1.1.  The
full-screen Mode 3 scenes of M5 have no 3D under them and take the bitmap's
half of VRAM back.

**WRAM (128 KB)**

| | |
|---|---|
| `$7F0000` | chunky framebuffer, 128x128 = 16 KB (only 128x112 displayed) |
| `$7F4000` | scene decompression scratch, 32 KB |
| `$7E2000..` | duel rules state, story state, deck state (a few hundred bytes) |
| `$7E0100..` | OAM shadow (544 B), CGRAM shadow (512 B), span/edge lists |
| `$0000-$00FF` | direct page: rasteriser accumulators and increments |

**ROM (4 MB, banks `$C0-$FF`)**

| banks | content |
|---|---|
| `$C0-$C1` | code (816-tcc output + asm), rules, AI |
| `$C2` | reciprocal / trig / span LUTs (16 KB + 1 KB) |
| `$C6` | arena floor texture, 256x64 direct colour (16 KB) |
| `$C8` | card faces: 72 monsters + 6 supports + the back, 16x16 direct colour, one 256-byte PAGE each (20,224 B) |
| `$C7-$CE` | Mode 3 scene images (title, 8 story backdrops, ending, battle art), LZSS packed |
| `$CA` | OBJ font + palettes + face/palette map, top-view BG tiles and tilemap (9.3 KB) |
| `$CB` | card faces as 32x32 4bpp sprites, 512 B each (39.5 KB) |
| `$D3-$D6` | soundbank (module + BRR samples) |
| `$D7-$FF` | headroom: uncompressed high-quality scene art, extra card art |

The bank table is generated, not hand-maintained: `gen_snes_*.py` emit `.SECTION
... BANK n SLOT 0 SEMIFREE` blocks plus a `snes_banks.h` of `{bank, offset}`
descriptors, so a section that outgrows a bank is a build error, not a silent
wrap.

---

## 6. Assets — real art only

**No placeholder graphics anywhere.**  Every pixel in the ROM is converted from
`assets/source/` by `tools/snes/*.py`, the way the MSX2 and Atari ST ports do
it; nothing is drawn procedurally and no "coloured rectangle stands in for the
card" ever reaches a commit.  Sources already in the tree:

| source | used for | pipeline |
|---|---|---|
| `assets/source/cards/*.png|webp` (87 files) | 79 board faces + hand-card sprites + battle close-ups | `gen_snes_cards.py`: 16x16 direct-colour page per face, through the ST generator's own crop/frame rules; sprites and close-ups still to come |
| `assets/source/textures/*.png` | arena floor, card frame | `gen_snes_textures.py` |
| `assets/source/title/*.png` | Mode 3 title painting | `gen_snes_scenes.py` |
| `assets/source/story_portraits/*` (23) | VN portraits | `gen_snes_scenes.py` |
| `assets/source/ending/ending256x240.png` | ending picture | `gen_snes_scenes.py` |
| `assets/fonts/` | Mode 3 4bpp font and the 5x7 chunky font | `gen_snes_scenes.py` |
| `msx_music/*.vgm`, `Music/*.wav` | soundtrack | `gen_snes_music.py` |
| `sounds/*.wav` | SFX | `gen_snes_sfx.py` |

Two quantisers, because the two display models have different colour spaces:
direct-colour BBGGGRRR for everything the Mode 7 bitmap shows (retuned
`msx2_grb332.py`, 2-bit blue), and a per-scene 256-entry CGRAM palette with
`gfx4snes` for the Mode 3 pictures.  Card art is dithered against the direct
colour cube once and cached, since 78 cards x two representations is the bulk of
the conversion time.

---

## 7. Audio

SPC700 + 64 KB ARAM via pvsneslib's snesmod driver (`smconv`, `snesbrr`).

* **Music** — `gen_snes_music.py` converts the AY-3-8910 VGM streams in
  `msx_music/` into IT modules: the same parsing `tools/atarist/gen_atarist_audio.py`
  already does (register deltas, period rescaling), emitting notes/volumes into
  three tone channels plus noise, against a small BRR instrument set sampled
  from `Music/*.wav`.  This reuses a proven front end and keeps ARAM small; a
  later pass can replace individual instruments with real BRR samples without
  changing the note data.
* **SFX** — `sounds/*.wav` at 8 kHz through `snesbrr`, played on the channels
  the module leaves free.
* ARAM budget: driver ~4 KB, samples <= 40 KB, module <= 16 KB.

Known verification gap: the headless emulator opens no audio device, so music is
checked by asserting the SPC upload completes and the driver's ARAM state
advances, not by listening.  Audible verification needs a normal emulator run by
the owner.

---

## 8. Input, saves, flow

* **Input** — pvsneslib `padsCurrent()`.  In the duel: left/right move the
  cursor, A confirms, B goes back, X is the second action of whatever state the
  screen is in (defence position, direct attack, the battle phase itself), START
  ends the turn.
* **The harness's four switches**, all of them in `src/snes/snes_duel.c` and all
  of them real behaviour rather than debug hooks: **R** fills the board from the
  two decks (the fixture every measurement and every card identification runs
  against — a fixed BOARD, not fixed art), **SELECT** draws that board with no
  cards on it (the ablation behind every card-cost number in section 4.4), **L**
  hands the player's side to the rules as well (the demo, and the soak duel the
  verifier plays to a result), **Y** pins the board to the moving cadence so it
  can be measured.  A game frame is many fields, so a scripted press has to
  be HELD for longer than the slowest frame or the poll never sees it down.
* **Saves** — 8 KB SRAM at `$30:6000` (the HiROM SRAM window), a checksummed record holding
  story progress (`g_story_progress` frontier vs. selected foe, the distinction
  the PC-FX port established), four deck slots, and settings.  Each slot keeps
  both the 40-card DECK list and the 64-card STORAGE list, matching the PC-FX /
  FM TOWNS editor rather than flattening the collection into a library.  The
  current editor is a Mode 3 gallery with the reference six-column grid:
  **X** switches DECK/STORAGE, arrows move the cursor (including three-row
  scrolling), **A** moves a card between lists, **B** opens CARD CHECK, and
  **START** proceeds only at 40 cards.  **Y** saves, SELECT changes slot, R
  restores the deterministic first-run collection, and L returns to the title.
  The active saved deck is passed to the rules model when a duel starts.
* **Scenes** — title -> menu (story / free duel / deck / options) -> story map ->
  VN dialogue -> duel -> battle close-ups -> victory/defeat -> ending.  Same
  state machine as the MSX2 fork, with a Mode 3/Mode 7 switch at each boundary.

---

## 9. Verification

`SNES/snesfaust/snesfaust-mednafen` is already built and is a fully headless
Mednafen SNES-Faust.  Its `script` mode is the harness:

```sh
snesfaust-mednafen script rom.sfc inputs.txt FRAMES last.ppm wram.bin \
    cov.cov 0 ppu_prefix ppu_trace.txt frame_dir CAP_START CAP_END STEP
```

* **Input injection works** — `inputs.txt` rows are `start_frame end_frame
  p1_mask p2_mask`, so the menu, a whole duel and the deck editor can be driven
  from a script.  (This is the SNES equivalent of the FM TOWNS event log; there
  is no "cannot inject input" caveat here.)
* **Frames** land as PPM in `frame_dir`; `tools/snes/verify.py` asserts on them
  — the board is not a flat colour, the slab is in perspective, the camera moved
  between frames.  A black screenshot is not proof of anything.
* **Cards are identified, not merely counted.**  `verify.py` projects a card's
  own texels through the same camera model the renderer uses, reads them out of
  the screenshot, and matches them against `snes_card_tex.bin` — so "the right
  card is in that slot" is an assertion rather than an impression.  Matches are
  weighted by how RARE the colour is across the sheet: half of every face is the
  frame's near-black navy, and unweighted, a flat support sigil matches a
  monster's dark corners better than the monster does.
* **SELECT is an ablation switch, not a debug key.**  It draws the board with no
  cards on it, which is what attributes the render cost between floor and cards
  and what lets the slab's shape be measured at all — a card's dark frame
  quantises into the same byte as the surround.
* **WRAM dump** is the frame-stamp channel: the game writes a scene id, a frame
  counter and the last render duration (measured off an IRQ-driven counter) into
  a known WRAM address, and `verify.py` reads them out of `wram.bin`.  That is
  how the section 4 estimates get replaced with measurements.
* **`snap`** prints PPU/CPU state and dumps CGRAM and the VRAM tile sheet — the
  way to debug a wrong Mode 7 matrix or a mis-uploaded palette.
* **`cov`** gives instruction coverage, useful to prove a rasteriser path is
  actually taken and to find dead banks.
* `record-snesfaust-video.sh` makes an MP4 from a scripted run for reviewing
  animation.

`verify.py` empties a capture's frame directory before every run.  It did not,
once, and a fixed flicker went on being "reproduced" out of the previous run's
leftover frames for an hour.

Regression targets: boot to title, menu navigation, a scripted full duel to a
win, a story chapter with dialogue, deck editor save/load round trip through
SRAM, and a perf capture of the duel board at rest and during motion.

---

## 10. Milestones

| # | deliverable | done when |
|---|---|---|
| **M0** | toolchain built, `Makefile.snes`, HiROM/FastROM 4 MB ROM boots | `snap` shows the title colour on frame 60; `header` reports HiROM/FastROM/32 Mbit |
| **M1** | Mode 7 chunky framebuffer harness: 256 solid tiles, DMA presenter, fixed-detail motion cadence | a scripted run shows the 128x80 board at rest and during motion with pixel-exact 2x2 texels |
| **M2** | Q8.8 math, LUTs, **floor mapper** with the real arena texture | textured ground under a moving camera; **measured** ms/frame in `wram.bin` |
| **M3** | quad rasteriser + real card textures on the board | **done** — five slots per side show the right cards, identified out of a screenshot against the card sheet; the set monster shows the back; a card in the air goes through the two-chain convex-quad path |
| **M4** | rules integration, hand, cursor, HUD text — playable duel | **done** — a duel is played through the UI (card chosen, carried, set; battle phase; attacks; turn passed) and a demo duel plays itself to a decided result with the band reading the outcome; the HUD's text is decoded back off the screenshot and checked against the rules |
| **M4.5** | the sprite HUD, the Mode 3 top view, and the board's framing | **done** — the HUD's letters measure one screen pixel a stroke across every 3D board state (so they cannot be bitmap); the bare slab shows four grooves and no seam on its centre line (five columns); the top view identifies all twenty field slots pixel for pixel against the sprite sheet; and no field across the mode change is blank |
| **M5** | Mode 3 title / story / ending with real art and typewriter text | **done** — title, story portrait window, and ending painting are generated as real 8bpp scene assets; BG2 typewriter text is VBlank-updated; `verify.py` captures all three paths, including A-button title start |
| **M6** | audio: module playback + SFX | SPC upload asserted, ARAM state advances |
| **M7** | deck editor + SRAM saves | **done** — reference DECK/STORAGE six-column editor, CARD CHECK preview, 40-card gate, and four-slot checksummed SRAM round trip survive a fresh emulator process; the active deck feeds the duel rules |
| **M8** | perf and polish pass | ablation-measured improvements only, no speculative rewrites |

---

## 11. Known traps, carried over from the other ports

* **ROM is faster than RAM.**  FastROM is 6 master cycles, *all* WRAM is 8.
  Textures and tables stay in ROM and are read in place; never copy them to RAM
  "for speed".
* **No PPU register writes during active display.**  This is the SNES form of
  the PC-FX active-display rule that caused real, console-only glitch lines.
  Palette, VRAM and mode changes happen in vblank or force blank, full stop.
* **No 32-bit arithmetic in the renderer** — `tcc__divl`/`tcc__mull` are a
  cliff, and the Atari ST `fxdiv` overflow bug is what a "broken texture mapper"
  actually looks like when it is really a broken divide.
* **A card is a convex quad, not a trapezoid** (MSX2).
* **Measure before optimising, and ablate to attribute.**  Every perf claim in
  this port must come from a `wram.bin` timing stamp or an ablation, never from
  reading the code.
* **Rebuild before capturing.**  The MSX2 port repeatedly photographed a stale
  ROM; `verify.py` refuses to run if the ROM is older than any source it
  depends on.  For the same reason the demo/soak mode is a RUNTIME switch and
  not a second ROM: a soak build that looks like the real one is how that
  mistake happens.
* **An uninitialised static is not zero.**  A probe in the frame stamp read 255
  out of a fresh boot: 816-tcc's `.bss` lands in a RAM section pvsneslib's crt0
  clear does not actually cover, while anything with an initialiser is copied
  from the ROM image and is exact.  Every static in this port is initialised
  explicitly.  What found it was a duel that played itself with no input, which
  reads as a rules bug and is not one.
* **The Makefile rebuilds every object when any header changes.**  There is no
  dependency generation; the first time a header changed without this, half the
  build kept the old frame-stamp struct and wrote its checksum over the new
  field, which reads exactly like a crash.
