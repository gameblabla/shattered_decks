# MSX2 Port Plan — SCREEN 8 / NEO-16 16 MB Cartridge

Target: MSX2 (Z80A 3.58 MHz, V9938, 128 KB VRAM), NEO-16 mapper ROM up to 16 MB,
built with MSXgl + SDCC, verified with the bundled `openmsx-headless` (NEO 1.2 +
planar-page fix) against a real `msx2.rom` BIOS.

This document is the complete design. It is written against the actual state of
this repository as of the current branch, and it names the exact files, budgets
and measurements each decision rests on.

---

## 0. Two corrections to the brief, stated up front

Both change the design, so they are settled here rather than discovered in month
three. Neither blocks the plan; the plan below already accounts for them.

**0.1 — "512x212 at screen mode 8" does not exist.** On the V9938 the two are
different modes:

| Mode | MSX name | Resolution | Colours | Palette | Pages in 128 KB |
|---|---|---|---|---|---|
| GRAPHIC 7 | SCREEN 8 | 256x212 | 256 simultaneous, **fixed GRB332** | none for the bitmap | 2 x 64 KB |
| GRAPHIC 6 | SCREEN 7 | 512x212 | 16 | 16 of 512, programmable | 2 x 64 KB |
| GRAPHIC 5 | SCREEN 6 | 512x212 | 4 | 4 of 512, programmable | 4 x 32 KB |

So the choice for high-resolution title/cutscene work is 512x212 **at 16
colours** (SCREEN 7), not at 256. Recommendation, and what the rest of this plan
assumes: **stay in SCREEN 8 for everything, including the title and the 2D
cutscenes.** For hand-painted anime portrait art, 256 fixed colours at 256x212
beats 16 palette colours at 512x212 by a wide margin, and never leaving GRAPHIC 7
removes an entire class of mode-switch bugs (VRAM layout, sprite table
relocation, command-engine coordinate space all change with the mode). SCREEN 7
is kept in the plan only as an optional, isolated presentation mode for the
credits roll and the ending text crawl, where 512-wide text is worth 16 colours
(§14.3). And if richer colour is what the title and ending really want, the
answer is not a V9938 mode at all — it is the optional MSX2+ path in §17, which
buys thousands of colours at *identical* streaming cost.

**0.2 — SCREEN 8 has no palette, therefore no palette fades.** The 256 colours
in GRAPHIC 7 are a hardwired GRB332 ramp; the V9938's 16 palette registers still
exist in SCREEN 8 but apply **only to sprites**. Every fade-to-black, cross-fade
and colour-cycling effect in the current game (`waifu_fm_video_fade_q8()`,
`WAIFU_FM_PALETTE_ENDING_BLACK`, the PC-FX 8bpp fade class) is unavailable as a
hardware effect. Fades must be produced one of three ways, all covered in §7.4:
pre-rendered dimmed copies in ROM (ROM is what we have), dithered wipes via the
VDP `HMMV`/`LMMV` fill commands, or a hard cut behind a 2-frame white/black
flash. This is a genuine loss of polish relative to the PC-FX and PC builds and
should be budgeted for in art, not fought in code.

---

## 1. What this port actually is

The existing codebase is a portable 8-bit-indexed **software framebuffer** game:
`waifu_fm_step()` renders a full 256x240 indexed frame with a fixed-point 3D
rasteriser, and each frontend uploads that framebuffer. Five targets already do
this (headless, SDL 1.2, SDL3, PC-FX, CD32X, FM TOWNS).

**That model cannot be carried to the MSX2, and no amount of optimisation
changes that.** The arithmetic:

- A full SCREEN 8 frame is 256 x 212 = **54,272 bytes**.
- The Z80 has 59,720 T-states per 60 Hz frame at 3.58 MHz.
- An unrolled `OUTI` to the VDP data port is 18 T-states (and needs padding
  during active display in GRAPHIC 7; see §6.1).
- One full-screen CPU upload = 54,272 x 18 = **977,000 T-states ≈ 16.4 frames.**

So the sustainable ROM→VRAM streaming rate is about **2,300 bytes per frame**
(≈40 % of a frame's cycles, leaving the rest for game logic, music and the ISR),
which is roughly **138 KB/s**. Full-screen animation of any kind is off the
table. There is no software rasteriser in this port at all.

The MSX2 port is therefore a **retained-screen compositor**:

> The screen is a still image assembled from pre-rendered ROM bitmaps. A frame
> repairs only what changed. Whole-screen changes are streamed into the hidden
> page over several frames and revealed with a page flip. Moving elements that
> must be smooth (cursors, selection highlights) are hardware sprites.

This suits the game: it is a turn-based card game whose steady state is a static
board with a moving cursor. The frames that are expensive — a card flying to the
field, an attack effect — are discrete, authored animations that can be
pre-rendered as localised cels and played back at 12–20 Hz over a static
background.

### 1.1 The porting seam

| Layer | Fate on MSX2 |
|---|---|
| `src/game/deck.c`, `src/game/ai.c` | **Port as-is.** Pure integer logic, no rendering. `WaifuDeck` is `int cards[40]` — 80 bytes under SDCC's 16-bit `int`. |
| `src/game/card_ids.h`, story tables, dialogue, fusion rules | **Port as-is**, but relocate text/tables into banked ROM (§5.3). |
| Battle rules inside `src/main.c` (`BattleCalc`, fusion/equip resolution, LP, deck-out, phase state machine) | **Extract, then port** — see §2. |
| `src/engine/renderer3d*.c`, `drawTexturedQuad`, `Point3D`/`FaceToDraw`, `divTab[1024]`, the whole Q8 3D pipeline | **Dropped entirely.** Replaced by offline pre-rendering (§4). |
| `src/main.c` 2D primitives (`draw_text*`, `fb_*` damage tracking, `put_px`) | **Replaced** by a VDP compositor (§7). The *concepts* (dirty rects, retained overlay) survive and are directly reusable. **Software blitting itself survives too** — see the note below. |
| `src/engine/renderer3d*.c` textured-quad path | Dropped as a general renderer, but its *job* comes back, scoped, as the card rasterizer of §8. |
| `src/engine/platform.h` seams | **Reused**, plus new MSX-specific seams (§9). |
| Audio (`src/game/sounds.c`, 44.1 kHz stereo mixer) | **Replaced** by a PSG tracker replayer (§12). |
| `src/game/assets.c` staging model | **Replaced.** On a linear-addressed cartridge there is no staging: assets are read straight out of the bank window into the VDP port. |

### 1.2 Software blitting is still a first-class technique

Dropping the framebuffer does **not** mean the CPU stops drawing. It stops
drawing *whole screens*. Three CPU draw paths remain, and the port depends on
all three:

| What | How | Cost |
|---|---|---|
| **Opaque rectangular 2D art** — card preview art, deck-editor icons, HUD panels, animation cels | Unrolled `OUTI` straight from the ROM bank window to the VDP data port. No mask, no staging buffer, no RAM. | ~18-21 T/px |
| **2D art with transparency** — portrait busts, cut-in overlays, effect cels | The same `OUTI` runs, driven by a **run-length skip list baked offline**: `(skip n, copy n, ...)`. A skip costs one VRAM address re-set (~50 T), an opaque pixel costs the same as any other. | ~18-21 T/px opaque, ~50 T per transparent run |
| **Perspective-warped card faces** on the 3D board | The scoped rasterizer, §8. | ~22 T/px (Tier A), ~50 T/px (Tier B) |

A 96x160 portrait at ~70 % coverage is roughly 10,700 opaque pixels ≈ 4-5 frames
of budget. That is entirely affordable for a one-shot. So software blitting of
portraits and cards is **available and used**; where §14.2 nonetheless bakes the
full-screen dialogue art, the reason is cost per beat, not capability.

---

## 2. Prerequisite refactor: extract the rules from `src/main.c`

`src/main.c` is 23,434 lines and holds 1,255 file-static globals. Rules and
rendering are interleaved throughout it. SDCC cannot compile it (16-bit `int`,
no 64-bit type — note the 75 `unsigned long long` profiling counters, all of
which are host-only), and even if it could, the resident code budget is 32 KB
(§5.2).

**Step 0 of this port, before any MSX code is written, is a
platform-independent refactor that benefits every existing target:**

Create `src/game/rules.c` / `rules.h` containing the pure, render-free duel
model:

- board state (`BOARD_COLS 5`, `BOARD_ROWS 4`, `HAND_SLOTS 5`, `I_FIELD 5`),
- LP, deck, hand, field, equip and fusion state,
- `BattleCalc` and attack/defence resolution,
- support-card effects (`is_thunder_support_card` … `is_heal_support_card`),
- the `WaifuBattlePhase` state machine's *transitions* (not its timings or
  camera work),
- story progression (`g_story_progress` vs `g_story_duel_index`),
- deck-editor list manipulation.

Acceptance criterion: `./waifu_fm_headless --frames 3600 --commands
scripts/battle_mode_demo.txt` produces byte-identical frames before and after
the extraction, on every existing scripted scenario under `scripts/`. This is a
pure move-and-rename with no behaviour change, verified by the existing harness
(`.claude/skills/headless-core/SKILL.md`).

Portability constraints the extracted module must satisfy, so SDCC can build it:

- `int` is 16-bit on Z80 — audit every `int` that can exceed ±32,767. LP values,
  ATK/DEF (max 3000 in `card_data.txt`) and card ids are all safe; RNG state
  (`WaifuDeckRng.state` is `uint32_t`) is fine but `waifu_deck_rng_next()`'s
  32-bit multiply is slow — provide a 16-bit LCG variant behind
  `WAIFU_RNG16` and confirm it does not change existing targets.
- No `long long`, no floats (already true outside profiling).
- No `Division()`/`divTab` dependency (that is renderer-only).
- Tables `const` so SDCC places them in `_RODATA` (ROM), not RAM.

The MSX build then compiles: `rules.c`, `deck.c`, `ai.c`, `card_ids.h`, story
tables, plus a new `src/platform/msx2/` frontend. It never compiles `main.c`.

---

## 3. Hardware budget

### 3.1 CPU

| Item | T-states/frame | Notes |
|---|---|---|
| Frame total @ 60 Hz | 59,720 | 3.579545 MHz |
| VDP interrupt entry + dispatch | ~400 | RAM ISR, §5.2 |
| Arkos AKG PSG replayer | 2,000–5,000 | measure in M1; AKM is smaller, AKG faster |
| ayFX SFX replayer | ~600 | |
| Input scan (joystick + keyboard row) | ~300 | |
| Rules/AI step | 500–3,000 | bursty; AI turn may span frames (§10.3) |
| **Available for ROM→VRAM streaming** | **~42,000** | ≈ **2,300 bytes/frame** |

### 3.2 VRAM (128 KB, GRAPHIC 7)

GRAPHIC 7 presents VRAM to the command engine as **256 pixels wide x 512 lines**.
Page 0 is Y 0–255, page 1 is Y 256–511. Only Y 0–211 of each page is displayed.

