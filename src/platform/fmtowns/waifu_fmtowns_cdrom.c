/* CD asset backend for src/game/assets.c's WAIFU_ASSET_USE_CDROM builds on
 * FM TOWNS Marty.  Resolves each WaifuAssetBlobId to an 8.3 ISO9660 filename
 * staged on the data track (see Makefile.fmtowns's $(CDROOT) rules) and reads
 * the requested byte range through fmt_iso9660_find()/fmt_cdrom_read()
 * (src/platform/fmtowns/common/iso9660.c + cdrom.c, both already proven
 * booting since milestone 3 -- see STATUS.md).
 *
 * Modeled on waifu_pcfx_cdrom.c's waifu_assets_platform_read_blob_slice():
 * one LBA lookup per blob (cached), a sector-aligned fast path straight into
 * the caller's buffer, and a one-sector scratch buffer for unaligned
 * heads/tails.  FM TOWNS CD-ROM sectors are also 2048 bytes (Mode 1), same
 * as PC-FX, so the same shape applies directly. */
#include "assets.h"
#include "iso9660.h"
#include "cdrom.h"
#include "fmtowns_audio.h"
#include "fmtowns_cd_diag.h"
#include <string.h>

#define FMT_CD_SECTOR_BYTES 2048u

static const char *blob_filename(WaifuAssetBlobId blob)
{
    switch (blob) {
    case WAIFU_ASSET_BLOB_TITLE_SCREEN:       return "TITLE.BIN";
    case WAIFU_ASSET_BLOB_STORY_PORTRAITS:    return "PORTRAIT.BIN";
    case WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK:return "PORTMASK.BIN";
    case WAIFU_ASSET_BLOB_CARD_FACES:         return "CARDFACE.BIN";
    case WAIFU_ASSET_BLOB_CARD_BIG_ART:       return "CARDBIG.BIN";
    case WAIFU_ASSET_BLOB_CARD_BACK:          return "CARDBACK.BIN";
    case WAIFU_ASSET_BLOB_SUPPORT_FACE:       return "SUPFACE.BIN";
    case WAIFU_ASSET_BLOB_SUPPORT_BIG_ART:    return "SUPBIG.BIN";
    case WAIFU_ASSET_BLOB_TEX_ATLAS:          return "TEXATLAS.BIN";
    default:                                  return 0; /* PC-FX-only YUV blobs: never requested (see assets.c) */
    }
}

/* One-entry cache: assets.c reads each blob's slices back-to-back (title,
 * then portraits, then cards, ...), never interleaved across blobs within a
 * single load step, so a single cached (blob, lba) pair avoids a repeat
 * fmt_iso9660_find() directory scan per slice. */
static WaifuAssetBlobId g_cached_blob = (WaifuAssetBlobId)-1;
static uint32_t g_cached_lba;
static uint32_t g_cached_size;

static int blob_lba(WaifuAssetBlobId blob, uint32_t *out_lba)
{
    const char *name;
    if (blob == g_cached_blob) { *out_lba = g_cached_lba; return 1; }
    name = blob_filename(blob);
    if (!name) return 0;
    /* Name the file before the directory scan, not after: everything the
     * diagnostic layer can report about a read that never comes back is
     * only as good as the last stage set (fmtowns_cd_diag.h). */
    fmtowns_cd_diag_stage(name);
    if (fmt_iso9660_find(name, &g_cached_lba, &g_cached_size) != 0) return 0;
    g_cached_blob = blob;
    *out_lba = g_cached_lba;
    return 1;
}

/* Every read the asset loader does, with retries and a breadcrumb, and with
 * the failure report shown rather than a silent "0" handed back to a core
 * that has no way to say what went wrong.  Returns 1 if the data is there.
 *
 * The core treats a failed slice as "this asset is missing" and carries on
 * drawing without it, which on a disc that has stopped answering means a
 * game made of blank cards.  Giving up is still offered -- the report has a
 * key for it -- but it is not what happens by default. */
static int diag_read(uint32_t lba, uint16_t count, void *buf)
{
    for (;;) {
        if (fmtowns_cd_diag_read(lba, count, buf) == 0) {
            return 1;
        }
        if (!fmtowns_cd_diag_report_failure()) {
            return 0;
        }
    }
}

int waifu_assets_platform_read_blob_slice(WaifuAssetBlobId blob, void *dst, size_t offset, size_t bytes)
{
    static uint8_t scratch[FMT_CD_SECTOR_BYTES] __attribute__((aligned(4)));
    uint32_t base_lba;
    uint8_t *out = (uint8_t *)dst;
    if (!dst) return 0;
    if (!blob_lba(blob, &base_lba)) return 0;

    /* Reading data sectors stops whatever CD-DA track is playing -- the drive
     * has one head (see cdda.h).  Tell the music layer so it restarts the
     * track once this burst of reads is over, instead of the game going
     * silent for the rest of the scene. */
    fmtowns_audio_note_data_read();

    /* Breadcrumbs are worth their VRAM stores while the loading screen is
     * up -- that is the screen a hang would freeze on -- and pure overhead
     * once the game is drawing its own frames over them. */
    fmtowns_cd_diag_breadcrumbs(!waifu_assets_ready());

    while (bytes > 0) {
        uint32_t lba = base_lba + (uint32_t)(offset / FMT_CD_SECTOR_BYTES);
        size_t sector_off = offset & (FMT_CD_SECTOR_BYTES - 1u);
        if (sector_off == 0 && bytes >= FMT_CD_SECTOR_BYTES) {
            size_t whole = bytes & ~((size_t)FMT_CD_SECTOR_BYTES - 1u);
            uint16_t count = (uint16_t)(whole / FMT_CD_SECTOR_BYTES);
            if (!diag_read(lba, count, out)) return 0;
            out += whole;
            offset += whole;
            bytes -= whole;
        } else {
            size_t chunk = FMT_CD_SECTOR_BYTES - sector_off;
            if (chunk > bytes) chunk = bytes;
            if (!diag_read(lba, 1, scratch)) return 0;
            memcpy(out, scratch + sector_off, chunk);
            out += chunk;
            offset += chunk;
            bytes -= chunk;
        }
    }
    return 1;
}
