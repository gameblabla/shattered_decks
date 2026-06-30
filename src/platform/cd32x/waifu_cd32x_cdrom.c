/* SH-2 side of the CD32X CD-ROM/CD-DA seam.
 *
 * The concrete ISO9660 reader and CD-DA control live on the resident Sega CD
 * supervisor and are derived from RaycastDemo + Megadev.  This file exposes the
 * generic asset/backend hooks to common code while keeping that supervisor
 * protocol behind one target-specific module. */
#include "waifu_cd32x_cdrom.h"
#include "assets.h"
#include "waifu_assets.h"
#include "cd32x_32x.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CD32X_COMM_READY        0x0001u
#define CD32X_COMM_XFER_WORD    0x0003u
#define CD32X_COMM_XFER_DONE    0xFFFFu
#define CD32X_CD_CMD_READ_BLOB  0xCD01u
#define CD32X_MD_CMD_CDDA_PLAY  0xCD03u
#define CD32X_MD_CMD_CDDA_STOP  0xCD04u
#define CD32X_CD_STATUS_ERROR   0xCDEEu
#define CD32X_CARD_FACE_CHUNK_BYTES 32768u
#define CD32X_CARD_FACE_CHUNK_COUNT 5u
#define CD32X_PRIV_BLOB_CARD_FACE_0 0x0100
#define CD32X_PRIV_BLOB_CARD_SINGLE_0 0x0200
#define CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0 0x0300
#define CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0 0x0400
#define CD32X_PRIV_BLOB_PORTRAIT_MASK_0 0x0500
#define CD32X_CARD_ONE_BYTES ((size_t)WAIFU_CARD_W * (size_t)WAIFU_CARD_H)
#define CD32X_CARD_BIG_ONE_BYTES ((size_t)WAIFU_BIG_W * (size_t)WAIFU_BIG_H)
#define CD32X_CD_SECTOR_BYTES 2048u
#define CD32X_CARD_BIG_CD_SLOT_BYTES (((CD32X_CARD_BIG_ONE_BYTES + CD32X_CD_SECTOR_BYTES - 1u) / CD32X_CD_SECTOR_BYTES) * CD32X_CD_SECTOR_BYTES)
#ifndef WAIFU_STORY_PORTRAIT_CD_STRIDE
#define WAIFU_STORY_PORTRAIT_CD_STRIDE (((size_t)WAIFU_STORY_PORTRAIT_W * (size_t)WAIFU_STORY_PORTRAIT_H + CD32X_CD_SECTOR_BYTES - 1u) & ~(CD32X_CD_SECTOR_BYTES - 1u))
#endif

struct WaifuCd32xCdrom {
    uint32_t cd_read_seq;
    uint8_t last_cdda_track;
};

typedef struct BlobPathEntry {
    WaifuAssetBlobId blob;
    const char *path;
} BlobPathEntry;

static WaifuCd32xCdrom g_cdrom;

static int cd32x_request_blob_raw(int blob, void *dst, size_t bytes);

static const BlobPathEntry g_blob_paths[] = {
    { WAIFU_ASSET_BLOB_TITLE_SCREEN, "/ASSETS/TITLE_SCREEN_IMG.BIN;1" },
    { WAIFU_ASSET_BLOB_STORY_PORTRAITS, "/ASSETS/STORY_PORTRAITS.BIN;1" },
    { WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK, "/ASSETS/STORY_PORTRAIT_MASK.BIN;1" },
    { WAIFU_ASSET_BLOB_CARD_FACES, "/ASSETS/CARD_FACES.BIN;1" },
    { WAIFU_ASSET_BLOB_CARD_BIG_ART, "/ASSETS/CARD_BIG_ART.BIN;1" },
    { WAIFU_ASSET_BLOB_CARD_BIG_ART_CD, "/ASSETS/CARD_BIG_ART_CD.BIN;1" },
    { WAIFU_ASSET_BLOB_CARD_BACK, "/ASSETS/CARD_BACK.BIN;1" },
    { WAIFU_ASSET_BLOB_SUPPORT_FACE, "/ASSETS/SUPPORT_FACE.BIN;1" },
    { WAIFU_ASSET_BLOB_SUPPORT_BIG_ART, "/ASSETS/SUPPORT_BIG_ART.BIN;1" },
    { WAIFU_ASSET_BLOB_SUPPORT_BIG_ART_CD, "/ASSETS/SUPPORT_BIG_ART_CD.BIN;1" }
};

WaifuCd32xCdrom *waifu_cd32x_cdrom_create(void)
{
    memset(&g_cdrom, 0, sizeof(g_cdrom));
    return &g_cdrom;
}

void waifu_cd32x_cdrom_destroy(WaifuCd32xCdrom *cdrom)
{
    (void)cdrom;
}