| Region | VRAM | Bytes | Use |
|---|---|---|---|
| Page 0 visible | Y 0–211 | 54,272 | display / back buffer A |
| Page 0 offscreen | Y 212–239 | 7,168 | font strip, cursor cels, small staging |
| Page 0 sprite tables | Y 240–250 | ~2,700 | SPT `0xF000`, colour `0xF800`, SAT `0xFA00` (MSXgl `VDP_G7_ADDR_*`) |
| Page 0 tail | Y 251–255 | 1,280 | reserve |
| Page 1 visible | Y 256–467 | 54,272 | display / back buffer B |
| Page 1 offscreen | Y 468–511 | 11,264 | resident card-art cache (§7.3) |

**Usable offscreen VRAM: ~18.4 KB.** This is the port's second-scarcest
resource after main RAM, and it is what makes the card cache in §7.3 possible.

Because sprite tables are addressed by R#5/R#11 independent of the displayed
page, one copy at the page-0 addresses serves both pages.

### 3.3 Main RAM — the hard constraint

Under MSXgl's NEO crt0 (`engine/src/crt0/crt0_rom_neo.asm`), ROM occupies pages
0–2 and **data lives at 0xC000**. Page 3 is 16 KB of RAM, minus the BIOS work
area (~0xF380–0xFFFF) and the stack.

> **Working RAM budget: ~12 KB.**

Everything is designed around this. There is no framebuffer, no asset staging
buffer, no decompression window (see §5.4 — assets are stored raw specifically
so that no RAM staging is needed).

Draft allocation:

| Block | Bytes |
|---|---|
| Duel state (2 decks, 2 hands, field, equips, LP, phase) | ~600 |
| Story/save state, player name, progress | ~200 |
| Deck editor working list (40 + collection cursor) | ~200 |
| Screen model: per-slot occupancy + dirty journal for both pages | ~800 |
| VDP command staging (`VDP_Command` structs, §7.2) | ~256 |
| Text/dialogue line buffer + typewriter state | ~512 |
| PSG replayer state (Arkos) | ~600 |
| ayFX state | ~100 |
| Sprite shadow tables (32 x 4 attr + colour) | ~640 |
| MSXgl globals + mapper shadow (`g_Bank0Segment[]`) | ~200 |
| Stack | 1,024 |
| C locals / spill / heap | ~2,000 |
| **Subtotal** | **~7.1 KB** |
| **Headroom** | ~4.9 KB |

The headroom is real and should be defended: SDCC's Z80 code generator spills
aggressively and `--max-allocs-per-node` tuning shifts RAM use noticeably.

**Optional Phase-2 RAM expansion (32 KB).** If the budget runs out: map ROM only
into pages 1–2 (NEO-16 banks 1 and 2, 0x4000–0xBFFF) and put **RAM in page 0**,
gaining 16 KB. MSXgl already supports this shape — `crt0_rom_neo.asm` has
`INSTALL_RAM_ISR` gated on `ROM_RAMISR == RAMISR_PAGE3`, and the ROM-resident
ISR is otherwise placed at segment offset `0x00020038` for NEO-16. The cost is
real: the BIOS ROM disappears from page 0, so **all BIOS calls are lost** and the
port must do direct PPI keyboard scanning and its own interrupt handling. Treat
this as a contingency, not a baseline; the plan below fits in 12 KB.

---

## 4. Offline pre-rendering: the heart of the port

The current 3D board render is a deterministic function of a camera pose. The
FM TOWNS target has already proved the technique in this repository:
`tools/fmtowns/gen_turn_card_cache.py` bakes 59 camera poses x 10 board slots
into `src/generated/fmtowns_turn_card_cache.h` (61,724 bytes) and
`fmtowns_turn_card_geometry.h` (per-pose, per-slot screen quads). The MSX2 port
takes the same idea much further: **the entire 3D presentation becomes ROM.**

### 4.1 The renderer that does the pre-rendering

The existing headless build. It is deterministic, it dumps PNGs, and it already
accepts scripted input:

```
./waifu_fm_headless --frames N --commands scripts/<scenario>.txt \
                    --out out_frames --dump-every 1
```

New tooling under `tools/msx2/`:

| Tool | Output |
|---|---|
| `gen_msx_views.py` | Renders each authored duel camera pose (empty board, no cards) at 256x212, quantises to GRB332, emits a raw ROM bitmap. |
| `gen_msx_cards.py` | Renders each card's single master texture (x3 shading variants), its big preview art and its deck-editor icon; quantises; emits raw tiles (§4.4). |
| `gen_msx_quads.py` | Per (view, slot): the projected destination quad, its cache bounding box, and the **baked span program** the rasterizer executes (§8.4). Reuses the projection that already produces `fmtowns_turn_card_geometry.h`. |
| `gen_msx_paths.py` | Card-flight keyframe quads per (hand slot → field slot) path, plus their baked poses (§8.4, §8.6). |
| `gen_msx_scenes.py` | Full-screen 2D scenes: title, ending, 5 duel-intro backdrops, sanctum map, plaza, deck-editor frame. |
| `gen_msx_cels.py` | Localised animation cels (attack, card flip, fusion burst, thunder) as fixed-size sprite strips. |
| `gen_msx_dialogue.py` | **Flattened** dialogue scenes: backdrop + character + empty text box baked into one 256x212 image per (scene, speaker, expression). See §14.2 — no runtime portrait compositing exists on this target. |
| `gen_msx2p.py` | Optional MSX2+ variants of the named enhanced images: SCREEN 10 (YJK+YAE) or SCREEN 12 (YJK) encodings of the same sources. See §17. |
| `gen_msx_text.py` | Dialogue and UI strings, tokenised and word-wrapped offline to the 32-column layout. |
| `gen_msx_rom.py` | Packs everything into 16 KB segments, emits the segment table and `msx2_asset_index.h`. |

The pattern (offline generator → checked-in `src/generated/*.h` → regenerated by
the target Makefile, never hand-edited) matches existing repository practice; see
the "regenerate-or-desync" warning in `.claude/skills/headless-core/SKILL.md`.
Add the same guard here: a build-time checksum of `card_data.txt` +
`cfx_screen_config.h` baked into `msx2_asset_index.h`, checked by
`Makefile.msx2`.

### 4.2 GRB332 quantisation

SCREEN 8's fixed palette is 3 bits green, 3 bits red, 2 bits blue:
`byte = (G<<5) | (R<<2) | B`. Blue resolution is poor, which matters for this
game's desert/night art.

Pipeline per source image:
1. Resolve the existing 256-entry indexed asset through
   `waifu_palette_rgb[]` (`src/generated/waifu_assets.h`) to RGB888.
2. Downscale to the MSX target size with a box filter in linear light.
3. Quantise to GRB332 with **ordered (Bayer 4x4) dithering**, not
   error-diffusion — ordered dithering is stable under the small per-frame
   sub-pixel changes between adjacent pre-rendered poses, so a camera move does
   not shimmer. Error diffusion crawls.
4. Emit a per-asset "colour key" index for transparency (the game already uses
   `IDX_BLACK 255` and a portrait mask; pick an unused GRB332 value, e.g. pure
   magenta `0x83`, and reserve it globally).

Deliverable: `tools/msx2/grb332.py` with a `--preview` mode that writes
side-by-side PNGs, so art can be reviewed before a 16 MB ROM is built.

### 4.3 Duel presentation: fixed views, not free cameras

The current duel uses continuously interpolated cameras (`lerp_camera`,
`interactive_turn_camera`, `turn_camera`). Continuous camera motion requires
full-screen redraws at animation rates — impossible (§1).

**Design: a small set of fixed views, with cuts (not moves) between them.**

| View | Source camera in `main.c` | Purpose |
|---|---|---|
| `V_HAND_P` | `player_camera()` | player hand, low angle |
| `V_TOP` | `top_camera()` | tactical top-down board |
| `V_PLACE_P` | `placement_camera()` | placing a card |
| `V_PLACE_E` | `enemy_placement_camera()` | COM placing |
| `V_BATTLE_TOP` | `battle_top_camera()` | attack resolution, player side |
| `V_BATTLE_TOP_E` | `enemy_battle_top_camera()` | attack resolution, COM side |
| `V_SIDE_0..3` | `side_battle_camera(row)` | per-row side view during an attack |
| `V_HAND_E` | `enemy_camera()` | COM hand |

Ten to twelve views. Each is one 54,272-byte ROM bitmap of the **empty board**
(floor, arena walls, backdrop, HUD frame — everything except cards and text).

For each view, `gen_msx_views.py` also emits, per board slot and per hand slot,
the **destination quad** the card occupies — the four projected corners, derived
exactly as `gen_turn_card_geometry.py` already derives them for the FM TOWNS
target. These are true perspective quads, not bounding boxes: the cards are drawn
into them by the scoped rasterizer of §8, so no approximation is made and the
views can be chosen for composition rather than to hide one.

This yields a compact runtime contract:

```c
typedef struct MsxSlotQuad {
    u8  x[4], y[4];        /* projected corners, GRAPHIC 7 pixels */
    u8  cache_x, cache_y;  /* baked bounding box in the offscreen card cache */
    u8  cache_w, cache_h;
    u8  shade;             /* which baked brightness variant (§8.7) */
    u16 span_off;          /* baked span program for this (view, slot)  (§8.4) */
} MsxSlotQuad;

typedef struct MsxView {
    u16 seg; u16 off;      /* ROM location of the 54,272-byte base image */
    MsxSlotQuad field[10]; /* 2 rows x 5 board slots, back-to-front order */
    MsxSlotQuad hand[5];
} MsxView;
```

### 4.4 Card art: one master texture, not a size ladder

Pre-warping card faces per (card x slot x view) is 78 x 15 x 12 x ~1.5 KB =
**21 MB** — it does not fit, and that is before any animation pose. So the cards
are the one thing this port does *not* bake: each card ships **one master
texture**, and the scoped rasterizer in §8 maps it into whatever quad the view
asks for.

| Asset | Pixels | Bytes | Per 78 cards |
|---|---|---|---|
| Master board texture | 48 x 64 | 3,072 | 240 KB |
| ...x 3 baked brightness variants (§8.7) | | | **720 KB** |
| Big preview / reveal art | 96 x 96 | 9,216 | 719 KB |
| Deck-editor icon | 24 x 32 | 768 | 60 KB |
| Face-down backs + support variants | | | ~20 KB |

78 = 72 monsters (`card_ids.h`) + 6 support variants (`deck.h`).

The big preview art and the deck-editor icons are ordinary opaque 2D bitmaps and
are **software blitted** (§7.2) — they are axis-aligned and need no rasterizer.
Only the board and hand cards go through §8.

### 4.5 What replaces camera motion

| Current effect | MSX2 realisation |
|---|---|
| Smooth hand→top camera lift (`IB_PLAYER_HAND_TO_TOP`) | Hard cut behind a 2-frame VDP `HMMV` white flash, or a 6-step vertical box wipe (§7.4) revealing the pre-staged view in the hidden page. |
| COM cursor walking the row (`IB_COM_TARGET`) | Hardware sprite cursor moving over the static `V_TOP` view. Free. |
| Card flying from hand to field | 12 baked pose quads per path, drawn by the §8 rasterizer (Tier A span programs, ~102 KB for all 50 paths) over a static background with per-frame rect repair (§7.5). Tier B covers any pose that was not enumerated. |
| Direct-attack impact burst | 10-frame 64x48 cel over the impact point. 3,072 B/frame — comfortably inside the 2,300 B/frame budget at 15 Hz. |
| Fusion / thunder / equip animations | Same cel technique, authored per effect. |
| Battle "burn wipe" consuming a card | Pre-rendered 8-frame generic cel (not per-card), software blitted over the card's cache rect. |

