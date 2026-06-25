#include "assets.h"

#include <stddef.h>
#include <string.h>
#ifndef WAIFU_ASSET_NO_STDIO
#include <stdio.h>
#endif

#if defined(WAIFU_ASSET_USE_CDROM)
#define WAIFU_ASSET_EXTERNAL_TITLE_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS 1
#define WAIFU_ASSET_EXTERNAL_CARD_IMAGES 1
#endif

#include "waifu_assets.h"
#include "title_asset.h"

#define WAIFU_ASSET_KIND_COMPILED 0
#define WAIFU_ASSET_KIND_CART_ROM 1
#define WAIFU_ASSET_KIND_CDROM 2

#if defined(WAIFU_ASSET_USE_CDROM)
#define WAIFU_ASSET_ACTIVE_BACKEND WAIFU_ASSET_KIND_CDROM
#elif defined(WAIFU_ASSET_USE_CART_ROM)
#define WAIFU_ASSET_ACTIVE_BACKEND WAIFU_ASSET_KIND_CART_ROM
#else
#define WAIFU_ASSET_ACTIVE_BACKEND WAIFU_ASSET_KIND_COMPILED
#endif

#ifndef WAIFU_ASSET_RAM_BUDGET
#if defined(WAIFU_FM_PCFX)
/* PC-FX has enough main RAM to keep the full monster big-art cache resident.
   The old 512 KiB budget forced a 16-slot LRU cache, which caused CD misses
   during repeated duel/card-preview paths. */
#define WAIFU_ASSET_RAM_BUDGET (2u * 1024u * 1024u)
#else
#define WAIFU_ASSET_RAM_BUDGET (512u * 1024u)
#endif
#endif

#define TITLE_BYTES ((size_t)TITLE_SCREEN_W * (size_t)TITLE_SCREEN_H)
#define TITLE_64K_BYTES (TITLE_BYTES * 2u)
#define TITLE_16M_BYTES (TITLE_BYTES * 2u)
#define TITLE_TOTAL_BYTES (TITLE_BYTES + TITLE_16M_BYTES)
#define CARD_FACE_BYTES ((size_t)WAIFU_CARD_COUNT * (size_t)WAIFU_CARD_W * (size_t)WAIFU_CARD_H)
#define CARD_ONE_BYTES ((size_t)WAIFU_CARD_W * (size_t)WAIFU_CARD_H)
#define CARD_BIG_ONE_BYTES ((size_t)WAIFU_BIG_W * (size_t)WAIFU_BIG_H)
#define PCFX_CD_SECTOR_BYTES 2048u
#define CARD_BIG_CD_SLOT_BYTES (((CARD_BIG_ONE_BYTES + (size_t)PCFX_CD_SECTOR_BYTES - 1u) / (size_t)PCFX_CD_SECTOR_BYTES) * (size_t)PCFX_CD_SECTOR_BYTES)
#define CARD_BIG_CACHE_SLOT_BYTES CARD_BIG_ONE_BYTES
#define CARD_BIG_FACE_BYTES ((size_t)WAIFU_CARD_COUNT * CARD_BIG_ONE_BYTES)

static WaifuBigArtDraw g_big_art_draws[WAIFU_ASSET_BIG_ART_DRAW_MAX];
static int g_big_art_draw_count = 0;

#if defined(WAIFU_FM_HEADLESS_TESTS)
static unsigned long g_debug_platform_read_count = 0;
#endif

#ifndef WAIFU_ASSET_NO_STDIO
static const char *blob_path(WaifuAssetBlobId blob)
{
    switch (blob) {
    case WAIFU_ASSET_BLOB_TITLE_SCREEN: return "assets/generated/title_screen_img.bin";
    case WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV16: return "assets/generated/title_screen_pcfx_yuv16.bin";
    case WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV422: return "assets/generated/title_screen_pcfx_yuv422.bin";
    case WAIFU_ASSET_BLOB_STORY_PORTRAITS: return "assets/generated/story_portraits.bin";
    case WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK: return "assets/generated/story_portrait_mask.bin";
    case WAIFU_ASSET_BLOB_CARD_FACES: return "assets/generated/card_faces.bin";
    case WAIFU_ASSET_BLOB_CARD_BIG_ART: return "assets/generated/card_big_art.bin";
    case WAIFU_ASSET_BLOB_CARD_BIG_ART_CD: return "assets/generated/card_big_art_cd.bin";
    case WAIFU_ASSET_BLOB_CARD_BACK: return "assets/generated/card_back.bin";
    case WAIFU_ASSET_BLOB_SUPPORT_FACE: return "assets/generated/support_face.bin";
    case WAIFU_ASSET_BLOB_SUPPORT_BIG_ART: return "assets/generated/support_big_art.bin";
    case WAIFU_ASSET_BLOB_SUPPORT_BIG_ART_CD: return "assets/generated/support_big_art_cd.bin";
    default: return NULL;
    }
}
#endif

