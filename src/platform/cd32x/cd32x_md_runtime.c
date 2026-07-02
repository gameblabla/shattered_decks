#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *out = dst;
    while (n && *src) {
        *dst++ = *src++;
        --n;
    }
    while (n) {
        *dst++ = '\0';
        --n;
    }
    return out;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