---

## 5. ROM layout, NEO-16 banking, and the 16 MB budget

### 5.1 The mapper

NEO-16 (`ROM_MAPPER == ROM_NEO16` in `engine/src/rom_mapper.h`): three 16 KB
banks, segment registers written as 16-bit values at 0x5000, 0x6000, 0x7000.
The bundled openMSX implements NEO spec 1.2 (14-bit segment number, up to
256 MB) — `NEO_MAPPER_1_2.txt`. 16 MB = 1,024 segments of 16 KB, well inside a
14-bit register, and inside the 12-bit pre-1.2 mask too, so **the ROM will also
work on emulators without the 1.2 fix** as long as no segment number exceeds
4,095. At 16 MB we use 1,024. Good.

MSXgl's `SET_BANK_SEGMENT(b, s)` maintains a RAM shadow `g_Bank0Segment[]`
because the mapper registers are write-only. That shadow is what makes safe
re-entrant banking possible (§5.5).

### 5.2 Address-space layout

From `projects/samples/s_neomap.c` and `engine/script/js/build.js:485-486`,
MSXgl's NEO-16 boot maps:

```
page 0  0x0000-0x3FFF   bank 0  <- segment 2   ISR + resident low code   [FIXED]
page 1  0x4000-0x7FFF   bank 1  <- segment 0   main resident code        [FIXED]
page 2  0x8000-0xBFFF   bank 2  <- segment 1   STREAMING WINDOW          [SWITCHED]
page 3  0xC000-0xFFFF   RAM                    data, stack, BIOS workarea
```

- **Resident code budget: 32 KB** (segments 0 and 2). This holds: crt0, the ISR,
  the VDP compositor, the OUTI streamer, the input layer, the rules engine, the
  scene state machines, and the Arkos replayer.
- **Segment 1 onward** is the banked pool: everything paged through 0x8000.
  Banked *code* uses MSXgl's `SUPPORT_BANKED_CALL` trampoline from
  `crt0_rom_neo.asm`; banked *data* is streamed, never called.
- Mapper register writes at 0x5000/0x6000/0x7000 land in page 1, which is ROM —
  correct and intended; the write is decoded by the cartridge, not stored.

32 KB of resident Z80 code is tight for a game of this scope. Mitigation, in
priority order: (1) put every scene's *presentation* code in banked segments and
call it through the trampoline, keeping only the dispatcher resident; (2) keep
all tables in banked ROM behind a `rom_read_*` accessor; (3) build with
`--opt-code-size` for non-hot modules and `--max-allocs-per-node` tuning for hot
ones. Track resident size in CI from the map file and fail the build over 30 KB.

### 5.3 16 MB budget

| Region | Size | Notes |
|---|---|---|
| Resident code (segments 0, 2) | 32 KB | hard limit |
| Banked code | 96 KB | scene presentation, deck editor, ending |
| Duel view bases (12 x 54,272) | 636 KB | §4.3 |
| Dimmed view variants (4 views x 3 steps) | 636 KB | for fades, §7.4 |
| Card master textures x 3 shading variants (78 cards) | 720 KB | §4.4, §8.7 — replaces the old size ladder and the pre-skewed variants |
| Card big preview art (96x96) | 719 KB | §4.4, software blitted |
| Card backs, deck-editor icons | 80 KB | |
| Baked span programs (views + card-flight paths) | 133 KB | §8.4 |
| Full-screen 2D scenes (~24) | 1,272 KB | title, ending, 5 intros, map, plaza, editor, results, loss |
| **Pre-composited dialogue scenes (~50)** | **2,650 KB** | **§14.2 — backdrop+character+box baked flat; replaces any runtime portrait set** |
| Animation cels (attack, flip, fusion, thunder, burn, UI) | 2,400 KB | the big discretionary pool |
| PSG music (Arkos AKM, ~10 tracks) | 150 KB | |
| MSX-Audio FM banks + optional ADPCM | 1,200 KB | optional hardware, §12.3 |
| MSX2+ enhanced image variants (~24) | 1,272 KB | optional, §17 — additive, never replaces the MSX2 set |
| Text, dialogue, tables | 80 KB | 5 duels x ~30 lines + UI |
| Font strips (8x8 + 8x16 outline) | 16 KB | |
| **Subtotal** | **~11.9 MB** | |
| **Slack** | **~4.1 MB** | more animation, more expressions, more views, more MSX2+ variants |

The slack is deliberate. The whole reason for a 16 MB cartridge is that **ROM is
how this port buys back the frames the Z80 cannot render**, and the honest
failure mode of the plan is running out of *cycles*, never out of ROM.

### 5.4 Do not compress

Recommendation: **store every VRAM-bound asset raw and uncompressed.**

Rationale: the streaming path is `ROM bank window → OUTI → VDP data port`, with
no RAM buffer anywhere. Any compression forces a RAM staging buffer we do not
have (§3.3) and adds decode cost on top of an already cycle-bound copy. MSXgl's
`compress.c` supports RLE/LZ variants; the one place they earn their keep is
**text**, which is RAM-bound and CPU-cheap to expand. Use a simple dictionary/
tokeniser there (`gen_msx_text.py`) and nowhere else.

The one exception worth measuring in M1: a **streamable RLE** whose decoder
writes directly to the VDP port without a buffer. If a typical board view
compresses 3:1, that turns a 24-frame full-screen stream into an 8-frame one for
about +6 T-states per output byte. Prototype it, measure it, then decide; do not
assume it.

### 5.5 The banking hazard, and its rule

The single most likely source of intermittent, hard-to-reproduce corruption in
this port: the ISR runs while the main code has the 0x8000 window pointed at a
graphics segment, and the music replayer reads its pattern data from the wrong
segment.

**Rule, enforced by design and by review:**

1. The Arkos replayer *code* is resident (segment 0 or 2), never banked.
2. The replayer's *data* lives in dedicated music segments.
3. The ISR does, every time, without exception:
   ```c
   u16 saved = GET_BANK_SEGMENT(2);       /* MSXgl RAM shadow */
   SET_BANK_SEGMENT(2, g_music_segment);
   AKG_Decode();
   SET_BANK_SEGMENT(2, saved);
   ```
   Cost ≈ 40 T-states. This is only correct because MSXgl maintains
   `g_Bank0Segment[]`; the mapper registers cannot be read back.
4. A long OUTI stream that crosses a segment boundary must either complete with
   interrupts disabled, or re-assert its segment after each `EI` window. The
   streamer in §6.2 does the former, in ≤16 KB chunks with a `HALT`-synchronised
   `EI` between chunks (§6.3).
5. Never place a jump target and its caller in different switched segments
   without going through the `SUPPORT_BANKED_CALL` trampoline.

Add an assertion mode (`MSX2_DEBUG_BANK`) that writes a magic value at the top
of every graphics segment and has the ISR verify the segment it selected before
decoding. Cheap, and it turns a class of silent corruption into a visible halt.

---

## 6. The streaming path

### 6.1 Timing caveat on raw `OUTI`

`MSXgl-main/showpicturemode12.s` is the reference technique: set the VRAM write
address, then run long unrolled `OUTI` blocks against the VDP data port,
switching ROM banks between blocks. It is written for MSX2+ SCREEN 12, and it
uses **bare 18 T-state `OUTI`s with no padding.**

On a V9938 in GRAPHIC 7 **during active display**, consecutive VRAM accesses
need roughly 29 T-states of spacing; 18 T-states will drop bytes. Three legal
resolutions, in order of preference:

1. **Blank the display** (R#1 bit 6 = 0) while streaming a whole-screen image
   into the hidden page. GRAPHIC 7 with the screen off allows the tight
   sequence. This is the primary path for full-screen loads, and it is invisible
   because the *displayed* page is unaffected — we blank only if we are
   streaming to the visible page, which the design avoids.
2. **Stream during vertical blank only.** ~ (262-212) = 50 lines ≈ 11,400
   T-states per frame ≈ 630 bytes of tight OUTI per frame. Correct, always safe,
   but 3.6x slower than the budget in §3.1.
3. **Pad to 29 T-states** (`OUTI` + `NOP`-equivalents, or `OUTI` interleaved
   with useful work) for streams that must run during active display.

One corroborating data point, from the same msx.org thread as the rasterisation
sources (§8.2): Grauw quotes **18 cycles per byte for a plain MSX2 VRAM fill**,
and derives 247 ms for a 256x192 screen — which is exactly 49,152 x 18 / 3.58 MHz.
So 18 T is the community's working figure for MSX2 and the tight `OUTI` sequence
is very likely correct at least with the screen blanked. That is corroboration,
not a measurement of *our* case: what still has to be established is the active-
display case in GRAPHIC 7 specifically.

**The M1 milestone must measure this on real timing before any content work
starts** (§15). Do not inherit `showpicturemode12.s`'s spacing on faith; that
file targets a different mode on a different VDP revision.

### 6.2 `msx2_stream.s` — the streamer

Our own assembly module, modelled on `showpicturemode12.s` but corrected and
generalised. Entry:

```
; HL = ROM source (0x8000..0xBFFF), DE = byte count, A' = starting segment
; VRAM write address already set by the caller (R#14 + port 0x99 pair)
msx2_stream_rom_to_vram:
```

- Unrolled `OUTI` in 256-byte blocks (`B=0` trick from the reference file: 256
  `OUTI`s drive B from 0 back to 0).
- On crossing 0xC000, `SET_BANK_SEGMENT(2, ++seg)` and reset HL to 0x8000. Note
  that `example_show_banks.txt`'s pattern of poking 0xC000 as a scratch cell is
  a *BASIC-era* convention; here we call MSXgl's `SET_BANK_SEGMENT` (or its
  inlined equivalent) so the RAM shadow stays correct — §5.5 depends on it.
- Assets are **16 KB-segment-aligned by the packer** (`gen_msx_rom.py`), so a
  54,272-byte view is exactly 3 full segments (49,152 B) plus 5,120 B, i.e.
  three tight 16 KB blocks and one short one. No unaligned crossing arithmetic
  at runtime.
- Two entry points: `..._fast` (screen blanked, 18 T) and `..._display`
  (padded to 29 T).

### 6.3 Multi-frame streaming

A full 54,272-byte view at 2,300 bytes/frame is **24 frames (0.4 s)**. That is
fine for a scene change and unacceptable for anything interactive, which is why
the design never streams a full screen inside a duel turn. Two mechanisms:

- **`msx2_stream_job`** — a resumable job descriptor in RAM (segment, offset,
  remaining, VRAM destination cursor). The main loop advances it by one
  budgeted chunk per frame, `HALT`-synchronised. Music and input keep running.
  When it completes, the caller is free to flip.