#if !defined(WAIFU_FM_PCFX)
int waifu_assets_platform_read_blob_slice(WaifuAssetBlobId blob, void *dst, size_t offset, size_t bytes)
{
#ifdef WAIFU_ASSET_NO_STDIO
    (void)blob; (void)dst; (void)offset; (void)bytes;
    return 0;
#else
    const char *path = blob_path(blob);
    FILE *fp;
    size_t got;
    if (!path || !dst) return 0;
#if defined(WAIFU_FM_HEADLESS_TESTS)
    ++g_debug_platform_read_count;
#endif
    fp = fopen(path, "rb");
    if (!fp) return 0;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) { fclose(fp); return 0; }
    got = fread(dst, 1, bytes, fp);
    if (got < bytes && feof(fp)) {
        /* Host CD-ROM emulation sometimes requests a sector-rounded slice from
           a file whose stored payload is not sector padded.  Real PC-FX CD reads
           can safely over-read into the next sector; for stdio tests, zero-fill
           the harmless tail so the same staged-load path can be validated. */
        memset((uint8_t *)dst + got, 0, bytes - got);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return got == bytes;
#endif
}
#endif

static int read_blob_slice_platform(WaifuAssetBlobId blob, uint8_t *dst, size_t offset, size_t bytes)
{
    return waifu_assets_platform_read_blob_slice(blob, dst, offset, bytes);
}

static int read_blob_platform(WaifuAssetBlobId blob, uint8_t *dst, size_t bytes)
{
    return read_blob_slice_platform(blob, dst, 0, bytes);
}

#if defined(WAIFU_FM_HEADLESS_TESTS)
unsigned long waifu_assets_debug_platform_read_count(void)
{
    return g_debug_platform_read_count;
}

void waifu_assets_debug_reset_platform_read_count(void)
{
    g_debug_platform_read_count = 0;
}
#endif

void waifu_assets_big_art_draw_queue_reset(void)
{
    g_big_art_draw_count = 0;
}

void waifu_assets_note_big_art_draw(WaifuBigArtKind kind, int card_id, int x, int y)
{
    if (g_big_art_draw_count >= WAIFU_ASSET_BIG_ART_DRAW_MAX) return;
    if (x < 0 || y < 0 || x + WAIFU_BIG_W > 256 || y + WAIFU_BIG_H > 240) return;
    g_big_art_draws[g_big_art_draw_count].kind = kind;
    g_big_art_draws[g_big_art_draw_count].card_id = card_id;
    g_big_art_draws[g_big_art_draw_count].x = x;
    g_big_art_draws[g_big_art_draw_count].y = y;
    ++g_big_art_draw_count;
}

int waifu_assets_big_art_draw_count(void)
{
    return g_big_art_draw_count;
}

const WaifuBigArtDraw *waifu_assets_big_art_draws(void)
{
    return g_big_art_draws;
}

int waifu_assets_big_art_blob_slice(WaifuBigArtKind kind, int card_id, WaifuAssetBlobSlice *out)
{
    if (!out) return 0;
    if (kind == WAIFU_BIG_ART_SUPPORT) {
        out->blob = WAIFU_ASSET_BLOB_SUPPORT_BIG_ART;
        out->offset = 0;
        out->bytes = CARD_BIG_ONE_BYTES;
        return 1;
    }
    if (card_id < 0) card_id = 0;
    if (card_id >= WAIFU_CARD_COUNT) card_id = WAIFU_CARD_COUNT - 1;
    out->blob = WAIFU_ASSET_BLOB_CARD_BIG_ART;
    out->offset = (size_t)card_id * CARD_BIG_ONE_BYTES;
    out->bytes = CARD_BIG_ONE_BYTES;
    return 1;
}
#define CARD_EXTRA_BYTES (CARD_ONE_BYTES * 2u)
#define CARDS_TOTAL_BYTES (CARD_FACE_BYTES + CARD_EXTRA_BYTES)
#ifndef WAIFU_ASSET_BIG_CACHE_SLOTS
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
/* Keep every monster's 112x112 big art resident.  Runtime prewarm still filters
   duplicates and support cards, so battle loads read the unique cards from both
   decks once and later duels reuse the cache instead of thrashing a small LRU. */
#define WAIFU_ASSET_BIG_CACHE_SLOTS WAIFU_CARD_COUNT
#else
#define WAIFU_ASSET_BIG_CACHE_SLOTS 16
#endif
#endif
#define PORTRAIT_ONE_BYTES ((size_t)WAIFU_STORY_PORTRAIT_W * (size_t)WAIFU_STORY_PORTRAIT_H)
#define PORTRAIT_SLOT_BYTES (PORTRAIT_ONE_BYTES * 2u)
#define PORTRAIT_SLOT_COUNT 2

