#ifndef JA_OS_GDT_H
#define JA_OS_GDT_H

#include "types.h"

/*
 * Descriptor indices:
 *
 *   0x00  null
 *   0x08  kernel code
 *   0x10  kernel data
 *   0x18  user data
 *   0x20  user code
 *   0x28  TSS low
 *   0x30  TSS high
 *
 * User selectors include RPL=3.
 */
#define GDT_KERNEL_CODE_SELECTOR 0x08U
#define GDT_KERNEL_DATA_SELECTOR 0x10U
#define GDT_USER_DATA_SELECTOR   0x1BU
#define GDT_USER_CODE_SELECTOR   0x23U
#define GDT_TSS_SELECTOR         0x28U

bool gdt_init(
    u64 kernel_stack_top
);

void gdt_set_rsp0(
    u64 kernel_stack_top
);

u64 gdt_rsp0(void);
u64 gdt_ist1(void);

#endif