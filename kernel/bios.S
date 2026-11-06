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

/* u32 bios_disk(u16 ax, u16 cx, u16 dx, u16 buf_seg)
 *
 * INT 13h with those registers; ES:BX is set to buf_seg:0000.  Returns the
 * BIOS's AX, with bit 16 set if it reported carry -- so a non-zero high
 * half means the operation failed.
 */
        .globl  bios_disk
bios_disk:
        pushl   %ebp
        movl    %esp, %ebp
        pushal

        /* Fill the scratch block while 32-bit flat addressing still makes
         * that trivial. */
        movw    8(%ebp),  %ax
        movw    %ax, blk_ax
        movw    12(%ebp), %ax
        movw    %ax, blk_cx
        movw    16(%ebp), %ax
        movw    %ax, blk_dx
        movw    20(%ebp), %ax
        movw    %ax, blk_es
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
        sti

        movw    4(%bx),  %ax
        movw    6(%bx),  %cx
        movw    8(%bx),  %dx
        movw    10(%bx), %si
        movw    %si, %es
        xorw    %si, %si
        pushw   %bx
        xorw    %bx, %bx
        int     $0x13
        popw    %bx

        movw    %ax, 4(%bx)
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
