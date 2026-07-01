#include "assets.h"
#include "cfx_screen_config.h"

#include <stddef.h>
#include <string.h>

#if defined(WAIFU_ASSET_USE_CDROM)
#define WAIFU_ASSET_EXTERNAL_TITLE_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_ENDING_IMAGE 1
#define WAIFU_ASSET_EXTERNAL_STORY_PORTRAITS 1
#define WAIFU_ASSET_EXTERNAL_CARD_IMAGES 1
#endif

#include "waifu_assets.h"
#include "title_asset.h"

#if defined(WAIFU_FM_CD32X)
#include "waifu_cd32x_memory.h"
#include "waifu_cd32x_video.h"
#include "cd32x_title_asset.h"
#endif

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

#if defined(WAIFU_FM_CD32X)
#define WAIFU_TITLE_ASSET_W CD32X_TITLE_SCREEN_W
#define WAIFU_TITLE_ASSET_H CD32X_TITLE_SCREEN_H
#else
#define WAIFU_TITLE_ASSET_W TITLE_SCREEN_W
#define WAIFU_TITLE_ASSET_H TITLE_SCREEN_H
#endif
#define TITLE_BYTES ((size_t)WAIFU_TITLE_ASSET_W * (size_t)WAIFU_TITLE_ASSET_H)
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

/* The raw blob backend lives in a per-platform translation unit and is reached
   only through waifu_assets_platform_read_blob_slice() (host_assets.c on the
   host; waifu_pcfx_cdrom.c on PC-FX; a ROM/RAM reader elsewhere). This keeps
   stdio and any file/device API out of the common asset code below. */

static int read_blob_slice_platform(WaifuAssetBlobId blob, uint8_t *dst, size_t offset, size_t bytes)
{
    return waifu_assets_platform_read_blob_slice(blob, dst, offset, bytes);
}

static int read_blob_platform(WaifuAssetBlobId blob, uint8_t *dst, size_t bytes)
{
    return read_blob_slice_platform(blob, dst, 0, bytes);
}

void waifu_assets_big_art_draw_queue_reset(void)
{
    g_big_art_draw_count = 0;
}

