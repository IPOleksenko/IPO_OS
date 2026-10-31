ENTRY(_start)

SECTIONS
{
    . = 0x10000;
    _kernel_text_start = .;

    .text :
    {
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)
    }

    . = ALIGN(4096);
    _kernel_rodata_end = .;

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss*)
    }
    _kernel_bss_end = .;

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.comment*)
    }
}
