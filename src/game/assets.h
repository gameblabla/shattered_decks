#ifndef WAIFU_FM_ASSETS_H
#define WAIFU_FM_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform-neutral large-asset access and staging.
 *
 * Build modes:
 *   default / WAIFU_ASSET_BACKEND_COMPILED:
 *       Asset bytes are compiled as const data and are addressed directly.
 *
 *   WAIFU_ASSET_BACKEND_CART_ROM:
 *       Asset bytes are addressed from ROM space. The reference build shares
 *       the compiled const pointer path; console ports can replace only this
 *       module with true ROM banking/CD filesystem providers.
 *
 *   WAIFU_ASSET_BACKEND_CDROM:
 *       Large visual banks are loaded into a small RAM staging area on demand.
 *       The game enters a black LOADING... state while this module fills the
 *       requested group. Title, story portraits, and card art are mutually
 *       staged so the title screen never loads card images into RAM.
 */
typedef enum WaifuAssetBackend {
    WAIFU_ASSET_BACKEND_COMPILED = 0,
    WAIFU_ASSET_BACKEND_CART_ROM = 1,
    WAIFU_ASSET_BACKEND_CDROM = 2
} WaifuAssetBackend;

typedef enum WaifuAssetRequest {
    WAIFU_ASSET_REQUEST_NONE = 0,
    WAIFU_ASSET_REQUEST_TITLE,
    WAIFU_ASSET_REQUEST_STORY_INTRO,
    WAIFU_ASSET_REQUEST_STORY_DUEL,
    WAIFU_ASSET_REQUEST_ENDING,
    WAIFU_ASSET_REQUEST_CARDS
} WaifuAssetRequest;

typedef enum WaifuAssetMemoryProfile {
    WAIFU_ASSET_MEMORY_512K = 512,
    WAIFU_ASSET_MEMORY_2MB = 2048,
    WAIFU_ASSET_MEMORY_LARGE = 4096
} WaifuAssetMemoryProfile;


typedef enum WaifuAssetBlobId {
    WAIFU_ASSET_BLOB_TITLE_SCREEN = 0,
    WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV16,
    WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV422,
    WAIFU_ASSET_BLOB_ENDING_SCREEN_PCFX_YUV422,
    WAIFU_ASSET_BLOB_STORY_PORTRAITS,
    WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK,
    WAIFU_ASSET_BLOB_CARD_FACES,
    WAIFU_ASSET_BLOB_CARD_BIG_ART,
    WAIFU_ASSET_BLOB_CARD_BIG_ART_CD,
    WAIFU_ASSET_BLOB_CARD_BACK,
    WAIFU_ASSET_BLOB_SUPPORT_FACE,
    WAIFU_ASSET_BLOB_SUPPORT_BIG_ART,
    WAIFU_ASSET_BLOB_SUPPORT_BIG_ART_CD
} WaifuAssetBlobId;

typedef enum WaifuBigArtKind {
    WAIFU_BIG_ART_CARD = 0,
    WAIFU_BIG_ART_SUPPORT = 1
} WaifuBigArtKind;

#ifndef WAIFU_ASSET_BIG_ART_DRAW_MAX
#define WAIFU_ASSET_BIG_ART_DRAW_MAX 8
#endif

typedef struct WaifuBigArtDraw {
    WaifuBigArtKind kind;
    int card_id;
    int x;
    int y;
} WaifuBigArtDraw;

typedef struct WaifuAssetBlobSlice {
    WaifuAssetBlobId blob;
    size_t offset;
    size_t bytes;
} WaifuAssetBlobSlice;

/* Optional platform override. PC/Unix uses the default stdio reader in assets.c;
 * PC-FX can implement this with CD/SCSI reads from the cdlink LBA table. */
int waifu_assets_platform_read_blob_slice(WaifuAssetBlobId blob, void *dst, size_t offset, size_t bytes);

/* Per-frame large-art draw queue. The renderer notes exact 112x112 art draws;
 * platform presenters may use it to replace normal CPU->VRAM upload of that
 * rectangle with a more appropriate hardware sprite/CD path. */
