#ifndef JA_OS_PROCESS_H
#define JA_OS_PROCESS_H

#include "types.h"
#include "address_space.h"
#include "capability.h"

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

    u64 thread_count;

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
bool process_create(Process *process);

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

/*
 * Thread ownership accounting.
 *
 * These are used by the thread subsystem.
 */
bool process_thread_attach(Process *process);
bool process_thread_detach(Process *process);

#endif