# MSX2 final-issues implementation plan

Source issue list: `MSX2_issues_last.txt`

Design authority: `MSX2_PORT_PLAN.md`

Current implementation/status: `src/msx2/STATUS.md`

Prepared against the repository state of 2026-09-04.

## 1. Objective

Close every item in `MSX2_issues_last.txt` without weakening the MSX2 port's
existing guarantees:

- the MSX2 target remains a fork contained in `src/msx2/` and `tools/msx2/`;
- the shipping ROM remains playable on a baseline MSX2 with a V9938 and 128 KB
  VRAM;
- the two GRAPHIC 7 pages never expose a partially composed frame;
- code that executes while the NEO page-2 window contains data is linked below
  `0x8000`;
- page-0 code-bank calls continue to go through `msx2_bank.c` trampolines;
- story continue codes remain reproducible;
- soak and story-soak builds remain deterministic;
- generated headers and blobs are regenerated through `Makefile.msx2`, never
  edited by hand;
- verification uses real openMSX, not MSXgl's bundled `openmsx-headless` Z80.

The issue report is treated as authoritative observed behaviour even where the
current source contains comments describing an earlier attempted fix. Those
comments are useful evidence, but an issue is closed only by a reproducible test
on the current shipping build.

## 2. Current baseline and hard constraints

`./msx2.sh ram` currently reports:

| Area | Current use | Limit/headroom relevant to this work |
|---|---:|---:|
| Fixed `_CODE` | 30,681 bytes | 32 KB total; only 16 KB is below `0x8000` |
| Page-0 duel bank, segment 2 | 12,297 bytes | 4,087 bytes free |
| Page-0 modal bank, segment 3 | 1,684 bytes | 14,700 bytes free |
| Page-0 story bank, segment 4 | 9,345 bytes | 7,039 bytes free |
| Static RAM | 7,250 bytes | 5,934 bytes to `HIMEM`, including stack headroom |

Music state already reserves 700 bytes of static RAM in `msx2_audio.c`, so the
audio implementation should replace that reservation rather than add a second
working area. The larger risk is fixed-code placement: `Msx2_AudioTick()` is
currently linked at `0x8F36`. A decoder cannot map music data into the
`0x8000-0xBFFF` window while its own executing code is in that window.

The seven supplied tracks are AY-3-8910 VGM 1.51 files:

| File | Size | Intended use |
|---|---:|---|
| `titlescreen.vgm` | 63 KB | title |
| `Overworld.vgm` | 5.9 KB | opening, story road/map, deck editor |
| `Battle.vgm` | 48 KB | ordinary/free duel |
| `Boss.vgm` | 33 KB | story boss duels |
| `FinalBoss.vgm` | 38 KB | final story duel |
| `Victory.vgm` | 1.2 KB | win/reward/ending result |
| `Fail.vgm` | 2.0 KB | loss |

This is a good fit for MSXgl's PSG-only lVGM path. Several tracks cross 16 KB
boundaries, so segment continuation is mandatory rather than optional.

## 3. Execution order

The work should be integrated in this order:

```text
diagnostic hooks and deterministic fixtures
        |
        +--> hand/deal fix ---------+
        +--> duplicate-view fix ----+--> visual regression pass
        +--> sprite transition fix -+
        +--> text cleanup ----------+
        +--> save-flow fix ---------+
        |
        +--> entropy implementation --> deterministic/non-deterministic tests
        |
        +--> VGM conversion/packing --> resident lVGM playback --> audio tests
                                                |
                                                +--> full build/budget/soaks
```

The diagnostic layer comes first because the two most important rendering bugs
are transient and can disappear from a final screenshot. Audio lands after the
visual fixes because it changes interrupt cost and page-2 ownership, which can
otherwise make a graphics regression look unrelated.

## 4. Phase 0: make transient failures observable

### 4.1 Add a test-only MSX2 regression build

Add `MSX2_REGRESSION=1` plumbing to `Makefile.msx2` and
`src/msx2/project_config.js`, producing `-DMSX2_DEBUG_REGRESSION`. It must not be
enabled in the shipping, soak, or story-soak variants.

The regression build should support two mechanisms:

1. A fixed seed supplied at build time, for example `MSX2_TEST_SEED=0x1234`, so
   an input script always sees the same hands and AI decisions.
2. Small named start fixtures, selected by a numeric define, which enter a
   precise board/story state without replaying several minutes of unrelated
   UI. Fixtures must call the same public state-transition functions as the
   game after constructing their initial state; they must not introduce a
   second implementation of the behaviour being tested.

Suggested fixture IDs:

```c
enum Msx2RegressionFixture {
    MSX2_FIXTURE_NONE = 0,
    MSX2_FIXTURE_COM_TURN4_HAND,
    MSX2_FIXTURE_PLAYER_TOP_PLACE,
    MSX2_FIXTURE_TOP_PASS_TURN,
    MSX2_FIXTURE_STORY_SAVE_ROW,
    MSX2_FIXTURE_DUEL_RESULT_WIN,
    MSX2_FIXTURE_DUEL_RESULT_LOSE,
};
```

Keep fixture construction in a new `src/msx2/msx2_regression.c/.h`, compiled
only for the regression variant. If it needs to manipulate duel-bank internals,
expose one test-only entry point through `msx2_bank.c`; do not make banked
statics globally writable.

### 4.2 Extend the probe only in the regression build

The normal `Msx2Probe` is deliberately small. Under
`MSX2_DEBUG_REGRESSION`, append a versioned diagnostic block and bump the wire
version used by `tools/msx2/read_probe.py`. Record at least:

