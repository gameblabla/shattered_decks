// ─────────────────────────────────────────────────────────────────────────────
//  waifu_msx2_ascii_s7_b1.c — ASCII16-X rules bank
// ─────────────────────────────────────────────────────────────────────────────

#ifdef MSX2_ASCII16X

// The duel model, its deck helpers, AI and card tables are mutually
// self-contained.  Keeping those bodies together in one 16 KiB page-2 bank
// makes the fixed page small enough for the ASCII16-X contract without
// copying code into RAM or splitting rules operations across banks.
#define waifu_deck_rng_seed             waifu_deck_rng_seed_In
#define waifu_deck_rng_next             waifu_deck_rng_next_In
#define waifu_deck_clear                waifu_deck_clear_In
#define waifu_deck_remaining            waifu_deck_remaining_In
#define waifu_deck_draw                 waifu_deck_draw_In
#define waifu_deck_shuffle              waifu_deck_shuffle_In
#define waifu_deck_build_random         waifu_deck_build_random_In
#define waifu_deck_build_opponent_story waifu_deck_build_opponent_story_In
#include "../game/deck.c"
#include "../game/ai.c"
#include "msx2_cards.c"
#include "msx2_duel.c"
#undef waifu_deck_build_opponent_story
#undef waifu_deck_build_random
#undef waifu_deck_shuffle
#undef waifu_deck_draw
#undef waifu_deck_remaining
#undef waifu_deck_clear
#undef waifu_deck_rng_next
#undef waifu_deck_rng_seed

#endif
