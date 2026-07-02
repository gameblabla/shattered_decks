#include "platform.h"
#include <string.h>

#ifndef WAIFU_CD32X_SAVE_CAP
#define WAIFU_CD32X_SAVE_CAP 4096
#endif

static unsigned char g_save[WAIFU_CD32X_SAVE_CAP];
static int g_save_len = 0;

int waifu_platform_storage_exists(const char *name)
{
    (void)name;
    return g_save_len > 0;
}

int waifu_platform_storage_write(const char *name, const void *data, int len)
{
    (void)name;
    if (len < 0 || len > WAIFU_CD32X_SAVE_CAP || (!data && len > 0)) return 0;
    if (len > 0) memcpy(g_save, data, (size_t)len);
    g_save_len = len;
    return len;
}

int waifu_platform_storage_read(const char *name, void *data, int max_len)
{
    int n;
    (void)name;
    if (!data || max_len < 0 || g_save_len <= 0) return -1;
    n = g_save_len;
    if (n > max_len) n = max_len;
    memcpy(data, g_save, (size_t)n);
    return n;
}
