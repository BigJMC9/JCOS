BITS 64
DEFAULT REL

SECTION .text
GLOBAL _start
GLOBAL cpu_halt_forever
GLOBAL arch_cli
GLOBAL arch_sti
GLOBAL arch_pause
GLOBAL arch_halt
GLOBAL arch_in8
GLOBAL arch_out8
GLOBAL arch_in32
GLOBAL arch_out32
GLOBAL arch_read_msr
GLOBAL arch_write_msr
GLOBAL arch_cpuid
GLOBAL arch_read_cs
GLOBAL arch_read_cr2
GLOBAL arch_read_cr3
GLOBAL arch_write_cr3
GLOBAL arch_read_cr4
GLOBAL arch_load_idt
GLOBAL arch_triple_fault
GLOBAL arch_store_gdt
GLOBAL arch_load_gdtr
GLOBAL arch_reload_segments
GLOBAL arch_load_tr
GLOBAL arch_read_tr
GLOBAL isr_stub_offsets
EXTERN kernel_main
EXTERN interrupt_dispatch

; UEFI calls the ELF entry with Microsoft x64 ABI (RCX = BootInfo*).
; The standalone C kernel uses the System V ABI (RDI = first argument).
_start:
    cli
    cld
    mov rbx, rcx
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
    mov [r10], eax
    mov [r11], ebx
    mov [r12], ecx
    mov [r13], edx
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
    ;
    ; SysV ABI:
    ;
    ; RDI = code selector
    ; RSI = data selector
    ;
    ; RETFQ expects:
    ;
    ;   [RSP + 0] = new RIP
    ;   [RSP + 8] = new CS
    ;
    ; The ordinary C return address remains below
    ; those values and is consumed by RET afterward.
    ;

    movzx eax, di
    push rax

    lea rax, [rel .reload_cs]
    push rax

    retfq

.reload_cs:
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
    mov word [rsp], 0
    mov qword [rsp + 2], 0
    lidt [rsp]
    int3
    jmp cpu_halt_forever

isr_stub_0:
    push qword 0
    push qword 0
    jmp isr_common

isr_stub_1:
    push qword 0
    push qword 1
    jmp isr_common

isr_stub_2:
    push qword 0
    push qword 2
    jmp isr_common

isr_stub_3:
    push qword 0
    push qword 3
    jmp isr_common

isr_stub_4:
    push qword 0
    push qword 4
    jmp isr_common

isr_stub_5:
    push qword 0
    push qword 5
    jmp isr_common

isr_stub_6:
    push qword 0
    push qword 6
    jmp isr_common

isr_stub_7:
    push qword 0
    push qword 7
    jmp isr_common

isr_stub_8:
    push qword 8
    jmp isr_common

isr_stub_9:
    push qword 0
    push qword 9
    jmp isr_common

isr_stub_10:
    push qword 10
    jmp isr_common

isr_stub_11:
    push qword 11
    jmp isr_common

isr_stub_12:
    push qword 12
    jmp isr_common

isr_stub_13:
    push qword 13
    jmp isr_common

isr_stub_14:
    push qword 14
    jmp isr_common

isr_stub_15:
    push qword 0
    push qword 15
    jmp isr_common

isr_stub_16:
    push qword 0
    push qword 16
    jmp isr_common

isr_stub_17:
    push qword 17
    jmp isr_common

isr_stub_18:
    push qword 0
    push qword 18
    jmp isr_common

isr_stub_19:
    push qword 0
    push qword 19
    jmp isr_common

isr_stub_20:
    push qword 0
    push qword 20
    jmp isr_common

isr_stub_21:
    push qword 21
    jmp isr_common

isr_stub_22:
    push qword 0
    push qword 22
    jmp isr_common

isr_stub_23:
    push qword 0
    push qword 23
    jmp isr_common

isr_stub_24:
    push qword 0
    push qword 24
    jmp isr_common

isr_stub_25:
    push qword 0
    push qword 25
    jmp isr_common

isr_stub_26:
    push qword 0
    push qword 26
    jmp isr_common

isr_stub_27:
    push qword 0
    push qword 27
    jmp isr_common

isr_stub_28:
    push qword 0
    push qword 28
    jmp isr_common

isr_stub_29:
    push qword 29
    jmp isr_common

