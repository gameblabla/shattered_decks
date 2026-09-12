# The "60-colour 240x200" bitplane framebuffer, reverse-engineered

Source material: `SNES/alternative/cubes_demo.sfc` (32 KiB, `MG CUBES DEMO`,
LoROM/SlowROM) plus the forum thread in `SNES/alternative/notes.txt`. No source
was published, so everything below was recovered from the binary and then
confirmed by running it under `SNES/snesfaust/snesfaust-mednafen`.

Everything marked **measured** came out of the emulator. Everything marked
**derived** is arithmetic from the hardware timings. Everything marked
**inference** is my reading of the author's intent and is flagged as such.

---

## 1. What the demo actually is

Three spinning cubes, each confined to its own 64x64 pixel window, drawn
one-per-loop-iteration into a WRAM staging buffer and DMA'd into VRAM.

The 240x200 figure in the thread is **not** what this ROM does. This ROM is a
proof of the *pixel format* and the *fill primitive*; 240x200 is the author's
claim about what the format scales to. Section 5 checks that claim.

### Verified numbers

| Fact | Value | How |
|---|---|---|
| Distinct colours on screen | **exactly 60** | measured (PPM histogram) |
| Cube re-render cadence | every **4.33** NTSC frames | measured (per-window frame diff over frames 100-130) |
| Render throughput | **13.9 cube-renders/sec** | measured |
| Cost of one cube | **~193,000 CPU cycles** | derived from the above at 2.68 MHz |
| Unique PCs executed | 975 | measured (`snesfaust cov`) |

---

## 2. PPU configuration (from the init at $8000)

```
$2105 = $01   BG mode 1              BG1 = 4bpp, BG2 = 4bpp, BG3 = 2bpp
$212C = $01   main screen  = BG1 only
$212D = $04   sub  screen  = BG3 only
$2130 = $02   CGWSEL: colour-math addend is the SUB SCREEN
$2131 = $41   CGADSUB: ADD, HALVE, enabled on BG1 only
$2132        NEVER WRITTEN -> fixed colour = black
$2107 = $10   BG1 tilemap @ word $1000, 32x32
$2109 = $28   BG3 tilemap @ word $2800, 32x32
$210B = $00   BG1 character base @ word $0000
$210C = $02   BG3 character base @ word $2000
$420D        NEVER WRITTEN -> SlowROM, 2.68 MHz
$4200 = $00   NMI/IRQ disabled; the NMI and IRQ vectors are both a bare RTI.
              V-blank is polled on $4212 bit 7 (routine $8AB8).
CGRAM        20 entries: 0..15 = BG1 palette 0, 16..19 = BG3 palette 4
```

So the picture is: **one 4bpp layer on the main screen, one 2bpp layer on the
sub screen, combined by half-add colour math.**

---

## 3. Where "60 colours" comes from — and the quirk that makes it 60 and not 45

The naive reading is 16 x 4 = 64 combinations. The truth is more specific, and I
verified it exhaustively against the framebuffer:

* BG1 index 0 is transparent, so the main-screen layer only has **15 usable
  colours** (1..15). The demo's face-colour table `$8B8C` indeed never emits 0.
* BG3 index 0 is transparent too, so the sub screen has no pixel there. The
  colour-math addend then falls back to the sub-screen **backdrop**.
* **The hardware does not halve when the addend is the sub-screen backdrop.**
  So sub-index 0 is not "add black then halve" (which would darken); it is a
  clean **pass-through of the main-screen colour at full brightness**.

That gives exactly four states per main colour:

```
sub index 0  ->  a                     (full brightness, no math)
sub index 1  ->  (a + b1) >> 1
sub index 2  ->  (a + b2) >> 1
sub index 3  ->  (a + b3) >> 1
```

**15 x 4 = 60.**

I generated all 60 from the ROM's CGRAM image under that rule and compared it
set-for-set against the 60 colours in a captured frame: **exact match, zero
difference in either direction.** The first model I tried (halve on index 0 too)
mispredicted 14 of the 60, which is what surfaced the quirk.

Two consequences that matter for us:

1. **The 60 are not a free palette.** They are the outer product of a 15-entry
   palette and a 4-entry palette, with one axis pinned to identity. Any
   quantiser has to solve for both palettes jointly, not pick 60 colours.
2. **The sub layer is a shade/tint axis, not a colour axis.** In the demo the
   four BG3 entries are `-, (25,25,25), (13,13,13), (5,5,5)` — a neutral grey
   ramp. So BG1 chooses *what* the surface is and BG3 chooses *how lit* it is.
   That is a texture-times-light decomposition, and it is exactly the shape the
   card game wants (section 7).

