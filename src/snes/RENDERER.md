# SNES board renderer

The duel board uses a 128x128 chunky framebuffer in bank `$7F`, displayed by
Mode 7 in direct colour. The visible board is 128x80 texels at rest and during
motion; each texel is shown as a 2x2 screen pixel. The lower 64 screen lines
read rows 80..111 of the same surface. The HUD itself is a 1:1 OBJ layer, so
life points, labels, hand cards, card names, stats, cursors, and result banners
remain full screen resolution. The backdrop gradient supplies the HUD plate.

The software renderer fills floor spans and card spans into the framebuffer.
Resting cards use a perspective texture walk; cards in flight use an affine
quad walk. Floor and card textures are cached in the board surface and are
re-rendered when the board, cards, or camera pose changes. The renderer then
uploads bounded row ranges over several vblanks, with OAM and HUD updates kept
in their own DMA work. A new frame waits for its previous upload to finish.

The hand-to-top transition is a 20-field matrix zoom around the board centre.
It changes the Mode 7 scale while the hand moves offscreen and does not bake a
new board pose for every animation field. Once the top card tiles are resident,
the presenter switches during vblank to Mode 3. Top view is a full 256x224
8bpp BG1 table with up to twenty field cards on sprites; opponent cards use
vertically flipped artwork. Returning to the board re-primes the Mode 7 HDMA
tables before restoring the hand view.

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
