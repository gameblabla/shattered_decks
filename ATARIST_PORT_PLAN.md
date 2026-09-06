# Shattered Decks — Atari ST/STE port plan

Answers `ATARI_ST_requirements.txt`.  The port is a **fork**, laid out the same
way the MSX2 one is: everything target specific lives in `src/atarist/` and
`tools/atarist/`, and the only shared C is the render-free rules core.

---

## 1. Targets

| build | machine | RAM | requires |
|---|---|---|---|
| `waifu.st`  | Atari ST (STF/STFM), 8 MHz 68000 | **512 KB** | nothing; Blitter used *if detected* |
| `waifuste.st` | Atari STE | **1 MB** | Blitter **and** DMA sound assumed present |

Both boot themselves: a FAT12 720 KB floppy image whose `AUTO\WAIFU.PRG` TOS
runs at boot.  No desktop, no GEM, no file selector.

## 2. What is shared and what is forked

Reused as-is (compiled into the ST build):

* `src/msx2/msx2_duel.c` — the render-free duel rules model.  It is already the
  Z80-independent fork of the rules in `src/main.c`; the only thing tying it to
  MSX was `msxgl.h`'s integer typedefs, which `src/atarist/atarist_types.h`
  supplies instead.
* `src/game/ai.c`, `src/game/deck.c` — opponent AI and deck construction.
* `src/generated/msx2_card_tables.h` — 432 bytes of ATK/DEF/attribute/tribe.

Everything else (video, input, audio, 3D, presentation, story, disk) is new
code under `src/atarist/`.  `src/main.c` is never compiled for this target.

## 3. Video model

### 3.1 In-game — 320x200, 16 colours, raster split

ST low resolution: four interleaved bitplanes, 32000 bytes per screen,
double buffered (two screens, flipped in the VBL by writing `$ffff8201/03`).

A **Timer B raster split** gives the two halves of the screen independent
16-colour palettes, which is what the requirements ask for:

```
scanline   0 .. SPLIT-1     ARENA palette   (3D board: sky, sand, stone, rim)
scanline SPLIT .. 199       CARD  palette   (card art, HUD, text)
```

Timer B counts display-enable pulses, so programming it with
`TBDR = SPLIT` and event-count mode fires exactly once per frame on the
boundary line.  Its handler writes 16 words to `$ffff8240` and rearms.  The VBL
handler restores the arena palette for the top of the next frame.  Both palette
sets are double buffered in RAM so the game may change either at any time
without tearing.

### 3.2 3D board — chunky + C2P, adaptive internal resolution

The board view occupies the **top** region (scanlines 0..SPLIT-1).  It is
rasterised into an 8-bit **chunky** buffer and converted with a C2P, because a
textured perspective rasteriser writing four interleaved planes directly is not
affordable at 8 MHz.

Internal resolution is adaptive, exactly as the FM TOWNS and MSX2 ports do it:

| board state | chunky size | C2P |
|---|---|---|
| camera moving / animating | 160 x (SPLIT/2) | 2x2 pixel doubling |
| camera at rest            | 320 x SPLIT     | 1:1 |

The doubling C2P is the fast path: one source byte produces two adjacent
output pixels and one source row produces two output rows, so a moving board
costs a quarter of the conversion of a still one.  The still frame is only
produced when the board is otherwise idle, so the extra cost is free.

The chunky buffer uses only palette indices 0..15 (the ARENA palette); the C2P
is therefore a straight 4-bit-plane transpose and never has to quantise.

### 3.3 Cards — full 320x200, drawn planar

Card thumbnails and the 2D battle animations are **not** part of the C2P path.
They are pre-shifted 4-plane sprites blitted straight into the planar screen at
full 320x200 resolution, so nothing about the 3D downscale touches them:

* ST: word-aligned planar blits from C/68000, with the software blit replaced
  by the **Blitter** when `_CPU`-independent detection (cookie jar `_SND`/
  hardware probe at `$ffff8a3c`) says one is fitted.
* STE: Blitter always.

### 3.4 Static screens — 416x276 overscan

