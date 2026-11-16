/* The script room's engine: linked to run at ROOM_BASE, header first.
 * Code and constants come first and end on a page boundary, so the
 * kernel can make them read-only. */
OUTPUT_FORMAT("binary")
ENTRY(room_main)

SECTIONS
{
    . = 0x01400000;
    .roomhdr : { KEEP(*(.roomhdr)) }
    .text   : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    . = ALIGN(4096);
    _room_ro_end = .;
    .data   : { *(.data) *(.data.*) }
    . = ALIGN(16);
    _room_image_end = .;
    .bss (NOLOAD) : { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(4096);
    _room_bss_end = .;
    ASSERT(_room_bss_end <= 0x01400000 + 0x00400000 - 0x10000 - 0x1000 - 0x40000 - 0x1000 - 0x200000, "the room's engine leaves under 2 MB for the scripts")
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
