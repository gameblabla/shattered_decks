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
| M2 — VRAM map, compositor skeleton, glyphs, page flip | **done**: GRAPHIC 7 layer, double-buffered page flip, glyphs, fills, the duel board |
| M3 — asset pipeline, title screen | **done**: every picture the game shows is baked from `assets/source/` and streamed from the cartridge |
| M4 — the duel screen, played by a person | **done**: place, fuse, attack, end turn on real input, on the game's own 3D arena, with the cards mapped into their true projected quads and a baked camera move at the door |
| M5 — story mode | **done**: opening, sanctum map, dialogue, five duels, ending, with the dialogue a composited visual-novel scene — both speakers on the shipped painting at once |

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

### The duel screen

**The board is the game's board.** Nothing about the arena is drawn offline:
`tools/msx2/gen_msx_views.py` builds the capture tool out of `src/main.c`, runs
it with `--dump-msx2-views`, and bakes what `render_board()` drew. The floor,
the slab sides, the perspective and the slot layout are whatever the other five
targets render (`MSX2_PORT_PLAN.md` §0.3.1, §4.3). A view whose art disagrees
with the other targets is a capture bug, not a styling choice, which is the
whole point of taking this route.

The pose is `msx2_top_camera()` — raked, not straight down. A pure top-down
camera projects every slot to an axis-aligned rectangle, and the perspective
rasterizer would then be an elaborate way of blitting; raking the eye back buys
real trapezoids, so the COM row reads as further away because it is.

**The cards are drawn into those trapezoids**, by the §8 Tier A span rasterizer:

* `gen_msx_views.py` rasterises the 40x48 master texture into each slot's quad
  *offline* and serialises the result as run lengths — `COPY n` (texels straight
  from RAM to the VDP data port), `DUP n` (magnification, the same byte pushed
  again, no source read and no address re-set) and `ADV n` (minification). The
  Z80 does no arithmetic at all;
* a row whose texels run backwards — every COM-side card, which the shared
  renderer rotates 180 degrees on the board plane — reads the **mirrored** copy
  of the texture forwards instead. One extra 158 KB blob buys that; a reverse
  block copy would have been a second inner loop earning nothing else;
* the texture and the program are pulled into RAM for the draw (1,920 + 1,198
  bytes), so the inner loop never touches the mapper. About two frames a card,
  paid when a card *arrives*: a settled board costs nothing;
* an emptied slot is put back from the SLOTS blob, which is the *captured*
  arena's own quantised bytes at that quad's bounding box.

The selection bracket follows the quad — a rectangle around a trapezoid sits
visibly beside the card it is selecting — and it is drawn into a flat ring the
generator bakes just outside every quad, so erasing it is the same four VDP
`LINE` commands in `MSX2_RING_COLOR` and no artwork underneath is ever repaired.

**The hand is not board geometry.** It is a flat HUD strip on every target, so
it stays five axis-aligned 40x48 blits in a baked band — which also keeps the
cards a player is choosing between at a readable size.

**A duel opens on a baked camera move** (§4.6): eight whole pictures of the
114-row board band, streamed one after the next by the ordinary §6.2 path, about
a quarter of a second each. The last pose is byte for byte the resting view, so
the move ends already showing the picture the duel is played on and needs no
settle stream after it. There is no codec and no decoder anywhere in the port.
The write front crawling down the screen is visible and is the artefact §4.6.3
predicted; at these durations it reads as a deliberate transition.

Mid-duel camera moves are **not** done, and the reason is worth writing down: a
baked pose is a picture of the *empty* board, because cards depend on duel
state. That is truthful for an opening sweep and would be a visible defect for
any push during a turn — every monster would blink out for the second the camera
moved. They wait on the rasterizer being fast enough to repaint fifteen quads
inside a pose's own streaming time.

The player places monsters in attack or defence, plays supports and equips,
builds a multi-card fusion chain out of the hand, attacks, and ends the turn.
Both pages are tracked separately: this screen remembers what each buffer is
showing and paints the difference, at most one card a frame.

### Story mode

`STORY MODE` from the title runs the whole thing:

* the **opening** — Serena's four remembered lines, typed into the text box over
  the desert painting with her bust standing on it;
* the **sanctum map** — the stage the frontier has reached, the five opponents
  with their titles, everything past the frontier shown `- SEALED -`, and
  `LEAVE THE ROAD` back to the title. A cleared opponent can be replayed without
  moving the frontier, which is `g_story_progress` versus `g_story_duel_index`
  in `src/main.c` by another name;
