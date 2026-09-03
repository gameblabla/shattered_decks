// ─────────────────────────────────────────────────────────────────────────────
//  msx2_cards.c — the one translation unit that emits the card stat tables.
// ─────────────────────────────────────────────────────────────────────────────
#define MSX2_CARD_TABLES_IMPL
#include "msx2_cards.h"

// ... and the board geometry, for the same reason.  msx2_scenes.h only DECLARES
// the four projected-slot tables: it is included by a dozen files, and a
// `static const` there was a dozen copies of nearly a kilobyte in a 32 KB code
// area.  This is the one translation unit that defines them.
#include "msx2_scenes.h"
#include "msx2_scene_geometry.h"
