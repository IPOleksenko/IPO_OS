ENTRY(_start)

PHDRS
{
    text PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0x800000;

    .text : ALIGN(4)
    {
        *(.entry)
        *(.text.startup)
        *(.text.main)
        *(.text*)
        KEEP(*(SORT_NONE(.init)))
        KEEP(*(SORT_NONE(.fini)))
    } :text

    .rodata : ALIGN(4)
    {
        *(.rodata*)
        *(.rdata*)
    } :text

    .eh_frame_hdr : ALIGN(4)
    {
        *(.eh_frame_hdr)
    } :text

    .eh_frame : ALIGN(4)
    {
        KEEP(*(.eh_frame))
        *(.eh_frame.*)
    } :text

    .gcc_except_table : ALIGN(4)
    {
        *(.gcc_except_table*)
    } :text

    . = ALIGN(4);

    .init_array : ALIGN(4)
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } :data

    .fini_array : ALIGN(4)
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } :data

    .data : ALIGN(4)
    {
        *(.data*)
    } :data

    .bss : ALIGN(4)
    {
        PROVIDE_HIDDEN (__bss_start = .);
        *(COMMON)
        *(.bss*)
        PROVIDE_HIDDEN (__bss_end = .);
    } :data

    _end = .;
    PROVIDE(end = .);
}