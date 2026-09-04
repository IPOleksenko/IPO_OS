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
    } :text

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    } :text

    .data : ALIGN(4)
    {
        *(.data*)
    } :data

    .bss : ALIGN(4)
    {
        *(COMMON)
        *(.bss*)
    } :data
}