Title, story portraits, menu and deck editor are not real-time, so they use the
left/right + top/bottom border removal path (`AtariST/Fullscreen_Attempt`,
`AtariST/mpp` mode 2) at 416x276, with per-scanline palette rewrites for far
more than 16 colours on screen.  These screens are streamed from floppy in
`mpp` form and never coexist with the in-game buffers.

## 4. 3D renderer

Ported from the shape of the existing ports (`src/engine/renderer3d.c`,
`src/msx2/msx2_board.c`), not from their pixel code:

* fixed-point 16.16 camera and 8.8 screen coordinates;
* the board is a flat grid of quads — **texture mapped** (requirement), using
  an affine per-scanline u/v stepper with a 64x64 wrapped texture and a
  power-of-two stride so the inner loop is `move.b (a0,d0.w),(a1)+` with two
  adds;
* the board *sides* / rim are **flat filled** convex quads (requirement);
* cards standing on the field are texture-mapped quads, using the MSX2
  shortcut: a card is always an axis-consistent convex quad, so it is rasterised
  as two edge chains with a per-row affine texture step rather than through a
  generic triangle setup.

Blitter is used where it wins: large flat fills and the planar card blits.  The
textured inner loops stay on the CPU (the Blitter cannot do a per-pixel
texture fetch).

## 5. Audio

* **In-game (ST and STE): YM2149 register streams.**  `tools/atarist/gen_atarist_audio.py`
  converts the same AY VGM files the MSX2 port uses (`msx_music/*.vgm`) into a
  compact 50 Hz per-frame register-delta stream, retuning the period registers
  from the MSX PSG clock (1.7897 MHz) to the ST's 2.0 MHz.  The player runs off
  Timer C / VBL and costs a few hundred cycles a frame.
* **Title and overworld:** MOD, from `AtariST/MOD_files`, with
  `AtariST/StandardST_MODplayer` (hmin849) on ST and
  `AtariST/STE_FullScreen_MOD_Play` (DMA sound) on STE.
* **STE in-game:** MOD if the frame budget allows once the renderer is
  measured; the YM path stays as the fallback and as the ST path.

## 6. Input

Raw IKBD handler (`AtariST/Other/ikbd.S` as the reference): the ACIA interrupt
maintains a key-down bitmap plus joystick 1 state, and the game latches it once
per frame.  Jagpad (`AtariST/Other/jagpad.S`) is read on STE as an extra
controller.

## 7. Disk and assets

`tools/atarist/gen_atarist_assets.py` converts the shared PNG sources into:

* `PAL.DAT` — arena/card/title palette sets;
* `CARDS.DAT` — 72 monster + 6 support thumbnails, 4-plane, plus a small
  index;
* `FLOOR.DAT` — 64x64 board textures (chunky, arena palette);
* `TITLE.MPP`, `END.MPP` — the 416x276 static screens;
* `MUS/*.YMS`, `MUS/*.MOD` — the audio streams.

Everything that does not fit in RAM at once is loaded per scene through
`atarist_disk.c` (GEMDOS `Fopen`/`Fread` while GEMDOS is still alive, which it
is: the game keeps TOS resident and simply owns the screen and the interrupts).
`lz4w` (`AtariST/Other/lz4w.S`) decompresses the larger blobs.

## 7b. The battle animation

An attack is presented full screen, **one card at a time**, and that is a
palette decision before it is a staging one.

`DAT/BIG.CRD` holds a 96x96 painting per monster with **its own sixteen
colours**: black and white pinned, and the other **fourteen fitted by k-means to
that one painting**.  Nothing else is on screen while a battle card is up --
there is no board, no hand and no HUD panel -- so nothing has to be reserved,
and a card gets roughly twice the colour budget of the six free entries it has
on the duel screen.  The cost is that **two cards can never share the screen**:
the second would have to be dithered into the first one's sixteen and would come
back as sludge.

That constraint is what dictates the choreography, which follows the PC-FX and
MSX2 presentations with a palette swap slipped into the gap:

1. **The attacker enters alone** from the right and holds in the centre with its
   ATK printed under it.