#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
#define STORY_TWO_PORTRAIT_BYTES ((size_t)PORTRAIT_SLOT_COUNT * PORTRAIT_SLOT_BYTES)
#define CARD_BIG_CACHE_BYTES ((size_t)WAIFU_ASSET_BIG_CACHE_SLOTS * CARD_BIG_CACHE_SLOT_BYTES + CARD_BIG_CACHE_SLOT_BYTES)
#define ASSET_STAGE_A_BYTES ((CARDS_TOTAL_BYTES > STORY_TWO_PORTRAIT_BYTES) ? CARDS_TOTAL_BYTES : STORY_TWO_PORTRAIT_BYTES)
#if defined(WAIFU_FM_PCFX)
/* PC-FX title pixels are CD/SCSI-DMA'd directly into KING KRAM.  Do not reserve
   title staging RAM; the title framebuffer is never used as a CPU-resident
   working set on PC-FX. */
#define ASSET_BASE_STAGE_BYTES ASSET_STAGE_A_BYTES
#else
#define ASSET_BASE_STAGE_BYTES ((ASSET_STAGE_A_BYTES > TITLE_TOTAL_BYTES) ? ASSET_STAGE_A_BYTES : TITLE_TOTAL_BYTES)
#endif
#define ASSET_STAGE_BYTES (ASSET_BASE_STAGE_BYTES + CARD_BIG_CACHE_BYTES)
static uint8_t g_asset_stage_ram[ASSET_STAGE_BYTES] __attribute__((aligned(4)));
static int g_story_portrait_slot_id[PORTRAIT_SLOT_COUNT] = {-1, -1};
static int g_requested_portrait_id[PORTRAIT_SLOT_COUNT] = {-1, -1};
static int g_title_loaded = 0;
static int g_cards_loaded = 0;
static WaifuAssetRequest g_pending_request = WAIFU_ASSET_REQUEST_NONE;
static int g_load_step = 0;
static int g_ready = 1;
static size_t g_ram_used = 0;
static size_t g_ram_high_water = 0;

#if !defined(WAIFU_FM_PCFX)
static uint8_t *stage_title_ptr(void) { return g_asset_stage_ram; }
static uint8_t *stage_title64_ptr(void) { return g_asset_stage_ram + TITLE_BYTES; }
static uint8_t *stage_title16m_ptr(void) { return g_asset_stage_ram + TITLE_BYTES; }
#endif
static uint8_t *stage_card_faces_ptr(void) { return g_asset_stage_ram; }
static uint8_t *stage_card_back_ptr(void) { return g_asset_stage_ram + CARD_FACE_BYTES; }
static uint8_t *stage_support_face_ptr(void) { return g_asset_stage_ram + CARD_FACE_BYTES + CARD_ONE_BYTES; }
static uint8_t *stage_big_cache_ptr(int slot) { return g_asset_stage_ram + ASSET_BASE_STAGE_BYTES + ((size_t)slot * CARD_BIG_CACHE_SLOT_BYTES); }
static uint8_t *stage_support_big_ptr(void) { return g_asset_stage_ram + ASSET_BASE_STAGE_BYTES + ((size_t)WAIFU_ASSET_BIG_CACHE_SLOTS * CARD_BIG_CACHE_SLOT_BYTES); }
static int g_big_cache_card_id[WAIFU_ASSET_BIG_CACHE_SLOTS];
static unsigned g_big_cache_stamp[WAIFU_ASSET_BIG_CACHE_SLOTS];
static unsigned g_big_cache_clock = 1;
static int g_support_big_loaded = 0;
static int g_prewarm_big_card_ids[WAIFU_ASSET_BIG_CACHE_SLOTS];
static int g_prewarm_big_card_count = 0;
static int g_prewarm_support_big = 0;
static int g_prewarm_all_big_cards = 0;
static int find_big_cache_slot(int card_id);
static int big_cache_loaded_count(void);
static void prewarm_list_add_all_monster_big_art(void);
static uint8_t *stage_portrait_pixels_ptr(int slot) { return g_asset_stage_ram + ((size_t)slot * PORTRAIT_SLOT_BYTES); }
static uint8_t *stage_portrait_mask_ptr(int slot) { return stage_portrait_pixels_ptr(slot) + PORTRAIT_ONE_BYTES; }
#endif

static void note_high_water(size_t used)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (used > g_ram_high_water) g_ram_high_water = used;
#else
    (void)used;
#endif
}

void waifu_assets_init(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    g_title_loaded = 0;
    g_cards_loaded = 0;
    g_story_portrait_slot_id[0] = -1;
    g_story_portrait_slot_id[1] = -1;
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    g_pending_request = WAIFU_ASSET_REQUEST_NONE;
    g_load_step = 0;
    g_ready = 1;
    g_ram_used = 0;
    g_ram_high_water = 0;
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) {
        g_big_cache_card_id[i] = -1;
        g_big_cache_stamp[i] = 0;
        g_prewarm_big_card_ids[i] = -1;
    }
    g_big_cache_clock = 1;
    g_support_big_loaded = 0;
    g_prewarm_big_card_count = 0;
    g_prewarm_support_big = 0;
    g_prewarm_all_big_cards = 0;
#endif
}

void waifu_assets_reset(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    waifu_assets_init();
#endif
}