void waifu_assets_note_big_art_draw(WaifuBigArtKind kind, int card_id, int x, int y)
{
    if (g_big_art_draw_count >= WAIFU_ASSET_BIG_ART_DRAW_MAX) return;
    if (x < 0 || y < 0 || x + WAIFU_BIG_W > WAIFU_FM_WIDTH || y + WAIFU_BIG_H > WAIFU_FM_HEIGHT) return;
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
#if defined(WAIFU_FM_CD32X)
/* CD32X cannot keep the full 72-card face atlas (~144 KiB) resident in 32X
   SDRAM without overrunning into the stack.  Card faces are streamed per-card
   into an LRU staged in the asset arena.  Keep this large enough for the
   currently visible hand/field set so drawing does not thrash the CD every
   frame. */
#define CD32X_CARD_FACE_CACHE_SLOTS 24
#define CD32X_CARD_FACE_ENTRY_PREWARM_LIMIT 5
#define CARD_FACE_STAGE_BYTES ((size_t)CD32X_CARD_FACE_CACHE_SLOTS * CARD_ONE_BYTES)
#else
#define CARD_FACE_STAGE_BYTES CARD_FACE_BYTES
#endif
#define CARDS_TOTAL_BYTES (CARD_FACE_STAGE_BYTES + CARD_EXTRA_BYTES)
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
#ifndef WAIFU_STORY_PORTRAIT_CD_STRIDE
#define WAIFU_STORY_PORTRAIT_CD_STRIDE ((WAIFU_STORY_PORTRAIT_W * WAIFU_STORY_PORTRAIT_H + PCFX_CD_SECTOR_BYTES - 1u) & ~(PCFX_CD_SECTOR_BYTES - 1u))
#endif
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
#define PORTRAIT_PLANE_BYTES ((size_t)WAIFU_STORY_PORTRAIT_CD_STRIDE)
#else
#define PORTRAIT_PLANE_BYTES PORTRAIT_ONE_BYTES
#endif
#define PORTRAIT_SLOT_BYTES (PORTRAIT_PLANE_BYTES * 2u)
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
#elif defined(WAIFU_FM_CD32X)
/* CD32X displays only the 8bpp title plane.  Keep the PC-FX 16M title staging
   out of the 32X SDRAM arena without changing PC-FX/headless behavior.
   Title, portraits, and cards are mutually exclusive working sets on CD32X;
   card big-art is packed after CARDS_TOTAL_BYTES rather than after this title
   maximum so card mode does not run past the actual 32X SDRAM arena. */
#define ASSET_BASE_STAGE_BYTES ((ASSET_STAGE_A_BYTES > TITLE_BYTES) ? ASSET_STAGE_A_BYTES : TITLE_BYTES)
#define CARD_BIG_STAGE_OFFSET CARDS_TOTAL_BYTES
#else
#define ASSET_BASE_STAGE_BYTES ((ASSET_STAGE_A_BYTES > TITLE_TOTAL_BYTES) ? ASSET_STAGE_A_BYTES : TITLE_TOTAL_BYTES)
#endif
#ifndef CARD_BIG_STAGE_OFFSET
#define CARD_BIG_STAGE_OFFSET ASSET_BASE_STAGE_BYTES
#endif
#define ASSET_CARD_STAGE_BYTES (CARD_BIG_STAGE_OFFSET + CARD_BIG_CACHE_BYTES)
#define ASSET_STAGE_BYTES ((ASSET_BASE_STAGE_BYTES > ASSET_CARD_STAGE_BYTES) ? ASSET_BASE_STAGE_BYTES : ASSET_CARD_STAGE_BYTES)
#if defined(WAIFU_FM_CD32X)
typedef char WaifuCd32xAssetStageFits[(ASSET_STAGE_BYTES <= WAIFU_CD32X_ASSET_ARENA_BYTES) ? 1 : -1];
#define g_asset_stage_ram (waifu_cd32x_asset_arena())
#else
static uint8_t g_asset_stage_ram[ASSET_STAGE_BYTES] __attribute__((aligned(4)));
#endif
static int g_story_portrait_slot_id[PORTRAIT_SLOT_COUNT] = {-1, -1};
static int g_requested_portrait_id[PORTRAIT_SLOT_COUNT] = {-1, -1};
static int g_title_loaded = 0;
static int g_ending_loaded = 0;
static int g_cards_loaded = 0;
static WaifuAssetRequest g_pending_request = WAIFU_ASSET_REQUEST_NONE;
static int g_load_step = 0;
static int g_ready = 1;
static size_t g_ram_used = 0;
static size_t g_ram_high_water = 0;

#if !defined(WAIFU_FM_PCFX)
#if defined(WAIFU_FM_CD32X)
/* CD32X keeps the title resident in the (otherwise idle on title/menu) asset
   arena so the software full-redraw path can re-blit it into the framebuffer
   every frame -- the 32X page-flips each frame, so only a fully redrawn frame
   avoids the per-frame stale-page flash. */
static uint8_t *stage_title_ptr(void) { return g_asset_stage_ram; }
static uint8_t *stage_title64_ptr(void) { return NULL; }
static uint8_t *stage_title16m_ptr(void) { return NULL; }
#else
static uint8_t *stage_title_ptr(void) { return g_asset_stage_ram; }
static uint8_t *stage_title64_ptr(void) { return g_asset_stage_ram + TITLE_BYTES; }
static uint8_t *stage_title16m_ptr(void) { return g_asset_stage_ram + TITLE_BYTES; }
#endif
#endif
static uint8_t *stage_card_faces_ptr(void) { return g_asset_stage_ram; }
static uint8_t *stage_card_back_ptr(void) { return g_asset_stage_ram + CARD_FACE_STAGE_BYTES; }
static uint8_t *stage_support_face_ptr(void) { return g_asset_stage_ram + CARD_FACE_STAGE_BYTES + CARD_ONE_BYTES; }
static int g_prewarm_face_card_count = 0;
#if defined(WAIFU_FM_CD32X)
static uint8_t *stage_card_face_cache_slot_ptr(int slot) { return g_asset_stage_ram + ((size_t)slot * CARD_ONE_BYTES); }
static int g_cd32x_face_cache_card_id[CD32X_CARD_FACE_CACHE_SLOTS];
static unsigned g_cd32x_face_cache_stamp[CD32X_CARD_FACE_CACHE_SLOTS];
static unsigned g_cd32x_face_cache_clock = 1;
static int g_prewarm_face_card_ids[CD32X_CARD_FACE_CACHE_SLOTS];
static void cd32x_card_face_cache_reset(void);
static int cd32x_find_card_face_slot(int card_id);
#endif
static uint8_t *stage_big_cache_ptr(int slot) { return g_asset_stage_ram + CARD_BIG_STAGE_OFFSET + ((size_t)slot * CARD_BIG_CACHE_SLOT_BYTES); }
static uint8_t *stage_support_big_ptr(void) { return g_asset_stage_ram + CARD_BIG_STAGE_OFFSET + ((size_t)WAIFU_ASSET_BIG_CACHE_SLOTS * CARD_BIG_CACHE_SLOT_BYTES); }
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
static uint8_t *stage_portrait_mask_ptr(int slot) { return stage_portrait_pixels_ptr(slot) + PORTRAIT_PLANE_BYTES; }
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
    g_ending_loaded = 0;
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
    case WAIFU_ASSET_REQUEST_ENDING: return "ENDING";
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

static void evict_ending(void)
{
    g_ending_loaded = 0;
}

static void clear_big_art_cache_metadata(void)
{
    int loaded = big_cache_loaded_count();
    if (loaded > 0) {
        size_t bytes = (size_t)loaded * CARD_BIG_CACHE_SLOT_BYTES;
        if (g_ram_used >= bytes) g_ram_used -= bytes;
        else g_ram_used = 0;
    }
    if (g_support_big_loaded) {
        if (g_ram_used >= CARD_BIG_CACHE_SLOT_BYTES) g_ram_used -= CARD_BIG_CACHE_SLOT_BYTES;
        else g_ram_used = 0;
    }
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) {
        g_big_cache_card_id[i] = -1;
        g_big_cache_stamp[i] = 0;
    }
    g_big_cache_clock = 1;
    g_support_big_loaded = 0;
}

