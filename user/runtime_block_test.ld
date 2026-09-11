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
    . = 0x0000008000000000;

    .text : ALIGN(0x1000)
    {
        *(.text .text.*)
    } :text


    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } :text


    ASSERT(
        . <= 0x0000008000002000,
        "runtime blocking test text exceeds two pages"
    )


    . = 0x0000008000002000;


    .runtime_block_result : ALIGN(8)
    {
        KEEP(*(.runtime_block_result))
    } :data


    .data : ALIGN(8)
    {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.*)
    } :data


    .bss : ALIGN(8)
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data


    ASSERT(
        . <= 0x0000008000003000,
        "runtime blocking test data exceeds one page"
    )


    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}