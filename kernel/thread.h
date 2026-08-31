#ifndef JA_OS_THREAD_H
#define JA_OS_THREAD_H

#include "types.h"
#include "pmm.h"
#include "address_space.h"
#include "arch.h"

#define THREAD_KERNEL_STACK_PAGES 4ULL
#define THREAD_KERNEL_STACK_SIZE (THREAD_KERNEL_STACK_PAGES * FRAME_SIZE)

typedef void (*ThreadEntry)(void *argument);

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

    CpuContext context;

    ThreadEntry entry;
    void *argument;

    u64 user_rip;
    u64 user_rsp;
    
    u64 kernel_stack_physical;
    u64 kernel_stack_base;
    u64 kernel_stack_top;
    u64 kernel_stack_size;

    bool owns_kernel_stack;
    bool context_ready;

    struct Thread *run_next;
    bool on_run_queue;
} Thread;

bool thread_prepare_kernel(
    Thread *thread,
    ThreadEntry entry,
    void *argument
);

bool thread_prepare_user(
    Thread *thread,
    u64 user_rip,
    u64 user_rsp
);

bool thread_switch(
    Thread *next
);


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
 * Activate a thread's address space and kernel
 * entry stack.
 *
 * The caller must have interrupts disabled.
 * This does not switch the current kernel RSP.
 */
bool thread_activate(Thread *thread);

/*
 * Create a non-running thread with its own
 * 16 KiB kernel stack.
 */
bool thread_create(Thread *thread, AddressSpace *address_space);

bool thread_destroy(Thread *thread);

#endif