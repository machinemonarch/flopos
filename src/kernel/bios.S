/* ===========================================================================
 * bios.S -- call the BIOS from protected mode.
 *
 * The floppy on a Satellite Pro 430CDT hangs off a dedicated external
 * port.  A controller driver written from a datasheet might or might not
 * drive it; the machine's own BIOS certainly does, because it booted from
 * it.  So rather than reimplement the controller, FLOPOS steps back into
 * real mode, asks the BIOS, and returns.
 *
 * Getting out, in this order and no other:
 *
 *   1. interrupts off; the protected-mode IDT and GDT put aside
 *   2. far jump into a 16-bit code segment -- CR0.PE cannot be cleared
 *      while CS is 32-bit, or the processor carries on decoding 32-bit
 *      instructions at a 16-bit address
 *   3. load 16-bit data selectors, so the hidden descriptor caches hold
 *      the right base and a 64K limit before they stop being selectors
 *   4. clear PE and far jump, which makes CS a paragraph again
 *   5. point IDTR at the real-mode vector table and let interrupts in
 *
 * and the reverse afterwards.
 *
 * Every selector used here is based at 0x10000, where the kernel lives, so
 * offsets within it fit in sixteen bits -- and real mode sees exactly the
 * same base with its segment registers set to 0x1000.  That symmetry is
 * the whole trick; without it the descriptor caches disagree with the
 * segment registers and the first memory access after the switch is wrong.
 *
 * One wrinkle worth knowing: a 16-bit absolute address cannot be relocated
 * into this code, because i386 ELF keeps the addend in the field being
 * patched and the kernel is linked past 0xFFFF.  So the real-mode half
 * reaches its scratch space through BX, set up while still in 32 bits,
 * where the arithmetic is easy.
 * ===========================================================================
 */
/* Both the code and the scratch below must live in the first 64K of the
 * kernel: the 16-bit selectors this uses are based at 0x10000 with a 64K
 * limit, and anything past 0x20000 simply cannot be addressed once the
 * processor is in 16-bit mode.  The linker script places these sections
 * immediately after the entry stub and asserts that they fit -- without
 * that, growing the kernel past 64K breaks this in a way that looks like
 * the machine hanging for no reason.
 */
        .code32
        .section .lowtext, "ax"

        .set    SEL_CODE32, 0x08
        .set    SEL_DATA32, 0x10
        .set    SEL_CODE16, 0x18         /* both based at KBASE            */
        .set    SEL_DATA16, 0x20
        .set    KBASE,      0x10000
        .set    RM_SEG,     0x1000       /* KBASE >> 4                     */
        .set    RM_STACK,   0x7C00       /* the boot sector is long done   */

/* u32 bios_int_raw(u16 which, u16 ax, u16 bx, u16 cx, u16 dx, u16 es, u16 di)
 *
 * The bare trip.  Call bios_int() instead, always: see bioscall.c.
 *
 * which is 0x13 for the disk, 0x15 for the services APM lives behind, or
 * 0x10 for video -- which is how VESA is asked what this card can do, and
 * the reason ES:DI is carried through at all: every VBE call that returns
 * anything returns it into a buffer the caller points at.
 *
 * Returns the BIOS's AX, with bit 16 set if it reported carry -- so a
 * non-zero high half means the call failed.
 */
        .globl  bios_int_raw
bios_int_raw:
        pushl   %ebp
        movl    %esp, %ebp
        pushal
        pushfl                           /* see the note at the bottom     */

        /* Fill the scratch block while 32-bit flat addressing still makes
         * that trivial. */
        movw    8(%ebp),  %ax
        movw    %ax, blk_which
        movw    12(%ebp), %ax
        movw    %ax, blk_ax
        movw    16(%ebp), %ax
        movw    %ax, blk_bx
        movw    20(%ebp), %ax
        movw    %ax, blk_cx
        movw    24(%ebp), %ax
        movw    %ax, blk_dx
        movw    28(%ebp), %ax
        movw    %ax, blk_es
        movw    32(%ebp), %ax
        movw    %ax, blk_di
        movw    $0, blk_flags

        /* The far pointer that puts us in real mode, built by hand because
         * no 16-bit relocation could hold it. */
        movl    $real_mode, %eax
        subl    $KBASE, %eax
        movw    %ax, blk_far_off
        movw    $RM_SEG, blk_far_seg

        cli
        sidt    blk_pm_idt
        sgdt    blk_pm_gdt
        movl    %esp, blk_pm_esp

        /* BX carries the scratch block's offset through every mode the
         * processor is about to be in. */
        movl    $blk, %ebx
        subl    $KBASE, %ebx

        ljmp    $SEL_CODE16, $(thunk16 - KBASE)

