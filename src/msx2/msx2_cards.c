// ─────────────────────────────────────────────────────────────────────────────
//  msx2_cards.c — the one translation unit that emits the card stat tables.
// ─────────────────────────────────────────────────────────────────────────────
#define MSX2_CARD_TABLES_IMPL
#include "msx2_cards.h"

// ... and the small retained-view tables, for the same reason.  This is the one
// translation unit that defines them instead of duplicating them in every file
// that includes msx2_scenes.h.
#include "msx2_scenes.h"
#include "msx2_scene_geometry.h"

// The lVGM streams are packed by gen_msx_scenes.py after the text asset.  Keep
// their small lookup table in this existing data translation unit rather than
// making every user of msx2_scenes.h carry a private copy.
const Msx2MusicAsset g_msx2_music_assets[MSX2_MUSIC_ASSET_COUNT] = {
	{ 0, MSX2_MUSIC_NONE_SEGMENTS, MSX2_MUSIC_NONE_LOOP },
	{ MSX2_MUSIC_TITLE_SEGMENT, MSX2_MUSIC_TITLE_SEGMENTS, MSX2_MUSIC_TITLE_LOOP },
	{ MSX2_MUSIC_OPENING_SEGMENT, MSX2_MUSIC_OPENING_SEGMENTS, MSX2_MUSIC_OPENING_LOOP },
	{ MSX2_MUSIC_OVERWORLD_SEGMENT, MSX2_MUSIC_OVERWORLD_SEGMENTS, MSX2_MUSIC_OVERWORLD_LOOP },
	{ MSX2_MUSIC_DECK_EDITOR_SEGMENT, MSX2_MUSIC_DECK_EDITOR_SEGMENTS, MSX2_MUSIC_DECK_EDITOR_LOOP },
	{ MSX2_MUSIC_BATTLE_SEGMENT, MSX2_MUSIC_BATTLE_SEGMENTS, MSX2_MUSIC_BATTLE_LOOP },
	{ MSX2_MUSIC_BOSS_SEGMENT, MSX2_MUSIC_BOSS_SEGMENTS, MSX2_MUSIC_BOSS_LOOP },
	{ MSX2_MUSIC_FINAL_BOSS_SEGMENT, MSX2_MUSIC_FINAL_BOSS_SEGMENTS, MSX2_MUSIC_FINAL_BOSS_LOOP },
	{ MSX2_MUSIC_RESULT_SEGMENT, MSX2_MUSIC_RESULT_SEGMENTS, MSX2_MUSIC_RESULT_LOOP },
	{ MSX2_MUSIC_LOST_SEGMENT, MSX2_MUSIC_LOST_SEGMENTS, MSX2_MUSIC_LOST_LOOP },
};
