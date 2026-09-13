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
stamp's `dropped` word, and the previous picture stays up.  The lower 80
screen lines are a black band the OBJ HUD sits on: life points, labels, hand
cards, card names, stats, cursors and result banners are all sprites at full
screen resolution.

Two painting paths feed the same frame (`snes_duel.c`, `snes_board3d.c`):

* **Resting camera** (yaw 0 or 128, level, at the ROM floor's pose): the floor
  is copied from a pre-rendered chunky image in ROM, cards are drawn over it
  off the 32x32 sheets with a per-row perspective walk, and from then on the
  picture is PATCHED -- the slot that changed, the cursor's old and new slots,
  the held card's cells -- and only those cells are converted.  A resting
  cell that nothing covers is DMA'd from the planar ROM floor without
  conversion at all.
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

The lift is four rendered poses on an eased path from the seat to 4.1 units
over the middle of the board looking straight down (`LIFT_HEIGHT`,
`LIFT_HORIZON`, with a half-sine bump of 0.8 units in the middle so no
intermediate pose is over the cell budget).  The overhead board is 160x128
pixels, 340 cells, 1:1 with its 32-texel-a-unit world texture.  A pose is
about fifty fields of mapping and conversion (measured in the emulator: 20
fields of walking, 30 of conversion for 340 cells); it is rendered in small
steps (`job_run`), and between the steps the hand's sprites are moved in
place and uploaded by the NMI, so the hand glides continuously while the
board arrives four times.  The overhead view's red cursor is a sprite
bracket projected through the same camera.

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

The card check and the battle cut-in are Mode 3 pictures, not sprites
(`snes_cardart.c`).  Each card is the PC-FX 120x160 battle card -- the
112x112 painting inside the gold-rimmed frame of `draw_big_battle_card_stats`
-- baked by `tools/snes/gen_snes_bigcards.py` as 225 8bpp BG1 tiles with
eighty colours of its own; the frame's foot (the ATK/DEF plate) is a shared
tile set.  Two cards share CGRAM by the second one's tiles having bitplane 7
set at upload time (eight fixed-source DMAs into the odd bytes of the last
plane pair), so slot 0 reads entries 32..111 and slot 1 reads 160..239.  The
words are BG2 text.  The check screen is the PC-FX layout: card on the left,
CARD CHECK / name / stars / attribute and tribe / LORE / ATK and DEF on the
right.  The battle reveals both cards from the screen's edges inwards with two
inverted, AND-combined BG1 windows.

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
geometry and resolution, card orientation, top-view transition, card check,
fusion, placement flight, duel flow, and render cost. Emulator timing is a
regression measure; it is not a physical hardware timing claim.
