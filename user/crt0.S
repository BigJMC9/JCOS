.section .text

.global _start
.type _start,@function

.extern jcos_main
.extern jcos_thread_exit


_start:
    /*
     * Kernel bootstrap ABI:
     *
     *   [rsp + 0] = first startup capability
     *   [rsp + 8] = second startup capability
     *
     * SysV C ABI:
     *
     *   RDI = first argument
     *   RSI = second argument
     */
    mov 0(%rsp), %rdi
    mov 8(%rsp), %rsi

    xor %ebp, %ebp

    call jcos_main

    /*
     * Returning from jcos_main means process
     * termination.
     */
    call jcos_thread_exit

    ud2

.size _start, .-_start


.section .note.GNU-stack,"",@progbits