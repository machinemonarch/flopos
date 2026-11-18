/* FLOPOS kernel layout.
 *
 * Two parts.  The low one -- the 16-bit entry stub, and the real-mode
 * thunk with its scratch that every BIOS call goes through -- runs where
 * the boot sector loads the image, at 0x10000, and stays there: real mode
 * can only reach the first megabyte, and the thunk's 16-bit selectors only
 * its first 64K.  The high one -- all the rest: .text, .rodata, .data and
 * .bss -- is linked to run at 1MB (KERNEL_HIGH).  In the image it sits
 * right behind the low part; the 32-bit entry (start.S) copies it up,
 * zeroes .bss, and only then calls kmain.  So the kernel's own memory is
 * no longer squeezed under the 0x70000 disk buffer, and the heap starts
 * where .bss ends (heap.c).
 *
 * The image is loaded whole below Saffran's picture at 0x80000 before it
 * is copied up, which is the one limit left on it.
 */
OUTPUT_FORMAT("binary")
ENTRY(_start16)

KERNEL_HIGH = 0x00100000;

SECTIONS
{
    . = 0x00010000;

    .stub16 : { *(.stub16) }

    /* The real-mode thunk and its scratch, kept at the front on purpose.
     * They are reached through 16-bit selectors based at 0x10000 with a
     * 64K limit, so they have to be inside the first 64K of the image.
     * The ASSERT below is what stops that becoming a mystery later. */
    .lowtext : { *(.lowtext) }
    .lowdata : { *(.lowdata) }
    . = ALIGN(16);
    _low_end = .;
    _high_lma = .;                       /* where the high part is in the image */

    . = KERNEL_HIGH;
    _high_start = .;
    /* Every section at the same distance between where it runs and where
     * it sits in the image, so one straight copy puts each in its place.
     * Left to itself the linker packs the image tighter than the running
     * layout (alignment gaps differ) and skews everything after .text. */
    .text KERNEL_HIGH : AT(_high_lma) { *(.text) *(.text.*) }
    .rodata : AT(_high_lma + (ADDR(.rodata) - KERNEL_HIGH)) { *(.rodata) *(.rodata.*) }
    .data   : AT(_high_lma + (ADDR(.data) - KERNEL_HIGH)) { *(.data) *(.data.*) }

    . = ALIGN(16);
    _high_end = .;
    _image_end = .;
    _image_bytes = (_low_end - 0x00010000) + (_high_end - _high_start);

    _bss_start = .;
    .bss (NOLOAD) : { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;

    ASSERT(_high_lma + (_high_end - _high_start) <= 0x00080000,
           "the kernel image is too big to load below Saffran's picture at 0x80000")

    ASSERT(ADDR(.rodata) - LOADADDR(.rodata) == ADDR(.text) - LOADADDR(.text) &&
           ADDR(.data) - LOADADDR(.data) == ADDR(.text) - LOADADDR(.text),
           "the kernel's sections sit at different distances in the image: one copy cannot place them")

    ASSERT(_low_end - 0x00010000 < 0x10000,
           "the real-mode thunk has been pushed past the 64K line it must live under")

    ASSERT(_bss_end <= 0x00800000,
           "the kernel's memory runs over 8MB: the heap and the apps' area need the rest")

    /DISCARD/ : {
        *(.comment) *(.comment.*)
        *(.note) *(.note.*)
        *(.eh_frame) *(.eh_frame_hdr)
        *(.debug*)
    }
}
