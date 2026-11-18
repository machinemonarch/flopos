/* ===========================================================================
 * heap.c -- a bump allocator over extended memory.
 *
 * Everything big lives above 1MB, after the kernel (which runs at 1MB:
 * linker.ld), and is handed out from here, forwards only.  Nothing is ever freed: the scene list is
 * fixed and allocated once at boot, so a free() would never be called.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define NEED_KB   1024u                  /* less than this is not worth it  */

/* The linker puts this at the end of .bss, which since the kernel moved
 * up to 1MB (linker.ld) is where the heap begins. */
extern char _bss_end[];

static u32 s_base, s_next, s_limit;

/* Two complementary patterns, because an address with nothing behind it
 * will often read back whatever was last on the bus and so pass a
 * single-pattern test. */
static int memory_is_real(u32 addr)
{
    volatile u32 *p = (volatile u32 *)addr;
    u32 save = *p;
    int ok = 1;

    *p = 0xC0FFEE42u;
    if (*p != 0xC0FFEE42u) ok = 0;
    *p = ~0xC0FFEE42u;
    if (*p != (u32)~0xC0FFEE42u) ok = 0;
    *p = save;
    return ok;
}

void heap_init(void)
{
    u32 kb = BOOTINFO_EXTKB;

    /* From the end of the kernel to what the BIOS counted, but never past
     * 9MB: the apps' area starts at 12MB, and a lying BIOS should not get
     * us a fault.  The kernel itself needs memory above 1MB to run at all
     * (start.S stops without it), so there is no low fallback any more. */
    if (kb > 8192) kb = 8192;
    s_base = s_next = ((u32)_bss_end + 15u) & ~15u;
    s_limit = 0x00100000u + kb * 1024u;
    if (s_limit < s_base + NEED_KB * 1024u || !memory_is_real(s_base))
        fatal("Not enough memory above 1MB.", "FLOPOS needs at least 3MB in all.");
}

/* Nothing in FLOPOS can carry on without the buffer it asked for, so a
 * failed allocation says so on the screen instead of handing back a null
 * that would be written through into low memory -- there is no paging
 * here, so that would corrupt silently rather than fault. */
void *halloc(usize bytes)
{
    u32 p;

    bytes = (bytes + 15u) & ~15u;        /* keep everything 16-byte aligned */
    p = s_next;
    if (p + bytes > s_limit) {
        fatal("Not enough memory to set up the screen.",
              "FLOPOS needs about 128kb and could not get it.");
    }
    s_next = p + bytes;
    memset8((void *)p, 0, bytes);
    return (void *)p;
}

u32 heap_base(void)  { return s_base; }
u32 heap_used(void)  { return s_next - heap_base(); }
u32 heap_total(void) { return s_limit - heap_base(); }
int heap_is_low(void) { return 0; }

/* gcc will emit calls to these for struct copies and array initialisers
 * however hard we try to avoid it, so give it real ones. */
void *memset(void *d, int c, usize n)
{ memset8(d, (u8)c, n); return d; }

void *memcpy(void *d, const void *s, usize n)
{ memcpy8(d, s, n); return d; }

void *memmove(void *d, const void *s, usize n)
{
    u8 *dd = (u8 *)d; const u8 *ss = (const u8 *)s;
    if (dd == ss || n == 0) return d;
    if (dd < ss) { while (n--) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
