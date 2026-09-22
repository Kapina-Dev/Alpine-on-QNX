ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00100000;
    .text : ALIGN(4)
    {
        *(.text*)
    } :text

    . = ALIGN(0x1000);
    .rodata : ALIGN(4)
    {
        *(.rodata*)
    } :rodata

    . = ALIGN(0x1000);
    .data : ALIGN(4)
    {
        *(.data*)
    } :data

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
    } :data
}