static const char *blob_path(WaifuAssetBlobId blob)
{
    size_t i;
    for (i = 0; i < sizeof(g_blob_paths) / sizeof(g_blob_paths[0]); ++i) {
        if (g_blob_paths[i].blob == blob) return g_blob_paths[i].path;
    }
    return 0;
}

static int blob_from_path(const char *path, WaifuAssetBlobId *out)
{
    size_t i;
    if (!path || !out) return 0;
    for (i = 0; i < sizeof(g_blob_paths) / sizeof(g_blob_paths[0]); ++i) {
        if (strcmp(path, g_blob_paths[i].path) == 0) {
            *out = g_blob_paths[i].blob;
            return 1;
        }
    }
    return 0;
}

static int cd32x_wait_supervisor_idle(void)
{
    uint32_t timeout;
    for (timeout = 0; MARS_SYS_COMM0 != 0u && timeout < 0x01FFFFFFu; ++timeout) {
    }
    return MARS_SYS_COMM0 == 0u;
}

static int cd32x_request_blob(WaifuAssetBlobId blob, void *dst, size_t bytes)
{
    return cd32x_request_blob_raw((int)blob, dst, bytes);
}

static int cd32x_request_blob_raw(int blob, void *dst, size_t bytes)
{
    uint8_t *out = (uint8_t *)dst;
    volatile uint16_t *out16 = (volatile uint16_t *)dst;
    size_t remaining;
    size_t written = 0;
    uint16_t words;
    int word_aligned;

    if (!out || bytes == 0u) return 0;
    if (!cd32x_wait_supervisor_idle()) return 0;
    words = (uint16_t)((bytes + 1u) >> 1);
    if (((size_t)words << 1) < bytes) return 0;
    word_aligned = ((((uintptr_t)dst) & 1u) == 0u) && ((bytes & 1u) == 0u);

    /* COMM0=1 is intentionally reused from Raycaster's cpy_to_32x handshake.
       The MD-side loop leaves that value alone, so the resident Sub-CPU
       supervisor can see the request and then stream words through COMM2. */
    MARS_SYS_COMM2 = words;
    MARS_SYS_COMM4 = CD32X_CD_CMD_READ_BLOB;
    MARS_SYS_COMM6 = (uint16_t)blob;
    MARS_SYS_COMM0 = CD32X_COMM_READY;

    remaining = bytes;
    {
        /* Bounded waits so a stalled supervisor (e.g. a CD read that never
           completes) degrades to a clean failure instead of freezing the whole
           SH-2.  The initial window is generous because the supervisor may be
           seeking/reading sectors before it starts streaming. */
        uint32_t guard = 0;
        for (;;) {
            uint16_t state = MARS_SYS_COMM0;
            if (state == CD32X_COMM_XFER_WORD) break;
            if (state == 0u && MARS_SYS_COMM4 == CD32X_CD_STATUS_ERROR) return 0;
            if (++guard >= 0x08000000u) { MARS_SYS_COMM0 = 0; return 0; }
        }
    }

    if (word_aligned) {
        size_t word_count = bytes >> 1;
        for (size_t i = 0; i < word_count; ++i) {
            uint32_t guard = 0;
            while (MARS_SYS_COMM0 != CD32X_COMM_XFER_WORD) {
                if (++guard >= 0x02000000u) { MARS_SYS_COMM0 = 0; return 0; }
            }
            out16[i] = MARS_SYS_COMM2;
            MARS_SYS_COMM0 = CD32X_COMM_READY;
        }
    } else {
        while (remaining) {
            uint16_t value;
            uint32_t guard = 0;
            while (MARS_SYS_COMM0 != CD32X_COMM_XFER_WORD) {
                if (++guard >= 0x02000000u) { MARS_SYS_COMM0 = 0; return 0; }
            }
            value = MARS_SYS_COMM2;
            out[written++] = (uint8_t)(value >> 8);
            --remaining;
            if (remaining) {
                out[written++] = (uint8_t)value;
                --remaining;
            }
            MARS_SYS_COMM0 = CD32X_COMM_READY;
        }
    }

    {
        uint32_t guard = 0;
        while (MARS_SYS_COMM0 != CD32X_COMM_XFER_DONE) {
            if (++guard >= 0x02000000u) { MARS_SYS_COMM0 = 0; return 0; }
        }
    }
    MARS_SYS_COMM0 = 0;
    return 1;
}

int waifu_cd32x_cdrom_read_file(const char *path, void *dst, size_t dst_size, size_t *bytes_read)
{
    WaifuAssetBlobId blob;
    if (bytes_read) *bytes_read = 0;
    if (!blob_from_path(path, &blob)) return 0;
    if (!cd32x_request_blob(blob, dst, dst_size)) return 0;
    if (bytes_read) *bytes_read = dst_size;
    ++g_cdrom.cd_read_seq;
    return 1;
}