WaifuAssetBackend waifu_assets_backend(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return WAIFU_ASSET_BACKEND_CDROM;
#elif WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CART_ROM
    return WAIFU_ASSET_BACKEND_CART_ROM;
#else
    return WAIFU_ASSET_BACKEND_COMPILED;
#endif
}

const char *waifu_assets_backend_name(void)
{
    switch (waifu_assets_backend()) {
    case WAIFU_ASSET_BACKEND_CART_ROM: return "cart-rom";
    case WAIFU_ASSET_BACKEND_CDROM: return "cdrom";
    default: return "compiled";
    }
}

WaifuAssetMemoryProfile waifu_assets_memory_profile(void)
{
    size_t b = waifu_assets_ram_budget_bytes();
    if (b <= 512u * 1024u) return WAIFU_ASSET_MEMORY_512K;
    if (b <= 2u * 1024u * 1024u) return WAIFU_ASSET_MEMORY_2MB;
    return WAIFU_ASSET_MEMORY_LARGE;
}

size_t waifu_assets_ram_budget_bytes(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return (size_t)WAIFU_ASSET_RAM_BUDGET;
#else
    return 0;
#endif
}

size_t waifu_assets_ram_used_bytes(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_ram_used;
#else
    return 0;
#endif
}

size_t waifu_assets_ram_high_water_bytes(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_ram_high_water;
#else
    return 0;
#endif
}

int waifu_assets_ram_budget_ok(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_ram_high_water <= (size_t)WAIFU_ASSET_RAM_BUDGET;
#else
    return 1;
#endif
}

const char *waifu_assets_request_name(WaifuAssetRequest req)
{
    switch (req) {
    case WAIFU_ASSET_REQUEST_TITLE: return "TITLE";
    case WAIFU_ASSET_REQUEST_STORY_INTRO: return "STORY INTRO";
    case WAIFU_ASSET_REQUEST_STORY_DUEL: return "STORY PORTRAITS";
    case WAIFU_ASSET_REQUEST_CARDS: return "CARD ART";
    default: return "READY";
    }
}

WaifuAssetRequest waifu_assets_pending_request(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_pending_request;
#else
    return WAIFU_ASSET_REQUEST_NONE;
#endif
}

#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
static void evict_title(void)
{
    if (g_title_loaded) {
        g_title_loaded = 0;
#if !defined(WAIFU_FM_PCFX)
        if (g_ram_used >= TITLE_TOTAL_BYTES) g_ram_used -= TITLE_TOTAL_BYTES;
        else g_ram_used = 0;
#endif
    }
}

static void evict_cards(void)
{
    if (g_cards_loaded) {
        g_cards_loaded = 0;
        if (g_ram_used >= CARDS_TOTAL_BYTES) g_ram_used -= CARDS_TOTAL_BYTES;
        else g_ram_used = 0;
    }
    /* Deliberately do not evict g_big_cache_card_id[] or support big art here.
       Those live after ASSET_BASE_STAGE_BYTES, outside the title/portrait/card
       working-set overlay.  Keeping them resident makes the big-art cache global
       for the whole game session and lets story/random battles reuse card art
       already loaded by earlier duels or deck previews. */
}

static void evict_portraits(void)
{
    for (int i = 0; i < PORTRAIT_SLOT_COUNT; ++i) {
        if (g_story_portrait_slot_id[i] >= 0) {
            if (g_ram_used >= PORTRAIT_SLOT_BYTES) g_ram_used -= PORTRAIT_SLOT_BYTES;
            else g_ram_used = 0;
        }
        g_story_portrait_slot_id[i] = -1;
    }
}

static int portrait_id_loaded(int portrait_id)
{
    for (int i = 0; i < PORTRAIT_SLOT_COUNT; ++i) {
        if (g_story_portrait_slot_id[i] == portrait_id) return 1;
    }
    return 0;
}

static int requested_portraits_ready(void)
{
    for (int i = 0; i < PORTRAIT_SLOT_COUNT; ++i) {
        if (g_requested_portrait_id[i] >= 0 && !portrait_id_loaded(g_requested_portrait_id[i])) return 0;
    }
    return 1;
}

static void start_request(WaifuAssetRequest req)
{
    g_pending_request = req;
    g_load_step = 0;
    g_ready = 0;
}
#endif

void waifu_assets_request_title(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    evict_cards();
    evict_portraits();
    if (g_title_loaded) { g_pending_request = WAIFU_ASSET_REQUEST_NONE; g_ready = 1; return; }
    start_request(WAIFU_ASSET_REQUEST_TITLE);
#endif
}

void waifu_assets_request_story_intro(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    g_requested_portrait_id[0] = 0;
    g_requested_portrait_id[1] = -1;
    evict_title();
    evict_cards();
    if (requested_portraits_ready()) { g_pending_request = WAIFU_ASSET_REQUEST_NONE; g_ready = 1; return; }
    evict_portraits();
    start_request(WAIFU_ASSET_REQUEST_STORY_INTRO);
#endif
}

