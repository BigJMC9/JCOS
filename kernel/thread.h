#ifndef JA_OS_THREAD_H
#define JA_OS_THREAD_H

#include "types.h"
#include "pmm.h"
#include "process.h"

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

typedef enum {
    THREAD_WAIT_NONE = 0,

    THREAD_WAIT_IPC_RECEIVE,
    THREAD_WAIT_IPC_SEND,
    THREAD_WAIT_PROCESS_EXIT
} ThreadWaitKind;

typedef enum {
    THREAD_WAIT_RESULT_NONE = 0,

    THREAD_WAIT_RESULT_PENDING,
    THREAD_WAIT_RESULT_CANCELLED,
    THREAD_WAIT_RESULT_COMPLETED,
    THREAD_WAIT_RESULT_PEER_CLOSED,
    THREAD_WAIT_RESULT_TIMED_OUT
} ThreadWaitResult;

typedef struct Thread {
    u64 id;
    Process *process;
    /*
    * Intrusive Process ownership list.
    *
    * Completely separate from run_next.
    */
    struct Thread *process_prev;
    struct Thread *process_next;
    
    ThreadState state;

    u64 interrupt_rsp;

    ThreadEntry entry;
    void *argument;
    
    u64 kernel_stack_physical;
    u64 kernel_stack_base;
    u64 kernel_stack_top;
    u64 kernel_stack_size;

    bool owns_kernel_stack;
    bool interrupt_context_ready;

    /*
    * Outstanding kernel wait reservation.
    *
    * A non-NONE wait means another kernel object
    * retains a reference to this Thread.
    *
    * The Thread must not be destroyed until the
    * owning subsystem releases this reservation.
    */
    ThreadWaitKind wait_kind;
    ThreadWaitResult wait_result;
    void *wait_object;
    /* Boot-unique operation identity, not a scheduler wake counter. */
    u64 wait_id;

    struct Thread *run_next;
    bool on_run_queue;

    /* Owned by occupied capability slots; changed only by capability.c. */
    u64 capability_refs;
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

/*
 * Adopt the currently executing boot thread.
 * Its loader-provided stack is not PMM-owned.
 */
bool thread_system_init(
    Process *kernel_process,
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
 * Scheduler-private idle transition. The idle context is not a Thread and is
 * deliberately absent from Process/Thread object accounting. Enter requires
 * the previous current Thread to have been committed DEAD and detached from
 * the run queue. Activate-from-idle accepts only a normal READY queued Thread.
 * Both calls require local interrupts to be disabled.
 */
bool thread_scheduler_enter_idle(u64 idle_stack_top);
bool thread_scheduler_activate_from_idle(Thread *thread);

/*
 * Create a non-running thread with its own
 * 16 KiB kernel stack.
 */
/* false leaves fresh output empty; unpublished rollback failures are retained
 * in one module-owned slot. Live storage is rejected unchanged. */
bool thread_create(Thread *thread, Process *process);
bool thread_reclaim_unpublished_stack(void);
bool thread_creation_cleanup_pending(void);
u32 thread_object_count(void);
bool thread_storage_in_use(const Thread *thread);

/*
 * Kernel-internal wait ownership. The object owner must serialize publishing
 * its reverse link, predicate checks, completion, parking, and detachment.
 * Only the current thread ends a terminal wait; forced termination aborts it.
 * Delayed completion must carry wait_id: pointer identity alone is insufficient.
 * A successful terminal transition does not itself enqueue the thread.
 */
bool thread_wait_begin(Thread *thread, ThreadWaitKind kind, void *object);
bool thread_wait_end(Thread *thread, ThreadWaitKind kind, void *object);
bool thread_wait_matches(const Thread *thread, ThreadWaitKind kind, const void *object);
bool thread_wait_matches_id(const Thread *thread, ThreadWaitKind kind, const void *object, u64 wait_id);
bool thread_wait_try_complete(Thread *thread, ThreadWaitKind kind, const void *object, u64 wait_id, ThreadWaitResult result);
bool thread_wait_active(const Thread *thread);
bool thread_wait_cancel(Thread *thread, ThreadWaitKind kind, void *object);
bool thread_wait_cancelled(const Thread *thread, ThreadWaitKind kind, const void *object);
bool thread_wait_abort(Thread *thread, ThreadWaitKind kind, void *object);
bool thread_destroy(Thread *thread);

#endif