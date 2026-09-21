#ifndef JA_OS_PROCESS_H
#define JA_OS_PROCESS_H

#include "types.h"
#include "address_space.h"
#include "capability.h"

struct Thread;
struct UserElfImage;
struct ProcessExitQueue;

/* Kernel-internal terminal record; not a userspace ABI or a reap receipt. */
typedef enum {
    PROCESS_EXIT_NONE = 0,
    PROCESS_EXIT_NORMAL,
    PROCESS_EXIT_FAULT,
    PROCESS_EXIT_TERMINATED
} ProcessExitReason;

typedef struct {
    ProcessExitReason reason;
    u64 process_id;
    u64 thread_id;
    u64 vector;
    u64 error_code;
    u64 rip;
    u64 cr2;
} ProcessExitInfo;

typedef struct Process {
    u64 id;

    /*
     * Kernel process:
     *   points at address_space_kernel().
     *
     * User process:
     *   points at owned_address_space below.
     */
    AddressSpace *address_space;

    CapabilityTable capabilities;

    /*
     * User processes currently own their
     * AddressSpace inline because JCOS does not
     * yet have a general kernel heap.
     */
    AddressSpace owned_address_space;

    /*
    * Intrusive list of Threads owned by this
    * Process.
    */
    struct Thread *thread_head;
    struct Thread *thread_tail;

    u64 thread_count;

    /* One retained ELF ledger in the initial static-executable profile. */
    struct UserElfImage *elf_image;

    /* First terminal outcome wins. Retained through thread/VM cleanup retries. */
    ProcessExitInfo exit_info;

    /* Optional R6a.2 terminal-event reservation. The queue owns exactly one
     * reserved slot until this incarnation publishes or is unwatched. */
    struct ProcessExitQueue *exit_queue;
    u64 exit_queue_id;

    bool kernel;
    bool owns_address_space;
    bool initialized;
} Process;

/*
 * Adopt the already-created kernel AddressSpace
 * as process 1.
 */
bool process_system_init(AddressSpace *kernel_space);
Process *process_kernel(void);

/*
 * Create a user process with:
 *
 *   - a fresh user AddressSpace
 *   - an empty CapabilityTable
 */
/* false is clean for a fresh output; unpublished paging resources, if any,
 * are owned by AddressSpace's bounded rollback slot. Live storage is unchanged. */
bool process_create(Process *process);
u32 process_object_count(void);
bool process_storage_in_use(const Process *process);

/*
 * Process destruction requires an empty
 * capability table.
 *
 * Kernel process destruction is forbidden.
 */
bool process_destroy(Process *process);

AddressSpace *process_address_space(Process *process);
CapabilityTable *process_capabilities(Process *process);

u64 process_thread_count(const Process *process);

/* Snapshot for the kernel owner while Process storage is still live. false
 * clears a separate output. Reading does not consume the record. No wakeup or
 * userspace handle-query ABI is implied. Copy before process_destroy/reap.
 * A terminal record also prevents new thread attachment to this incarnation. */
bool process_exit_info(const Process *process, ProcessExitInfo *out);

/* Task-layer publication only. Caller excludes IRQs, has stopped all siblings
 * and closed owned endpoints, and will never resume the remaining current
 * thread. Does not allocate, reap, or overwrite an existing terminal outcome. */
bool process_exit_publish(Process *process, const ProcessExitInfo *info);

bool process_thread_attach(Process *process, struct Thread *thread);
/* Caller serializes validation and subsequent detach (UP: interrupts off). */
bool process_thread_can_detach(const Process *process, const struct Thread *thread);
bool process_thread_detach(Process *process, struct Thread *thread);
bool process_thread_contains(const Process *process, const struct Thread *thread);

struct Thread *process_thread_first(const Process *process);

#endif