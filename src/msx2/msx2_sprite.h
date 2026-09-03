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

// Sprite colours, as palette indices.  Index 0 is transparent on a sprite.
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

// The sprites the selector uses: the last MSX2_GEM_PLANES of the thirty-two,
// so they are behind everything the effects put up (the V9938 gives the lower
// id the higher priority) and can never take one of the eight-per-line slots a
// burst wants.  The planes are disjoint masks of one solid, so stacking them
// costs nothing but attribute bytes.
#define MSX2_SPR_CURSOR  (32 - MSX2_GEM_PLANES)

void Msx2_SpriteInit(void);

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

// The selector, at (x, y) -- the top-left of the gem, not of the sprite cell --
// spinning at `frame`.  `color` is MSX2_SPR_RED or MSX2_SPR_TEAL; the shadow
// and highlight tints come with it.  Call it every frame.
void Msx2_SpriteGem(u8 x, u8 y, u8 frame, u8 color);

// Take the selector off the screen, all of its planes.
void Msx2_SpriteHideGem(void);
