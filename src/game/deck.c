#include "deck.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static int deck_card_is_valid(int card)
{
    return card >= 0 && card < WAIFU_CARD_COUNT + WAIFU_SUPPORT_CARD_VARIANTS;
}

static int deck_card_copy_count(const WaifuDeck *deck, int card)
{
    int n = 0;
    if (!deck) return 0;
    for (int i = 0; i < deck->count; ++i) if (deck->cards[i] == card) ++n;
    return n;
}

static int deck_append_limited(WaifuDeck *deck, int card, int max_copies)
{
    if (!deck || deck->count >= WAIFU_DECK_SIZE || !deck_card_is_valid(card)) return 0;
    if (max_copies > 0 && deck_card_copy_count(deck, card) >= max_copies) return 0;
    deck->cards[deck->count++] = card;
    return 1;
}

uint32_t waifu_deck_runtime_seed(uint32_t salt)
{
    uintptr_t stack_mix = (uintptr_t)&salt;
    uint32_t s = salt ^ (uint32_t)time(NULL) ^ ((uint32_t)clock() << 11);
    s ^= (uint32_t)(stack_mix >> 4);
    s ^= (uint32_t)(stack_mix >> 19);
    if (s == 0u) s = 0x6d2b79f5u;
    return s;
}

void waifu_deck_rng_seed(WaifuDeckRng *rng, uint32_t seed)
{
    if (!rng) return;
    if (seed == 0u) seed = 0x9e3779b9u;
    rng->state = seed;
}

uint32_t waifu_deck_rng_next(WaifuDeckRng *rng)
{
    uint32_t x;
    if (!rng) return 0u;
    x = rng->state;
    if (x == 0u) x = 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x ? x : 0x6d2b79f5u;
    return rng->state;
}

void waifu_deck_clear(WaifuDeck *deck)
{
    if (!deck) return;
    for (int i = 0; i < WAIFU_DECK_SIZE; ++i) deck->cards[i] = WAIFU_DECK_NONE;
    deck->count = 0;
    deck->pos = 0;
}

int waifu_deck_remaining(const WaifuDeck *deck)
{
    if (!deck) return 0;
    if (deck->pos < 0) return deck->count;
    if (deck->pos > deck->count) return 0;
    return deck->count - deck->pos;
}

int waifu_deck_draw(WaifuDeck *deck)
{
    if (!deck || deck->pos < 0 || deck->pos >= deck->count) return WAIFU_DECK_NONE;
    return deck->cards[deck->pos++];
}

void waifu_deck_shuffle(WaifuDeck *deck, WaifuDeckRng *rng)
{
    if (!deck || !rng) return;
    for (int i = deck->count - 1; i > 0; --i) {
        int j = (int)(waifu_deck_rng_next(rng) % (uint32_t)(i + 1));
        int tmp = deck->cards[i];
        deck->cards[i] = deck->cards[j];
        deck->cards[j] = tmp;
    }
    deck->pos = 0;
}

void waifu_deck_build_random(WaifuDeck *deck, WaifuDeckRng *rng, int strength_bias)
{
    static const int strong_pool[] = {37, 40, 33, 28, 8, 14, 48, 49, 54, 55, 56, 44, 27};
    static const int mid_pool[] = {0, 2, 5, 9, 10, 11, 16, 17, 18, 21, 24, 25, 26, 30, 32, 38, 39, 41, 42, 45, 46, 47, 50, 51, 52, 53, 57};
    static const int weak_pool[] = {6, 7, 12, 15, 19, 20, 22, 23, 29, 34, 35, 43};
    int guard = 0;

    waifu_deck_clear(deck);
    if (!rng) return;

    while (deck->count < WAIFU_DECK_SIZE && guard++ < 5000) {
        uint32_t roll = waifu_deck_rng_next(rng) % 100u;
        int card;
        if (roll < 16u) {
            card = WAIFU_CARD_COUNT + (int)(waifu_deck_rng_next(rng) % WAIFU_SUPPORT_STANDARD_CARD_VARIANTS);
        } else if (roll < (uint32_t)(strength_bias ? 50 : 34)) {
            card = strong_pool[waifu_deck_rng_next(rng) % (uint32_t)(sizeof(strong_pool) / sizeof(strong_pool[0]))];
        } else if (roll < 82u) {
            card = mid_pool[waifu_deck_rng_next(rng) % (uint32_t)(sizeof(mid_pool) / sizeof(mid_pool[0]))];
        } else {
            card = weak_pool[waifu_deck_rng_next(rng) % (uint32_t)(sizeof(weak_pool) / sizeof(weak_pool[0]))];
        }
        (void)deck_append_limited(deck, card, 4);
    }

    guard = 0;
    while (deck->count < WAIFU_DECK_SIZE && guard++ < 5000) {
        int card = (int)(waifu_deck_rng_next(rng) % (uint32_t)WAIFU_CARD_COUNT);
        (void)deck_append_limited(deck, card, 4);
    }
    for (int card = 0; deck->count < WAIFU_DECK_SIZE && card < WAIFU_CARD_COUNT; ++card) {
        while (deck->count < WAIFU_DECK_SIZE && deck_append_limited(deck, card, 4)) {
            ;
        }
    }
    waifu_deck_shuffle(deck, rng);
}