---

## 4. The rendering method

Live path (confirmed by coverage): transform -> face loop -> per-scanline extents
-> byte-granular span fill -> poll V-blank -> two DMAs. There is a Bresenham line
drawer and a plot-pixel routine at `$8232`/`$8269`/`$82F1` with a full cube edge
list; **coverage proves they never execute** — dead wireframe scaffolding.

### 4.1 Geometry — `$8335`

Orthographic, no perspective divide, no Z-sort. 8 vertices of signed bytes at
`$8B44` (+/-16), rotated on two axes with an 8-bit sine table at `$8BC2` (cosine
is the same table at +$40) through the hardware multiplier (`$4202`/`$4203`) at
`$844D`. Screen coords are stored as byte pairs at `$0020`, biased by +$20 to
centre them in the 64x64 window.

### 4.2 Face loop — `$88C9`

Six quads from `$8B74` (4 vertex indices each). Backface culling is the sign of
a 2D cross product of two edge vectors; negative draws, otherwise skip. Nothing
else — that is sufficient and correct for a convex solid, and it is why there is
no depth buffer anywhere.

Per face the flat colour is fetched as two nibble-ish indices, `$8B8C` (4bpp,
values 1..15) and `$8B9E` (2bpp, values 1..3), and immediately **expanded into
bitplane masks**: bit *b* of the colour index becomes a whole byte of `$00` or
`$FF` in `$6F,$70,$75,$76` (planes 0-3) and `$79,$7A` (planes 0-1 of the sub
layer). This expansion is the entire trick.

### 4.3 Scanline extents — `$880E`

Four calls, one per quad edge. A Bresenham walk that, at every step, does
`min` into `$0200,y` and `max` into `$0300,y`. Cheap to write, correct for
convex faces, and — see section 6 — the single most expensive thing in the demo,
because it steps **per pixel** rather than per scanline.

### 4.4 The span fill — `$8656` (4bpp) and `$874F` (2bpp)

This is the part worth stealing. Address computation for the 4bpp buffer:

```
X = (y >> 3) * 256          ; tile row: 8 tiles across, 32 bytes each
  + (xleft >> 3) * 32       ; tile column
  + (y & 7) * 2             ; row within the tile
```

which is the **native SNES 4bpp tile layout, byte for byte** — planes 0,1 at
`+0,+1` and planes 2,3 at `+$10,+$11`. The 2bpp buffer uses the same shape with
128/16 strides. So the staging buffer is DMA'd to VRAM raw, with no packing step
at all. The buffer lives at `$7F0000` (2048 bytes, 4bpp) and `$7F0800` (1024
bytes, 2bpp) — one 64x64 window's worth, reused for all three cubes.

The loop then walks **tile columns**, not pixels. Per column it builds an 8-bit
coverage mask: `$FF`, ANDed with a left-edge mask `$873F[xleft & 7]`
(`FF,7F,3F,1F,0F,07,03,01`) on the first column and a right-edge mask
`$8747[xright & 7]` (`80,C0,E0,F0,F8,FC,FE,FF`) on the last.

* **mask == $FF** (interior column): four plain stores, planes 0-3, and the
  8 pixels are done. No read, no shift, no per-pixel anything.
* **otherwise** (edge column): read-modify-write, `(old & ~m) | (colour & m)`,
  four times.

Then `X += 32` and repeat. The 2bpp filler is identical with two planes.

**This is the whole idea: flat-shade a bitplane target by broadcasting the
colour index as plane-sized bytes, and only pay per-pixel cost at the two ends
of each span.**

### 4.5 Present — `$8AC7`

Wait for V-blank by polling, then two DMAs (`$4300 = $01`, `$4301 = $18`,
i.e. word writes to `$2118/$2119`): 2048 bytes to the BG1 char slot for this
cube, 1024 bytes to the BG3 char slot. 3072 bytes total, about half a V-blank.

### 4.6 The tilemaps — the "linear framebuffer" part

Both tilemaps are static, uploaded once, and never touched again. Each cube
window is an 8x8 block of cells whose tile indices run 0,1,2,...,63 in reading
order; everywhere else is tile 192, a cleared tile. That is what makes the WRAM
buffer a *linear* framebuffer: consecutive bytes in WRAM are consecutive tiles
on screen, so the DMA is a straight blit.

BG3's cells carry palette bits `%100` (palette 4), which is how the 2bpp layer
lands on CGRAM 16..19.

---

## 5. Does 240x200 actually fit? (derived)

### VRAM

240x200 = 30x25 = **750 tiles**.

