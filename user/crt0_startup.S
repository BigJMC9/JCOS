.section .text

.global _start
.type _start,@function

.extern jcos_main
.extern jcos_thread_exit

_start:
    /* R5 startup ABI: initial RSP points at JcosProgramStartup. */
    mov %rsp, %rdi
    xor %ebp, %ebp
    call jcos_main
    call jcos_thread_exit
    ud2

.size _start, .-_start

.section .note.GNU-stack,"",@progbits
