# MSX2 port — status

Target: MSX2 (Z80A 3.58 MHz, V9938, 128 KB VRAM), NEO-16 mapper cartridge,
built with MSXgl + SDCC. Design: `MSX2_PORT_PLAN.md` at the repository root.

This port is **a fork, not a branch of the shared frontend**. It never compiles
`src/main.c`, and no MSX code exists outside `src/msx2/` and `tools/msx2/`.

---

## Where it stands

| Milestone | State |
|---|---|
| M1a — rules fit in RAM and a duel can be played blind | **done** (see below) |
| M1b — timing truth ROM (OUTI spacing, HMMM/LMMV throughput) | not started |
| M2 — VRAM map, compositor skeleton, glyphs, page flip | **partly done**: GRAPHIC 7 layer, double-buffered page flip, glyphs, fills; no duel board yet |
| M3 — asset pipeline, title screen | **done for full-screen scenes**: title art streams from the cartridge and the menu is driven by real input |

### The title screen

`./msx2.sh shot --page 1` after the default key sequence:

* the backdrop is `assets/source/title/title256_msx2.png`, the same picture the
  other targets show, dithered to GRB332 offline and **streamed out of the
  cartridge** — segments 4-7, 54,272 bytes, never linked into the Z80's 32 KB;
* the logo is drawn over it transparently with a hard shadow (the 6x8 bitmap
  font expanded 2x as runs of fills, since the printer has no scaler);
* `PRESS SPACE TO START` and the copyright sit directly on the artwork, white
  with a one-pixel black outline, because a colour that reads on the bright sky
  does not read on the dark rock and vice versa;
* the prompt blinks without re-streaming anything: both states of its strip are
  baked once into the offscreen VRAM rows below the visible 212, so a blink is
  one `HMMM` and no Z80 work at all;