/* ---- 16-bit protected mode ------------------------------------------- */
        .code16
thunk16:
        movw    $SEL_DATA16, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        movw    %ax, %ss

        movl    %cr0, %eax
        andl    $0xFFFFFFFE, %eax
        movl    %eax, %cr0

        ljmpw   *(%bx)                   /* -> RM_SEG:real_mode            */

/* ---- real mode -------------------------------------------------------- */
real_mode:
        movw    $RM_SEG, %ax             /* same base the caches hold      */
        movw    %ax, %ds
        xorw    %ax, %ax
        movw    %ax, %ss
        movw    $RM_STACK, %sp

        lidtl   14(%bx)                  /* the BIOS vector table at 0     */

        /* Interrupts on, for every service alike.  This is only safe
         * because bios_int() in bioscall.c has already handed the interrupt
         * controllers back to the BIOS's own numbering before calling in
         * here -- so a timer tick that lands down here reaches the BIOS's
         * own timer handler, which is the one thing it knows what to do
         * with.  Nothing may call this entry directly for that reason. */
        sti
        movw    4(%bx),  %ax
        movw    6(%bx),  %cx
        movw    8(%bx),  %dx
        movw    10(%bx), %si
        movw    %si, %es
        movw    40(%bx), %di             /* ES:DI, for anything VESA      */
        movw    38(%bx), %si             /* which interrupt               */
        pushw   %bx
        movw    36(%bx), %bx             /* BX last: it is how we got here */
        cmpw    $0x15, %si
        je      call15
        cmpw    $0x10, %si
        je      call10
        int     $0x13
        jmp     called
call15: int     $0x15
        jmp     called
call10: int     $0x10
called: movw    %bx, %si             /* the BIOS's BX, kept: bios_regs  */
        popw    %bx

        movw    %ax, 4(%bx)
        movw    %cx, 6(%bx)
        movw    %dx, 8(%bx)
        movw    %si, 36(%bx)
        jnc     1f
        movw    $1, 12(%bx)
1:
        cli

/* ---- back into protected mode ---------------------------------------- */
        lgdtl   20(%bx)
        movl    %cr0, %eax
        orl     $1, %eax
        movl    %eax, %cr0

        ljmpl   $SEL_CODE32, $back32     /* 32-bit offset: relocatable     */

        .code32
back32:
        movw    $SEL_DATA32, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        movw    %ax, %ss
        movl    blk_pm_esp, %esp
        lidt    blk_pm_idt

        /* The flags, and with them whether interrupts are allowed.  pushal
         * does not carry them, so without this every call came back with
         * interrupts off -- no clock, no keyboard, a machine that looks
         * exactly like one that has crashed.  It never showed up because
         * the calls made during start-up are followed by the kernel
         * turning interrupts on anyway; the first one made with the shell
         * already running killed it on the spot.  Restored after the
         * interrupt table, never before: letting one in while the table
         * still points at the BIOS would be the same bug the other way
         * round. */
        popfl
        popal
        movzwl  blk_ax, %eax
        cmpw    $0, blk_flags
        je      1f
        orl     $0x10000, %eax           /* carry: the BIOS said no        */
1:
        popl    %ebp
        ret

/* ---- scratch ----------------------------------------------------------
 * One contiguous block, because the real-mode half reaches all of it
 * through BX with small constant displacements.  The offsets here are
 * quoted in the code above and must not drift.
 */
        .section .lowdata, "aw"
        .align  16
        .globl  blk
blk:
blk_far_off:  .word 0                    /*  0 */
blk_far_seg:  .word 0                    /*  2 */
blk_ax:       .word 0                    /*  4 */
.globl blk_bx, blk_cx, blk_dx
blk_cx:       .word 0                    /*  6 */
blk_dx:       .word 0                    /*  8 */
blk_es:       .word 0                    /* 10 */
blk_flags:    .word 0                    /* 12 */
blk_rm_idt:   .word 0x03FF               /* 14 */
              .long 0x00000000           /* 16 */
blk_pm_gdt:   .word 0                    /* 20 */
              .long 0                    /* 22 */
blk_pm_idt:   .word 0                    /* 26 */
              .long 0                    /* 28 */
blk_pm_esp:   .long 0                    /* 32 */
blk_bx:       .word 0                    /* 36 */
blk_which:    .word 0                    /* 38 */
blk_di:       .word 0                    /* 40 */

/* The stack on the way in: EBP, then the eight registers, then the flags.
 * blk_pm_esp is taken after all three, so the way back has to undo them in
 * that order or the register values come back shifted by one. */