- board mode and view;
- shown and draw page;
- `g_deal_slot`, `g_deal_step`, `g_deal_reveal`;
- five-bit wanted/shown hand occupancy for both pages;
- `g_hand_left`, `g_hand_hidden`, and whether a camera move/stream started;
- gem sprite visible/hidden and its last X/Y;
- counters for full-view streams, band streams, display blank/unblank pairs,
  and page flips;
- story phase and selected map/save row;
- current music track, music segment, decoder pointer offset, decoder frame
  count, loop count, and decoder errors;
- initial seed, most recent duel seed, and entropy-source flags.

Banked board/story code should publish these values to a small resident debug
structure at the points where state changes. The resident probe then copies the
structure without calling into a switched page-0 bank.

### 4.3 Capture sequences, not just final frames

Extend `msx2.sh` with a `trace` or `frames` command backed by an openMSX Tcl
script that dumps VRAM and the sprite attribute table at specified emulated
times. Decode each dump through `tools/msx2/vram_png.py`. A manifest should
associate every image with emulated time, visible page, board mode/view, and
stream counters from the RAM probe.

Add `tools/msx2/compare_sequence.py` for three simple assertions:

- a rectangular region remains unchanged between selected frames;
- a sprite ID is hidden throughout a time interval;
- no frame is all black unless the test explicitly permits a transition.

This is preferable to judging only an MKV: it makes a one-frame gem leak or
black flash a machine-checkable failure.

## 5. Issue A: opponent turn-four hand appears as three cards, then five

### 5.1 Relevant code

- `src/msx2/msx2_board.c`
  - `Msx2_BoardClearHandBand()`
  - `Msx2_BoardStepCameraMove()`
  - `Msx2_BoardStepDeal()`
  - `Msx2_BoardSnapshot()`
  - `Msx2_BoardPaint()`
- `src/msx2/msx2_duel.c`
  - turn-start draws and hand compaction/refill
- `src/msx2/msx2_probe.c/.h`
- `tools/msx2/read_probe.py`

### 5.2 Working diagnosis

The rules state and presentation state must first be separated. The existing
probe already reports all five logical hand cards. The defect can therefore be
classified in one run:

- if the probe reports only three logical cards, the bug is in turn-start draw
  or hand-slot compaction in `msx2_duel.c`;
- if the probe reports five while one page shows three, the bug is in deal
  reveal/page bookkeeping in `msx2_board.c`.

The second is more likely from the symptom and current code. A chair hand is
represented simultaneously by:

- logical slots in `g_duel.side[owner].hand[]`;
- reveal frontier `g_deal_reveal`;
- the current flight (`g_deal_slot`, `g_deal_step`, `g_deal_px[page]`);
- wanted slots `g_want[]`;
- two independent copies of `g_shown[page][]`;
- the bulk-hand dirty mask `g_hand_left`.

`g_deal_reveal` is a positional frontier, not an occupancy mask. If a later
turn has holes, or if a page is marked current before all five destinations are
settled, `i < g_deal_reveal` can describe a different picture from the one that
was actually drawn. The existing landing code draws a completed card onto both
pages, but the final cleanup and ordinary one-card painter can still disagree
about which slots are owed.

### 5.3 Code change

After the trace identifies the exact mismatch, make the deal state explicit:

```c
static u8 g_deal_target_mask;       /* occupied hand slots this deal must show */
static u8 g_deal_landed_mask[2];    /* destinations complete on each page */
```

At `Msx2_BoardClearHandBand()`:

1. finish the rules turn-start draw before taking the snapshot;
2. derive `g_deal_target_mask` once from the incoming owner's five logical
   hand slots;
3. clear both landed masks and all per-page flight positions;
4. mark every hand slot consistently empty in both `g_shown` pages;
5. start at the first set bit, not blindly at slot zero.

At each landing in `Msx2_BoardStepDeal()`:

1. erase the old flight only from the hidden page;
2. draw the settled destination on both pages;
3. set that slot in both landed masks only after the VDP writes complete;
4. update `g_shown` for exactly that destination;
5. advance to the next set bit.

End the deal only when both expressions are true:

```c
g_deal_landed_mask[0] == g_deal_target_mask &&
g_deal_landed_mask[1] == g_deal_target_mask
```

Then perform one final per-page erase of any flight residue, verify no hand
slot remains dirty, and enter `M_COM`/`M_IDLE`. Keep `g_deal_reveal` only if it
is still useful for animation order; `Msx2_BoardSnapshot()` should use an
explicit visible mask for hand slots rather than infer visibility from a
frontier.

If the trace instead shows only three logical cards, correct the turn-start
rules in `msx2_duel.c` and retain the masks as regression instrumentation only.
Do not make the board invent cards that the rules do not own.

### 5.4 Tests

1. Fixture `MSX2_FIXTURE_COM_TURN4_HAND` begins immediately before the COM
   turn-start step with a known partially used hand and enough deck cards.
2. Assert that the logical hand reaches the intended occupancy before the
   camera handoff begins.
3. Capture every page flip during the deal. On every captured frame, all
   previously landed slots must remain visible; visible occupancy may increase
   but never decrease.
4. At deal completion, both page masks must equal the target mask and the two
   decoded hand bands must be byte-identical.
5. Run a deterministic multi-turn script through at least turns 1-8 to catch
   the reported turn-four case and later sparse-hand cases.
