/* Host (SDL 1.2 / headless) implementation of the asset blob backend.
 *
 * Common asset code (src/game/assets.c) owns the blob catalogue, slicing and
 * RAM caching; it pulls raw bytes exclusively through the platform seam
 * waifu_assets_platform_read_blob_slice(). On the host that seam is backed by
 * the generated *.bin files under assets/generated/ via stdio, which also lets
 * the headless tests exercise the same staged CD-ROM load path the PC-FX build
 * uses on real hardware.
 *
 * Other targets supply their own translation unit instead of this one:
 *   - PC-FX: src/platform/pcfx/waifu_pcfx_cdrom.c (CD/SCSI DMA into KRAM)
 *   - cart-ROM consoles: a memory-mapped ROM reader
 *   - RAM-only: a reader over a preloaded buffer
 * so no <stdio.h> (or any file API) is pulled into the common game code. */

#include "assets.h"

#include <stddef.h>
#include <string.h>
#include <stdio.h>

static const char *blob_path(WaifuAssetBlobId blob)
{
    switch (blob) {
    case WAIFU_ASSET_BLOB_TITLE_SCREEN: return "assets/generated/title_screen_img.bin";
    case WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV16: return "assets/generated/title_screen_pcfx_yuv16.bin";
    case WAIFU_ASSET_BLOB_TITLE_SCREEN_PCFX_YUV422: return "assets/generated/title_screen_pcfx_yuv422.bin";
    case WAIFU_ASSET_BLOB_ENDING_SCREEN_PCFX_YUV422: return "assets/generated/ending_screen_pcfx_yuv422.bin";
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

#if defined(WAIFU_FM_HEADLESS_TESTS)
static unsigned long g_debug_platform_read_count = 0;

unsigned long waifu_assets_debug_platform_read_count(void)
{
    return g_debug_platform_read_count;
}

void waifu_assets_debug_reset_platform_read_count(void)
{
    g_debug_platform_read_count = 0;
}
#endif

int waifu_assets_platform_read_blob_slice(WaifuAssetBlobId blob, void *dst, size_t offset, size_t bytes)
{
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
}
