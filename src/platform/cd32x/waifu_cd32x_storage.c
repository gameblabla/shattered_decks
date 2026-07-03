#include "platform.h"
#include "waifu_cd32x_cdrom.h"
#include <string.h>

/* CD32X save persistence — two devices:
 *   device 0 = Sega CD internal Backup RAM (Sub-CPU _BURAM BIOS)
 *   device 1 = Backup RAM cartridge (Main-CPU memory-mapped)
 * (see cd32x_boot_main.c / cd32x_md_iface.s for the two backends).
 *
 * The story save blob is framed into a fixed-size record
 *   [0..1]  payload length, little-endian
 *   [2..]   payload bytes (zero-padded to the record size)
 * and cached per device so the frequent title/menu story_save_exists() probe
 * does not hit Backup RAM every frame. */

#define CD32X_SAVE_HEADER_BYTES 2
#define CD32X_SAVE_PAYLOAD_CAP  (WAIFU_CD32X_SAVE_RECORD_BYTES - CD32X_SAVE_HEADER_BYTES)
#define CD32X_SAVE_DEVICES      2

static unsigned char g_record[CD32X_SAVE_DEVICES][WAIFU_CD32X_SAVE_RECORD_BYTES] __attribute__((aligned(4)));
static int g_record_len[CD32X_SAVE_DEVICES] = { -1, -1 }; /* payload length, -1 = not loaded */
static int g_exists[CD32X_SAVE_DEVICES] = { -1, -1 };     /* cached exists, -1 = unprobed */

static int clamp_device(int device) { return device == 1 ? 1 : 0; }

static int record_payload_len(int d)
{
    return (int)g_record[d][0] | ((int)g_record[d][1] << 8);
}

int waifu_platform_storage_exists_dev(int device, const char *name)
{
    int d = clamp_device(device);
    (void)name;
    if (g_record_len[d] >= 0) return g_record_len[d] > 0 ? 1 : 0;
    if (g_exists[d] < 0) g_exists[d] = waifu_cd32x_save_exists(d) ? 1 : 0;
    return g_exists[d];
}

int waifu_platform_storage_write_dev(int device, const char *name, const void *data, int len)
{
    int d = clamp_device(device);
    (void)name;
    if (len < 0 || len > CD32X_SAVE_PAYLOAD_CAP || (!data && len > 0)) return 0;

    memset(g_record[d], 0, WAIFU_CD32X_SAVE_RECORD_BYTES);
    g_record[d][0] = (unsigned char)(len & 0xFF);
    g_record[d][1] = (unsigned char)((len >> 8) & 0xFF);
    if (len > 0) memcpy(g_record[d] + CD32X_SAVE_HEADER_BYTES, data, (size_t)len);

    if (!waifu_cd32x_save_write_record(g_record[d], d)) {
        /* Keep the cache populated so a same-session read still works even if the
           persistent write failed (e.g. no cart inserted). */
        g_record_len[d] = len;
        g_exists[d] = 1;
        return 0;
    }
    g_record_len[d] = len;
    g_exists[d] = 1;
    return len;
}

int waifu_platform_storage_read_dev(int device, const char *name, void *data, int max_len)
{
    int d = clamp_device(device);
    int n;
    (void)name;
    if (!data || max_len < 0) return -1;

    if (g_record_len[d] < 0) {
        if (!waifu_cd32x_save_read_record(g_record[d], d)) { g_exists[d] = 0; return -1; }
        n = record_payload_len(d);
        if (n < 0 || n > CD32X_SAVE_PAYLOAD_CAP) { g_record_len[d] = 0; g_exists[d] = 0; return -1; }
        g_record_len[d] = n;
        g_exists[d] = 1;
    }
    if (g_record_len[d] <= 0) return -1;

    n = g_record_len[d];
    if (n > max_len) n = max_len;
    memcpy(data, g_record[d] + CD32X_SAVE_HEADER_BYTES, (size_t)n);
    return n;
}

/* Single-device seam maps to internal Backup RAM (device 0). */
int waifu_platform_storage_exists(const char *name) { return waifu_platform_storage_exists_dev(0, name); }
int waifu_platform_storage_write(const char *name, const void *data, int len) { return waifu_platform_storage_write_dev(0, name, data, len); }
int waifu_platform_storage_read(const char *name, void *data, int max_len) { return waifu_platform_storage_read_dev(0, name, data, max_len); }
