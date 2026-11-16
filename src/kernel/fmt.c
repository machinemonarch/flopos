/* ===========================================================================
 * fmt.c -- just enough string formatting for an operating system with no
 * libc behind it.
 *
 * Supports %u %d %x %s %c %% with an optional field width and a leading
 * zero flag, so "%03u" works.  No floats, for the obvious reason: nothing
 * in FLOPOS touches the FPU.
 *
 * fmt_vstr does the work and fmt_str wraps it, so callers that already
 * hold a va_list -- term_printf, mainly -- do not have to reimplement the
 * loop.
 * ===========================================================================
 */
#include "flopos.h"

char *fmt_u32(char *dst, u32 v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) { *dst++ = '0'; return dst; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *dst++ = tmp[--n];
    return dst;
}

char *fmt_s32(char *dst, s32 v)
{
    if (v < 0) { *dst++ = '-'; return fmt_u32(dst, (u32)(-v)); }
    return fmt_u32(dst, (u32)v);
}

char *fmt_hex(char *dst, u32 v, int digits)
{
    static const char hexd[] = "0123456789ABCDEF";
    int i;
    if (digits < 1) {
        digits = 8;
        while (digits > 1 && ((v >> ((digits - 1) * 4)) & 0xF) == 0) digits--;
    }
    for (i = digits - 1; i >= 0; i--) *dst++ = hexd[(v >> (i * 4)) & 0xF];
    return dst;
}

void fmt_vstr(char *dst, usize cap, const char *fmt, __builtin_va_list ap)
{
    char scratch[24];
    char *out = dst;
    char *end = dst + cap - 1;

    while (*fmt && out < end) {
        int zero = 0, width = 0, len = 0, pad;
        char *s;

        if (*fmt != '%') { *out++ = *fmt++; continue; }
        fmt++;

        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        s = scratch;
        switch (*fmt++) {
        case 'u': *fmt_u32(scratch, __builtin_va_arg(ap, u32)) = 0; break;
        case 'd': *fmt_s32(scratch, __builtin_va_arg(ap, s32)) = 0; break;
        case 'x': *fmt_hex(scratch, __builtin_va_arg(ap, u32), 0) = 0; break;
        case 'c': scratch[0] = (char)__builtin_va_arg(ap, int);
                  scratch[1] = 0; break;
        case 's':
            s = (char *)__builtin_va_arg(ap, char *);
            if (!s) s = "(null)";
            break;
        case '%': scratch[0] = '%'; scratch[1] = 0; break;
        case 0:   *out = 0; return;
        default:  scratch[0] = '?'; scratch[1] = 0; break;
        }

        while (s[len]) len++;
        for (pad = len; pad < width && out < end; pad++)
            *out++ = zero ? '0' : ' ';
        while (*s && out < end) *out++ = *s++;
    }

    *out = 0;
}

void fmt_str(char *dst, usize cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    fmt_vstr(dst, cap, fmt, ap);
    __builtin_va_end(ap);
}
