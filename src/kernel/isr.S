/* ===========================================================================
 * isr.S -- interrupt entry points.
 *
 * 32 CPU exception stubs plus 16 hardware IRQ stubs.  Each one normalises
 * the stack into the same shape and jumps to a single trampoline, so the C
 * side sees one struct no matter how it got there.
 *
 * The CPU pushes an error code for some exceptions and not others; the
 * no-error stubs push a zero so the frame layout never varies.
 * ===========================================================================
 */
        .code32
        .text

/* ---- stub generators -------------------------------------------------- */
        .macro ISR_NOERR vec
        .globl isr\vec
isr\vec:
        pushl   $0
        pushl   $\vec
        jmp     isr_common
        .endm

        .macro ISR_ERR vec
        .globl isr\vec
isr\vec:
        pushl   $\vec
        jmp     isr_common
        .endm

        /* Exceptions 0-31.  8, 10-14 and 17 arrive with an error code. */
        ISR_NOERR 0
        ISR_NOERR 1
        ISR_NOERR 2
        ISR_NOERR 3
        ISR_NOERR 4
        ISR_NOERR 5
        ISR_NOERR 6
        ISR_NOERR 7
        ISR_ERR   8
        ISR_NOERR 9
        ISR_ERR   10
        ISR_ERR   11
        ISR_ERR   12
        ISR_ERR   13
        ISR_ERR   14
        ISR_NOERR 15
        ISR_NOERR 16
        ISR_ERR   17
        ISR_NOERR 18
        ISR_NOERR 19
        ISR_NOERR 20
        ISR_NOERR 21
        ISR_NOERR 22
        ISR_NOERR 23
        ISR_NOERR 24
        ISR_NOERR 25
        ISR_NOERR 26
        ISR_NOERR 27
        ISR_NOERR 28
        ISR_NOERR 29
        ISR_NOERR 30
        ISR_NOERR 31

        /* Hardware IRQ 0-15, remapped to vectors 32-47. */
        ISR_NOERR 32
        ISR_NOERR 33
        ISR_NOERR 34
        ISR_NOERR 35
        ISR_NOERR 36
        ISR_NOERR 37
        ISR_NOERR 38
        ISR_NOERR 39
        ISR_NOERR 40
        ISR_NOERR 41
        ISR_NOERR 42
        ISR_NOERR 43
        ISR_NOERR 44
        ISR_NOERR 45
        ISR_NOERR 46
        ISR_NOERR 47

        /* An app's two doors into the system (wall.c): 0x80 asks for
         * something, 0x81 comes back from a call the system made into it. */
        ISR_NOERR 128
        ISR_NOERR 129

/* ---- the one true trampoline -----------------------------------------
 * Stack on entry to isr_dispatch, low address first:
 *   edi esi ebp esp ebx edx ecx eax   (pusha, 8 dwords)
 *   vector
 *   error code
 *   eip cs eflags                     (pushed by the CPU)
 */
isr_common:
        pushal
        pushl   %ds
        pushl   %es

        movw    $0x10, %ax              /* make sure we are on flat data   */
        movw    %ax, %ds
        movw    %ax, %es

        leal    8(%esp), %eax           /* -> the pusha block              */
        pushl   %eax
        call    isr_dispatch
        addl    $4, %esp

        popl    %es
        popl    %ds
        popal
        addl    $8, %esp                /* drop vector and error code      */
        iret

/* ---- into an app, and back out of it -----------------------------------
 * u32 wall_go(u32 eip, u32 usp, u32 *kesp, u32 *esp0)
 *
 * Leaves the system for the app: the registers a C function must keep are
 * put on this stack, the stack's place is written to *kesp (to come back
 * to) and to *esp0 (where the CPU puts the system's stack when the app is
 * interrupted or asks for something), and an IRET drops to the app's
 * privilege at eip, on its own stack.  It "returns" when wall_back is
 * called with that kesp: with the value given there.
 */
        .text
        .globl  wall_go, wall_back
wall_go:
        pushl   %ebp
        pushl   %ebx
        pushl   %esi
        pushl   %edi
        movl    20(%esp), %eax          /* eip                             */
        movl    24(%esp), %ecx          /* the app's stack                 */
        movl    28(%esp), %edx
        movl    %esp, (%edx)
        movl    32(%esp), %edx
        movl    %esp, (%edx)
        pushl   $0x33                   /* the app's data, at its privilege */
        pushl   %ecx
        pushl   $0x202                  /* interrupts on; no port access   */
        pushl   $0x2B                   /* the app's code                  */
        pushl   %eax
        movw    $0x33, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        xorl    %eax, %eax
        xorl    %ebx, %ebx
        xorl    %ecx, %ecx
        xorl    %edx, %edx
        xorl    %esi, %esi
        xorl    %edi, %edi
        xorl    %ebp, %ebp
        iret

/* void wall_back(u32 kesp, u32 value): wall_go returns value. */
wall_back:
        movl    8(%esp), %eax
        movl    4(%esp), %esp
        popl    %edi
        popl    %esi
        popl    %ebx
        popl    %ebp
        ret

/* ---- a table so the C side can fill in the IDT ----------------------- */
        .section .rodata
        .align  4
        .globl  isr_table
isr_table:
        .long   isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
        .long   isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
        .long   isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
        .long   isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
        .long   isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
        .long   isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
        .long   isr128, isr129
