/* ===========================================================================
 * FLOPOS kernel entry
 *
 * This file is linked first, so it sits at the very start of KERNEL.BIN and
 * therefore at physical 0x10000.  The boot sector far-jumps here with
 * CS = 0x1000, IP = 0, still in real mode.
 *
 * Two halves:
 *   .code16  -- probe RAM, open the A20 gate, load a flat GDT, flip CR0.PE
 *   .code32  -- flat 4GB segments, a real stack, zero .bss, call kmain()
 *
 * The 16-bit half runs with CS = 0x1000 but reaches its own data through
 * DS = 0 and 32-bit address registers, which sidesteps the fact that a
 * 16-bit relocation cannot hold an address linked at 0x10000.
 * ===========================================================================
 */

        .set    INFO_SEG,  0x0000       /* boot info block lives at 0x500  */
        .set    PM_STACK,  0x0008F000   /* below the EBDA, above .bss      */

        .section .stub16, "ax"
        .code16
        .globl  _start16
_start16:
        cli
        cld

        /* Segments: DS = 0 so the boot info block and the BIOS data area
         * are addressable with flat 16-bit displacements.  Stack goes well
         * clear of the kernel image. */
        xorw    %ax, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    $0x8000, %ax
        movw    %ax, %ss
        movw    $0xF000, %sp

        movb    %dl, 0x502              /* boot drive, as handed to us     */

/* ---- how much RAM is there above 1MB? ---------------------------------
 * Three ways, oldest last, because a 1996 BIOS may answer any one of them
 * and stay silent on the others.
 *
 * The subtlety that matters: a BIOS with no E801 support can still return
 * with carry clear and every register zero, which looks exactly like a
 * successful report of no memory.  A real Toshiba Satellite Pro 430CDT
 * does precisely that.  So a zero is treated as "no answer" and we drop to
 * the next method, rather than believing a 16MB machine has nothing.
 */
        xorw    %cx, %cx
        xorw    %dx, %dx
        movw    $0xE801, %ax
        int     $0x15
        jc      mem_88
        cmpb    $0x86, %ah              /* unsupported                     */
        je      mem_88
        cmpb    $0x80, %ah              /* invalid command                 */
        je      mem_88
        /* Some BIOSes answer in CX/DX instead of AX/BX. */
        testw   %cx, %cx
        jz      mem_e801_have
        movw    %cx, %ax
        movw    %dx, %bx
mem_e801_have:
        movzwl  %bx, %ebx
        shll    $6, %ebx                /* BX counts 64K blocks past 16M   */
        movzwl  %ax, %eax
        addl    %ebx, %eax
        testl   %eax, %eax
        jnz     mem_store               /* a real answer; take it          */

mem_88:                                 /* the AT-era call, near-universal */
        movb    $0x88, %ah
        int     $0x15
        jc      mem_cmos
        cmpb    $0x86, %ah
        je      mem_cmos
        movzwl  %ax, %eax
        testl   %eax, %eax
        jnz     mem_store

mem_cmos:
        /* Last resort: the CMOS extended-memory size, two bytes that every
         * AT-compatible machine has carried since 1984.  Caps at 15MB on
         * most of them, which is more than FLOPOS will ever ask for. */
        movb    $0x31, %al
        outb    %al, $0x70
        inb     $0x71, %al
        movb    %al, %ah                /* high byte                       */
        movb    $0x30, %al
        outb    %al, $0x70
        inb     $0x71, %al              /* low byte; AX is now KB          */
        movzwl  %ax, %eax

mem_store:
        movl    %eax, 0x504             /* KB of extended memory           */

/* ---- open the A20 gate ------------------------------------------------ */
        call    a20_is_open
        testw   %ax, %ax
        jnz     a20_done

        movw    $0x2401, %ax            /* 1. ask the BIOS nicely          */
        int     $0x15
        call    a20_is_open
        testw   %ax, %ax
        jnz     a20_done

        call    a20_via_kbc             /* 2. the keyboard controller      */
        call    a20_is_open
        testw   %ax, %ax
        jnz     a20_done

        inb     $0x92, %al              /* 3. the PS/2 fast gate           */
        testb   $0x02, %al
        jnz     a20_fast_done
        orb     $0x02, %al
        andb    $0xFE, %al              /* never touch the reset bit       */
        outb    %al, $0x92
a20_fast_done:
        call    a20_is_open
        testw   %ax, %ax
        jnz     a20_done
        jmp     a20_failed
a20_done:
        movb    $1, 0x508               /* note it worked, for the HUD     */

/* ---- into protected mode --------------------------------------------- */
        movb    $0xFF, %al              /* mask every IRQ; no IDT yet      */
        outb    %al, $0xA1
        outb    %al, $0x21

        /* DS is still 0, so a 32-bit register holds the GDT descriptor's
         * full linear address.  A 16-bit displacement could not: i386 ELF
         * relocations keep the addend in the field being patched, so a
         * linked address past 0xFFFF simply will not fit there. */
        xorw    %ax, %ax
        movw    %ax, %ds
        movl    $gdt_desc, %esi
        lgdtl   (%esi)

        movl    %cr0, %eax
        orl     $0x00000001, %eax       /* CR0.PE                          */
        movl    %eax, %cr0

        ljmpl   $0x08, $pm_start        /* flat 32-bit code selector       */

