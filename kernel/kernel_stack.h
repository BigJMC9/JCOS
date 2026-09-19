#ifndef JA_OS_KERNEL_STACK_H
#define JA_OS_KERNEL_STACK_H

#include "types.h"
#include "address_space.h"

/*
 * Guarded kernel-stack virtual arena.
 *
 * Owned thread stacks keep their existing PMM allocation, but execute through
 * a dedicated supervisor-only virtual slot:
 *
 *   [guard][16 KiB stack][guard]
 *
 * The ordinary physmap alias remains available for controlled kernel
 * maintenance; RSP/RSP0 always use the guarded arena mapping.
 */
bool kernel_stack_arena_init(AddressSpace *kernel_space, u64 probe_physical);
bool kernel_stack_arena_ready(void);

bool kernel_stack_map(u32 storage_slot, u64 physical, u64 *virtual_base_out);

/* Constructor rollback: mapping is discarded before the PMM range is freed.
 * A failed PMM free leaves the allocation owned but no longer published. */
bool kernel_stack_discard_unpublished(u64 physical, u64 virtual_base);

/* Live-thread destruction is transactional: a failed PMM free restores the
 * guarded mapping so the Thread remains retryable and internally consistent. */
bool kernel_stack_release(u64 physical, u64 virtual_base);

bool kernel_stack_mapping_valid(u64 physical, u64 virtual_base);
bool kernel_stack_virtual_released(u64 virtual_base);

#endif
