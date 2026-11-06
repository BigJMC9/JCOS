#include "device_mmio.h"
bool device_mmio_interrupts_enabled(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    return (flags & (1ULL << 9)) != 0;
}
u32 device_mmio_load32(u64 address) {
    u32 value;
    /* This provider uses strict UC leaves. Fence compiler/CPU observations;
     * exactly one aligned DWORD load, with no polling or interrupt masking. */
    __asm__ volatile ("lfence\n\tmovl (%1), %0\n\tlfence"
        : "=r"(value) : "r"(address) : "memory");
    return value;
}
