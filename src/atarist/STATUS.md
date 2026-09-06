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
| 3D board rasteriser | not started |
| card art, HUD text | not started |
| duel presentation over `msx2_duel.c` | not started |
| title / story / deck editor, 416x276 screens | not started |
| STE blitter + DMA sound | detection only |

## Measured

Bring-up scene: 15 vblanks per frame, almost all of it the deliberately naive
per-pixel C that fills the chunky buffer.  It is not a renderer measurement; the
first real one comes with the board rasteriser.
