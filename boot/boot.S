/* ===========================================================================
 * FLOPOS boot sector  --  512 bytes, loaded by the BIOS at 0000:7C00
 *
 * Job list, in order:
 *   1. sane segments + stack
 *   2. remember the BIOS boot drive (DL)
 *   3. switch to VGA mode 13h straight away, so the load is visible
 *   4. pull SAFFRAN.PIX off the floppy into 8000:0000 -- his palette, then
 *      his picture -- painting him onto the screen as each chunk arrives,
 *      so he appears exactly as fast as the disk delivers him
 *   5. pull KERNEL.BIN into 1000:0000 underneath him, one track per
 *      INT 13h call, with a thin bar along the bottom for how far it has
 *      got.  The bar stops at half way: the kernel fills in the rest as
 *      it really does get ready.
 *   6. far jump into the kernel's own 16-bit stub
 *
 * Bytes 3..61 hold a FAT12 BPB.  We assemble a placeholder here and the
 * image builder copies mformat's real one over it, so the disk stays a
 * perfectly ordinary DOS floppy that happens to boot.
 *
 * Where SAFFRAN.PIX and KERNEL.BIN sit on the disk is patched in by
 * tools/mkimage.py at build time; the magic words below are what it looks
 * for.
 * ===========================================================================
 */

        .code16
        .section .boot, "ax"
        .globl  _start

_start:
        jmp     after_bpb
        nop

/* ---- offset 3: BPB placeholder, overwritten by the image builder ------- */
        .ascii  "FLOPOS  "              /* 3   OEM name, 8 bytes           */
        .word   512                     /* 11  bytes per sector            */
        .byte   1                       /* 13  sectors per cluster         */
        .word   1                       /* 14  reserved sectors            */
        .byte   2                       /* 16  number of FATs              */
        .word   224                     /* 17  root directory entries      */
        .word   2880                    /* 19  total sectors               */
        .byte   0xF0                    /* 21  media descriptor            */
        .word   9                       /* 22  sectors per FAT             */
        .word   18                      /* 24  sectors per track           */
        .word   2                       /* 26  heads                       */
        .long   0                       /* 28  hidden sectors              */
        .long   0                       /* 32  large total sectors         */
        .byte   0                        /* 36  drive number               */
        .byte   0                        /* 37  reserved                   */
        .byte   0x29                    /* 38  extended boot signature     */
        .long   0x5D0F05E5              /* 39  volume serial               */
        .ascii  "FLOPOS     "           /* 43  volume label, 11 bytes      */
        .ascii  "FAT12   "              /* 54  filesystem type, 8 bytes    */
/* ---- offset 62: our code ---------------------------------------------- */

after_bpb:
        cli
        xorw    %ax, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %ss
        movw    $0x7C00, %sp            /* stack grows down from us        */
        sti
        cld

        /* DL still holds the drive the BIOS booted from.  Trust it, but
         * fall back to 0x00 if it looks like a hard disk -- some very old
         * BIOSes pass junk. */
        testb   $0x80, %dl
        jz      1f
        movb    $0x00, %dl
1:      movb    %dl, boot_drive

        /* Mode 13h: 320x200, 256 colours, linear framebuffer at A000:0000.
         * Doing this before the load means the stripes below are visible. */
        movw    $0x0013, %ax
        int     $0x10

        /* Tell the kernel which drive we came from (boot info block @ 0x500) */
        movb    boot_drive, %al
        movb    %al, 0x502

        call    load                    /* Saffran, painted on as he comes  */

        movw    kernel_lba, %ax         /* then the kernel, under him      */
        movw    %ax, lba
        movw    kernel_sectors, %ax
        movw    %ax, sectors_left
        movw    %ax, total
        movw    $0x1000, dest_seg
        call    load

        /* Hand over to the kernel's 16-bit stub at 1000:0000. */
        movb    boot_drive, %dl
        ljmp    $0x1000, $0x0000

/* ---- read one file's sectors to dest_seg ------------------------------- */
load:
        cmpw    $0, sectors_left
        je      load_done

        /* LBA -> CHS for a 1.44M floppy: 18 sectors/track, 2 heads.
         *   index  = lba % 18          sector = index + 1
         *   track  = lba / 18
         *   head   = track % 2         cylinder = track / 2            */
        movw    lba, %ax
        xorw    %dx, %dx
        movw    $18, %cx
        divw    %cx                     /* ax = track, dx = index        */
        movw    %dx, %si                /* si = index within track       */
        xorw    %dx, %dx
        movw    $2, %cx
        divw    %cx                     /* ax = cylinder, dx = head      */
        movb    %al, chs_cyl
        movb    %dl, chs_head
        movw    %si, %ax
        incw    %ax
        movb    %al, chs_sect

        /* How much can go in one INT 13h call?  Three limits, and the
         * smallest wins:
         *
         *   what is left to read
         *   what is left in this track -- the BIOS will not cross one
         *   what is left before the next 64K physical boundary, because
         *     the floppy controller's DMA cannot step over one.  Miss this
         *     and the BIOS returns error 09h part way through a large
         *     kernel, which is a deeply confusing way to fail.
         */
        movw    $18, %ax
        subw    %si, %ax
        cmpw    sectors_left, %ax
        jbe     1f
        movw    sectors_left, %ax