* the **dialogue** — the nine or ten lines before each duel, in the writing's own
  order;
* the **duel**, dealt against the selected opponent on the stage's arena;
* the **ending** — the four closing lines over the ending painting, once the
  fifth opponent falls.

**A dialogue beat is a composited visual-novel scene** (§14.2), not a flattened
picture of one. The backdrop is one of the shipped `assets/source/bg/`
paintings with the text box baked into it, streamed **once per scene**. Serena
stands on the left and the opponent on the right, both on screen at the same
time, blitted at runtime from baked run tables; the inactive speaker is dimmed
rather than removed. The two brightness variants are cut from the same alpha
mask, so they cover byte for byte the same pixels and a speaker change is a pure
overwrite of two rects — no stream, no flip, no background repair. That replaced
about 2.2 MB of per-(duel, speaker) composites with 393 KB of busts.

The run table and the pixels are stored apart rather than interleaved, which is
the one departure from §14.2's letter: a bust is then blitted with the rectangle
copy the port already had, one call per opaque run, instead of a new skip-list
inner loop. Same arithmetic, one fewer piece of assembly.

The text pipeline is untouched. Story prose is still a **fixed-stride record
table in the cartridge**, parsed straight out of `src/main.c` by
`gen_msx_scenes.py`: ten kilobytes of prose is ten kilobytes the 32 KB code
budget does not have, and a second hand-copy of five thousand words of dialogue
is a second copy that goes stale. The typewriter is still per page, so a line
appears character by character on a double-buffered screen without the box being
redrawn.

### M1a evidence

`./msx2.sh verify --seconds 90` builds the **soak** ROM (`make -f Makefile.msx2
soak`, i.e. `-DMSX2_DEBUG_AUTOPLAY`, which hands the player's turn to the COM's
own AI) and reads the state probe back out of a RAM dump:

```
status       OK
duels done   2   player 2  /  com 0
RAM          data ends 0xD364, SP 0xF361, 8189 bytes free between them
```

Complete duels, cycling the five story opponents and free battle, with no
watchdog trip and no failed state invariant. The duel rules, the deck builder
and the AI all run on a Z80 inside the RAM budget.

The duel count is far lower than the 417 the renderer-less M1a build managed in
900 seconds, and that is the soak measuring the *presented* game: it takes one
rules step every frame (a person takes one every few seconds), streams a fresh
54 KB arena and plays a two-second camera move for every duel, and rasterises
cards into perspective quads underneath all of it. The number to watch here is
`status`, not the rate.

Footprint (`./msx2.sh ram`): 4,964 bytes of static RAM (8.2 KB free below the
stack) and 27,731 bytes in `_CODE`. The RAM went up by the rasterizer's two
buffers -- the 1,920-byte card texture and the 1,198-byte span program -- which
is what keeps its inner loop clear of the mapper.

---

## Build and verify

```
make -f Makefile.msx2            # regenerate tables, build the ROM
make -f Makefile.msx2 soak       # ... the same ROM, playing itself
./msx2.sh verify --seconds 90    # build the soak ROM, run blind, read the probe
./msx2.sh ram                    # code/RAM footprint against the budgets
```

**`verify` builds the soak ROM, and leaves it in `out/`.** The shipping build
waits for a hand on the joystick, so a blind run against it sits on turn 1 for
the whole run and reports a hang that is really an empty chair. Rebuild with
plain `make -f Makefile.msx2` before taking screenshots of the played game.

`MSXGL_PATH` selects the MSXgl tree (default `MSXgl-main`), `ROM_SIZE_KB` the
cartridge size. The default is 2048, which is the smallest size the baked assets
fit in (they end at 1712 KB); the shipping cartridge is 16384.

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

The game consumes the input layer, so the *shipping* ROM is driven the way a
player drives it: `msx2.sh` presses keys through openMSX's keyboard matrix from
Tcl (`--keys "6.0:space 6.8:space"`), and `./msx2.sh shot --page 0|1` decodes
either GRAPHIC 7 page to a PNG. That is how every screen below was checked.

The blind probe is for the *soak* ROM, which plays itself: it stamps its state
into RAM every frame (`msx2_probe.c`), the run script dumps all 64 KiB of
CPU-visible memory, and `tools/msx2/read_probe.py` finds the struct by its magic
and prints it. `MSX2_STAGE(n)` marks how far `main()` got, which turns a boot
crash into a number instead of a black screen.

