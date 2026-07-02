/* Minimal freestanding C runtime for the CD32X SH-2 build.
 * Keep this target-local so the host/headless and PC-FX builds continue using
 * their normal C libraries. */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        ++pa;
        ++pb;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) { *d++ = *src++; --n; }
    while (n) { *d++ = '\0'; --n; }
    return dst;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

time_t time(time_t *out)
{
    static time_t t;
    ++t;
    if (out) *out = t;
    return t;
}

clock_t clock(void)
{
    static clock_t c;
    return ++c;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int snprintf(char *dst, size_t size, const char *fmt, ...)
{
    (void)fmt;
    if (dst && size) dst[0] = '\0';
    return 0;
}