/* ---- is A20 on? returns AX = 1 if 1MB does not wrap to 0 -------------- */
a20_is_open:
        pushw   %ds
        pushw   %es
        pushw   %si
        pushw   %di
        pushw   %bx

        xorw    %ax, %ax
        movw    %ax, %es                /* ES:DI -> 0x000500               */
        movw    $0x0500, %di
        movw    $0xFFFF, %ax
        movw    %ax, %ds                /* DS:SI -> 0x100500 if A20 is on  */
        movw    $0x0510, %si

        movb    %es:(%di), %bl          /* save both bytes                 */
        movb    %ds:(%si), %bh

        movb    $0x00, %es:(%di)
        movb    $0xFF, %ds:(%si)
        movb    %es:(%di), %al          /* did the high write alias down?   */

        movb    %bh, %ds:(%si)          /* put them back                   */
        movb    %bl, %es:(%di)

        cmpb    $0xFF, %al
        movw    $0, %ax
        je      a20_open_out            /* aliased -> A20 still closed     */
        movw    $1, %ax
a20_open_out:
        popw    %bx
        popw    %di
        popw    %si
        popw    %es
        popw    %ds
        ret

/* ---- A20 through the 8042 output port --------------------------------- */
a20_via_kbc:
        call    kbc_wait_write
        movb    $0xAD, %al              /* disable the keyboard            */
        outb    %al, $0x64

        call    kbc_wait_write
        movb    $0xD0, %al              /* read output port                */
        outb    %al, $0x64
        call    kbc_wait_read
        inb     $0x60, %al
        movb    %al, %ah

        call    kbc_wait_write
        movb    $0xD1, %al              /* write output port              */
        outb    %al, $0x64
        call    kbc_wait_write
        movb    %ah, %al
        orb     $0x02, %al              /* bit 1 is the A20 gate          */
        outb    %al, $0x60

        call    kbc_wait_write
        movb    $0xAE, %al              /* re-enable the keyboard         */
        outb    %al, $0x64
        call    kbc_wait_write
        ret

kbc_wait_write:                         /* wait for input buffer empty    */
        movw    $0, %cx
1:      inb     $0x64, %al
        testb   $0x02, %al
        jz      2f
        loop    1b
2:      ret

kbc_wait_read:                          /* wait for output buffer full    */
        movw    $0, %cx
1:      inb     $0x64, %al
        testb   $0x01, %al
        jnz     2f
        loop    1b
2:      ret

/* ---- no A20, no 32-bit kernel ----------------------------------------- */
a20_failed:
        movw    $0x0003, %ax            /* back to text so we can complain */
        int     $0x10
        xorw    %ax, %ax
        movw    %ax, %ds
        movl    $a20_msg, %esi
1:      movb    (%esi), %al
        incl    %esi
        testb   %al, %al
        jz      2f
        movb    $0x0E, %ah
        movw    $0x0007, %bx
        int     $0x10
        jmp     1b
2:      jmp     2b

a20_msg:
        .asciz  "FLOPOS: cannot open the A20 gate on this machine. Halted."

/* ---- the GDT ----------------------------------------------------------
 * The two 16-bit entries are not used at boot.  They are how the kernel
 * gets back to real mode later: you cannot clear CR0.PE while CS is a
 * 32-bit segment, so the trip out goes through a 16-bit code segment
 * first, and the matching data segment fixes up the descriptor caches on
 * the way.
 *
 * Both are based at 0x10000, which is where the kernel lives.  That makes
 * every offset inside it fit in sixteen bits, and it matches what real
 * mode sees with its segment registers set to 0x1000 -- so the descriptor
 * caches stay consistent across the switch in both directions.
 */
        .globl  gdt, gdt_desc
        .align  8
gdt:
        .quad   0x0000000000000000      /* 0x00 null                       */
        .quad   0x00CF9A000000FFFF      /* 0x08 code32, base 0, limit 4G   */
        .quad   0x00CF92000000FFFF      /* 0x10 data32, base 0, limit 4G   */
        .quad   0x00009A010000FFFF      /* 0x18 code16, base 0x10000, 64K  */
        .quad   0x000092010000FFFF      /* 0x20 data16, base 0x10000, 64K  */
        .quad   0x00CFFA000000FFFF      /* 0x28 an app's code: privilege 3 */
        .quad   0x00CFF2000000FFFF      /* 0x30 an app's data: privilege 3 */
        .quad   0x0000000000000000      /* 0x38 the task state: wall.c     */
gdt_end:
gdt_desc:
        .word   gdt_end - gdt - 1
        .long   gdt                     /* linear address, A20 not needed  */

/* =======================================================================
 * 32-bit land
 * =======================================================================
 */
        .code32
        .globl  pm_start
pm_start:
        movw    $0x10, %ax              /* flat data everywhere            */
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %fs
        movw    %ax, %gs
        movw    %ax, %ss
        movl    $PM_STACK, %esp
        movl    %esp, %ebp

        /* The kernel proper runs at 1MB (linker.ld): copy it up from where
         * it sits in the image, behind this stub.  A20 is open by now (the
         * stub checked), and without memory above 1MB there is nowhere to
         * go: stop. */
        cmpl    $1024, 0x504             /* BOOTINFO_EXTKB: KB above 1MB */
        jb      halt
        cld
        movl    $_high_lma, %esi
        movl    $_high_start, %edi
        movl    $_high_end, %ecx
        subl    %edi, %ecx
        shrl    $2, %ecx
        rep     movsl

        /* .bss is NOLOAD, so it is not in the disk image -- clear it. */
        movl    $_bss_start, %edi
        movl    $_bss_end, %ecx
        subl    %edi, %ecx
        xorl    %eax, %eax
        shrl    $2, %ecx
        rep     stosl

        /* A flat model needs no segment juggling from here on. */
        call    kmain

        /* kmain never returns, but just in case. */
halt:   cli
        hlt
        jmp     halt
