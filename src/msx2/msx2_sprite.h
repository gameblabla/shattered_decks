// ─────────────────────────────────────────────────────────────────────────────
//  msx2_sprite.h — the V9938 sprite layer, over GRAPHIC 7
//
//  Everything else in this port is bitmap: one byte a pixel, two 64 KB pages,
//  and a change costs a VDP command or a block write.  Sprites cost neither.
//  In GRAPHIC 7 the V9938 still draws 32 sprites of 16x16 (magnified to 32x32
//  here) from a pattern table, in the sixteen palette colours that in this mode
//  belong to the sprites alone -- and the tables are absolute VRAM addresses,
//  so one set of sprites floats over BOTH pages and survives every page flip.
//
//  That is exactly what the effects wanted.  An explosion painted into the
//  bitmap has to be saved, drawn, and put back on two pages; the same explosion
//  as a sprite is one attribute byte a frame, which is why it can have eight
//  frames instead of one held pose.
//
//  WHERE THE TABLES LIVE, AND THE RULE THAT KEEPS THEM.
//  MSXgl's GRAPHIC 7 defaults put the patterns at 0xF000, the colours at
//  0xF800 and the attributes at 0xFA00 -- rows 240..250 of page 0, past the
//  212 that display.  Nothing may copy over them, so Msx2_VideoCopyPage()
//  duplicates rows 0..239 instead of all 256, and every offscreen stash in the
//  port is below row 240.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"
#include "msx2_scenes.h"

// The last VRAM row a page copy may touch.  Above it are the sprite tables.
#define MSX2_SPRITE_VRAM_ROW  240

// SPRITE COLOURS IN GRAPHIC 7 ARE NOT THE PALETTE.
// This is the one mode where the sixteen palette registers do not reach the
// sprite plane: the V9938 colours a GRAPHIC 7 sprite out of a FIXED table
// burnt into the chip (data book p.98), so VDP_SetPaletteEntry() below is
// documentation of intent and nothing else.  What each index actually is, as
// GRB with three bits a channel:
//
//   0 000 black*  1 002 dk blue  2 030 dk red   3 032 dk magenta
//   4 300 dk grn  5 302 dk teal  6 330 dk yell  7 332 grey
//   8 472 orange  9 007 blue    10 070 red     11 077 magenta
//  12 700 green  13 707 cyan    14 770 yellow  15 777 white
//
// (*) Index 0 is the sprite's transparent code, so black is not ordinarily
// available at all -- see MSX2_SPR_BLACK.
//
// The names below are the colour each index was CHOSEN for; several of them
// are a shade or two away from what the chip draws.  They are left as they are
// because the screens they build were signed off as they look.
#define MSX2_SPR_WHITE   1
#define MSX2_SPR_GOLD    2
#define MSX2_SPR_RED     3
#define MSX2_SPR_TEAL    4
#define MSX2_SPR_BLUE    5
// The selector's two families, each a shadow and a highlight around the flat
// colour above: a one-bit sprite has no shades of its own, so the gem gets one
// sprite per band and the bands get their tints from here.
#define MSX2_SPR_RED_DK   6
#define MSX2_SPR_RED_HI   7
#define MSX2_SPR_TEAL_DK  8
#define MSX2_SPR_TEAL_HI  9
// The heat between gold and red, for the blade sweep.
#define MSX2_SPR_ORANGE  10
// OPAQUE BLACK, WHICH THE FIXED TABLE DOES NOT OTHERWISE HAVE.
// The wipe that takes a destroyed card off the board has to be BLACK, and
// every index from 1 up is a colour: the burn was drawn in 11 and came out
// bright magenta.  Index 0 IS black in the table -- it is only unavailable
// because a sprite treats code 0 as transparent, and that is exactly what R#8
// bit 5 (TP) turns off.  So the burn clears TP for as long as it is on the
// screen and paints in code 0; nothing else in the port draws a code-0 sprite,
// and in GRAPHIC 7 the bit reaches nothing but the sprite plane.
#define MSX2_SPR_BLACK   0

// Pattern slots.  A 16x16 sprite is four 8x8 quarters, so a slot is 32 bytes
// and the attribute's pattern byte is slot * 4.
#define MSX2_SPR_BURST0  0    // eight frames of an expanding burst
#define MSX2_SPR_BURST_N 8
// Eight frames of the spinning selector, MSX2_GEM_PLANES one-bit planes each,
// frame after frame: the plane of a frame is GEM0 + frame * PLANES + plane.
#define MSX2_SPR_GEM0    8
#define MSX2_SPR_GEM_N   (MSX2_GEM_FRAMES * MSX2_GEM_PLANES)
// Slots 0..31 are exactly the cartridge's pattern blob, in order, which is what
// lets Msx2_SpriteInit() upload it as one run.  The letters come after, because
// they are the only patterns still built at runtime -- out of the font.
#define MSX2_SPR_LETTER0 (MSX2_SPR_GEM0 + MSX2_SPR_GEM_N)
#define MSX2_SPR_LETTER_N 8
// Two more built at runtime: one segment of a blade leaning down-right, and its
// mirror.  A stroke is four of them stacked, and the bar inside a segment
// drifts exactly as far across as the next segment is offset, so four cells
// read as one unbroken cut.
#define MSX2_SPR_SLASH0  (MSX2_SPR_LETTER0 + MSX2_SPR_LETTER_N)
#define MSX2_SPR_SLASH_R MSX2_SPR_SLASH0
#define MSX2_SPR_SLASH_L (MSX2_SPR_SLASH0 + 1)

