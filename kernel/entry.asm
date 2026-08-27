BITS 64
DEFAULT REL

SECTION .text
GLOBAL _start
GLOBAL cpu_halt_forever
EXTERN kernel_main

; UEFI loader calls the ELF entry using Microsoft x64 ABI:
;   RCX = BootInfo*
; The C kernel is built with the System V ABI:
;   RDI = first argument
_start:
    cli
    cld
    mov rbx, rcx

    ; BootInfo.kernel_stack_top is deliberately field 0.
    mov rsp, [rbx]
    and rsp, -16
    xor rbp, rbp

    mov rdi, rbx
    call kernel_main

cpu_halt_forever:
.hang:
    cli
    hlt
    jmp .hang
