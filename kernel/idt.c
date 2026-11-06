/* ===========================================================================
 * idt.c -- interrupt descriptor table, the 8259 pair, and the panic screen.
 * ===========================================================================
 */
#include "flopos.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

typedef struct {
    u16 off_lo;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 off_hi;
} __attribute__((packed)) IdtEntry;

typedef struct {
    u16 limit;
    u32 base;
} __attribute__((packed)) IdtPtr;

typedef struct {
    u32 edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    u32 vector;
    u32 err;
    u32 eip, cs, eflags;
} Regs;

extern void *isr_table[48];

static IdtEntry s_idt[256];
static IdtPtr   s_idtp;

/* Handlers the rest of the kernel plugs in. */
void timer_irq(void);
void kbd_irq(void);
void serial_irq(void);

static void idt_set(int n, u32 handler)
{
    s_idt[n].off_lo = (u16)(handler & 0xFFFF);
    s_idt[n].off_hi = (u16)(handler >> 16);
    s_idt[n].sel    = 0x08;              /* flat 32-bit code selector      */
    s_idt[n].zero   = 0;
    s_idt[n].flags  = 0x8E;              /* present, ring 0, int gate      */
}

static void pic_remap(void)
{
    u8 m1, m2;

    m1 = inb(PIC1_DATA);
    m2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();     /* ICW1: init + expect ICW4       */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();    /* ICW2: master base vector 32    */
    outb(PIC2_DATA, 0x28); io_wait();    /*       slave base vector 40     */
    outb(PIC1_DATA, 0x04); io_wait();    /* ICW3: slave on IRQ2            */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();    /* ICW4: 8086 mode                */
    outb(PIC2_DATA, 0x01); io_wait();

    (void)m1; (void)m2;
    outb(PIC1_DATA, 0xFC);               /* let through IRQ0 and IRQ1 only */
    outb(PIC2_DATA, 0xFF);
}

/* Hand the interrupt controller back to the BIOS, and take it back again.
 *
 * The 8259s are remapped at boot so that hardware interrupts do not collide
 * with the CPU's exception vectors.  The BIOS knows nothing about that: its
 * own handlers sit at the original vectors, and its floppy code enables
 * interrupts and waits for IRQ6.  So before stepping back into real mode to
 * call it, the controllers have to be put back the way it left them, or the
 * first timer tick vectors into nothing and the machine stops.
 */
static u8 s_saved_mask1, s_saved_mask2;

static void pic_program(u8 base1, u8 base2)
{
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, base1); io_wait();
    outb(PIC2_DATA, base2); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
}

void pic_to_bios(void)
{
    s_saved_mask1 = inb(PIC1_DATA);
    s_saved_mask2 = inb(PIC2_DATA);
    pic_program(0x08, 0x70);             /* where the BIOS put them       */
    outb(PIC1_DATA, 0xBE);               /* let through the timer and the
                                          * floppy, nothing else          */
    outb(PIC2_DATA, 0xFF);
}

void pic_to_kernel(void)
{
    pic_program(0x20, 0x28);
    outb(PIC1_DATA, s_saved_mask1);
    outb(PIC2_DATA, s_saved_mask2);
}

/* Let a driver open its own interrupt line once it knows it has hardware. */
void irq_unmask(int irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit = (u8)(1 << (irq & 7));
    outb(port, (u8)(inb(port) & ~bit));
}

void idt_init(void)
{
    int i;

    for (i = 0; i < 256; i++) idt_set(i, 0), s_idt[i].flags = 0;
    for (i = 0; i < 48; i++)  idt_set(i, (u32)isr_table[i]);

    pic_remap();

    s_idtp.limit = sizeof(s_idt) - 1;
    s_idtp.base  = (u32)&s_idt[0];
    __asm__ __volatile__("lidt (%0)" :: "r"(&s_idtp));
}

void irq_enable(void) { sti(); }

