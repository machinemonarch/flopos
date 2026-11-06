/* FLOPOS kernel layout.
 *
 * The disk image is the flat span .stub16 .. .data, loaded at 0x10000 by
 * the boot sector.  .bss is NOLOAD so it costs nothing on the floppy; the
 * 32-bit entry code zeroes it before kmain() runs.  Big frame buffers and
 * lookup tables are not here at all -- they come from halloc(), which
 * carves up extended memory above 1MB.
 */
OUTPUT_FORMAT("binary")
ENTRY(_start16)

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
    _low_end = .;

    .text   : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    .data   : { *(.data) *(.data.*) }

    . = ALIGN(16);
    _image_end = .;

    _bss_start = .;
    .bss (NOLOAD) : { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;

    ASSERT(_low_end - 0x00010000 < 0x10000,
           "the real-mode thunk has been pushed past the 64K line it must live under")

    /DISCARD/ : {
        *(.comment) *(.comment.*)
        *(.note) *(.note.*)
        *(.eh_frame) *(.eh_frame_hdr)
        *(.debug*)
    }
}