void waifu_assets_request_story_duel(int opponent_portrait_id)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (opponent_portrait_id < 0) opponent_portrait_id = 0;
    if (opponent_portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) opponent_portrait_id = WAIFU_STORY_PORTRAIT_COUNT - 1;
    g_requested_portrait_id[0] = 0;
    g_requested_portrait_id[1] = opponent_portrait_id == 0 ? -1 : opponent_portrait_id;
    evict_title();
    evict_cards();
    if (requested_portraits_ready()) { g_pending_request = WAIFU_ASSET_REQUEST_NONE; g_ready = 1; return; }
    evict_portraits();
    start_request(WAIFU_ASSET_REQUEST_STORY_DUEL);
#else
    (void)opponent_portrait_id;
#endif
}

#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
static void prewarm_list_clear(void)
{
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) g_prewarm_big_card_ids[i] = -1;
    g_prewarm_big_card_count = 0;
    g_prewarm_support_big = 0;
    g_prewarm_all_big_cards = 0;
}

static int prewarm_list_contains(int card_id)
{
    for (int i = 0; i < g_prewarm_big_card_count; ++i) {
        if (g_prewarm_big_card_ids[i] == card_id) return 1;
    }
    return 0;
}

static void prewarm_list_add_card(int card_id)
{
    if (card_id < 0) return;
    if (card_id >= WAIFU_CARD_COUNT) {
        if (!g_support_big_loaded) g_prewarm_support_big = 1;
        return;
    }
    if (find_big_cache_slot(card_id) >= 0) return;
    if (g_prewarm_big_card_count >= WAIFU_ASSET_BIG_CACHE_SLOTS) return;
    if (prewarm_list_contains(card_id)) return;
    g_prewarm_big_card_ids[g_prewarm_big_card_count++] = card_id;
}

static void prewarm_list_add_all_monster_big_art(void)
{
    /* The PC-FX card-check and battle-reveal paths must not synchronously read
       from CD while rendering.  The expanded cache can hold every monster big-art
       image, so the first card working-set load fills any missing full-size art
       once; later duels/deck previews reuse the same resident cache. */
    if (WAIFU_ASSET_BIG_CACHE_SLOTS < WAIFU_CARD_COUNT) return;
    if (big_cache_loaded_count() < WAIFU_CARD_COUNT) g_prewarm_all_big_cards = 1;
    for (int card_id = 0; card_id < WAIFU_CARD_COUNT; ++card_id) {
        prewarm_list_add_card(card_id);
    }
}
#endif

void waifu_assets_request_cards(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    prewarm_list_clear();
    prewarm_list_add_all_monster_big_art();
    if (!g_support_big_loaded) g_prewarm_support_big = 1;
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    evict_title();
    evict_portraits();
    if (g_cards_loaded) {
        g_pending_request = (g_prewarm_big_card_count > 0 || (g_prewarm_support_big && !g_support_big_loaded)) ? WAIFU_ASSET_REQUEST_CARDS : WAIFU_ASSET_REQUEST_NONE;
        g_load_step = 3;
        g_ready = (g_pending_request == WAIFU_ASSET_REQUEST_NONE);
        return;
    }
    start_request(WAIFU_ASSET_REQUEST_CARDS);
#endif
}

void waifu_assets_request_cards_for_list(const int *card_ids, int count)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    prewarm_list_clear();
    if (card_ids && count > 0) {
        for (int i = 0; i < count; ++i) prewarm_list_add_card(card_ids[i]);
    }
    prewarm_list_add_all_monster_big_art();
    /* Support/equip big art is a common mid-animation miss; stage it with the
       normal card working set so the reveal path never blocks on CD. */
    g_prewarm_support_big = 1;
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    evict_title();
    evict_portraits();
    if (g_cards_loaded) {
        g_pending_request = (g_prewarm_big_card_count > 0 || (g_prewarm_support_big && !g_support_big_loaded)) ? WAIFU_ASSET_REQUEST_CARDS : WAIFU_ASSET_REQUEST_NONE;
        g_load_step = 3;
        g_ready = (g_pending_request == WAIFU_ASSET_REQUEST_NONE);
        return;
    }
    start_request(WAIFU_ASSET_REQUEST_CARDS);
#else
    (void)card_ids;
    (void)count;
#endif
}

int waifu_assets_ready(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_ready;
#else
    return 1;
#endif
}

int waifu_assets_needs_loading_screen(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return !g_ready;
#else
    return 0;
#endif
}

#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
static int cd_read_blob(WaifuAssetBlobId blob, uint8_t *dst, size_t bytes)
{
    return read_blob_platform(blob, dst, bytes);
}

static size_t cd_round_sector_bytes(size_t bytes)
{
    return (bytes + (size_t)PCFX_CD_SECTOR_BYTES - 1u) & ~((size_t)PCFX_CD_SECTOR_BYTES - 1u);
}