void waifu_assets_big_art_draw_queue_reset(void);
void waifu_assets_note_big_art_draw(WaifuBigArtKind kind, int card_id, int x, int y);
int waifu_assets_big_art_draw_count(void);
const WaifuBigArtDraw *waifu_assets_big_art_draws(void);
int waifu_assets_big_art_blob_slice(WaifuBigArtKind kind, int card_id, WaifuAssetBlobSlice *out);

void waifu_assets_init(void);
void waifu_assets_reset(void);
WaifuAssetBackend waifu_assets_backend(void);
const char *waifu_assets_backend_name(void);
WaifuAssetMemoryProfile waifu_assets_memory_profile(void);
size_t waifu_assets_ram_budget_bytes(void);
size_t waifu_assets_ram_used_bytes(void);
size_t waifu_assets_ram_high_water_bytes(void);
int waifu_assets_ram_budget_ok(void);

/* Request one resident working set. CD-ROM builds may need several load steps;
 * compiled/cart builds complete immediately. Story duel requests stage Serena
 * plus the current opponent portrait only, not all story portraits. */
void waifu_assets_request_title(void);
void waifu_assets_request_story_intro(void);
void waifu_assets_request_story_duel(int opponent_portrait_id);
void waifu_assets_request_ending(void);
void waifu_assets_request_cards(void);
/* Request card working set and pre-warm likely large monster art. The list is copied; support cards imply support big-art prewarm. */
void waifu_assets_request_cards_for_list(const int *card_ids, int count);
WaifuAssetRequest waifu_assets_pending_request(void);
const char *waifu_assets_request_name(WaifuAssetRequest req);

int waifu_assets_ready(void);
int waifu_assets_needs_loading_screen(void);
int waifu_assets_load_step(void);
int waifu_assets_loading_percent(void);
const char *waifu_assets_loading_label(void);

int waifu_assets_title_ready(void);
int waifu_assets_cards_ready(void);
int waifu_assets_story_portrait_ready(int portrait_id);

const uint8_t *waifu_assets_title_screen_img(void);
void waifu_assets_title_screen_dims(int *w, int *h);
const uint16_t *waifu_assets_title_screen_pcfx_yuv16(void);
const uint16_t *waifu_assets_title_screen_pcfx_yuv422(void);
const uint8_t *waifu_assets_ending_screen_img(void);
const uint16_t *waifu_assets_ending_screen_pcfx_yuv422(void);
const uint8_t *waifu_assets_story_portrait_pixels(int portrait_id);
const uint8_t *waifu_assets_story_portrait_mask(int portrait_id);
const uint8_t *waifu_assets_card_face(int card_id);
const uint8_t *waifu_assets_card_big_art(int card_id);
/* Synchronously load the exact monster big-art pair needed by an imminent
 * reveal/cut-in into the resident cache.  Compiled/cart builds are already
 * resident and return success. */
int waifu_assets_prewarm_big_art_pair(int card_a, int card_b);
/* Non-blocking large-art cache probes.  CD-ROM builds return NULL on a miss;
 * they never issue a CD/SCSI read.  Presenters use these to upload known-resident
 * card art directly to hardware without risking a frame-time stall. */
const uint8_t *waifu_assets_card_big_art_cached(int card_id);
const uint8_t *waifu_assets_support_big_art_cached(void);
const uint8_t *waifu_assets_big_art_cached(WaifuBigArtKind kind, int card_id);
int waifu_assets_big_art_cache_loaded_count(void);
int waifu_assets_big_art_cache_slot_count(void);
int waifu_assets_big_art_cache_contains(int card_id);
int waifu_assets_support_big_art_loaded(void);
#if defined(WAIFU_FM_HEADLESS_TESTS)
unsigned long waifu_assets_debug_platform_read_count(void);
void waifu_assets_debug_reset_platform_read_count(void);
#endif
const uint8_t *waifu_assets_card_back(void);
const uint8_t *waifu_assets_support_face(void);
const uint8_t *waifu_assets_support_big_art(void);

#ifdef __cplusplus
}
#endif

#endif /* WAIFU_FM_ASSETS_H */
