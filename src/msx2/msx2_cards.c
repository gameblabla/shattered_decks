// ─────────────────────────────────────────────────────────────────────────────
//  msx2_cards.c — the one translation unit that emits the card stat tables.
// ─────────────────────────────────────────────────────────────────────────────
#define MSX2_CARD_TABLES_IMPL
#include "msx2_cards.h"

// ... and the small retained-view tables, for the same reason.  This is the one
// translation unit that defines them instead of duplicating them in every file
// that includes msx2_scenes.h.  ASCII16-X keeps these scene tables beside the
// board bank; its rules bank only needs the card-stat columns below.
#ifndef MSX2_ASCII16X
#include "msx2_scenes.h"
#include "msx2_scene_geometry.h"
#endif

// The lVGM asset tables used to live here.  They are in the boot bank now
// (src/msx2/msx2_audio_probe.c): three chip tables of ten records each is 120
// bytes of a _CODE area that has tens left, and the resident player only ever
// needs the ONE table its machine resolved to, which Msx2_AudioInit() copies
// into RAM at boot.
