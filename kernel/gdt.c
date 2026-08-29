#include "gdt.h"
#include "arch.h"
#include "lib.h"

#define GDT_ENTRY_COUNT 7U

#define GDT_NULL_INDEX        0U
#define GDT_KERNEL_CODE_INDEX 1U
#define GDT_KERNEL_DATA_INDEX 2U
#define GDT_USER_DATA_INDEX   3U
#define GDT_USER_CODE_INDEX   4U
#define GDT_TSS_LOW_INDEX     5U
#define GDT_TSS_HIGH_INDEX    6U

#define DOUBLE_FAULT_STACK_SIZE 16384U

typedef struct PACKED {
    u32 reserved0;

    u64 rsp0;
    u64 rsp1;
    u64 rsp2;

    u64 reserved1;

    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;

    u64 reserved2;

    u16 reserved3;
    u16 iomap_base;
} Tss64;

_Static_assert(sizeof(Tss64) == 104, "x86-64 TSS must be 104 bytes");

static u64 g_gdt[GDT_ENTRY_COUNT];

static Tss64 g_tss;

static ArchDescriptorTablePointer g_gdtr;

/*
 * Keep IST1 independent from the ordinary
 * kernel stack.
 *
 * It lives inside the kernel image, so
 * existing kernel mapping automatically maps it.
 */
static u8 g_double_fault_stack[
    DOUBLE_FAULT_STACK_SIZE
] __attribute__((aligned(16)));

static u64 make_code_data_descriptor(u8 access, u8 flags) {
    u64 descriptor = 0;

    /*
     * Flat descriptor.
     *
     * Base = 0
     * Limit = 0xFFFFF
     */
    descriptor |= 0xFFFFULL;
    descriptor |= (u64)access << 40;
    descriptor |= 0xFULL << 48;
    descriptor |= ((u64)flags & 0xFULL) << 52;

    return descriptor;
}

static void install_tss_descriptor(u64 base, u32 limit) {
    u64 low = 0;

    low |= (u64)(limit & 0xFFFFU);
    low |= (base & 0xFFFFFFULL) << 16;

    /*
     * 0x89:
     *
     *   P = 1
     *   DPL = 0
     *   type = 9 (available 64-bit TSS)
     */
    low |= 0x89ULL << 40;
    low |= ((u64)(limit >> 16) & 0xFULL) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;

    g_gdt[GDT_TSS_LOW_INDEX] = low;
    g_gdt[GDT_TSS_HIGH_INDEX] = (base >> 32) & 0xFFFFFFFFULL;
}

bool gdt_init(u64 kernel_stack_top) {
    if (!kernel_stack_top) return false;

    k_memset(g_gdt, 0, sizeof(g_gdt));
    k_memset(&g_tss, 0, sizeof(g_tss));

    /*
     * Kernel code:
     *
     * access = 0x9A
     *   present
     *   DPL0
     *   executable
     *   readable
     *
     * flags = 0xA
     *   granularity
     *   long mode
     */
    g_gdt[GDT_KERNEL_CODE_INDEX] = make_code_data_descriptor(0x9AU, 0xAU);

    /*
     * Kernel data:
     *
     * access = 0x92
     * flags  = 0xC
     */
    g_gdt[GDT_KERNEL_DATA_INDEX] = make_code_data_descriptor(0x92U, 0xCU);

    /* Ring-3 data. */
    g_gdt[GDT_USER_DATA_INDEX] = make_code_data_descriptor(0xF2U, 0xCU);

    /* Ring-3 64-bit code. */
    g_gdt[GDT_USER_CODE_INDEX] = make_code_data_descriptor(0xFAU, 0xAU);

    /* RSP0 will eventually be changed whenever the scheduler switches to another thread. */
    g_tss.rsp0 = kernel_stack_top & ~0xFULL;

    /* Reserve IST1 for double faults. */
    u64 ist1_top = (u64)(void *)(g_double_fault_stack + sizeof(g_double_fault_stack));

    g_tss.ist1 = ist1_top & ~0xFULL;

    /*
     * No I/O permission bitmap.
     *
     * An offset at or beyond the TSS limit means
     * there is no bitmap present.
     */
    g_tss.iomap_base = (u16)sizeof(Tss64);

    install_tss_descriptor((u64)(void *)&g_tss, (u32)(sizeof(Tss64) - 1U));

    g_gdtr.limit = (u16)(sizeof(g_gdt) - 1U);
    g_gdtr.base = (u64)(void *)g_gdt;

    arch_load_gdtr(&g_gdtr);
    arch_reload_segments(GDT_KERNEL_CODE_SELECTOR, GDT_KERNEL_DATA_SELECTOR);

    if (arch_read_cs() != GDT_KERNEL_CODE_SELECTOR) return false;

    arch_load_tr(GDT_TSS_SELECTOR);

    if (arch_read_tr() != GDT_TSS_SELECTOR) return false;

    return true;
}

void gdt_set_rsp0(u64 kernel_stack_top) {
    g_tss.rsp0 = kernel_stack_top & ~0xFULL;
}

u64 gdt_rsp0(void) {
    return g_tss.rsp0;
}

u64 gdt_ist1(void) {
    return g_tss.ist1;
}