6. Run ordinary soak for 300 seconds to ensure the new presentation state does
   not stall rules progress.

## 6. Issue B: placing from the hand redraws the overhead view twice

### 6.1 Relevant code

- `Msx2_BoardHandVisible()`
- `Msx2_BoardCutTo()`
- `Msx2_BoardPrepareLanding()`
- `Msx2_BoardStepBend()`
- `Msx2_BoardRunFx()` cleanup/hold path
- all calls to `Msx2_StreamSceneBlanked()` in `msx2_board.c`

### 6.2 Working diagnosis

A hand monster currently causes two conceptually different transitions:

1. selecting it moves the cursor from the chair hand to the field and the
   generic end-of-frame code cuts to `MSX2_VIEW_OVER`;
2. confirming placement starts the landing effect, which blanks the display
   while removing the hand, caching the flight card, and priming both pages.

There are also later page-level restores/levelling operations during bend and
hold. The issue report describes these as two overhead redraws separated by a
black frame. The trace counters must determine whether the second event is an
actual full `MSX2_VIEW_SEGMENT` stream or an unnecessary display blank around
card-cache work. The fix differs:

- duplicate full stream: remove the second view transition;
- no full stream, only blanking: compose flight cache/first pose into hidden
  VRAM without disabling the displayed page, or hide the cost behind the first
  already-required cut.

### 6.3 Code change

Introduce an explicit view-transition result instead of allowing several
helpers to infer that the view is stale:

```c
typedef enum Msx2ViewEnsureResult {
    MSX2_VIEW_ALREADY_READY,
    MSX2_VIEW_COMPOSED_NOW,
} Msx2ViewEnsureResult;

static Msx2ViewEnsureResult Msx2_BoardEnsureView(u8 view);
```

`Msx2_BoardEnsureView()` owns the only path that may full-stream a resting
view. It records `g_view`, raster view, per-page base validity, and a generation
counter. Landing code receives/reads that generation and must never stream the
same base again when both pages already hold it.

For the expected overhead-placement path:

- cut to overhead once when the player commits to choosing a field slot;
- keep both overhead pages valid;
- make `Msx2_BoardPrepareLanding()` modify only the flight/card rectangles and
  panel regions;
- do not call `VDP_EnableDisplay(FALSE)` merely for four small cache blits if
  they can be issued to hidden/offscreen VRAM with command completion waits;
- if a temporary blank is still required for VDP data-port timing, fold the
  card-cache load into the original overhead cut so the player sees one
  transition, not two;
- keep bend cleanup rectangle-local, as intended by the existing backing-store
  design.

Add assertions in regression builds:

```c
MSX2_ASSERT(g_view == MSX2_VIEW_OVER);
MSX2_ASSERT(g_view_generation == g_fx_base_generation);
MSX2_ASSERT(g_full_view_streams == streams_at_placement_start);
```

### 6.4 Tests

- Start in the player chair with a known monster in hand.
- Press A to choose it, then A to place it.
- Count full-view streams from first press through settled landing: exactly one
  transition to overhead, zero further overhead-base streams.
- Allow no all-black captured frame after the overhead view first becomes
  visible.
- Compare a board-only mask before/after every landing frame. Outside the
  flight bounding rectangle, destination slot, and info panel, pixels must not
  change.
- Repeat for attack and defence placement, all five hand slots, all five field
  slots, and placement initiated while already overhead.

## 7. Issue C: selector gem survives briefly at old coordinates

### 7.1 Confirmed cause

`Msx2_StreamScene()` clears sprites for full scene changes, but duel camera
paths use `Msx2_StreamSceneBlanked()` directly. When passing from overhead,
`Msx2_BoardSwitchView()` first calls `Msx2_BoardCutTo(BOARD_VIEW_PLAYER)`.
Neither helper hides the gem before re-enabling the display. The next call to
`Msx2_BoardShowCursor()` sees `M_TURN` and hides it, leaving a one-frame window
in which the new bitmap is displayed under the old sprite X/Y.

### 7.2 Code change

Create one transition barrier in `msx2_sprite.c/.h`:

```c
void Msx2_SpriteTransitionBegin(void)
{
    Msx2_SpriteHideGem();
    Msx2_SpriteSlashHide();
    Msx2_SpriteBurnHide();
    VDP_CommandWait();
}
```

Call it before any operation that changes the coordinate space or replaces the
underlying screen:

- `Msx2_BoardCutTo()` before display blanking;
- `Msx2_BoardSwitchView()` before an overhead-to-chair normalization cut;
- `Msx2_BoardStepCameraMove()` before its first pose if not already hidden;
- board result/cut-in entry and exit;
- title/story/modal scene entry where appropriate.

Do not clear all 32 sprite entries every frame. Hide the active sprite groups
once at the transition boundary, then let the destination scene explicitly
show what it owns. Add a sprite-owner/debug enum so a stale owner is detectable:

```c
enum Msx2SpriteOwner { SPR_OWNER_NONE, SPR_OWNER_BOARD, SPR_OWNER_MODAL,
                       SPR_OWNER_BATTLE, SPR_OWNER_RESULT };
```

### 7.3 Tests

- Fixture `MSX2_FIXTURE_TOP_PASS_TURN` places the gem at a distinctive field
  coordinate, then passes.
- Dump the SAT every frame from the pass input through the first COM-chair
  deal frame. Gem planes must hold hidden Y before the first new bitmap/page is
  exposed and remain hidden until the destination scene requests a cursor.