/* ---- the blue screen, except it is magenta --------------------------- */
static const char *s_exc_name[32] = {
    "divide by zero", "debug", "NMI", "breakpoint",
    "overflow", "bound range", "invalid opcode", "no coprocessor",
    "double fault", "coprocessor overrun", "bad TSS", "segment not present",
    "stack fault", "general protection", "page fault", "reserved",
    "coprocessor error", "alignment check", "machine check", "SIMD",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved"
};

static int s_in_panic = 0;

static void panic(Regs *r)
{
    static u8 pal[768];
    char line[64];
    int i, y;

    if (s_in_panic) { for (;;) { cli(); hlt(); } }
    s_in_panic = 1;
    cli();

    /* A loud, legible palette of our own -- whatever the scene left in the
     * DAC is no use for reading an error message. */
    for (i = 0; i < 256; i++) {
        pal[i * 3 + 0] = (u8)(10 + (i >> 3));
        pal[i * 3 + 1] = 0;
        pal[i * 3 + 2] = (u8)(20 + (i >> 2));
    }
    pal[0] = 4;  pal[1] = 0;  pal[2] = 12;        /* background           */
    pal[3 * 15 + 0] = 63; pal[3 * 15 + 1] = 63; pal[3 * 15 + 2] = 63;
    pal[3 * 14 + 0] = 63; pal[3 * 14 + 1] = 48; pal[3 * 14 + 2] = 0;
    pal[3 * 12 + 0] = 63; pal[3 * 12 + 1] = 12; pal[3 * 12 + 2] = 32;
    vga_set_palette(pal);

    memset8(g_back, 0, SCR_PIX);
    for (y = 0; y < SCR_H; y++)                   /* faint scanline wash  */
        if (y & 1) draw_hline(g_back, 0, y, SCR_W, 1);

    draw_frame(g_back, 8, 8, SCR_W - 16, SCR_H - 16, 12);
    draw_text(g_back, 20, 20, "FLOPOS HAS LOST THE THREAD", 15);
    draw_hline(g_back, 20, 30, 280, 12);

    fmt_str(line, sizeof line, "trap %u: %s",
            r->vector, r->vector < 32 ? s_exc_name[r->vector] : "hardware");
    draw_text(g_back, 20, 40, line, 14);

    fmt_str(line, sizeof line, "eip %x  err %x  eflags %x",
            r->eip, r->err, r->eflags);
    draw_text(g_back, 20, 52, line, 15);
    fmt_str(line, sizeof line, "eax %x  ebx %x", r->eax, r->ebx);
    draw_text(g_back, 20, 62, line, 15);
    fmt_str(line, sizeof line, "ecx %x  edx %x", r->ecx, r->edx);
    draw_text(g_back, 20, 72, line, 15);
    fmt_str(line, sizeof line, "esi %x  edi %x", r->esi, r->edi);
    draw_text(g_back, 20, 82, line, 15);

    draw_text(g_back, 20, 104, "The trip ended early. Sorry about that.", 14);
    draw_text(g_back, 20, 120, "Press CTRL+ALT+DEL to reboot, or power", 15);
    draw_text(g_back, 20, 130, "cycle the machine.", 15);
    draw_text(g_back, 20, 160, "Please note the numbers above if you", 1 + 7);
    draw_text(g_back, 20, 170, "intend to report this.", 8);

    vga_present();
    snd_silence();

    /* Poll the keyboard by hand: interrupts are off and staying off. */
    for (;;) {
        if (inb(0x64) & 1) {
            u8 sc = inb(0x60);
            if (sc == 0x53) reboot();              /* Delete              */
        }
    }
}

void isr_dispatch(Regs *r)
{
    u32 v = r->vector;

    if (v < 32) { panic(r); return; }

    switch (v) {
    case 32: timer_irq(); break;
    case 33: kbd_irq();   break;
    case 36: serial_irq(); break;        /* COM1                          */
    default: break;
    }

    if (v >= 40) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
