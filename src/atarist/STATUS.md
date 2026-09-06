# Atari ST / STE port — status

Design: `ATARIST_PORT_PLAN.md`.  Build: `Makefile.atarist`.

## Build

```sh
make -f Makefile.atarist              # ST  -> build/atarist/waifu.st
make -f Makefile.atarist ste          # STE -> build/atarist/waifuste.st
make -f Makefile.atarist verify       # build, boot headless, read the probe
```

The output is a self-booting 720 KB FAT12 floppy: TOS runs `AUTO\WAIFU.PRG`.

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

## Boot hazards already paid for

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

## Where it stands

| area | state |
|---|---|
| boot, crt0, floppy image | done, verified |
| 320x200x16 double buffer, VBL flip | done, verified |
| Timer B raster split (arena / card palettes) | done, verified |
| chunky->planar, doubling + 1:1 | done, verified (doubling path on screen) |
| IKBD keyboard + joystick, scripted queue | done |
| probe + headless harness | done |
| YM2149 stream player | player written, converter not yet |
| 3D board rasteriser (textured ground, textured card quads, flat rim) | done, verified |
| planar 2D layer (rects, 8x8 text, masked blits) | done, verified |
| duel presentation over `msx2_duel.c` | done, playable, verified |
| card art from real sources | generated placeholders; converter not yet |
| title / story / deck editor, 416x276 screens | not started |
| STE blitter + DMA sound | detection only |

## Measured

Duel screen, plain ST, EmuTOS, Hatari:

| frame | vblanks |
|---|---|
| nothing moving | 1 |
| board moving (160x56 + doubling C2P) | 3 |
| the two settle frames (320x112 + 1:1 C2P + hand repaint) | 16 |

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
