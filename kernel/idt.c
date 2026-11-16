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

extern void *isr_table[50];

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

/* What the kernel wants open, kept here rather than read back from the
 * chip.  Reading it back is how a bug becomes permanent: if anything --
 * a BIOS's "unexpected interrupt" handler is the classic -- masks the
 * timer behind our back, the next save-and-restore would faithfully put
 * it back masked, and the clock would never run again.  So the chip is
 * always told what we intend, never what it last said. */
static u8 s_kmask1 = 0xFC;               /* IRQ0 and IRQ1 open             */
static u8 s_kmask2 = 0xFF;

static void pic_program(u8 base1, u8 base2)
{
    outb(PIC1_CMD, 0x11); io_wait();     /* ICW1: init + expect ICW4       */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, base1); io_wait();   /* ICW2: where the vectors start  */
    outb(PIC2_DATA, base2); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();    /* ICW3: slave on IRQ2            */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();    /* ICW4: 8086 mode                */
    outb(PIC2_DATA, 0x01); io_wait();
}

/* Hand the interrupt controllers back to the BIOS, and take them back.
 *
 * At boot they are moved to vectors 0x20 and 0x28 so hardware interrupts
 * stop colliding with the processor's own exceptions.  The BIOS knows
 * nothing about that.  Its handlers sit at 0x08 and 0x70, and its own
 * code -- the video BIOS included, which was the long-hunted freeze --
 * switches interrupts on the moment it is entered.  A timer tick arriving
 * while the controllers still point at 0x20 lands on a real-mode vector
 * the BIOS never meant for hardware, whose handler either never says it
 * has finished, which blocks every interrupt from then on, or masks the
 * line it does not recognise, which stops the clock for good.
 *
 * Re-initialising the chips on the way back also clears anything left
 * marked as in service, so even a BIOS that misbehaves cannot leave a
 * line stuck.  Only bios_int() calls these. */
void pic_to_bios(void)
{
    pic_program(0x08, 0x70);             /* where the BIOS put them       */
    outb(PIC1_DATA, 0xBE);               /* the timer and the floppy only */
    outb(PIC2_DATA, 0xFF);
}

void pic_to_kernel(void)
{
    pic_program(0x20, 0x28);
    outb(PIC1_DATA, s_kmask1);
    outb(PIC2_DATA, s_kmask2);
}

/* Let a driver open its own interrupt line once it knows it has hardware. */
void irq_unmask(int irq)
{
    u8 bit = (u8)(1 << (irq & 7));
    if (irq < 8) { s_kmask1 &= (u8)~bit; outb(PIC1_DATA, s_kmask1); }
    else         { s_kmask2 &= (u8)~bit; outb(PIC2_DATA, s_kmask2); }
}

void idt_init(void)
{
    int i;

    for (i = 0; i < 256; i++) idt_set(i, 0), s_idt[i].flags = 0;
    for (i = 0; i < 48; i++)  idt_set(i, (u32)isr_table[i]);
    /* The two doors an app may knock on (wall.c): the only vectors that
     * can be raised from an app's privilege. */
    idt_set(0x80, (u32)isr_table[48]); s_idt[0x80].flags = 0xEE;
    idt_set(0x81, (u32)isr_table[49]); s_idt[0x81].flags = 0xEE;

    pic_to_kernel();

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

/* The address a page fault was reaching for.  There is no paging here, so
 * this is only ever meaningful for the faults that carry it, but it costs
 * one instruction and a fault we cannot explain is worth every scrap. */
static u32 read_cr2(void)
{
    u32 v;
    __asm__ __volatile__("movl %%cr2, %0" : "=r"(v));
    return v;
}

static int s_in_panic = 0;

/* A sentence for the crash screen, when the stop was asked for (the
 * stack's tripwire in time.c) rather than stumbled into. */
const char *g_panic_note;

static void panic(Regs *r)
{
    static u8 pal[768];
    char line[64];
    int i, y;

    if (s_in_panic) { for (;;) { cli(); hlt(); } }
    s_in_panic = 1;
    cli();

    /* Down the wire first, before anything is drawn.  A panic screen is
     * only a panic screen if somebody can see it, and the one case where
     * that is least likely is a fault in the code that draws -- which is
     * exactly when a fault is most likely.  This costs nothing and makes
     * it impossible for the machine to die without saying so. */
    serial_print("\n\n*** FLOPOS HAS LOST THE THREAD ***\n");
    if (g_panic_note) { serial_print(g_panic_note); serial_print("\n"); }
    serial_printf("trap %u (%s)  err %x\n", r->vector,
                  r->vector < 32 ? s_exc_name[r->vector] : "hardware", r->err);
    serial_printf("eip %x  eflags %x  cr2 %x\n", r->eip, r->eflags, read_cr2());
    serial_printf("eax %x ebx %x ecx %x edx %x\n", r->eax, r->ebx, r->ecx, r->edx);
    serial_printf("esi %x edi %x ebp %x  cs %x\n", r->esi, r->edi, r->ebp, r->cs);

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
    pal[3 * PAL_KEY + 0] = 63; pal[3 * PAL_KEY + 1] = 26; pal[3 * PAL_KEY + 2] = 46;   /* keys: pink */
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

    draw_text(g_back, 20, 104, g_panic_note ? g_panic_note : "The trip ended early. Sorry about that.", 14);
    draw_text(g_back, 20, 120, "Press", 15);
    draw_text(g_back, 20 + 6 * 6, 120, "CTRL+ALT+DEL", PAL_KEY);      /* keys are pink everywhere */
    draw_text(g_back, 20 + 6 * 19, 120, "to reboot, or power", 15);
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

/* Cycles spent in interrupts: work, even when it interrupts a halt (the
 * sound is decoded here), so the HUD takes it out of the idle time. */
u32 g_irq_cycles;
int g_irq_depth;                         /* interrupts inside interrupts; wall.c clears it when an app is closed from inside one */

void isr_dispatch(Regs *r)
{
    u32 v = r->vector, c0;

    /* From an app: its doors, its faults (which close it, not the
     * machine), and where its stack was, for calls made back into it. */
    if ((r->cs & 3) == 3) {
        if (wall_trap(r)) return;
        if (v < 32) { g_irq_depth = 0; wall_fault(r, v < 32 ? s_exc_name[v] : "hardware"); }
    }
    if (v < 32) { panic(r); return; }
    c0 = g_cpu_mhz ? tsc32() : 0;
    g_irq_depth++;

    switch (v) {
    case 32: timer_irq(); break;
    case 33: kbd_irq();   break;
    case 36: serial_irq(); break;        /* COM1                          */
    default: break;
    }

    if (v >= 40) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* After the timer's acknowledgement, with the interrupt's own work
     * done: the sound queue refilled, if an app has asked for that. */
    if (v == 32) audio_deferred();
    if (v == 32) room_tick(r);           /* a script room past its time is stopped */
    cli();                                /* the sound's work may have let others in */
    if (--g_irq_depth == 0 && g_cpu_mhz) g_irq_cycles += tsc32() - c0;
}