static int cd_read_blob_padded_from_start(WaifuAssetBlobId blob, uint8_t *dst, size_t bytes)
{
    /* PC-FX CD reads are much faster as one multi-sector command than as many
       one-sector reads.  The card face/back/support staging area has spare RAM
       beyond each destination, and later stages overwrite any over-read padding,
       so sector-rounding these start-aligned blobs is safe and avoids the long
       duel-loading stall from 58+ individual card-face reads. */
    return read_blob_slice_platform(blob, dst, 0, cd_round_sector_bytes(bytes));
}

static int cd_read_blob_slice(WaifuAssetBlobId blob, uint8_t *dst, size_t offset, size_t bytes)
{
    return read_blob_slice_platform(blob, dst, offset, bytes);
}

static void add_ram_used(size_t bytes)
{
    g_ram_used += bytes;
    note_high_water(g_ram_used);
}

static const uint8_t *load_big_card_art_cached(int card_id);
static int load_all_big_card_art_cached(void);

static int load_requested_portrait_slot(int slot)
{
    int portrait_id = g_requested_portrait_id[slot];
    size_t off;
    if (portrait_id < 0) return 1;
    if (portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return 0;
    off = (size_t)portrait_id * PORTRAIT_ONE_BYTES;
    if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_STORY_PORTRAITS, stage_portrait_pixels_ptr(slot), off, PORTRAIT_ONE_BYTES)) return 0;
    if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK, stage_portrait_mask_ptr(slot), off, PORTRAIT_ONE_BYTES)) return 0;
    g_story_portrait_slot_id[slot] = portrait_id;
    add_ram_used(PORTRAIT_SLOT_BYTES);
    return 1;
}
#endif

int waifu_assets_load_step(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (g_ready) return 1;
    switch (g_pending_request) {
    case WAIFU_ASSET_REQUEST_TITLE:
        if (g_load_step == 0) {
#if defined(WAIFU_FM_PCFX)
            /* PC-FX title pixels are not staged in CPU RAM.  The 16M title
               image is uploaded once from CD directly into KING KRAM by the
               PC-FX video backend while the VDC fade layer is still black;
               prompt/menu/fade changes then touch only VDC VRAM. */
            g_title_loaded = 1;
            note_high_water(g_ram_used);
#else
            if (!cd_read_blob(WAIFU_ASSET_BLOB_TITLE_SCREEN, stage_title_ptr(), TITLE_BYTES)) return 0;
            if (!cd_read_blob(WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV422, stage_title16m_ptr(), TITLE_16M_BYTES)) return 0;
            g_title_loaded = 1;
            add_ram_used(TITLE_TOTAL_BYTES);
#endif
            ++g_load_step;
            g_ready = 1;
            g_pending_request = WAIFU_ASSET_REQUEST_NONE;
            return 1;
        }
        break;
    case WAIFU_ASSET_REQUEST_STORY_INTRO:
    case WAIFU_ASSET_REQUEST_STORY_DUEL:
        if (g_load_step < PORTRAIT_SLOT_COUNT) {
            int slot = g_load_step++;
            if (!load_requested_portrait_slot(slot)) return 0;
            if (g_load_step >= PORTRAIT_SLOT_COUNT) {
                g_ready = 1;
                g_pending_request = WAIFU_ASSET_REQUEST_NONE;
                return 1;
            }
            return 0;
        }
        break;
    case WAIFU_ASSET_REQUEST_CARDS:
        if (g_load_step == 0) {
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_CARD_FACES, stage_card_faces_ptr(), CARD_FACE_BYTES)) return 0;
            ++g_load_step;
            return 0;
        }
        if (g_load_step == 1) {
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_CARD_BACK, stage_card_back_ptr(), CARD_ONE_BYTES)) return 0;
            ++g_load_step;
            return 0;
        }
        if (g_load_step == 2) {
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_SUPPORT_FACE, stage_support_face_ptr(), CARD_ONE_BYTES)) return 0;
            ++g_load_step;
            g_cards_loaded = 1;
            add_ram_used(CARDS_TOTAL_BYTES);
            return 0;
        }
        if (g_load_step == 3) {
            if (g_prewarm_support_big && !g_support_big_loaded) {
                if (!cd_read_blob(WAIFU_ASSET_BLOB_SUPPORT_BIG_ART, stage_support_big_ptr(), CARD_BIG_ONE_BYTES)) return 0;
                g_support_big_loaded = 1;
                add_ram_used(CARD_BIG_CACHE_SLOT_BYTES);
            }
            ++g_load_step;
            return 0;
        }
        if (g_load_step == 4 && g_prewarm_all_big_cards) {
            if (!load_all_big_card_art_cached()) return 0;
            g_load_step = 4 + g_prewarm_big_card_count;
            return 0;
        }
        if (g_load_step >= 4 && g_load_step < 4 + g_prewarm_big_card_count) {
            int prewarm_index = g_load_step - 4;
            int card_id = g_prewarm_big_card_ids[prewarm_index];
            if (card_id >= 0 && card_id < WAIFU_CARD_COUNT) {
                if (!load_big_card_art_cached(card_id)) return 0;
            }
            ++g_load_step;
            return 0;
        }
        g_ready = 1;
        g_pending_request = WAIFU_ASSET_REQUEST_NONE;
        return 1;
    default:
        break;
    }
    g_ready = 1;
    g_pending_request = WAIFU_ASSET_REQUEST_NONE;
    return 1;