```
BG1 4bpp tile data   750 x 32 =  24,000
BG3 2bpp tile data   750 x 16 =  12,000
BG1 tilemap 32x32              =   2,048
BG3 tilemap 32x32              =   2,048
                                 -------
                                  40,096   of 65,536
```

So yes, comfortably — 25,440 bytes spare. **The real ceiling is not bytes, it is
the tilemap entry format: 10 bits of character index, so 1024 tiles per layer.**
750 of 1024 used, 274 spare. That cap is what forbids a straight double-buffer
of the pixel data (1500 tiles), and it is why the author needs "partial rolling
buffers".

I could not reconstruct his exact "488 bytes are left over" from this binary —
that figure implies he commits ~65 KB, i.e. roughly a field plus a large second
staging region, and this ROM does not contain that allocation. Treat the 488 as
his number for his layout, not as a checkable fact.

### Bandwidth — this is the actual wall

NTSC master clock 21.477 MHz; 262 lines x 1364 = 357,368 master cycles/frame.
DMA moves one byte per 8 master cycles.

```
V-blank, 224-line mode:  38 lines x 1364 = 51,832 master  ->  ~6,200 bytes/frame
Full 240x200 6bpp field:                                       36,000 bytes
36,000 / 6,200 = 5.8 frames                              ->  ~10.3 fps
```

That is the honest V-blank-only ceiling for a *full* repaint, and it is below the
author's 15 fps. His 15 assumes partial updates: "with tile reuse (filled
polygons for example) the framerate can boost" — i.e. he is not repainting
36 KB every frame. Section 7 shows why flat-shaded geometry makes that enormous.

Do **not** reach for overscan (239 lines) to get more V-blank; it goes the wrong
way — 23 lines, ~3,900 bytes.

---

## 6. Why the demo is slow, and what that means

It is **not** bandwidth bound. 3 KB/frame is half a V-blank; the measurement says
one 64x64 cube costs ~193,000 CPU cycles, which is 4.3 frames of a 44,671-cycle
SlowROM frame. It is CPU bound in an unoptimised rasteriser. Cycle-counting the
listing:

| Term | Cost as written |
|---|---|
| Span fill, interior column | ~92 cycles per 8 px (4bpp) + ~65 (2bpp) = **~20 cycles/pixel** |
| Edge walk `$880E` | per-*pixel* Bresenham, ~45 cycles/step, ~1,700 steps/cube |
| Clear `$820E` | 3,072 bytes via 16-bit `STA long,x`, ~20,000 cycles |

Those three roughly account for the measured 193k. The headroom is large and
mostly obvious:

1. **The interior loop re-derives its coverage mask every column.** Peel the
   first and last columns out and the interior becomes straight stores.
2. **Planes 0,1 are adjacent bytes; so are 2,3.** One 16-bit store writes both.
   Four byte stores -> two word stores.
3. **Within one tile column, plane 0/1 rows are at +0,+2..+14 — contiguous.**
   Point the stack pointer at the tile and fill a whole 8x8 tile with 16 `PHA`s:
   32 bytes in 64 cycles, **0.5 cycles/pixel** for 4bpp. (Rebuild `S` after; and
   this must run with interrupts off, which is free here since the demo uses
   none.)
4. **The edge walker should be a fixed-point DDA**: one 16-bit add per scanline
   instead of one Bresenham step per pixel. Roughly a 10x cut on that term.
5. **FastROM is never enabled.** Header is `$20` and `$420D` is never written.
   Moving to `$30` + banks `$80+` is a flat +33% CPU for free.
6. **Don't clear what you overwrite.** Track the dirty rect, or fill background
   spans instead of clearing.

Taken together, a tightened version of exactly this method should be several
times faster than the demo. **The 15/20 fps numbers in the thread are bandwidth
estimates and should not be read as the demo's measured speed.**

---

## 7. H-blank siphoning — what it is and what it buys (derived)

The bandwidth wall in section 5 is "you may only touch VRAM during V-blank".
That is not quite true: VRAM is safe whenever the PPU is not fetching, which
includes **H-blank on every active scanline**. HDMA is the hardware's sanctioned
way to use that window — it is *defined* to fire in H-blank. So:

> **Siphoning = stream framebuffer bytes into VRAM through HDMA channels during
> the 200 active scanlines, on top of the normal V-blank DMA.**

### Getting the transfer mode right

