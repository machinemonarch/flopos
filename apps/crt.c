/* ===========================================================================
 * crt.c -- what every app needs before its own code: the header the
 * kernel checks before running it, and the memory routines a C compiler
 * is entitled to call on its own for copying structures.
 * ===========================================================================
 */
#include "app.h"

extern char _app_image_end[], _app_bss_end[];
int app_main(const Api *api, int argc, char **argv);

__attribute__((section(".apphdr"), used))
const AppHeader app_header = {
    { 'F', 'L', 'O', 'P', 'A', 'P', 'P', 0 },
    APP_API_VERSION, APP_BASE,
    (u32)_app_image_end, (u32)_app_bss_end, (u32)app_main,
    APP_NAME
};

void *memset(void *d, int c, unsigned n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, unsigned n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
