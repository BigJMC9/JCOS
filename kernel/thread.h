#ifndef JA_OS_THREAD_H
#define JA_OS_THREAD_H

#include "types.h"
#include "pmm.h"
#include "address_space.h"

#define THREAD_KERNEL_STACK_PAGES 4ULL
#define THREAD_KERNEL_STACK_SIZE \
    (THREAD_KERNEL_STACK_PAGES * FRAME_SIZE)

typedef enum {
    THREAD_STATE_INVALID = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_DEAD
} ThreadState;

typedef struct Thread {
    u64 id;
    AddressSpace *address_space;
    ThreadState state;

    /*
     * Device/allocator-facing physical base and
     * CPU-visible virtual stack range.
     */
    u64 kernel_stack_physical;
    u64 kernel_stack_base;
    u64 kernel_stack_top;
    u64 kernel_stack_size;

    bool owns_kernel_stack;
} Thread;


/*
 * Adopt the currently executing boot thread.
 * Its loader-provided stack is not PMM-owned.
 */
bool thread_system_init(
    AddressSpace *kernel_space,
    u64 bootstrap_stack_base,
    u64 bootstrap_stack_size,
    u64 bootstrap_stack_top
);

Thread *thread_current(void);


/*
 * Create a non-running thread with its own
 * 16 KiB kernel stack.
 */
bool thread_create(
    Thread *thread,
    AddressSpace *address_space
);

bool thread_destroy(
    Thread *thread
);

#endif