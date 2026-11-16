/* Port I/O and a few byte-pushing helpers, all inline. */
#ifndef IO_H
#define IO_H
#include "flopos.h"

static inline void outb(u16 port, u8 v)
{ __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port)); }

static inline u8 inb(u16 port)
{ u8 v; __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

static inline void outw(u16 port, u16 v)
{ __asm__ __volatile__("outw %0, %1" :: "a"(v), "Nd"(port)); }

/* Block moves straight between a port and memory.  The card FIFOs are a
 * single address that has to be read or written hundreds of times, and
 * the processor has an instruction for exactly that which is several
 * times quicker than a loop around inw. */
static inline void outsw(u16 port, const void *buf, int n)
{ __asm__ __volatile__("rep outsw" : "+S"(buf), "+c"(n) : "d"(port)); }

static inline void insw(u16 port, void *buf, int n)
{ __asm__ __volatile__("rep insw" : "+D"(buf), "+c"(n) : "d"(port) : "memory"); }

static inline void outsl(u16 port, const void *buf, int n)
{ __asm__ __volatile__("rep outsl" : "+S"(buf), "+c"(n) : "d"(port)); }

static inline void insl(u16 port, void *buf, int n)
{ __asm__ __volatile__("rep insl" : "+D"(buf), "+c"(n) : "d"(port) : "memory"); }

static inline u16 inw(u16 port)
{ u16 v; __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ~1us on anything this old: a dummy read of an unused port. */
static inline void io_wait(void)
{ __asm__ __volatile__("outb %%al, $0x80" :: "a"(0)); }

static inline void cli(void) { __asm__ __volatile__("cli"); }
static inline void sti(void) { __asm__ __volatile__("sti"); }
static inline void hlt(void) { __asm__ __volatile__("hlt"); }

/* Whether interrupts were on, so a routine that must switch them off can
 * put them back exactly as it found them rather than guessing. */
static inline u32 irq_save(void)
{
    u32 f;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(u32 f)
{
    if (f & 0x200) sti();
}

/* The low half of the cycle counter.  Only meaningful when g_cpu_mhz is
 * non-zero, which is how everything here knows a counter exists at all:
 * on a 486 the instruction does not exist and executing it is a fault. */
static inline u32 tsc32(void)
{
    u32 lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    (void)hi;
    return lo;
}

static inline void memset8(void *d, u8 v, usize n)
{
    __asm__ __volatile__("cld; rep stosb"
                         : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

static inline void memset32(void *d, u32 v, usize dwords)
{
    __asm__ __volatile__("cld; rep stosl"
                         : "+D"(d), "+c"(dwords) : "a"(v) : "memory");
}

static inline void memcpy32(void *d, const void *s, usize dwords)
{
    __asm__ __volatile__("cld; rep movsl"
                         : "+D"(d), "+S"(s), "+c"(dwords) :: "memory");
}

static inline void memcpy8(void *d, const void *s, usize n)
{
    __asm__ __volatile__("cld; rep movsb"
                         : "+D"(d), "+S"(s), "+c"(n) :: "memory");
}

static inline int strlen8(const char *s)
{ int n = 0; while (s[n]) n++; return n; }

#endif /* IO_H */