- **Blanked bulk load** — for scene transitions where the screen is already
  black/covered, blank the display and stream at full 18 T rate: 977,000
  T-states ≈ 16 frames (0.27 s) for a whole screen, with no interaction. Used
  for title → menu, duel entry, ending.

Both show the existing "LOADING" affordance the game already has
(`WAIFU_I_LOADING_ASSETS`, `waifu_assets_loading_percent()`), which maps
naturally onto job progress.

---

## 7. The compositor

### 7.1 Screen model

RAM holds a description of what is on each page, not the pixels:

```c
typedef struct MsxSlotState {
    u8 card;        /* card id or 0xFF */
    u8 flags;       /* face-down, equipped, defending, highlighted */
} MsxSlotState;

typedef struct MsxScreenModel {
    u8            view;                  /* current MsxView index */
    MsxSlotState  slot[15];              /* 10 field + 5 hand */
    u8            text_dirty;            /* HUD/dialogue regions */
    u8            page_valid;            /* bit 0 = page0 matches model, bit 1 = page1 */
} MsxScreenModel;
```

A frame: diff the model against `page_valid` for the page being drawn, produce a
short list of repair operations, execute as many as the frame budget allows,
carry the rest to the next frame. This is the same discipline the core already
implements with `waifu_fm_frame_dirty_rects()` / `fb_damage_*` — the concepts
port even though the code does not.

### 7.2 Repair operations, cheapest first

| Op | Mechanism | Cost |
|---|---|---|
| Move a cursor / selection marker | **Hardware sprite** — write 2 bytes to the SAT | ~50 T. Free. |
| Erase a rect to a flat colour | VDP `HMMV` | command engine, no CPU |
| Copy a rect from offscreen VRAM to the visible page | VDP `HMMM` | command engine, no CPU |
| Copy with transparency | VDP `LMMM` with logical op | command engine, slower |
| Draw a glyph | `HMMM` from the offscreen font strip | ~64 bytes of engine work |
| Blit an already-warped card from the offscreen VRAM cache | transparent `LMMM` | command engine; see §7.3 and §8.5 |
| Warp a card into that cache for the first time | §8 rasterizer | ~0.44 frame, paid once per card arrival |
| Blit an opaque 2D rect straight from ROM | `OUTI` per row + address setup | 40x28 ≈ 21,800 T ≈ 0.37 frame |
| Blit a 2D rect with transparency | `OUTI` runs + baked skip list (§1.2) | opaque pixels at ~18-21 T, ~50 T per skip run |
| Restore background under a card | `OUTI` from the view base in ROM | same as above |

The order matters enormously. **A `HMMM` costs the Z80 almost nothing** — set up
the command registers (~15 port writes), then poll or, better, fire-and-check
next frame. The CPU-side `OUTI` path costs a third of a frame per medium rect.
So the compositor's whole job is to keep work on the VDP command engine and off
the Z80.

MSXgl exposes all of this: `VDP_CommandHMMM`, `VDP_CommandHMMV`,
`VDP_CommandLMMM`, `VDP_CommandWait`, `VDP_CommandSetupR32`, and the
`VDP_Command` struct (`engine/src/vdp.h:86`).

### 7.3 The offscreen card cache — the key optimisation

18.4 KB of offscreen VRAM (§3.2) is enough to hold **every card image currently
on screen**, staged once when it arrives and thereafter blitted with `HMMM`.

The slots hold cards **already warped into their view's quad** (§8.5), each in
the bounding box the offline tool baked for that (view, slot), padded with
colour 0 so the transparent `LMMM` clips them to the quad. Page-1 offscreen
(Y 468–511, 11,264 B) is laid out as that cache:

- 10 board-slot boxes, sized by the *largest* warped bounding box that slot takes
  across all views — ~32x24 typical, ~768 B each = ~7,700 B, plus
- 5 hand-slot boxes at ~24x18 = ~432 B each = ~2,200 B, plus
- 1 scratch box for the moving card at ~56x40 = 2,240 B.

That is ~12.1 KB against page 1's 11,264 B, so the hand boxes spill into page-0
offscreen (Y 212–239, 7,168 B), which also holds the font strip (6,144 B for 96
glyphs as a 256x24 strip). The exact allocation is an M2 deliverable and is
*generated*, not hand-written: `gen_msx_quads.py` knows every warped box for
every (view, slot) and emits the packing. The sizes above are the starting
point.

Consequence: playing a card costs **one rasterizer pass of ~0.44 frame**
(§8.4), after which every subsequent redraw of that card — every view cut, every
highlight toggle, every background repair — is a free transparent `LMMM`. That is
what makes a view cut affordable: stream the new base into the hidden page over
24 frames while re-warping the fifteen cards for the new view (6.6 frames,
concurrent), `LMMM` them on, then flip. The cut is stream-bound.

### 7.4 Fades and wipes without a palette

Per §0.2, no hardware fades. Three tools:

1. **Pre-rendered dim steps.** For the four views that actually fade (`V_TOP`,
   title, ending, results), store 3 progressively darkened GRB332 copies. 3 x
   54,272 = 163 KB per view; 636 KB for four. Streaming each step costs 24
   frames, so a 3-step fade is 1.2 s — too slow for a snap fade, right for a
   deliberate scene close. For quicker fades, dim only the region that matters
   (e.g. the board rect, not the HUD).
2. **Dithered wipe via `HMMV`.** Fill successive dither-patterned bands of the
   visible page with the target colour using the command engine. No CPU cost,
   ~8–16 frames for a full screen, and it reads as a stylised transition rather
   than a broken fade. **This is the recommended default.**
3. **Box wipe reveal.** With the next view already composed in the hidden page,
   flip, then `HMMM` expanding bands of the *new* page over the old — or simply
   flip behind a 2-frame white `HMMV` flash. Cheapest and, for a duel camera
   cut, the most appropriate (it reads as an edit, which is what it is).

### 7.5 Page flipping