// Twenty-four of the ordinary sprite ids form two 3x4 card wipes.  Their
// 32-pixel rows are deliberately not vertically overlapped: both cards can
// wipe at once without exceeding the V9938's eight-sprites-per-scanline limit.
#define MSX2_SPR_BURN_PATTERN 42
#define MSX2_SPR_BURN_CARD_IDS 12

// The sprites the selector uses: the last MSX2_GEM_PLANES of the thirty-two,
// so they are behind everything the effects put up (the V9938 gives the lower
// id the higher priority) and can never take one of the eight-per-line slots a
// burst wants.  The planes are disjoint masks of one solid, so stacking them
// costs nothing but attribute bytes.
#define MSX2_SPR_CURSOR  (32 - MSX2_GEM_PLANES)

void Msx2_SpriteInit(void);

// Hide every transient sprite group before a scene or camera coordinate space
// changes.  The bitmap page is not exposed until the VDP command queue has
// also completed, so an old gem/slash/burn cannot ride over the first frame of
// the destination scene.
void Msx2_SpriteTransitionBegin(void);

// Take every sprite off the screen.  Cheap: it writes one byte per sprite.
void Msx2_SpriteClear(void);

// Place sprite `id` (0..31).  x and y are the top-left of the 32x32 magnified
// square; y is stored as line-1, which is what the VDP wants.
void Msx2_SpriteAt(u8 id, u8 x, u8 y, u8 slot, u8 color);

// Hide one sprite without disturbing the others.
void Msx2_SpriteHide(u8 id);

// Build the pattern slots for a word out of the bitmap font, doubled to 16x16.
// Returns how many letters were taken (at most MSX2_SPR_LETTER_N).
u8   Msx2_SpriteWord(const c8* text);

// The word, centred on `y`, as one sprite per letter in `color`.  A 32x32
// letter is two sprite lines wide in the eight-per-line budget, so this is for
// short words: YOU WIN, YOU LOSE, FUSION.
void Msx2_SpriteShowWord(const c8* text, u8 y, u8 color);

// How far apart Msx2_SpriteShowWord puts two letters.  Public because a caller
// that slides a word has to know how wide the whole of it is.
#define MSX2_SPR_WORD_PITCH 26

// The same word with its LEFT EDGE at `x`, which may be negative or past the
// right of the screen -- that is what lets a result banner slide on from off
// the side.  `text` and the letter count must be the ones a preceding
// Msx2_SpriteWord() uploaded; this call only moves what is already there.
void Msx2_SpriteWordAt(const c8* text, u8 n, i16 x, u8 y, u8 color);

// ── The blade sweep ─────────────────────────────────────────────────────────
// Two crossing strokes of four 32x32 segments each, centred on (x, y).  `grown`
// is how many segments of each stroke to show, 0..MSX2_SPR_SLASH_SEGS, and it
// is the whole of the animation: a sprite that is not shown costs one byte to
// hide and a sprite that is costs twenty to place, so a growing cut needs no
// part of the screen under it saved, restored, or levelled between pages.
#define MSX2_SPR_SLASH_SEGS  4
// Sprites 3..10.  0..2 are the burst, which plays over the top of this, and the
// three at the end belong to the selector.
#define MSX2_SPR_SLASH_ID    3
void Msx2_SpriteSlash(u8 x, u8 y, u8 grown_a, u8 grown_b, u8 core);
void Msx2_SpriteSlashHide(void);

// Cover one battle card with opaque black 32x32 sprite tiles.  `which` is 0
// for the left lane and 1 for the right lane; step is 0..6.
void Msx2_SpriteBurnCard(u8 which, u8 x, u8 y, u8 step);
void Msx2_SpriteBurnHide(void);

// The selector, at (x, y) -- the top-left of the gem, not of the sprite cell --
// spinning at `frame`.  `color` is MSX2_SPR_RED or MSX2_SPR_TEAL; the shadow
// and highlight tints come with it.  Call it every frame.
void Msx2_SpriteGem(u8 x, u8 y, u8 frame, u8 color);

// Take the selector off the screen, all of its planes.
void Msx2_SpriteHideGem(void);
