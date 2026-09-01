// ─────────────────────────────────────────────────────────────────────────────
//  msx2_raster.h — the scoped 3D card rasterizer (MSX2_PORT_PLAN.md §8)
//
//  The duel board is a picture captured from the game's own renderer.  The
//  cards on it are not: each one is mapped into its true projected quad, so the
//  fifteen cards lie on a board that recedes away from the player instead of
//  standing in a grid of upright rectangles.
//
//  This is §8's Tier A and only Tier A.  The quads are known offline -- the
//  camera poses are authored, so the projection was already done at bake time
//  -- and what ships in the cartridge is a *span program* per (view, slot):
//
//      dy, x0, src_lo, src_hi, mirror, <COPY n | DUP n | ADV n>..., ENDROW
//      ...
//      END
//
//  The interpreter below therefore does no arithmetic of any kind.  A COPY is a
//  block of texels straight from RAM to the VDP data port, a DUP is the same
//  byte pushed n times (which is how magnification costs nothing), and an ADV
//  skips texels where the quad is smaller than the texture.  turbor's rule from
//  the SandStone notes, taken to its limit: if you do not need to calculate
//  something, do not calculate it.
//
//  A row whose texels run backwards -- every card on the COM side, which is
//  rotated 180 degrees on the board plane -- reads the MIRRORED copy of the
//  texture forwards instead.  One extra blob of card art buys that; a reverse
//  block copy would have been a second inner loop earning nothing else.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"

// Forget which texture and which program are cached.  Called on entry to the
// duel screen; without it the "already loaded" test would believe card 0 was
// resident before anything had been read at all.
void Msx2_RasterInit(void);

// Pull one card texture and one slot's span program in from the cartridge.
// Both are RAM-resident for the draw: that is what keeps the inner loop free of
// bank switching, and it is why the two buffers are worth their four kilobytes.
void Msx2_RasterLoad(u8 card_index, u8 slot);

// Replay the loaded program onto the draw page.  Roughly two frames of VDP time
// for one card, paid when a card ARRIVES rather than every frame -- the board
// keeps what it is showing, so a settled screen costs nothing at all.
void Msx2_RasterDraw(void);

// The two together, which is what every caller actually wants.
void Msx2_RasterCard(u8 card_index, u8 slot);
