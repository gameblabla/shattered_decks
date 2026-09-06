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

| file | what | bytes |
|---|---|---|
| `DAT/ARENA.TEX` | 128x128 chunky board slab: a sandstone checkerboard | 16,384 |
| `DAT/FIELD.CRD` | 79 card faces, 32x32 chunky, ARENA palette | 80,896 |
| `DAT/HAND.CRD` | the same 79 faces, 32x24 planar, CARD palette | 30,336 |
| `DAT/TITLE.SCR` | the title painting, 320x200 planar + 25 palettes | 33,262 |

**Eight colours to the board, eight greys to the cards.**  Both sixteen-colour
sets (`src/generated/atarist_art.h`) are cut the same way: entries 0..7 are the
structural colours of that half of the screen -- the checkerboard's two tiles
and their grain, the groove, the slab's rim, the slot gold; or the HUD's panel,
gold, red and green -- and entries 8..13 plus black and white are an eight-step
GREY RAMP that the card paintings are dithered into, at the same indices in
both palettes, so one dithered picture reads identically in the hand and on the
3D board.

The six greys are the ST's own levels.  Three bits a channel is eight greys
(0, 36, 73, 109, 145, 182, 218, 255), so black + these six + white is an
exactly even eight-step ramp on the hardware with no two steps sharing a
bucket.

Colour on the cards was tried twice and lost twice, and the reasons are worth
keeping:

* **Fitted colour entries.**  Five (arena) or seven (card) k-means centres over
  every thumbnail is the average of seventy-eight paintings, so every card came
  back the same washed blue-grey and no card had its own colour anyway.
* **A palette per scanline, split five ways.**  The shifter really will reload
  sixteen entries every line, which would give each of the five hand slots
  three private colours per row -- but a monster drawn in three colours a line
  is noise, and the row-to-row palette drift streaks it.  Eight greys spend no
  entry on hue and every entry on tone.

The conversion rules that survived:

* **Equalise, do not stretch.**  These paintings are dark; a contrast stretch
  leaves most pixels inside the bottom step of the ramp and a card comes back a
  black rectangle with a white face in it.
* **Low-pass before dithering** (1.0 px, harder than the colour path's 0.7).
  What an eight-level dither carries is broad tonal massing; detail finer than
  that only becomes speckle that hides it.
* **FULL-strength error diffusion.**  The damping the old scattered palette
  needed is exactly wrong for a ramp: the error one level cannot hold is
  precisely what the next one can, and full diffusion is what turns eight
  levels into continuous tone.  The ground keeps its damped dither, because
  sand tones are not a ramp.
* **Sigils and the card back skip both.**  They are drawn, not photographed --
  a few flat tones on a flat field -- and equalising plus blurring turned all
  six supports into the same grey blob.  Equip and guard also had to be given
  different SHAPES, having been told apart by tint alone.
* **Spread the sand terciles.**  Sandstone is a low-contrast photograph and its
  three levels come back within a few units of each other, which makes the
  board a flat wash with no texture in it.
* **The tile grain hash must MIX.**  `(x*7 + y*13 + x*y) % 7` is linear in x
  for a fixed y: whole columns of a tile satisfied it at once and the board
  came out as plaid.

The same painting is converted twice, once per palette, because the board half
and the hand half of the screen do not share one.

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
| card art from real sources, eight-grey ramp | done, verified (on screen) |
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
