/* Host (SDL 1.2 / headless) implementation of the platform storage seam.
 *
 * This is the *only* place in the host build that touches the filesystem for
 * save data; common game code serializes/deserializes the blob and persists it
 * through this stdio-backed interface. Other targets (PC-FX, ROM, RAM-only)
 * provide their own translation unit instead of this one. */

#include "platform.h"

#include <stdio.h>

int waifu_platform_storage_exists(const char *name)
{
    FILE *fp;
    if (!name) return 0;
    fp = fopen(name, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

int waifu_platform_storage_write(const char *name, const void *data, int len)
{
    FILE *fp;
    size_t written;
    if (!name || len < 0 || (!data && len > 0)) return 0;
    fp = fopen(name, "wb");
    if (!fp) return 0;
    written = (len > 0) ? fwrite(data, 1, (size_t)len, fp) : 0;
    if (fclose(fp) != 0) return 0;
    return ((int)written == len) ? len : 0;
}

int waifu_platform_storage_read(const char *name, void *data, int max_len)
{
    FILE *fp;
    size_t got;
    if (!name || !data || max_len < 0) return -1;
    fp = fopen(name, "rb");
    if (!fp) return -1;
    got = (max_len > 0) ? fread(data, 1, (size_t)max_len, fp) : 0;
    fclose(fp);
    return (int)got;
}
