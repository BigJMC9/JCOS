#ifndef JA_OS_INTERRUPTS_H
#define JA_OS_INTERRUPTS_H

#include "types.h"

typedef struct {
    u64 rax, rbx, rcx, rdx, rbp, rdi, rsi;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cs;
    u64 rflags;
} InterruptFrame;

void idt_init(void);
void interrupt_dispatch(InterruptFrame *frame);
void interrupts_enable(void);
void interrupts_disable(void);
u64 interrupt_count(u8 vector);
u64 interrupt_spurious_count(void);

#endif
