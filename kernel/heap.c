/* ===========================================================================
 * heap.c -- a bump allocator over extended memory.
 *
 * FLOPOS needs about half a megabyte of frame buffers and lookup tables,
 * which will not fit under the 640K line next to the kernel, the stack and
 * the VGA aperture.  So everything big lives above 1MB and is handed out
 * from here, forwards only.  Nothing is ever freed: the scene list is
 * fixed and allocated once at boot, so a free() would never be called.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define HEAP_BASE 0x00100000u            /* first byte above the 1MB line  */
#define LOW_TOP   0x0008E000u            /* leaves the stack at 0x8F000     */
#define NEED_KB   512u                   /* less than this is not worth it  */

/* The linker puts this at the end of .bss, which is the end of everything
 * the kernel image occupies. */
extern char _bss_end[];

static u32 s_next = HEAP_BASE;
static u32 s_limit = HEAP_BASE;
static int s_low;                        /* running out of low memory?      */

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

    /* Trust the BIOS, but never past 8MB -- we need nothing like that much
     * and a lying BIOS should not get us a fault. */
    if (kb > 8192) kb = 8192;
    s_limit = HEAP_BASE + kb * 1024u;

    /* Prove the memory is actually there before trusting the BIOS about
     * it.  Two complementary patterns, because an address with nothing
     * behind it will often read back whatever was last on the bus and so
     * pass a single-pattern test.
     *
     * Nothing here needs to check A20: the 16-bit stub verifies the gate
     * is open and halts with a message rather than continue without it, so
     * by the time this runs 1MB cannot be aliasing 0. */
    if (kb >= NEED_KB && memory_is_real(HEAP_BASE)) {
        s_next = HEAP_BASE;
        s_low = 0;
        return;
    }

    /* No usable extended memory.  Everything FLOPOS allocates comes to
     * about a hundred kilobytes, and there is close to half a megabyte
     * free between the end of the kernel and the stack, so fall back to
     * conventional memory and carry on rather than giving up.  This is
     * what lets it run on a machine whose BIOS will not tell us about
     * anything above the 1MB line. */
    s_next  = ((u32)_bss_end + 15u) & ~15u;
    s_limit = LOW_TOP;
    s_low = 1;
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

u32 heap_base(void)  { return s_low ? (((u32)_bss_end + 15u) & ~15u) : HEAP_BASE; }
u32 heap_used(void)  { return s_next - heap_base(); }
u32 heap_total(void) { return s_limit - heap_base(); }
int heap_is_low(void) { return s_low; }

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