* **nothing is ever drawn on the page the VDP is scanning out.** 128 KB of VRAM
  is exactly two GRAPHIC 7 pages, so a repaint goes to the hidden page and the
  swap happens in V-blank (`Msx2_VideoFlipRequest` / `Msx2_VideoPresent`, called
  straight after the frame loop's `HALT`).  Drawing on the visible page is what
  used to make the menu flicker: an erase and the redraw over it are two
  separate command-engine jobs, and a scan line passing between them shows the
  gap.  The consequence a scene has to honour is that a partial repaint reaches
  only *one* page, so every change is held in a dirty mask for
  `MSX2_VIDEO_PAGES` frames and painted again on the next buffer;
* the second buffer is built with one `Msx2_VideoCopyPage` (a whole-page
  `HMMM`, offscreen stashes included) rather than by streaming the cartridge a
  second time;
* SPACE opens the three-row menu (`STORY MODE` / `BATTLE MODE` / `LOAD STORY`,
  the last one dim because this target has no save back-end yet), and confirming
  a row deals a duel.  The help line lives *inside* the panel: it changes with
  the cursor, and anything redrawn over artwork would have to restore it.

Input is real: `msx2.sh` drives the openMSX keyboard matrix from Tcl, so the
menu is walked exactly as a player would walk it. There is no self-play code in
the ROM.

### M1a evidence

`./msx2.sh run --seconds 900` — 900 emulated seconds, no video, no input:

```
status       OK
duels done   417   player 215  /  com 202
RAM          data ends 0xC502, SP 0xF373, 11889 bytes free between them
```

417 complete duels, cycling the five story opponents and free battle, with no
watchdog trip and no failed state invariant. The duel rules, the deck builder
and the AI all run on a Z80 inside the RAM budget.

Footprint (`./msx2.sh ram`): 1282 bytes of static RAM (11.9 KB free below the
stack), 19538 bytes of code.

---

## Build and verify

```
make -f Makefile.msx2            # regenerate tables, build the ROM
./msx2.sh verify --seconds 60    # build, run blind, read the state probe
./msx2.sh ram                    # code/RAM footprint against the budgets
```

`MSXGL_PATH` selects the MSXgl tree (default `MSXgl-main`), `ROM_SIZE_KB` the
cartridge size (default 1024 during bring-up; the shipping cartridge is 16384).

`./msx2.sh shot` photographs the game by dumping VRAM and decoding it
(`tools/msx2/vram_png.py`) rather than by asking the emulator for a screenshot:
openMSX needs a display for that and its GL renderer returns an empty frame
under Xvfb. `--page 0|1` chooses which GRAPHIC 7 page to decode, including the
hidden one. Note that GRAPHIC 6/7 interleave VRAM across two 64 KB banks, so a
raw read of the emulator's `physical VRAM` shows every picture twice at half
width; the decoder undoes that.

`./msx2.sh neo-test` runs the NEO Mapper 1.2 large-ROM regression through the
project OpenMSX command layer. `./msx2.sh header`, `./msx2.sh asm`, and
`./msx2.sh cov` expose ROM analysis through the same layer. Scripted keyboard
input is available with `./msx2.sh input`; it uses the bundled deterministic
backend's active-low MSX PPI keyboard matrix.

### Do not use the bundled openmsx-headless to run this ROM

`MSXgl-main/openmsx-headless-.../openmsx` has a **broken Z80 core**: it does not
implement `LD r,(IX+d)` and `LD (IX+d),r` for `r != A` — the load or store is
silently skipped, registers keep their old values. SDCC uses IX as its frame
pointer and emits those two instructions constantly, so every C function with
stack locals computes garbage and the ROM dies within a few hundred
instructions. This was proved with a 20-instruction hand-written test ROM:

```
ld ix,#C300 / ld hl,#1234 / ld (ix-3),l / ld (ix-2),h
ld hl,#0000 / ld l,(ix-3)  / ld h,(ix-2)     ->  HL = 0000, should be 1234
```

Real openMSX gives `1234`. `openmsx selftest` still reports `z80 execution: ok`,
so the selftest does not cover these opcodes. Use that build only for what it
was patched for — NEO mapper and SCREEN 7/8 capture experiments — never to run
compiled C.

`msx2.sh` therefore drives **real openMSX** (20.0-rc1 at `/usr/local/bin/openmsx`,
which knows the `NEO-16` romtype), windowless via `set renderer none`.

### How a blind run is observed

The MSX2 game still does not consume the input layer, so the ROM drives itself
and stamps its state into RAM every frame (`msx2_probe.c`); the run script dumps
all 64 KiB of CPU-visible memory and `tools/msx2/read_probe.py` finds the struct
by its magic and prints it. The bundled backend now injects scheduled
keyboard-matrix events through `./msx2.sh input`; its regression probe verifies
SPACE on row 8/bit 0 and RIGHT on both PSG joystick connectors. This is ready
for input-path bring-up once the game consumes the new latch.
`MSX2_STAGE(n)` marks how far `main()` got, which turns a boot crash into a
number instead of a black screen.

---

## Layout

| File | What |
|---|---|
| `msx2_main.c` | boot, vblank ISR, the scene loop, the blind autoplay driver |
| `msx2_title.c/.h` | title screen: streamed art, logo, attract prompt, menu |
| `msx2_video.c/.h` | GRAPHIC 7 layer: pages, fills, glyphs, 2x text |
| `msx2_input.c/.h` | joystick + keyboard, latched once per frame |
| `msx2_stream.c/.h` | cartridge segment -> VRAM streaming through the 0x8000 window |
| `msx2_duel.c/.h` | the duel rules: board, LP, battle, supports, fusion, turn order |
| `msx2_cards.c/.h` | card stat tables (generated) |
| `msx2_probe.c/.h` | the headless observation channel |
| `msx2_audio.c/.h` | silent stubs that reserve the sound driver's 700 bytes of RAM |
| `msx2_libc.c` | `time()`/`clock()` for the shared deck builder |
| `compat/waifu_assets.h` | shim so `src/game/deck.c` compiles without the 5.3 MB asset header |
| `project_config.js`, `msxgl_config.h` | MSXgl build configuration |

Shared code compiled unmodified: `src/game/deck.c`, `src/game/ai.c`, plus
`card_ids.h` and `deck_pools.h`.

Generated: `src/generated/msx2_card_tables.h` from `tools/msx2/gen_msx_tables.py`
(ATK/DEF/attribute/tribe lifted out of `waifu_assets.h`, 432 bytes of ROM), and
`src/generated/msx2_scenes.h` plus `src/msx2/assets/*.bin` from
`tools/msx2/gen_msx_scenes.py` (full-screen art dithered to GRB332).
`tools/msx2/pack_msx_rom.py` writes those binaries into the built cartridge at
the segments the header names, and fails the build if the streamer has drifted
above 0x8000.

### How streaming works, and the two rules it lives by

A GRAPHIC 7 screen is 54,272 bytes, so scenes are never linked — they are packed
into whole 16 KB NEO segments and pushed at the VDP through the 0x8000 window
(`msx2_stream.c`). Two constraints make it work:

1. **The copy runs with interrupts off.** While the window holds picture data,
   none of the code up there exists — including the ISR. Interrupts come back
   between segments, so the longest blackout is one 16 KB chunk (~70 ms), and
   the streamer itself must be linked *below* 0x8000. `pack_msx_rom.py` checks
   that on every build rather than trusting it.
2. **The display is blanked for the duration.** In GRAPHIC 7 with the screen on
   the VDP wants ~29 T-states between VRAM writes and `OTIR` gives it 21, so
   bytes would be dropped. This is also why key presses are ignored while a
   scene streams, and why `msx2.sh`'s default key sequence starts at six
   seconds.

---

## Open issues, in priority order

1. **Code is 8.4 KB past 0x8000.** SDCC links `_CODE` contiguously from 0x4000,
   so it spills into page 2 — the *switched* streaming window. Streaming lives
   with this today only because the streamer is itself below 0x8000 and runs
   with interrupts off, so nothing in the swapped-out window is reachable while
   a chunk is in flight (see above). That is a real constraint, not a
   workaround to remove: every future routine that runs *during* a stream —
   a music replayer tick, a progress bar — has the same rule, and the
   build-time check in `pack_msx_rom.py` is what keeps it honest. Trimming
   MSXgl to GRAPHIC 7 and bitmap printing only (unused modes and print
   back-ends off in `msxgl_config.h`) took resident code from 31.4 KB to
   24.7 KB; banked code through `SUPPORT_BANKED_CALL` is still the answer if it
   grows much further.

2. **The AI is slow.** A blind run manages roughly 16 rules steps per 80 frames,
   i.e. ~80 ms per step, most of it in `waifu_ai_choose_com_*` plus rebuilding
   `WaifuAiState` (226 bytes) for every query. Harmless now — the presented game
   wants about one step per frame anyway, and the plan already calls for the AI
   to run as a coroutine — but it must be measured properly at M1b and the state
   build should be made incremental if a turn ever visibly stalls.

3. **The duel screen does not exist.** Choosing a row from the title deals a
   duel and shows a placeholder line of LP; the board, cards and animation are
   M2. Card art follows the PC-FX sizes (112x112 full size, plus thumbnails)
   and will use the same `assets/source/cards` originals through a second
   generator alongside `gen_msx_scenes.py`.

4. **The player side is currently played by the AI.** `Msx2_DuelStep()` drives
   both sides so a duel completes with no input. The action API
   (`Msx2_PlaceMonster`, `Msx2_PlaySupport`, `Msx2_Attack`, `Msx2_EndTurn`) is
   already the seam a human player will drive; only the input layer is missing.

5. **Fusion is the pairwise chain only.** `Msx2_PlaceMonster` fuses a hand card
   onto an occupied slot via `fusion_result_for_cards`' recipes. The multi-card
   chain selection and the Water chain rule from `src/main.c` are not ported yet.

6. **Sound is stubs.** `msx2_audio.c` records the requested track and reserves
   700 bytes for the Arkos AKG + ayFX state, so the RAM is already spent.

---

## Deliberate divergences from `src/main.c`

Noted here so they are decisions, not drift:

* Card ids are `u8` with `0xFF` for empty, not `int` with `-1`.
* Traps are owner-generic rather than player-only. The AI never sets one, so the
  two are behaviourally identical.
* The 25-entry `WaifuBattlePhase` collapses to five phases: everything in the
  original that exists to time an animation belongs to presentation here.
* One portability fix landed in shared code: `src/game/ai.c` used `-999999` as a
  score sentinel, which does not fit a 16-bit `int`. It is now `-30000`; real
  scores stay inside ±10000, so no target changes behaviour.
