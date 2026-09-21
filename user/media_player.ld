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
    .text : ALIGN(0x1000) { *(.text .text.*) } :text
    .rodata : ALIGN(16) { *(.rodata .rodata.*) } :text
    . = ALIGN(0x1000);
    .data : ALIGN(0x1000) { *(.data .data.*) } :data
    .bss : ALIGN(16) { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}