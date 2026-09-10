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

u64 arch_reschedule_interrupt(u64 operation);

typedef struct PACKED {
    u16 limit;
    u64 base;
} ArchDescriptorTablePointer;

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

/*
 * Entries:
 *
 *   0..47  -> vectors 0..47
 *   48     -> vector 0x80
 *   49     -> vector 0x81
 *   50     -> vector 0xFF
 */
extern const s32 isr_stub_offsets[51];

#endif