`VDP_SetPage()` (R#2). Flip on the vblank interrupt, never mid-frame.

The classic double-buffer trap applies and must be handled explicitly: **after a
flip, the new front page is stale with respect to every repair applied to the
old front page.** Policy:

- Every repair is journaled into `MsxScreenModel` and applied to **both** pages
  before that repair is considered retired. `page_valid` tracks which page has
  seen which state.
- For steady-state play (cursor on sprites, occasional glyph), we do **not**
  flip at all — we edit the visible page directly with `HMMM` (invisible;
  command-engine writes during display are fine) and keep both pages in sync
  lazily.
- Flipping is reserved for: view cuts, scene changes, and cel animation
  sequences, where the hidden page is fully composed before the flip.
- Cel animation ping-pongs: compose frame N+1 in the hidden page while frame N
  is displayed. Each cel frame needs background-repair + cel-blit into the
  hidden page, i.e. the *hidden* page must first be caught up to the current
  static state — which is why `page_valid` exists.

`VDP_SetPageAlternance` / R#9 `EO` is for interlaced double-height modes and is
**not** used here.

---

## 8. The scoped 3D card rasterizer

The board is pre-rendered ROM. **The cards on it are not** — they are drawn live
by a small, purpose-built affine texture-mapped quad rasterizer. This is the only
3D software rendering in the port, and it is deliberately scoped: at most fifteen
textured quads, each roughly 40x28 pixels, and never a floor, a wall, or a
backdrop.

### 8.1 Why a rasterizer rather than more baking

Pre-warping card faces offline does not close: 78 cards x 15 slots x 12 views x
~1.5 KB is **21 MB**, before a single animation pose. One master texture per card
plus ~2 KB of Z80 is 240 KB. The rasterizer is not a compromise here; it is the
cheap option, and it is what removes the ugly "views must be chosen so an
axis-aligned rectangle reads correctly" constraint that the earlier draft of §4.3
carried.

### 8.2 Sources, and what we take from each

Primary references (also listed in §20):

- `MSX_docs/RT3D/` — turbor's SandStone / Calculus work-in-progress disk, plus
  `notes.txt`, plus his explanation in the msx.org *3D rasterisation* thread.
  These are a real, shipped MSX2 3D engine, not theory.
- MSX Computer Magazine #92, *Multiplication* — the `a*b` lookup identity.
- Grauw's measurements in the same msx.org thread, useful for calibration.

| Technique | Taken? |
|---|---|
| Vertex list + triangles indexing into it; per-triangle normal, centre, and a **detail level** (outline / +medians / filled / +flat shading / textured / textured+shaded) | Concept adopted: our "detail level" is the per-slot draw mode (absent / face-down / solid / textured), chosen offline per view |
| **Backface cull from the normal's Z component alone**, before computing any other point — "if you do not need to calculate something, simply don't" | Adopted, taken to its limit: visibility is a baked per-(view, slot) flag. An empty or off-screen slot costs zero |
| Flat shading straight out of that same normal Z | Adopted **offline** — §8.7 |
| Two Bresenham edge walks; issue a **VDP LINE command** per span, and by the time the CPU has the next pair of endpoints the VDP has finished. Only reload the VDP registers that actually changed. Choose horizontal vs vertical spans by the triangle's larger extent | Adopted for *solid* fills (erase, highlight frames, face-down backs). Not for texture — the VDP cannot fetch texels |
| **Textured triangles always use horizontal spans and `OUT` the colour codes directly in SCREEN 8. No perspective correction — linear offsetting — and the offset list is computed once for the widest span and reused** | **Adopted verbatim. This is the core of §8.4 and §8.6.** |
| Erase using the previous frame's min/max bounding box via VDP block fill, and compute the next frame's normals *while the VDP is filling* | Adopted — §8.6, and it is the same CPU/command-engine overlap the compositor already relies on (§7.2) |
| Bucket sort on triangle-centre Z, then painter's algorithm back-to-front | Adopted **offline**: the baked span program lists a view's slots already in back-to-front order |
| Vertices stored as *(index into x-array, index into y-array, index into z-array)*, so each of the 9 rotation-matrix elements is multiplied once per distinct coordinate value into a cache, and each vertex transform becomes 9 table lookups and adds — **zero multiplications per vertex** | **Not needed at runtime**, because our geometry is baked (§8.4). Recorded here because it is the technique that makes a *live* camera affordable if §19's open question 4 is ever answered "yes" |
| `a*b = f(a+b) - f(a-b)` where `f(x) = floor(x^2/4)`; two page-aligned 256-byte tables (low and high bytes); 43 T-states, constant time; operands must satisfy -64 <= a,b <= +63 so neither sum nor difference leaves the table | Adopted for Tier B — §8.6 |
| "Try to do each calculation only once. And if you do not need a value, don't calculate it." | The governing principle of this whole section |

### 8.3 Two tiers

| | Tier A — resting cards | Tier B — the moving card |
|---|---|---|
| Count on screen | up to 15 | 1 |
| Geometry | fully baked offline | interpolated at runtime |
| Runtime arithmetic | **none** | edge DDA + u/v stepping |
| Cost | ~0.44 frame per card, **paid once** (§8.5) | ~1.9 frames per card per pose |
| Used for | every board and hand card at rest, and every enumerable animation pose | poses that cannot be enumerated |

Tier A is the shipping fast path and covers, by baking more poses, everything the
game currently authors. Tier B is a real runtime rasterizer, fully specified
below, kept because it is what stops the design dead-ending the first time the art
wants a motion nobody enumerated.

### 8.4 Tier A: baked span programs

`gen_msx_quads.py` reuses the *exact* projection already in `src/main.c` — the
one that produces `src/generated/fmtowns_turn_card_geometry.h`, which is a
per-pose, per-slot quad table this repository already ships. For each of the ~12
views x 15 slots it emits the destination quad, rasterizes it **offline**, and
serialises the result as a tiny program:

```
NEWROW  y, x0, texrow      ; begin a destination scanline
COPY    n                  ; n new texels, source advances
DUP     n                  ; repeat the last texel n times (magnification)
SKIP    n                  ; n transparent pixels: re-set the VRAM address
ENDROW
END
```

`COPY`/`DUP`/`SKIP` are exactly turbor's "linear offsetting with a precomputed
offset list", expressed as run lengths instead of offsets so the inner loop can
use block I/O. The runtime interpreter holds the VDP data port in `C` and the
texel pointer in `HL`, so:

| Op | Inner cost | Notes |
|---|---|---|
| `COPY n` | ~18-25 T/px | `OTIR` (21 T/px) is the baseline; an unrolled `OUTI` run is 18 T/px where the run is long enough to be worth the setup |
| `DUP n` | ~12-19 T/px | the texel is already in `A` |
| `SKIP n` | ~50 T total | one VRAM address re-set, independent of `n` |
| `NEWROW` | ~50 T | VRAM address set |

**These are design-band estimates, not measurements.** The exact inner loop and
its real cost are an M1/M5 deliverable, like every other number in this document
that precedes M1.

Working figure: a 40x28 card is 1,120 pixels at ~22 T/px plus 28 row setups,
≈ 26,000 T ≈ **0.44 frame**. Program size ≈ 28 rows x ~6 bytes ≈ 170 bytes, so
12 views x 15 slots x 170 ≈ **31 KB** of ROM. Baking the card-flight poses as
well (50 hand-slot→field-slot paths x 12 poses x 170 B) adds ~102 KB. Both are
noise against 16 MB.

### 8.5 The warped-card VRAM cache — why 0.44 frame per card is affordable

Fifteen cards is ~6.6 frames of rasterisation. Per frame that is impossible; per
*card arrival* it is nothing. So the rasterizer never draws to the screen:

1. When a card appears in a slot, the rasterizer draws it **into that slot's
   baked bounding box in the offscreen VRAM card cache** (§7.3), writing colour
   index **0** everywhere outside the quad.
2. Every later appearance — after a background repair, a highlight toggle, a page
   sync, a redraw following a wipe — is a **transparent `LMMM`**: a V9938 logical
   VRAM→VRAM move whose transparent operation does not write source colour 0.
   Command engine, essentially no CPU.

This is what marries the rasterizer to the retained-screen compositor of §7. A
view cut re-warps all fifteen cards (6.6 frames) *while* the new base image
streams (24 frames), so **the cut stays stream-bound, not rasterizer-bound** — the
rasterizer is free.

Two consequences to hold on to:

- **Colour index 0 is reserved** as the transparency key and cannot be used as art
  colour in card textures. In GRB332 index 0 is pure black; card art uses 0x01
  for near-black. This is a global reservation, decided once, enforced by
  `grb332.py`.
- **Background repair under a card** is: re-stream that rect from the view base in
  ROM (`OUTI`), then transparent-`LMMM` the card back on top. Both are per-slot
  rect operations, both inside the §9.2 budget.

The transparent-`LMMM`-in-GRAPHIC-7 behaviour is the single load-bearing hardware
assumption of this section, and it is **an M2 acceptance item** (§16), not
something to build 15 slots of design on unverified.

### 8.6 Tier B: the live affine textured quad

Used for the one card in motion when its pose is not enumerable.

**Geometry costs nothing.** The path keyframe quads are baked
(`gen_msx_paths.py`: 4 keyframes x 8 bytes per path; 5 hand slots x 10 field
slots = 1,600 bytes total) and the runtime lerps between adjacent keyframes with
shifts. No matrices, no projection, no perspective divide — turbor's principle
again: those values are already known offline, so we do not compute them. This is
also why the index-cached rotation trick from `notes.txt` is documented but
unused; it exists for the day someone wants a genuinely free camera.

**What is genuinely runtime:**

- **Edge walk.** Two 8.8 DDA steppers (left edge, right edge) initialised from the
  interpolated corners. Additions only, no division — the DDA error form avoids
  the slope divide entirely.
- **Per-span setup.** Given span length `L` and texture row width `W`, the step is
  `W/L`. Division is avoided with a per-width table `ustep_W[L]`, `L = 1..96`,
  2 bytes each = 192 bytes per distinct master-texture width. With two or three
  distinct widths that is under 600 bytes of ROM and **zero runtime arithmetic**.
  The same table serves `v` stepping down the quad.
- **The multiplication that survives** — corner interpolation, and the shading
  index — uses the MCM #92 identity: `a*b = f(a+b) - f(a-b)`, `f(x) = floor(x^2/4)`,
  two page-aligned 256-byte tables, **43 T-states, constant time**, operands
  clamped to -64..+63 so neither `a+b` nor `a-b` escapes the table. 512 bytes of
  resident ROM; there is no reason not to carry it.
- **Inner loop**, the same shape as Tier A but with run lengths derived from the
  8.8 accumulator rather than read from a program:

```
span:   ld   a,(de)        ;  7   texel
        out  (c),a         ; 12   -> VDP data port (0x98)
        ld   a,l           ;  4   u fraction
        add  a,ustep_lo    ;  7
        ld   l,a           ;  4
        ld   a,e           ;  4
        adc  a,ustep_hi    ;  7
        ld   e,a           ;  4   (texture row page-aligned: carry is contained)
        djnz span          ; 13
                           ; ~= 62 T/px unrolled to ~50
```

Working figure: a 56x40 card is 2,240 px at ~50 T/px ≈ 112,000 T ≈ **1.9 frames**.
One card, at a 15-20 Hz animation rate, is affordable. Two are not — which is
precisely why Tier B is scoped to a single moving card and everything else is
baked.

### 8.7 Shading

turbor gets flat shading free, because the normal's Z is already computed for
backface culling. We have no runtime normals, so the equivalent is a choice:

- a 256-byte brightness remap LUT applied per pixel during `COPY` — correct, but
  it adds ~11 T/px, a **+50 %** on the Tier A inner loop; or
- **bake three brightness variants of each master texture** and let the baked
  per-(view, slot) record name one.

Take the ROM: 3 x 240 KB = 720 KB, versus making every card 50 % more expensive
to draw forever. That is this cartridge's whole thesis in one decision.

### 8.8 Code and RAM budget

| Item | Size | Where |
|---|---|---|
| Tier A span interpreter | ~400 B | resident |
| Tier B affine rasterizer | ~1.2 KB | resident |
| MCM multiplication tables | 512 B | ROM, page-aligned |
| `ustep_W[]` tables | ~600 B | ROM |
| Edge state, u/v accumulators, current span | ~64 B | RAM |

~2 KB of the 32 KB resident code budget (§5.2) and 64 bytes of the ~12 KB RAM
budget (§3.3). Both already accounted for.

### 8.9 What the rasterizer buys back

- Card art collapses from 1,040 KB of pre-rendered size classes + 350 KB of
  pre-skewed side variants to **240 KB of master textures** (+480 KB of shading
  variants, §8.7).
- Card faces get **real perspective**, so §4.3's views can be chosen for
  composition rather than to hide an approximation.
- Card flight becomes baked span programs (~102 KB) rather than per-path cel
  strips, and any future motion has a real fallback in Tier B.

---

## 9. Platform seams

New target defines: `WAIFU_FM_MSX2`, `WAIFU_PLATFORM_NO_FRAMEBUFFER`.

`src/engine/platform.h` mostly works as-is. The MSX2 build returns:

| Seam | MSX2 answer |
|---|---|
| `waifu_platform_storage_*` | Password codes (§13), or cartridge SRAM if present |
| `waifu_platform_background_request` | 0 — background is part of the pre-rendered view |
| `waifu_platform_ui_extra_w` / `_ui_hud` | 0 / no-op |
| `waifu_platform_arena_backdrop` | 0 |
| `waifu_platform_performance_tier` | 0 (baseline) |
| `waifu_platform_glyph` | 0 — MSX uses its own VDP glyph path, not the core bitmap font |
| `waifu_platform_text_overlay*` | Hardware: 1. Text panels are composed by the MSX compositor, and `_is_hardware()` returns 1, which already tells the shared code not to recompose the framebuffer behind them |
| `waifu_platform_story_portrait` | 1 — always. Portraits are baked into the flattened dialogue composites (§14.2); the core must never attempt a portrait blit on this target |
| `WAIFU_PLATFORM_HW3D` block | not defined; the inert stubs compile away |

`waifu_platform_text_overlay_is_hardware() == 1` is a good fit and worth calling
out: it is exactly the PC-FX VDC-overlay case, and the shared code already
handles it.

**New MSX2-specific seams**, declared in `src/platform/msx2/msx2_present.h` (not
in `platform.h`, since no other target has them):

```c
void msx2_view_select(u8 view);                       /* stage + cut to a view  */
void msx2_slot_set(u8 slot, u8 card, u8 flags);       /* model update, deferred */
void msx2_text_line(u8 col, u8 row, const char *s);   /* glyph HMMM run         */
void msx2_cel_play(u8 cel_id, u8 x, u8 y);            /* localized animation    */
u8   msx2_present_idle(void);                         /* 1 when repairs drained */
```

The scene state machines call these instead of drawing pixels. Everything above
them (rules, AI, story flow) stays shared.

---

## 10. Frame loop and scheduling

### 10.1 Structure

```
main():
  init VDP GRAPHIC 7, sprites, both pages black
  install RAM ISR (music + SFX + input latch + frame counter)
  scene = TITLE
  forever:
      HALT                          ; sync to vblank
      input_read()                  ; from ISR-latched state
      scene_step()                  ; rules + model updates, NO pixels
      present_step(budget)          ; drain the repair queue within budget
      stream_step(budget)           ; advance any active stream job
      if (flip_requested && hidden_page_complete) VDP_SetPage(hidden)
```

The ISR does music, SFX and input latching only. All VRAM work is in the main
loop, so no VDP register state is ever touched from two contexts.

### 10.2 Budgeting

`present_step` and `stream_step` share a per-frame T-state allowance. Implement
the budget as a **byte/op counter**, not a timer: each queued op declares its
approximate cost (a `HMMM` ≈ 200, an `OUTI` row ≈ 800, a glyph ≈ 250), and the
loop stops when the allowance is spent. Deterministic, testable, and it degrades
by taking an extra frame rather than by tearing.

### 10.3 The AI turn

`src/game/ai.c` may take longer than one frame on a Z80. Do not let it block:
run it as a coroutine-style step machine (`ai_step()` returning
`AI_BUSY`/`AI_DONE`) so music never stutters. The existing `IB_COM_*` phases
already give the AI a multi-frame window in which the COM is visibly
"thinking" — use it.

---

## 11. Sprites

Sprite mode 2 (V9938): 32 sprites, 8 per scanline, 16x16 patterns, per-line
colour from the sprite colour table, optional OR-ing of overlapping sprites.
Sprite colours come from the **16 palette registers**, which in SCREEN 8 are
otherwise unused — so sprites can carry 16 arbitrary 9-bit-RGB colours over a
GRB332 background. That is a real, free win.

Per the brief, sprites are for simple things only:

| Element | Sprites |
|---|---|
| Board/hand selection cursor | 4 x 16x16 (corner brackets), 1 colour + 1 OR'd highlight layer |
| Attack target reticle | 4 x 16x16 |
| Menu cursor / arrow | 1–2 |
| Card "selected" glow corners | 4 |
| Text prompt blinker | 1 |
| Damage-number digits | up to 4, `VDP_SPRITE_SCALE_2` for readability |

Reserve the 8-per-line limit deliberately: the cursor brackets are horizontally
separated by design so no scanline carries more than 4. Sprite tables live at
the MSXgl GRAPHIC 7 defaults (`VDP_G7_ADDR_SPT 0xF000`, colour `0xF800`,
`VDP_G7_ADDR_SAT 0xFA00`) and are accounted for in the VRAM map (§3.2).

Moving a cursor is two SAT bytes — **this is why cursor motion is free and card
motion is not**, and the UI design should lean on it: prefer cursor/selection
feedback over animated card movement wherever the game's feel allows.

---

## 12. Audio

### 12.1 Baseline: PSG (AY-3-8910), always present

Use **Arkos Tracker 2** with MSXgl's replayer (`engine/src/arkos/`):

- **AKG** (`akg_player.asm`) — faster, larger data. Recommended for in-game.
- **AKM** (`akm_player.asm`) — smallest data, slower. Use for the long ambient
  tracks if ROM ever became an issue, which it will not.

Tracks map 1:1 onto the existing `WaifuFmMusicTrack` enum (title, opening dream,
deck editor, boss, final boss, random battle, results, lost) — 8 required tracks
plus map/plaza ambience. Composed natively in Arkos Tracker 2; the existing
mixer-based `sounds.c` synthesis is not portable and is not ported.

Replayer runs in the ISR, wrapped in the bank save/restore of §5.5.

### 12.2 SFX

**ayFX** (`engine/src/ayfx/ayfx_player.c`): a compact PSG effect bank that steals
one PSG channel from the music for the duration of an effect and restores it.
Map the existing `WaifuSoundEffect` set (select, confirm, card placed, card
destroyed, turn passed, laser shoot, direct hit, card drawn, you lost) to an ayFX
bank. ~600 T-states/frame.

This is the recommended default: it is cheap, it is proven, and it needs no
extra hardware.

### 12.3 Optional MSX-Audio (Y8950)

MSXgl has a driver (`engine/src/msx-audio.c`). When an MSX-Audio/MSX-Music
cartridge is detected at boot:

- Switch music to FM (9 channels, far richer), keeping PSG for SFX, **or**
- Better: MSX-Audio's **ADPCM channel with its own sample RAM** gives real
  sampled percussion and voice with essentially no Z80 cost after upload. If FM
  support is built at all, this is the part worth building.

Detection must be non-fatal: an absent Y8950 falls back to PSG silently. Build
both music sets offline (`gen_msx_audio.py`), select at runtime.

### 12.4 Optional 4-bit PCM "trick" channel

MSXgl has `engine/src/pcm/pcmenc.c` + `pcmplay.c` — the well-known PSG
volume-register PCM technique. Honest assessment: **it consumes most of the CPU
while playing.** At 8 kHz with 4-bit samples the replay loop is essentially a
busy-wait, which means no compositing, no streaming, and often no music
underneath.

Therefore: allowed **only** in non-interactive moments where nothing else is
running — the title logo sting, a victory voice clip on the results screen, the
final-boss intro. Never during a duel turn, never concurrent with a stream job.
Budget 4 KB/s of ROM; 5 minutes total = 1.2 MB, which we have.

The brief says this is not a hard requirement. Treat it as a stretch goal for
M8, and prefer MSX-Audio ADPCM (§12.3) if the FM path is built at all, since it
gives better audio for a fraction of the CPU.

---

## 13. Saving

**Open question, flagged early because it affects UX design:** whether the
target NEO-16 cartridge exposes battery-backed SRAM is not established by
anything in this repository. Plan for both.

**Primary (guaranteed to work): a continue-code / password system.**

The save payload the game actually needs is small. Rather than encoding the
40-card deck (40 cards x 7 bits = 35 bytes → an unusable ~56-character
password), exploit the fact that **the deck is reconstructible from progress**:
`waifu_deck_build_opponent_story()` and the story reward sequence are
deterministic. Save:

- `g_story_progress` (0–5) — 3 bits
- player name (8 chars, 5-bit alphabet) — 40 bits
- deck template id + up to 4 explicit swaps — ~28 bits
- checksum — 8 bits

≈ 79 bits → **16 characters** in a 32-symbol alphabet, entered on the existing
name-entry-style grid UI. The `pcfx-story-progress-vs-selection` distinction
(frontier vs selected foe) is preserved: the code stores the frontier.

**Secondary (if SRAM exists):** a `waifu_platform_storage_*` implementation over
the cartridge's SRAM segment, using the same blob format as the other targets.
The seam already exists; only the backend changes. Detect at boot by
write-read-back probing a scratch byte, and expose SAVE in the menu only when
the probe succeeds.

**Tertiary (development convenience):** an MSX-DOS build variant that saves to
disk, useful for testing long story runs without typing codes.

---

## 14. Screens, one by one

### 14.1 Title
SCREEN 8 full-screen pre-rendered art streamed with the display blanked (16
frames, behind a black screen at boot). Sprite-based blinking PRESS START.
Attract mode: cycle 3 pre-rendered art frames at 4 s intervals via page flip
(each 24 frames of background streaming — plenty of time inside 4 s). Menu rows
are glyph runs from the font strip; the menu cursor is a sprite.

### 14.2 Story dialogue — entirely 2D, fully pre-composited

**Decision: dialogue scenes are flat 2D images baked whole. No portrait is ever
composited at runtime.** Streaming a backdrop and then laying a large portrait
over it is the wasteful path and is dropped from the design.

**To be precise about why** — the reason is cost per beat, not capability. A
transparent portrait blit is entirely possible: §1.2's run-length skip-list path
draws a 96x160 portrait at ~70 % coverage in about 4-5 frames of budget, and the
same technique draws cards, cut-ins and effect cels everywhere else in this port.
What makes it the wrong choice *here* is the arithmetic of a dialogue beat:

- Composited at runtime, a beat costs **two streams** — the backdrop (24 frames)
  and then the portrait (4-5 frames) — plus a VRAM address re-set per transparent
  run, for a picture that then sits perfectly still for ten seconds.
- Baked, the same beat costs **one stream** and nothing else.
- The hardware alternative is not available: `LMMM`, the transparent-capable
  logical move, is VRAM→VRAM only, and the 18.4 KB offscreen budget (§3.2) is
  already committed to the font strip and the card cache (§7.3) — there is
  nowhere to park a portrait to copy *from*. The CPU→VRAM logical command `LMMC`
  is slower than a plain `OUTI` stream.
- And the composite is *never* partially reused: the backdrop is never shown
  without a character, so the second stream buys no reuse at all.

So the runtime path costs roughly 30 frames per beat to produce exactly what one
24-frame stream produces. That is the whole argument, and it is a ROM-versus-work
trade — the trade this cartridge exists to make.

What we do instead: `gen_msx_dialogue.py` bakes, offline, one complete 256x212
GRB332 image per **(scene, speaker, expression)** — backdrop, character, framing
and an *empty* text box, all flattened into a single 54,272-byte asset. At
runtime a dialogue beat is one stream and nothing else.

- **Text is the only live element.** The empty box is baked in; glyphs are
  `HMMM` runs from the offscreen font strip at ~250 T-states each, so the
  typewriter reveal is effectively free and the shared
  `story-text-typewriter-and-ending` behaviour is preserved exactly. Clearing
  the box between lines is one `HMMV` — command engine, no CPU.
- **The typewriter hides the streaming.** A line takes 1–2 seconds to type
  (60–120 frames); a composite streams in 24 budgeted frames. So while the
  current line is still typing, the *next* speaker/expression composite is
  already being streamed into the hidden page. On line advance we flip, and the
  expression change is instantaneous on screen. This is the single most
  valuable consequence of baking: it converts the expensive operation into one
  that is completely hidden by a beat the player is already waiting through.
- **No flip when nothing changed.** If the next line has the same speaker and
  expression, do not flip: `HMMV` the box on the visible page and type into it.
  Flips are reserved for actual composite changes, which keeps `page_valid`
  (§7.5) simple — each page holds one composite, and only the box region ever
  diverges.
- Same treatment for the opening dream, the plaza beats, the reward beat and the
  ending narration: all flat, all baked.

Cost: ~50 composites (5 duels x [Serena + opponent] x 3 expressions, plus
establishing shots and the non-duel beats) at 54,272 bytes = **~2.65 MB**, which
replaces the 276 KB portrait set in §5.3. That is exactly the trade this
cartridge exists to make — several megabytes of ROM to remove a runtime
compositing problem the Z80 cannot afford. `WAIFU_STORY_PORTRAIT_COUNT`,
`waifu_assets_story_portrait_pixels()` and `waifu_platform_story_portrait()` are
consequently unused on this target; the seam returns 1 (§9) so the shared code
never attempts a portrait blit.

### 14.3 Ending / credits
Optional SCREEN 7 (512x212, 16 colours) for the credits crawl: at 16 colours a
line of text is 4 bpp, halving the blit cost, and a *palette* exists again so
the crawl can fade properly. This is the one place the mode switch earns itself.
Isolate it behind `msx2_mode_credits()` and switch back to GRAPHIC 7 on exit.

### 14.4 Sanctum map / plaza
Pre-rendered backdrop per story stage (`STORY_SCENE_DESERT`, `_TEMPLE`,
`_VOLCANO`, `_VOID` — 4 backdrops), node markers as sprites, selection as a
sprite bracket. Stage changes are scene loads. The parallax/scroll of the
current 3D map is dropped; the compensating polish is better static art, which
16 MB affords.

### 14.5 Deck editor
Grid of 24x32 deck-editor icons (§4.4) — ordinary opaque 2D blits, no
rasterizer. This screen has the most
simultaneous distinct card art of any screen (a 78-card collection view), so it
**must** paginate: 5x4 = 20 icons per page at 768 B = 15,360 B per page, which
is 7 frames of budgeted streaming per page turn — acceptable with a brief wipe.
Do not attempt a smoothly scrolling collection list.

### 14.6 Duel
The core case, fully described in §4.3, §7.

### 14.7 Results / reward / loss
Pre-rendered backdrops + `SZ_BIG` (96x96) reward card art + glyph text.

---

## 15. Milestones

Each milestone ends with a **captured PNG or MKV from `openmsx-headless`**, not
a successful build. This mirrors the repository's existing verification rule
("a successful build, a live emulator process, or an all-black screenshot is not
by itself visual proof").

Verification command shape (from `README_CAPTURE.txt`):

```
MSXgl-main/openmsx-headless-.../openmsx emu build/msx2/waifu.rom 300 \
    --bios MSXgl-main/msx2.rom --direct-cart --screenshot shot.png
# or --record out.mkv --capture-fps 60
```

`--direct-cart` is the deterministic path and should be the default in
`msx2.sh`. PNG output is 512x424 RGBA.

| M | Deliverable | Acceptance |
|---|---|---|
| **M0** | Rules extraction (§2) | All existing `scripts/*.txt` scenarios produce byte-identical headless frames |
| **M1** | **Timing truth ROM.** MSXgl NEO-16 skeleton, GRAPHIC 7, measures: OUTI spacing needed during display vs blanked; actual `HMMM`/`HMMV`/`LMMM` throughput in G7; **the §8.4 span-interpreter and §8.6 affine inner loops, per pixel**; Arkos AKG ISR cost; streamable-RLE gain | A table of measured numbers replaces every estimate in §3.1, §6.1, §7.2, §8.4, §8.6. **All later budgets are re-derived from it.** |
| **M2** | VRAM map + compositor skeleton: font strip in offscreen VRAM, glyph `HMMM`, sprite cursor, page flip with `page_valid` journaling, **and a transparent-`LMMM` probe in GRAPHIC 7** | Screenshot: text + moving sprite cursor over a streamed still, both pages consistent across 10 flips. **Transparent `LMMM` confirmed to skip source colour 0 in G7** — §8.5 depends on it and must not proceed on assumption |
| **M2b** | **Rasterizer bring-up.** `msx2_raster.s` Tier A against one baked quad; `msx2_blit.s` opaque + skip-list paths | Screenshot: one card correctly warped into a board quad from its master texture, compared against the offline reference render; measured cost within the §8.4 band |
| **M3** | Asset pipeline: `grb332.py`, `gen_msx_scenes.py`, `gen_msx_rom.py`, 16 MB packer, NEO segment table | Title screen streams from segment N and matches its `--preview` PNG within a stated ΔE |
| **M4** | Title → menu → story-dialogue chain: baked 2D composites (§14.2), hidden-page prestream during the typewriter, PSG music, ayFX | 30 s MKV showing the full opening with music, and a speaker change with **no visible load** |
| **M5** | Duel presentation: `gen_msx_views.py` + `gen_msx_quads.py`, view cuts, the warped-card VRAM cache, slot model, HUD. Tier B rasterizer if any authored pose is not enumerable | Screenshot of a fully populated board in `V_TOP` and `V_HAND_P` with all 15 cards correctly warped; view cut stream-bound (rasterisation fully hidden behind the base stream) |
| **M6** | Full duel loop: rules + AI coroutine + all phases + cel animations for place/attack/destroy | Scripted run completes duel 1 with correct LP and outcome, matching a headless reference run |
| **M7** | Story mode end-to-end: 5 duels, deck editor, rewards, password save/load, ending | Full playthrough MKV; password round-trips |
| **M8** | Stretch: MSX-Audio FM/ADPCM detection, PCM stings, SCREEN 7 credits, attract mode | Runs identically with and without a Y8950 |
| **M8+** | Optional MSX2+ layer (§17): version detection, SCREEN 10/12 presentation for the named enhanced images, MSX-MUSIC music set | Same ROM boots on an MSX2 and an MSX2+; side-by-side captures show enhanced colour on the 2+ and byte-identical gameplay frames on both |
| **M9** | Hardware validation on a real MSX2 + NEO cartridge | Per repository policy, physical hardware is authoritative over emulator inference |

**M1 is the gate.** Every number in this document before M1 is an estimate
derived from first principles; M1 replaces them with measurements. If M1 shows
the streaming rate is meaningfully below 2,300 bytes/frame, the correct response
is to reduce the number of views and lean harder on sprites and cels — not to
start optimising the compositor.

---

## 16. Build integration

New files:

```
Makefile.msx2                     # wraps MSXgl's node build + asset generation
msx2.sh                           # build + openmsx-headless capture, mirrors fmtowns.sh
projects/waifu_msx2/
    project_config.js             # Target="ROM_NEO16", ROMSize=16384, Machine="2"
    msxgl_config.h
src/platform/msx2/
    msx2_main.c                   # frame loop (§10)
    msx2_present.c/.h             # compositor (§7)
    msx2_stream.s                 # OUTI streamer (§6.2)
    msx2_glyph.s                  # glyph HMMM fast path
    msx2_raster.s                 # §8: Tier A span interpreter + Tier B affine quad
    msx2_blit.s                   # §1.2: opaque OUTI blit + skip-list transparent blit
    msx2_mul.s                    # MCM #92 multiplication tables + helper
    msx2_video.c/.h               # VDP setup, page flip, sprites
    msx2_audio.c/.h               # Arkos + ayFX + optional Y8950
    msx2_input.c                  # joystick + keyboard
    msx2_storage.c                # password codec / SRAM probe
    msx2_bank.h                   # banking rules of §5.5, in one place
    STATUS.md                     # per repository convention
tools/msx2/
    grb332.py  gen_msx_views.py  gen_msx_cards.py  gen_msx_scenes.py
    gen_msx_cels.py  gen_msx_dialogue.py  gen_msx_text.py
    gen_msx_quads.py  gen_msx_paths.py
    gen_msx_audio.py  gen_msx_rom.py
src/generated/
    msx2_asset_index.h            # segment/offset table, checked in, regenerated
docs/msx2/                        # measured timings from M1, VRAM map, view catalogue
.claude/skills/msx2-architecture/SKILL.md
.claude/skills/msx2-build-verify/SKILL.md
```

`project_config.js` key settings (from `projects/template_msx2/project_config.js`
and `engine/script/js/build.js`):

```js
Machine  = "2";
Target   = "ROM_NEO16";
ROMSize  = 16384;                  // KB
LibModules = [ "system", "bios", "vdp", "print", "input", "memory",
               "arkos", "ayfx", "msx-audio" ];
```

Per `AGENTS.md`, add the two new skills to the routing list, and keep the
generated-header regeneration inside `Makefile.msx2` so the desync trap
documented in `headless-core/SKILL.md` cannot bite this target.

---

## 17. Optional MSX2+ enhancements

Everything above targets a baseline MSX2 and must keep working unchanged on one.
This section is a **strictly additive** layer: the same ROM, the same code paths,
the same timings, detected at boot and applied only to presentation.

### 17.1 What the V9958 adds that we can actually use

| Feature | Value here |
|---|---|
| **SCREEN 12** (YJK) — 19,268 colours | Photographic/painted full-screen art at *the same 1 byte per pixel*. |
| **SCREEN 10/11** (YJK + YAE) — 12,499 colours **plus** a 16-colour palette escape per pixel | Painted art **with crisp palette-coloured text and UI on top**. This is the important one. |
| **Horizontal scroll registers** R#26/R#27 (+ R#25 `SP2` two-page scroll) | Hardware parallax with zero CPU. |
| **MSX-MUSIC (YM2413) built into most MSX2+ machines** | Free FM music, no cartridge required (§12.3 gets a default target). |

**The decisive fact: SCREEN 10/11/12 are all 1 byte per pixel, exactly like
SCREEN 8.** A 256x212 image is 54,272 bytes in every one of them. So the
enhanced art costs *the same number of streamed bytes, the same number of frames,
and the same VRAM* as the baseline art. The MSX2+ layer is a pure colour-quality
win with **no runtime cost whatsoever** — the only price is ROM for a second copy
of the chosen images, which is precisely the resource this cartridge has spare.

`MSXgl-main/showpicturemode12.s` is, literally, a SCREEN 12 full-screen image
loader. It is the reference for this path (subject to the same timing audit as
everything else — §6.1).

### 17.2 YJK, and why it is only for the right screens

YJK stores luminance **Y per pixel** but chroma **J and K shared across each
group of 4 horizontally adjacent pixels**. Chroma is 4:1 horizontally
subsampled. Consequences:

- **Excellent** for painted/photographic art with soft colour transitions:
  portraits, skies, backdrops, logos with gradients.
- **Poor** for hard-edged saturated graphics: coloured 8x8 text, thin UI rules,
  1-pixel card borders — a colour edge bleeds across its 4-pixel group.

SCREEN 10/11 (YJK + YAE) is the fix: bit 3 of a pixel byte escapes that pixel out
of YJK into one of 16 palette colours. So a screen can be painted art *and*
carry perfectly sharp, arbitrarily coloured text over it.

**Rule: any enhanced screen that shows text uses SCREEN 10. Any enhanced screen
that is pure art uses SCREEN 12.**

### 17.3 The enhanced set (tier b)

Only named, static, full-screen images are enhanced. Nothing in the duel is.

| Screen | Mode | Why |
|---|---|---|
| Title art | SCREEN 10 | painted art + crisp menu text and PRESS START |
| Attract-mode art frames (3) | SCREEN 12 | pure art, no text |
| Ending illustration | SCREEN 12 | pure art |
| Ending narration / credits | SCREEN 10 | text over art, and YAE's palette makes a proper fade possible again (§0.2) |
| Dialogue composites (§14.2), the ~14 most-used | SCREEN 10 | painted character art + sharp baked text box |
| Story-stage establishing shots (4) | SCREEN 12 | pure art |
| Results / reward backdrop | SCREEN 10 | |

~24 images x 54,272 = **1,272 KB**, already carried in the §5.3 budget.

Enhancing *all* ~50 dialogue composites instead of 14 would cost a further
~2 MB, which the 4.4 MB slack can absorb. That is an art-budget call, not an
engineering one.

### 17.4 A genuine bonus: fades come back

Per §0.2, SCREEN 8 has no palette, so the baseline port fakes every fade. On
SCREEN 10/11 the 16 YAE palette entries **are** programmable, so any element
drawn through the palette escape — text, UI, rules, logo — can be faded and
colour-cycled in hardware, for free, by writing palette registers. The YJK art
underneath still cannot fade, so the pattern is: **fade the text and UI in
hardware, wipe the art with `HMMV` (§7.4).** Applied to the title and the
credits, that alone closes most of the perceived polish gap.

### 17.5 Hardware-scrolled map parallax (tier c)

R#26/R#27 scroll the display horizontally with no CPU cost. On the sanctum map
(§14.4) this restores some of the motion the baseline port gives up: a scrolling
sky or dune band behind static node markers. Requires the scrolled layer to be
authored as a wrap-around strip and `SP2` (R#25) set for clean two-page
scrolling. Isolated to one screen; skip it if tier (c) is not taken.

### 17.6 MSX-MUSIC (tier c)

Most MSX2+ machines ship with a YM2413. MSXgl has `engine/src/msx-music.c`. This
makes §12.3's FM path free on MSX2+ rather than dependent on an add-on
cartridge, which changes its cost/benefit substantially: if tier (c) is taken,
FM becomes the default MSX2+ music and PSG the MSX2 fallback. Note this is
YM2413 (9 channels, fixed instrument ROM), **not** the Y8950 of §12.3 — it has
no ADPCM, so sampled voice remains MSX-Audio-only.

### 17.7 Implementation

Detection: the MSX version byte at BIOS `0x002D` (0 = MSX1, 1 = MSX2, 2 = MSX2+,
3 = turbo R). MSXgl exposes it; the bundled openMSX README references the same
byte. Set `g_msx2p = (version >= 2)` once at boot.

```c
/* msx2_video.c */
u8 msx2_present_image(u8 image_id);   /* picks the variant + the mode for it */
```

`msx2_asset_index.h` gains, per enhanced image, an optional
`{seg, off, mode}` variant record. `msx2_present_image()` selects the enhanced
variant when `g_msx2p` and the record exists, sets the corresponding VDP mode,
and streams. Everything else — the streamer (§6.2), the budget (§10.2), the page
flip (§7.5) — is byte-for-byte the same code, because the byte count is the same.

**Non-negotiable constraints:**

1. **Presentation only.** No gameplay, timing, RNG or rules difference between
   MSX2 and MSX2+. A scripted run must produce identical *game state* on both;
   only pixels differ.
2. **Never in the duel.** The duel stays in GRAPHIC 7 permanently. Mode switches
   happen only on full-screen scene boundaries where the screen is already being
   replaced, so there is no visible artefact and no VRAM-layout hazard mid-scene.
3. **Sprite tables move with the mode.** SCREEN 10/11/12 have their own table
   addresses; `msx2_present_image()` must re-assert R#5/R#11/R#6 on every switch
   and hide all sprites across the transition.
4. **Return to GRAPHIC 7 on exit**, always, through one function — never
   ad hoc at call sites. Same discipline as `msx2_mode_credits()` (§14.3).
5. **Verified on both.** Every M8+ capture is taken twice, once on an MSX2
   machine profile and once on an MSX2+, from the same ROM. Note that the
   bundled openMSX's headless renderer states reduced YJK fidelity — so MSX2+
   colour output specifically must be confirmed on real hardware at M9, per the
   repository's hardware-is-authoritative rule.

### 17.8 turbo R

Out of scope. A turbo R runs this ROM as an MSX2+ and gets the §17 enhancements;
no R800 path is planned. Worth noting only so it is a decision rather than an
oversight.

---

## 18. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| **`showpicturemode12.s` timing does not transfer to GRAPHIC 7 during display** | Dropped bytes, corrupted images, intermittent | M1 measures it first; `_fast`/`_display` streamer variants (§6.2) |
| **32 KB resident code ceiling** | Late, painful restructuring | Track resident size in CI from M2; banked-call discipline from day one (§5.2) |
| **12 KB RAM ceiling** | Same | Budget table maintained in `STATUS.md`; Phase-2 page-0 RAM as contingency (§3.3) |
| **Bank/ISR race** (§5.5) | Silent, rare corruption; the classic MegaROM bug | Save/restore rule + `MSX2_DEBUG_BANK` magic-value assertion |
| **Page-flip staleness** | Flicker, half-drawn boards | `page_valid` journaling (§7.5); test explicitly in M2 |
| **No palette → fades look bad** | Perceived polish regression | Decided up front (§0.2, §7.4); art direction leans on dithered wipes and cuts |
| **Transparent `LMMM` does not skip colour 0 in GRAPHIC 7** | §8.5 collapses; every card redraw becomes a CPU blit instead of a free command | Probed at M2 before anything is built on it. Fallback: redraw the card's rect from the view base and re-run its Tier A span program (~0.44 frame), which caps simultaneous card redraws at ~2/frame and pushes more work behind wipes |
| **Rasterizer inner loop slower than the §8.4 band** | View cuts stop being stream-bound; card flight drops below 15 Hz | Measured at M1 before content work; levers in order: re-warp only the cards that actually changed, smaller master textures, more baked poses, fewer views |
| **Master-texture magnification looks soft** | Cards read blurry against crisp pre-rendered board art | 48x64 masters are larger than every destination quad, so the common case is minification; shading variants are baked from the full-resolution source (§8.7) |
| **`gen_msx_*` desync from `card_data.txt`** | Silently wrong art in a 16 MB ROM | Checksum baked into `msx2_asset_index.h`, checked by the Makefile (§4.1) |
| **PCM trick channel starves the frame** | Audio/graphics stutter | Confined to non-interactive moments (§12.4); prefer MSX-Audio ADPCM |
| **NEO cartridge SRAM availability unknown** | Save design churn | Password system is the primary path and needs no hardware (§13) |
| **openmsx-headless renderer is not full-fidelity** (its README states reduced sprite/raster/YJK accuracy) | Emulator-only visual bugs | Sprite-heavy screens verified on real hardware at M9; keep sprite use simple, as the brief already requires |
| **MSX2+ mode switch corrupts sprite tables / VRAM layout** | Glitched screens after a scene change | One switch function, re-asserts R#5/R#11/R#6, sprites hidden across the transition; switches only at scene boundaries (§17.7) |
| **YJK chroma subsampling smears UI** | Enhanced screens look worse than baseline | SCREEN 10 (YAE palette escape) for anything with text; SCREEN 12 only for pure art (§17.2) |
| **MSX2+ layer drifts into gameplay** | Two divergent games to test | Constraint 1 of §17.7, enforced by an M8+ acceptance test comparing game state, not pixels |

---

## 19. Open questions for the owner

1. **Cartridge SRAM.** Does the intended NEO-16 hardware have battery-backed
   save? Answer decides whether §13's password system is the shipping save
   method or a fallback.
2. **PAL/NTSC.** 50 Hz gives 71,600 T-states/frame (+20 % CPU) but 50 Hz
   animation pacing. The game core is written for 60 Hz
   (`WAIFU_FM_FPS 60`, `waifu_fm_set_frame_vblanks()`). Recommend building
   60 Hz-first and using the existing vblank-count seam for 50 Hz.
3. **Minimum machine.** 128 KB VRAM is required (SCREEN 8 double buffering). Is
   a 64 KB-VRAM MSX2 in scope? If so, page flipping is impossible and the whole
   §7.5 design changes — recommend requiring 128 KB and refusing to boot
   otherwise with a clear message.
4. **Scope of the 3D feel.** How much does the duel's camera motion matter to
   the identity of the game? §4.5 replaces every camera move with a cut. If
   smooth motion is essential to one specific beat (e.g. the final boss's
   opening), that beat can get a bespoke small-window cel animation — but it
   must be named now, because it is ROM and authoring work, not code.
5. **MSX-Audio.** Build the FM/ADPCM path at all? It is roughly a milestone of
   work (§12.3) and doubles the music authoring effort.
6. **MSX2+ scope.** §17 is written as strictly optional and additive. Which of
   the three tiers do you want: (a) none, (b) enhanced title/ending/dialogue art
   only, (c) that plus MSX-MUSIC FM and hardware-scrolled map parallax? Tier (b)
   is roughly one milestone and ~1.3 MB of ROM; tier (c) adds a second music
   authoring pass. Decide before M3, because the asset packer's segment table
   has to reserve the variant slots.

---

## 20. References

**3D rasterisation on MSX (the basis of §8)**

- `MSX_docs/RT3D/` — turbor's (David Heremans) work-in-progress disk from
  *SandStone*: `readme.txt`, `notes.txt`, the Compass-format `deel2.asm`, the
  `prepros.pl` model preprocessor and the `.txt`/`.dat` figure data. Note
  `readme.txt`'s warning that the routines are incomplete and will lock up on
  exit. The `.asm` files are Compass-tokenised, not plain text; the Compass
  sources needed to read them are at
  <https://github.com/turbor/compass-1.2-sources/releases>.
- `MSX_docs/RT3D/notes.txt` — the index-cached transform trick (vertices store
  indices into per-axis coordinate arrays, so each rotation-matrix element is
  multiplied once per distinct value, then every vertex is lookups and adds), and
  the governing principle: *"try to do each calculation only once, and if you do
  not need a value, don't calculate it."*
- turbor, *3D rasterisation*, MSX Resource Center forum —
  <https://www.msx.org/forum/msx-talk/development/3d-rasterisation?page=1#comment-317636>
  Backface culling from the normal's Z component alone, flat shading from that
  same value, Bresenham edge pairs driving the VDP LINE command (reloading only
  the registers that changed), **texture-mapped triangles as horizontal spans
  with `OUT` colour codes in SCREEN 8 and a precomputed linear offset list**, VDP
  block-fill erase from the previous frame's min/max box overlapped with CPU
  normal calculation, and bucket-sorted painter's ordering.
- Grauw, same thread — span-buffer versus scanline rasterisation, and the
  calibration figures quoted in §6.1 (18 cycles/byte for an MSX2 VRAM fill;
  247 ms for 256x192; R800 waits 54 cycles/byte).

**Z80 arithmetic**

- *Multiplication*, MSX Computer Magazine #92 —
  <https://www.msxcomputermagazine.nl/mccw/92/Multiplication/en.html>
  `a*b = f(a+b) - f(a-b)` with `f(x) = floor(x^2/4)`; two page-aligned 256-byte
  tables (low and high bytes); 43 T-states, constant time; operands must lie in
  -64..+63 so neither the sum nor the difference leaves the table.

**Toolchain and hardware**

- `MSXgl-main/` — engine and build system: `engine/src/rom_mapper.h` (NEO-8 /
  NEO-16 bank addresses and the `g_Bank0Segment[]` shadow),
  `engine/src/crt0/crt0_rom_neo.asm`, `engine/src/vdp.h` (command engine,
  GRAPHIC 7 sprite table addresses), `engine/src/arkos/`, `engine/src/ayfx/`,
  `engine/src/msx-audio.c`, `engine/src/msx-music.c`, `engine/src/pcm/`,
  `projects/samples/s_neomap.c`, `projects/template_msx2/project_config.js`.
- `MSXgl-main/showpicturemode12.s` and `example_show_banks.txt` — the unrolled
  `OUTI` ROM→VRAM streaming technique and the bank-switch pattern §6.2 is built
  on (written for MSX2+ SCREEN 12; see the §6.1 timing caveat).
- `MSXgl-main/openmsx-headless-.../` — `README_CAPTURE.txt`,
  `NEO_MAPPER_1_2.txt`, `PLANAR_PAGE_FIX.txt`: NEO 1.2 14-bit segment registers,
  GRAPHIC 6/7 planar CPU-port addressing
  (`physical = ((logical << 16) | (logical >> 1)) & 0x1FFFF`), HMMV on SCREEN 8,
  and the documented fidelity limits of the headless renderer.

**This repository**

- `src/generated/fmtowns_turn_card_geometry.h` and
  `tools/fmtowns/gen_turn_card_cache.py` — the existing per-pose, per-slot quad
  bake that `gen_msx_quads.py` extends (§4.1, §8.4).
- `.claude/skills/headless-core/SKILL.md` — the headless harness used as this
  port's offline renderer, and the regenerate-or-desync rules the MSX asset
  pipeline must follow.
