#include "deck.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "deck_pools.h"

static uint32_t deck_rotl32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32u - n));
}

static uint32_t deck_mix32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

#if defined(WAIFU_FM_PCFX)
static uint32_t pcfx_entropy_seed_material(void)
{
    uint32_t vce = 0u;
    uint32_t timer = 0u;
#if defined(__v810__)
    __asm__ volatile (
        "in.h 0x300[r0],%[vce]\n"
        "in.h 0xfc0[r0],%[timer]\n"
        : [vce] "=r" (vce), [timer] "=r" (timer)
        :
        : "memory");
#endif
    return (vce & 0xffffu) ^ deck_rotl32(timer & 0xffffu, 7);
}
#endif

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
#if defined(WAIFU_FM_PCFX)
    s ^= pcfx_entropy_seed_material();
    s = deck_rotl32(s, 7) ^ pcfx_entropy_seed_material();
#endif
    s = deck_mix32(s);
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
    int guard = 0;

    waifu_deck_clear(deck);
    if (!rng) return;
    for (int i = 0; i < 3; ++i) {
        (void)deck_append_limited(deck, WAIFU_SUPPORT_THUNDER_CARD_ID, 3);
    }

    while (deck->count < WAIFU_DECK_SIZE && guard++ < 5000) {
        uint32_t roll = waifu_deck_rng_next(rng) % 100u;
        int card;
        if (roll < 16u) {
            card = WAIFU_CARD_COUNT + (int)(waifu_deck_rng_next(rng) % WAIFU_SUPPORT_STANDARD_CARD_VARIANTS);
        } else if (roll < (uint32_t)(strength_bias ? 50 : 34)) {
            card = waifu_random_strong_pool[waifu_deck_rng_next(rng) % (uint32_t)WAIFU_RANDOM_STRONG_POOL_COUNT];
        } else if (roll < 82u) {
            card = waifu_random_mid_pool[waifu_deck_rng_next(rng) % (uint32_t)WAIFU_RANDOM_MID_POOL_COUNT];
        } else {
            card = waifu_random_weak_pool[waifu_deck_rng_next(rng) % (uint32_t)WAIFU_RANDOM_WEAK_POOL_COUNT];
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
    int pool_index = duel;
    const int *pool;
    int count;
    if (pool_index < 0) pool_index = 0;
    if (pool_index >= (int)(sizeof(waifu_opponent_story_pools) / sizeof(waifu_opponent_story_pools[0]))) {
        pool_index = (int)(sizeof(waifu_opponent_story_pools) / sizeof(waifu_opponent_story_pools[0])) - 1;
    }
    pool = waifu_opponent_story_pools[pool_index];
    count = waifu_opponent_story_pool_counts[pool_index];
    return pool[pos % count];
}

void waifu_deck_build_opponent_story(WaifuDeck *deck, int duel_index, WaifuDeckRng *rng, int shuffle)
{
    waifu_deck_clear(deck);
    for (int i = 0; i < WAIFU_DECK_SIZE; ++i) {
        int card = opponent_story_card_at(duel_index, i);
        if (duel_index >= 3 && (i == 2 || i == 4 || i == 8)) card = WAIFU_SUPPORT_THUNDER_CARD_ID;
        else if (duel_index >= 2 && ((i + 1) % 7) == 0) card = WAIFU_SUPPORT_EQUIP_CARD_ID;
        deck->cards[deck->count++] = card;
    }
    if (shuffle && rng) waifu_deck_shuffle(deck, rng);
    if (duel_index >= 4 && deck->count >= 5) {
        int opening_has_thunder = 0;
        int thunder_pos = -1;
        for (int i = 0; i < deck->count; ++i) {
            if (deck->cards[i] != WAIFU_SUPPORT_THUNDER_CARD_ID) continue;
            if (i < 5) opening_has_thunder = 1;
            else if (thunder_pos < 0) thunder_pos = i;
        }
        if (!opening_has_thunder && thunder_pos >= 0) {
            int tmp = deck->cards[0];
            deck->cards[0] = deck->cards[thunder_pos];
            deck->cards[thunder_pos] = tmp;
        }
    }
}

#ifdef WAIFU_FM_HEADLESS_TESTS
void waifu_deck_build_headless_battle(WaifuDeck *deck, const int opening[5], int seed)
{
    WaifuDeckRng rng;
    waifu_deck_clear(deck);
    if (!opening) return;
    for (int i = 0; i < 5 && deck->count < WAIFU_DECK_SIZE; ++i) deck->cards[deck->count++] = opening[i];
    for (int i = 0; i < 3 && deck->count < WAIFU_DECK_SIZE; ++i) {
        deck->cards[deck->count++] = WAIFU_SUPPORT_THUNDER_CARD_ID;
    }
    waifu_deck_rng_seed(&rng, (uint32_t)(seed ? seed : 17));
    while (deck->count < WAIFU_DECK_SIZE) {
        int card = (int)(waifu_deck_rng_next(&rng) % (uint32_t)WAIFU_CARD_COUNT);
        deck->cards[deck->count++] = card;
    }
    deck->pos = 5;
}
#endif