---

## Layout

| File | What |
|---|---|
| `msx2_main.c` | boot, vblank ISR, the scene loop, the blind autoplay driver |
| `msx2_title.c/.h` | title screen: streamed art, logo, attract prompt, menu |
| `msx2_board.c/.h` | the duel screen: the ten projected slots, the hand strip, the cursor, the HUD, the opening camera move |
| `msx2_raster.c/.h` | §8 Tier A: the baked span-program card rasterizer |
| `msx2_story.c/.h` | story mode: the opening, the sanctum map, dialogue, the ending |
| `msx2_video.c/.h` | GRAPHIC 7 layer: pages, fills, glyphs, 2x text |
| `msx2_input.c/.h` | joystick + keyboard, latched once per frame |
| `msx2_stream.c/.h` | cartridge segment -> VRAM streaming through the 0x8000 window |
| `msx2_duel.c/.h` | the duel rules: board, LP, battle, supports, fusion, turn order |
| `msx2_cards.c/.h` | card stat tables (generated) |
| `msx2_probe.c/.h` | the headless observation channel |
| `waifu_msx2_s2_b0.c` | the page-0 code bank: where the streamer and the two big scenes are compiled |
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

`tools/msx2/gen_msx_views.py` is the board's own generator, imported by
`gen_msx_scenes.py` so the cartridge segment map stays owned by one tool. It
compiles `src/main.c` with `-DWAIFU_MSX2_VIEW_DUMP` into
`build/msx2_capture/waifu_msx2_dump`, runs `--dump-msx2-views`, and bakes the
resting view, the empty-slot tiles, the span programs and the camera-move strip
out of what the shared renderer drew. **`src/main.c` is therefore a build input
of the cartridge**: a change to the arena, to the card geometry or to
`msx2_top_camera()` re-bakes it, and `Makefile.msx2` says so.
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

1. **Code is 11.3 KB past 0x8000.** SDCC links `_CODE` contiguously from 0x4000,
   so it spills into page 2 — the *switched* streaming window. Streaming lives
   with this today only because the streamer is itself below 0x8000 and runs
   with interrupts off, so nothing in the swapped-out window is reachable while
   a chunk is in flight (see above). That is a real constraint, not a
   workaround to remove: every future routine that runs *during* a stream —
   a music replayer tick, a progress bar — has the same rule, and the
   build-time check in `pack_msx_rom.py` is what keeps it honest. Trimming
   MSXgl to GRAPHIC 7 and bitmap printing only (unused modes and print
   back-ends off in `msxgl_config.h`) took resident code from 31.4 KB to
   24.7 KB, and the duel screen and story mode were then compiled into the
   page-0 bank (`waifu_msx2_s2_b0.c`) rather than into `_CODE` — that bank still
   has ~10 KB free, and is where the next big scene should go. Banked code
   through `SUPPORT_BANKED_CALL` is the answer if that runs out too.

2. **The AI is slow.** Roughly 80 ms per rules step, most of it in
   `waifu_ai_choose_com_*` plus rebuilding `WaifuAiState` (226 bytes) for every
   query. It shows as the COM taking a beat to answer, which reads as thinking
   rather than as a stall, and the board keeps repainting underneath it — but it
   must be measured properly at M1b, and the state build should be made
   incremental if a turn ever visibly hangs.

3. **There is no save back-end.** `LOAD STORY` is drawn dim on the title and
   refuses the button, story progress lives only in RAM, and a run always starts
   at the first opponent. The MSX2 route is SRAM in a mapper segment or a disk
   file; neither is chosen yet.

4. **Sound is stubs.** `msx2_audio.c` records the requested track and reserves
   700 bytes for the Arkos AKG + ayFX state, so the RAM is already spent.

5. **Mid-duel camera moves are not baked** (`MSX2_PORT_PLAN.md` §21.2). A
   baked pose is a picture of the empty board, so a push during a turn would
   blink every monster off the field for a second. The strip player is already
   general; what the mid-duel moves need is a rasterizer fast enough to repaint
   fifteen quads inside a pose's own streaming time. Card flight (§4.5) is the
   other half of the same gap: the offline rasteriser can already turn a quad
   into a span program, it just does not yet interpolate a quad along a path.

6. **No name entry.** The other targets let the player name Serena before the
   first dream; here she is always SERENA, and the title's help line says so.

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