static void evict_cards(void)
{
    if (g_cards_loaded) {
        g_cards_loaded = 0;
        if (g_ram_used >= CARDS_TOTAL_BYTES) g_ram_used -= CARDS_TOTAL_BYTES;
        else g_ram_used = 0;
    }
#if defined(WAIFU_FM_CD32X)
    /* CD32X title, portrait, card-face, and big-art storage all overlap inside
       the transient SDRAM arena.  Any working-set switch can overwrite big art,
       so cached-only draw probes must not keep stale card IDs. */
    clear_big_art_cache_metadata();
#else
    /* Deliberately do not evict g_big_cache_card_id[] or support big art here.
       Those live after CARD_BIG_STAGE_OFFSET, outside the title/portrait/card
       working-set overlay.  Keeping them resident makes the big-art cache global
       for the whole game session and lets story/random battles reuse card art
       already loaded by earlier duels or deck previews. */
#endif
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
    evict_ending();
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
    evict_ending();
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
    evict_ending();
    evict_cards();
    if (requested_portraits_ready()) { g_pending_request = WAIFU_ASSET_REQUEST_NONE; g_ready = 1; return; }
    evict_portraits();
    start_request(WAIFU_ASSET_REQUEST_STORY_DUEL);
#else
    (void)opponent_portrait_id;
#endif
}

void waifu_assets_request_ending(void)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    evict_title();
    evict_cards();
    evict_portraits();
#if defined(WAIFU_FM_CD32X)
    if (g_ending_loaded) { g_pending_request = WAIFU_ASSET_REQUEST_NONE; g_ready = 1; return; }
    start_request(WAIFU_ASSET_REQUEST_ENDING);
#else
    g_ending_loaded = 1;
    g_pending_request = WAIFU_ASSET_REQUEST_NONE;
    g_ready = 1;
#endif
#endif
}

