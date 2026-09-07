# Atari ST / STE port — status

Design: `ATARIST_PORT_PLAN.md`.  Build: `Makefile.atarist`.

## Build

```sh
make -f Makefile.atarist              # ST  -> build/atarist/waifu.st
make -f Makefile.atarist ste          # STE -> build/atarist/waifuste.st
make -f Makefile.atarist verify       # build, boot headless, read the probe
```

The output is a self-booting 720 KB FAT12 floppy: TOS runs `AUTO\WAIFU.PRG`.

## Raster splits

The screen carries a LIST of palettes (`Atarist_SetSplits`), not two.  Timer B
counts display-enable pulses, so its handler loads the next palette and re-arms
itself with the GAP to the one after -- gaps, not absolute lines, because the
subtraction would otherwise happen inside a level 6 interrupt on every split.

The duel uses two entries; the title screen uses twenty-five -- one per eight
scanlines -- which is what lets it show a photographic backdrop.
`Atarist_SetSplitEnabled(1)` *sets* the count to two rather than raising it, so
a scene that installed a gradient can hand the screen back as two halves.

## Audio

`tools/atarist/gen_atarist_audio.py` converts `msx_music/*.vgm` -- the same
AY-3-8910 recordings the MSX2 port plays -- into 50 Hz register-delta streams,
rescaling every period register from the MSX PSG's 1.7897 MHz to the ST's
2 MHz (otherwise the whole soundtrack plays a tone and a half sharp).  117 KB
for the seven tracks; the floppy carries them in `MUS\` and `atarist_disk.c`
loads one at a time into a 32 KB buffer.

Verified by recording: the MCP `record` tool writes Matroska with PCM audio,
and a Goertzel sweep over the result shows the dominant pitch moving from
second to second -- a still RMS level alone would not tell a tune from a stuck
drone.

## Art

Every picture in the port comes off the floppy, converted from `assets/source/`
by `tools/atarist/gen_atarist_assets.py`.  Nothing is drawn procedurally any
more.

| file | what | raw | on the floppy |
|---|---|---|---|
| `DAT/ARENA.TEX` | 128x128 chunky board slab: a sandstone checkerboard | 16,384 | 875 |
| `DAT/FIELD.CRD` | 79 card faces, 32x32 chunky, ARENA palette | 80,896 | 18,365 |
| `DAT/HAND.CRD` | the same 79 faces as WHOLE card fronts, 32x44 planar, CARD palette | 55,616 | 27,661 |
| `DAT/BIG.CRD` | 72 battle cards, 96x96 planar + a palette each | 335,232 | 301,605 |
| `DAT/TITLE.SCR` | the title painting with the logo baked in, 25 bands of 16 colours | 33,262 | 19,203 |
| `MUS/*.YMS` | the seven YM streams | 115,248 | 27,844 |

### Everything on the floppy is ZX0 packed

`tools/atarist/zx0pack.py` packs and `src/atarist/atarist_unzx0.S` (Marty and
Hodges' 68000 depacker) unpacks; the whole payload went from 611 KB to 391 KB,
which is what left 257 KB free on a 720 KB disk instead of about 36 KB.  Three things
about it are load-bearing:

* **The depacker wants `zx0`, not `zx0 -c`.**  It reads the current (v2)
  stream; classic v1 output walks off the end of the buffer.  Settled by
  transliterating the assembly into Python, and every blob the build writes is
  still round-tripped through that transliteration, so a stream the hardware
  could not depack fails the build instead of the boot.
* **Depacking is IN PLACE**, because a 512 KB machine has no room for a scratch
  buffer beside a 79 KB card sheet.  The packed bytes are read into the TAIL of
  the destination and the writer overtakes the reader; how far the writer may
  run ahead is measured per blob by the packer and stored in its header, so a
  caller only leaves `ATARIST_ZX0_SLACK` (256) bytes past the unpacked size.
  The worst blob in the set needs 4.
* **`BIG.CRD` is packed RECORD BY RECORD, behind an offset index.**  It cannot
  be packed as a file: the game seeks to one card out of seventy-two and the
  machine cannot hold the other seventy-one.  Per-record packing only buys 10%
  — a dithered planar portrait is close to noise, and there is no shared
  dictionary across records — but it keeps the single seek-and-read the loader
  always did.  One record does not compress at all and is stored raw; the
  loader takes either, which is the path that codes for it.

`BIG.CRD` is streamed, not resident: the two cards an attack needs are read
when the animation starts.  That read is the duel's worst frame, and getting it
there took two passes — four opens a battle (a palette and an image per card)
stalled for 251 vblanks; one whole-record read per card brought it to 185; and
holding the handle open for the life of the program brought it to 166, which is
below the music-track load the duel already pays on entry.  **The open is the
expensive half**, not the transfer: GEMDOS walks the directory and then the FAT
chain to a file read from 300 KB in.
| `DAT/TITLE.SCR` | the title painting, 320x200 planar + 25 palettes | 33,262 |

**Eight to structure, two to black and white, six fitted to the paintings.**
Both sixteen-colour sets (`src/generated/atarist_art.h`) are cut the same way:

* **EIGHT structural entries.**  On the board half, the two tile stones (three
  tones each, less one shared) and the gold that is both a card frame's rule
  and the cursor tint; on the card half, the panel, the HUD's gold, red, green
  and yellow, and the card frame's stone.
* **Black and white**, which both halves need and neither can fit.
* **SIX FITTED**, at indices 8..13, by k-means over the paintings with the
  other ten pinned as fixed centres, so a free entry only goes where the ten
  serve a painting worst.

and a card is then dithered against **all sixteen**.  That is the strategy: the
six carry the hues nothing else in the palette has, and the dither borrows the
tile browns, the frame gold, the panel blues, black and white for everything
else -- so a monster has ten colours of support behind its own six.

Two earlier cuts are recorded so they are not re-planned:

* **An eight-step grey ramp at 8..13.**  Exactly even on the hardware (three
  bits a channel is 0, 36, 73, 109, 145, 182, 218, 255), and it spends no entry
  on hue at all, so every card came back a grey blob.
* **A palette per scanline, split five ways.**  The shifter really will reload
  sixteen entries every line, which gives each of the five hand slots three
  private colours per row -- but a monster drawn in three colours a line is
  noise, and the row-to-row palette drift streaks it.

**The board is two photographs, and a card is the PC's own card front.**
`sandstone_1.png` IS the light tile and `sandstone_2.png` IS the dark one, each
resampled to sixteen texels and dithered into its own stone's tones; a card
face is `monster_card_front_template.png` (or the spell/trap template for a
support) with the painting fitted into the window
`card_front_template_pixel_monster_original_res.txt` names, scaled to whatever
size the ST shows a card at.  On the board that is 32x32 stretched over a
0.70 x 0.90 quad, which is the template's own aspect; in the hand it is the art
window plus its gold rule, because the hand slot is landscape and the ATK/DEF
numbers are drawn under it in the HUD font.

The conversion rules that survived:

* **Stretch, do not equalise, and do not lift.**  With six fitted colours and
  ten structural ones the dark end of a painting has somewhere to go.
  Equalisation is what the grey ramp needed; here it flattens tonal massing.  A
  brightness lift on top of an autocontrast bleached the whole sheet to
  white-on-blue.
* **Saturation up, contrast slightly down.**  Hue is what this palette can
  reproduce and what a sixteen-colour dither under-serves; contrast is what it
  exaggerates by itself.
* **Low-pass (0.5 px) before dithering**, and prepare the ART, not the
  composite: blurring the composited card softens the frame's gold rules into
  brown smears, and those rules are the only thing that says "card" at
  twenty-four pixels across.
* **Diffusion at 0.75** for a card, against the tiles' 0.60 and the ramp's 1.0.
  Sixteen fitted entries are dense in luminance and sparse in hue, so full
  diffusion overshoots into the saturated ones and speckles a grey robe with
  red and green confetti.  A TILE takes more (`TILE_DIFFUSION`) because its
  three tones are one stone's own terciles and therefore a real ramp, so the
  error always has somewhere near to go; at 0.35 the slab posterised into three
  flat bands with grain only along their seams.
* **Sigils skip the blur.**  They are drawn, not photographed, and rounding
  their shapes off turned all six supports into the same blob.  Equip and guard
  differ by SHAPE, not tint.
* **Crop in on the face.**  The content box is pulled in again -- a quarter off
  each side, a third off the bottom -- and the fit is anchored to the top of
  what is left.  A whole full-body painting in twenty-four pixels spends most
  of them on a torso the dither cannot hold, and the cards came back as
  seventy-eight dark columns.
* **The card back is WARMED into the board's browns.**  The painting is a navy
  field with a gold compass, and a face-down card lies among the tiles: navy on
  sandstone reads as a hole in the board.  Its channels are scaled, and it must
  skip the flat-art preparation entirely -- a per-channel autocontrast
  NEUTRALISES a single-hue picture and puts the navy straight back.
* **Crop a slab photograph to its inner field** (14% a side) before resampling.
  Both are a stone inside a bevelled border, and at sixteen texels that border
  is two texels on every side: the board came out a wall of picture frames.
* **Tone from a BOX average, grain from a point sample, mixed 0.30.**  A mean
  over eighty source pixels a texel is a perfectly smooth stone and the tiles
  came back flat; a point sample of the same photograph is a real stone pixel,
  so the two together are the slab's tone with the slab's own speckle, and
  neither is invented noise.
* **A tile may only reach for its own stone.**  The dark tile allowed the light
  stone's tones (the brighter half of the palette, which the dither wants) came
  back as the same tile, and the checkerboard disappeared.
* **Separate the tones by LUMINANCE under `st3`, about the middle one.**  This
  is what made the dark tile a flat brown square for a whole session.  The
  first cut only asked that `st3` be a different *tuple*, and TILE_DARK
  (99,68,30) against RIM_SIDE (107,77,34) passes that -- they differ in the blue
  bucket alone, by one step, on a colour with almost no blue in it.  The second
  cut separated by luminance but lifted upward from the darkest tone, which
  dragged the light slab to gold foil.  Anchor on the middle tone, push the
  other two outward, and require ~30 units of `st3` luminance (a full three-bit
  step is 36).  Terciles are also read with `contrast=False`, since a
  per-channel stretch neutralises a one-hue stone and the first run came pink.
* **The dark slab's shade applies to the TEXEL AS WELL AS THE ENTRY.**  Its
  three palette entries are its terciles times 0.72 -- it is darkened on top of
  being the darker photograph, or the two stones read as one texture with a
  seam -- but the photograph the dither read was not, so its whole luminance
  range sat *above* the three tones it was allowed and every texel clamped to
  the brightest.  This survived the separation fix: separation cannot help a
  distribution that misses its palette.
* **The groove is the dark stone's own darkest tone**, not a fourth colour.
  The dark stone has three entries in a palette this tight, and spending one on
  a line leaves it a two-tone.  The line still reads: it is the darkest brown
  on the board and it runs unbroken along a tile edge.
* **The card frame is QUANTISED, the painting is DITHERED.**  A frame is flat
  tones, two gold rules and a black keyline, every one of which the palette
  holds exactly; Floyd-Steinberg over it gains nothing and turns each rule into
  a dotted line.  The card is therefore composited in INDEX space
  (`framed_card_indices`), and the hand's band is cut out of that array rather
  than cropped and resampled as RGB -- resampling a dithered picture blends its
  noise into new colours and re-quantises them, which is why the hand row used
  to look muddier than the board.
* **The HUD's two saturated inks are kept out of the art.**  CARD_RED and
  CARD_GREEN have no neighbour in the palette, so the diffusion reached for
  them constantly and sprayed a grey robe with red and green confetti.  Damping
  the diffusion hid it and posterised everything else; excluding the two
  entries fixes it outright.

The same painting is converted twice, once per palette, because the board half
and the hand half of the screen do not share one.  It is converted a **third**
time for `DAT/BIG.CRD`, where the rules are different again: the battle
animation shows one card filling the screen with nothing else on it, so black
and white are pinned and the **other fourteen entries are fitted to that one
painting**.  See ATARIST_PORT_PLAN.md section 7b for the animation those drive,
and why the size is 96x96 rather than 112 (the disk, not the screen).

The title screen carries **one palette per eight scanlines** -- 25 raster
splits, about four hundred colours on a screen that shows sixteen.  Black, the
menu highlight and white are pinned in every band so the menu can be drawn over
the picture without knowing which band a glyph landed in.  The picture is a
whole screen in the shifter's own layout, so it is a block move, not a blit;
the blinking caret restores only the menu plate (40 rows), which took the title
from 7 vblanks a repaint to 3.

Boot now reads about 160 KB before the title appears -- roughly 950 vblanks,
nineteen seconds on the hardware -- so `Atarist_AssetsInit` puts LOADING on the
screen first.  A machine that cannot spare the 127 KB, or a floppy without the
files, still runs: the port falls back to flat colours and the probe reports
`NOMEM` / `NOFILE` with `mark` saying which read failed.

## Verification

`AtariST/hatari-headless-mcp` + `AtariST/etos512us.img`.  The MCP server has no
key-injection tool, so:

* `tools/atarist/hatari_mcp.py` is the JSON-RPC driver.  **`dump_ram`, `debug`
  and `reasm` all pause the emulator and do not resume it** — the driver issues
  `emu continue` after each, without which every second sample looks identical
  and a healthy game reads as frozen.
* `tools/atarist/verify.py` boots the image, finds `AtaristProbe` by scanning a
  RAM dump for its magic, pokes a scripted-input queue into it, samples again,
  and takes a screenshot.
* **Poked bytes must be space separated.**  `reasm` parses a bare hex run as a
  number, so `"0011"` writes one byte and every following byte of the payload
  shifts; the scripted input arrived as garbage and every duel measurement read
  as idle.  `Hatari.poke()` now takes real bytes and formats them.
* Probe field offsets are computed from the struct format, not written down:
  adding two fields silently redirected the `script_len` poke at `mark`.
* **A poke longer than about fifty bytes writes NOTHING and reports success.**
  Measured against a live probe: 48 bytes lands, 64 bytes does not, and the
  call is silent about it.  A scripted-input queue of more than a dozen entries
  therefore arrived as a block of zeros, which the game reads as "every entry
  lasts one frame" -- so the whole script fired in a dozen frames while the
  title was still coming up, and a blind run looked exactly as though the game
  had ignored its input.  `Hatari.poke()` now splits at 32 bytes.
* **A transient cannot be caught by a screenshot sweep.**  Headless Hatari runs
  about fifteen times real time, so the four-second attack animation is a
  quarter second of wall clock; and the MCP `screenshot` tool pauses the
  emulator, so sweeping it stalls the very thing being watched.  Build with
  `EXTRA_CFLAGS=-DATARIST_BATTLE_TIME_SCALE=16` and bracket `--run-seconds`
  across separate `verify.py` runs instead: each run is clean, and the
  animation is then seconds wide.

```sh
python3 tools/atarist/verify.py --image build/atarist/waifu.st --machine st
python3 tools/atarist/verify.py --image ... --script "START:2,0:30,A:2"
```

## Hardware hazards already paid for

* **Supervisor mode must be entered as the first line of the port.**  Reading a
  TOS system variable (the cookie jar pointer at `$5a0`) from user mode is a bus
  error, and TOS quietly returns the AUTO program to the desktop.
* **`Super(0)` toggles.**  `crt0.S` inquires with `Super(1)` first, because a
  program already in supervisor mode is dropped *out* of it otherwise.
* **Both ACIAs share MFP channel 6.**  A keyboard handler that leaves a MIDI
  byte unread re-enters at level 6 forever and the VBL stops; `Atarist_AciaPoll`
  drains the MIDI ACIA too.
* **Timer B is stopped and reloaded from the VBL**, not left free running: an
  MFP timer reloads on expiry, so a free-running split walks up the screen.
* **YM register 7's top two bits are the I/O port directions, not the mixer.**
  Port A carries the floppy drive select lines, so writing a plain `0x3f`
  "everything muted" turns port A into an input and the next `Fread` never
  returns.  `ym_write()` forces `0xc0` on, and register 14 -- port A itself --
  is never written from a music stream.
* **Every GEMDOS wrapper constrains its operands to registers.**  A `"g"`
  operand may be a stack-relative memory reference, and the wrappers push
  arguments before reading the later ones, so `%sp` has already moved: the call
  gets garbage.  It hung the boot on one build and returned an error on the
  next, from the same source.

## Where it stands

| area | state |
|---|---|
| boot, crt0, floppy image | done, verified |
| 320x200x16 double buffer, VBL flip | done, verified |
| Timer B raster split (arena / card palettes) | done, verified |
| chunky->planar, doubling + 1:1 | done, verified (doubling path on screen) |
| IKBD keyboard + joystick, scripted queue | done |
| probe + headless harness | done |
| YM2149 music: converter, floppy streams, player | done, verified (recorded and analysed) |
| GEMDOS disk layer | done |
| 3D board rasteriser (textured slab, flat-lying card quads, rim) | done, verified |
| planar 2D layer (rects, 8x8 text, masked blits) | done, verified |
| duel presentation over `msx2_duel.c` | done, playable, verified |
| card art in the PC card frame, 6 fitted colours + all 16 | done, verified (on screen) |
| title screen + menu, scene flow title<->duel | done, verified (real painting, 25 splits) |
| multi-palette raster split list (25-band title picture) | done, verified |
| Blitter block copy, used for the ground cache | done, verified on both builds |
| STE build (4-bit palettes, Blitter assumed) | boots and plays, verified |
| story mode beyond duel selection | not started |
| 416x276 overscan screens | not started |
| >48-colours-per-scanline trick | not started |
| STE DMA sound / MOD player | not started |

## Measured

Duel screen, plain ST, EmuTOS, Hatari:

| frame | vblanks |
|---|---|
| nothing moving | 1 |
| board moving (160x56 + doubling C2P) | 3 |
| the two settle frames (320x112 + 1:1 C2P + hand repaint) | 16 |
| a scene change that loads a music track from floppy | ~170 |

The last one is a real floppy read of up to 29 KB and is what it costs on the
hardware; it happens once when a scene opens, never inside one.

`worst_vbls` in the probe is the number to read; `frame_vbls` only sees the last
frame, and this screen is quiet most of the time.  Poke `worst_vbls` back to
zero to measure a window rather than the whole run (the first frame after a
resolution change builds a ground cache and is not representative).

### How it got there, and what not to undo

Four measured facts, each of which cost a factor:

1. **The 2D layer must not address the screen per 16-pixel group.**  A helper
   that did `Atarist_BackBuffer() + y * 160` internally is a cross-module call
   plus `__mulsi3` -- the 68000 has no 32x32 multiply -- per sixteen pixels, and
   it made the HUD cost 24 vblanks.  Row pointers, advanced per scanline.
2. **The HUD is laid out on 16-pixel boundaries.**  An aligned rectangle is
   whole 32-bit stores; a misaligned one is a read-modify-write per edge group
   per row, which is three times the cost.  Text sits on 8-pixel boundaries for
   the same reason: at that alignment a glyph falls inside one group.
3. **The ground is rasterised once, not once a frame.**  The duel camera is
   fixed, so the textured plane is the same picture every frame; it is cached
   per resolution (both, not one slot -- a single slot is rebuilt on every
   settle) and copied in.  That took a moving frame from 10 vblanks to 3.
4. **The board and the HUD have separate dirty counters.**  Both count down
   from 2, because the screen is double buffered and a single repaint leaves
   the other buffer one present behind.  The board animates; the hand does not,
   so a twelve-frame settle repaints it twice, not twelve times.

`atarist_raster.S` holds the two inner loops that stayed hand-written, and says
why in its header.  `ATARIST_BOARD_STILL_FULLRES=0` turns off the at-rest
full-resolution pass, which is the switch to reach for on slower machines.

## Ablation, the tool that found all of it

Guessing where 68000 cycles go does not work: every estimate above was wrong by
two to ten times until it was measured.  Compile a variant with one thing
removed, run the same scripted input, compare `worst_vbls`:

```sh
make -f Makefile.atarist EXTRA_CFLAGS=-DATARIST_BOARD_STILL_FULLRES=0
```

Hatari's own profiler is not usable here: the MCP `debug` tool pauses and
resumes the emulator per command, and `profile stats` reports no activity.

## The hand is a row of cards, and the card half is brown

`DAT/HAND.CRD` used to be a card's ART WINDOW plus the frame's inner gold rule
-- a band cut out of the composited index array -- and the duel screen drew a
filled panel and a one-pixel outline round each one.  That box is what made the
hand read as five text fields.  It is the WHOLE card front now, 32x44, the same
object the board's cards wear, and the duel screen draws no box: the card, its
two figures in the column beside it, and a yellow repaint of the front's own
outer keyline for the selection.  25 KB more in RAM, 12 KB more on the floppy.

The CARD palette's four structural tones went with it.  They were a cold
blue-grey panel ramp that only the boxes needed; they are now four browns cut
from the same two sandstone photographs the board is, so the card half and the
board half are one picture.  `CARD_PANEL_DARK` is shaded well below the darkest
tile because it is the band a card front sits ON.

## Escape is not a quit key any more

It dropped straight to the desktop from anywhere.  On an ST keyboard Escape is
directly above Tab -- the end-turn key -- so a mis-hit mid-duel looked exactly
like the game crashing out.  `Atarist_SceneStep` returns the quit request now
and only the title screen raises it; in a duel Escape is "back", and in the
hand it gives the duel up and returns to the title.

Space is CONFIRM in every duel state (it used to mean "battle phase" in the
hand and "cancel" in a sub-state), and B opens the battle phase.

## Photographing the battle animation

The bracketed `--run-seconds` sweep in the memory notes does not work any more:
the duel seeds itself from the vblank counter, so two runs are two different
games and a bracket compares nothing.  Drive ONE run instead and screenshot it
repeatedly -- `Hatari.screenshot()` pauses the emulator, so each shot must be
followed by `emu continue` -- with the animation built at
`EXTRA_CFLAGS=-DATARIST_BATTLE_TIME_SCALE=48`.  **`touch` the source first**:
the makefile does not depend on `EXTRA_CFLAGS`, so a rebuild that only changes
a `-D` silently keeps the old object and the animation still runs at shipping
speed.