#else
    return 1;
#endif
}

int waifu_assets_loading_percent(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (g_ready) return 100;
    switch (g_pending_request) {
    case WAIFU_ASSET_REQUEST_TITLE: return g_load_step ? 100 : 0;
    case WAIFU_ASSET_REQUEST_STORY_INTRO:
    case WAIFU_ASSET_REQUEST_STORY_DUEL: return (g_load_step * 100) / PORTRAIT_SLOT_COUNT;
    case WAIFU_ASSET_REQUEST_CARDS: {
        int total = 4 + g_prewarm_big_card_count;
        if (total < 4) total = 4;
        return (g_load_step * 100) / total;
    }
    default: return 100;
    }
#else
    return 100;
#endif
}

const char *waifu_assets_loading_label(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (g_ready) return "READY";
    if (g_pending_request == WAIFU_ASSET_REQUEST_CARDS) {
        if (g_load_step == 0) return "CARD FACES";
        if (g_load_step == 1) return "CARD BACK";
        if (g_load_step == 2) return "SUPPORT FACE";
        if (g_load_step == 3) return "SUPPORT BIG ART";
        return "MONSTER BIG ART";
    }
    if (g_pending_request == WAIFU_ASSET_REQUEST_STORY_INTRO || g_pending_request == WAIFU_ASSET_REQUEST_STORY_DUEL) {
        if (g_load_step == 0) return "SERENA";
        return "OPPONENT";
    }
    return waifu_assets_request_name(g_pending_request);
#else
    return "READY";
#endif
}

int waifu_assets_title_ready(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_title_loaded;
#else
    return 1;
#endif
}

int waifu_assets_cards_ready(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_cards_loaded;
#else
    return 1;
#endif
}

int waifu_assets_story_portrait_ready(int portrait_id)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (portrait_id < 0 || portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return 0;
    return portrait_id_loaded(portrait_id);
#else
    (void)portrait_id;
    return 1;
#endif
}

const uint8_t *waifu_assets_title_screen_img(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
#if defined(WAIFU_FM_PCFX)
    return NULL;
#else
    return g_title_loaded ? stage_title_ptr() : NULL;
#endif
#else
    return title_screen_img;
#endif
}

const uint16_t *waifu_assets_title_screen_pcfx_yuv16(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
#if defined(WAIFU_FM_PCFX)
    return NULL;
#else
    return g_title_loaded ? (const uint16_t *)stage_title64_ptr() : NULL;
#endif
#else
#ifdef WAIFU_ASSET_EXTERNAL_TITLE_IMAGE
    return NULL;
#else
    return title_screen_pcfx_yuv16;
#endif
#endif
}

const uint16_t *waifu_assets_title_screen_pcfx_yuv422(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
#if defined(WAIFU_FM_PCFX)
    return NULL;
#else
    return g_title_loaded ? (const uint16_t *)stage_title16m_ptr() : NULL;
#endif
#else
#ifdef WAIFU_ASSET_EXTERNAL_TITLE_IMAGE
    return NULL;
#else
    return title_screen_pcfx_yuv422;
#endif
#endif
}

const uint8_t *waifu_assets_story_portrait_pixels(int portrait_id)
{
    if (portrait_id < 0 || portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return NULL;
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    for (int i = 0; i < PORTRAIT_SLOT_COUNT; ++i) {
        if (g_story_portrait_slot_id[i] == portrait_id) return stage_portrait_pixels_ptr(i);
    }
    return NULL;
#else
    return waifu_story_portraits + ((size_t)portrait_id * PORTRAIT_ONE_BYTES);
#endif
}

const uint8_t *waifu_assets_story_portrait_mask(int portrait_id)
{
    if (portrait_id < 0 || portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return NULL;
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    for (int i = 0; i < PORTRAIT_SLOT_COUNT; ++i) {
        if (g_story_portrait_slot_id[i] == portrait_id) return stage_portrait_mask_ptr(i);
    }
    return NULL;
#else
    return waifu_story_portrait_mask + ((size_t)portrait_id * PORTRAIT_ONE_BYTES);
#endif
}


#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
static int find_big_cache_slot(int card_id)
{
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) {
        if (g_big_cache_card_id[i] == card_id) return i;
    }
    return -1;
}

static int big_cache_loaded_count(void)
{
    int n = 0;
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) {
        if (g_big_cache_card_id[i] >= 0) ++n;
    }
    return n;
}

static int choose_big_cache_slot(void)
{
    int best = 0;
    unsigned best_stamp = g_big_cache_stamp[0];
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) {
        if (g_big_cache_card_id[i] < 0) return i;
        if (g_big_cache_stamp[i] < best_stamp) { best = i; best_stamp = g_big_cache_stamp[i]; }
    }
    return best;
}