int waifu_assets_platform_read_blob_slice(WaifuAssetBlobId blob, void *dst, size_t offset, size_t bytes)
{
    const char *path = blob_path(blob);
    size_t got = 0;
    uint8_t *out = (uint8_t *)dst;
    if (!dst) return 0;

    if (blob == WAIFU_ASSET_BLOB_CARD_FACES) {
        if (offset % CD32X_CARD_ONE_BYTES == 0u && bytes == CD32X_CARD_ONE_BYTES) {
            size_t card_id = offset / CD32X_CARD_ONE_BYTES;
            if (card_id >= WAIFU_CARD_COUNT) return 0;
            return cd32x_request_blob_raw((int)CD32X_PRIV_BLOB_CARD_SINGLE_0 + (int)card_id, out, bytes);
        }
        if (offset == 0u) {
            size_t done = 0;
            unsigned chunk = 0;
            while (done < bytes && chunk < CD32X_CARD_FACE_CHUNK_COUNT) {
                size_t n = bytes - done;
                if (n > CD32X_CARD_FACE_CHUNK_BYTES) n = CD32X_CARD_FACE_CHUNK_BYTES;
                if (!cd32x_request_blob_raw((int)CD32X_PRIV_BLOB_CARD_FACE_0 + (int)chunk, out + done, n)) return 0;
                done += n;
                ++chunk;
            }
            return done >= bytes;
        }
    }
    if (blob == WAIFU_ASSET_BLOB_CARD_BIG_ART) {
        if (offset % CD32X_CARD_BIG_ONE_BYTES == 0u && bytes == CD32X_CARD_BIG_ONE_BYTES) {
            size_t card_id = offset / CD32X_CARD_BIG_ONE_BYTES;
            if (card_id >= WAIFU_CARD_COUNT) return 0;
            return cd32x_request_blob_raw((int)CD32X_PRIV_BLOB_CARD_BIG_SINGLE_0 + (int)card_id, out, bytes);
        }
    }
    if (blob == WAIFU_ASSET_BLOB_STORY_PORTRAITS ||
        blob == WAIFU_ASSET_BLOB_STORY_PORTRAIT_MASK) {
        if (offset % (size_t)WAIFU_STORY_PORTRAIT_CD_STRIDE == 0u &&
            bytes == (size_t)WAIFU_STORY_PORTRAIT_CD_STRIDE) {
            size_t portrait_id = offset / (size_t)WAIFU_STORY_PORTRAIT_CD_STRIDE;
            int base = (blob == WAIFU_ASSET_BLOB_STORY_PORTRAITS)
                ? CD32X_PRIV_BLOB_PORTRAIT_PIXELS_0
                : CD32X_PRIV_BLOB_PORTRAIT_MASK_0;
            if (portrait_id >= WAIFU_STORY_PORTRAIT_COUNT) return 0;
            return cd32x_request_blob_raw(base + (int)portrait_id, out, bytes);
        }
    }

    if (!path) return 0;
    /* CD32X currently supports start-aligned reads plus the chunked card-face
       working set needed by battle mode. Other offset slices remain disabled
       until those assets get either per-file CD layout or an offset-aware
       supervisor command. */
    if (offset != 0) return 0;
    return waifu_cd32x_cdrom_read_file(path, dst, bytes, &got) && got >= bytes;
}

static int cd32x_supervisor_request(uint16_t cmd, uint16_t arg0, uint16_t arg1, int wait_for_completion)
{
    uint32_t timeout;

    if (!cd32x_wait_supervisor_idle()) return 0;

    MARS_SYS_COMM2 = arg0;
    MARS_SYS_COMM6 = arg1;
    MARS_SYS_COMM4 = cmd;
    MARS_SYS_COMM0 = CD32X_COMM_READY;

    if (!wait_for_completion) return 1;

    for (timeout = 0; MARS_SYS_COMM0 != 0u && timeout < 0x03FFFFFFu; ++timeout) {
    }
    if (MARS_SYS_COMM0 != 0u) return 0;
    return MARS_SYS_COMM4 != CD32X_CD_STATUS_ERROR;
}

int waifu_cd32x_cdda_play(uint8_t track, uint8_t loop)
{
    if (track < 2u) return 0;
    if (cd32x_supervisor_request(CD32X_MD_CMD_CDDA_PLAY, track, loop ? 1u : 0u, 1)) {
        g_cdrom.last_cdda_track = track;
        return 1;
    }
    return 0;
}

int waifu_cd32x_cdda_stop(void)
{
    if (cd32x_supervisor_request(CD32X_MD_CMD_CDDA_STOP, 0, 0, 1)) {
        g_cdrom.last_cdda_track = 0;
        return 1;
    }
    return 0;
}
