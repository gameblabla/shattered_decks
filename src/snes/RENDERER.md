# SNES board renderer

The duel board is a 256x144 chunky direct-colour frame in WRAM (`$7E7000`,
one byte a pixel, BBGGGRRR) shown at 1:1 through Mode 3 BG1 in direct colour.
It is not uploaded whole: the sparse presenter (`snes_fb.asm`,
`snes_conv_drivers.inc`, the generated `snes_conv_gen.asm`) converts the
occupied 8x8 cells into 8bpp planar tiles, allocates them out of a 702-tile
store shared copy-on-write between two logical maps, and switches the PPU to
the new map in one vblank once every tile it names is resident.  A frame's
span -- the cells between the first and last occupied column of each 8-row
band -- may not exceed 351 cells; a frame over that is refused, counted in the
stamp's `dropped` word, and the previous picture stays up.  At the chair the
lower 80 screen lines are a black band the OBJ HUD sits on: life points,
labels, hand cards, card names, stats, cursors and result banners are all
sprites at full screen resolution.  The overhead view moves that BG1 clip
down to the plate's top line (197) and tile-scrolls the same 144-line picture
so the selected field row remains near screen centre; no redraw or map swap is
needed when its cursor moves, and neither the board nor a hand card is ever
drawn over the plate (a hand card whose rows would reach it is not shown).

Two painting paths feed the same frame (`snes_duel.c`, `snes_board3d.c`):

* **Resting camera** (yaw 0 or 128, level, at the ROM floor's pose): the floor
  is copied from a pre-rendered chunky image in ROM, cards are drawn over it
  off the 32x32 sheets with a per-row perspective walk (`snesDrawCardRow`
  does a slot row's setup -- two divides, three edge slopes -- and
  `snesCardRows` in `snes_raster.asm` steps the pixel rows and their five
  spans: the C that did that cost four times the walker it was calling, and
  moving it took a full-board bake from 11236 to 8878 lines), and from then
  on the picture is PATCHED -- the slot that changed, the cursor's old and new slots,
  the held card's cells -- and only those cells are converted.  A resting
  cell that nothing covers is DMA'd from the planar ROM floor without
  conversion at all.  A monster in DEFENCE lies turned a quarter on its slot
  (bit 7 of its entry in the face arrays, `FACE_DEF`): it is taken out of
  its row and drawn after it as the convex quad of `snesCardQuad` with the
  footprint's extents swapped and the texture corners walked one vertex
  round, the PC's rotated defence card; in the world texture it is a column
  walk of the face (`texture_stamp_turned`).
* **Moving camera** (the lift to the overhead view, the turn between seats):
  the twenty cards are stamped from the same 32x32 sheets into a 256x128
  world-space texture in bank `$7F` (kept warm in the first idle frame after
  the board changes), the slab is one textured quad through the inverse-ray
  mapper, and every occupied cell is converted.  Without yaw the slab is a
  trapezoid symmetric about the middle of the frame, and the whole per-row
  job -- edges, plane terms, the texel step in texels (`depth / 4`, rounded
  once) and the constant-v walk -- is `snesFloorRowsPitch` in
  `snes_raster.asm`; a yawed camera goes through the C mapper.  The frame is
  cleared by a fixed-source DMA from a ROM byte ramp: WRAM cannot be DMA'd
  to WRAM, and the earlier WRAM-sourced fill was silently ignored, which is
  where the fragments of the previous pose at a moving board's edges came
  from.

The lift is three rendered poses on an eased path from the seat to exactly
4.0 units over the middle of the board looking straight down (`LIFT_HEIGHT`,
`LIFT_HORIZON`, with a half-sine bump of 0.8 units in the middle so no
intermediate pose is over the cell budget).  At 4.0 units the focal length
of 128 makes a unit 32 pixels, the world texture's 32 texels: the overhead
board is the texture itself, 160x128 pixels in sixteen whole 8-line bands
of twenty cells (320 of 351), and its floor rows are MVN block moves
(`snesSpanFloorTex` takes that path when the step is exactly one texel a
pixel) instead of the ~30-cycle-a-texel walk.  A perspective pose is about
forty fields of mapping and conversion, the overhead one about thirty; the
poses are the lift's duration, so there are three, and the descent renders
one fewer because its last pose would be the rest camera through the moving
path, baked again from the ROM floor a moment later.

**Every hot loop runs on the "3.5 MHz RAM"** (`snes_fastdp.inc`).  All of
WRAM is an eight-cycle access, but DMA channels 1-3 never move anything, and
their thirty-six register bytes at $4310-$433B are read/write latches at six
cycles: with D = $4300 they are the direct page of the span walkers, the
motion mapper, the sprite store, the fixed-point math and the converter
driver, one call at a time (leaf scratch only -- nothing survives a call
there).  The walkers also write the frame through WMDATA ($2180, a six-cycle
port with its own address counter) instead of an indexed WRAM store, so the
constant-v walk has no slow access left in it; the world image is cleared by
ROM-to-WMDATA DMA and the card stamps are the same walk.  What the cycle
profiler (`tools/snes/prof.py`, on the patched headless core) still shows
as WRAM-bound is the converter's tile routines, whose output must be the
ring slot itself.

**A constant-v span is a BOUNDED RUN where it can be proved one**
(`snes_raster.asm`, `RS_RUN`; the 2026-09-14 performance pass,
`SNES_PERFORMANCE_v2.md`).  The walk keeps B for the index's high byte and
rebuilds X with `tax` after every eight-bit add, which costs a load and a
store of the fraction every texel.  Over a piece of the span that cannot
cross its texture row, X holds the whole index and the FRACTION lives in B
(`xba; clc; adc; xba; bcc +; inx; +`), and the whole texels of du are baked
into the addresses of a 128-body unrolled block (`lda.l TEX-(127-i)*k,x`)
entered n bodies from its end -- no loop, and k = 0..3 cost the same:
about 150 master cycles a texel against the walk's 189 (measured on the
world texture, k = 1..2).  A dispatcher proves each piece: a cheap
`index.low + count*(k+1) <= 255`, else the exact end through the CPU
multiplier; a k = 0 span that wraps is cut at the wrap through the CPU
divider and the wrap step walked, a k >= 1 span that wraps is walked whole.
So that the board's rows never wrap, the world texture's u origin is
`SNES_WORLD_U_CENTRE` = 144 (`snes_video.h`): the board lies on texels
64..223 and an overhead row is one block move.  The card stamps
(`snesBoardTextureCard`) have a 32-body block of their own with a fixed
entry per stamp.  The converters fold the four pair lookups in reverse
(`T3; >>2 | T2; >>2 | T1; >>2 | T0`, six shifts a plane pair instead of
twelve) and the half converter stores each duplicate row once.  Measured
per sampled texel (same texels, same pictures): the fixture bake 234 -> 196
master cycles of sampling, the motion frames 202 -> 182; full-tile
conversion -12.6%, half -16%; the whole bake 8876 -> 8323 lines, a motion
frame 3184 -> 3005.

