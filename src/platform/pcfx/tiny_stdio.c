/* Tiny PC-FX formatting shim.
   Purpose: avoid pulling newlib's vfprintf/snprintf stack into the V810 build.
   This is intentionally small and only supports the game's UI string patterns:
   %s, %.Ns, %c, %d, %u, %lu, %02d/%04d, %zu and %%.
   It is not a general libc replacement. */
#ifdef WAIFU_FM_PCFX
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static int tiny_putc(char **pp, size_t *rem, char c)
{
    if (*rem > 1) {
        **pp = c;
        ++(*pp);
        --(*rem);
    }
    return 1;
}

static int tiny_puts_n(char **pp, size_t *rem, const char *s, int maxn)
{
    int n = 0;
    if (!s) s = "";
    while (*s && (maxn < 0 || n < maxn)) {
        tiny_putc(pp, rem, *s++);
        ++n;
    }
    return n;
}

static int tiny_put_unsigned(char **pp, size_t *rem, unsigned long v, int width, int zero)
{
    char tmp[16];
    int n = 0;
    int out = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int)sizeof(tmp));
    while (n < width) { tiny_putc(pp, rem, zero ? '0' : ' '); ++out; --width; }
    while (n > 0) { tiny_putc(pp, rem, tmp[--n]); ++out; }
    return out;
}

static int tiny_put_signed(char **pp, size_t *rem, long v, int width, int zero)
{
    int out = 0;
    unsigned long u;
    if (v < 0) {
        tiny_putc(pp, rem, '-');
        ++out;
        u = (unsigned long)(-v);
        if (width > 0) --width;
    } else {
        u = (unsigned long)v;
    }
    return out + tiny_put_unsigned(pp, rem, u, width, zero);
}

int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap)
{
    char *p = dst;
    size_t rem = size;
    int total = 0;
    if (!fmt) fmt = "";
    while (*fmt) {
        if (*fmt != '%') {
            total += tiny_putc(&p, &rem, *fmt++);
            continue;
        }
        ++fmt;
        if (*fmt == '%') {
            total += tiny_putc(&p, &rem, *fmt++);
            continue;
        }

        int zero = 0;
        int width = 0;
        int precision = -1;
        int long_mod = 0;
        int size_mod = 0;

        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt++ - '0');
            }
        }
        if (*fmt == 'l') { long_mod = 1; ++fmt; }
        else if (*fmt == 'z') { size_mod = 1; ++fmt; }

        switch (*fmt) {
        case 's':
            total += tiny_puts_n(&p, &rem, va_arg(ap, const char *), precision);
            break;
        case 'c':
            total += tiny_putc(&p, &rem, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i':
            if (long_mod) total += tiny_put_signed(&p, &rem, va_arg(ap, long), width, zero);
            else if (size_mod) total += tiny_put_signed(&p, &rem, (long)va_arg(ap, size_t), width, zero);
            else total += tiny_put_signed(&p, &rem, (long)va_arg(ap, int), width, zero);
            break;
        case 'u':
            if (long_mod) total += tiny_put_unsigned(&p, &rem, va_arg(ap, unsigned long), width, zero);
            else if (size_mod) total += tiny_put_unsigned(&p, &rem, (unsigned long)va_arg(ap, size_t), width, zero);
            else total += tiny_put_unsigned(&p, &rem, (unsigned long)va_arg(ap, unsigned int), width, zero);
            break;
        default:
            /* Cheap failure mode: preserve enough text to spot unsupported use. */
            total += tiny_putc(&p, &rem, '%');
            if (*fmt) total += tiny_putc(&p, &rem, *fmt);
            break;
        }
        if (*fmt) ++fmt;
    }
    if (size) {
        if (rem > 0) *p = 0;
        else dst[size - 1] = 0;
    }
    return total;
}

int snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *dst, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(dst, (size_t)0x7fffffff, fmt, ap);
    va_end(ap);
    return r;
}
#endif
