OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    /* Position-independent ET_DYN kernel. The UEFI loader chooses the physical base. */
    . = 0;

    .text : ALIGN(0x1000)
    {
        *(.text*)
        *(.rodata*)
    } :text

    .data : ALIGN(0x1000)
    {
        *(.data*)
        *(.got*)
    } :data

    .bss : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss*)
    } :data

    /DISCARD/ :
    {
        *(.comment*)
        *(.eh_frame*)
        *(.note*)
        *(.interp*)
    }
}
