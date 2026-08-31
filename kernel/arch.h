#ifndef JA_OS_ARCH_H
#define JA_OS_ARCH_H

#include "types.h"

void arch_cli(void);
void arch_sti(void);
void arch_pause(void);
void arch_halt(void);
NORETURN void cpu_halt_forever(void);
NORETURN void arch_triple_fault(void);
NORETURN void arch_enter_user(u64 rip, u64 rsp);

u8  arch_in8(u16 port);
void arch_out8(u16 port, u8 value);
u32 arch_in32(u16 port);
void arch_out32(u16 port, u32 value);

u64 arch_read_msr(u32 msr);
void arch_write_msr(u32 msr, u64 value);
void arch_cpuid(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d);
u16 arch_read_cs(void);

u64 arch_read_cr2(void);
u64 arch_read_cr3(void);
void arch_write_cr3(u64 value);
u64 arch_read_cr4(void);

typedef struct PACKED {
    u16 limit;
    u64 base;
} ArchDescriptorTablePointer;

typedef struct {
    u64 rbx;
    u64 rbp;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;

    u64 rsp;
    u64 rip;
} CpuContext;

_Static_assert(__builtin_offsetof(CpuContext, rbx) == 0, "CpuContext rbx offset");
_Static_assert(__builtin_offsetof(CpuContext, rbp) == 8, "CpuContext rbp offset");
_Static_assert(__builtin_offsetof(CpuContext, r12) == 16, "CpuContext r12 offset");
_Static_assert(__builtin_offsetof(CpuContext, r13) == 24, "CpuContext r13 offset");
_Static_assert(__builtin_offsetof(CpuContext, r14) == 32, "CpuContext r14 offset");
_Static_assert(__builtin_offsetof(CpuContext, r15) == 40, "CpuContext r15 offset");
_Static_assert(__builtin_offsetof(CpuContext, rsp) == 48, "CpuContext rsp offset");
_Static_assert(__builtin_offsetof(CpuContext, rip) == 56, "CpuContext rip offset");
_Static_assert(sizeof(CpuContext) == 64, "CpuContext size");

void arch_context_switch(
    CpuContext *old_context,
    const CpuContext *new_context
);

NORETURN void arch_context_enter(
    const CpuContext *context
);

void arch_store_gdt(
    ArchDescriptorTablePointer *descriptor
);

void arch_load_gdtr(
    const ArchDescriptorTablePointer *descriptor
);

void arch_reload_segments(
    u16 code_selector,
    u16 data_selector
);

void arch_load_tr(
    u16 selector
);

u16 arch_read_tr(void);

void arch_load_idt(const void *descriptor);

/* Entries 0..47 are vectors 0..47. Entry 48 is vector 128. Entry 49 is vector 255. */
extern const s32 isr_stub_offsets[50];

#endif
