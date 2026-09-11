#include "../../include/user_abi.h"

.section .text


.global jcos_thread_id
.type jcos_thread_id,@function

jcos_thread_id:
    mov $JCOS_SYSCALL_THREAD_ID, %eax
    int $0x80
    ret

.size jcos_thread_id, .-jcos_thread_id


.global jcos_thread_exit
.type jcos_thread_exit,@function

jcos_thread_exit:
    mov $JCOS_SYSCALL_THREAD_EXIT, %eax
    int $0x80

    /*
     * THREAD_EXIT must never return.
     */
    ud2

.size jcos_thread_exit, .-jcos_thread_exit


/*
 * int jcos_ipc_try_send(
 *     handle,
 *     message
 * );
 *
 * SysV input:
 *
 *   RDI = handle
 *   RSI = message
 *
 * Kernel ABI:
 *
 *   RBX = capability
 *   RCX = word_count
 *   RDX = word0
 *   RSI = word1
 *   RDI = word2
 *   R8  = word3
 */

.global jcos_ipc_try_send
.type jcos_ipc_try_send,@function

jcos_ipc_try_send:
    test %rsi, %rsi
    jz .Ltry_send_failed

    push %rbx

    mov %rdi, %rbx
    mov %rsi, %r10

    mov JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10), %ecx

    mov 0(%r10), %rdx
    mov 8(%r10), %rsi
    mov 16(%r10), %rdi
    mov 24(%r10), %r8

    mov $JCOS_SYSCALL_IPC_TRY_SEND, %eax
    int $0x80

    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    sete %al
    movzbl %al, %eax

    pop %rbx
    ret

.Ltry_send_failed:
    xor %eax, %eax
    ret

.size jcos_ipc_try_send, .-jcos_ipc_try_send


.global jcos_ipc_send_blocking
.type jcos_ipc_send_blocking,@function

jcos_ipc_send_blocking:
    test %rsi, %rsi
    jz .Lblocking_send_failed

    push %rbx

    mov %rdi, %rbx
    mov %rsi, %r10

    mov JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10), %ecx

    mov 0(%r10), %rdx
    mov 8(%r10), %rsi
    mov 16(%r10), %rdi
    mov 24(%r10), %r8

    mov $JCOS_SYSCALL_IPC_SEND_BLOCKING, %eax
    int $0x80

    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    sete %al
    movzbl %al, %eax

    pop %rbx
    ret

.Lblocking_send_failed:
    xor %eax, %eax
    ret

.size jcos_ipc_send_blocking, .-jcos_ipc_send_blocking


/*
 * RECEIVE wrappers.
 *
 * SysV input:
 *
 *   RDI = capability
 *   RSI = output message
 *
 * Kernel return:
 *
 *   RAX = result
 *   RCX = word_count
 *   RDX = word0
 *   RSI = word1
 *   RDI = word2
 *   R8  = word3
 */

.global jcos_ipc_try_receive
.type jcos_ipc_try_receive,@function

jcos_ipc_try_receive:
    test %rsi, %rsi
    jz .Ltry_receive_null

    push %rbx
    push %rsi

    mov %rdi, %rbx

    mov $JCOS_SYSCALL_IPC_TRY_RECEIVE, %eax
    int $0x80

    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    jne .Ltry_receive_failed

    mov 0(%rsp), %r10

    mov %rdx, 0(%r10)
    mov %rsi, 8(%r10)
    mov %rdi, 16(%r10)
    mov %r8, 24(%r10)
    mov %ecx, JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10)

    mov $1, %eax
    jmp .Ltry_receive_done

.Ltry_receive_failed:
    mov 0(%rsp), %r10

    movq $0, 0(%r10)
    movq $0, 8(%r10)
    movq $0, 16(%r10)
    movq $0, 24(%r10)
    movl $0, JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10)

    xor %eax, %eax

.Ltry_receive_done:
    add $8, %rsp
    pop %rbx
    ret

.Ltry_receive_null:
    xor %eax, %eax
    ret

.size jcos_ipc_try_receive, .-jcos_ipc_try_receive


.global jcos_ipc_receive_blocking
.type jcos_ipc_receive_blocking,@function

jcos_ipc_receive_blocking:
    test %rsi, %rsi
    jz .Lblocking_receive_null

    push %rbx
    push %rsi

    mov %rdi, %rbx

    mov $JCOS_SYSCALL_IPC_RECEIVE_BLOCKING, %eax
    int $0x80

    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    jne .Lblocking_receive_failed

    mov 0(%rsp), %r10

    mov %rdx, 0(%r10)
    mov %rsi, 8(%r10)
    mov %rdi, 16(%r10)
    mov %r8, 24(%r10)
    mov %ecx, JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10)

    mov $1, %eax
    jmp .Lblocking_receive_done

.Lblocking_receive_failed:
    mov 0(%rsp), %r10

    movq $0, 0(%r10)
    movq $0, 8(%r10)
    movq $0, 16(%r10)
    movq $0, 24(%r10)
    movl $0, JCOS_IPC_MESSAGE_WORD_COUNT_OFFSET(%r10)

    xor %eax, %eax

.Lblocking_receive_done:
    add $8, %rsp
    pop %rbx
    ret

.Lblocking_receive_null:
    xor %eax, %eax
    ret

.size jcos_ipc_receive_blocking, .-jcos_ipc_receive_blocking


.section .note.GNU-stack,"",@progbits