1:
        movw    dest_seg, %cx           /* paragraphs into the 64K page   */
        andw    $0x0FFF, %cx
        negw    %cx
        andw    $0x0FFF, %cx            /* paragraphs still to go         */
        jnz     2f
        movw    $0x1000, %cx            /* exactly on a boundary: a full
                                         * page is available              */
2:      shrw    $5, %cx                 /* 32 paragraphs to a sector      */
        cmpw    %cx, %ax
        jbe     3f
        movw    %cx, %ax
3:      movb    %al, chunk

        movb    $5, retries
retry:
        movw    dest_seg, %es
        xorw    %bx, %bx
        movb    $0x02, %ah              /* INT 13h AH=02: read sectors   */
        movb    chunk, %al
        movb    chs_cyl, %ch
        movb    chs_sect, %cl
        movb    chs_head, %dh
        movb    boot_drive, %dl
        int     $0x13
        jnc     read_ok

        xorw    %ax, %ax                /* AH=00: recalibrate the drive  */
        movb    boot_drive, %dl
        int     $0x13
        decb    retries
        jnz     retry
        jmp     disk_error

read_ok:
        movzbw  chunk, %ax
        addw    %ax, lba
        subw    %ax, sectors_left
        shlw    $5, %ax                 /* sectors * 512 / 16 paragraphs */
        addw    %ax, dest_seg
        call    progress
        jmp     load
load_done:
        ret

/* ---- after every chunk: show how far the load has got -------------------
 * While Saffran is loading (dest_seg at 8000 or above), everything of him
 * that has arrived goes on the screen: his palette, as much of it as has
 * come, then his pixels, as many rows as have come.  He fills in from the
 * top down, at the speed of the drive.
 *
 * While the kernel is loading, a two-pixel bar along the bottom grows to
 * half the width of the screen, in palette entry 254: the bright green the
 * picture keeps back for text.
 */
progress:
        pusha
        pushw   %ds
        pushw   $0xA000
        popw    %es
        movw    dest_seg, %cx
        subw    $0x8000, %cx            /* paragraphs of Saffran so far    */
        jb      bar                     /* below 8000: it is the kernel    */

        pushw   $0x8000
        popw    %ds
        xorw    %si, %si
        movw    $0x3C8, %dx             /* the DAC, from entry 0           */
        xorb    %al, %al
        outb    %al, %dx
        incw    %dx
        pushw   %cx
        movw    $768, %cx
        rep     outsb                   /* si ends at 768: the pixels      */
        popw    %cx
        shlw    $4, %cx                 /* bytes of him loaded so far      */
        subw    $768, %cx
        jbe     shown                   /* nothing past the palette yet    */
        cmpw    $64000, %cx
        jbe     1f
        movw    $64000, %cx
1:      xorw    %di, %di
        rep     movsb
        jmp     shown

bar:    movw    sectors_left, %ax       /* width = 160 - left * 160 / total */
        movw    $160, %cx
        mulw    %cx
        divw    total
        subw    %ax, %cx
        movb    $254, %al
        movw    $198 * 320, %di
        pushw   %cx
        rep     stosb
        popw    %cx
        movw    $199 * 320, %di
        rep     stosb
shown:  popw    %ds
        popa
        ret

/* ---- unrecoverable read error ----------------------------------------- */
disk_error:
        /* Back to text mode so the message is legible, then say so. */
        movw    $0x0003, %ax
        int     $0x10
        movw    $err_msg, %si
1:      lodsb
        testb   %al, %al
        jz      2f
        movb    $0x0E, %ah
        movw    $0x0007, %bx
        int     $0x10
        jmp     1b
2:      xorw    %ax, %ax                /* wait for a key, then reboot   */
        int     $0x16
        ljmp    $0xFFFF, $0x0000

err_msg:
        .asciz  "FLOPOS: the floppy would not read. Any key reboots.\r\n"

/* ---- variables -------------------------------------------------------- */
boot_drive:     .byte   0
chs_cyl:        .byte   0
chs_head:       .byte   0
chs_sect:       .byte   0
chunk:          .byte   0
retries:        .byte   0
dest_seg:       .word   0x8000          /* Saffran first                 */
total:          .word   1

        /* mkimage.py finds these four by their magic placeholder values.
         * lba and sectors_left start out as SAFFRAN.PIX's, and are set to
         * the kernel's for the second load. */
lba:            .word   0xAA55          /* -> first sector of SAFFRAN.PIX */
sectors_left:   .word   0x55AA          /* -> its length in sectors       */
kernel_lba:     .word   0xBB66          /* -> first sector of KERNEL.BIN  */
kernel_sectors: .word   0x66BB          /* -> its length in sectors       */

        /* boot signature at 510 */
        .org    510
        .word   0xAA55
