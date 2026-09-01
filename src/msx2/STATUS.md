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
| M2 — VRAM map, compositor skeleton, glyphs, page flip | not started |
| M3 — asset pipeline, title screen | not started |

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

Neither emulator can inject input, so the ROM drives itself and stamps its state
into RAM every frame (`msx2_probe.c`); the run script dumps all 64 KiB of
CPU-visible memory and `tools/msx2/read_probe.py` finds the struct by its magic
and prints it. `MSX2_STAGE(n)` marks how far `main()` got, which turns a boot
crash into a number instead of a black screen.

---

## Layout

| File | What |
|---|---|
| `msx2_main.c` | boot, vblank ISR, frame loop, the blind autoplay driver |
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
(ATK/DEF/attribute/tribe lifted out of `waifu_assets.h`, 432 bytes of ROM).

---

## Open issues, in priority order

1. **Code is 3.2 KB past 0x8000.** SDCC links `_CODE` contiguously from 0x4000,
   so it already spills into page 2 — which the plan reserves as the *switched*
   streaming window. It works today only because MSXgl's crt0 maps segment 1
   there at boot and nothing switches it yet. The first asset stream will
   detonate this. Fix before M2: banked code through `SUPPORT_BANKED_CALL`, or a
   page-0 (segment 2) code area. Until then the honest resident-code budget is
   **16 KB, not the 32 KB the plan assumes** — worth correcting in the plan.

2. **The AI is slow.** A blind run manages roughly 16 rules steps per 80 frames,
   i.e. ~80 ms per step, most of it in `waifu_ai_choose_com_*` plus rebuilding
   `WaifuAiState` (226 bytes) for every query. Harmless now — the presented game
   wants about one step per frame anyway, and the plan already calls for the AI
   to run as a coroutine — but it must be measured properly at M1b and the state
   build should be made incremental if a turn ever visibly stalls.

3. **The player side is currently played by the AI.** `Msx2_DuelStep()` drives
   both sides so a duel completes with no input. The action API
   (`Msx2_PlaceMonster`, `Msx2_PlaySupport`, `Msx2_Attack`, `Msx2_EndTurn`) is
   already the seam a human player will drive; only the input layer is missing.

4. **Fusion is the pairwise chain only.** `Msx2_PlaceMonster` fuses a hand card
   onto an occupied slot via `fusion_result_for_cards`' recipes. The multi-card
   chain selection and the Water chain rule from `src/main.c` are not ported yet.

5. **Sound is stubs.** `msx2_audio.c` records the requested track and reserves
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