isr_stub_30:
    push qword 30
    jmp isr_common

isr_stub_31:
    push qword 0
    push qword 31
    jmp isr_common

isr_stub_32:
    push qword 0
    push qword 32
    jmp isr_common

isr_stub_33:
    push qword 0
    push qword 33
    jmp isr_common

isr_stub_34:
    push qword 0
    push qword 34
    jmp isr_common

isr_stub_35:
    push qword 0
    push qword 35
    jmp isr_common

isr_stub_36:
    push qword 0
    push qword 36
    jmp isr_common

isr_stub_37:
    push qword 0
    push qword 37
    jmp isr_common

isr_stub_38:
    push qword 0
    push qword 38
    jmp isr_common

isr_stub_39:
    push qword 0
    push qword 39
    jmp isr_common

isr_stub_40:
    push qword 0
    push qword 40
    jmp isr_common

isr_stub_41:
    push qword 0
    push qword 41
    jmp isr_common

isr_stub_42:
    push qword 0
    push qword 42
    jmp isr_common

isr_stub_43:
    push qword 0
    push qword 43
    jmp isr_common

isr_stub_44:
    push qword 0
    push qword 44
    jmp isr_common

isr_stub_45:
    push qword 0
    push qword 45
    jmp isr_common

isr_stub_46:
    push qword 0
    push qword 46
    jmp isr_common

isr_stub_47:
    push qword 0
    push qword 47
    jmp isr_common

isr_stub_255:
    push qword 0
    push qword 255
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

align 4
isr_stub_offsets:
    dd isr_stub_0 - isr_stub_offsets
    dd isr_stub_1 - isr_stub_offsets
    dd isr_stub_2 - isr_stub_offsets
    dd isr_stub_3 - isr_stub_offsets
    dd isr_stub_4 - isr_stub_offsets
    dd isr_stub_5 - isr_stub_offsets
    dd isr_stub_6 - isr_stub_offsets
    dd isr_stub_7 - isr_stub_offsets
    dd isr_stub_8 - isr_stub_offsets
    dd isr_stub_9 - isr_stub_offsets
    dd isr_stub_10 - isr_stub_offsets
    dd isr_stub_11 - isr_stub_offsets
    dd isr_stub_12 - isr_stub_offsets
    dd isr_stub_13 - isr_stub_offsets
    dd isr_stub_14 - isr_stub_offsets
    dd isr_stub_15 - isr_stub_offsets
    dd isr_stub_16 - isr_stub_offsets
    dd isr_stub_17 - isr_stub_offsets
    dd isr_stub_18 - isr_stub_offsets
    dd isr_stub_19 - isr_stub_offsets
    dd isr_stub_20 - isr_stub_offsets
    dd isr_stub_21 - isr_stub_offsets
    dd isr_stub_22 - isr_stub_offsets
    dd isr_stub_23 - isr_stub_offsets
    dd isr_stub_24 - isr_stub_offsets
    dd isr_stub_25 - isr_stub_offsets
    dd isr_stub_26 - isr_stub_offsets
    dd isr_stub_27 - isr_stub_offsets
    dd isr_stub_28 - isr_stub_offsets
    dd isr_stub_29 - isr_stub_offsets
    dd isr_stub_30 - isr_stub_offsets
    dd isr_stub_31 - isr_stub_offsets
    dd isr_stub_32 - isr_stub_offsets
    dd isr_stub_33 - isr_stub_offsets
    dd isr_stub_34 - isr_stub_offsets
    dd isr_stub_35 - isr_stub_offsets
    dd isr_stub_36 - isr_stub_offsets
    dd isr_stub_37 - isr_stub_offsets
    dd isr_stub_38 - isr_stub_offsets
    dd isr_stub_39 - isr_stub_offsets
    dd isr_stub_40 - isr_stub_offsets
    dd isr_stub_41 - isr_stub_offsets
    dd isr_stub_42 - isr_stub_offsets
    dd isr_stub_43 - isr_stub_offsets
    dd isr_stub_44 - isr_stub_offsets
    dd isr_stub_45 - isr_stub_offsets
    dd isr_stub_46 - isr_stub_offsets
    dd isr_stub_47 - isr_stub_offsets
    dd isr_stub_255 - isr_stub_offsets
