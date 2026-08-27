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
    /* PIE/ET_DYN kernel: virtual addresses start at zero and the loader
       chooses a physical base at boot. */
    . = 0;

    .text : ALIGN(0x1000)
    {
        *(.text*)
        *(.rodata*)
    } :text

    .data : ALIGN(0x1000)
    {
        *(.data*)
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
        *(.dynamic*)
        *(.dynsym*)
        *(.dynstr*)
        *(.gnu.hash*)
        *(.hash*)
        *(.rela*)
    }
}
