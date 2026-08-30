ENTRY(main)

PHDRS
{
    text PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = 0;

    .text : ALIGN(4096)
    {
        *(.text*)
    } :text

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    } :text

    .data : ALIGN(4096)
    {
        *(.data*)
    } :data

    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss*)
    } :data
}