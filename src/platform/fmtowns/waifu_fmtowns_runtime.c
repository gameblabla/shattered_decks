/* Minimal freestanding C runtime for the FM TOWNS Marty build.
 * Keep this target-local so the host/headless, PC-FX and CD32X builds
 * continue using their normal C libraries.  Modeled on
 * src/platform/cd32x/waifu_cd32x_runtime.c -- same reasoning applies here:
 * -ffreestanding gives us no libc, and gcc still emits calls to these for
 * ordinary C (struct copies, array init, string handling in the portable
 * game core). */
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>

/* sounds.c's WAV-capture debug path (waifu_sound_wav_open/write/close/abort)
 * is unconditionally compiled (not gated behind WAIFU_FM_NO_HEADLESS_MAIN --
 * unlike the command-file player, it is not headless-only tooling) but has
 * no use on a console with no filesystem to write to.  fopen() always
 * failing makes every caller take its existing "could not open" failure
 * path; the rest exist only so those (now dead, but not provably-to-the-
 * compiler-dead in a -shared/-Bsymbolic link where they stay exported)
 * calls still link. */
FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int fclose(FILE *fp) { (void)fp; return 0; }
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) { (void)ptr; (void)size; (void)nmemb; (void)fp; return 0; }
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) { (void)ptr; (void)size; (void)nmemb; (void)fp; return 0; }
int fseek(FILE *fp, long offset, int whence) { (void)fp; (void)offset; (void)whence; return -1; }
long ftell(FILE *fp) { (void)fp; return -1; }
int ferror(FILE *fp) { (void)fp; return 1; }
int fputc(int c, FILE *fp) { (void)c; (void)fp; return -1; }
char *fgets(char *s, int size, FILE *fp) { (void)s; (void)size; (void)fp; return NULL; }
int fprintf(FILE *fp, const char *fmt, ...) { (void)fp; (void)fmt; return 0; }

/* -------------------------------------------------------------------
 * Block moves.
 *
 * These are not incidental libc filler, they are the hottest code in the
 * port.  The portable game core reaches them through src/main.c's
 * fill_u8_fast()/copy_u8_fast(), which on a target with no hand-written
 * backend fall through to plain memset/memcpy -- so a full-screen clear is
 * one memset of 61440 bytes, and every card, panel and glyph blit is a
 * stack of per-row memcpys.
 *
 * memcpy uses `rep movsl`: four bytes moved per iteration with the loop
 * counter in hardware, instead of a load/store/increment/branch sequence
 * per byte.  Measured in a duel, on the same scene from the same input
 * script, that took the game step from 4.5 ms to 1.7 ms per frame.  The
 * destination is aligned first because an unaligned dword access costs an
 * extra bus cycle on every single transfer on the 386SX's 16-bit bus, and
 * card blits land on odd addresses constantly.  Blocks under BLOCK_MIN take
 * the byte loop: three `rep` instructions plus the alignment arithmetic is
 * a fixed cost that a handful of bytes cannot repay.
 *
 * memset is deliberately NOT given the same treatment, however obvious it
 * looks.  It was tried, and on the same fixed scene it took the game step
 * from 4.5 ms to 50.7 ms -- an 11x regression -- with memcpy left alone, so
 * the attribution is not in doubt.  What is in doubt is the mechanism: a
 * standalone in-payload benchmark of the very same routine says `rep stosl`
 * beats the byte loop at every size tried (2.5x at 64 bytes, 4.4x at 256,
 * 6x at 61440), so this is something about how the fills interleave with
 * the rest of a real frame under Tsugaru, not about the instruction being
 * slow in isolation.  Nobody has run this on a physical Marty, where the
 * 386's published timings say `rep stosd` should win.  Until someone does,
 * the emulator is the only oracle there is, and it says byte loop -- so if
 * you are about to "fix" this, measure it on a fixed scene first (see
 * STATUS.md's "Performance" section for the method).
 *
 * -fno-builtin (Makefile.fmtowns) keeps gcc from recognising these bodies
 * and rewriting them into calls to themselves.  head.S clears the
 * direction flag at boot and nothing sets it, so the string ops always
 * run forwards.
 */
#define BLOCK_MIN 32

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t head, words;

    if (n < BLOCK_MIN) {
        while (n--) *d++ = *s++;
        return dst;
    }

    /* Bring the destination to a 4-byte boundary before going dword-wide. */
    head = (size_t)(0u - (size_t)(uintptr_t)d) & 3u;
    n -= head;
    words = n >> 2;
    n &= 3u;

    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(head) :: "memory");
    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(words) :: "memory");
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;
    /* Only a backwards-overlapping move needs the descending copy; every
     * other case, disjoint ones included, is exactly memcpy. */
    if (d < s || d >= s + n) return memcpy(dst, src, n);

    /* Descending.  Left a byte loop on purpose: the game core's overlapping
     * moves are rare and short (text and list scrolls), so the alignment
     * dance a reversed rep movsl needs would cost more than it saves. */
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

/* Byte loop on purpose -- see the block comment above before changing it. */
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
