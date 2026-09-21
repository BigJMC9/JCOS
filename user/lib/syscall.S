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

.global jcos_clock_ticks
.type jcos_clock_ticks,@function
jcos_clock_ticks:
    mov $JCOS_SYSCALL_CLOCK_TICKS, %eax
    int $0x80
    ret
.size jcos_clock_ticks, .-jcos_clock_ticks

.global jcos_clock_frequency
.type jcos_clock_frequency,@function
jcos_clock_frequency:
    mov $JCOS_SYSCALL_CLOCK_FREQUENCY, %eax
    int $0x80
    ret
.size jcos_clock_frequency, .-jcos_clock_frequency

/* RDI=cap, RSI=width*, RDX=height*. Kernel returns width in RCX, height in RDX. */
.global jcos_display_info
.type jcos_display_info,@function
jcos_display_info:
    test %rsi, %rsi
    jz .Ldisplay_info_failed
    test %rdx, %rdx
    jz .Ldisplay_info_failed
    push %rbx
    push %rsi
    push %rdx
    mov %rdi, %rbx
    mov $JCOS_SYSCALL_DISPLAY_INFO, %eax
    int $0x80
    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    jne .Ldisplay_info_pop_failed
    mov 8(%rsp), %r10
    mov %ecx, (%r10)
    mov 0(%rsp), %r10
    mov %edx, (%r10)
    mov $1, %eax
    add $16, %rsp
    pop %rbx
    ret
.Ldisplay_info_pop_failed:
    add $16, %rsp
    pop %rbx
.Ldisplay_info_failed:
    xor %eax, %eax
    ret
.size jcos_display_info, .-jcos_display_info

/* RDI=cap, RSI=pixels, RDX=width, RCX=height. */
.global jcos_display_present
.type jcos_display_present,@function
jcos_display_present:
    test %rsi, %rsi
    jz .Ldisplay_present_failed
    push %rbx
    mov %rdi, %rbx
    xchg %rcx, %rsi
    mov $JCOS_SYSCALL_DISPLAY_PRESENT, %eax
    int $0x80
    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    sete %al
    movzbl %al, %eax
    pop %rbx
    ret
.Ldisplay_present_failed:
    xor %eax, %eax
    ret
.size jcos_display_present, .-jcos_display_present

/* RDI=cap, RSI=rate*, RDX=max*. Kernel returns values in RCX/RDX. */
.global jcos_audio_info
.type jcos_audio_info,@function
jcos_audio_info:
    test %rsi, %rsi
    jz .Laudio_info_failed
    test %rdx, %rdx
    jz .Laudio_info_failed
    push %rbx
    push %rsi
    push %rdx
    mov %rdi, %rbx
    mov $JCOS_SYSCALL_AUDIO_INFO, %eax
    int $0x80
    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    jne .Laudio_info_pop_failed
    mov 8(%rsp), %r10
    mov %ecx, (%r10)
    mov 0(%rsp), %r10
    mov %edx, (%r10)
    mov $1, %eax
    add $16, %rsp
    pop %rbx
    ret
.Laudio_info_pop_failed:
    add $16, %rsp
    pop %rbx
.Laudio_info_failed:
    xor %eax, %eax
    ret
.size jcos_audio_info, .-jcos_audio_info

/* RDI=cap, RSI=PCM pointer, RDX=byte count. */
.global jcos_audio_write
.type jcos_audio_write,@function
jcos_audio_write:
    test %rsi, %rsi
    jz .Laudio_write_failed
    push %rbx
    mov %rdi, %rbx
    mov %rsi, %rcx
    mov $JCOS_SYSCALL_AUDIO_WRITE, %eax
    int $0x80
    cmp $JCOS_SYSCALL_RESULT_OK, %rax
    sete %al
    movzbl %al, %eax
    pop %rbx
    ret
.Laudio_write_failed:
    xor %eax, %eax
    ret
.size jcos_audio_write, .-jcos_audio_write


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
