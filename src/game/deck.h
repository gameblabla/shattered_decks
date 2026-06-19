#ifndef WAIFU_FM_DECK_H
#define WAIFU_FM_DECK_H

#include <stdint.h>
#include "waifu_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAIFU_DECK_SIZE 40
#define WAIFU_SUPPORT_CARD_VARIANTS 4
#define WAIFU_SUPPORT_EQUIP_CARD_ID (WAIFU_CARD_COUNT + 0)
#define WAIFU_SUPPORT_GUARD_CARD_ID (WAIFU_CARD_COUNT + 1)
#define WAIFU_SUPPORT_DRAW_CARD_ID  (WAIFU_CARD_COUNT + 2)
#define WAIFU_SUPPORT_HEAL_CARD_ID  (WAIFU_CARD_COUNT + 3)
#define WAIFU_DECK_NONE (-1)

typedef struct WaifuDeck {
    int cards[WAIFU_DECK_SIZE];
    int count;
    int pos;
} WaifuDeck;

typedef struct WaifuDeckRng {
    uint32_t state;
} WaifuDeckRng;

uint32_t waifu_deck_runtime_seed(uint32_t salt);
void waifu_deck_rng_seed(WaifuDeckRng *rng, uint32_t seed);
uint32_t waifu_deck_rng_next(WaifuDeckRng *rng);
void waifu_deck_clear(WaifuDeck *deck);
int waifu_deck_remaining(const WaifuDeck *deck);
int waifu_deck_draw(WaifuDeck *deck);
void waifu_deck_shuffle(WaifuDeck *deck, WaifuDeckRng *rng);
void waifu_deck_build_random(WaifuDeck *deck, WaifuDeckRng *rng, int strength_bias);
void waifu_deck_build_from_list(WaifuDeck *deck, const int *cards, int count, WaifuDeckRng *rng, int shuffle);
void waifu_deck_build_opponent_story(WaifuDeck *deck, int duel_index, WaifuDeckRng *rng, int shuffle);

#ifdef WAIFU_FM_HEADLESS_TESTS
void waifu_deck_build_headless_battle(WaifuDeck *deck, const int opening[5], int seed);
#endif

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_DECK_H */