- Repeat top-to-player, player-to-COM, COM-to-player, board-to-card-check,
  board-to-battle-cut-in, duel-result-to-story, and story-to-title.

## 8. Issue D: remove redundant text and its visible clearing

### 8.1 Requested removals

Remove these bitmap-panel messages:

- `SUMMON` during the player's ordinary placement flight;
- `YOU WIN THE DUEL`;
- `YOU HAVE LOST` (the current loss-side counterpart to the issue's wording);
- `SPACE RETURNS TO THE TITLE`.

Keep the large sprite result word `YOU WIN`/`YOU LOSE` unless the owner asks to
remove the result presentation entirely. It is a separate, intentional overlay
and is not cleared through the bitmap panel.

### 8.2 Code change

In `msx2_board.c`:

- `Msx2_BoardFxDraw()`: for `FX_SUMMON`, do not call
  `Msx2_BoardFxBanner()`; retain the COM placement message unless separately
  requested, since it communicates hidden opponent action;
- `Msx2_BoardCardLines()`: when `g_mode == M_OVER`, leave the line empty rather
  than drawing win/loss prose;
- `Msx2_BoardPrompt()`: return an empty string or `NULL` for `M_OVER`;
- `Msx2_BoardPromptLine()`: clear/redraw only if that region actually changed.

Prefer a semantic result over magic empty strings:

```c
static bool Msx2_BoardPrompt(const c8** out);
```

Then remove the unused IDs from `src/msx2/msx2_ui_strings.def` and regenerate
`src/generated/msx2_scenes.h` plus the text blob via `Makefile.msx2`. Removing
entries changes generated numeric IDs, so no generated file may be edited in
isolation.

The important performance rule is that removing text also removes its dirty
event. Do not continue clearing and repainting the panel with no glyphs; that
would preserve the visible flash the request is trying to eliminate.

### 8.3 Tests

- Search the generated and handwritten sources for all removed strings.
- Capture an ordinary summon and assert the info-panel bitmap does not change
  solely for a summon banner.
- Capture win and loss results and assert the bitmap panel remains stable while
  the sprite word enters/holds.
- Confirm A/B still dismiss a settled result and that free battle returns to
  title while story battle returns to reward/map.

## 9. Issue E: story SAVE returns to title instead of opening save choice

### 9.1 Relevant path

The intended call chain already exists:

```text
PH_MAP / MAP_CODE_ROW
  -> Msx2_StoryEnterSavePick()
  -> PH_SAVE_PICK
  -> floppy/password choice
  -> Msx2_StoryEnterCodeOutput()
  -> PH_CODE_OUT
```

The observed jump to title means either the input is also being interpreted as
quit, the page-0 story bank is not restored after disk probing, or a phase/state
value is corrupted. The plan must prove which one before changing UX code.

### 9.2 Instrument first

For each story step in the regression build, record:

- phase before/after;
- input pressed mask and typed character;
- map cursor and save picker cursor;
- return value from `Msx2_StoryStep_In()`;
- page-0 bank before entering story, before/after `Msx2_DiskPresent()`, and on
  trampoline return;
- `g_want_quit`;
- scene selected by `msx2_main.c`.

Assertions:

```c
MSX2_ASSERT(Msx2_Bank0Current() == MSX2_BANK0_STORY);
MSX2_ASSERT(g_phase == PH_SAVE_PICK);
MSX2_ASSERT(!g_want_quit);
```

Add `Msx2_Bank0Current()` as a read-only diagnostic/public invariant helper;
do not expose the mapper shadow for arbitrary writes.

### 9.3 Likely fixes by result

If the confirm edge is also seen as B/quit:

- consume the entry press at the phase boundary;
- add `Msx2_InputConsume(mask)` or a one-frame `g_phase_armed` latch so the
  destination phase ignores the button that opened it;
- keep this generic enough for load/save/modal screens, which share the risk.

If page 0 is restored incorrectly after disk probing:

- make the page-0 restore explicit around every BIOS/disk call;
- preserve both the PPI primary-slot value and `msx2_bank.c`'s NEO segment
  shadow;
- return to the story bank before executing any story-bank instruction;
- keep `msx2_disk.c` in fixed `_CODE`.

If state is correct but the title artwork merely appears:

- inspect the stream segment passed by `Msx2_StoryEnterSavePick()`;
- retain the current intended map backdrop, not `MSX2_SCENE_TITLE_SEGMENT`;
- clear stale title sprites before showing the picker;
- ensure both pages receive the picker and `g_map_dirty` is cleared only after
  both are valid.

The password choice must always be live. FLOPPY DISK is live only when
`Msx2_DiskPresent()` succeeds; selecting an unavailable disk reports
`NO DRIVE ANSWERED` and stays in the picker. After either a successful floppy
write or choosing password, show the 16-character code as the paper backup.

### 9.4 Tests

Targeted tests:

1. `MSX2_FIXTURE_STORY_SAVE_ROW` + A must enter `PH_SAVE_PICK`, never return
   `MSX2_STORY_QUIT`.
2. C-BIOS/no-drive: default cursor is PASSWORD, both rows render, and A shows a
   non-empty 16-character code.
3. Force cursor to unavailable FLOPPY DISK: A leaves the picker visible with
   `NO DRIVE ANSWERED`; it does not return to title.
4. B from save picker returns to the story map; B from code output also returns
   to the map.
5. Decode the emitted password and compare progress, name, and all encoded deck
   overrides.

Shipping-path test:

- boot the unmodified shipping variant;
- enter STORY MODE through the real title menu;
- advance the opening through real key-matrix input;
- move to SAVE GAME and select it;
- capture the map, picker, and password screen in sequence.

Disk test:

- configure a real openMSX machine with a disk ROM and a disposable formatted
  720 KB image;
- save, power-cycle, load through the title's LOAD STORY flow, and compare the
  restored story state;
- finally repeat on physical hardware. Emulator success does not replace this
  hardware check because `STATUS.md` explicitly marks DSKIO unverified.

Never run the disk test against a valuable image: the current implementation
uses the last logical sector as its save sector.

## 10. Issue F: implement music playback with MSXgl lVGM

### 10.1 Selected design

Use MSXgl's PSG-only `vgm/lvgm_player` at 60 Hz. All supplied files identify as
AY-3-8910 VGM, so disable unused OPLL, Y8950, SCC, SCC+, second PSG, OPL4, and
notify features except the segment-boundary callback. This reduces code size
and avoids probing optional audio hardware.

Do not link raw VGM. Convert each source to lVGM with MSXgl's `MSXzip` exporter,
using duplicate-write simplification and 16 KB splitting. The split output must
contain `LVGM_OP_NOTIFY, LVGM_NOTIFY_SEG_END` at segment boundaries and be
padded so every continuation starts at `0x8000` in the next NEO segment.

Before lVGM conversion, use the repository's existing VGM optimization tools
in `FMTOWNSCD_EXAMPLE_Cube/vgmtools/` and
`FMTOWNSCD_EXAMPLE_Cube/vgmtool` to strip unnecessary file overhead and reduce
the source VGM sizes. This optimization must be lossless with respect to the
music: it may remove redundant commands, unused metadata, and other data that
does not affect playback, but it must not remove, simplify away, mute, or alter
any audible sound, channel, instrument, note, effect, timing, loop, or register
event. Compare the optimized files against the originals through register-event
and rendered-audio checks before accepting them as generator inputs.

### 10.2 Asset pipeline

Add `tools/msx2/gen_msx_audio.py` which:

1. parses and validates each VGM header;
2. rejects unsupported chips or malformed timing commands;
3. invokes/builds MSXgl's MSXzip converter reproducibly;
4. converts at 60 Hz with `-lVGM -bin --simplify --split 16384` (verify the
   exact converter syntax in the checked-in MSXzip version);
5. verifies the resulting `lVGM` signature, PSG device set, loop marker, end
   marker, and every segment continuation;
6. emits one binary per track plus a machine-readable metadata file containing
   byte size, segment count, loop policy, and source SHA-256.

`gen_msx_scenes.py` currently owns the one global asset segment allocator and
manifest. Keep one owner: have it consume the audio metadata and place audio
after the existing assets, then emit generated track records in
`src/generated/msx2_scenes.h` (or a new generated
`msx2_audio_assets.h` produced in the same grouped Make rule).

Suggested generated record:

```c
typedef struct Msx2MusicAsset {
    u16 first_segment;
    u8  segment_count;
    u8  loop;
} Msx2MusicAsset;

extern const Msx2MusicAsset g_msx2_music_assets[MSX2_MUSIC_COUNT];
```

Add all VGM files and the converter version/build inputs to Make dependencies.
The packer's `manifest.txt` remains the authority for ROM placement.

### 10.3 Runtime player and bank safety

Modify `src/msx2/msx2_audio.c/.h` to replace the silent state reservation with
the real player state and a small driver:

```c
static volatile u8  g_requested_track;
static u8           g_playing_track;
static u16          g_music_segment;
static u16          g_music_loop_segment;

void Msx2_MusicPlay(u8 track); /* queue only; idempotent for same track */
void Msx2_MusicStop(void);     /* queue stop and PSG silence */
void Msx2_AudioTick(void);     /* ISR: switch bank, decode one tick, restore */
static bool Msx2_LvgmNotify(u8 id);
```

`Msx2_AudioTick()` must:

1. save `GET_BANK_SEGMENT(2)`;
2. apply a queued track change at a safe tick boundary;
3. map the track's current segment into bank 2;
4. call `LVGM_Decode()` exactly once per 60 Hz tick;
5. on segment notification, increment the music segment and set the lVGM
   pointer to `0x8000`;
6. on loop notification, restore the recorded loop segment as well as the
   pointer;
7. restore the exact saved bank before returning.

The entire ISR-side call graph must live below `0x8000`: handler, audio tick,
lVGM decoder, notify callback, PSG output routine, and mapper helpers. Reorder
modules if sufficient, or compile a scoped PSG-only copy/wrapper early in the
project module order. Add every required symbol to `RESIDENT_SYMBOLS` in
`tools/msx2/pack_msx_rom.py`; a link above `0x8000` must fail the build.

This constraint is non-negotiable. Merely disabling interrupts in graphics
streaming does not help: the audio tick itself maps page 2 to music data while
it is executing.

`Msx2_MusicPlay()` must be idempotent so repeated scene calls do not restart a
track. Map tracks as follows:

| Runtime ID | Asset |
|---|---|
| `MSX2_MUSIC_TITLE` | `titlescreen.vgm` |
| `MSX2_MUSIC_OPENING` | `Overworld.vgm` |
| new `MSX2_MUSIC_OVERWORLD` | `Overworld.vgm` |
| `MSX2_MUSIC_DECK_EDITOR` | `Overworld.vgm` |
| `MSX2_MUSIC_BATTLE` | `Battle.vgm` |
| `MSX2_MUSIC_BOSS` | `Boss.vgm` |
| `MSX2_MUSIC_FINAL_BOSS` | `FinalBoss.vgm` |
| `MSX2_MUSIC_RESULT` | `Victory.vgm` |
| `MSX2_MUSIC_LOST` | `Fail.vgm` |

Update `Msx2_DealDuel()` to choose BOSS for non-final story boss fights rather
than using BATTLE for all non-final duels. Update the story map to use
OVERWORLD instead of TITLE. On duel completion, select RESULT or LOST before
the hold/return transition.

### 10.4 Interaction with graphics and SFX

The current streamer writes the NEO bank register directly and restores code
segment 1 without updating MSXgl's mapper shadow. Before audio relies on
`GET_BANK_SEGMENT(2)`, make bank ownership coherent:

- either route all page-2 changes through a project wrapper that updates the
  shadow and hardware together;
- or maintain a dedicated `g_msx2_bank2_segment` shadow used by both streamer
  and audio.

There must be one authority. Otherwise the ISR can save a stale segment and
restore the wrong data/code after decoding.

The issue asks for music, not a new SFX engine. Keep existing SFX calls stable;
if PSG SFX remain stubs, document that explicitly. If SFX are enabled later,
they need channel arbitration with lVGM rather than unsynchronised PSG writes.

### 10.5 Audio tests

Static/conversion tests:

- every source is AY-only and converts successfully;
- generated tracks have valid lVGM headers and terminal/loop markers;
- segment splits are aligned and do not overlap another manifest asset;
- rebuilding without source changes is byte-for-byte reproducible;
- source hash changes force regeneration.

Runtime tests:

- probe decoder frame count advances once per V-blank while playing;
- current segment advances on a track larger than 16 KB;
- title and battle tracks loop for at least two full loops;
- Victory and Fail follow their selected one-shot/loop policy;
- changing scenes changes track once and does not restart it every frame;
- after each tick, bank 2 equals the value saved on entry;
- run simultaneous long scene streams and music; the ROM must neither crash
  nor corrupt images;
- `./msx2.sh verify --seconds 300` still completes a duel with music enabled.

Observable-output test:

- add an openMSX audio-capture helper or PSG-register trace;
- verify non-silent AY register writes and compare a short capture fingerprint
  for each track;
- listen to title, Overworld, Battle, Boss, FinalBoss, Victory, and Fail for
  tempo, looping, stuck notes, and transition pops;
- repeat on physical MSX2 hardware, especially while a 54 KB scene streams.

Budget acceptance:

- fixed code remains below `0xC000`;
- the complete ISR audio call graph is below `0x8000`;
- page-0 bank limits still pass;
- static RAM does not exceed the previous 700-byte audio reservation by more
  than an explicitly reviewed amount;
- measure ISR duration as part of the still-open M1b timing work and record it
  in `src/msx2/STATUS.md`.

## 11. Issue G: seed randomness from RTC, Z80 R, and timing

### 11.1 Behavioural requirements

- Two human-started free battles should not normally deal the same decks after
  a reset.
- Entropy collection must work when the RTC is absent, unset, frozen, or
  emulated deterministically.
- No random source is allowed to affect rules after the duel seed is chosen.
- Soak, story-soak, captures, and regression builds must be exactly repeatable.
- Continue-code reconstruction must remain deterministic. Do not seed the
  starter deck from wall-clock entropy unless the save payload is expanded to
  encode that deck/seed.

### 11.2 New module

Add `src/msx2/msx2_entropy.c/.h`:

```c
enum Msx2EntropyFlags {
    MSX2_ENTROPY_RTC   = 1 << 0,
    MSX2_ENTROPY_R     = 1 << 1,
    MSX2_ENTROPY_TICKS = 1 << 2,
    MSX2_ENTROPY_INPUT = 1 << 3,
};

void Msx2_EntropyInit(void);
void Msx2_EntropyMixInput(u8 held, u8 pressed, c8 typed);
u32  Msx2_EntropyNextSeed(void);
u8   Msx2_EntropySources(void);
```

Implementation outline:

1. Read the RTC's BCD second/minute/hour/day/month/year nibbles using MSXgl's
   `clock` module or direct `0xB4/0xB5` access. Validate BCD ranges and
   `RTC_IsSettingOK()` before setting the RTC flag. Sample coherently (read
   seconds before and after, retry if they changed).
2. Read Z80 `R` several times around other work. Mask or deliberately mix bit
   7 according to Z80 semantics; do not mistake `R` for a random-number
   generator. It contributes instruction-phase variation only.
3. Mix `g_msx2_ticks` and the exact tick at title/menu input edges.
4. Mix low-cost input timing/data in `Msx2_InputUpdate()` via
   `Msx2_EntropyMixInput()`.
5. Use a small avalanche mixer to build a non-zero 32-bit state, then keep the
   existing xorshift32 generator for subsequent duel seeds.
6. In `Msx2_DealDuel()`, request one seed only after the player commits to the
   duel, maximizing timing entropy.

Use compile-time deterministic override:

```c
#if defined(MSX2_DEBUG_AUTOPLAY) || defined(MSX2_DEBUG_STORY_AUTOPLAY) || \
    defined(MSX2_DEBUG_REGRESSION)
    return MSX2_TEST_SEED;
#endif
```

Do not overload `time()`/`clock()` with new side effects. They may continue to
report ticks for shared code; MSX2 duel seeding should use the explicit entropy
API. Add `"clock"` to `LibModules` only if the chosen RTC calls require
`clock.c`, and disable unused RTC save/text helpers in `msxgl_config.h` to
control code size.

### 11.3 Tests

- Deterministic variants: two clean builds/runs produce identical seed, first
  hands, AI choices, and probe state at the same frame.
- RTC test: set two different RTC times in openMSX, issue identical timed input,
  and verify different seeds with the RTC source flag set.
- No-RTC/frozen-RTC test: identical machine resets followed by different input
  delays produce different seeds using ticks/R/input.
- Rapid-start test: start several duels in one boot and verify the internal
  sequence never produces zero and does not repeat in a practical sample.
- Continue-code test: save, reset with a different RTC, load, and verify story
  progress/name/deck overrides are identical. A shuffled duel order may differ
  only when a new duel legitimately receives a new seed.
- Statistical smoke test: collect at least 1,000 seeds in an emulator harness,
  report duplicates and per-bit one frequency. This is not a cryptographic
  test; it catches stuck sources and missing mixing.
- Physical hardware: cold boot twice, start at visibly different times, and
  compare initial free-battle hands.

## 12. File-by-file change map

| File | Planned responsibility |
|---|---|
| `MSX2_issues_last.txt` | Leave as the source report; optionally mark items closed only after acceptance |
| `src/msx2/msx2_board.c` | hand deal masks; one-view ownership; transition sprite barrier calls; remove redundant panel text/dirty work |
| `src/msx2/msx2_duel.c` | change only if probe proves logical turn-start draw is wrong |
| `src/msx2/msx2_sprite.c/.h` | transition barrier and optional debug sprite owner/visibility |
| `src/msx2/msx2_story.c` | save-phase input/state fix; map/ending music selection |
| `src/msx2/msx2_story_load.c` | no change unless shared phase-entry input consumption belongs here |
| `src/msx2/msx2_disk.c` | bank restore fix only if tracing proves disk probe corrupts page 0 |
| `src/msx2/msx2_bank.c/.h` | read-only current-bank invariant; regression trampolines if needed |
| `src/msx2/msx2_audio.c/.h` | real lVGM playback, segment callback, queued/idempotent transitions, diagnostics |
| `src/msx2/msx2_entropy.c/.h` | RTC/R/tick/input seed material and deterministic override |
| `src/msx2/msx2_main.c` | initialize entropy; choose duel/result tracks; consume next seed |
| `src/msx2/msx2_input.c` | mix human input timing; optional transition-edge consumption API |
| `src/msx2/msx2_probe.c/.h` | conditional regression diagnostics and audio/entropy counters |
| `src/msx2/msx2_ui_strings.def` | remove unused SUMMON/result/prompt strings |
| `src/msx2/project_config.js` | lVGM/PSG/clock modules, defines, and linker order |
| `src/msx2/msxgl_config.h` | PSG-only lVGM and minimum RTC feature switches |
| `src/msx2/waifu_msx2_s2_b0.c` | include regression-only duel fixture code only if bank access is required |
| `tools/msx2/gen_msx_audio.py` | validate/convert VGM to segmented lVGM metadata/blobs |
| `tools/msx2/gen_msx_scenes.py` | allocate music segments and generate track records through the single manifest owner |
| `tools/msx2/pack_msx_rom.py` | pack music; enforce resident ISR/audio symbols and non-overlap |
| `tools/msx2/read_probe.py` | decode the conditional/versioned diagnostics |
| `tools/msx2/compare_sequence.py` | transient visual/SAT assertions |
| `msx2.sh` | regression variant and multi-frame/audio capture commands |
| `Makefile.msx2` | VGM dependencies, grouped generated outputs, variant stamps, regression targets |
| `src/msx2/STATUS.md` | final measured behaviour, audio mapping, budgets, tests, and remaining hardware caveats |

## 13. Delegation plan

Use one integrator and up to three parallel implementers. Assign files, not just
topics, to avoid agents editing the same large source simultaneously.

### 13.1 Workstream A: diagnostics and verification tooling

Owner files:

- `msx2.sh`
- `tools/msx2/read_probe.py`
- new `tools/msx2/compare_sequence.py`
- `src/msx2/msx2_probe.c/.h`
- new regression harness files

Deliverables:

- regression build and fixed-seed plumbing;
- multi-frame VRAM/SAT capture;
- diagnostic probe version;
- fixtures for hand, placement, pass-turn, save, and results;
- baseline traces that reproduce every reported bug before a fix.

This workstream starts first. It hands trace evidence to B and C, then remains
available to add assertions without editing their production modules.

### 13.2 Workstream B: duel presentation fixes

Owner files:

- `src/msx2/msx2_board.c`
- `src/msx2/msx2_sprite.c/.h`
- `src/msx2/msx2_ui_strings.def`
- `src/msx2/msx2_duel.c` only if rules-state evidence requires it

Deliverables:

- monotonic, two-page-correct turn-four hand deal;
- one overhead transition per placement;
- sprite transition barrier;
- redundant text and redundant dirty work removed;
- targeted frame-sequence tests passing.

Split this further only after diagnostics: hand/deal and view/sprite/text can be
separate commits, but two agents should not edit `msx2_board.c` concurrently.

### 13.3 Workstream C: story save flow

Owner files:

- `src/msx2/msx2_story.c`
- `src/msx2/msx2_story_load.c`
- `src/msx2/msx2_disk.c`
- relevant bank/input helpers coordinated with the integrator

Deliverables:

- phase/bank trace identifying the real cause;
- correct save picker and password flow on no-drive configurations;
- disk-backed save/load tested in a drive-equipped openMSX profile;
- a clearly retained physical-hardware verification item if hardware is not
  available during implementation.

### 13.4 Workstream D: audio and entropy

Owner files:

- `src/msx2/msx2_audio.c/.h`
- new `src/msx2/msx2_entropy.c/.h`
- `tools/msx2/gen_msx_audio.py`
- audio-related generator/config changes

Deliverables:

- reproducible PSG-only VGM-to-lVGM conversion;
- safe multi-segment playback and loops;
- resident-call-graph enforcement;
- scene-to-track mapping;
- RTC/R/tick/input entropy with deterministic test override;
- audio and seed diagnostics.

This is one workstream because audio and entropy both touch `msx2_main.c`,
`project_config.js`, fixed-code layout, ISR timing, and build variants. If two
agents are used, assign one the offline audio pipeline and one runtime
audio/entropy, then have the integrator alone modify shared build/config files.

### 13.5 Integrator responsibilities

The integrator owns:

- `Makefile.msx2`, `project_config.js`, `msxgl_config.h`, and
  `pack_msx_rom.py` final merges;
- conflict resolution in `msx2_main.c` and generated-asset allocation;
- running all build variants after every merge;
- budget comparisons and resident-symbol checks;
- final shipping-path captures and `STATUS.md` update.

Recommended commit order:

1. `msx2: add final-issue regression traces and fixtures`
2. `msx2: make hand deal completion explicit on both pages`
3. `msx2: coalesce overhead placement composition`
4. `msx2: hide sprite layers at transition boundaries`
5. `msx2: remove redundant summon and result panel text`
6. `msx2: keep story save inside the save phase`
7. `msx2: generate and pack PSG lVGM tracks`
8. `msx2: play banked lVGM safely from the V-blank ISR`
9. `msx2: seed human duels from RTC R and input timing`
10. `msx2: document final verification and hardware caveats`

Each commit should build independently. Do not combine all rendering changes
into one commit; transient regressions are much easier to bisect when hand,
view, sprite, and text changes are separate.

## 14. Full verification matrix

### 14.1 Fast checks after each production change

```sh
make -f Makefile.msx2
./msx2.sh ram
git diff --check
```

Also run the affected regression fixture and compare both visible pages/SAT,
not just the final screenshot.

### 14.2 Required variants

```sh
make -f Makefile.msx2                 # shipping
make -f Makefile.msx2 soak            # duel autoplay
make -f Makefile.msx2 story-soak      # deterministic story progression
make -f Makefile.msx2 regression      # targeted final-issue fixtures
```

The variant stamp must distinguish all four; MSXgl does not track changed `-D`
flags by itself.

### 14.3 Automated acceptance

```sh
./msx2.sh verify --seconds 300
./msx2.sh neo-test
./msx2.sh ram
```

Add scripted regression commands for:

- COM turn-four hand, frame-by-frame;
- player overhead placement, attack and defence;
- top-view pass and chair handoff SAT trace;
- result win/loss panel stability;
- story save picker on no-drive and drive-equipped machines;
- password round-trip across reset;
- title/battle music segment crossing and loop;
- deterministic soak seed and variable human seed.

### 14.4 Manual observable-output pass

On real openMSX:

1. Play free battle through at least eight turns.
2. Inspect both COM and player hand deals, including hands with empty slots.
3. Place from every hand position in attack and defence.
4. Enter/leave overhead view, pass from overhead, check a card, and observe
   every transition for stale sprites or black flashes.
5. Win and lose a duel; confirm only the desired sprite result remains.
6. Start story, open SAVE GAME, exercise PASSWORD, return to road, and load the
   code from the title.
7. With a drive profile, save to a disposable floppy image, reset, and load.
8. Listen to every supplied track through at least one loop or natural end.
9. Start free battles after different delays and confirm hands vary.

### 14.5 Physical hardware gate

Physical hardware is authoritative. Before calling the work fully complete:

- verify music tempo and multi-segment playback while scenes stream;
- verify the turn-four hand and overhead placement visually;
- verify no selector residue across turn/scene transitions;
- verify RTC-based entropy on cold/warm boots;
- test floppy write/read with a sacrificial formatted disk if disk saving is a
  shipping requirement.

If hardware is unavailable, mark only the hardware gate pending. Do not state
that emulator timing or disk behaviour proves the physical machine.

## 15. Definition of done

All work is complete only when:

- every issue has a deterministic reproduction and a passing regression;
- a COM hand never regresses from N visible settled cards to fewer during a
  deal, and both pages finish identical;
- one player placement performs at most one full overhead-view composition and
  shows no intervening black frame;
- the gem and all other transient sprites are hidden before a new coordinate
  space or scene becomes visible;
- the four requested bitmap strings and their needless panel clears are gone;
- SAVE GAME opens a floppy/password picker, password always works, and neither
  picker nor password output accidentally returns to title;
- all seven supplied VGM tracks play through the PSG using lVGM, including
  tracks spanning multiple NEO segments;
- the ISR-side audio call graph is enforced below `0x8000` and bank 2 is always
  restored;
- human duel seeds mix valid RTC data when available plus R/ticks/input timing,
  while all automated variants remain deterministic;
- shipping, soak, story-soak, regression, NEO mapper, code-bank, RAM, and
  generated-asset checks pass;
- observable output has been reviewed on real openMSX and the remaining
  physical-hardware status is recorded honestly in `src/msx2/STATUS.md`.
