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
  snes_sprites.c              hand cards, cursor, HUD sprites
  snes_text.c                 chunky text into the Mode-7 buffer
  snes_story.c                VN flow + typewriter
  snes_deck.c                 deck editor
  snes_audio.c                snesmod driver glue
  snes_save.c                 SRAM
tools/snes/
  gen_snes_scenes.py          Mode 3 8bpp scenes from assets/source
  gen_snes_cards.py           card face textures + hand-card sprites
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

**Scale, and free adaptive resolution.**  With `M7B = M7C = 0`, `M7X/M7Y = 0`,
`M7HOFS/M7VOFS = 0`, the matrix is a pure scale and each screen pixel samples
source `x = A * screen_x`:

| mode | `M7A = M7D` | buffer | screen px per texel | bytes to DMA |
|---|---|---|---|---|
| **still** | `$0400` (4.0) | 128 x 112 | 2 x 2 | 14336 |
| **moving** | `$0200` (2.0) | 64 x 56 | 4 x 4 | 3584 |

Both are exact powers of two, so there is no resampling shimmer.  This is the
adaptive-internal-resolution trick the FM TOWNS, MSX2 and Atari ST ports all
use — except here **the doubling is free**, done by the PPU rather than by a
pixel-doubling copy.  Rendering a moving board costs a quarter of the pixels.

**Upload budget.**  NTSC vblank is 38 lines ≈ 51,800 master cycles ≈ 6.4 KB of
DMA.  So:

* moving (3584 B, 56 row-DMAs of 64 bytes) — **one vblank**, no tearing;
* still (14336 B) — **three vblanks**, uploaded top-down.  The still frame is
  only produced when the board is otherwise idle, so a 3-step progressive
  refresh of an unchanging image is invisible.