#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
static void prewarm_list_clear(void)
{
    for (int i = 0; i < WAIFU_ASSET_BIG_CACHE_SLOTS; ++i) g_prewarm_big_card_ids[i] = -1;
    g_prewarm_big_card_count = 0;
    g_prewarm_support_big = 0;
    g_prewarm_all_big_cards = 0;
#if defined(WAIFU_FM_CD32X)
    for (int i = 0; i < CD32X_CARD_FACE_CACHE_SLOTS; ++i) g_prewarm_face_card_ids[i] = -1;
    g_prewarm_face_card_count = 0;
#endif
}

#if defined(WAIFU_FM_CD32X)
static int prewarm_face_list_contains(int card_id)
{
    for (int i = 0; i < g_prewarm_face_card_count; ++i) {
        if (g_prewarm_face_card_ids[i] == card_id) return 1;
    }
    return 0;
}

static void prewarm_face_list_add_card(int card_id)
{
    if (card_id < 0 || card_id >= WAIFU_CARD_COUNT) return;
    if (cd32x_find_card_face_slot(card_id) >= 0) return;
    if (g_prewarm_face_card_count >= CD32X_CARD_FACE_CACHE_SLOTS) return;
    if (prewarm_face_list_contains(card_id)) return;
    g_prewarm_face_card_ids[g_prewarm_face_card_count++] = card_id;
}
#endif

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
    evict_ending();
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
#if defined(WAIFU_FM_CD32X)
    /* CD32X battle entry must not front-load big-art reads: each 112x112 card
       costs a CD seek plus a word-by-word supervisor transfer.  Do prewarm the
       small face LRU for the opening player hand only, because otherwise the
       first visible hand frame synchronously streams faces.  Later hand/field
       faces are loaded on demand from 32 KiB chunks; prewarming the whole deck
       before the first draw just moves the hang onto the loading screen. */
    if (card_ids && count > 0) {
        int limit = count < CD32X_CARD_FACE_ENTRY_PREWARM_LIMIT ? count : CD32X_CARD_FACE_ENTRY_PREWARM_LIMIT;
        for (int i = 0; i < limit; ++i) prewarm_face_list_add_card(card_ids[i]);
    }
#else
    if (card_ids && count > 0) {
        for (int i = 0; i < count; ++i) prewarm_list_add_card(card_ids[i]);
    }
    prewarm_list_add_all_monster_big_art();
    /* Support/equip big art is a common mid-animation miss; stage it with the
       normal card working set so the reveal path never blocks on CD. */
    g_prewarm_support_big = 1;
#endif
    g_requested_portrait_id[0] = -1;
    g_requested_portrait_id[1] = -1;
    evict_title();
    evict_ending();
    evict_portraits();
    if (g_cards_loaded) {
#if defined(WAIFU_FM_CD32X)
        g_pending_request = (g_prewarm_face_card_count > 0 ||
                             g_prewarm_big_card_count > 0 ||
                             (g_prewarm_support_big && !g_support_big_loaded)) ? WAIFU_ASSET_REQUEST_CARDS : WAIFU_ASSET_REQUEST_NONE;
#else
        g_pending_request = (g_prewarm_big_card_count > 0 || (g_prewarm_support_big && !g_support_big_loaded)) ? WAIFU_ASSET_REQUEST_CARDS : WAIFU_ASSET_REQUEST_NONE;
#endif
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
    off = (size_t)portrait_id * PORTRAIT_PLANE_BYTES;
    if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_STORY_PORTRAITS, stage_portrait_pixels_ptr(slot), off, PORTRAIT_PLANE_BYTES)) return 0;
    if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK, stage_portrait_mask_ptr(slot), off, PORTRAIT_PLANE_BYTES)) return 0;
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
#if !defined(WAIFU_FM_CD32X)
            if (!cd_read_blob(WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV422, stage_title16m_ptr(), TITLE_16M_BYTES)) return 0;
            add_ram_used(TITLE_TOTAL_BYTES);
#else
            /* CD32X streams the title directly to 32X framebuffer pages, not
               to SH-2 SDRAM, so it should not count against asset RAM. */