You want two VRAM *words* per line per channel, i.e. the byte order
`$2118,$2119,$2118,$2119`. That is **HDMA transfer mode 5** ("2 registers, write
twice, alternating"). Mode 3 gives `$2118,$2118,$2119,$2119` and will shred your
data; mode 4 walks off into `$211A/$211B`. Use indirect mode (bit 6 of `$43x0`)
so one table entry can stream a long contiguous run: `{127 lines, ptr}` moves
508 bytes, two entries cover 200 lines.

Set `$2115 = $80` (increment after `$2119`) once and the VRAM address advances
by itself; you only reload `$2116` per band.

### The budget

Per fullsnes, HDMA costs 18 master cycles per active channel per scanline, plus
8 per byte, plus 16 per line if any channel is active.

```
8 channels x 4 bytes/line = 8 x (18 + 4x8) + 16 = 416 master cycles per line
over 200 active lines                            = 83,200 master cycles
                                                 = 23% of the frame, stolen from the CPU
bytes moved                = 8 x 4 x 200         = 6,400 bytes/frame
```

```
V-blank DMA alone            ~6,200 bytes/frame  ->  36,000/6,200 = 5.8 frames  -> 10.3 fps
V-blank DMA + full siphon   ~12,600 bytes/frame  ->  36,000/12,600 = 2.9 frames -> 20.7 fps
```

**That reproduces the author's "boost the worst-case ideal framerate to 20"
essentially exactly**, which is good evidence this is what he means.

### The three things that make it correct

1. **It is not free.** It costs ~23% of CPU time. Since the pre-siphon
   bottleneck is bandwidth, that is a good trade — but only until the rasteriser
   becomes the limit again. Enabling FastROM (+33%) more than pays for the
   siphon; do both or neither.
2. **You must never siphon into tiles the beam is about to fetch.** This is what
   "partial rolling buffers" is for: write into the band the beam has *already
   passed*, or into a pool of tiles the currently-displayed tilemap does not
   reference. Getting this wrong produces tearing that only shows on hardware.
3. **HDMA and general DMA share the eight channels.** A channel doing HDMA
   cannot also do a V-blank blit. Reserve e.g. channels 1-7 for siphoning and
   channel 0 for the V-blank DMA, which drops the siphon to 7 x 4 = 5,600
   bytes/frame.

We already do per-band HDMA in `snes_m7fb.c` (M7A/M7D/M7VOFS tables), so the
plumbing pattern is familiar — this is the same idea pointed at `$2118/$2119`
instead of the Mode 7 registers.

---

## 8. What this means for the card game

Current state (`snes_video.h`, `snes_m7fb.c`): Mode 7 direct colour, a 128x128
chunky bitmap, board at 128x80 resting and 64x40 moving, BBGGGRRR.

### The trade, stated honestly

| | Mode 7 direct colour (now) | Bitplane 4bpp+2bpp (this) |
|---|---|---|
| Resolution | 128x80 board | 240x200 |
| Colour | 256, but a fixed 3-3-2 grid (8R/8G/**4B**) | 60, freely chosen (as a 15x4 product) |
| Flat fill | 1 byte per pixel | **~0.5-2 cycles/pixel**, or free via tile reuse |
| Texture map | 1 byte per pixel — trivial | **must be bitplane-converted — this is the cost** |
| Upload for a full board | 10,240 bytes | 36,000 bytes (before tile reuse) |

The format is a large win for flat geometry and a real loss for textures. The
request — **flat-shaded board, texture-mapped cards** — sits on exactly the
right side of that line for the board and the wrong side for the cards, so the
design has to be built around making the card path cheap.

### 8.1 Use the two layers as material x light

This is the single most valuable thing to take from the demo, and it is not
obvious from the forum post:

* **BG1 (4bpp, main) = the material layer.** 15 colours. Card texels live here.
* **BG3 (2bpp, sub) = the shade layer.** 4 states, applied to whatever BG1 has.

Then:

* A **flat-shaded board polygon** writes a constant to both: 4 broadcast plane
  bytes + 2 broadcast plane bytes per 8 pixels. Fully in the cheap path.
* A **textured card** does per-pixel work on the 4 BG1 planes only, and writes
  **one flat shade value across the whole card face** into BG3 — because a card
  is a flat quad with one lighting term. That is a third of the per-pixel work
  saved for free, and it makes the card's lighting change (highlight on the
  selected card, dimming for a tapped card) cost *nothing*: it is a two-byte
  change to the flat BG3 span, not a re-texture.

### 8.2 Therefore: quantise card art to 15 colours, not 60

This is the practical restatement of section 3. If BG3 is the per-polygon
lighting axis, a card's texture only ever indexes the 15 BG1 entries; the
on-screen 60 come from the shade ladder multiplying those. So the art pipeline
job is:

1. Pick **15 colours** that cover all card art (this is the hard, global one —
   every card face shares the single BG1 palette, exactly like the current
   `gen_snes_textures.py` constraint).
2. Pick **3 shade entries** (plus the free pass-through) forming a lighting
   ramp. Neutral greys give brightness; a warm/cool pair gives cheap coloured
   lighting across the board.
3. Quantise each card face to the 15, and pick its shade index per polygon at
   render time.

The alternative — letting the shade vary per texel to reach all 60 — costs 6
planes of per-pixel work instead of 4 and forces a joint product-quantisation of
the (15, 4) palette pair. Don't, at least not first.

### 8.3 Making textured cards affordable

Per-pixel bitplane writes are a non-starter (~40+ cycles/pixel). The two routes
that work:

* **Pre-shifted strips (recommended).** Since a card is a flat quad, its span is
  an affine walk. Keep the texture in bitplane form and hold **8 pre-shifted
  copies** (by `x & 7`); a span then costs `LDA / ORA / STA` per plane per 8
  pixels — roughly **3x the flat cost, not 8x**. Build the 8 shifts in WRAM once
  per frame for the 1-4 cards actually on screen; do not bake 8x into ROM for
  every card face.
* **Bake per camera pose.** The 3D renderer already caches resting camera states
  (`renderer3d` split, the selective battle cache). Cards at a resting pose can
  be pre-converted to bitplane strips once and blitted byte-aligned — the
  fastest possible path — with the pre-shift route only used while the camera
  moves.

### 8.4 Tile reuse is the real win, and flat shading is what unlocks it

A fully-covered 8x8 tile of a flat-shaded polygon is byte-identical to every
other fully-covered tile of the same colour. So **don't render it — point the
tilemap at it.**

* Pre-build the solid tiles once: 15 x 32 = 480 bytes (BG1) + 4 x 16 = 64 bytes
  (BG3). Negligible.
* Per frame, classify each of the 750 cells: fully inside one flat polygon ->
  write 2 bytes of tilemap; straddling an edge, or touched by a card -> allocate
  from a dynamic tile pool and rasterise it.

Rough budget for a duel frame with four cards on a flat board:

```
cells covered by cards      ~140
cells straddling an edge    ~120
                            ----
dynamic tiles                260  x 48 bytes  = 12,480
tilemap refresh              750  x 2 x 2 BGs =  3,000
                                                ------
                                                15,480 bytes/frame
```

`15,480 / 6,200` = 2.5 V-blanks (~24 fps ceiling); with siphoning,
`15,480 / 12,600` = 1.2 frames (~50 fps ceiling). Against 36,000 bytes for a
naive full repaint, tile reuse is worth more than siphoning is — and it is
purely a software change. **Do tile reuse first; add siphoning only if the
budget still binds.**

Watch the 1024-tile index cap: 750 static + a ~270-entry dynamic pool is 1020.
That is tight. If it bites, shrink the field (240x160 = 30x20 = 600 static,
leaving 424) rather than trying to page tiles.

### 8.5 Order of work, if we pursue this

1. Port the span filler (`$8656`/`$874F` shape) into `snes_raster.asm`, with the
   fixes from section 6: peeled edge columns, 16-bit stores, `PHA` runs for full
   tiles. Measure it against the existing Mode 7 filler on a flat board.
2. Replace the per-pixel edge walk with a per-scanline DDA.
3. Add the solid-tile pool + tilemap-reuse classifier. This is where the
   framerate actually comes from.
4. Enable FastROM (header `$30`, code in `$80+`).
5. Only then: pre-shifted card texturing.
6. Only then: HDMA siphoning, with the rolling-band discipline from section 7.

Steps 1-4 are testable in isolation against `tools/snes/verify.py` and do not
require committing to the format; step 3 is the go/no-go measurement.

---

## 9. Reproducing any of this

```bash
cd SNES/alternative
../snesfaust/snesfaust-mednafen script cubes_demo.sfc in.txt 240 shot.ppm wram.bin cov.cov 0
# in.txt is one line: "0 0 0 0"
# cov.cov lists every PC executed -> proves $8232/$8269/$82F1 are dead
```

Per-window frame differencing (the 4.33-frame cadence) uses the `frame_dir`
arguments: `... "" "" fr 100 130 1`, then diff the three 64x64 windows at
x = 16..79 / 96..159 / 176..239, y = 104..167.

The 60-colour check: read CGRAM entries 0..19 from the ROM at `$9702`, apply the
rule in section 3, and compare the resulting set against the distinct RGB
triples in `shot.ppm`. They match exactly.
