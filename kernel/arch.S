.intel_syntax noprefix
.section .text
.global _start
.global cpu_halt_forever
.global arch_cli
.global arch_sti
.global arch_pause
.global arch_halt
.global arch_in8
.global arch_out8
.global arch_in32
.global arch_out32
.global arch_read_msr
.global arch_write_msr
.global arch_cpuid
.global arch_read_cs
.global arch_read_cr2
.global arch_read_cr3
.global arch_write_cr3
.global arch_read_cr4
.global arch_load_idt
.global arch_triple_fault
.global arch_store_gdt
.global arch_load_gdtr
.global arch_reload_segments
.global arch_load_tr
.global arch_read_tr
.global isr_stub_offsets
.extern kernel_main
.extern interrupt_dispatch

_start:
    cli
    cld
    mov rbx, rcx
    mov rsp, QWORD PTR [rbx]
    and rsp, -16
    xor rbp, rbp
    mov rdi, rbx
    call kernel_main

cpu_halt_forever:
.Lhang:
    cli
    hlt
    jmp .Lhang

arch_cli:
    cli
    ret
arch_sti:
    sti
    ret
arch_pause:
    pause
    ret
arch_halt:
    hlt
    ret

arch_in8:
    mov dx, di
    xor eax, eax
    in al, dx
    ret
arch_out8:
    mov dx, di
    mov eax, esi
    out dx, al
    ret
arch_in32:
    mov dx, di
    in eax, dx
    ret
arch_out32:
    mov dx, di
    mov eax, esi
    out dx, eax
    ret

arch_read_msr:
    mov ecx, edi
    rdmsr
    shl rdx, 32
    or rax, rdx
    ret
arch_write_msr:
    mov ecx, edi
    mov eax, esi
    mov rdx, rsi
    shr rdx, 32
    wrmsr
    ret

arch_cpuid:
    push rbx
    push r12
    push r13
    mov r10, rdx
    mov r11, rcx
    mov r12, r8
    mov r13, r9
    mov eax, edi
    mov ecx, esi
    cpuid
    mov DWORD PTR [r10], eax
    mov DWORD PTR [r11], ebx
    mov DWORD PTR [r12], ecx
    mov DWORD PTR [r13], edx
    pop r13
    pop r12
    pop rbx
    ret

arch_read_cs:
    xor eax, eax
    mov ax, cs
    ret
arch_read_cr2:
    mov rax, cr2
    ret
arch_read_cr3:
    mov rax, cr3
    ret
arch_write_cr3:
    mov cr3, rdi
    ret
arch_read_cr4:
    mov rax, cr4
    ret

arch_store_gdt:
    sgdt [rdi]
    ret
arch_load_gdtr:
    lgdt [rdi]
    ret


arch_reload_segments:
    movzx eax, di
    push rax

    lea rax, [rip + .Lgdt_reload_cs]
    push rax

    /*
     * RETFQ
     *
     * Encoding is REX.W + RETF.
     */
    .byte 0x48, 0xCB

.Lgdt_reload_cs:
    mov ax, si

    mov ds, ax
    mov es, ax
    mov ss, ax

    ret

arch_load_tr:
    mov ax, di
    ltr ax
    ret
arch_read_tr:
    xor eax, eax
    str ax
    ret

arch_load_idt:
    lidt [rdi]
    ret
arch_triple_fault:
    cli
    sub rsp, 16
    mov WORD PTR [rsp], 0
    mov QWORD PTR [rsp + 2], 0
    lidt [rsp]
    int3
    jmp cpu_halt_forever

isr_stub_0:
    push 0
    push 0
    jmp isr_common

isr_stub_1:
    push 0
    push 1
    jmp isr_common

isr_stub_2:
    push 0
    push 2
    jmp isr_common

isr_stub_3:
    push 0
    push 3
    jmp isr_common

isr_stub_4:
    push 0
    push 4
    jmp isr_common

isr_stub_5:
    push 0
    push 5
    jmp isr_common

isr_stub_6:
    push 0
    push 6
    jmp isr_common

isr_stub_7:
    push 0
    push 7
    jmp isr_common

isr_stub_8:
    push 8
    jmp isr_common

isr_stub_9:
    push 0
    push 9
    jmp isr_common

isr_stub_10:
    push 10
    jmp isr_common

isr_stub_11:
    push 11
    jmp isr_common

isr_stub_12:
    push 12
    jmp isr_common

isr_stub_13:
    push 13
    jmp isr_common

isr_stub_14:
    push 14
    jmp isr_common

isr_stub_15:
    push 0
    push 15
    jmp isr_common

isr_stub_16:
    push 0
    push 16
    jmp isr_common

isr_stub_17:
    push 17
    jmp isr_common

isr_stub_18:
    push 0
    push 18
    jmp isr_common

isr_stub_19:
    push 0
    push 19
    jmp isr_common

isr_stub_20:
    push 0
    push 20
    jmp isr_common

isr_stub_21:
    push 21
    jmp isr_common

