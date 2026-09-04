// ─────────────────────────────────────────────────────────────────────────────
//  msx2_raster.h — the live affine card mapper
//
//  The duel board is the game's board, and the cards on it are not upright
//  rectangles: each one is mapped into its true projected quad, so fifteen
//  cards lie on a floor that recedes away from the player.
//
//  This USED to be a baked span program per (view, slot) -- 120 rasterisations
//  of the card texture into a fixed quad, 240 KB of cartridge, replayed by an
//  interpreter that did no arithmetic at all.  That was the right trade while
//  the board was a photograph: the quads could only ever be the three captured
//  ones, so baking them cost nothing that was not already spent.
//
//  The board is drawn now (msx2_arena.h), so a quad is no longer a fixed thing
//  and 240 KB of programs would still only cover three of the poses it can be
//  drawn at.  What runs instead is an ordinary affine scanline mapper: two edge
//  DDAs down the quad's sides, one texture row per destination row, and a
//  texel-stepping inner loop that pushes bytes at the VDP data port.
//
//  It is not slower.  GRAPHIC 7 needs 32 T-states between two writes to the
//  data port while the display is on, and the whole DDA -- fetch, output, step
//  the fractional source pointer, loop -- is about sixty.  A projected slot is
//  some 30x15 pixels, so a card is about eight milliseconds either way, paid
//  when a card ARRIVES rather than every frame.
//
//  It also drops the mirrored texture set.  A row whose texels run backwards --
//  every card on the COM side, which the shared renderer rotates 180 degrees on
//  the board plane -- used to read a second, pre-mirrored copy of the card blob
//  forwards, because the interpreter's COPY was a forward block move and could
//  not do anything else.  The DDA can step its source pointer down as easily as
//  up, so that copy is gone too.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Forget which texture is cached.  Called on entry to the duel screen; without
// it the "already loaded" test would believe card 0 was resident before
// anything had been read at all.
void Msx2_RasterInit(void);

// Select the camera geometry used by subsequent field-card draws.
// MSX2_VIEW_TOP is the player's tactical view; MSX2_VIEW_COM is the same board
// from the opponent's chair.
void Msx2_RasterSetView(u8 view);

// Read one card texture in from the cartridge and map it into one slot's
// projected quad on the draw page.  The texture is RAM-resident for the draw,
// which is what keeps the inner loop free of bank switching and is why the
// buffer is worth its 1,920 bytes; a repaint of the same card skips the read.
// `defense` selects the pre-turned 48x40 texture set -- the position a real
// table states by turning the card, and this port used to state with the word
// "DEF" printed over it.
void Msx2_RasterCard(u8 card_index, u8 slot, u8 defense);