Mode 7 has no second tilemap base — there is no page flip — so this staged
upload *is* the double-buffering story, and it is why the moving case is sized
to fit a single vblank.

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
lines 160..223   hand / HUD band, chunky-filled + sprites over it
```

The 3D viewport is 128x80 texels still / 64x40 moving, which is what makes the
frame rate land where section 4 says it does.  HUD text is plotted straight
into the chunky buffer as 5x7-texel glyphs (10x14 screen pixels, 25 columns) so
it costs no sprite slots; hand cards, the slot cursor and the LP digits are
sprites on top.

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
| 5 hand cards, 32x48 each (32x32 + 32x16) | 10 | 3840 B |
| opponent hand backs (one shared card back) | 5 | 768 B |
| slot cursor + card-select frame | 4 | 512 B |
| LP digits, phase icons | ~12 | 1 KB |

Under 8 KB, so one OBJ name table is enough.  The hand row occupies lines
160..223 only, so the per-line sprite limits are never contended by the board.

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

~26 CPU cycles a texel.  Textures live **uncompressed and 256-byte aligned in
ROM** precisely so this loop can index a row with an 8-bit `Y` and read at
FastROM speed; WRAM would be 8 cycles per access instead of 6.  The loop is
unrolled 8x with the increments in direct page.

**(B) The general affine quad mapper — animating cards only.**

Cards that lift, tilt and fly during a play or a battle are not axis aligned, so
both u and v vary along a span.  These use two edge chains (the MSX2 port's
lesson: a card is a *convex quad*, not a trapezoid — assuming a trapezoid is
what put a card off the board rim), perspective-correct endpoints per span
(u/w, v/w, 1/w interpolated down the edges, one reciprocal lookup per span) and
an affine walk between them.  Their textures are 16x16 so the composite index
is `(v.int << 4) | u.int` and can be formed with one `and`/`ora` pair.  These
quads are rendered only in the **moving** (64x40) resolution, which is when they
occur anyway.

Painter's order, no z-buffer: floor, far support row, far monster row, near
monster row, near support row, then any animating card.

### 4.4 Frame budget

Framebuffer stores are the floor: WRAM is 8 master cycles per access on every
SNES, so ~26 CPU cycles/texel ≈ 170 master cycles.  One 60 Hz frame is 357,954
master cycles.

| mode | texels | overdraw | master cycles | frames | fps |
|---|---|---|---|---|---|
| moving 64x40 | 2560 | 1.6x | ~700 k | 2 | **~28** |
| still 128x80 | 10240 | 1.6x | ~2.8 M | 8 | **~7** |

Plus DMA (14 KB still = 115 k cycles), sprite/OAM work, rules and AI.  The still
frame is produced once when the board settles, so its cost is paid once, not
every frame; interaction always happens in the moving mode.  These numbers are
*estimates to be replaced by measurement at milestone M2* — the whole point of
doing the floor mapper first is to get a real number before the rest is built on
it.

Levers held in reserve, in the order they would be spent: shrink the 3D viewport
height; skip floor texels that a card will overwrite; a 32x32 (not 64x64) floor
texture so the row pointer never crosses a bank; unrolling the span walker
further; dropping the still mode to 128x64.

---

## 5. Memory map

**VRAM (64 KB)** — duel: `$0000-$3FFF` words = Mode 7 bitmap (both halves),
`$4000-$5FFF` words = OBJ tiles, rest spare.  Mode 3 scenes: BG1 chars
`$0000`, BG1 map `$7000`, BG2 chars `$6000`, BG2 map `$7800`, OBJ `$7C00`.

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
| `$C3-$C5` | card field textures: 72 monsters + 6 supports, 32x32 direct-colour, 256-byte-aligned rows (~81 KB) |
| `$C6` | arena floor textures, card frames, chunky font |
| `$C7-$CE` | Mode 3 scene images (title, 8 story backdrops, ending, battle art), LZSS packed |
| `$CF-$D2` | hand-card sprite tiles + palettes |
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
| `assets/source/cards/*.png|webp` (87 files) | 78 field-card textures + hand-card sprites + battle close-ups | `gen_snes_cards.py`: 32x32 direct-colour texture, 32x48 4bpp sprite, 8bpp Mode 3 close-up |
| `assets/source/textures/*.png` | arena floor, card frame | `gen_snes_textures.py` |
| `assets/source/bg/{desert,sky,stone,ember}.png` | arena horizon band, field variants | `gen_snes_textures.py` |
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

* **Input** — pvsneslib `padsCurrent()`.  D-pad moves the slot cursor, A
  confirms, B cancels, X opens the card detail, Y toggles the still/moving
  board, L/R page the hand, Start = phase advance, Select = menu.
* **Saves** — 8 KB SRAM at `$70:0000` (HiROM), a checksummed record holding
  story progress (`g_story_progress` frontier vs. selected foe, the distinction
  the PC-FX port established), the four deck slots the MSX2 continue-code format
  already caps at, and settings.  `snes_save.c` writes it under NMI-disabled
  guard and re-checksums.
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
  — the board is not a flat colour, the cursor moved between frames, the card at
  a slot matches the expected texture's mean colour.  A black screenshot is not
  proof of anything.
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

Regression targets: boot to title, menu navigation, a scripted full duel to a
win, a story chapter with dialogue, deck editor save/load round trip through
SRAM, and a perf capture of the duel board in both resolutions.

---

## 10. Milestones

| # | deliverable | done when |
|---|---|---|
| **M0** | toolchain built, `Makefile.snes`, HiROM/FastROM 4 MB ROM boots | `snap` shows the title colour on frame 60; `header` reports HiROM/FastROM/32 Mbit |
| **M1** | Mode 7 chunky framebuffer harness: 256 solid tiles, DMA presenter, still/moving scale switch | a scripted run shows a test image at both scales, pixel-exact 2x2 and 4x4 |
| **M2** | Q8.8 math, LUTs, **floor mapper** with the real arena texture | textured ground under a moving camera; **measured** ms/frame in `wram.bin` |
| **M3** | quad rasteriser + real card textures on the board | five slots per side show the right cards; convex-quad edges verified against the MSX2 rim bug |
| **M4** | rules integration, hand sprites, cursor, HUD text — playable duel | scripted duel plays to a win, verified frame by frame |
| **M5** | Mode 3 title / story / ending with real art and typewriter text | scripted story chapter captures |
| **M6** | audio: module playback + SFX | SPC upload asserted, ARAM state advances |
| **M7** | deck editor + SRAM saves | save/load round trip across a reset |
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
  depends on.