2. **It slides off to the left** and leaves the screen empty.
3. **The palette changes** -- this is the one moment nothing is drawn, and it is
   why the attacker has to leave before the defender arrives.
4. **The defender enters from the right** to the centre, with its ATK or DEF
   shown according to its position.
5. **The attempted attack is shown whether or not it lands**: the screen flashes
   and the card is struck and shakes.  Showing an outcome without the blow read
   as a slide show.
6. **The verdict.**  The defender is destroyed and dissolves; or neither falls;
   or the attack *fails and is countered*, in which case the palette swaps back
   over an empty screen, the attacker returns from the left, and it is the
   attacker that dissolves.  A set trap that fired takes the same counter path
   with the attacker dying and no defender ever shown.

A direct attack has no defender to bring on: the attacker leaves, the palette
stays its own, and the strike lands with the damage printed.

Mechanics that the 68000 imposes:

* **Every horizontal position is a multiple of sixteen.**  The card slides in
  whole 16-pixel groups, so the blit is four `move.w` per group with no shifting
  and no read-modify-write, and the panel behind it is an aligned rectangle with
  no masked edge groups.  Slowing the slide holds each step for more frames
  rather than shrinking it, so the grid holds at any speed.
* **The shake is vertical.**  A horizontal one would take the panel off the
  group grid for the sake of four pixels; moving a row pointer is free.
* **The flash is a palette write**, not a white rectangle: nothing to draw and
  nothing to undo.
* **The frame round the card is black and white only.**  Reserving an entry for
  a gold rule would take it off the painting, which is the one thing on screen.
* **Records are streamed.**  `DAT/BIG.CRD` is 327 KB -- monsters only, since a
  support is never an attacker or a defender and a face-down monster is turned
  face up by the attack itself -- and the two cards a battle needs are read with
  a seek and a read each (`Atarist_DiskLoadAt`).  Fixed-size records, so a face
  is `index * record`.
* `-DATARIST_BATTLE_TIME_SCALE=n` slows the whole animation proportionally.  It
  exists for verification: headless Hatari runs about fifteen times real time,
  so the shipping four-second animation is a quarter second of wall clock and a
  screenshot sweep steps straight over it.

Why 96x96 and not the 112 the board half is tall: a 112x112 4bpp face is 6,272
bytes and seventy-two of them are 451 KB, against about 390 KB free on a 720 KB
disk that already carries the program, 161 KB of board/hand/title art and 117 KB
of music.  The art does not compress -- measured, a byte RLE saves 2% and zlib
19% -- so 96 is the largest 16-pixel-aligned square that fits.

## 8. Verification

The bundled headless Hatari MCP build (`AtariST/hatari-headless-mcp` +
`AtariST/etos512us.img`) is the ground truth.  It has **no key-injection tool**,
so the port is verified the way the other blind targets are:

* `src/atarist/atarist_probe.c` keeps a magic-tagged probe struct (scene, frame,
  duel state, error code, frame-time histogram) that `tools/atarist/hatari_mcp.py`
  locates by scanning a RAM dump for the magic and decodes;
* the same struct carries a **scripted input queue** the harness pokes with
  `reasm`, which is how menus and duels are driven headlessly;
* `screenshot` proves what actually reached the screen.

`tools/atarist/verify.py` wraps: build → boot → wait → screenshot → probe
decode, and fails on a probe error code or a stalled frame counter.

## 9. Order of work

1. **Skeleton + boot** — `crt0.S`, `Makefile.atarist`, floppy image, probe,
   harness. *(verified: boots and stamps its probe)*
2. **Video core** — screens, palettes, VBL, Timer B split, C2P.
3. **Input** — IKBD, scripted queue.
4. **3D** — chunky rasteriser, textured floor, flat rim, adaptive resolution.
5. **Cards + HUD** — planar blits, 8x8 text, duel panel.
6. **Rules integration** — `msx2_duel.c` + AI + deck, full duel loop.
7. **Audio** — YM stream player, then MOD.
8. **Static screens** — 416x276 title/story/deck editor.
9. **STE build** — Blitter-always, DMA sound, richer palettes.