static int load_all_big_card_art_cached(void)
{
    int before = big_cache_loaded_count();
    size_t bytes = (size_t)WAIFU_CARD_COUNT * CARD_BIG_ONE_BYTES;
    if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_CARD_BIG_ART, stage_big_cache_ptr(0), 0, bytes)) return 0;
    for (int card_id = 0; card_id < WAIFU_CARD_COUNT; ++card_id) {
        g_big_cache_card_id[card_id] = card_id;
        g_big_cache_stamp[card_id] = g_big_cache_clock++;
        if (g_big_cache_clock == 0) g_big_cache_clock = 1;
    }
    if (before < WAIFU_CARD_COUNT) {
        add_ram_used((size_t)(WAIFU_CARD_COUNT - before) * CARD_BIG_CACHE_SLOT_BYTES);
    }
    return 1;
}

static const uint8_t *load_big_card_art_cached(int card_id)
{
    int slot = find_big_cache_slot(card_id);
    if (slot < 0) {
        size_t off = (size_t)card_id * CARD_BIG_ONE_BYTES;
        slot = choose_big_cache_slot();
        if (g_big_cache_card_id[slot] < 0) add_ram_used(CARD_BIG_CACHE_SLOT_BYTES);
        if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_CARD_BIG_ART, stage_big_cache_ptr(slot), off, CARD_BIG_ONE_BYTES)) return NULL;
        g_big_cache_card_id[slot] = card_id;
    }
    g_big_cache_stamp[slot] = g_big_cache_clock++;
    if (g_big_cache_clock == 0) g_big_cache_clock = 1;
    return stage_big_cache_ptr(slot);
}
#endif

const uint8_t *waifu_assets_card_face(int card_id)
{
    if (card_id < 0) card_id = 0;
    if (card_id >= WAIFU_CARD_COUNT) card_id = WAIFU_CARD_COUNT - 1;
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (!g_cards_loaded) return NULL;
    return stage_card_faces_ptr() + ((size_t)card_id * CARD_ONE_BYTES);
#else
    return waifu_card_faces + ((size_t)card_id * CARD_ONE_BYTES);
#endif
}


const uint8_t *waifu_assets_card_big_art(int card_id)
{
    if (card_id < 0) card_id = 0;
    if (card_id >= WAIFU_CARD_COUNT) card_id = WAIFU_CARD_COUNT - 1;
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (!g_cards_loaded) return NULL;
    return load_big_card_art_cached(card_id);
#else
    return waifu_big_card_art + ((size_t)card_id * CARD_BIG_ONE_BYTES);
#endif
}

const uint8_t *waifu_assets_card_big_art_cached(int card_id)
{
    if (card_id < 0 || card_id >= WAIFU_CARD_COUNT) return NULL;
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    {
        int slot = find_big_cache_slot(card_id);
        if (slot < 0) return NULL;
        return stage_big_cache_ptr(slot);
    }
#else
    return waifu_big_card_art + ((size_t)card_id * CARD_BIG_ONE_BYTES);
#endif
}

const uint8_t *waifu_assets_support_big_art_cached(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_support_big_loaded ? stage_support_big_ptr() : NULL;
#else
    return waifu_support_big_art;
#endif
}

const uint8_t *waifu_assets_big_art_cached(WaifuBigArtKind kind, int card_id)
{
    if (kind == WAIFU_BIG_ART_SUPPORT) return waifu_assets_support_big_art_cached();
    return waifu_assets_card_big_art_cached(card_id);
}

int waifu_assets_big_art_cache_loaded_count(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return big_cache_loaded_count();
#else
    return WAIFU_CARD_COUNT;
#endif
}

int waifu_assets_big_art_cache_slot_count(void)
{
    return WAIFU_ASSET_BIG_CACHE_SLOTS;
}

int waifu_assets_big_art_cache_contains(int card_id)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (card_id < 0 || card_id >= WAIFU_CARD_COUNT) return 0;
    return find_big_cache_slot(card_id) >= 0;
#else
    return card_id >= 0 && card_id < WAIFU_CARD_COUNT;
#endif
}

int waifu_assets_support_big_art_loaded(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_support_big_loaded;
#else
    return 1;
#endif
}

const uint8_t *waifu_assets_card_back(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_cards_loaded ? stage_card_back_ptr() : NULL;
#else
    return waifu_card_back;
#endif
}

const uint8_t *waifu_assets_support_face(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    return g_cards_loaded ? stage_support_face_ptr() : NULL;
#else
    return waifu_support_face;
#endif
}

const uint8_t *waifu_assets_support_big_art(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    if (!g_cards_loaded) return NULL;
    if (!g_support_big_loaded) {
        if (!cd_read_blob(WAIFU_ASSET_BLOB_SUPPORT_BIG_ART, stage_support_big_ptr(), CARD_BIG_ONE_BYTES)) return NULL;
        g_support_big_loaded = 1;
        add_ram_used(CARD_BIG_CACHE_SLOT_BYTES);
    }
    return stage_support_big_ptr();
#else
    return waifu_support_big_art;
#endif
}