void waifu_deck_build_from_list(WaifuDeck *deck, const int *cards, int count, WaifuDeckRng *rng, int shuffle)
{
    waifu_deck_clear(deck);
    if (!deck || !cards) return;
    if (count > WAIFU_DECK_SIZE) count = WAIFU_DECK_SIZE;
    for (int i = 0; i < count; ++i) {
        if (deck_card_is_valid(cards[i])) deck->cards[deck->count++] = cards[i];
    }
    if (shuffle && rng) waifu_deck_shuffle(deck, rng);
}

static int opponent_story_card_at(int duel, int pos)
{
    static const int dream[]    = {20, 23, 29, 34, 19, 30, 6, 43, 45, 50, 51, 53};
    static const int plaza[]    = {12, 15, 22, 29, 36, 6, 19, 23, 30, 34, 50, 52, 53};
    static const int adept[]    = {28, 33, 37, 15, 22, 40, 12, 36, 8, 14, 49, 54, 55};
    static const int reaver[]   = {37, 40, 8, 14, 28, 33, 12, 36, 22, 15, 44, 51, 52, 57};
    static const int burning[]  = {8, 37, 40, 14, 28, 33, 36, 22, 12, 15, 44, 47, 51};
    static const int void_w[]   = {40, 37, 8, 28, 14, 33, 36, 22, 12, 15, 47, 54, 57};
    static const int sphinx[]   = {33, 37, 40, 28, 14, 8, 36, 22, 12, 15, 48, 49, 55, 56};
    static const int demon[]    = {8, 37, 40, 28, 14, 33, 36, 22, 12, 15, 44, 47, 49, 57};
    const int *pool = dream;
    int count = (int)(sizeof(dream) / sizeof(dream[0]));
    if (duel == 1) { pool = plaza; count = (int)(sizeof(plaza) / sizeof(plaza[0])); }
    else if (duel == 2) { pool = adept; count = (int)(sizeof(adept) / sizeof(adept[0])); }
    else if (duel == 3) { pool = reaver; count = (int)(sizeof(reaver) / sizeof(reaver[0])); }
    else if (duel == 4) { pool = burning; count = (int)(sizeof(burning) / sizeof(burning[0])); }
    else if (duel == 5) { pool = void_w; count = (int)(sizeof(void_w) / sizeof(void_w[0])); }
    else if (duel == 6) { pool = sphinx; count = (int)(sizeof(sphinx) / sizeof(sphinx[0])); }
    else if (duel >= 7) { pool = demon; count = (int)(sizeof(demon) / sizeof(demon[0])); }
    return pool[pos % count];
}

void waifu_deck_build_opponent_story(WaifuDeck *deck, int duel_index, WaifuDeckRng *rng, int shuffle)
{
    waifu_deck_clear(deck);
    for (int i = 0; i < WAIFU_DECK_SIZE; ++i) {
        int card = opponent_story_card_at(duel_index, i);
        if (duel_index >= 3 && i == 4) card = WAIFU_SUPPORT_THUNDER_CARD_ID;
        else if (duel_index >= 2 && ((i + 1) % 7) == 0) card = WAIFU_SUPPORT_EQUIP_CARD_ID;
        deck->cards[deck->count++] = card;
    }
    if (shuffle && rng) waifu_deck_shuffle(deck, rng);
}

#ifdef WAIFU_FM_HEADLESS_TESTS
void waifu_deck_build_headless_battle(WaifuDeck *deck, const int opening[5], int seed)
{
    WaifuDeckRng rng;
    waifu_deck_clear(deck);
    if (!opening) return;
    for (int i = 0; i < 5 && deck->count < WAIFU_DECK_SIZE; ++i) deck->cards[deck->count++] = opening[i];
    waifu_deck_rng_seed(&rng, (uint32_t)(seed ? seed : 17));
    while (deck->count < WAIFU_DECK_SIZE) {
        int card = (int)(waifu_deck_rng_next(&rng) % (uint32_t)WAIFU_CARD_COUNT);
        deck->cards[deck->count++] = card;
    }
    deck->pos = 5;
}
#endif
