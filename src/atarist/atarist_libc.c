/* ─────────────────────────────────────────────────────────────────────────────
 *  atarist_libc.c — the handful of C library entries a -nostdlib TOS build
 *  still needs.
 *
 *  GCC open-codes calls to memset/memcpy for struct assignment and array
 *  initialisation whatever the source says, and src/game/deck.c seeds its RNG
 *  from time()/clock().  Linking MiNTlib for those five symbols would cost a
 *  quarter of a megabyte on a machine that has half of one, so they live here.
 *  The clock sources are the port's own VBL counter, which is genuinely
 *  unpredictable relative to when a player presses a key -- the same argument
 *  the MSX2 fork makes in msx2_libc.c.
 * ───────────────────────────────────────────────────────────────────────────── */

#include <stdint.h>
#include <stddef.h>

extern volatile uint32_t g_atarist_vbl;

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;
    /* Long-align, then long-fill: byte-at-a-time clears of a 32000-byte screen
     * are four times slower than they need to be on a 68000. */
    while (n && ((uint32_t)d & 3u)) { *d++ = v; --n; }
    if (n >= 4) {
        uint32_t w = ((uint32_t)v << 24) | ((uint32_t)v << 16) |
                     ((uint32_t)v << 8) | v;
        uint32_t *p = (uint32_t *)d;
        size_t longs = n >> 2;
        n &= 3u;
        while (longs--) *p++ = w;
        d = (uint8_t *)p;
    }
    while (n--) *d++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if ((((uint32_t)d ^ (uint32_t)s) & 1u) == 0u) {
        while (n && ((uint32_t)d & 3u)) { *d++ = *s++; --n; }
        if (n >= 4) {
            uint32_t *p = (uint32_t *)d;
            const uint32_t *q = (const uint32_t *)s;
            size_t longs = n >> 2;
            n &= 3u;
            while (longs--) *p++ = *q++;
            d = (uint8_t *)p;
            s = (const uint8_t *)q;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || !n) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n; s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    while (n--) { if (*p != *q) return (int)*p - (int)*q; ++p; ++q; }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) { }
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* deck.c mixes these into its seed.  A vblank counter is the only clock the
 * game keeps, and it is exactly the entropy that matters here. */
unsigned long time(unsigned long *t)
{
    unsigned long now = (unsigned long)g_atarist_vbl;
    if (t) *t = now;
    return now;
}

long clock(void)
{
    return (long)g_atarist_vbl;
}
