// ─────────────────────────────────────────────────────────────────────────────
//  msx2_cards.h — card stat tables for the MSX2 fork
//
//  The framebuffer targets read ATK/DEF/attribute/tribe out of the 5.3 MB
//  src/generated/waifu_assets.h.  The Z80 build lifts the same four columns
//  into 432 bytes of ROM via tools/msx2/gen_msx_tables.py; this header is the
//  only place that generated file is referenced.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "msxgl.h"
#include "msx2_card_tables.h"

// Support cards live above the monster id range, exactly as in the shared code
// (deck.h's WAIFU_SUPPORT_*_CARD_ID).
#define MSX2_SUPPORT_VARIANTS   6
#define MSX2_TOTAL_CARDS        (MSX2_CARD_COUNT + MSX2_SUPPORT_VARIANTS)
