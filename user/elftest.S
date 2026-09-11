#include "../include/user_abi.h"

.section .text

.global _start
.type _start,@function

_start:
    /*
     * Write a known marker into the ELF's
     * writable data segment.
     */
    movabs $0x4A434F53454C4631, %rax
    mov %rax, result_magic(%rip)

    /*
     * SYSCALL_THREAD_ID = 0
     */
    mov $JCOS_SYSCALL_THREAD_ID, %eax
    int $0x80

    mov %rax, result_thread_id(%rip)

    /*
     * SYSCALL_THREAD_EXIT = 3
     */
    mov $JCOS_SYSCALL_THREAD_EXIT, %eax
    int $0x80

    /*
     * THREAD_EXIT must never return.
     */
    ud2

.size _start, .-_start


.section .data

.balign 8

.global result_magic
result_magic:
    .quad 0

.global result_thread_id
result_thread_id:
    .quad 0