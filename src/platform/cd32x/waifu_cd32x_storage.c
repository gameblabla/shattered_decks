#include "platform.h"
#include "waifu_cd32x_cdrom.h"
#include <string.h>

/* CD32X save persistence.
 *
 * The story save blob is stored in a single fixed-size record in the Sega CD
 * internal Backup RAM (see cd32x_boot_main.c for the _BURAM service).  This
 * seam frames the caller's variable-length blob into that record as
 *   [0..1]  payload length, little-endian
 *   [2..]   payload bytes (zero-padded to the record size)
 * and caches it so the frequent title/menu story_save_exists() probe does not
 * hit Backup RAM every frame. */

#define CD32X_SAVE_HEADER_BYTES 2
#define CD32X_SAVE_PAYLOAD_CAP  (WAIFU_CD32X_SAVE_RECORD_BYTES - CD32X_SAVE_HEADER_BYTES)

static unsigned char g_record[WAIFU_CD32X_SAVE_RECORD_BYTES] __attribute__((aligned(4)));
static int g_record_len = -1;   /* payload length in g_record, -1 = not loaded */
static int g_exists = -1;       /* cached BRMSERCH result, -1 = unprobed */

static int record_payload_len(void)
{
    return (int)g_record[0] | ((int)g_record[1] << 8);
}

int waifu_platform_storage_exists(const char *name)
{
    (void)name;
    if (g_record_len >= 0) return g_record_len > 0 ? 1 : 0;
    if (g_exists < 0) g_exists = waifu_cd32x_save_exists() ? 1 : 0;
    return g_exists;
}

int waifu_platform_storage_write(const char *name, const void *data, int len)
{
    (void)name;
    if (len < 0 || len > CD32X_SAVE_PAYLOAD_CAP || (!data && len > 0)) return 0;

    memset(g_record, 0, sizeof(g_record));
    g_record[0] = (unsigned char)(len & 0xFF);
    g_record[1] = (unsigned char)((len >> 8) & 0xFF);
    if (len > 0) memcpy(g_record + CD32X_SAVE_HEADER_BYTES, data, (size_t)len);

    if (!waifu_cd32x_save_write_record(g_record)) {
        /* Leave the cache populated so an immediate retry/read still works even
           though the persistent write failed. */
        g_record_len = len;
        g_exists = 1;
        return 0;
    }
    g_record_len = len;
    g_exists = 1;
    return len;
}

int waifu_platform_storage_read(const char *name, void *data, int max_len)
{
    int n;
    (void)name;
    if (!data || max_len < 0) return -1;

    if (g_record_len < 0) {
        if (!waifu_cd32x_save_read_record(g_record)) { g_exists = 0; return -1; }
        n = record_payload_len();
        if (n < 0 || n > CD32X_SAVE_PAYLOAD_CAP) { g_record_len = 0; g_exists = 0; return -1; }
        g_record_len = n;
        g_exists = 1;
    }
    if (g_record_len <= 0) return -1;

    n = g_record_len;
    if (n > max_len) n = max_len;
    memcpy(data, g_record + CD32X_SAVE_HEADER_BYTES, (size_t)n);
    return n;
}
