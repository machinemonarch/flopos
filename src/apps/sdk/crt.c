/* ===========================================================================
 * crt.c -- what every app needs before its own code: the header the
 * kernel checks before running it, and the memory routines a C compiler
 * is entitled to call on its own for copying structures.
 * ===========================================================================
 */
#include "app.h"

extern char _app_image_end[], _app_bss_end[], _app_ro_end[];
int app_main(const Api *api, int argc, char **argv);
static int app_start(const Api *api, int argc, char **argv);

__attribute__((section(".apphdr"), used))
const AppHeader app_header = {
    { 'F', 'L', 'O', 'P', 'A', 'P', 'P', 0 },
    APP_API_VERSION, APP_BASE,
    (u32)_app_image_end, (u32)_app_bss_end, (u32)app_start,
    APP_NAME, (u32)_app_ro_end
};

/* The stack's canary: a random word beside every return address that has
 * an array near it, checked on the way out.  This app's own, picked before
 * any of its code runs -- here, where nothing is checked yet -- with the
 * low byte zero so a runaway string ends on it. */
u32 __stack_chk_guard;

__attribute__((no_stack_protector))
static int app_start(const Api *api, int argc, char **argv)
{
    u32 g = 0;
    api->random((u8 *)&g, 4);
    __stack_chk_guard = (g & ~0xFFu) | (g & ~0xFFu ? 0 : 0x2A2A2A00u);
    return app_main(api, argc, argv);
}

/* An overrun caught: stopped at once, with a word the system recognises. */
void __stack_chk_fail(void)
{
    __asm__ __volatile__("ud2" :: "a"(STACK_SMASHED));
    for (;;) { }
}

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