A pose is rendered in small steps, and a game loop takes a measured SLICE
of them (`job_slice`: steps until three and a half fields have gone by),
sliding the hand's sprites in place every other field for the NMI to
upload, before handing the loop back for the pad, the sprite layer and the
audio queue -- and while a pose is in progress the main loop does not wait
for vblank (`snesDuelBusy`), since a slice ending just after one would idle
most of a field.  The hand therefore glides continuously while the board
arrives three times.  Measured: about 140 fields from the UP press to the
overhead view over a full board; one step a game loop, with a vblank wait
and a sprite rebuild each, was over a thousand.  The overhead view's red
cursor is a sprite bracket projected through the same camera.

The opponent's turn is presented, not just applied (`snes_duel.c`,
`COM_PRESENT_*`): the rules act once (`Msx2_DuelStep`), the hand as it was
before that action is kept as a snapshot, and the frontend then shows it
along the lower edge as card backs -- the PC's convention, the active
duelist's hand is always at the bottom -- with the red cursor visiting a
slot every four fields before settling on the chosen one, the chosen back
flying to its slot (projected from WORLD coordinates: `snesProject` applies
the camera's yaw itself), the placed cue, and the board rendered with the
card once it has landed.  Draws, the opponent's and the player's, arrive
one card every six fields with the draw cue (`note_draws`, `hand_visible`).
No face, name or number of the opponent's hand reaches the HUD.

The card being played is not drawn in the hand while its slot is being
chosen: it hovers over the board as the held card (an affine quad off the
32x32 sheet), the flight on A starts from where it hovers, and B puts it
back.  A fusion chain's cards leave the hand the same way while the target is
chosen.

In board view, LEFT/RIGHT move across the hand and A begins placement. In the
placement view, A places face-up, X places face-down, and B cancels. DOWN on a
hand card queues it for fusion; A then enters the field target view, where A
confirms and B cancels. X enters the attack phase and START ends the turn. UP
opens the top view; its own red cursor moves with the directional buttons, A
opens card check, and B closes card check or returns to the board. Y opens the
deck editor from the title attract screen.

Victory and failure use one-shot result audio and a large multi-sprite banner.

Card checks and support effects are Mode 3 pictures, and the battle cut-in
is a Mode 4 one, not sprites (`snes_cardart.c`, `snes_battle.c`).  Each card is the PC-FX 120x160 battle card -- the
112x112 painting inside the gold-rimmed frame of `draw_big_battle_card_stats`
-- baked by `tools/snes/gen_snes_bigcards.py` as 225 8bpp BG1 tiles with
eighty colours of its own; the frame's foot (the ATK/DEF plate) is a shared
tile set.  Two cards share CGRAM by the second one's tiles having bitplane 7
set at upload time (eight fixed-source DMAs into the odd bytes of the last
plane pair), so slot 0 reads entries 32..111 and slot 1 reads 160..239.  The
words are BG2 text.  The check screen is the PC-FX layout: card on the left,
CARD CHECK / name / stars / attribute and tribe / LORE / ATK and DEF on the
right.  Support effects use the same layout for their rule text; Thunder then
takes every destroyed victim one at a time the PC-FX way: the card alone in
the middle of the screen, the direct attack's sprite burst over it with the
destruction cue and a white flash, and a wipe from the top down (window 1's
edges by HDMA, `snesCardArtWipe`) while the burst dies.  The FUSION is not a
card-art scene at all but the PC-FX's own screen over the board's Mode 3
layer (`fusion_scene_enter`): ten direct-colour tiles and a map for the navy
stripes and panel (built once at the duel's entry), the materials gliding
out of the hand as the hand's own resident OBJ sprites into a row while
sparks circle, a white-and-gold backdrop flash with every layer off, and the
card that came of it under FUSION SUCCESS or FUSION FAILED / LAST CARD
PLACED.  The battle (`snes_battle.c`) is Mode 4 so BG1 can be offset per
tile column: the two cards enter by scrolling vertically -- the player's up,
the opponent's down -- which offset-per-tile does smoothly, where horizontal
motion would step in eight-pixel chunks; the direct attack has its own
effects timeline.  Its sound cues fire on crossing their field of the
timeline, independently of which pose branch a late step lands in.  A blow
that decides the duel holds its result pose until the destruction sample has
played out (`B_VERDICT_AT`) before the verdict and its music arrive -- the
song's load restarts the SPC player, which keys every voice off -- and the
skip is refused for it.

The OBJ card pump (`snesObjVblank`) takes TWO rows a vblank, not a card:
the 816-tcc round each 128-byte DMA is five lines, the NMI hands the window
back at line ~231 and the OAM copy ends at 235, so four rows ran past the
end of vblank and the check refused nearly every card (they arrived by
retries, and on a screen with no drain to share the window with, never).

The title, story dialogue and ending are separate Mode 3 scenes
(`snes_scene.c`).  The title is the whole painting on BG1 with PRESS START
baked into a second map's two rows and the menu drawn as BG2 text over a
colour-math window (a translucent blue plate, no tiles).  The story dialogue's
sky is the backdrop tinted per scanline by HDMA (COLDATA, red/green and blue
channels), the ground is three rows of 4bpp BG2 tiles under the horizon on
line 120 (the void scatters six star tiles instead), and the two speakers are
16x17 blocks of 8bpp BG1 tiles, 112 colours each, sliding in by map rewrite.
Which of the four paintings a dialogue gets follows the shared
`story_scene_kind`: desert, then temple, volcano and void.  Scene transitions
reinitialize the video state and do not depend on a black intermediate frame.

Build and regression: `make -f Makefile.snes verify`. The harness checks the
cartridge header, title and input path, SRAM, story and ending scenes, board
geometry and resolution, card orientation, top-view transition, the overhead
board's clip at the plate, a turned defence card in both views, card check,
Thunder's victims, the fusion screen, placement lowering, duel flow, the
opponent's turn presentation, and render cost. Emulator timing is a
regression measure; it is not a physical hardware timing claim.