#endif
            g_title_loaded = 1;
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
            int slot = g_load_step;
            if (!load_requested_portrait_slot(slot)) return 0;
            ++g_load_step;
            if (g_load_step >= PORTRAIT_SLOT_COUNT) {
                g_ready = 1;
                g_pending_request = WAIFU_ASSET_REQUEST_NONE;
                return 1;
            }
            return 0;
        }
        break;
    case WAIFU_ASSET_REQUEST_ENDING:
        if (g_load_step == 0) {
#if defined(WAIFU_FM_CD32X)
            if (!cd_read_blob(WAIFU_ASSET_BLOB_ENDING_SCREEN_PCFX_YUV422, stage_title_ptr(), TITLE_BYTES)) return 0;
#endif
            g_ending_loaded = 1;
            ++g_load_step;
            g_ready = 1;
            g_pending_request = WAIFU_ASSET_REQUEST_NONE;
            return 1;
        }
        break;
    case WAIFU_ASSET_REQUEST_CARDS:
        if (g_load_step == 0) {
#if defined(WAIFU_FM_CD32X)
            /* CD32X streams card faces per-card into the LRU.  Reset it here,
               then prewarm the requested opening/near-draw faces below while
               the loading screen is visible. */
            cd32x_card_face_cache_reset();
#else
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_CARD_FACES, stage_card_faces_ptr(), CARD_FACE_BYTES)) return 0;
#endif
            ++g_load_step;
            return 0;
        }
        if (g_load_step == 1) {
#if defined(WAIFU_FM_CD32X)
            /* CARD_BACK.BIN is 2052 bytes (38x54), just over one sector.  PC-FX
               can safely sector-pad this read into its larger staging arena, but
               CD32X keeps card back/support face tightly packed in the 32X SDRAM
               arena.  Read the exact file size here. */
            if (!cd_read_blob(WAIFU_ASSET_BLOB_CARD_BACK, stage_card_back_ptr(), CARD_ONE_BYTES)) return 0;
#else
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_CARD_BACK, stage_card_back_ptr(), CARD_ONE_BYTES)) return 0;
#endif
            ++g_load_step;
            return 0;
        }
        if (g_load_step == 2) {
#if defined(WAIFU_FM_CD32X)
            if (!cd_read_blob(WAIFU_ASSET_BLOB_SUPPORT_FACE, stage_support_face_ptr(), CARD_ONE_BYTES)) return 0;
#else
            if (!cd_read_blob_padded_from_start(WAIFU_ASSET_BLOB_SUPPORT_FACE, stage_support_face_ptr(), CARD_ONE_BYTES)) return 0;
#endif
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
#if defined(WAIFU_FM_CD32X)
        if (g_load_step >= 4 && g_load_step < 4 + g_prewarm_face_card_count) {
            int prewarm_index = g_load_step - 4;
            int card_id = g_prewarm_face_card_ids[prewarm_index];
            if (card_id >= 0 && card_id < WAIFU_CARD_COUNT) {
                if (!waifu_assets_card_face(card_id)) return 0;
            }
            ++g_load_step;
            return 0;
        }
#endif
        if (g_load_step == 4 + g_prewarm_face_card_count && g_prewarm_all_big_cards) {
            if (!load_all_big_card_art_cached()) return 0;
            g_load_step = 4 + g_prewarm_face_card_count + g_prewarm_big_card_count;
            return 0;
        }
        if (g_load_step >= 4 + g_prewarm_face_card_count &&
            g_load_step < 4 + g_prewarm_face_card_count + g_prewarm_big_card_count) {
            int prewarm_index = g_load_step - 4 - g_prewarm_face_card_count;
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
    case WAIFU_ASSET_REQUEST_ENDING: return g_load_step ? 100 : 0;
    case WAIFU_ASSET_REQUEST_STORY_INTRO:
    case WAIFU_ASSET_REQUEST_STORY_DUEL: return (g_load_step * 100) / PORTRAIT_SLOT_COUNT;
    case WAIFU_ASSET_REQUEST_CARDS: {
        int total = 4 + g_prewarm_face_card_count + g_prewarm_big_card_count;
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
#if defined(WAIFU_FM_CD32X)
        if (g_load_step < 4 + g_prewarm_face_card_count) return "CARD FACE";
#endif
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

void waifu_assets_title_screen_dims(int *w, int *h)
{
    if (w) *w = WAIFU_TITLE_ASSET_W;
    if (h) *h = WAIFU_TITLE_ASSET_H;
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
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
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
#if defined(WAIFU_FM_PCFX) || defined(WAIFU_FM_CD32X)
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

const uint8_t *waifu_assets_ending_screen_img(void)
{
#ifdef WAIFU_ASSET_EXTERNAL_ENDING_IMAGE
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM && defined(WAIFU_FM_CD32X)
    return g_ending_loaded ? stage_title_ptr() : NULL;
#else
    return NULL;
#endif
#else
    return ending_screen_img;
#endif
}

const uint16_t *waifu_assets_ending_screen_pcfx_yuv422(void)
{
#ifdef WAIFU_ASSET_EXTERNAL_ENDING_IMAGE
    return NULL;
#else
    return ending_screen_pcfx_yuv422;
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

#if defined(WAIFU_FM_CD32X)
static void cd32x_card_face_cache_reset(void)
{
    for (int i = 0; i < CD32X_CARD_FACE_CACHE_SLOTS; ++i) {
        g_cd32x_face_cache_card_id[i] = -1;
        g_cd32x_face_cache_stamp[i] = 0;
    }
    g_cd32x_face_cache_clock = 1;
}

static int cd32x_find_card_face_slot(int card_id)
{
    for (int i = 0; i < CD32X_CARD_FACE_CACHE_SLOTS; ++i) {
        if (g_cd32x_face_cache_card_id[i] == card_id) return i;
    }
    return -1;
}

static int cd32x_choose_card_face_slot(void)
{
    int best = 0;
    unsigned best_stamp = g_cd32x_face_cache_stamp[0];
    for (int i = 0; i < CD32X_CARD_FACE_CACHE_SLOTS; ++i) {
        if (g_cd32x_face_cache_card_id[i] < 0) return i;
        if (g_cd32x_face_cache_stamp[i] < best_stamp) {
            best = i;
            best_stamp = g_cd32x_face_cache_stamp[i];
        }
    }
    return best;
}
#endif

static int load_all_big_card_art_cached(void)
{
#if WAIFU_ASSET_BIG_CACHE_SLOTS >= WAIFU_CARD_COUNT
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
#else
    /* CD32X and other constrained CD builds keep the same LRU API but cannot
       reserve a full-card big-art cache.  A request to prewarm every card is
       treated as a no-op; individual card art remains demand-streamed through
       load_big_card_art_cached() and the small slot count selected by the port. */
    return 1;
#endif
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
#if defined(WAIFU_FM_CD32X)
    {
        int slot = cd32x_find_card_face_slot(card_id);
        uint8_t *ptr;
        if (slot < 0) {
            slot = cd32x_choose_card_face_slot();
            ptr = stage_card_face_cache_slot_ptr(slot);
            if (!cd_read_blob_slice(WAIFU_ASSET_BLOB_CARD_FACES,
                                    ptr,
                                    (size_t)card_id * CARD_ONE_BYTES,
                                    CARD_ONE_BYTES)) {
                g_cd32x_face_cache_card_id[slot] = -1;
                return NULL;
            }
            g_cd32x_face_cache_card_id[slot] = card_id;
        }
        g_cd32x_face_cache_stamp[slot] = g_cd32x_face_cache_clock++;
        if (g_cd32x_face_cache_clock == 0) g_cd32x_face_cache_clock = 1;
        return stage_card_face_cache_slot_ptr(slot);
    }
#else
    return stage_card_faces_ptr() + ((size_t)card_id * CARD_ONE_BYTES);
#endif
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

int waifu_assets_prewarm_big_art_pair(int card_a, int card_b)
{
#if WAIFU_ASSET_ACTIVE_BACKEND == WAIFU_ASSET_KIND_CDROM
    int ok = 1;
    if (!g_cards_loaded) return 0;
    if (card_a >= 0 && card_a < WAIFU_CARD_COUNT) {
        if (!load_big_card_art_cached(card_a)) ok = 0;
    }
    if (card_b >= 0 && card_b < WAIFU_CARD_COUNT && card_b != card_a) {
        if (!load_big_card_art_cached(card_b)) ok = 0;
    }
    return ok;
#else
    (void)card_a;
    (void)card_b;
    return 1;
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
