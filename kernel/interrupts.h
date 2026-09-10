#ifndef JA_OS_INTERRUPTS_H
#define JA_OS_INTERRUPTS_H

#include "types.h"

#define RESCHEDULE_VECTOR 0x81U

typedef struct {
    u64 rax, rbx, rcx, rdx, rbp, rdi, rsi;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;

    u64 vector;
    u64 error_code;

    u64 rip;
    u64 cs;
    u64 rflags;
} InterruptFrame;

_Static_assert(__builtin_offsetof(InterruptFrame, rax) == 0,"InterruptFrame rax offset");
_Static_assert(__builtin_offsetof(InterruptFrame, vector) == 120, "InterruptFrame vector offset");
_Static_assert(__builtin_offsetof(InterruptFrame, error_code) == 128, "InterruptFrame error offset");
_Static_assert(__builtin_offsetof(InterruptFrame, rip) == 136, "InterruptFrame rip offset");
_Static_assert(__builtin_offsetof(InterruptFrame, cs) == 144, "InterruptFrame cs offset");
_Static_assert(__builtin_offsetof(InterruptFrame, rflags) == 152, "InterruptFrame rflags offset");
_Static_assert(sizeof(InterruptFrame) == 160, "InterruptFrame size");

/*
 * Tail of an x86-64 interrupt-return frame.
 *
 * In 64-bit mode IRETQ consumes RSP and SS
 * after RIP, CS, and RFLAGS.
 *
 * For Ring3 -> Ring0 these contain the saved
 * user RSP/SS. Kernel interrupt contexts also
 * require these fields when synthesizing an
 * IRETQ return frame.
 */
typedef struct {
    u64 rsp;
    u64 ss;
} InterruptStackFrame;

_Static_assert(sizeof(InterruptStackFrame) == 16, "InterruptStackFrame size");

typedef struct {
    bool valid;

    u64 thread_id;

    u64 vector;
    u64 error_code;

    u64 rip;
    u64 rax;

    u64 user_rsp;
    u64 user_ss;

    u64 cr2;
} UserFaultInfo;

void interrupt_clear_user_fault(void);
bool interrupt_last_user_fault(UserFaultInfo *info);
bool interrupt_from_user(const InterruptFrame *frame);
const InterruptStackFrame * interrupt_user_stack(const InterruptFrame *frame);

void idt_init(bool tss_ready);
InterruptFrame *interrupt_dispatch(InterruptFrame *frame);
void interrupts_enable(void);
void interrupts_disable(void);
u64 interrupt_count(u8 vector);
u64 interrupt_spurious_count(void);

#endif
