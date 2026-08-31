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

/*
 * Present after InterruptFrame when the CPU
 * changed privilege levels, such as Ring3 -> Ring0.
 */
typedef struct {
    u64 rsp;
    u64 ss;
} InterruptStackFrame;

_Static_assert(__builtin_offsetof(InterruptFrame, rax) == 0,"InterruptFrame rax offset");
_Static_assert(__builtin_offsetof(InterruptFrame, vector) == 120, "InterruptFrame vector offset");
_Static_assert(__builtin_offsetof(InterruptFrame, error_code) == 128, "InterruptFrame error offset");
_Static_assert(__builtin_offsetof(InterruptFrame, rip) == 136, "InterruptFrame rip offset");
_Static_assert(__builtin_offsetof(InterruptFrame, cs) == 144, "InterruptFrame cs offset");
_Static_assert(__builtin_offsetof(InterruptFrame, rflags) == 152, "InterruptFrame rflags offset");
_Static_assert(sizeof(InterruptFrame) == 160, "InterruptFrame size");

bool interrupt_from_user(const InterruptFrame *frame);
const InterruptStackFrame * interrupt_user_stack(const InterruptFrame *frame);

void idt_init(bool tss_ready);
void interrupt_dispatch(InterruptFrame *frame);
void interrupts_enable(void);
void interrupts_disable(void);
u64 interrupt_count(u8 vector);
u64 interrupt_spurious_count(void);

#endif
