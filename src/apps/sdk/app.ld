/* Every app is linked to run at APP_BASE, header first. */
OUTPUT_FORMAT("binary")
ENTRY(app_main)

SECTIONS
{
    . = 0x00C00000;
    .apphdr : { KEEP(*(.apphdr)) }
    .text   : { *(.text) *(.text.*) }
    .rodata : { *(.rodata) *(.rodata.*) }
    _app_ro_end = .;                     /* not page aligned: padding would cost floppy */
    .data   : { *(.data) *(.data.*) }
    . = ALIGN(16);
    _app_image_end = .;
    .bss (NOLOAD) : { *(.bss) *(.bss.*) *(COMMON) }
    . = ALIGN(16);
    _app_bss_end = .;
    ASSERT(_app_bss_end - 0x00C00000 <= 0x00BC0000, "app is bigger than its 12MB less the stack")
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
