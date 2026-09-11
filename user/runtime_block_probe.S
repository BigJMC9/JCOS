#include "../include/runtime_block_test_abi.h"

.section .text

.extern jcos_ipc_receive_blocking
.extern jcos_ipc_send_blocking

.global jcos_runtime_probe_entry_rsp_mod16
.type jcos_runtime_probe_entry_rsp_mod16,@function

jcos_runtime_probe_entry_rsp_mod16:
    mov %rsp, %rax
    and $0xF, %rax
    ret

.size jcos_runtime_probe_entry_rsp_mod16, .-jcos_runtime_probe_entry_rsp_mod16

/*
 * JcosU64 jcos_runtime_probe_receive(
 *     CapabilityHandle handle,
 *     JcosIpcMessage *message
 * );
 *
 * Returns a bitmask proving:
 *
 *   - blocking RECEIVE succeeded
 *   - call-site stack was aligned
 *   - RBX/RBP/R12-R15 survived
 */
.global jcos_runtime_probe_receive
.type jcos_runtime_probe_receive,@function

jcos_runtime_probe_receive:
    /*
     * Preserve the C caller's real callee-saved
     * state before replacing it with sentinels.
     */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /*
     * 24 bytes makes RSP 16-byte aligned before
     * calling another SysV function.
     *
     * +0  = wrapper result
     * +8  = call-site RSP % 16
     */
    sub $24, %rsp

    mov %rsp, %rax
    and $0xF, %rax
    mov %rax, 8(%rsp)


    movabs $JCOS_RTB_SENTINEL_RBX, %rbx
    movabs $JCOS_RTB_SENTINEL_RBP, %rbp
    movabs $JCOS_RTB_SENTINEL_R12, %r12
    movabs $JCOS_RTB_SENTINEL_R13, %r13
    movabs $JCOS_RTB_SENTINEL_R14, %r14
    movabs $JCOS_RTB_SENTINEL_R15, %r15


    call jcos_ipc_receive_blocking

    mov %rax, 0(%rsp)


    xor %eax, %eax


    cmpq $JCOS_SYSCALL_RESULT_OK, 0(%rsp)
    jne 1f

    or $JCOS_RTB_PROBE_RESULT_OK, %rax

1:
    cmpq $0, 8(%rsp)
    jne 2f

    or $JCOS_RTB_PROBE_STACK_ALIGNED, %rax

2:
    movabs $JCOS_RTB_SENTINEL_RBX, %r10
    cmp %r10, %rbx
    jne 3f

    or $JCOS_RTB_PROBE_RBX_PRESERVED, %rax

3:
    movabs $JCOS_RTB_SENTINEL_RBP, %r10
    cmp %r10, %rbp
    jne 4f

    or $JCOS_RTB_PROBE_RBP_PRESERVED, %rax

4:
    movabs $JCOS_RTB_SENTINEL_R12, %r10
    cmp %r10, %r12
    jne 5f

    or $JCOS_RTB_PROBE_R12_PRESERVED, %rax

5:
    movabs $JCOS_RTB_SENTINEL_R13, %r10
    cmp %r10, %r13
    jne 6f

    or $JCOS_RTB_PROBE_R13_PRESERVED, %rax

6:
    movabs $JCOS_RTB_SENTINEL_R14, %r10
    cmp %r10, %r14
    jne 7f

    or $JCOS_RTB_PROBE_R14_PRESERVED, %rax

7:
    movabs $JCOS_RTB_SENTINEL_R15, %r10
    cmp %r10, %r15
    jne 8f

    or $JCOS_RTB_PROBE_R15_PRESERVED, %rax

8:
    add $24, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    ret

.size jcos_runtime_probe_receive, .-jcos_runtime_probe_receive


/*
 * Same ABI-preservation probe for a blocking
 * SEND.
 */
.global jcos_runtime_probe_send
.type jcos_runtime_probe_send,@function

jcos_runtime_probe_send:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    sub $24, %rsp

    mov %rsp, %rax
    and $0xF, %rax
    mov %rax, 8(%rsp)


    movabs $JCOS_RTB_SENTINEL_RBX, %rbx
    movabs $JCOS_RTB_SENTINEL_RBP, %rbp
    movabs $JCOS_RTB_SENTINEL_R12, %r12
    movabs $JCOS_RTB_SENTINEL_R13, %r13
    movabs $JCOS_RTB_SENTINEL_R14, %r14
    movabs $JCOS_RTB_SENTINEL_R15, %r15


    call jcos_ipc_send_blocking

    mov %rax, 0(%rsp)


    xor %eax, %eax


    cmpq $JCOS_SYSCALL_RESULT_OK, 0(%rsp)
    jne 11f

    or $JCOS_RTB_PROBE_RESULT_OK, %rax

11:
    cmpq $0, 8(%rsp)
    jne 12f

    or $JCOS_RTB_PROBE_STACK_ALIGNED, %rax

12:
    movabs $JCOS_RTB_SENTINEL_RBX, %r10
    cmp %r10, %rbx
    jne 13f

    or $JCOS_RTB_PROBE_RBX_PRESERVED, %rax

13:
    movabs $JCOS_RTB_SENTINEL_RBP, %r10
    cmp %r10, %rbp
    jne 14f

    or $JCOS_RTB_PROBE_RBP_PRESERVED, %rax

14:
    movabs $JCOS_RTB_SENTINEL_R12, %r10
    cmp %r10, %r12
    jne 15f

    or $JCOS_RTB_PROBE_R12_PRESERVED, %rax

15:
    movabs $JCOS_RTB_SENTINEL_R13, %r10
    cmp %r10, %r13
    jne 16f

    or $JCOS_RTB_PROBE_R13_PRESERVED, %rax

16:
    movabs $JCOS_RTB_SENTINEL_R14, %r10
    cmp %r10, %r14
    jne 17f

    or $JCOS_RTB_PROBE_R14_PRESERVED, %rax

17:
    movabs $JCOS_RTB_SENTINEL_R15, %r10
    cmp %r10, %r15
    jne 18f

    or $JCOS_RTB_PROBE_R15_PRESERVED, %rax

18:
    add $24, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    ret

.size jcos_runtime_probe_send, .-jcos_runtime_probe_send


.section .note.GNU-stack,"",@progbits