isr_stub_22:
    push 0
    push 22
    jmp isr_common

isr_stub_23:
    push 0
    push 23
    jmp isr_common

isr_stub_24:
    push 0
    push 24
    jmp isr_common

isr_stub_25:
    push 0
    push 25
    jmp isr_common

isr_stub_26:
    push 0
    push 26
    jmp isr_common

isr_stub_27:
    push 0
    push 27
    jmp isr_common

isr_stub_28:
    push 0
    push 28
    jmp isr_common

isr_stub_29:
    push 29
    jmp isr_common

isr_stub_30:
    push 30
    jmp isr_common

isr_stub_31:
    push 0
    push 31
    jmp isr_common

isr_stub_32:
    push 0
    push 32
    jmp isr_common

isr_stub_33:
    push 0
    push 33
    jmp isr_common

isr_stub_34:
    push 0
    push 34
    jmp isr_common

isr_stub_35:
    push 0
    push 35
    jmp isr_common

isr_stub_36:
    push 0
    push 36
    jmp isr_common

isr_stub_37:
    push 0
    push 37
    jmp isr_common

isr_stub_38:
    push 0
    push 38
    jmp isr_common

isr_stub_39:
    push 0
    push 39
    jmp isr_common

isr_stub_40:
    push 0
    push 40
    jmp isr_common

isr_stub_41:
    push 0
    push 41
    jmp isr_common

isr_stub_42:
    push 0
    push 42
    jmp isr_common

isr_stub_43:
    push 0
    push 43
    jmp isr_common

isr_stub_44:
    push 0
    push 44
    jmp isr_common

isr_stub_45:
    push 0
    push 45
    jmp isr_common

isr_stub_46:
    push 0
    push 46
    jmp isr_common

isr_stub_47:
    push 0
    push 47
    jmp isr_common

isr_stub_255:
    push 0
    push 255
    jmp isr_common

isr_common:
    cld
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rsi
    push rdi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax
    mov r12, rsp
    mov rdi, rsp
    and rsp, -16
    call interrupt_dispatch
    mov rsp, r12
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rbp
    pop rdi
    pop rsi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16
    iretq

.balign 4
isr_stub_offsets:
    .long isr_stub_0 - isr_stub_offsets
    .long isr_stub_1 - isr_stub_offsets
    .long isr_stub_2 - isr_stub_offsets
    .long isr_stub_3 - isr_stub_offsets
    .long isr_stub_4 - isr_stub_offsets
    .long isr_stub_5 - isr_stub_offsets
    .long isr_stub_6 - isr_stub_offsets
    .long isr_stub_7 - isr_stub_offsets
    .long isr_stub_8 - isr_stub_offsets
    .long isr_stub_9 - isr_stub_offsets
    .long isr_stub_10 - isr_stub_offsets
    .long isr_stub_11 - isr_stub_offsets
    .long isr_stub_12 - isr_stub_offsets
    .long isr_stub_13 - isr_stub_offsets
    .long isr_stub_14 - isr_stub_offsets
    .long isr_stub_15 - isr_stub_offsets
    .long isr_stub_16 - isr_stub_offsets
    .long isr_stub_17 - isr_stub_offsets
    .long isr_stub_18 - isr_stub_offsets
    .long isr_stub_19 - isr_stub_offsets
    .long isr_stub_20 - isr_stub_offsets
    .long isr_stub_21 - isr_stub_offsets
    .long isr_stub_22 - isr_stub_offsets
    .long isr_stub_23 - isr_stub_offsets
    .long isr_stub_24 - isr_stub_offsets
    .long isr_stub_25 - isr_stub_offsets
    .long isr_stub_26 - isr_stub_offsets
    .long isr_stub_27 - isr_stub_offsets
    .long isr_stub_28 - isr_stub_offsets
    .long isr_stub_29 - isr_stub_offsets
    .long isr_stub_30 - isr_stub_offsets
    .long isr_stub_31 - isr_stub_offsets
    .long isr_stub_32 - isr_stub_offsets
    .long isr_stub_33 - isr_stub_offsets
    .long isr_stub_34 - isr_stub_offsets
    .long isr_stub_35 - isr_stub_offsets
    .long isr_stub_36 - isr_stub_offsets
    .long isr_stub_37 - isr_stub_offsets
    .long isr_stub_38 - isr_stub_offsets
    .long isr_stub_39 - isr_stub_offsets
    .long isr_stub_40 - isr_stub_offsets
    .long isr_stub_41 - isr_stub_offsets
    .long isr_stub_42 - isr_stub_offsets
    .long isr_stub_43 - isr_stub_offsets
    .long isr_stub_44 - isr_stub_offsets
    .long isr_stub_45 - isr_stub_offsets
    .long isr_stub_46 - isr_stub_offsets
    .long isr_stub_47 - isr_stub_offsets
    .long isr_stub_255 - isr_stub_offsets

.section .note.GNU-stack,"",@progbits
